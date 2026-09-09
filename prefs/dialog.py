"""首选项对话框 —— QTabWidget 七标签页 + 恢复默认 + 热键即时应用。"""
from __future__ import annotations

import logging
from collections.abc import Callable

from PySide6.QtWidgets import (
    QDialog,
    QHBoxLayout,
    QLabel,
    QMessageBox,
    QPushButton,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

import config
import defaults
import history
import startup
from . import pages
from .key_edit import HotkeyEdit

log = logging.getLogger("zpin.prefs")


class PreferencesDialog(QDialog):
    """首选项对话框：QTabWidget 七标签页 + 恢复默认 + 热键即时应用。"""

    def __init__(self, apply_hotkeys: Callable[[], dict[str, bool]],
                 apply_font: Callable[[], None],
                 apply_log_level: Callable[[], None],
                 set_hotkeys_disabled: Callable[[bool], None],
                 parent: QWidget | None = None) -> None:
        """构建七标签页并按当前配置初始化热键总开关与状态显示。

        Args:
            apply_hotkeys: 按当前配置全量重绑热键，返回 {action: 是否注册成功}。
            apply_font: 应用全局字体。
            apply_log_level: 应用日志级别。
            set_hotkeys_disabled: 设置热键总开关。
            parent: 父控件。
        """
        super().__init__(parent)
        self.setWindowTitle("ZPin 设置")
        self.setModal(False)
        self.setMinimumWidth(430)
        self._apply_hotkeys = apply_hotkeys
        self._apply_font = apply_font
        self._apply_log_level = apply_log_level
        self._set_hotkeys_disabled = set_hotkeys_disabled
        self._edits: dict[str, HotkeyEdit] = {}
        self._status: dict[str, QLabel] = {}
        self._build()
        self.set_hotkeys_disabled(bool(config.get("Hotkeys/disabled")))
        self.refresh_hotkey_status()

    # ---- 构建 ----
    def _build(self) -> None:
        """构建标签页与底部按钮；重建时整体替换 _body，避免旧控件叠加。"""
        self._edits.clear()
        self._status.clear()
        # 整块（标签页 + 底部按钮）换掉，避免重建时叠加出第二套控件
        old = getattr(self, "_body", None)
        if old is not None:
            self.layout().removeWidget(old)
            old.deleteLater()
        lay = self.layout() or QVBoxLayout(self)
        body = QWidget(self)
        self._body = body
        body_lay = QVBoxLayout(body)
        body_lay.setContentsMargins(0, 0, 0, 0)
        tabs = QTabWidget(body)
        self._tabs = tabs
        tabs.addTab(pages.build_general(self), "常规")
        tabs.addTab(pages.build_interface(self), "界面")
        tabs.addTab(pages.build_capture(self), "截图")
        tabs.addTab(pages.build_pin(self), "贴图")
        tabs.addTab(pages.build_output(self), "输出")
        tabs.addTab(pages.build_control(self), "快捷键")
        tabs.addTab(pages.build_about(), "关于")
        body_lay.addWidget(tabs)

        bottom = QHBoxLayout()
        done = QPushButton("完成")
        done.setDefault(True)
        done.clicked.connect(self.close)
        bottom.addStretch(1)
        bottom.addWidget(done)
        body_lay.addLayout(bottom)
        lay.addWidget(body)

    def register_hotkey_row(self, action: str, edit: HotkeyEdit, status: QLabel) -> None:
        """登记一行热键编辑框与状态标签，并接上变更回调。

        Args:
            action: 动作名（对应 Hotkeys/<action> 配置键）。
            edit: 热键捕获输入框。
            status: 冲突/注册失败的状态提示标签。
        """
        self._edits[action] = edit
        self._status[action] = status
        edit.accel_changed.connect(lambda _t, a=action: self._on_accel_changed(a))

    def set_hotkeys_disabled(self, disabled: bool) -> None:
        """切换热键总开关并刷新各行状态显示。

        Args:
            disabled: True 为禁用全部全局快捷键。
        """
        self._set_hotkeys_disabled(disabled)
        self.refresh_hotkey_status()

    def apply_log_level(self) -> None:
        """委托外部回调应用当前日志级别。"""
        self._apply_log_level()

    # ---- 热键 ----
    def _on_accel_changed(self, action: str) -> None:
        """单行热键变更：写配置并重绑；冲突或注册失败时回滚并提示。"""
        conflicts = self._validate_hotkeys()
        if action in conflicts:
            return
        key = f"Hotkeys/{action}"
        text = self._edits[action].text().strip()
        prev = str(config.get(key))
        config.set(key, text)
        config.sync()
        if bool(config.get("Hotkeys/disabled")):
            # 全局禁用中：apply_bindings 自会只记账不注册，这里照常调用，
            # 让管理器记住新键，总开关恢复后即按新值生效
            self._apply_hotkeys()
            self.refresh_hotkey_status()
            return
        results = self._apply_hotkeys()
        if text and not results.get(action, True):
            # 新键被其它程序占用：回滚配置与输入框，并恢复旧键的注册
            config.set(key, prev)
            config.sync()
            self._edits[action].setText(prev)
            st = self._status[action]
            st.setText("注册失败，可能被其他程序占用（已恢复原键）")
            st.show()
            self._apply_hotkeys()
            return
        self.refresh_hotkey_status()

    def refresh_hotkey_status(self) -> None:
        """按当前实际注册结果刷新每行的状态行（打开对话框时也调一次）。"""
        disabled = bool(config.get("Hotkeys/disabled"))
        results = self._apply_hotkeys() if not disabled else {}
        conflicts = self._validate_hotkeys()
        for a, st in self._status.items():
            text = self._edits[a].text().strip()
            if a in conflicts:
                continue
            if disabled:
                if text:
                    st.setText("已禁用全局快捷键")
                    st.show()
                else:
                    st.hide()
            elif text and not results.get(a, True):
                st.setText("注册失败，可能被其他程序占用")
                st.show()
            else:
                st.hide()

    def _validate_hotkeys(self) -> set[str]:
        """检查各行加速键文本是否重复，冲突的行标红提示。

        Returns:
            set[str]: 与其他动作冲突的动作名集合。
        """
        seen: dict[str, list[str]] = {}
        for a, ed in self._edits.items():
            t = ed.text().strip()
            if t:
                seen.setdefault(t, []).append(a)
        conflicted = {a for lst in seen.values() if len(lst) > 1 for a in lst}
        for a, st in self._status.items():
            if a in conflicted:
                st.setText("与其他动作冲突")
                st.show()
            elif not st.text().startswith("注册失败"):
                st.hide()
        return conflicted

    # ---- 恢复默认（每页独立） ----
    def _sync_side_effects(self) -> None:
        """把 config 当前值应用到各子系统（恢复某页后整体同步，保证幂等）。"""
        self._apply_log_level()
        self._apply_font()
        self._set_hotkeys_disabled(bool(config.get("Hotkeys/disabled")))
        startup.set_enabled(bool(config.get("General/autostart")))
        history.set_limits()

    def restore_page(self, title: str, prefixes: tuple[str, ...]) -> None:
        """只恢复指定前缀的设置键（如 ("Capture/",)），其它页不受影响。

        NON_RESETTABLE_KEYS 里的内部状态/用户数据（迁移标记、上次格式、贴图组名）
        永不覆盖。

        Args:
            title: 页面标题，用于确认弹窗文案。
            prefixes: 要恢复的配置键前缀元组。
        """
        affected = [k for k in defaults.DEFAULTS
                    if k.startswith(prefixes) and k not in defaults.NON_RESETTABLE_KEYS]
        if not affected:
            return
        if QMessageBox.question(
            self, "恢复默认",
            f"将「{title}」页的全部设置恢复为默认值？\n（仅本页，其它页不受影响）",
            QMessageBox.Yes | QMessageBox.No, QMessageBox.No,
        ) != QMessageBox.Yes:
            return
        for key in affected:
            config.set(key, defaults.DEFAULTS[key])
        config.sync()
        self._sync_side_effects()
        self._build()            # 整页重建，读到新值
        self._apply_hotkeys()    # 热键若有变化按新值重绑
        self.refresh_hotkey_status()
        log.info("已恢复「%s」页默认设置（%d 项）", title, len(affected))
