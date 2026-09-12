"""贴图窗口 —— 无边框置顶小窗：拖动/光标锚点缩放/透明度/边框阴影/右键全项菜单。

缩放规则：>=100% 最近邻（保持锐利），50%~100% 平滑，
<50% 预生成缩略图（上限 _THUMB_MAX）。窗口含阴影边距 _MARGIN，锚点换算已扣除。
"""
from __future__ import annotations

import logging
from typing import TYPE_CHECKING

from PySide6.QtCore import QPoint, QPointF, QRectF, Qt, QTimer
from PySide6.QtGui import (
    QCloseEvent,
    QColor,
    QCursor,
    QGuiApplication,
    QImage,
    QKeyEvent,
    QMouseEvent,
    QPaintEvent,
    QPainter,
    QPen,
    QShowEvent,
    QTransform,
    QWheelEvent,
)
from PySide6.QtWidgets import QApplication, QMenu, QWidget

import config
import output
import winapi

if TYPE_CHECKING:
    from pins import PinManager

log = logging.getLogger("zpin.pin")

_ZOOM_MIN = 0.05
_ZOOM_MAX = 8.0
_ZOOM_STEP = 1.15
_MARGIN = 8
_THUMB_MAX = 512
_MIN_SIDE = 24  # 缩放下限：较长边不小于该像素

MENU_STYLE = """
QMenu { background: rgba(23,23,28,244); color:#E8E8E8; border:1px solid rgba(255,255,255,32);
        border-radius: 8px; padding: 4px; font-size: 12px; }
QMenu::item { padding: 5px 24px 5px 10px; border-radius: 5px; }
QMenu::item:selected { background: rgba(255,255,255,30); }
QMenu::item:disabled { color: rgba(232,232,232,90); }
QMenu::separator { height: 1px; background: rgba(255,255,255,28); margin: 4px 8px; }
QMenu::indicator:checked { background: rgba(111,195,255,80); border-radius: 3px; width: 6px; height: 6px; }
"""


class PinWindow(QWidget):
    """单张贴图的浮动窗口：光标锚点缩放、旋转/翻转/灰度、透明度与右键菜单。

    Args:
        image: 贴图内容（物理像素图像）。
        manager: 所属管理器（PinManager），负责删除/移动/分组与可见性同步。
        group: 初始贴图组名。
        pos: 落点（全局逻辑坐标）。
        ref_dpr: 图像内容的参考 DPR；为空时用落点屏的 DPR。
    """

    def __init__(self, image: QImage, manager: PinManager, group: str, pos: QPointF,
                 ref_dpr: float | None = None) -> None:
        super().__init__(
            None,
            Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool,
        )
        self.setAttribute(Qt.WA_TranslucentBackground)
        self.setMouseTracking(True)
        self._manager = manager
        self._group = group
        self._base = QImage(image)          # 原始图（不随变换改）
        self._source = QImage(image)        # 变换后（旋转/翻转/灰度）
        self._thumb: QImage | None = None
        self._scale = 1.0
        self._rotation = 0.0
        self._flip_h = False
        self._flip_v = False
        self._grayscale = False
        self._border = True
        self._border_color = QColor(config.get("Pin/border_color"))
        self._glow = bool(config.get("Pin/border_glow"))
        self._shadow = bool(config.get("Pin/shadow"))
        self._opacity = max(10, min(100, int(config.get("Pin/default_opacity")))) / 100.0
        self._drag_offset: QPointF | None = None
        # HiDPI：截图图是物理像素，Qt 窗口走逻辑坐标，要 1:1 显示必须除以参考缩放比。
        # 截图内容的参考比例 = 那次截图所用屏的比例（由调用方经 captured 信号传进来）。
        # 混合 DPI 下它可能 ≠ 贴图落点屏的比例，默认用落点屏比例；调用方传入时以它为准，
        # 这样「截图即贴」时贴出来与截图画面严格等大（此前跨缩放屏贴图会大一圈的根因）。
        scr = QGuiApplication.screenAt(QPoint(int(pos.x()), int(pos.y())))
        local = float(scr.devicePixelRatio()) if scr else 1.0
        if local < 1.0:
            local = float(QGuiApplication.primaryScreen().devicePixelRatio()) or 1.0
        self._ref_dpr = float(ref_dpr or 0.0) or local

        self.setWindowOpacity(self._opacity)
        self.setStyleSheet(MENU_STYLE)
        # 不设焦点策略的话 QWidget 默认 NoFocus，keyPressEvent 永远收不到，
        # 菜单上标着的 Ctrl+C / Del / Esc 等全是摆设
        self.setFocusPolicy(Qt.StrongFocus)
        self._update_size()
        self.move(round(pos.x() - _MARGIN), round(pos.y() - _MARGIN))

        self._label_timer = QTimer(self)
        self._label_timer.setSingleShot(True)
        self._label_timer.timeout.connect(self.update)

    # ---- 几何 ----
    def _img_size(self) -> tuple[float, float]:
        return float(self._source.width()), float(self._source.height())

    def _img_rect(self) -> QRectF:
        """图像在窗口内的逻辑矩形。物理像素 ÷ dpr = 逻辑 px，保证 1:1 视觉尺寸。"""
        w, h = self._img_size()
        f = self._scale / self._ref_dpr
        return QRectF(_MARGIN, _MARGIN, w * f, h * f)

    def _update_size(self) -> None:
        r = self._img_rect()
        self.resize(round(r.width()) + _MARGIN * 2, round(r.height()) + _MARGIN * 2)

    def showEvent(self, ev: QShowEvent) -> None:
        super().showEvent(ev)
        winapi.set_topmost(int(self.winId()))
        self.setFocus()  # 贴图后立刻能吃键盘快捷键

    def _max_side(self) -> float:
        return float(config.get("Pin/max_window_size") or 0) or 12000.0

    # ---- 变换 ----
    def _rebuild_source(self) -> None:
        """按当前灰度/旋转/翻转设置从原图重建 _source，并重设窗口尺寸。"""
        img = self._base
        if self._grayscale:
            img = img.convertToFormat(QImage.Format_Grayscale8).convertToFormat(
                QImage.Format_ARGB32)
        t = QTransform()
        t.rotate(self._rotation)
        if self._flip_h:
            t.scale(-1.0, 1.0)
        if self._flip_v:
            t.scale(1.0, -1.0)
        self._source = (img.transformed(t, Qt.SmoothTransformation)
                        if not t.isIdentity() else QImage(img))
        self._thumb = None
        self._clamp_scale()
        self._update_size()
        self.update()

    def _clamp_scale(self) -> None:
        """把缩放系数夹进 [_ZOOM_MIN, _ZOOM_MAX]，并满足窗口边长下限与配置上限。"""
        w, h = self._img_size()
        m = max(w, h)
        if m > 0:
            # 下限：窗口逻辑边不小于 _MIN_SIDE（换算回物理放大系数需乘 dpr）
            self._scale = max(self._scale, _MIN_SIDE * self._ref_dpr / m)
            # 上限：窗口（含阴影边距）任一边不超过 Pin/max_window_size（逻辑 px）
            limit = self._max_side()
            over = m * self._scale / self._ref_dpr + _MARGIN * 2 - limit
            if over > 0:
                self._scale = max(_ZOOM_MIN, (limit - _MARGIN * 2) * self._ref_dpr / m)
        self._scale = max(_ZOOM_MIN, min(_ZOOM_MAX, self._scale))

    def _apply(self) -> None:
        self._clamp_scale()
        self._update_size()
        self._flash_label()
        self.update()

    # ---- 缩放（光标锚点） ----
    def _zoom_at(self, factor: float, global_pos: QPointF) -> None:
        """以指定全局位置为锚点缩放，保持锚点下的图像内容不动。

        Args:
            factor: 缩放系数（>1 放大，<1 缩小，实际值经 _clamp_scale 夹取）。
            global_pos: 锚点（全局逻辑坐标）。
        """
        new_scale = self._scale * factor
        if new_scale == self._scale:
            return
        f_old = self._scale / self._ref_dpr   # 缩放前：逻辑 px / image px
        tl = self.frameGeometry().topLeft()
        # 光标下的图像坐标必须按旧比例算，否则 move 目标恒等于原左上角
        ix = (global_pos.x() - tl.x() - _MARGIN) / f_old
        iy = (global_pos.y() - tl.y() - _MARGIN) / f_old
        self._scale = new_scale
        self._apply()
        f_new = self._scale / self._ref_dpr   # clamp 后的实际比例，落位用它
        self.move(
            round(global_pos.x() - ix * f_new - _MARGIN),
            round(global_pos.y() - iy * f_new - _MARGIN),
        )
        self._keep_on_screen()

    def _keep_on_screen(self) -> None:
        """缩放到很大/很小后锚点换算可能把窗口推到屏幕外，这里拉回来一点。"""
        scr = QGuiApplication.screenAt(self.frameGeometry().center())
        if scr is None:
            scr = QGuiApplication.primaryScreen()
        if scr is None:
            return
        g = scr.availableGeometry()
        vis = 64  # 至少留这么多像素在屏内，方便再拖回来
        x = min(max(self.x(), g.left() + vis - self.width()), g.right() - vis)
        y = min(max(self.y(), g.top() - self.height() + vis), g.bottom() - vis)
        if (x, y) != (self.x(), self.y()):
            self.move(round(x), round(y))

    def zoom_in(self) -> None:
        """以窗口中心为锚点放大一步（_ZOOM_STEP）。"""
        g = QPointF(self.mapToGlobal(self.rect().center()))
        self._zoom_at(_ZOOM_STEP, g)

    def zoom_out(self) -> None:
        """以窗口中心为锚点缩小一步（1/_ZOOM_STEP）。"""
        g = QPointF(self.mapToGlobal(self.rect().center()))
        self._zoom_at(1.0 / _ZOOM_STEP, g)

    def actual_size(self) -> None:
        """恢复 100% 缩放（不改旋转/翻转等变换）。"""
        self._scale = 1.0
        self._apply()

    def reset(self) -> None:
        """重置缩放与全部变换（旋转/翻转/灰度）到初始状态。"""
        self._scale = 1.0
        self._rotation = 0.0
        self._flip_h = self._flip_v = False
        self._grayscale = False
        self._rebuild_source()
        self._apply()

    # ---- 图像处理（v1 范围：旋转/翻转/灰度） ----
    def rotate(self) -> None:
        """顺时针旋转 90°。"""
        self._rotation = (self._rotation + 90.0) % 360.0
        self._rebuild_source()
        self._apply()

    def flip_h(self) -> None:
        """水平翻转。"""
        self._flip_h = not self._flip_h
        self._rebuild_source()
        self._apply()

    def flip_v(self) -> None:
        """垂直翻转。"""
        self._flip_v = not self._flip_v
        self._rebuild_source()
        self._apply()

    def toggle_grayscale(self) -> None:
        """切换灰度显示。"""
        self._grayscale = not self._grayscale
        self._rebuild_source()
        self._apply()

    # ---- 外观 ----
    def set_opacity(self, pct: int) -> None:
        """设置窗口不透明度并立即生效。

        Args:
            pct: 不透明度百分比（超出 10~100 的值会被夹取）。
        """
        self._opacity = max(0.1, min(1.0, pct / 100.0))
        self.setWindowOpacity(self._opacity)

    def set_border(self, on: bool) -> None:
        """开关边框描边。

        Args:
            on: True 显示边框。
        """
        self._border = on
        self.update()

    def set_shadow(self, on: bool) -> None:
        """开关底部阴影。

        Args:
            on: True 显示阴影。
        """
        self._shadow = on
        self.update()

    def set_group(self, name: str) -> None:
        """更新所属贴图组名（组间移动由 PinManager 完成）。

        Args:
            name: 新的组名。
        """
        self._group = name

    # ---- 输出 ----
    def copy_image(self) -> None:
        """把当前图像（含变换）复制到系统剪贴板。"""
        # setImage 而非 setPixmap：不持有 HBITMAP 原生句柄，规避剪贴板读取崩溃
        QApplication.clipboard().setImage(self._source)

    def _save_as(self) -> None:
        """「另存为」：弹路径对话框后写入所选位置（写盘在后台线程）。"""
        def _done(path: str | None) -> None:
            if path:
                self._manager.notify.emit("贴图", f"已保存：{path}")
        output.save_image_dialog_async(self._source, self, on_done=_done)

    # ---- 交互 ----
    def mousePressEvent(self, ev: QMouseEvent) -> None:
        if ev.button() == Qt.LeftButton:
            gp = ev.globalPosition()
            self._drag_offset = QPointF(gp.x() - self.x(), gp.y() - self.y())
        elif ev.button() == Qt.RightButton:
            self._show_menu()

    def mouseDoubleClickEvent(self, ev: QMouseEvent) -> None:
        """双击贴图 = 隐藏（托盘「显示全部」或隐藏/显示热键可找回）。"""
        self.hide()
        self._manager.notify_visibility()

    def mouseMoveEvent(self, ev: QMouseEvent) -> None:
        if self._drag_offset is not None:
            gp = ev.globalPosition()
            self.move(round(gp.x() - self._drag_offset.x()),
                      round(gp.y() - self._drag_offset.y()))

    def mouseReleaseEvent(self, ev: QMouseEvent) -> None:
        if ev.button() == Qt.LeftButton:
            self._drag_offset = None

    def wheelEvent(self, ev: QWheelEvent) -> None:
        delta = ev.angleDelta().y()
        if delta == 0:
            return
        factor = _ZOOM_STEP if delta > 0 else 1.0 / _ZOOM_STEP
        self._zoom_at(factor, ev.globalPosition())

    def closeEvent(self, ev: QCloseEvent) -> None:
        # Alt+F4 等外部关闭也要让管理器知道可见性变了
        self._manager.notify_visibility()
        super().closeEvent(ev)

    def keyPressEvent(self, ev: QKeyEvent) -> None:
        k, m = ev.key(), ev.modifiers()
        if k == Qt.Key_Escape:
            self._hide_pin()
        elif k == Qt.Key_Delete:
            self._manager.delete_pin(self)
        elif k == Qt.Key_C and m & Qt.ControlModifier:
            self.copy_image()
        elif k == Qt.Key_S and m & Qt.ControlModifier:
            self._save_as()
        elif k in (Qt.Key_Plus, Qt.Key_Equal) and m & Qt.ControlModifier:
            self.zoom_in()
        elif k == Qt.Key_Minus and m & Qt.ControlModifier:
            self.zoom_out()
        elif k == Qt.Key_0 and m & Qt.ControlModifier:
            self.actual_size()
        elif k == Qt.Key_R and m & Qt.ControlModifier:
            self.reset()
        elif k == Qt.Key_T and m & Qt.ControlModifier:
            self.rotate()
        else:
            super().keyPressEvent(ev)

    # ---- 菜单 ----
    def _hide_pin(self) -> None:
        """隐藏（关闭）这张贴图：从屏幕收起，可从托盘「隐藏/显示所有贴图」找回。"""
        self.hide()
        self._manager.notify_visibility()

    def _destroy_pin(self) -> None:
        """销毁：从贴图列表与屏幕彻底移除（不弹确认，与 Del 键一致）。"""
        self._manager.delete_pin(self)

    def _show_menu(self) -> None:
        """弹出右键全项菜单（输出/销毁/缩放/变换/外观/分组/隐藏）。"""
        menu = QMenu(self)
        menu.setStyleSheet(MENU_STYLE)
        menu.addAction("复制\tCtrl+C", self.copy_image)
        menu.addAction("另存为...\tCtrl+S", self._save_as)
        menu.addSeparator()
        menu.addAction("销毁", self._destroy_pin)
        menu.addSeparator()
        menu.addAction("放大\tCtrl++", self.zoom_in)
        menu.addAction("缩小\tCtrl+-", self.zoom_out)
        menu.addAction("实际大小\tCtrl+0", self.actual_size)
        menu.addAction("重置\tCtrl+R", self.reset)
        menu.addSeparator()
        menu.addAction("旋转 90°\tCtrl+T", self.rotate)
        menu.addAction("水平翻转", self.flip_h)
        menu.addAction("垂直翻转", self.flip_v)
        act_g = menu.addAction("灰度", self.toggle_grayscale)
        act_g.setCheckable(True)
        act_g.setChecked(self._grayscale)
        menu.addSeparator()

        op_menu = menu.addMenu("不透明度")
        for pct in (100, 90, 80, 70, 60, 50, 40, 30, 20, 10):
            a = op_menu.addAction(f"{pct}%")
            a.setCheckable(True)
            a.setChecked(round(self._opacity * 100) == pct)
            a.triggered.connect(lambda _=False, v=pct: self.set_opacity(v))


        act_b = menu.addAction("边框")
        act_b.setCheckable(True)
        act_b.setChecked(self._border)
        act_b.triggered.connect(lambda on: self.set_border(on))

        act_s = menu.addAction("阴影")
        act_s.setCheckable(True)
        act_s.setChecked(self._shadow)
        act_s.triggered.connect(lambda on: self.set_shadow(on))

        g_menu = menu.addMenu("贴图组")
        for name in self._manager.group_names():
            a = g_menu.addAction(name)
            a.setCheckable(True)
            a.setChecked(name == self._group)
            a.triggered.connect(lambda _=False, n=name: self._manager.move_pin(self, n))

        menu.addSeparator()
        menu.addAction("隐藏\tEsc", self._hide_pin)
        menu.exec(QCursor.pos())
        menu.deleteLater()

    # ---- 绘制 ----
    def _thumb_for(self) -> QImage:
        """取（或懒生成）缩放显示用的预览缩略图（长边不超过 _THUMB_MAX）。"""
        if self._thumb is not None:
            return self._thumb
        img = self._source
        k = min(_THUMB_MAX / max(1, img.width()), _THUMB_MAX / max(1, img.height()), 1.0)
        self._thumb = img.scaled(
            max(1, round(img.width() * k)), max(1, round(img.height() * k)),
            Qt.KeepAspectRatio, Qt.SmoothTransformation,
        )
        return self._thumb

    def _draw_shadow(self, p: QPainter, r: QRectF) -> None:
        steps = 6
        for i in range(steps, 0, -1):
            a = int(26 * (steps - i) / steps)
            p.setPen(Qt.NoPen)
            p.setBrush(QColor(0, 0, 0, a))
            p.drawRoundedRect(
                r.adjusted(-i * 0.8, -i * 0.6 + 1, i * 0.8, i * 0.6 + 2), 3 + i, 3 + i)

    def paintEvent(self, ev: QPaintEvent) -> None:
        p = QPainter(self)
        r = self._img_rect()
        if self._shadow:
            self._draw_shadow(p, r)
        if self._scale < 0.5:
            src = self._thumb_for()
            p.setRenderHint(QPainter.SmoothPixmapTransform, True)
            p.drawImage(r, src)
        elif self._scale < 1.0:
            p.setRenderHint(QPainter.SmoothPixmapTransform, True)
            p.drawImage(r, self._source)
        else:
            p.setRenderHint(QPainter.SmoothPixmapTransform, False)
            p.drawImage(r, self._source)
        if self._border:
            c = self._border_color
            if self._glow:
                # 外层淡色光晕（宽度 3 的半透明同色描边，模拟发光）
                p.setPen(QPen(QColor(c.red(), c.green(), c.blue(), 70), 3.0))
                p.setBrush(Qt.NoBrush)
                p.drawRect(r.adjusted(-0.5, -0.5, 0.5, 0.5))
            p.setPen(QPen(c, 1.4))
            p.setBrush(Qt.NoBrush)
            p.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5))
        self._draw_size_label(p, r)

    def _flash_label(self) -> None:
        self._label_timer.start(900)
        self.update()

    def _draw_size_label(self, p: QPainter, r: QRectF) -> None:
        """在图像角落绘制当前显示尺寸标签（缩放/变换后 0.9 秒内可见）。

        Args:
            p: 画笔。
            r: 图像在窗口内的逻辑矩形。
        """
        if self._label_timer.isActive():
            w, h = self._img_size()
            text = f"{round(w * self._scale)} × {round(h * self._scale)}"
            fm = p.fontMetrics()
            tw, th = fm.horizontalAdvance(text) + 12, fm.height() + 4
            x = r.right() - tw
            y = r.bottom() + 3
            if y + th > self.height():
                y = r.top() - th - 3
            p.setPen(Qt.NoPen)
            p.setBrush(QColor(20, 20, 24, 210))
            p.drawRoundedRect(QRectF(x, y, tw, th), 4, 4)
            p.setPen(QColor(255, 255, 255, 230))
            p.drawText(QRectF(x, y, tw, th), Qt.AlignCenter, text)
