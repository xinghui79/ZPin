"""ZPin —— 截图 + 贴图工具。

托盘常驻；全局热键触发截图/贴图动作。App 是装配根：创建并接线各子系统
（截图 overlay、贴图窗口、截图历史、首选项、热键、托盘）后进入事件循环。
"""
from __future__ import annotations

import logging
import os
import subprocess
import sys
import time
import traceback
from types import TracebackType

os.environ.setdefault("QT_AUTO_SCREEN_SCALE_FACTOR", "1")

from PySide6.QtCore import QObject, QPointF, QRectF, Signal
from PySide6.QtGui import QFont, QImage
from PySide6.QtWidgets import QApplication, QInputDialog, QMessageBox

import app_icon
import config
import defaults
import hotkey
import history
import memtrim
import output
import overlay
import pins
import prefs
import startup
from tray import TrayController

log = logging.getLogger("zpin")


def _setup_logging() -> None:
    """初始化配置并按配置建日志（文件 + stderr，详细级别可配）。"""
    config.init()
    level = logging.DEBUG if config.get("General/log_level") == "详细" else logging.INFO
    logging.basicConfig(
        level=level,
        format="%(asctime)s.%(msecs)03d [%(levelname)s] %(name)s: %(message)s",
        datefmt="%H:%M:%S",
        handlers=[
            logging.FileHandler(os.path.join(config.app_dir(), "zpin.log"), encoding="utf-8"),
            logging.StreamHandler(sys.stderr),
        ],
    )


def _restart_app() -> None:
    """以原命令行重启进程（打包态直接重启 exe），当前实例退出。"""
    if getattr(sys, "frozen", False):
        cmd = [sys.executable]
    else:
        cmd = [sys.executable, os.path.abspath(sys.argv[0])]
    subprocess.Popen(cmd, cwd=os.path.dirname(os.path.abspath(sys.argv[0])), close_fds=True)
    QApplication.quit()


def _install_excepthook() -> None:
    """把任何未捕获异常（含 Qt 事件回调里抛出的）也写进 zpin.log，便于排查闪退。"""

    def _hook(etype: type[BaseException], exc: BaseException,
              tb: TracebackType | None) -> None:
        try:
            log.error("未捕获异常:\n%s", "".join(traceback.format_exception(etype, exc, tb)))
        finally:
            sys.__excepthook__(etype, exc, tb)

    sys.excepthook = _hook


class _AppSignals(QObject):
    """应用级跨线程信号。"""

    hotkey = Signal(str)   # 从热键线程跨线程投递动作
    show = Signal()        # 第二实例请求弹气泡（回到主线程再调 Qt）
    notify = Signal(str, str)   # 后台保存线程经此回主线程弹气泡


class App:
    """应用装配根：创建各子系统、接线信号并进入事件循环。

    原 main() 内的闭包群等价迁移为方法；各子系统成为实例属性，便于阅读与测试。

    Args:
        argv: 命令行参数，原样交给 QApplication。
    """

    def __init__(self, argv: list[str]) -> None:
        self.app = QApplication(argv)
        self.app.setApplicationName("ZPin")
        self.app.setWindowIcon(app_icon.app_icon())
        self.app.setQuitOnLastWindowClosed(False)
        name, size = defaults.font_tuple(config.get("Interface/font"))
        self.app.setFont(QFont(name, size))

        self.sig = _AppSignals()
        self.accel_map = {a: config.hotkey_accel(a) for a in hotkey.ACTION_IDS}
        self.tray = TrayController(self.accel_map)
        self.selector = overlay.SelectionController()
        self.pin_mgr = pins.PinManager()
        self.hist = history.HistoryStore()
        self.hist.set_limits()
        self.hist.changed.connect(self.tray.set_history)
        self.manager: hotkey.HotkeyManager | None = None
        self.callbacks: dict[str, object] = {}
        self._prefs_dialog: list = []

    # ---- 截图入口 ----

    def start_full_capture(self) -> None:
        """截图（全屏直截）：不进选区界面，立即抓全屏并复制到剪贴板。"""
        memtrim.touch()
        try:
            import capture as _cap
            if self.selector.is_active():
                # 框选界面开着时按全屏热键：先收起遮罩再抓，否则会把半透明遮罩
                # 和选框一起抓进图里（cancel 内部会 close 掉全部覆盖窗）。
                self.selector.cancel()
            bounds = _cap.virtual_bounds()
            img, mp = _cap.grab_desktop()
            if img.isNull():
                self.tray.notify("截图失败", "无法抓取屏幕画面")
                return
            self.on_captured(img, "copy", QPointF(bounds.left(), bounds.top()),
                             mp.dpr_of(QRectF(bounds)))
        except Exception:
            log.exception("全屏截图失败")
            self.tray.notify("截图失败", "全屏截图发生异常，详见日志")

    def start_capture(self, mode: str) -> None:
        """热键入口：full 走全屏直截，其余进入框选遮罩。"""
        if mode == "full":
            self.start_full_capture()
            return
        memtrim.touch()
        try:
            self.selector.start(mode)
        except Exception:
            log.exception("启动截图失败")
            self.tray.notify("截图失败", "启动截图时发生异常，详见日志")

    def on_captured(self, img: QImage, action: str, top_left: QPointF, dpr: float) -> None:
        """截图产物分发：入历史 ->（可选自动保存）-> 按动作保存/贴图/复制。

        Args:
            img: 抓取的物理像素画布。
            action: "copy" / "save" / "pin"。
            top_left: 贴图落点（全局逻辑坐标）。
            dpr: 本次截图所用屏幕的缩放比（贴图 1:1 复原用）。
        """
        self.hist.add(img, dpr)
        history.compress_pending()   # 原图即刻交工作线程压 PNG，不等空闲（削内存峰值）
        if config.get("Output/auto_save"):
            output.save_image_async(img, directory=output.auto_save_dir())
        if action == "save":
            # 手动保存一律弹「另存为」，不静默存默认地址；写盘挪后台线程防卡顿
            output.save_image_dialog_async(img, on_done=self._notify_saved)
            return
        if action == "pin":
            # ref_dpr = 这次截图实际用的比例：贴图与截图所见严格 1:1。
            # 混合 DPI 下落点屏比例 ≠ 该比例时，若按落点屏算会把图放大/缩小
            # dpr 倍——此前"贴出来比截图大"的根因。
            self.pin_mgr.pin_image(img, top_left, ref_dpr=dpr)
            return
        # 用 setImage(QImage) 而非 setPixmap(QPixmap)：QImage 是纯内存数据，
        # 不持有 HBITMAP 原生句柄，可避免 Windows 在别处读取剪贴板
        # （WM_RENDERFORMAT）时因句柄失效而崩溃/退出。
        QApplication.clipboard().setImage(img)
        self.tray.notify("ZPin", "截图已复制到剪贴板")

    # ---- 贴图组 / 历史（托盘菜单回调） ----

    def _toggle_pins(self) -> None:
        hidden = self.pin_mgr.toggle_visible()
        self.tray.set_pins_hidden(hidden)
        if self.pin_mgr.all_windows():
            self.tray.notify("贴图", "已全部隐藏（可从此菜单显示）" if hidden else "已全部显示")

    def _pin_clipboard(self) -> None:
        """托盘入口：把剪贴板里的图片贴到屏幕左上安全落点。"""
        img = QApplication.clipboard().image()
        if img.isNull():
            self.tray.notify("贴图", "剪贴板里没有图片")
            return
        self.pin_mgr.pin_image(img, pins.clip_pivot())

    def _switch_group(self) -> None:
        name = self.pin_mgr.switch_group()
        self.tray.notify("贴图组", f"已切换到「{name}」")

    def _sync_pin_menu(self) -> None:
        self.tray.set_groups([
            (g.name, len(g.windows), i == self.pin_mgr.groups.index(self.pin_mgr.current))
            for i, g in enumerate(self.pin_mgr.groups)
        ])
        total = len(self.pin_mgr.all_windows())
        self.tray.set_status(f"贴图 {total} 张 · 组 {len(self.pin_mgr.groups)}")

    def _new_group(self) -> None:
        name, ok = QInputDialog.getText(None, "新建贴图组", "组名：")
        if ok and not self.pin_mgr.add_group(name):
            self.tray.notify("贴图组", "组名无效或已存在")

    def _rename_group(self) -> None:
        old = self.pin_mgr.current.name
        name, ok = QInputDialog.getText(None, "重命名贴图组", "组名：", text=old)
        if ok and not self.pin_mgr.rename_group(old, name):
            self.tray.notify("贴图组", "组名无效或已存在")

    def _remove_group(self) -> None:
        name = self.pin_mgr.current.name
        if not self.pin_mgr.remove_group(name):
            self.tray.notify("贴图组", "至少要保留一个组")
            return
        self.tray.notify("贴图组", f"已删除「{name}」")

    def _clear_history(self) -> None:
        self.hist.clear()
        self.tray.notify("历史", "已清空截图历史")

    def _repin_from_history(self, idx: int) -> None:
        img = self.hist.get(idx)
        if not img.isNull():
            ref = self.hist.dpr_of(idx)   # 每条历史记住自己截图时用的比例
            pivot = pins.clip_pivot()
            # 物理像素 -> 逻辑坐标需除以 ref，避免 HiDPI 下贴到中心偏右下
            pos = QPointF(pivot.x() - img.width() / 2 / ref,
                          pivot.y() - img.height() / 2 / ref)
            self.pin_mgr.pin_image(img, pos, ref_dpr=ref)

    def _copy_from_history(self, idx: int) -> None:
        """从历史重新复制（剪贴板被覆盖后可随时取回）。"""
        img = self.hist.get(idx)
        if not img.isNull():
            QApplication.clipboard().setImage(img)
            self.tray.notify("截图历史", "已复制到剪贴板")

    def _notify_saved(self, path: str | None) -> None:
        """后台保存结果回调（子线程调用），经信号回主线程弹气泡。

        Args:
            path: 成功为完整路径；失败为空串；用户取消为 None。
        """
        if path is None:
            self.sig.notify.emit("保存已取消", "")
        else:
            self.sig.notify.emit("已保存" if path else "保存失败", path)

    def _save_from_history(self, idx: int) -> None:
        img = self.hist.get(idx)
        if not img.isNull():
            output.save_image_dialog_async(img, on_done=self._notify_saved)

    def _show_help(self) -> None:
        """弹出帮助（键位实时读配置，改键后同步显示新键）。"""

        def k(act: str) -> str:
            # 打开帮助时实时读取当前配置键位（改过键后帮助同步显示新键）
            return config.hotkey_accel(act) or "（未绑定）"

        help_lines = [
            "ZPin —— 截图 · 标注 · 贴图",
            "",
            "■ 快捷键（可到 设置 → 快捷键 修改）",
            f"全屏截图          {k('capture_full')}   一键截取整个屏幕",
            f"框选截图          {k('capture')}   自由框选 / 单击吸附窗口或界面元素",
            f"隐藏/显示全部贴图 {k('toggle_pins')}",
            f"切换到下一组      {k('switch_group')}",
            "",
            "■ 框选截图技巧",
            "光标自动吸附：优先界面元素（按钮、文字块…），元素过大则回退整个窗口；",
            "未框选时 Enter / 双击 = 直接截取当前吸附到的元素或窗口（单击只先吸附选中）；",
            "悬停有放大镜：像素网格 + 坐标色值；拖动边角自动出现，Alt 随时召唤/收起；",
            "按 C = 复制光标处颜色（#RRGGBB），画面顶部弹提示，悬停/框选后随时可用；",
            "按住拖拽 = 自由框选，边和角会吸到屏幕与其它窗口的边、中线（有红色参考线）；",
            "选区外点/拖 = 自动扩选；框内拖动 = 整体移动（已画标注跟着框走）；角/边手柄 = 调整大小；",
            "选了标注工具后框内变画图：点工具栏「移动」按钮即可恢复框内拖动；",
            "方向键 = 逐像素微调（Shift 一次 10px）；Ctrl+方向 = 沿该侧扩选；",
            "Tab（已框选）= 循环手柄后用方向键改大小；",
            "Space = 自由框选/吸附切换；Tab（未框选）= 轮换检测层级 自动→仅窗口→仅元素；",
            "Enter 复制 · Ctrl+S 另存为 · Esc 取消；所有截图都会进「截图历史」可再取。",
            "",
            "■ 标注",
            "工具：矩形 / 椭圆 / 直线 / 箭头 / 画笔 / 文本 / 马赛克，"
            "「⋮」里还有 荧光笔 / 橡皮擦 / 气泡标注；",
            "不选工具时点已画图形 = 选中：可拖动、拖端点改形状、换颜色或粗细，Del 删除；",
            "文本与气泡的字号跟随「粗细」档位；一次橡皮涂抹算一步撤销（Ctrl+Z）；",
            "文字输入框：Enter 换行 · Ctrl+Enter 或点击外部 完成 · Esc 取消。",
            "",
            "■ 贴图操作",
            "左键拖动 = 移动；滚轮 = 以光标为中心缩放；",
            "右键菜单 = 不透明度/旋转/翻转/灰度/边框/阴影/贴图组等全部操作；",
            "贴图窗快捷键：Ctrl+C 复制 · Ctrl+S 另存为 · Ctrl+0 实际大小 · Ctrl+R 重置",
            "（还原缩放/旋转/灰度）· Ctrl+T 旋转 · Esc 隐藏 · Del 销毁 · 双击隐藏。",
            "托盘「贴图管理 → 贴剪贴板图片」：复制任意图片后直接贴上屏幕。",
        ]
        QMessageBox.information(None, "ZPin 帮助", "\n".join(help_lines))

    # ---- 热键 ----

    def _setup_hotkeys(self) -> None:
        """创建热键管理器并按初始 accel_map 注册；注册失败弹气泡提示。"""
        self.manager = hotkey.HotkeyManager()
        self.callbacks = {
            action: (lambda act: lambda: self.sig.hotkey.emit(act))(action)
            for action in hotkey.ACTION_IDS
        }
        results = self.manager.apply_bindings(self.accel_map, self.callbacks)
        failed_labels = [hotkey.ACTION_LABELS[a] for a, ok in results.items() if not ok]
        if failed_labels:
            self.tray.notify("热键注册失败",
                             "、".join(failed_labels) + " 被占用，可在 设置 → 快捷键 中更改")

    def dispatch(self, action: str) -> None:
        """热键动作 -> 具体入口（未识别的动作忽略）。"""
        labels = {
            "capture_full": lambda: self.start_capture("full"),
            "capture": lambda: self.start_capture("normal"),
            "toggle_pins": self.tray.toggle_pins_requested.emit,
            "switch_group": self.tray.switch_group_requested.emit,
        }
        handler = labels.get(action)
        if handler:
            handler()

    def set_shortcuts_disabled(self, disabled: bool) -> None:
        """托盘/首选项共用的热键总开关：反注册 + 持久化 + 托盘菜单文字同步。"""
        if self.manager is None:
            return
        self.manager.set_disabled(disabled)
        config.set("Hotkeys/disabled", bool(disabled))
        config.sync()
        self.tray.set_shortcuts_disabled(bool(disabled))

    # ---- 首选项回调 ----

    def apply_hotkeys(self) -> dict[str, bool]:
        """按当前配置全量重绑热键（含托盘键位提示与帮助弹窗同步）。"""
        assert self.manager is not None, "wire() 之后才可用"
        new_map = {a: config.hotkey_accel(a) for a in hotkey.ACTION_IDS}
        # 托盘菜单右侧的快捷键提示、帮助弹窗随之显示新键
        self.tray.set_accels(new_map)
        return self.manager.apply_bindings(new_map, self.callbacks)

    def apply_font(self) -> None:
        """把配置里的字体应用到全局。"""
        name, size = defaults.font_tuple(config.get("Interface/font"))
        self.app.setFont(QFont(name, size))

    def apply_log_level(self) -> None:
        """把配置里的日志级别应用到根 logger。"""
        level = logging.DEBUG if config.get("General/log_level") == "详细" else logging.INFO
        logging.getLogger().setLevel(level)

    def open_prefs(self) -> None:
        """打开首选项对话框（已开则前置激活，单实例复用）。"""
        if self._prefs_dialog and self._prefs_dialog[0].isVisible():
            self._prefs_dialog[0].raise_()
            self._prefs_dialog[0].activateWindow()
            return
        self._prefs_dialog.clear()
        self._prefs_dialog.append(prefs.PreferencesDialog(
            self.apply_hotkeys, self.apply_font, self.apply_log_level,
            self.set_shortcuts_disabled))
        self._prefs_dialog[0].show()

    # ---- 接线与运行 ----

    def wire(self) -> None:
        """全部信号接线与启动态恢复（顺序与拆分前的 main() 一致）。"""
        self.pin_mgr.notify.connect(self.tray.notify)
        self.pin_mgr.groups_changed.connect(lambda _names, cur: self._sync_pin_menu())
        self._sync_pin_menu()

        self.selector.captured.connect(self.on_captured)
        self.tray.capture_requested.connect(self.start_capture)
        self.tray.toggle_pins_requested.connect(self._toggle_pins)
        self.tray.pin_clipboard_requested.connect(self._pin_clipboard)
        self.tray.switch_group_requested.connect(self._switch_group)
        self.tray.clear_history_requested.connect(self._clear_history)
        self.tray.history_selected.connect(self._repin_from_history)
        self.tray.history_copy_requested.connect(self._copy_from_history)
        self.tray.history_save_requested.connect(self._save_from_history)
        self.tray.group_selected.connect(self.pin_mgr.switch_to)
        self.tray.group_create_requested.connect(lambda: self._new_group())
        self.tray.group_rename_requested.connect(lambda: self._rename_group())
        self.tray.group_remove_requested.connect(lambda: self._remove_group())
        self.tray.help_requested.connect(self._show_help)
        self.tray.restart_requested.connect(_restart_app)

        self._setup_hotkeys()
        self.sig.hotkey.connect(memtrim.touch)
        self.sig.hotkey.connect(self.dispatch)
        # show_cb 在热键线程里被回调，必须回主线程再碰 Qt
        self.sig.show.connect(lambda: self.tray.notify("ZPin", "ZPin 已在运行"))
        hotkey.get_window().show_cb = self.sig.show.emit
        # 后台保存线程经此把结果气泡送回主线程
        self.sig.notify.connect(self.tray.notify)

        self.tray.shortcuts_disabled_toggled.connect(self.set_shortcuts_disabled)
        # 启动恢复上次的禁用状态：set_disabled 反注册上面刚注册的热键并同步托盘文字
        if config.get("Hotkeys/disabled"):
            self.set_shortcuts_disabled(True)

        self.tray.prefs_requested.connect(self.open_prefs)

    def run(self) -> int:
        """空闲整理接线后进入事件循环；返回 QApplication.exec() 退出码。"""
        # 空闲整理：先把历史里的原图压成 PNG，再裁工作集（见 memtrim._check）
        memtrim.register_cleaner(history.compress_pending)
        if config.get("General/keep_responsive"):
            memtrim.install()
        log.info(
            "ZPin 启动：热键=%s，自启=%s",
            {a: t for a, t in self.accel_map.items() if t},
            startup.is_enabled(),
        )
        return self.app.exec()


def main() -> int:
    """进程入口：日志、单实例守卫、装配 App 并进入事件循环。"""
    t0 = time.perf_counter()
    _setup_logging()
    _install_excepthook()
    log.info("启动[1/3] 日志与配置就绪 %.0fms", (time.perf_counter() - t0) * 1000)

    # 单实例：已运行则让其实例弹气泡，本进程退出
    if not hotkey.acquire_single_instance():
        hotkey.notify_show()
        return 0

    app = App(sys.argv)
    log.info("启动[2/3] 子系统装配完成 %.0fms", (time.perf_counter() - t0) * 1000)
    app.wire()
    log.info("启动[3/3] 信号接线完成 %.0fms", (time.perf_counter() - t0) * 1000)
    return app.run()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SystemExit:
        raise
    except Exception:
        # 窗口进程崩溃时 PyInstaller 只弹对话框，落一份日志才好排查
        logging.getLogger("zpin").exception("未处理的异常")
        raise
