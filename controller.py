"""标注控制器 —— 与 SelectionController 解耦的画图子系统。

SelectionController 进入「已选定区域」阶段后，所有画图职责（AnnotateEngine /
CaptureToolbar / TextEditor、当前绘制状态、撤销重做、文字批注、文字/气泡块拖动、
标注层绘制）都委托到这里。坐标锚定在截图底图物理像素（移动选区时标注始终跟随截图
内容）；本类通过 host 只读访问选区/底图/光标等共享状态，避免循环依赖。
"""
from __future__ import annotations

import logging

from PySide6.QtCore import QObject, QPointF, QRectF, Qt
from PySide6.QtGui import QBrush, QColor, QKeyEvent, QPainter, QPen

import config
from engine import AnnotateEngine
from shapes import (
    Arrow,
    Callout,
    Ellipse,
    Line,
    Mosaic,
    Rect,
    Shape,
    Stroke,
    TextShape,
    endpoints,
)
from text_edit import TextEditor
from toolbar import CaptureToolbar

log = logging.getLogger("zpin.controller")


class AnnotationController(QObject):
    """标注子系统控制器：托管标注工具状态、绘制流程、撤销重做与二次编辑。

    Args:
        host: 宿主 SelectionController，经其只读访问选区/底图/光标等共享状态。
    """

    def __init__(self, host: "SelectionController") -> None:
        super().__init__(host)
        self._host = host
        self._engine: AnnotateEngine | None = None
        self._toolbar: CaptureToolbar | None = None
        self._tool: str | None = None
        self._color = QColor(config.get("Interface/theme_color"))
        self._width = 4.0
        self._active_shape = None
        self._text_editor: TextEditor | None = None
        self._edit_shape: Shape | None = None      # 选中/拖动中的已画图形（二次编辑）
        self._edit_pt = -1                         # >=0 = 正在拖某个端点；-1 = 整体拖动
        self._edit_before: list = []               # 按下时的端点快照，抬手时记账
        self._edit_last = QPointF()                # 上一帧光标（图像坐标）
        self._edit_dx = 0.0
        self._edit_dy = 0.0
        self._edit_drag = False                    # True = 正在拖（松手后仅保持选中）

    # ---- host 共享状态只读访问 ----
    @property
    def engine(self) -> AnnotateEngine | None:
        """当前标注引擎（未进入标注阶段时为 None）。"""
        return self._engine

    @property
    def active_shape(self) -> Shape | None:
        """正在绘制、待提交入栈的活动形状。"""
        return self._active_shape

    @property
    def tool(self) -> str | None:
        """当前选中的标注工具名；None 表示未选工具。"""
        return self._tool

    @property
    def is_grabbing(self) -> bool:
        """是否正在拖拽已画图形（二次编辑中）。"""
        return self._edit_drag

    def img_pt(self, pos: QPointF) -> QPointF:
        """全局逻辑坐标 -> 底图画布物理像素坐标（标注锚定在截图内容上）。"""
        return self._host._map.phys_of(pos)

    # ---- 已画图形的抓取与二次编辑 ----
    def shape_at(self, ip: QPointF) -> Shape | None:
        """命中检查：反向遍历形状栈，返回最上层被点中的图形。"""
        if not self._engine:
            return None
        tol = 8.0 * self._host._dpr
        for s in reversed(self._engine.shapes):
            if s.hit(ip, tol):
                return s
        return None

    def _endpoint_at(self, s: Shape, ip: QPointF) -> int:
        """端点命中（比整体抓取更优先，容忍半径略大）。"""
        tol = 10.0 * self._host._dpr
        for i, p in enumerate(s.endpoints()):
            if (p - ip).manhattanLength() <= tol:
                return i
        return -1

    def select_shape(self, s: Shape | None) -> None:
        """设置当前二次编辑目标；改色/改粗细会作用在它身上。"""
        self._edit_shape = s
        self._edit_pt = -1
        self._edit_before = []
        self._edit_dx = self._edit_dy = 0.0
        self._edit_drag = False

    def try_grab_shape(self, pos: QPointF) -> bool:
        """点中已画图形 -> 选中并进入拖动/改形状（state=drawing），返回 True。"""
        s = self.shape_at(self.img_pt(pos))
        if s is None:
            self.select_shape(None)
            return False
        self.select_shape(s)
        ip = self.img_pt(pos)
        self._edit_pt = self._endpoint_at(s, ip)
        self._edit_before = endpoints(s)
        self._edit_last = QPointF(ip)
        self._edit_drag = True
        self._host._state = "drawing"
        return True

    def delete_selected(self) -> bool:
        """Del：删掉当前选中的已画图形（可撤销）。"""
        if self._edit_shape is None or not self._engine:
            return False
        self._engine.remove(self._edit_shape)
        self.select_shape(None)
        self._sync_toolbar_state()
        self._host._update_all()
        return True

    def apply_style(self, color: QColor | None = None, width: float | None = None) -> None:
        """选中图形时改颜色/粗细：直接重新上色（Snipaste 同样语义，不占撤销步）。"""
        s = self._edit_shape
        if s is None:
            return
        if color is not None:
            s.color = QColor(color)
        if width is not None:
            s.width = float(width * self._host._dpr)
        self._host._update_all()

    # ---- 生命周期 ----
    def reset(self) -> None:
        """清空全部标注状态（start / teardown 时调用）。"""
        self._close_text_editor(False)
        if self._toolbar is not None:
            self._toolbar.close()
            self._toolbar.deleteLater()
            self._toolbar = None
        self._engine = None
        self._active_shape = None
        self.select_shape(None)
        self._tool = None

    def ensure(self) -> None:
        """确保标注引擎与工具栏就绪，并把工具栏摆到选区旁。"""
        if self._engine is None:
            self._engine = AnnotateEngine(self._host._base)
        if self._toolbar is None:
            self._toolbar = CaptureToolbar(self._color, self._width)
            self._toolbar.action.connect(self._on_toolbar_action)
            self._toolbar.tool_selected.connect(self._on_tool_selected)
            self._toolbar.color_selected.connect(self._on_color_selected)
            self._toolbar.width_selected.connect(self._on_width_selected)
        self._position_toolbar()

    def _position_toolbar(self) -> None:
        """把工具栏摆到选区下缘（放不下改上缘），并夹在宿主 bounds 内。"""
        if not self._toolbar or not self._host._rect.isValid():
            return
        tb = self._toolbar
        tb.adjustSize()
        b = QRectF(self._host._bounds)
        x = self._host._rect.left()
        y = self._host._rect.bottom() + 10
        if y + tb.height() > b.bottom():
            y = self._host._rect.top() - tb.height() - 10
        x = min(max(b.left() + 4, x), b.right() - tb.width() - 4)
        y = max(b.top() + 4, y)
        tb.move(round(x), round(y))
        tb.show()
        tb.raise_()

    # ---- 工具栏回调 ----
    def _on_toolbar_action(self, action: str) -> None:
        """响应工具栏动作：undo/redo 走引擎，其余转发给宿主确认/取消。

        Args:
            action: 动作名（undo/redo/copy/pin/save/cancel）。
        """
        if action == "undo":
            if self._engine:
                self._engine.undo()
                self.select_shape(None)
                self._sync_toolbar_state()
                self._host._update_all()
        elif action == "redo":
            if self._engine:
                self._engine.redo()
                self.select_shape(None)
                self._sync_toolbar_state()
                self._host._update_all()
        elif action in ("copy", "pin", "save", "cancel"):
            if action == "cancel":
                self._host.cancel()
            else:
                self._host.confirm(action)

    def _on_tool_selected(self, name: str) -> None:
        self._tool = None if name in ("", "none") else name
        self.select_shape(None)
        self._close_text_editor(False)
        self._host._update_all()

    def _on_color_selected(self, color: QColor) -> None:
        self._color = QColor(color)
        self.apply_style(color=self._color)

    def _on_width_selected(self, width: float) -> None:
        self._width = float(width)
        self.apply_style(width=self._width)

    # ---- 文字批注 ----
    def _close_text_editor(self, commit: bool) -> None:
        """关闭当前文字编辑器。

        Args:
            commit: True 走提交流程；False 解绑信号后直接丢弃。
        """
        if self._text_editor:
            editor, self._text_editor = self._text_editor, None
            if commit:
                editor._commit()  # committed 闭包内自行解绑并收尾
            else:
                try:
                    editor.committed.disconnect()
                    editor.cancelled.disconnect()
                except RuntimeError:
                    pass
                editor.close()

    def _open_text_editor(self, global_pos: QPointF, shape: TextShape) -> None:
        """在指定位置打开文字编辑器，提交时把文字写入 shape 并入栈。

        Args:
            global_pos: 编辑器落点（全局逻辑坐标）。
            shape: 待写入文字的 TextShape / Callout。
        """
        self._close_text_editor(False)
        editor = TextEditor(self._color, max(13, round(shape.font_size / self._host._dpr)))
        self._text_editor = editor
        editor.move(round(global_pos.x()), round(global_pos.y()))
        editor.show()
        editor.raise_()
        editor.activateWindow()
        editor.setFocus()

        def detach() -> None:
            try:
                editor.committed.disconnect()
                editor.cancelled.disconnect()
            except RuntimeError:
                pass

        def commit(text: str) -> None:
            detach()  # close()->focusOut 会再次触发 _commit，先解绑防重复入栈
            self._active_shape = None   # 入栈后不能再以 active 身份重画，撤销时会出现残影
            if text.strip() and self._engine:
                shape.lines = text.splitlines() or [text]
                self._engine.add(shape)
                self._sync_toolbar_state()
            self._text_editor = None
            self._host._refocus_overlay()
            self._host._update_all()

        def cancel_edit() -> None:
            detach()
            self._text_editor = None
            self._active_shape = None
            self._host._refocus_overlay()
            self._host._update_all()

        editor.committed.connect(commit)
        editor.cancelled.connect(cancel_edit)

    # ---- 绘制 ----
    def _font_px(self) -> float:
        """文字/气泡字号（物理像素）：跟随粗细档，中档仍是 16px 基准。"""
        return max(13.0, (10.0 + self._width * 1.5)) * self._host._dpr

    def start_drawing(self, pos: QPointF) -> None:
        """按下开始一次交互：新建形状、进入文字编辑，或抓取已画图形/擦除。

        Args:
            pos: 按下位置（全局逻辑坐标）。
        """
        ip = self.img_pt(pos)
        tool = self._tool
        if tool not in ("text", "callout"):
            self.select_shape(None)     # 画新东西前先取消二次编辑选中
        w = self._width * self._host._dpr
        font_px = self._font_px()
        if tool in ("pen", "marker"):
            s = Stroke(self._color, w, highlight=(tool == "marker"))
            s.add_point(ip)
            self._active_shape = s
            self._host._state = "drawing"
        elif tool == "mosaic":
            s = Mosaic(max(8.0, w * 2.0))
            # 像素化底图要现在就注入：只在 engine.add() 时给的话，
            # 拖拽过程中的 Mosaic.draw() 因 pixelated=None 直接 return，笔迹全程不可见
            if self._engine:
                s.pixelated = self._engine.pixelated
            s.add_point(ip)
            self._active_shape = s
            self._host._state = "drawing"
        elif tool == "eraser":
            self._host._state = "erasing"
            if self._engine:
                self._engine.begin_erase_stroke()   # 一整段涂抹 = 一步撤销
                self._engine.erase_at(ip, tol=8.0 * self._host._dpr)
        elif tool == "text":
            if self.try_grab_shape(pos):        # 点中已落下的文字块 -> 拖动换位
                return
            shape = TextShape(self._color, font_px)
            shape.pos = QPointF(ip)
            self._open_text_editor(pos, shape)
        elif tool == "callout":
            if self.try_grab_shape(pos):        # 点中已落下的气泡块 -> 拖动换位
                return
            s = Callout(self._color, font_px)
            s.tail = QPointF(ip)
            s.pos = QPointF(ip)
            self._active_shape = s
            self._host._state = "drawing"
        else:
            cls = {"line": Line, "arrow": Arrow, "rect": Rect, "ellipse": Ellipse}.get(tool)
            if cls is None:
                return
            s = cls(self._color, w)
            s.p1 = QPointF(ip)
            s.p2 = QPointF(ip)
            self._active_shape = s
            self._host._state = "drawing"

    def update_drawing(self, pos: QPointF) -> None:
        """拖动中更新：二次编辑则改端点/整体平移，否则更新活动形状。

        Args:
            pos: 当前位置（全局逻辑坐标）。
        """
        ip = self.img_pt(pos)
        s = self._edit_shape
        if self._edit_drag and s is not None:
            if self._edit_pt >= 0:
                pts = s.endpoints()
                if self._edit_pt < len(pts):
                    pts[self._edit_pt] = QPointF(ip)   # 拖端点改形状
                    s.set_endpoints(pts)
            else:
                dx, dy = ip.x() - self._edit_last.x(), ip.y() - self._edit_last.y()
                if dx or dy:
                    s.translate(dx, dy)                # 整体拖动，实时跟随光标
                    self._edit_dx += dx
                    self._edit_dy += dy
                    self._edit_last = QPointF(ip)
            return
        a = self._active_shape
        if isinstance(a, (Stroke, Mosaic)):
            a.add_point(ip)
        elif isinstance(a, Callout):
            a.pos = ip
        elif isinstance(a, (Line, Rect)):
            a.p2 = ip

    def finish_drawing(self, pos: QPointF) -> None:
        """抬手收尾：二次编辑把 move/resize 记账入栈，新形状入栈或进入文字编辑。

        Args:
            pos: 抬手位置（全局逻辑坐标）。
        """
        self._host._state = "selected"
        if self._edit_drag and self._edit_shape is not None:
            s = self._edit_shape
            self._edit_drag = False
            if self._engine:
                if self._edit_pt >= 0:
                    self._engine.commit_resize(s, self._edit_before, endpoints(s))
                    self._edit_pt = -1
                    self._edit_before = endpoints(s)
                else:
                    self._engine.commit_move(s, self._edit_dx, self._edit_dy)
                    self._edit_dx = self._edit_dy = 0.0
                self._sync_toolbar_state()
            self._position_toolbar()
            self._host._update_all()
            return
        s, self._active_shape = self._active_shape, None
        if s is None:
            return
        if isinstance(s, Callout):
            if (s.pos - s.tail).manhattanLength() < 4.0:
                s.pos += QPointF(16.0, 16.0)
            self._active_shape = s   # 编辑期间保持气泡可见，提交/取消时清除
            self._open_text_editor(pos, s)
            return
        if isinstance(s, (Stroke, Mosaic)):
            ok = True
        else:
            r = QRectF(s.p1, s.p2).normalized()
            ok = max(r.width(), r.height()) >= 3.0
        if ok and self._engine:
            self._engine.add(s)
            self._sync_toolbar_state()
        # 画完把工具栏重新置顶显示：防止绘制过程 z 序变化把它压到覆盖层下面
        self._position_toolbar()
        self._host._update_all()

    def erase_at(self, pos: QPointF) -> None:
        """把光标位置换算成图像坐标后执行一次擦除。

        Args:
            pos: 光标位置（全局逻辑坐标）。
        """
        if self._engine:
            # 命中容差与 shape_at 一致：物理像素换算乘 dpr，高 DPI 下橡皮不至于偏小
            self._engine.erase_at(self.img_pt(pos), tol=8.0 * self._host._dpr)

    def translate_following(self, dx: float, dy: float) -> None:
        """选区整体平移时让全部标注跟移（物理像素，Snipaste 语义）。

        除已提交的形状外，编辑中尚未提交的形状（如正在输入的气泡）也一起跟移，
        否则提交后会脱锚。不占撤销步。

        Args:
            dx: X 方向位移（物理像素）。
            dy: Y 方向位移（物理像素）。
        """
        if self._engine:
            self._engine.translate_all(dx, dy)
        if self._active_shape is not None:
            self._active_shape.translate(dx, dy)

    def finish_erasing(self) -> None:
        """抬手：一整段涂抹合成一步撤销。"""
        if self._engine:
            self._engine.end_erase_stroke()
            self._sync_toolbar_state()

    def _sync_toolbar_state(self) -> None:
        if self._toolbar and self._engine:
            self._toolbar.set_enabled_actions(self._engine.can_undo(), self._engine.can_redo())

    # ---- 键盘：撤销/重做/删除 ----
    def handle_key(self, ev: QKeyEvent) -> bool:
        """处理删除与撤销/重做快捷键（Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y）。

        Args:
            ev: 键盘事件。

        Returns:
            事件是否被本类消费。
        """
        key = ev.key()
        mod = ev.modifiers()
        if key in (Qt.Key_Delete, Qt.Key_Backspace):
            return self.delete_selected()
        if key == Qt.Key_Z and mod & Qt.ControlModifier and self._engine:
            if mod & Qt.ShiftModifier:
                self._engine.redo()
            else:
                self._engine.undo()
            self.select_shape(None)   # 栈一动，选中的形状可能已被删/被重做掉
            self._sync_toolbar_state()
            self._host._update_all()
            return True
        if key == Qt.Key_Y and mod & Qt.ControlModifier and self._engine:
            self._engine.redo()
            self.select_shape(None)
            self._sync_toolbar_state()
            self._host._update_all()
            return True
        return False

    # ---- 标注层绘制（选区视口局部坐标 r） ----
    def draw(self, p: QPainter, r: QRectF) -> None:
        """把标注层绘制到选区视口：形状栈、活动形状与二次编辑框/端点。

        Args:
            p: 画笔（宿主覆盖层的 painter）。
            r: 选区视口局部矩形，用于裁剪与坐标平移。
        """
        if self._engine is None:
            return
        dpr = self._host._dpr
        ptl = self._host._phys_tl()
        p.save()
        p.setRenderHint(QPainter.Antialiasing, True)
        p.setClipRect(r)
        p.translate(r.topLeft())
        p.scale(1.0 / dpr, 1.0 / dpr)
        p.translate(-ptl.x(), -ptl.y())
        self._engine.draw(p)
        if self._active_shape is not None:
            self._active_shape.draw(p)
        s = self._edit_shape
        if s is not None and s in self._engine.shapes:
            accent = QColor(config.get("Interface/theme_color"))
            box = s.bounding_rect()
            if not box.isEmpty():
                p.setPen(QPen(accent, 1, Qt.DashLine))
                p.setBrush(Qt.NoBrush)
                p.drawRect(box)
            pts = s.endpoints()
            if pts:
                hs = 5.0 * dpr
                p.setPen(QPen(QColor(0, 0, 0, 160), 1))
                p.setBrush(QBrush(QColor("#FFFFFF")))
                for q in pts:
                    p.drawRect(QRectF(q.x() - hs, q.y() - hs, hs * 2, hs * 2))
        p.restore()
        if self._host._state == "erasing":
            # 橡皮头指示圈：红色空心圆标出擦除作用范围，随光标实时移动
            ip = self.img_pt(self._host._cursor)
            rr = 8.0 * dpr
            p.save()
            p.translate(r.topLeft())
            p.scale(1.0 / dpr, 1.0 / dpr)
            p.translate(-ptl.x(), -ptl.y())
            p.setPen(QPen(QColor("#E53935"), 1.5 * dpr, Qt.SolidLine, Qt.RoundCap))
            p.setBrush(QColor(229, 57, 53, 26))
            p.drawEllipse(QRectF(ip.x() - rr, ip.y() - rr, rr * 2, rr * 2))
            p.restore()
