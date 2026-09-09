"""首选项页面部件 —— 绑定配置键的控件 + 七个标签页的构建函数。

所有改动即时写入 config；恢复默认时由对话框整页重建。
"""
from __future__ import annotations

from collections.abc import Callable
from typing import TYPE_CHECKING

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QColor, QFont
from PySide6.QtWidgets import (
    QCheckBox,
    QColorDialog,
    QComboBox,
    QFileDialog,
    QFontComboBox,
    QFormLayout,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

import config
import defaults
import history
import hotkey
import output
import startup
from .key_edit import HotkeyEdit

if TYPE_CHECKING:
    from .dialog import PreferencesDialog


def qt_color_text(c: QColor) -> str:
    """QColor -> 文本：带 Alpha 时用 #AARRGGBB，否则用 #RRGGBB 大写。"""
    if c.alpha() < 255:
        return f"#{c.alpha():02X}{c.red():02X}{c.green():02X}{c.blue():02X}"
    return c.name().upper()


class BoundCheck(QCheckBox):
    """勾选框与布尔配置键双向绑定（勾上写 True）。"""

    def __init__(self, text: str, key: str,
                 on_change: Callable[[bool], None] | None = None) -> None:
        super().__init__(text)
        self._key = key
        self._on_change = on_change
        self.setChecked(bool(config.get(key)))
        self.toggled.connect(self._apply)

    def _apply(self, on: bool) -> None:
        config.set(self._key, on)
        config.sync()
        if self._on_change:
            self._on_change(on)


class InvertedCheck(QCheckBox):
    """配置键 True=关闭 的反向勾选（如 disable_guides）。

    Args:
        text: 显示文案。
        key: 配置键；控件勾选态 = not 配置值。
    """

    def __init__(self, text: str, key: str) -> None:
        super().__init__(text)
        self._key = key
        self.setChecked(not bool(config.get(key)))
        self.toggled.connect(self._apply)

    def _apply(self, on: bool) -> None:
        config.set(self._key, not on)
        config.sync()


class BoundSpin(QSpinBox):
    """整数微调框与整型配置键绑定；下限为负时把下限显示为「默认」。"""

    def __init__(self, key: str, lo: int, hi: int, suffix: str = "",
                 on_change: Callable[[int], None] | None = None) -> None:
        super().__init__()
        self._key = key
        self._on_change = on_change
        self.setRange(lo, hi)
        if suffix:
            self.setSuffix(suffix)
        if lo <= 0 < hi or (lo < 0 and hi >= 0):
            self.setSpecialValueText("默认" if lo < 0 else str(lo))
        self.setValue(int(config.get(key)))
        self.valueChanged.connect(self._apply)

    def _apply(self, v: int) -> None:
        config.set(self._key, v)
        config.sync()
        if self._on_change:
            self._on_change(v)


class BoundCombo(QComboBox):
    """下拉框与字符串配置键绑定（当前值不在选项里时停在第一项）。"""

    def __init__(self, key: str, items: list[str],
                 on_change: Callable[[str], None] | None = None) -> None:
        super().__init__()
        self._key = key
        self._on_change = on_change
        self.addItems(items)
        cur = str(config.get(key))
        if cur in items:
            self.setCurrentText(cur)
        self.currentTextChanged.connect(self._apply)

    def _apply(self, text: str) -> None:
        config.set(self._key, text)
        config.sync()
        if self._on_change:
            self._on_change(text)


class ColorButton(QPushButton):
    """色块按钮：点击弹 QColorDialog（遮罩色带 Alpha）。"""

    color_changed = Signal(QColor)

    def __init__(self, key: str) -> None:
        super().__init__()
        self._key = key
        self.setFixedSize(72, 22)
        self._sync_swatch(QColor(str(config.get(key))))
        self.clicked.connect(self._pick)

    def _pick(self) -> None:
        c = QColorDialog.getColor(
            QColor(str(config.get(self._key))), self, "选择颜色",
            QColorDialog.ShowAlphaChannel,
        )
        if c.isValid():
            config.set(self._key, qt_color_text(c))
            config.sync()
            self._sync_swatch(c)
            self.color_changed.emit(c)

    def _sync_swatch(self, c: QColor) -> None:
        self.setStyleSheet(
            f"QPushButton {{ background: rgba({c.red()},{c.green()},{c.blue()},{c.alpha()});"
            "border: 1px solid #888; border-radius: 3px; }"
        )


def _page(form: QFormLayout) -> QWidget:
    w = QWidget()
    w.setLayout(form)
    return w


def _restore_row(dialog: PreferencesDialog, title: str,
                 prefixes: tuple[str, ...]) -> QWidget:
    """页面底部「仅恢复本页」按钮。prefixes 匹配 defaults.DEFAULTS 的键前缀。"""
    row = QWidget()
    lay = QHBoxLayout(row)
    lay.setContentsMargins(0, 6, 0, 0)
    lay.addStretch(1)
    btn = QPushButton(f"恢复「{title}」默认")
    btn.setCursor(Qt.PointingHandCursor)
    btn.clicked.connect(lambda: dialog.restore_page(title, prefixes))
    lay.addWidget(btn)
    return row


def build_general(dialog: PreferencesDialog | None = None) -> QWidget:
    """常规页：自启、备份、内存整理、日志级别、历史条数/占用上限。"""
    form = QFormLayout()
    form.addRow("", BoundCheck("开机自动运行", "General/autostart",
                               on_change=startup.set_enabled))
    form.addRow("", BoundCheck("启动时自动备份配置", "General/auto_backup"))
    form.addRow("", BoundCheck("保持响应（空闲时整理内存，重启后生效）",
                               "General/keep_responsive"))
    on_log = dialog.apply_log_level if dialog is not None else None
    form.addRow("日志级别", BoundCombo("General/log_level", ["普通", "详细"],
                                       on_change=on_log))
    form.addRow("历史保留条数", BoundSpin("General/history_limit", 0, 100, " 条",
                                    on_change=history.set_limits))
    form.addRow("历史占用上限", BoundSpin("General/history_max_mb", 16, 512, " MB",
                                     on_change=history.set_limits))
    if dialog is not None:
        form.addRow("", _restore_row(dialog, "常规", ("General/",)))
    return _page(form)


def build_interface(dialog: PreferencesDialog) -> QWidget:
    """界面页：字体与主题色。"""
    form = QFormLayout()
    font_row = QHBoxLayout()
    combo = QFontComboBox()
    spin = QSpinBox()
    spin.setRange(6, 24)
    spin.setSuffix(" pt")

    def _apply_font() -> None:
        config.set("Interface/font", f"{combo.currentFont().family()},{spin.value()}")
        config.sync()
        dialog.apply_font()

    name, size = defaults.font_tuple(config.get("Interface/font"))
    combo.setCurrentFont(QFont(name))
    spin.setValue(size)
    combo.currentFontChanged.connect(_apply_font)
    spin.valueChanged.connect(_apply_font)
    font_row.addWidget(combo, 1)
    font_row.addWidget(spin)
    wrap = QWidget()
    wrap.setLayout(font_row)
    form.addRow("字体", wrap)
    form.addRow("主题色", ColorButton("Interface/theme_color"))
    form.addRow("", _restore_row(dialog, "界面", ("Interface/",)))
    return _page(form)


def build_capture(dialog: PreferencesDialog | None = None) -> QWidget:
    """截图页：边框、遮罩色、元素吸附、锚点、参考线、十字线、快捷键提示。"""
    form = QFormLayout()
    form.addRow("边框宽度", BoundSpin("Capture/border_width", 1, 10, " px"))
    form.addRow("遮罩颜色", ColorButton("Capture/mask_color"))
    form.addRow("", BoundCheck("吸附界面元素（按钮、文字块等）", "Capture/snap_elements"))
    form.addRow("", BoundCheck("拖拽时对齐参考线（屏幕/窗口的边与中线）", "Capture/snap_guides"))
    form.addRow("", BoundCheck("显示可调节锚点", "Capture/show_anchors"))
    form.addRow("锚点描边颜色", ColorButton("Capture/anchor_stroke_color"))
    form.addRow("", BoundCheck("显示全屏十字线", "Capture/show_crosshair"))
    form.addRow("", InvertedCheck("显示辅助线", "Capture/disable_guides"))
    form.addRow("", BoundCheck("显示快捷键提示", "Capture/show_hints"))
    if dialog is not None:
        form.addRow("", _restore_row(dialog, "截图", ("Capture/",)))
    return _page(form)


def build_pin(dialog: PreferencesDialog | None = None) -> QWidget:
    """贴图页：阴影、默认不透明度、边框色、发光、窗口尺寸上限。"""
    form = QFormLayout()
    form.addRow("", BoundCheck("显示阴影", "Pin/shadow"))
    form.addRow("默认不透明度", BoundSpin("Pin/default_opacity", 10, 100, " %"))
    form.addRow("边框颜色", ColorButton("Pin/border_color"))
    form.addRow("", BoundCheck("描边外发光（对新贴图生效）", "Pin/border_glow"))
    form.addRow("贴图窗口尺寸上限", BoundSpin("Pin/max_window_size", 500, 30000, " px"))
    tip_size = QLabel("新贴图按此上限自动缩小；已打开的贴图不受影响。")
    tip_size.setStyleSheet("color: #888;")
    tip_size.setWordWrap(True)
    form.addRow("", tip_size)
    if dialog is not None:
        form.addRow("", _restore_row(dialog, "贴图", ("Pin/",)))
    return _page(form)


def _dir_picker_row(key: str, title: str) -> QWidget:
    """只读路径输入框 + 「浏览...」按钮；选定后写回 config[key]。"""
    row = QHBoxLayout()
    edit = QLineEdit(str(config.get(key)))
    edit.setReadOnly(True)
    edit.setMinimumWidth(200)

    def _browse() -> None:
        d = QFileDialog.getExistingDirectory(edit, title, edit.text())
        if d:
            edit.setText(d)
            config.set(key, d)
            config.sync()

    btn = QPushButton("浏览...")
    btn.clicked.connect(_browse)
    row.addWidget(edit, 1)
    row.addWidget(btn)
    wrap = QWidget()
    wrap.setLayout(row)
    return wrap


def build_output(dialog: PreferencesDialog | None = None) -> QWidget:
    """输出页：文件名模板（带实时预览）、JPEG 质量、另存为/自动保存目录。"""
    form = QFormLayout()
    tpl_edit = QLineEdit(str(config.get("Output/name_template")))
    tpl_edit.setMinimumWidth(220)
    preview = QLabel()
    preview.setStyleSheet("color: #666;")

    def _apply_tpl(text: str) -> None:
        config.set("Output/name_template", text)
        config.sync()
        preview.setText("预览：" + output.render_name(text))

    tpl_edit.textEdited.connect(_apply_tpl)
    preview.setText("预览：" + output.render_name(tpl_edit.text()))
    form.addRow("文件名模板", tpl_edit)
    form.addRow("", preview)
    form.addRow("JPEG 质量", BoundSpin("Output/quality", 30, 100))
    q_tip = QLabel("仅影响存为 jpg 的质量；PNG / BMP 一律无损。95 已接近视觉无损。")
    q_tip.setStyleSheet("color: #888;")
    q_tip.setWordWrap(True)
    form.addRow("", q_tip)
    form.addRow("", BoundCheck("记住上次保存的格式", "Output/remember_ext"))
    form.addRow("另存为默认目录", _dir_picker_row("Output/default_dir", "选择保存目录"))

    tip_save = QLabel("截图保存与贴图另存为都会弹出「另存为」对话框选择位置，程序不设固定保存地址。")
    tip_save.setStyleSheet("color: #888;")
    tip_save.setWordWrap(True)
    form.addRow("", tip_save)

    form.addRow("", BoundCheck("每次截图自动保存一份", "Output/auto_save"))
    form.addRow("自动保存目录", _dir_picker_row("Output/auto_save_dir", "选择自动保存目录"))
    if dialog is not None:
        form.addRow("", _restore_row(dialog, "输出", ("Output/",)))
    return _page(form)


def build_control(dialog: PreferencesDialog) -> QWidget:
    """快捷键页：总开关 + 四个动作的捕获式改键行（冲突标红）。"""
    page = QWidget()
    lay = QVBoxLayout(page)
    lay.addWidget(BoundCheck("禁用所有快捷键", "Hotkeys/disabled",
                             on_change=dialog.set_hotkeys_disabled))
    grid = QGridLayout()
    grid.setHorizontalSpacing(12)
    grid.setVerticalSpacing(6)
    for row, (action, label) in enumerate(hotkey.ACTION_LABELS.items()):
        accel = str(config.get(f"Hotkeys/{action}"))
        edit = HotkeyEdit(accel)
        status = QLabel()
        status.setStyleSheet("color: #D33;")
        status.hide()
        cell = QWidget()
        cell_lay = QVBoxLayout(cell)
        cell_lay.setContentsMargins(0, 0, 0, 0)
        cell_lay.setSpacing(1)
        cell_lay.addWidget(edit)
        cell_lay.addWidget(status)
        clear = QPushButton("清除")
        clear.setFixedWidth(44)
        clear.clicked.connect(edit.clear_accel)
        grid.addWidget(QLabel(label), row, 0)
        grid.addWidget(cell, row, 1)
        grid.addWidget(clear, row, 2)
        dialog.register_hotkey_row(action, edit, status)
    wrap = QWidget()
    wrap.setLayout(grid)
    lay.addWidget(wrap)
    tip = QLabel("点击输入框后按下新组合键；Esc 取消，留空为清除。冲突会标红且不生效。")
    tip.setStyleSheet("color: #888;")
    tip.setWordWrap(True)
    lay.addWidget(tip)
    lay.addWidget(_restore_row(dialog, "快捷键", ("Hotkeys/",)))
    lay.addStretch(1)
    return page


def build_about() -> QWidget:
    """关于页：版本与技术栈说明。"""
    page = QWidget()
    lay = QVBoxLayout(page)
    title = QLabel("ZPin")
    f = title.font()
    f.setPointSize(16)
    f.setBold(True)
    title.setFont(f)
    lay.addWidget(title)
    lay.addWidget(QLabel(f"版本 {defaults.VERSION}"))
    body = QLabel(
        "截图标注 + 贴图 + 全局快捷键 + 托盘常驻。\n"
        "技术栈：Python + PySide6 + Win32（ctypes）。\n\n"
        "使用：按全局快捷键或托盘菜单发起截图，选区后可标注、\n"
        "复制、贴图、保存；贴图支持缩放/透明度/分组。"
    )
    body.setWordWrap(True)
    lay.addWidget(body)
    lay.addStretch(1)
    return page
