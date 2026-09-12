"""托盘：白色紧凑右键菜单 + 全项动作。"""
from __future__ import annotations

import logging
import os
import sys

import app_icon
from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QAction
from PySide6.QtWidgets import (
    QApplication,
    QMenu,
    QSystemTrayIcon,
)

log = logging.getLogger("zpin.tray")

MENU_STYLE = """
QMenu {
    background: rgba(255, 255, 255, 248);
    color: #202020;
    border: 1px solid rgba(0, 0, 0, 14);
    border-radius: 7px;
    padding: 2px;
    font-size: 11px;
}
QMenu::item { padding: 2px 16px 2px 8px; border-radius: 3px; }
QMenu::item:selected { background: rgba(30, 120, 215, 70); color: #FFFFFF; }
QMenu::separator { height: 1px; background: rgba(0, 0, 0, 10); margin: 2px 5px; }
QMenu::indicator { width: 0px; height: 0px; }
QToolTip { background:#FFFFFF; color:#202020; border:1px solid rgba(0,0,0,14); }
"""


def _promote_visible() -> None:
    """Win11 默认把新托盘图标收进 ^ 折叠区；把自己提升为常驻显示（仅打包版）。"""
    if not getattr(sys, "frozen", False):
        return
    try:
        import winreg

        exe = os.path.normcase(os.path.abspath(sys.executable))
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER, r"Control Panel\NotifyIconSettings"
        ) as root:
            i = 0
            while True:
                try:
                    sub = winreg.EnumKey(root, i)
                    i += 1
                except OSError:
                    break
                try:
                    with winreg.OpenKey(root, sub, 0, winreg.KEY_READ | winreg.KEY_WRITE) as k:
                        path = os.path.normcase(winreg.QueryValueEx(k, "ExecutablePath")[0])
                        if path == exe:
                            winreg.SetValueEx(k, "IsPromoted", 0, winreg.REG_DWORD, 1)
                except OSError:
                    continue
    except Exception:
        log.exception("提升托盘图标显示失败（不影响运行，可手动从 ^ 拖出）")


class TrayController(QSystemTrayIcon):
    capture_requested = Signal(str)          # full / normal
    toggle_pins_requested = Signal()
    pin_clipboard_requested = Signal()       # 把剪贴板里的图片贴到屏幕
    switch_group_requested = Signal()
    clear_history_requested = Signal()
    history_selected = Signal(int)          # 历史条目操作：重贴
    history_copy_requested = Signal(int)    # 历史条目操作：复制
    history_save_requested = Signal(int)    # 历史条目操作：另存为
    shortcuts_disabled_toggled = Signal(bool)
    group_selected = Signal(str)
    group_create_requested = Signal()
    group_rename_requested = Signal()
    group_remove_requested = Signal()
    prefs_requested = Signal()
    help_requested = Signal()
    restart_requested = Signal()

    def __init__(self, accels: dict[str, str]) -> None:
        super().__init__(app_icon.app_icon())
        self.setToolTip("ZPin")

        self.menu = QMenu()
        self.menu.setAttribute(Qt.WA_TranslucentBackground)
        self.menu.setStyleSheet(MENU_STYLE)

        self._accels = accels
        self._pins_hidden = False
        self._accel_items: list[tuple[str, str, QAction]] = []  # (标签, 动作, 菜单项)

        self._build_menu()
        self.setContextMenu(self.menu)
        self.activated.connect(self._on_activated)
        self.show()
        _promote_visible()

    def _text(self, label: str, action: str) -> str:
        accel = self._accels.get(action, "")
        return f"{label}\t{accel}" if accel else label

    def _reg(self, label: str, action: str, act: QAction) -> QAction:
        """登记带快捷键提示的菜单项，改键后可整体刷新文本。"""
        self._accel_items.append((label, action, act))
        return act

    def _build_menu(self) -> None:
        """按「截图 -> 贴图管理 -> 历史 -> 快捷键开关 -> 应用」装配托盘菜单。"""
        # ---- 截图 ----
        act_full = self._reg("全屏截图", "capture_full",
                             QAction(self._text("全屏截图", "capture_full"), self.menu))
        act_full.triggered.connect(lambda: self.capture_requested.emit("full"))
        self.menu.addAction(act_full)

        act_capture = self._reg("框选截图", "capture",
                                QAction(self._text("框选截图", "capture"), self.menu))
        act_capture.triggered.connect(lambda: self.capture_requested.emit("normal"))
        self.menu.addAction(act_capture)
        self.menu.addSeparator()

        # ---- 贴图管理 ----
        self.pin_menu = QMenu(self.menu)
        act_next = self._reg("下一组", "switch_group",
                             QAction(self._text("下一组", "switch_group"), self.pin_menu))
        act_next.triggered.connect(self.switch_group_requested.emit)
        self.pin_menu.addAction(act_next)
        act_clip = QAction("贴剪贴板图片", self.pin_menu)
        act_clip.triggered.connect(self.pin_clipboard_requested.emit)
        self.pin_menu.addAction(act_clip)
        self.pin_menu.addSeparator()
        self._rebuild_pin_menu([])
        act_pin_menu = QAction("贴图管理", self.menu)
        act_pin_menu.setMenu(self.pin_menu)
        self.menu.addAction(act_pin_menu)

        self.act_toggle = QAction("", self.menu)
        self.act_toggle.triggered.connect(self.toggle_pins_requested.emit)
        self.menu.addAction(self.act_toggle)
        self.set_pins_hidden(False)

        self.menu.addSeparator()

        # ---- 截图历史 ----
        self.history_menu = QMenu(self.menu)
        self._rebuild_history([])
        act_history = QAction("截图历史", self.menu)
        act_history.setMenu(self.history_menu)
        self.menu.addAction(act_history)
        self.menu.addSeparator()

        # ---- 全局快捷键 ----
        self.act_disable = QAction("停用全局快捷键", self.menu)
        self.act_disable.setCheckable(True)
        self.act_disable.toggled.connect(self._on_disable_toggled)
        self.menu.addAction(self.act_disable)
        self.menu.addSeparator()

        # ---- 应用 ----
        act_prefs = QAction("设置", self.menu)
        act_prefs.triggered.connect(self.prefs_requested.emit)
        self.menu.addAction(act_prefs)

        act_help = QAction("帮助", self.menu)
        act_help.triggered.connect(self.help_requested.emit)
        self.menu.addAction(act_help)

        act_restart = QAction("重新启动", self.menu)
        act_restart.triggered.connect(self.restart_requested.emit)
        self.menu.addAction(act_restart)

        act_quit = QAction("退出", self.menu)
        act_quit.triggered.connect(QApplication.quit)
        self.menu.addAction(act_quit)

    # ---- 状态与气泡 ----
    def notify(self, title: str, text: str) -> None:
        self.showMessage(title, text, QSystemTrayIcon.Information, 5000)

    def set_accels(self, accels: dict[str, str]) -> None:
        """热键被修改/恢复默认后，刷新菜单右侧的快捷键提示。"""
        self._accels = accels
        for label, action, act in self._accel_items:
            accel = accels.get(action, "")
            act.setText(f"{label}\t{accel}" if accel else label)
        # 「隐藏/显示全部贴图」文本含显隐状态 + 快捷键，随新键重刷
        self.set_pins_hidden(self._pins_hidden)

    def set_shortcuts_disabled(self, disabled: bool) -> None:
        # 停用时托盘图标 Z 两端变黄（app_icon 变体），菜单文字同步
        self.setIcon(app_icon.app_icon(disabled))
        self.act_disable.setChecked(disabled)
        self.act_disable.setText("启用全局快捷键" if disabled else "停用全局快捷键")

    def _on_disable_toggled(self, on: bool) -> None:
        self.act_disable.setText("启用全局快捷键" if on else "停用全局快捷键")
        self.shortcuts_disabled_toggled.emit(on)

    def set_pins_hidden(self, hidden: bool) -> None:
        """同步「全部贴图 隐藏/显示」状态：菜单文字随状态切换。"""
        self._pins_hidden = hidden
        label = "显示全部贴图" if hidden else "隐藏全部贴图"
        accel = self._accels.get("toggle_pins", "")
        self.act_toggle.setText(f"{label}\t{accel}" if accel else label)

    def set_groups(self, groups: list[tuple[str, int, bool]]) -> None:
        """(组名, 贴图数, 是否当前组)，重建「贴图管理」组列表部分。"""
        self._rebuild_pin_menu(groups)

    def _rebuild_pin_menu(self, groups: list[tuple[str, int, bool]]) -> None:
        # 保留构造期固定的「下一组 + 贴剪贴板图片」（前两个 action），其后全部重建
        while self.pin_menu.actions() and len(self.pin_menu.actions()) > 2:
            self.pin_menu.removeAction(self.pin_menu.actions()[-1])
        if self.pin_menu.actions():
            self.pin_menu.addSeparator()
        if not groups:
            empty = QAction("（暂无贴图）", self.pin_menu)
            empty.setEnabled(False)
            self.pin_menu.addAction(empty)
        for name, count, current in groups:
            label = f"{name}（{count}）" + ("（当前）" if current else "")
            act = QAction(label, self.pin_menu)
            act.setCheckable(True)
            act.setChecked(current)
            act.triggered.connect(lambda _=False, n=name: self.group_selected.emit(n))
            self.pin_menu.addAction(act)
        self.pin_menu.addSeparator()
        self.pin_menu.addAction("新建贴图组...", self.group_create_requested.emit)
        self.pin_menu.addAction("重命名当前组...", self.group_rename_requested.emit)
        self.pin_menu.addAction("删除当前组", self.group_remove_requested.emit)

    def set_history(self, entries: list[tuple[int, str]]) -> None:
        """(序号, 显示文本)，重建「历史」子菜单。"""
        self._rebuild_history(entries)

    def _rebuild_history(self, entries: list[tuple[int, str]]) -> None:
        """每条截图历史一个子菜单：重贴 / 复制 / 另存为。"""
        self.history_menu.clear()
        if not entries:
            empty = QAction("（暂无截图）", self.history_menu)
            empty.setEnabled(False)
            self.history_menu.addAction(empty)
            return
        for idx, text in entries:
            sub = self.history_menu.addMenu(text)
            a1 = QAction("重贴", sub)
            a1.triggered.connect(lambda _=False, i=idx: self.history_selected.emit(i))
            a2 = QAction("复制", sub)
            a2.triggered.connect(lambda _=False, i=idx: self.history_copy_requested.emit(i))
            a3 = QAction("另存为...", sub)
            a3.triggered.connect(lambda _=False, i=idx: self.history_save_requested.emit(i))
            sub.addAction(a1)
            sub.addAction(a2)
            sub.addAction(a3)
        self.history_menu.addSeparator()
        self.history_menu.addAction("清空截图历史", self.clear_history_requested.emit)

    def _on_activated(self, reason: QSystemTrayIcon.ActivationReason) -> None:
        if reason in (QSystemTrayIcon.Trigger, QSystemTrayIcon.DoubleClick):
            self.capture_requested.emit("normal")
