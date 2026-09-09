"""区域选择覆盖层 —— 每屏一个置顶覆盖窗 + SelectionController 跨屏状态机。

状态流：hover（整屏遮罩 + 吸附检测：UIA 界面元素优先、回退整窗 + 放大镜/取色）
→ creating（拖拽出新选区）→ selected（8 手柄/方向键微调/Ctrl+方向扩选/Enter 确认/
Esc 取消）→ moving（标注跟随平移）/resizing（放大镜自动出现）。
吸附键位：Space 切自由框选、Tab 轮换检测层级（auto/win/el）；
元素查询期间遮罩窗临时穿透，否则 UIA 只能看到遮罩自身（见 _set_hit_through）。
选区、命中、事件一律全局逻辑坐标（见 capture.py 的坐标约定）。
"""
from __future__ import annotations

import logging
import time
from collections.abc import Callable

from PySide6.QtCore import QObject, QPoint, QPointF, QRect, QRectF, Qt, QTimer, Signal
from PySide6.QtGui import (
    QBrush,
    QColor,
    QCursor,
    QGuiApplication,
    QImage,
    QKeyEvent,
    QMouseEvent,
    QPaintEvent,
    QPainter,
    QPainterPath,
    QPen,
    QScreen,
)
from PySide6.QtWidgets import QApplication, QWidget

import capture
import config
import uia
import winapi
from controller import AnnotationController

log = logging.getLogger("zpin.overlay")

_HANDLE_HIT = 6.0
_HANDLE_DRAW = 3.5

_MAG_SRC = 15          # 放大镜取样区边长（物理像素，奇数，中心即光标像素）
_MAG_VIEW = 150        # 放大区显示尺寸（10x 放大）
_MAG_INFO_H = 22       # 坐标/色值信息条高度
_MIN_SIZE = 2.0
_ANCHOR_LEN = 7.0
_EL_MOVE = 6.0        # 逻辑 px：光标移动不足则沿用上次的元素矩形，别狂问 COM
_EL_GAP = 0.02        # 秒：两次 UIA 调用的最小间隔（约 50 次/秒封顶）
_EL_MIN_PX = 6.0      # 更小的元素是噪声，不值得吸附
_EL_MAX_COVER = 0.85  # 元素盖住窗口 85% 以上 = 整页容器，不如吸附窗口
_GUIDE_TOL = 8.0      # 逻辑 px：边/角离参考线多近就吸上去
_WIN_CACHE_TTL = 0.25  # 秒：窗口矩形缓存有效期，hover 与参考线共用

# 8 个手柄：左上/上中/右上/右中/右下/下中/左下/左中（Tab 循环顺序）
_TL, _TM, _TR, _MR, _BR, _BM, _BL, _ML = range(8)

# 拖某个手柄时在动的边 (左, 右, 上, 下) —— 对齐吸附只作用于这些边
_RESIZE_EDGES = {
    _TL: (1, 0, 1, 0), _TM: (0, 0, 1, 0), _TR: (0, 1, 1, 0),
    _MR: (0, 1, 0, 0), _BR: (0, 1, 0, 1), _BM: (0, 0, 0, 1),
    _BL: (1, 0, 0, 1), _ML: (1, 0, 0, 0),
}


def _handle_points(r: QRectF) -> list[QPointF]:
    return [
        QPointF(r.left(), r.top()),
        QPointF(r.center().x(), r.top()),
        QPointF(r.right(), r.top()),
        QPointF(r.right(), r.center().y()),
        QPointF(r.right(), r.bottom()),
        QPointF(r.center().x(), r.bottom()),
        QPointF(r.left(), r.bottom()),
        QPointF(r.left(), r.center().y()),
    ]


class _OverlayWindow(QWidget):
    """单屏覆盖窗：只做事件转发与绘制，状态全部在 controller。"""

    def __init__(self, controller: "SelectionController", screen: QScreen) -> None:
        super().__init__(
            None,
            Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool,
        )
        self.controller = controller
        self.setScreen(screen)
        self.setGeometry(screen.geometry())
        self.setMouseTracking(True)
        self.setFocusPolicy(Qt.StrongFocus)
        self.setCursor(Qt.CrossCursor)

    def paintEvent(self, event: QPaintEvent) -> None:
        p = QPainter(self)
        self.controller.paint_screen(self, p)

    def mousePressEvent(self, ev: QMouseEvent) -> None:
        if ev.button() == Qt.LeftButton:
            self.controller.on_press(ev.globalPosition())
        elif ev.button() == Qt.RightButton:
            self.controller.on_right_press()

    def mouseMoveEvent(self, ev: QMouseEvent) -> None:
        self.controller.on_move(ev.globalPosition())

    def mouseReleaseEvent(self, ev: QMouseEvent) -> None:
        if ev.button() == Qt.LeftButton:
            self.controller.on_release(ev.globalPosition())

    def mouseDoubleClickEvent(self, ev: QMouseEvent) -> None:
        if ev.button() == Qt.LeftButton:
            self.controller.confirm()

    def keyPressEvent(self, ev: QKeyEvent) -> None:
        if self.controller.on_key(ev):
            return
        super().keyPressEvent(ev)


class _Magnifier(QWidget):
    """截图放大镜：光标附近像素的 Nearest 放大 + 像素网格 + 坐标/色值信息条。

    中心红框即光标所在像素；信息条显示物理坐标与该像素的 #RRGGBB 色值，
    供取色（按 C 复制）与像素级对齐使用。
    """

    def __init__(self, controller: "SelectionController") -> None:
        super().__init__(
            None, Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool)
        self.setAttribute(Qt.WA_ShowWithoutActivating)
        self.setAttribute(Qt.WA_TranslucentBackground)
        self._controller = controller
        self._info = ""
        self._src = QRect()
        self.resize(_MAG_VIEW + 2, _MAG_VIEW + _MAG_INFO_H + 2)

    def update_at(self, cursor: QPointF) -> None:
        """按光标位置刷新放大内容、信息条与窗口位置（贴边自动翻转）。"""
        host = self._controller
        base = host._base
        if base.isNull():
            return
        ap = host._map.abs_of(cursor)
        px, py = int(ap.x()), int(ap.y())
        half = _MAG_SRC // 2
        self._src = QRect(px - half, py - half, _MAG_SRC, _MAG_SRC).intersected(
            base.rect())
        cx = min(max(px, 0), base.width() - 1)
        cy = min(max(py, 0), base.height() - 1)
        self._info = (f"x:{px} y:{py}  "
                      f"{base.pixelColor(cx, cy).name(QColor.HexRgb).upper()}")
        pos = QPoint(int(cursor.x()) + 24, int(cursor.y()) + 24)
        scr = QGuiApplication.screenAt(pos)
        if scr is None:
            scr = QGuiApplication.primaryScreen()
        if scr is not None:
            g = scr.availableGeometry()
            if pos.x() + self.width() > g.right():
                pos.setX(int(cursor.x()) - 24 - self.width())
            if pos.y() + self.height() > g.bottom():
                pos.setY(int(cursor.y()) - 24 - self.height())
        self.move(pos)
        self.raise_()
        self.update()
        self.show()

    def paintEvent(self, ev: QPaintEvent) -> None:
        p = QPainter(self)
        p.setPen(Qt.NoPen)
        p.setBrush(QColor(23, 23, 28, 242))
        p.drawRoundedRect(QRectF(self.rect()).adjusted(0, 0, -1, -1), 6, 6)
        if self._src.isEmpty():
            return
        view = QRectF(1, 1, _MAG_VIEW, _MAG_VIEW)
        # 不开平滑变换 = Nearest：放大后是干净的马赛克像素块
        p.drawImage(view, self._controller._base, self._src)
        zoom = _MAG_VIEW / _MAG_SRC
        if zoom >= 8:   # 每个源像素 ≥8 显示像素才画网格，小尺寸时是噪声
            p.setPen(QPen(QColor(255, 255, 255, 36), 1))
            for i in range(1, _MAG_SRC):
                x = view.left() + i * zoom
                p.drawLine(QPointF(x, view.top()), QPointF(x, view.bottom()))
                y = view.top() + i * zoom
                p.drawLine(QPointF(view.left(), y), QPointF(view.right(), y))
        p.setPen(QPen(QColor("#FF2D55"), 2))
        p.setBrush(Qt.NoBrush)
        p.drawRect(QRectF(view.center().x() - zoom / 2, view.center().y() - zoom / 2,
                          zoom, zoom))
        p.setPen(QColor(235, 235, 235))
        p.drawText(QRectF(1, view.bottom() + 1, _MAG_VIEW, _MAG_INFO_H),
                   Qt.AlignCenter, self._info)


class SelectionController(QObject):
    captured = Signal(QImage, str, QPointF, float)  # 裁剪结果 + 触发模式 + 选区全局左上角 + 该图的比例
    cancelled = Signal()

    def __init__(self, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self._windows: list[_OverlayWindow] = []
        self._active = False
        self._mode = "normal"
        self._state = "hover"
        self._rect = QRectF()
        self._suggest = QRectF()
        self._cursor = QPointF()
        self._press = QPointF()
        self._move_offset = QPointF()
        self._resize_idx = -1
        self._focus_idx = -1
        self._resize_fix = QPointF()  # 拉伸时固定的对角点
        self._ext = (0, 0, 0, 0)      # 自动扩选方向 (L, R, T, B)
        self._ext_orig = QRectF()     # 扩选按下前的选区（拖坏时回滚）
        self._press_suggest = QRectF()  # 按下瞬间的窗口吸附候选（release 判定单击吸附用）
        self._base = QImage()
        self._map = capture.DesktopMap([], QRect(), QPoint())
        self._bounds = QRect()
        self._dpr = 1.0
        self._hwnds: set[int] = set()
        self._win_cache: list[tuple[int, int, int, int, int]] = []
        self._win_cache_at = 0.0
        self._el_at_cursor = QPointF()      # 上次问 UIA 时光标位置（逻辑）
        self._el_at_time = 0.0
        self._el_rect: QRectF | None = None  # 上次 UIA 结果（逻辑矩形），None = 无元素
        self._snap_free = False             # Space 切换：True = 自由框选，不吸附
        self._detect = "auto"               # Tab 轮换：auto（元素优先回退整窗）/ win / el
        self._saved_ex: dict[int, int] = {} # 穿透查询期间暂存的遮罩窗扩展样式
        self._magnifier: _Magnifier | None = None
        self._mag_pinned = False            # Alt 召唤：手动保持放大镜可见
        self._toast = ""                    # 选区内顶部短提示（如取色结果）
        self._toast_until = 0.0
        self._guides: list[tuple[str, float]] = []   # 命中的对齐参考线 [("v", x) / ("h", y)]
        # 标注子系统（独立控制器，见 controller.py）
        self._annot = AnnotationController(self)

    # ---- 标注坐标 ----
    def _phys_tl(self) -> QPointF:
        """选区在底图（物理虚拟桌面画布）里的原点，标注层绘制用。"""
        return self._map.phys_of(self._rect.topLeft())

    # ---- 标注相关（委托给 self._annot，见 controller.py） ----
    def _refocus_overlay(self) -> None:
        w = self._window_at(self._cursor)
        if w:
            w.activateWindow()
            w.setFocus()

    # ---- 生命周期 ----
    def start(self, mode: str) -> None:
        if self._active:
            self.cancel()
        # 重入防护：若上一次截图的覆盖窗还在 deleteLater 排队中（尚未真正销毁），
        # 先同步关掉，避免第二次抓取桌面时与正在销毁的全屏置顶窗竞态（可能导致崩溃）。
        for w in self._windows:
            try:
                w.close()
                w.deleteLater()
            except RuntimeError:
                pass
        self._windows.clear()
        self._hwnds.clear()
        self._mode = mode
        self._cursor = QPointF(QCursor.pos())
        self._bounds = capture.virtual_bounds()
        self._base, self._map = capture.grab_desktop()
        self._dpr = self._map.piece_at(self._cursor).dpr
        if self._base.isNull():
            log.error("截图底图抓取失败")
            return
        self._state = "hover"
        self._rect = QRectF()
        self._suggest = QRectF()
        self._win_cache = []
        self._win_cache_at = 0.0
        self._el_rect = None
        self._el_at_cursor = QPointF(-1e6, -1e6)   # 保证第一次 hover 真的问一次 UIA
        self._snap_free = False            # Space 状态按次复位，别带进下一张截图
        self._detect = "auto"              # Tab 检测层级同理
        self._mag_pinned = False           # Alt 召唤同理
        self._saved_ex.clear()             # 遮罩窗销毁后 HWND 可能被系统复用，样式缓存必须清
        self._guides = []
        self._focus_idx = -1
        self._annot.reset()
        self._active = True
        self._magnifier = _Magnifier(self)
        for screen in self._screens_ordered():
            w = _OverlayWindow(self, screen)
            w.show()
            self._hwnds.add(int(w.winId()))
            self._windows.append(w)
        for w in self._windows:
            winapi.set_topmost(int(w.winId()))
        focused = self._window_at(self._cursor)
        if focused:
            focused.activateWindow()
            focused.setFocus()

    def cancel(self) -> None:
        if not self._active:
            return
        self._teardown()
        self.cancelled.emit()

    def confirm(self, action: str | None = None) -> None:
        if not self._active or self._rect.width() < _MIN_SIZE or self._rect.height() < _MIN_SIZE:
            return
        self._dpr = self._map.dpr_of(self._rect)   # 标注/成品统一用选区主屏的比例
        crop = self._map.rect_of(self._rect).intersected(self._base.rect())
        if crop.isEmpty():
            self.cancel()
            return
        img = self._base.copy(crop)
        if self._annot.engine and self._annot.engine.shapes:
            p = QPainter(img)
            p.setRenderHint(QPainter.Antialiasing, True)
            p.translate(-crop.left(), -crop.top())
            self._annot.engine.draw(p)
            p.end()
        if action is None:
            # 截图完成的默认语义 = 复制到剪贴板（工具栏 ✓ / Enter / 双击）
            action = "copy"
        self._teardown()
        self.captured.emit(img, action, self._rect.topLeft(), self._dpr)

    def _teardown(self) -> None:
        self._active = False
        self._annot.reset()
        if self._magnifier is not None:
            self._magnifier.close()
            self._magnifier.deleteLater()
            self._magnifier = None
        for w in self._windows:
            w.close()
            w.deleteLater()
        self._windows.clear()
        self._hwnds.clear()
        self._saved_ex.clear()   # 窗口销毁后 HWND 会被系统复用，样式缓存不能带过会话

    def is_active(self) -> bool:
        return self._active

    def _screens_ordered(self) -> list[QScreen]:
        """所有屏幕，光标所在屏排最前（决定覆盖窗创建顺序与初始焦点）。"""
        cursor_screen = self._screen_at(self._cursor)
        screens = [cursor_screen] if cursor_screen else []
        for s in capture.QGuiApplication.screens():
            if s is not cursor_screen:
                screens.append(s)
        return [s for s in screens if s]

    # ---- 坐标工具 ----
    def _screen_at(self, pt: QPointF) -> QScreen | None:
        for s in capture.QGuiApplication.screens():
            if QRectF(s.geometry()).contains(pt):
                return s
        return None

    def _window_at(self, pt: QPointF) -> _OverlayWindow | None:
        for w in self._windows:
            if QRectF(w.geometry()).contains(pt):
                return w
        return self._windows[0] if self._windows else None

    def _update_all(self) -> None:
        for w in self._windows:
            w.update()

    # ---- 窗口吸附检测 ----
    def _window_under(self) -> QRectF:
        """返回光标所在窗口的逻辑矩形（自动吸附目标）；无则空矩形。

        窗口矩形是绝对物理像素（GetWindowRect 那套），画布是物理虚拟桌面，
        两者差一个画布原点且每屏比例不同，必须走 DesktopMap 的分段换算，
        不能整体除以某个统一 dpr（副屏会错位）。
        """
        cx, cy = self._cursor.x(), self._cursor.y()  # 逻辑光标
        for _hwnd, l, t, r, b in self._ensure_win_cache():
            wr = self._map.logical_of_abs_rect(l, t, r, b)
            if wr.contains(QPointF(cx, cy)) and wr.width() * wr.height() >= 1600:
                return wr          # _win_cache 已按 z 序，取最上层
        return QRectF()

    def _snap_target(self) -> QRectF:
        """hover 吸附目标；受 Space（自由框选开关）与 Tab（检测层级）控制。

        _detect="auto"：界面元素优先、不合适回退整窗；"win"：仅整窗；"el"：仅元素。
        """
        if self._snap_free:
            return QRectF()
        win = self._window_under()
        if self._detect == "win" or not config.get("Capture/snap_elements"):
            return win
        if self._detect == "el":
            el = self._element_under(win)
            return el if el is not None else QRectF()
        el = self._element_under(win)
        return el if el is not None else win

    def _element_under(self, win: QRectF) -> QRectF | None:
        """光标处 UIA 元素的逻辑矩形；判定不合适返回 None（交回窗口吸附）。"""
        now = time.monotonic()
        moved = (self._cursor - self._el_at_cursor).manhattanLength()
        if moved <= _EL_MOVE:
            return self._el_rect
        if now - self._el_at_time < _EL_GAP:
            return self._el_rect      # 距上次 COM 调用太近，先沿用
        # 有鼠标键按住时不做穿透查询：穿透期间到达的点击会落进下层应用
        # （0x01 左键 0x02 右键 0x04 中键 0x05/0x06 侧键）
        if any(winapi.user32.GetAsyncKeyState(vk) & 0x8000
               for vk in (0x01, 0x02, 0x04, 0x05, 0x06)):
            return None
        self._el_at_cursor = QPointF(self._cursor)
        self._el_at_time = now
        ap = self._map.abs_of(self._cursor)          # UIA 用的是绝对物理坐标
        self._set_hit_through(True)
        try:
            hit = uia.element_at(ap.x(), ap.y())
        finally:
            self._set_hit_through(False)
        if not hit:
            self._el_rect = None
            return None
        rect = self._map.logical_of_abs_rect(*hit[:4])
        if not rect.adjusted(-1, -1, 1, 1).contains(self._cursor):
            self._el_rect = None      # 不含光标 = 陈旧结果或别的窗口
            return None
        self._el_rect = rect if self._element_useful(rect, win) else None
        return self._el_rect

    def _show_toast(self, text: str) -> None:
        """选区内顶部显示一条 1.6 秒的短提示（取色结果等），随后自动消失。"""
        self._toast = text
        self._toast_until = time.monotonic() + 1.6
        QTimer.singleShot(1700, self._clear_toast)
        self._update_all()

    def _clear_toast(self) -> None:
        if self._toast and time.monotonic() >= self._toast_until:
            self._toast = ""
            self._update_all()

    def _update_magnifier(self) -> None:
        """按 Snipaste 时机刷新放大镜：调整边角时自动出现，Alt 常驻召唤，其余隐藏。"""
        if self._magnifier is None:
            return
        if self._state in ("resizing", "extending") or self._mag_pinned:
            self._magnifier.update_at(self._cursor)
        else:
            self._magnifier.hide()

    def _set_hit_through(self, on: bool) -> None:
        """把全部遮罩窗临时设为鼠标/UIA 穿透。

        遮罩窗盖着整个屏幕，不穿透时 ElementFromPoint 永远返回遮罩自身，
        控件吸附会整体失效。置位只覆盖单次查询（_EL_GAP 限流），查完立即恢复；
        查询前已确认无鼠标键按下，正常操作不会有点击落进下层应用。
        单窗失败跳过（降级成该窗不穿透 → 元素吸附本轮回退整窗），不影响其余窗。
        """
        for w in self._windows:
            hwnd = int(w.winId())
            try:
                if on:
                    if hwnd not in self._saved_ex:
                        cur = winapi.user32.GetWindowLongPtrW(hwnd, winapi.GWL_EXSTYLE)
                        self._saved_ex[hwnd] = cur
                        winapi.user32.SetWindowLongPtrW(
                            hwnd, winapi.GWL_EXSTYLE, cur | 0x20 | 0x80000)
                elif hwnd in self._saved_ex:
                    winapi.user32.SetWindowLongPtrW(
                        hwnd, winapi.GWL_EXSTYLE, self._saved_ex.pop(hwnd))
            except Exception:
                log.exception("遮罩窗穿透位切换失败 hwnd=%s", hwnd)

    @staticmethod
    def _element_useful(rect: QRectF, win: QRectF) -> bool:
        if rect.width() < _EL_MIN_PX or rect.height() < _EL_MIN_PX:
            return False
        if not win.isValid():
            return True
        inter = rect.intersected(win)
        if inter.isEmpty() or inter.width() * inter.height() < rect.width() * rect.height() * 0.6:
            return False   # 元素不在吸附窗口内（多半是别的全屏透明窗）
        cover = inter.width() * inter.height() / (win.width() * win.height())
        return cover <= _EL_MAX_COVER

    # ---- 对齐参考线 ----
    def _ensure_win_cache(self) -> list[tuple[int, int, int, int, int]]:
        """窗口矩形缓存（物理像素，z 序最上在前）：hover 吸附与参考线共用一份。"""
        now = time.monotonic()
        if not self._win_cache or now - self._win_cache_at > _WIN_CACHE_TTL:
            self._win_cache = winapi.visible_window_rects(self._hwnds)
            self._win_cache_at = now
        return self._win_cache

    def _guide_lines(self) -> tuple[list[float], list[float]]:
        """对齐候选线（逻辑坐标）：每屏的四边与中线 + 其它可见窗口的边与中线。"""
        xs: list[float] = []
        ys: list[float] = []
        for s in capture.QGuiApplication.screens():
            g = QRectF(s.geometry())
            xs += [g.left(), g.center().x(), g.right()]
            ys += [g.top(), g.center().y(), g.bottom()]
        for _h, l, t, r, b in self._ensure_win_cache():
            wr = self._map.logical_of_abs_rect(l, t, r, b)
            if wr.width() * wr.height() < 1600:
                continue          # 小窗/提示窗当基准只会添乱
            xs += [wr.left(), wr.center().x(), wr.right()]
            ys += [wr.top(), wr.center().y(), wr.bottom()]
        return xs, ys

    @staticmethod
    def _nearest(val: float, cands: list[float]) -> tuple[float, bool]:
        best, dist = val, _GUIDE_TOL + 1.0
        for c in cands:
            if abs(c - val) < dist:
                best, dist = c, abs(c - val)
        return best, dist <= _GUIDE_TOL

    def _snap_edges(self, rect: QRectF, allow: tuple) -> QRectF:
        """只把在动的边吸到最近的参考线上；命中的线记进 self._guides。"""
        if not config.get("Capture/snap_guides"):
            self._guides = []
            return rect
        xs, ys = self._guide_lines()
        l, t, r, b = rect.left(), rect.top(), rect.right(), rect.bottom()
        al, ar, at, ab = allow
        guides: list[tuple[str, float]] = []
        if al:
            v, hit = self._nearest(l, xs)
            if hit and v < r - 1:
                l = v
                guides.append(("v", v))
        if ar:
            v, hit = self._nearest(r, xs)
            if hit and v > l + 1:
                r = v
                guides.append(("v", v))
        if at:
            v, hit = self._nearest(t, ys)
            if hit and v < b - 1:
                t = v
                guides.append(("h", v))
        if ab:
            v, hit = self._nearest(b, ys)
            if hit and v > t + 1:
                b = v
                guides.append(("h", v))
        self._guides = guides
        return QRectF(QPointF(l, t), QPointF(r, b))

    def _snap_move(self, rect: QRectF) -> QRectF:
        """整体拖动：左/中/右（上/中/下）里挑最近的一条，整块平移过去，免得变形。"""
        if not config.get("Capture/snap_guides"):
            self._guides = []
            return rect
        xs, ys = self._guide_lines()
        guides: list[tuple[str, float]] = []

        def best_off(values: list[float],
                     cands: list[float]) -> tuple[float, float | None]:
            """离 cands 里吸附线最近的偏移；超容差返回 (0.0, None)。"""
            off, line, dist = 0.0, None, _GUIDE_TOL + 1.0
            for v in values:
                for c in cands:
                    if abs(c - v) < dist:
                        off, line, dist = c - v, c, abs(c - v)
            return (off, line) if dist <= _GUIDE_TOL else (0.0, None)

        dx, lx = best_off([rect.left(), rect.center().x(), rect.right()], xs)
        dy, ly = best_off([rect.top(), rect.center().y(), rect.bottom()], ys)
        if lx is not None:
            guides.append(("v", lx))
        if ly is not None:
            guides.append(("h", ly))
        self._guides = guides
        out = QRectF(rect)
        out.translate(dx, dy)
        return out

    # ---- 命中测试 ----
    def _hit_test(self, pt: QPointF) -> int:
        """返回手柄索引（0-7）；-1 = 选区内；-2 = 选区外。

        已选工具时选区内部一律让给画图：吸附到小控件时手柄的判定半径会盖住
        整个选区，按下就成了拖选区而不是画。手柄只在贴边/贴角的外侧生效。
        """
        r = self._rect
        drawing = self._annot.tool is not None and r.adjusted(2, 2, -2, -2).contains(pt)
        if not drawing:
            for i, hp in enumerate(_handle_points(r)):
                if (hp - pt).manhattanLength() <= _HANDLE_HIT * 2:
                    return i
        return -1 if self._rect.contains(pt) else -2

    # ---- 鼠标 ----
    def on_press(self, pos: QPointF) -> None:
        self._cursor = pos
        if self._state == "hover":
            # 交互约定：按下先进入拖拽候选（自由框选起点），不立即吸附。
            # 松手时若几乎没拖动：落在吸附窗口内 -> 单击吸附该窗口；否则回到 hover。
            self._state = "creating"
            self._press = pos
            self._rect = QRectF(pos, pos)
            s = self._suggest
            self._press_suggest = QRectF(s) if s.isValid() else QRectF()
        elif self._state == "selected":
            hit = self._hit_test(pos)
            if hit >= 0:
                self._state = "resizing"
                self._resize_idx = hit
                self._focus_idx = hit
                self._resize_fix = self._fixed_corner(hit)
            elif hit == -1 and self._annot.tool == "move":
                # 移动工具：框内拖动 = 移动整个截图框（不画图、不抓已画形状）
                self._state = "moving"
                self._move_offset = QPointF(
                    pos.x() - self._rect.left(), pos.y() - self._rect.top())
            elif hit == -1 and self._annot.tool:
                self._annot.start_drawing(pos)
            elif hit == -1 and self._annot.try_grab_shape(pos):
                # 未选工具时点中已画图形：选中并可拖动/改形状（状态已在内部置 drawing）
                pass
            elif hit == -1:
                self._state = "moving"
                self._move_offset = QPointF(
                    pos.x() - self._rect.left(), pos.y() - self._rect.top())
            else:
                # 选区外：自动扩选 —— 把鼠标所在方位的边/角拉向点击处
                # （4 条边 / 4 个角共 8 方位），不丢标注、不重开选区
                r = self._rect
                self._ext = (
                    1 if pos.x() < r.left() else 0,    # 扩左
                    1 if pos.x() > r.right() else 0,   # 扩右
                    1 if pos.y() < r.top() else 0,     # 扩上
                    1 if pos.y() > r.bottom() else 0,  # 扩下
                )
                self._ext_orig = QRectF(r)
                self._apply_extend(pos)
                self._state = "extending"
        if self._magnifier is not None:
            self._update_magnifier()
        self._update_all()

    def on_move(self, pos: QPointF) -> None:
        if pos == self._cursor:
            return
        self._cursor = pos
        if self._state == "hover":
            self._suggest = self._snap_target()
        elif self._state == "creating":
            free = QRectF(self._press, pos).normalized()
            self._rect = self._snap_edges(free, (
                pos.x() < self._press.x(), pos.x() > self._press.x(),
                pos.y() < self._press.y(), pos.y() > self._press.y()))
        elif self._state == "moving":
            old = QPointF(self._rect.topLeft())
            self._rect = self._snap_move(QRectF(pos - self._move_offset, self._rect.size()))
            self._clamp_rect()
            d = self._rect.topLeft() - old
            # Snipaste 语义：框内已画的标注锚定在选区上，随框一起平移
            if d != QPointF(0, 0):
                self._annot.translate_following(d.x() * self._dpr, d.y() * self._dpr)
        elif self._state == "resizing":
            self._apply_resize(pos)
            self._rect = self._snap_edges(self._rect,
                                          _RESIZE_EDGES.get(self._resize_idx, (0, 0, 0, 0)))
        elif self._state == "extending":
            self._apply_extend(pos)
            self._rect = self._snap_edges(self._rect, self._ext)
        elif self._state == "drawing" and (
                self._annot.active_shape is not None or self._annot.is_grabbing):
            self._annot.update_drawing(pos)
        elif self._state == "erasing" and self._annot.engine:
            self._annot.erase_at(pos)
        if self._magnifier is not None:
            self._update_magnifier()
        self._update_cursor_shape(pos)
        self._update_all()

    def on_release(self, pos: QPointF) -> None:
        self._guides = []
        if self._state == "creating":
            if self._rect.width() < _MIN_SIZE or self._rect.height() < _MIN_SIZE:
                # 单击未拖拽：落在吸附窗口上 -> 吸附该窗口；否则取消
                s = self._press_suggest
                if (s.isValid() and s.width() >= _MIN_SIZE and s.height() >= _MIN_SIZE
                        and s.adjusted(-3, -3, 3, 3).contains(self._press)):
                    self._rect = QRectF(s)
                    self._state = "selected"
                    self._suggest = QRectF()
                    self._annot.ensure()
                else:
                    self._state = "hover"
                    self._rect = QRectF()
            else:
                self._state = "selected"
                self._annot.ensure()
        elif self._state in ("moving", "resizing"):
            self._state = "selected"
            self._resize_idx = -1
            self._dpr = self._map.dpr_of(self._rect)   # 选区可能落在别的屏，混合 DPI 位移/绘制用它
            self._annot._position_toolbar()
        elif self._state == "extending":
            self._state = "selected"
            if (self._rect.width() < _MIN_SIZE or self._rect.height() < _MIN_SIZE
                    or not self._rect.isValid()):
                self._rect = self._ext_orig   # 扩过头/反向拖到极小：回滚到按下前
            self._annot._position_toolbar()
        elif self._state == "drawing":
            self._annot.finish_drawing(pos)
        elif self._state == "erasing":
            self._state = "selected"
            self._annot.finish_erasing()
        if self._magnifier is not None:
            self._update_magnifier()
        self._update_cursor_shape(pos)
        self._update_all()

    def on_right_press(self) -> None:
        self.cancel()

    def _fixed_corner(self, idx: int) -> QPointF:
        r = self._rect
        if idx == _TL:
            return QPointF(r.right(), r.bottom())
        if idx == _TR:
            return QPointF(r.left(), r.bottom())
        if idx == _BR:
            return QPointF(r.left(), r.top())
        if idx == _BL:
            return QPointF(r.right(), r.top())
        if idx == _TM:
            return QPointF(r.center().x(), r.bottom())
        if idx == _BM:
            return QPointF(r.center().x(), r.top())
        if idx == _MR:
            return QPointF(r.left(), r.center().y())
        return QPointF(r.right(), r.center().y())

    def _apply_resize(self, pos: QPointF) -> None:
        idx = self._resize_idx
        f = self._resize_fix
        if idx in (_TL, _TR, _BR, _BL):
            self._rect = QRectF(f, pos).normalized()
        elif idx in (_TM, _BM):
            anchor_x = f.x()
            top = min(pos.y(), f.y())
            h = abs(pos.y() - f.y())
            self._rect = QRectF(anchor_x - self._rect.width() / 2, top,
                                self._rect.width(), h)
        else:  # _ML / _MR
            anchor_y = f.y()
            left = min(pos.x(), f.x())
            w = abs(pos.x() - f.x())
            self._rect = QRectF(left, anchor_y - self._rect.height() / 2,
                                w, self._rect.height())
        self._clamp_rect()

    def _apply_extend(self, pos: QPointF) -> None:
        """自动扩选：把按下方位的边/角实时拉向鼠标位置（其余边保持不动）。"""
        L, R, T, B = self._ext
        r = self._rect
        if L:
            r.setLeft(min(pos.x(), r.right() - 1))
        if R:
            r.setRight(max(pos.x(), r.left() + 1))
        if T:
            r.setTop(min(pos.y(), r.bottom() - 1))
        if B:
            r.setBottom(max(pos.y(), r.top() + 1))
        # 夹在虚拟桌面内，避免扩出屏外
        b = QRectF(self._bounds)
        if L:
            r.setLeft(max(b.left(), r.left()))
        if R:
            r.setRight(min(b.right(), r.right()))
        if T:
            r.setTop(max(b.top(), r.top()))
        if B:
            r.setBottom(min(b.bottom(), r.bottom()))

    def _clamp_rect(self) -> None:
        b = QRectF(self._bounds)
        self._rect.moveLeft(max(b.left(), min(self._rect.left(), b.right() - self._rect.width())))
        self._rect.moveTop(max(b.top(), min(self._rect.top(), b.bottom() - self._rect.height())))

    def _update_cursor_shape(self, pos: QPointF) -> None:
        w = self._window_at(pos)
        if not w:
            return
        if self._state == "extending":
            # 扩选方向 -> 对应缩放光标（单边/对角）
            L, R, T, B = self._ext
            horiz, vert = L or R, T or B
            if horiz and vert:
                w.setCursor(Qt.SizeFDiagCursor if (L and T) or (R and B)
                            else Qt.SizeBDiagCursor)
            elif horiz:
                w.setCursor(Qt.SizeHorCursor)
            else:
                w.setCursor(Qt.SizeVerCursor)
            return
        if self._state in ("hover", "creating", "drawing", "erasing"):
            w.setCursor(Qt.CrossCursor)
            return
        hit = self._hit_test(pos)
        shapes = {
            _TL: Qt.SizeFDiagCursor, _BR: Qt.SizeFDiagCursor,
            _TR: Qt.SizeBDiagCursor, _BL: Qt.SizeBDiagCursor,
            _TM: Qt.SizeVerCursor, _BM: Qt.SizeVerCursor,
            _ML: Qt.SizeHorCursor, _MR: Qt.SizeHorCursor,
        }
        if hit >= 0:
            w.setCursor(shapes[hit])
        elif hit == -1:
            # 选区内：未选工具 = 拖动整体选区；已选工具 = 精确定位绘制起点
            w.setCursor(Qt.SizeAllCursor if self._annot.tool in (None, "move")
                        else Qt.CrossCursor)
        else:
            # 选区外：点按即开新框，用十字提示可框选，光标保持可见
            w.setCursor(Qt.CrossCursor)

    # ---- 键盘 ----
    def on_key(self, ev: QKeyEvent) -> bool:
        key = ev.key()
        mod = ev.modifiers()
        if key == Qt.Key_Escape:
            self.cancel()
            return True
        if key in (Qt.Key_Return, Qt.Key_Enter):
            if self._state == "hover" and self._suggest.isValid():
                # 还没框选：Enter 直接截取当前吸附到的元素/窗口
                self._rect = QRectF(self._suggest)
                self._state = "selected"
            self.confirm()
            return True
        if key == Qt.Key_C and mod & Qt.ControlModifier:
            self.confirm()
            return True
        if key == Qt.Key_S and mod & Qt.ControlModifier:
            self.confirm("save")
            return True
        if key == Qt.Key_Alt:
            # 召唤/收起放大镜（Snipaste 同款：不在合理时机时按 Alt 召唤）
            self._mag_pinned = not self._mag_pinned
            self._update_magnifier()
            return True
        if key == Qt.Key_C and not mod:
            # 取色：截图模式内随时按 C 复制光标处色值（不依赖放大镜可见）
            c = self._base.pixelColor(
                min(max(int(self._map.abs_of(self._cursor).x()), 0), self._base.width() - 1),
                min(max(int(self._map.abs_of(self._cursor).y()), 0), self._base.height() - 1))
            hexc = c.name(QColor.HexRgb).upper()
            QApplication.clipboard().setText(hexc)
            self._show_toast(f"已复制 {hexc}")
            return True
        if self._state == "hover" and key == Qt.Key_Space:
            # 自由框选 <-> 吸附模式（Snipaste 同款语义）
            self._snap_free = not self._snap_free
            self._suggest = self._snap_target()
            self._update_all()
            return True
        if self._state == "hover" and key == Qt.Key_Tab:
            # 检测层级轮换：自动（元素优先回退整窗）-> 仅窗口 -> 仅元素
            self._detect = {"auto": "win", "win": "el", "el": "auto"}[self._detect]
            self._suggest = self._snap_target()
            self._update_all()
            return True
        if self._annot.handle_key(ev):
            return True
        if self._state != "selected" or not self._rect.isValid():
            return False
        step = 10 if ev.modifiers() & Qt.ShiftModifier else 1
        if key == Qt.Key_Tab:
            self._focus_idx = 0 if self._focus_idx < 0 else (self._focus_idx + 1) % 9
            if self._focus_idx == 8:
                self._focus_idx = -1
            self._update_all()
            return True
        deltas = {
            Qt.Key_Left: (-step, 0), Qt.Key_Right: (step, 0),
            Qt.Key_Up: (0, -step), Qt.Key_Down: (0, step),
        }
        if key not in deltas:
            return False
        dx, dy = deltas[key]
        if mod & Qt.ControlModifier:
            # 扩大选区：沿按键方向把对应边向外推（Snipaste 同款 Ctrl+方向）
            grow = QRectF(self._rect)
            if dx < 0:
                grow.setLeft(grow.left() - step)
            elif dx > 0:
                grow.setRight(grow.right() + step)
            elif dy < 0:
                grow.setTop(grow.top() - step)
            else:
                grow.setBottom(grow.bottom() + step)
            if grow.width() >= _MIN_SIZE and grow.height() >= _MIN_SIZE:
                self._rect = grow
                self._clamp_rect()
                self._update_all()
            return True
        if self._focus_idx >= 0:
            self._resize_idx = self._focus_idx
            self._resize_fix = self._fixed_corner(self._focus_idx)
            pos = QPointF(
                _handle_points(self._rect)[self._focus_idx].x() + dx,
                _handle_points(self._rect)[self._focus_idx].y() + dy,
            )
            self._apply_resize(pos)
            self._resize_idx = -1
        else:
            old = QPointF(self._rect.topLeft())
            self._rect.translate(dx, dy)
            self._clamp_rect()
            d = self._rect.topLeft() - old
            # 用钳制后的实际位移：贴边时框没动，标注也不能动
            if d != QPointF(0, 0):
                self._annot.translate_following(d.x() * self._dpr, d.y() * self._dpr)
        self._update_all()
        return True

    # ---- 绘制 ----
    def paint_screen(self, window: _OverlayWindow, p: QPainter) -> None:
        """单屏覆盖层主流程：底图 -> 遮罩/吸附框 -> 标注 -> 辅助线 -> 边框/手柄/标签。"""
        geo = window.geometry()
        p.setRenderHint(QPainter.SmoothPixmapTransform, False)
        src = self._map.rect_of(QRectF(geo)).intersected(self._base.rect())
        if not src.isEmpty():
            p.drawImage(window.rect(), self._base, src)

        accent = QColor(config.get("Interface/theme_color"))
        border_w = max(1, int(config.get("Capture/border_width")))
        local = self._to_local_factory(geo)

        sel = self._rect.isValid() and self._rect.width() >= 1 and self._rect.height() >= 1
        if not sel:
            self._paint_hover_layer(window, p, geo, local, accent, border_w)
            self._paint_toast(window, p)
            return

        r = QRectF(local(self._rect.topLeft()), self._rect.size())
        self._paint_mask(window, p, r)

        # 标注层：形状坐标锚定在截图底图上，经选区视口映射回屏幕（委托标注控制器）
        self._annot.draw(p, r)

        self._paint_guides(window, p, geo, r)

        # 全屏十字线
        self._draw_crosshair_if_enabled(p, geo, local)

        # 边框
        p.setPen(QPen(accent, border_w))
        p.setBrush(Qt.NoBrush)
        p.drawRect(r)

        self._paint_handles(p, r, accent)
        self._paint_size_label(p, r)
        self._paint_toast(window, p)

    def _paint_toast(self, window: _OverlayWindow, p: QPainter) -> None:
        """选区/悬停顶部中央的短提示（取色结果），1.6 秒自动消失。"""
        if not (self._toast and time.monotonic() < self._toast_until):
            return
        fm = p.fontMetrics()
        tw = fm.horizontalAdvance(self._toast) + 24
        th = fm.height() + 10
        wx = (window.width() - tw) / 2
        p.setPen(Qt.NoPen)
        p.setBrush(QColor(20, 20, 24, 220))
        p.drawRoundedRect(QRectF(wx, 22, tw, th), 6, 6)
        p.setPen(QColor("#7CE38B"))
        p.drawText(QRectF(wx, 22, tw, th), Qt.AlignCenter, self._toast)

    def _paint_hover_layer(self, window: _OverlayWindow, p: QPainter, geo: QRect,
                           local: Callable[[QPointF], QPointF],
                           accent: QColor, border_w: int) -> None:
        """未框选态：整窗遮罩 + 吸附建议框/角锚点 + 十字线。"""
        p.setPen(Qt.NoPen)
        p.setBrush(QBrush(QColor(config.get("Capture/mask_color"))))
        p.drawRect(window.rect())
        # 吸附建议框 + 角锚点（仅在存在有效吸附建议时绘制）
        if self._suggest.isValid():
            sr = QRectF(local(self._suggest.topLeft()), self._suggest.size())
            p.setPen(QPen(accent, border_w))
            p.setBrush(Qt.NoBrush)
            p.drawRect(sr)
            if config.get("Capture/show_anchors"):
                self._draw_corner_anchors(
                    p, sr, QColor(config.get("Capture/anchor_stroke_color")), accent)
        self._draw_crosshair_if_enabled(p, geo, local)

    def _paint_mask(self, window: _OverlayWindow, p: QPainter, r: QRectF) -> None:
        """遮罩：窗口区域减去选区，选区外盖半透明遮罩色。"""
        window_path = QPainterPath()
        window_path.addRect(QRectF(window.rect()))
        sel_path = QPainterPath()
        sel_path.addRect(r)
        p.setPen(Qt.NoPen)
        p.setBrush(QBrush(QColor(config.get("Capture/mask_color"))))
        p.drawPath(window_path.subtracted(sel_path))

    def _paint_guides(self, window: _OverlayWindow, p: QPainter, geo: QRect,
                      r: QRectF) -> None:
        """选区边延伸辅助线 + 拖拽吸附命中的对齐参考线（红色，区别于选区边框）。"""
        # 辅助线：选区边延伸至窗口边缘
        if not config.get("Capture/disable_guides"):
            pen = QPen(QColor(255, 255, 255, 70), 1, Qt.DashLine)
            p.setPen(pen)
            wr = QRectF(window.rect())
            p.drawLine(QPointF(wr.left(), r.top()), QPointF(wr.right(), r.top()))
            p.drawLine(QPointF(wr.left(), r.bottom()), QPointF(wr.right(), r.bottom()))
            p.drawLine(QPointF(r.left(), wr.top()), QPointF(r.left(), wr.bottom()))
            p.drawLine(QPointF(r.right(), wr.top()), QPointF(r.right(), wr.bottom()))

        # 对齐参考线：拖拽中吸到的那条线
        if self._guides:
            p.setPen(QPen(QColor("#FF2D55"), 1))
            p.setBrush(Qt.NoBrush)
            wr = QRectF(window.rect())
            for kind, v in self._guides:
                if kind == "v":
                    x = v - geo.left()
                    p.drawLine(QPointF(x, wr.top()), QPointF(x, wr.bottom()))
                else:
                    y = v - geo.top()
                    p.drawLine(QPointF(wr.left(), y), QPointF(wr.right(), y))

    def _paint_handles(self, p: QPainter, r: QRectF, accent: QColor) -> None:
        """选中/调整态的 8 个手柄；聚焦中的手柄用主题色加亮。"""
        if self._state not in ("selected", "moving", "resizing", "extending"):
            return
        for i, hp in enumerate(_handle_points(r)):
            if i == self._focus_idx:
                p.setBrush(QBrush(accent))
            else:
                p.setBrush(QBrush(QColor("#FFFFFF")))
            p.setPen(QPen(QColor(0, 0, 0, 160), 1))
            p.drawRect(QRectF(hp.x() - _HANDLE_DRAW, hp.y() - _HANDLE_DRAW,
                              _HANDLE_DRAW * 2, _HANDLE_DRAW * 2))

    def _paint_size_label(self, p: QPainter, r: QRectF) -> None:
        """选区上方的物理尺寸标签与（可选）快捷键提示。"""
        pr = self._map.rect_of(self._rect)
        size_text = f"{pr.width()} × {pr.height()} px"
        self._draw_label(p, r, size_text)
        if config.get("Capture/show_hints"):
            self._draw_label(p, r, "Enter 复制 · Esc 取消", below=True, dim=True)

    def _to_local_factory(self, geo: QRect) -> Callable[[QPointF], QPointF]:
        """生成「全局逻辑坐标 -> 本窗局部坐标」的换算闭包。"""
        def to_local(pt: QPointF) -> QPointF:
            return QPointF(pt.x() - geo.left(), pt.y() - geo.top())
        return to_local

    def _draw_corner_anchors(self, p: QPainter, r: QRectF, color: QColor, fill: QColor) -> None:
        """吸附建议框的四个 L 形角锚点。"""
        pen = QPen(color, 2)
        p.setPen(pen)
        p.setBrush(Qt.NoBrush)
        pts = [r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()]
        for i, pt in enumerate(pts):
            sx = 1 if i in (0, 3) else -1  # 朝向框内
            sy = 1 if i in (0, 1) else -1
            p.drawLine(pt, QPointF(pt.x() + sx * _ANCHOR_LEN, pt.y()))
            p.drawLine(pt, QPointF(pt.x(), pt.y() + sy * _ANCHOR_LEN))

    def _draw_crosshair_if_enabled(self, p: QPainter, geo: QRect,
                                   local: Callable[[QPointF], QPointF]) -> None:
        if not config.get("Capture/show_crosshair"):
            return
        c = local(self._cursor)
        if not QRectF(geo.translated(-geo.topLeft())).contains(c):
            return
        pen = QPen(QColor(255, 255, 255, 110), 1, Qt.DashLine)
        p.setPen(pen)
        wr = QRectF(0, 0, geo.width(), geo.height())
        p.drawLine(QPointF(wr.left(), c.y()), QPointF(wr.right(), c.y()))
        p.drawLine(QPointF(c.x(), wr.top()), QPointF(c.x(), wr.bottom()))


    def _draw_label(self, p: QPainter, r: QRectF, text: str, below: bool = False,
                    dim: bool = False) -> None:
        fm = p.fontMetrics()
        w = fm.horizontalAdvance(text) + 16
        h = fm.height() + 6
        x = r.left()
        y = r.top() - h - 4 if not below else r.bottom() + 6
        p.save()
        p.setPen(Qt.NoPen)
        p.setBrush(QColor(20, 20, 24, 200))
        p.drawRoundedRect(QRectF(x, y, w, h), 4, 4)
        p.setPen(QColor(255, 255, 255, 150 if dim else 235))
        p.drawText(QRectF(x, y, w, h), Qt.AlignCenter, text)
        p.restore()
