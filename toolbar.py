"""截图标注工具条 —— 常用布局：撤销/重做 → 常用工具 → 颜色/粗细 → 完成动作 + ⋮ 更多。

一级只放高频工具（矩形/椭圆/直线/箭头/画笔/文本/马赛克），荧光笔/橡皮擦/气泡
标注收进 ⋮ 二级菜单，功能不丢、首屏不臃肿。图标全部用 SVG(Feather 风格线性) 渲染，
与预览图一致；浮动的 Qt.Tool 窗口，WA_ShowWithoutActivating 保持覆盖层持有键盘焦点，
自身也兜底转发 Esc/Enter/Ctrl+Z/Y/S。
"""
from __future__ import annotations

from collections.abc import Callable

from PySide6.QtCore import QEvent, QObject, QPoint, QRectF, QSize, Qt, Signal
from PySide6.QtGui import (
    QCloseEvent,
    QColor,
    QIcon,
    QImage,
    QKeyEvent,
    QMoveEvent,
    QPaintEvent,
    QPainter,
    QPixmap,
)
from PySide6.QtSvg import QSvgRenderer
from PySide6.QtWidgets import (
    QColorDialog,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QMenu,
    QToolButton,
    QVBoxLayout,
    QWidget,
    QWidgetAction,
)

# 一级工具：高频常用
TOOLS = [
    ("move", "移动选区（拖动截图框）"),
    ("rect", "矩形"),
    ("ellipse", "椭圆"),
    ("line", "直线"),
    ("arrow", "箭头"),
    ("pen", "画笔"),
    ("text", "文本"),
    ("mosaic", "马赛克"),
]
# ⋮ 更多菜单：低频但仍保留
MORE_TOOLS = [
    ("marker", "荧光笔"),
    ("eraser", "橡皮擦"),
    ("callout", "气泡标注"),
]

PALETTE = [
    "#E53935", "#FB8C00", "#FDD835", "#43A047",
    "#1E88E5", "#8E24AA", "#EC407A", "#00ACC1",
    "#212121", "#7E7E7E", "#FFFFFF", "#5C6BC0",
]

WIDTHS = [("细", 2.0), ("中", 4.0), ("粗", 8.0)]

TOOLBAR_STYLE = """
QToolButton { background: transparent; border: none; border-radius: 5px; padding: 3px; }
QToolButton:hover { background: rgba(255,255,255,28); }
QToolButton:pressed { background: rgba(255,255,255,44); }
QToolButton:checked { background: rgba(111,195,255,60); }
QMenu { background: rgba(23,23,28,244); color:#E8E8E8; border:1px solid rgba(255,255,255,32);
        border-radius: 8px; padding: 4px; font-size: 12px; }
QMenu::item { padding: 6px 22px 6px 8px; border-radius: 5px; }
QMenu::item:selected { background: rgba(255,255,255,30); }
"""

# ---- SVG 图标（24 视口 Feather 线性风格；颜色统一由调用方注入）----
# 每个图标一行 SVG 主体；未列出的名字兜底为空心圆。
def _svg_color(c: str, param: object) -> str:
    hex_c = param or c
    return (f'<circle cx="12" cy="12" r="8.4" fill="{hex_c}" '
            f'stroke="{c}" stroke-width="1.4" stroke-opacity="0.55"/>')


def _svg_width(c: str, param: object) -> str:
    d = float(param or 4.0)
    r = min(7.6, max(2.8, d * 1.1))
    return f'<circle cx="12" cy="12" r="{r:.1f}" fill="{c}"/>'


_SVG_BODIES: dict[str, Callable[[str, object], str]] = {
    "rect": lambda c, p: '<rect x="4" y="5" width="16" height="14" rx="2"/>',
    "ellipse": lambda c, p: '<ellipse cx="12" cy="12.5" rx="9" ry="6.6"/>',
    "line": lambda c, p: f'<line x1="4.5" y1="19.5" x2="19.5" y2="4.5" stroke="{c}"/>',
    "arrow": lambda c, p: (
        f'<line x1="4" y1="20" x2="16.4" y2="7.6" stroke="{c}"/>'
        f'<path d="M20.2 3.8l-1.8 6.6-4.8-4.6z" fill="{c}"/>'
    ),
    "pen": lambda c, p: '<path d="M17 3a2.83 2.83 0 1 1 4 4L7.5 20.5 2 22l1.5-5.5L17 3z"/>',
    "marker": lambda c, p: (f'<line x1="5.5" y1="18.5" x2="18.5" y2="5.5" stroke="{c}" '
                            f'stroke-width="5.2" stroke-linecap="round"/>'),
    "text": lambda c, p: (f'<path d="M6 5.5h12" stroke="{c}"/>'
                          f'<line x1="12" y1="5.5" x2="12" y2="18.5" stroke="{c}"/>'),
    "mosaic": lambda c, p: (
        '<rect x="3.6" y="3.6" width="7.4" height="7.4" rx="1"/>'
        '<rect x="13" y="3.6" width="7.4" height="7.4" rx="1"/>'
        '<rect x="3.6" y="13" width="7.4" height="7.4" rx="1"/>'
        '<rect x="13" y="13" width="7.4" height="7.4" rx="1"/>'
    ),
    "move": lambda c, p: (
        '<polyline points="5 9 2 12 5 15"/><polyline points="9 5 12 2 15 5"/>'
        '<polyline points="15 19 12 22 9 19"/><polyline points="19 9 22 12 19 15"/>'
        '<line x1="2" y1="12" x2="22" y2="12"/><line x1="12" y1="2" x2="12" y2="22"/>'
    ),
    "eraser": lambda c, p: (
        '<g transform="rotate(-38 12 17)">'
        '<rect x="6" y="13.5" width="12" height="7" rx="2"/>'
        f'<path d="M6 17h12" stroke="{c}"/>'
        '</g>'
    ),
    "callout": lambda c, p: (
        '<rect x="3" y="4.5" width="18" height="12.5" rx="3"/>'
        '<path d="M9.5 17l-3 4.5 6.5-4.5z"/>'
    ),
    "undo": lambda c, p: (
        '<polyline points="1 4 1 10 7 10"/>'
        '<path d="M3.51 15a9 9 0 1 0 2.13-9.36L1 10"/>'
    ),
    "redo": lambda c, p: (
        '<polyline points="23 4 23 10 17 10"/>'
        '<path d="M20.49 15a9 9 0 1 1-2.13-9.36L23 10"/>'
    ),
    "color": _svg_color,
    "width": _svg_width,
    "copy": lambda c, p: (
        '<rect x="9" y="9" width="13" height="13" rx="2"/>'
        '<path d="M5 15V5a2 2 0 0 1 2-2h10"/>'
    ),
    "pin": lambda c, p: (
        f'<circle cx="12" cy="6.6" r="4.4" stroke="{c}"/>'
        f'<line x1="12" y1="11" x2="12" y2="16.4" stroke="{c}"/>'
        f'<line x1="7.8" y1="19" x2="16.2" y2="19" stroke="{c}"/>'
    ),
    "save": lambda c, p: (
        '<path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11'
        'a2 2 0 0 1-2 2z"/>'
        '<polyline points="17 21 17 13 7 13 7 21"/>'
        '<polyline points="7 3 7 8 15 8"/>'
    ),
    "cancel": lambda c, p: ('<line x1="18" y1="6" x2="6" y2="18"/>'
                            '<line x1="6" y1="6" x2="18" y2="18"/>'),
    "more": lambda c, p: (f'<circle cx="5.2" cy="12" r="1.7" fill="{c}"/>'
                          f'<circle cx="12" cy="12" r="1.7" fill="{c}"/>'
                          f'<circle cx="18.8" cy="12" r="1.7" fill="{c}"/>'),
}


def _svg(name: str, color: str, sw: float = 2.0, param: object = None) -> str:
    """拼出单色线性 SVG；param 供 color/width 按钮传当前值。"""
    c = color
    make = _SVG_BODIES.get(name)
    body = make(c, param) if make else f'<circle cx="12" cy="12" r="8" stroke="{c}"/>'  # 兜底
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" '
        f'stroke="{c}" stroke-width="{sw}" stroke-linecap="round" stroke-linejoin="round">'
        f'{body}</svg>'
    )


def _glyph(name: str, color: str = "#E8E8E8", param: object = None) -> QIcon:
    """渲染 SVG 字符串 -> QIcon（20×20 透明底，内容收进 1px 安全边防贴边裁切）。"""
    pm = QPixmap(20, 20)
    pm.fill(Qt.transparent)
    svg = _svg(name, color, param=param).encode()
    renderer = QSvgRenderer(svg)
    if renderer.isValid():
        img = QImage(20, 20, QImage.Format_ARGB32)
        img.fill(Qt.transparent)
        p = QPainter(img)
        p.setRenderHint(QPainter.Antialiasing)
        renderer.render(p, QRectF(1, 1, 18, 18))
        p.end()
        pm = QPixmap.fromImage(img)
    return QIcon(pm)


class CaptureToolbar(QWidget):
    action = Signal(str)            # undo/redo/copy/pin/save/confirm/cancel
    tool_selected = Signal(str)     # TOOLS 名称 / MORE_TOOLS 名称
    color_selected = Signal(QColor)
    width_selected = Signal(float)

    def __init__(self, initial_color: QColor, initial_width: float) -> None:
        super().__init__(
            None,
            Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool,
        )
        self.setAttribute(Qt.WA_ShowWithoutActivating)
        # 圆角面板由 paintEvent 手绘（贴图窗同款方案）：QWidget 子类的样式表
        # background 不保证生效，无边框窗默认实底还会把圆角衬成方角
        self.setAttribute(Qt.WA_TranslucentBackground)
        self._color = QColor(initial_color)
        self._width = initial_width
        self._tool: str | None = None
        self._more: QMenu | None = None

        self.setStyleSheet(TOOLBAR_STYLE)
        lay = QVBoxLayout(self)
        lay.setContentsMargins(7, 5, 7, 5)
        lay.setSpacing(2)
        row = QWidget()
        row_lay = QHBoxLayout(row)
        row_lay.setContentsMargins(0, 0, 0, 0)
        row_lay.setSpacing(1)
        self._build_row(row_lay)
        lay.addWidget(row)
        self._sync_indicator_icons()
        self._build_tip()
        self._build_more_menu()

    def _add_sep(self, row_lay: QHBoxLayout) -> None:
        line = QFrame()
        line.setFrameShape(QFrame.VLine)
        line.setStyleSheet("QFrame { color: rgba(255,255,255,40); }")
        line.setFixedSize(1, 20)
        row_lay.addWidget(line)
        row_lay.addSpacing(3)

    def _add_btn(self, row_lay: QHBoxLayout, name: str, tip: str,
                 cb: Callable[[], None], checkable: bool = False) -> QToolButton:
        b = QToolButton()
        b.setIcon(_glyph(name))
        b.setIconSize(QSize(20, 20))
        b.setToolTip(tip)
        b.setCheckable(checkable)
        b.setFocusPolicy(Qt.NoFocus)
        b.setCursor(Qt.PointingHandCursor)
        b.installEventFilter(self)   # 自绘中文气泡（Qt 原生 tooltip 在非激活窗上不弹）
        b.clicked.connect(cb)
        row_lay.addWidget(b)
        return b

    def _add_color_width_btn(self, row_lay: QHBoxLayout, b: QToolButton) -> None:
        b.setFocusPolicy(Qt.NoFocus)
        b.setCursor(Qt.PointingHandCursor)
        b.installEventFilter(self)
        row_lay.addWidget(b)

    def _build_row(self, row_lay: QHBoxLayout) -> None:
        """按「撤销/重做 -> 常用工具 -> 颜色/粗细 -> 完成动作 -> ⋮」装配按钮行。"""
        # 1. 撤销 / 重做
        self._btn_undo = self._add_btn(row_lay, "undo", "撤销 (Ctrl+Z)",
                                       lambda: self.action.emit("undo"))
        self._btn_redo = self._add_btn(row_lay, "redo", "重做 (Ctrl+Y)",
                                       lambda: self.action.emit("redo"))
        self._add_sep(row_lay)

        # 2. 一级常用工具
        self._tool_buttons: dict[str, QToolButton] = {}
        for name, label in TOOLS:
            self._tool_buttons[name] = self._add_btn(
                row_lay, name, label, lambda _=False, n=name: self._select_tool(n),
                checkable=True)
        self._add_sep(row_lay)

        # 3. 颜色 / 粗细
        self._btn_color = QToolButton()
        self._btn_color.setIconSize(QSize(20, 20))
        self._btn_color.setToolTip("颜色")
        self._btn_color.clicked.connect(self._color_menu)
        self._add_color_width_btn(row_lay, self._btn_color)

        self._btn_width = QToolButton()
        self._btn_width.setIconSize(QSize(20, 20))
        self._btn_width.setToolTip("粗细")
        self._btn_width.clicked.connect(self._width_menu)
        self._add_color_width_btn(row_lay, self._btn_width)
        self._add_sep(row_lay)

        # 4. 完成动作：取消(Esc) 在左，复制 ✓(Enter) / 贴图 / 保存 在右
        self._add_btn(row_lay, "cancel", "取消 (Esc)", lambda: self.action.emit("cancel"))
        self._add_btn(row_lay, "copy", "复制 (Enter)", lambda: self.action.emit("copy"))
        self._add_btn(row_lay, "pin", "贴图", lambda: self.action.emit("pin"))
        self._add_btn(row_lay, "save", "保存 (Ctrl+S)", lambda: self.action.emit("save"))
        self._add_sep(row_lay)

        # 5. ⋮ 更多菜单（荧光笔 / 橡皮擦 / 气泡标注）
        self._btn_more = self._add_btn(row_lay, "more", "更多工具", self._more_menu)

    def paintEvent(self, ev: QPaintEvent) -> None:
        """手绘圆角深色面板：WA_TranslucentBackground 下透明四角才是真圆角。"""
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        p.setPen(Qt.NoPen)
        p.setBrush(QColor(23, 23, 28, 242))
        p.drawRoundedRect(QRectF(self.rect()).adjusted(0, 0, -1, -1), 8, 8)

    def _build_tip(self) -> None:
        """自绘中文气泡提示（顶层 Tool 窗，不拦截鼠标事件，跟随按钮显示中文名）。"""
        self._tip = QLabel(None, Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool)
        self._tip.setAttribute(Qt.WA_ShowWithoutActivating)
        self._tip.setAttribute(Qt.WA_TransparentForMouseEvents)
        self._tip.setAttribute(Qt.WA_TranslucentBackground)
        self._tip.setStyleSheet(
            "QLabel { background:#26262C; color:#F2F2F2; border:1px solid rgba(255,255,255,36);"
            " border-radius:6px; padding:3px 9px; font-size:12px; }")
        self._tip.hide()

    # ---- 自绘中文气泡 ----
    def eventFilter(self, obj: QObject, ev: QEvent) -> bool:
        if isinstance(obj, QToolButton):
            if ev.type() == QEvent.Enter and obj.toolTip():
                self._show_tip(obj.toolTip(), obj)
            elif ev.type() == QEvent.Leave:
                self._hide_tip()
        return super().eventFilter(obj, ev)

    def _show_tip(self, text: str, btn: QToolButton) -> None:
        if self._tip is None:      # 关闭/销毁过程中的迟到 Enter 事件
            return
        self._tip.setText(text)
        self._tip.adjustSize()
        geo = btn.screen().availableGeometry() if btn.screen() else QRectF().toRect()
        gx = btn.mapToGlobal(QPoint(btn.width() // 2, btn.height()))
        x = min(max(gx.x() - self._tip.width() // 2, geo.left() + 4),
                geo.right() - self._tip.width() - 4)
        y = gx.y() + 4
        if y + self._tip.height() > geo.bottom() and btn.y() > self._tip.height():
            y = gx.y() - self._tip.height() - 8   # 空间不足则放按钮上方
        self._tip.move(x, y)
        self._tip.show()
        self._tip.raise_()

    def _hide_tip(self) -> None:
        if self._tip is not None:
            self._tip.hide()

    def moveEvent(self, ev: QMoveEvent) -> None:   # 工具栏移动时收起气泡，避免残留
        if self._tip is not None:
            self._tip.hide()
        super().moveEvent(ev)

    def closeEvent(self, ev: QCloseEvent) -> None:  # 工具栏销毁时一并销毁气泡窗与菜单
        if self._tip is not None:
            self._tip.hide()
            self._tip.deleteLater()
            self._tip = None
        if self._more is not None:
            self._more.close()
            self._more.deleteLater()
            self._more = None
        super().closeEvent(ev)

    # ---- 状态 ----
    def set_enabled_actions(self, undo: bool, redo: bool) -> None:
        self._btn_undo.setEnabled(undo)
        self._btn_redo.setEnabled(redo)

    def _select_tool(self, name: str) -> None:
        for n, b in self._tool_buttons.items():
            b.setChecked(n == name)
        self._tool = None if name in ("", "none") else name
        self.tool_selected.emit(name)

    def _more_menu(self) -> None:
        """⋮ 菜单：popup 非阻塞弹出，再点一次 ⋮ 即收起（切换而非重开）。"""
        if self._more.isVisible():
            self._more.close()
            return
        self._more.popup(self.mapToGlobal(self.rect().bottomRight()))

    def _build_more_menu(self) -> None:
        """构建 ⋮ 二级工具菜单（持久实例，随工具栏销毁）。"""
        menu = QMenu(self)
        menu.setAttribute(Qt.WA_TranslucentBackground)
        for name, label in MORE_TOOLS:
            act = menu.addAction(_glyph(name), label)
            act.triggered.connect(lambda _=False, n=name: self._menu_pick(n))
        self._more = menu

    def _menu_pick(self, name: str) -> None:
        # 菜单工具不占用一级高亮：先清掉一级按钮选中态，避免上一把一级工具仍显高亮
        for b in self._tool_buttons.values():
            b.setChecked(False)
        self._tool = name
        self.tool_selected.emit(name)

    def _sync_indicator_icons(self) -> None:
        self._btn_color.setIcon(_glyph("color", param=self._color.name()))
        self._btn_width.setIcon(_glyph("width", param=self._width))

    # ---- 弹出菜单 ----
    def _color_menu(self) -> None:
        menu = QMenu(self)
        menu.setAttribute(Qt.WA_TranslucentBackground)
        grid_w = QWidget()
        grid = QGridLayout(grid_w)
        grid.setContentsMargins(6, 6, 6, 6)
        grid.setSpacing(5)
        for i, hex_color in enumerate(PALETTE):
            b = QToolButton()
            b.setFixedSize(22, 22)
            b.setCursor(Qt.PointingHandCursor)
            b.setStyleSheet(
                f"QToolButton {{ background: {hex_color}; border: 1px solid rgba(255,255,255,90);"
                "border-radius: 4px; } QToolButton:hover { border-color: #FFFFFF; }")
            b.clicked.connect(lambda _=False, hx=hex_color: self._pick_color(QColor(hx), menu))
            grid.addWidget(b, i // 6, i % 6)
        wa = QWidgetAction(menu)
        wa.setDefaultWidget(grid_w)
        menu.addAction(wa)
        custom = menu.addAction("自定义颜色...")
        custom.setFont(self.font())
        custom.triggered.connect(
            lambda: self._pick_color(QColorDialog.getColor(self._color, self, "自定义颜色"), menu))
        menu.exec(self.mapToGlobal(self.rect().bottomLeft()))
        menu.deleteLater()

    def _pick_color(self, color: QColor, menu: QMenu) -> None:
        menu.close()
        if not color.isValid():
            return
        self._color = color
        self._sync_indicator_icons()
        self.color_selected.emit(color)

    def _width_menu(self) -> None:
        menu = QMenu(self)
        menu.setAttribute(Qt.WA_TranslucentBackground)
        for label, w in WIDTHS:
            act = menu.addAction(_glyph("width", param=w), f"{label}（{w:.0f}px）")
            act.triggered.connect(lambda _=False, ww=w: self._pick_width(ww))
        menu.exec(self.mapToGlobal(self.rect().bottomLeft()))
        menu.deleteLater()

    def _pick_width(self, w: float) -> None:
        self._width = w
        self._sync_indicator_icons()
        self.width_selected.emit(w)

    # ---- 焦点兜底：工具条获得焦点时也响应常用键 ----
    def keyPressEvent(self, ev: QKeyEvent) -> None:
        k, m = ev.key(), ev.modifiers()
        if k == Qt.Key_Escape:
            self.action.emit("cancel")
        elif k in (Qt.Key_Return, Qt.Key_Enter):
            self.action.emit("copy")
        elif k == Qt.Key_Z and m & Qt.ControlModifier:
            self.action.emit("undo" if not m & Qt.ShiftModifier else "redo")
        elif k == Qt.Key_Y and m & Qt.ControlModifier:
            self.action.emit("redo")
        elif k == Qt.Key_S and m & Qt.ControlModifier:
            self.action.emit("save")
        else:
            super().keyPressEvent(ev)
