"""全局热键 —— 隐藏消息窗口接收 WM_HOTKEY + 多热键注册表。

RegisterHotKey 必须在窗口所属线程内调用（跨线程注册会得到误导性的错误 1408），
因此注册动作通过 WM_APP 消息投递到窗口线程执行。
WM_HOTKEY 是线程消息（msg.hwnd == NULL），DispatchMessageW 不进窗口过程，
必须在消息循环里直接按 wParam（热键 id）分发。
"""
from __future__ import annotations

import logging
import threading

import ctypes
from ctypes import wintypes

log = logging.getLogger("zpin.hotkey")

WM_HOTKEY = 0x0312
HWND_MESSAGE = -3

MOD_ALT = 0x0001
MOD_CONTROL = 0x0002
MOD_SHIFT = 0x0004
MOD_WIN = 0x0008

WM_APP_REGISTER_HOTKEY = 0x8000 + 1  # 投递到窗口线程执行注册
WM_APP_UNREGISTER_HOTKEY = 0x8000 + 2
WM_APP_SHOW = 0x8000 + 3             # 第二实例请求第一实例弹气泡

ERROR_ALREADY_EXISTS = 183

_mutex = None


def acquire_single_instance() -> bool:
    """创建并持有进程级命名互斥体，用于单实例保护。

    Returns:
        bool: False 表示已有实例在运行（互斥体已存在）。
    """
    global _mutex
    _mutex = kernel32.CreateMutexW(None, False, "ZPin.SingleInstance")
    if not _mutex:
        # 连互斥体都拿不到（极端环境）：按“已有实例”退出，避免双实例并存
        log.error("CreateMutexW 失败: %s", ctypes.get_last_error())
        return False
    return ctypes.get_last_error() != ERROR_ALREADY_EXISTS


def notify_show() -> bool:
    """通知已运行的实例（第二实例启动时调用）。

    Returns:
        bool: 是否找到主实例窗口并成功投递了 WM_APP_SHOW 消息。
    """
    hwnd = user32.FindWindowExW(HWND_MESSAGE, None, "ZPinHost", None)
    if not hwnd:
        return False
    user32.PostMessageW(hwnd, WM_APP_SHOW, 0, 0)
    return True


user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

user32.FindWindowExW.argtypes = [wintypes.HWND, wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR]
user32.FindWindowExW.restype = wintypes.HWND
kernel32.CreateMutexW.argtypes = [wintypes.LPVOID, wintypes.BOOL, wintypes.LPCWSTR]
kernel32.CreateMutexW.restype = wintypes.HANDLE
# hInstance 是 64 位句柄：不声明 restype 会被 ctypes 当成 C int 截断高位
kernel32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
kernel32.GetModuleHandleW.restype = wintypes.HINSTANCE

WNDPROC = ctypes.WINFUNCTYPE(
    ctypes.c_ssize_t, wintypes.HWND, ctypes.c_uint, wintypes.WPARAM, wintypes.LPARAM
)


class WNDCLASSW(ctypes.Structure):
    """RegisterClassW 所需的窗口类描述结构体。"""
    _fields_ = [
        ("style", ctypes.c_uint),
        ("lpfnWndProc", WNDPROC),
        ("cbClsExtra", ctypes.c_int),
        ("cbWndExtra", ctypes.c_int),
        ("hInstance", wintypes.HINSTANCE),
        ("hIcon", wintypes.HICON),
        ("hCursor", wintypes.HANDLE),
        ("hbrBackground", wintypes.HBRUSH),
        ("lpszMenuName", wintypes.LPCWSTR),
        ("lpszClassName", wintypes.LPCWSTR),
    ]


user32.RegisterClassW.argtypes = [ctypes.POINTER(WNDCLASSW)]
user32.CreateWindowExW.argtypes = [
    wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD,
    ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
    wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, wintypes.LPVOID,
]
user32.CreateWindowExW.restype = wintypes.HWND
user32.DefWindowProcW.argtypes = [wintypes.HWND, ctypes.c_uint, wintypes.WPARAM, wintypes.LPARAM]
user32.DefWindowProcW.restype = ctypes.c_ssize_t
user32.GetMessageW.argtypes = [
    ctypes.POINTER(wintypes.MSG), wintypes.HWND, ctypes.c_uint, ctypes.c_uint
]
user32.RegisterHotKey.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_uint, ctypes.c_uint]
user32.UnregisterHotKey.argtypes = [wintypes.HWND, ctypes.c_int]
user32.PostMessageW.argtypes = [wintypes.HWND, ctypes.c_uint, wintypes.WPARAM, wintypes.LPARAM]


# ---- 加速键文本 <-> (modifiers, VK) ----

_VK_F = 0x70  # F1
_OEM: dict[str, int] = {
    ";": 0xBA, "=": 0xBB, ",": 0xBC, "-": 0xBD, ".": 0xBE, "/": 0xBF,
    "`": 0xC0, "[": 0xDB, "\\": 0xDC, "]": 0xDD, "'": 0xDE,
}
_NAMED: dict[str, int] = {
    "PRINTSCREEN": 0x2C, "SCROLLLOCK": 0x91, "PAUSE": 0x13,
    "INS": 0x2D, "DEL": 0x2E, "HOME": 0x24, "END": 0x23,
    "PGUP": 0x21, "PGDN": 0x22, "SPACE": 0x20, "TAB": 0x09,
}
_MOD_TOKENS = {
    "CTRL": MOD_CONTROL, "CONTROL": MOD_CONTROL,
    "SHIFT": MOD_SHIFT, "ALT": MOD_ALT, "WIN": MOD_WIN, "META": MOD_WIN,
}
_DISPLAY = {
    0x2C: "PrintScreen", 0x91: "ScrollLock", 0x13: "Pause",
    0x2D: "Ins", 0x2E: "Del", 0x24: "Home", 0x23: "End",
    0x21: "PgUp", 0x22: "PgDn", 0x20: "Space", 0x09: "Tab",
}


def parse_accel(text: str) -> tuple[int, int] | None:
    """解析 "Ctrl+Shift+F1" / "Alt+L" 形式的加速键，非法返回 None。

    Args:
        text: 加速键文本，修饰键与主键用 "+" 连接。

    Returns:
        tuple[int, int] | None: (modifiers, VK) 二元组；文本为空或非法时返回 None。
    """
    if not text or not text.strip():
        return None
    mods = 0
    key = ""
    for part in text.replace(" ", "").split("+"):
        upper = part.upper()
        if upper in _MOD_TOKENS and key == "":
            mods |= _MOD_TOKENS[upper]
        else:
            if key:
                return None
            key = upper
    if not key:
        return None
    if key[0] == "F" and len(key) > 1 and key[1:].isdigit():
        n = int(key[1:])
        if 1 <= n <= 24:
            return mods, _VK_F + n - 1
    if len(key) == 1 and key.isdigit():
        return mods, ord(key)
    if len(key) == 1 and "A" <= key <= "Z":
        return mods, ord(key)
    if key in _NAMED:
        return mods, _NAMED[key]
    if key in _OEM:
        return mods, _OEM[key]
    return None


def format_accel(mods: int, vk: int) -> str:
    """把 (modifiers, VK) 组合格式化回 "Ctrl+Shift+F1" 形式的显示文本。

    Args:
        mods: MOD_* 修饰键位掩码。
        vk: Win32 虚拟键码。

    Returns:
        str: 修饰键与主键用 "+" 连接的文本；主键无法识别时只含修饰键部分。
    """
    parts = []
    if mods & MOD_CONTROL:
        parts.append("Ctrl")
    if mods & MOD_SHIFT:
        parts.append("Shift")
    if mods & MOD_ALT:
        parts.append("Alt")
    if mods & MOD_WIN:
        parts.append("Win")
    if 0x70 <= vk <= 0x87:
        parts.append(f"F{vk - 0x70 + 1}")
    elif 0x30 <= vk <= 0x39 or 0x41 <= vk <= 0x5A:
        parts.append(chr(vk))
    elif vk in _DISPLAY:
        parts.append(_DISPLAY[vk])
    else:
        for ch, code in _OEM.items():
            if code == vk:
                parts.append(ch)
                break
    return "+".join(parts)


class HotkeyWindow:
    """隐藏的消息窗口：接收全局热键 WM_HOTKEY。"""

    def __init__(self) -> None:
        """启动消息窗口线程并等待其就绪。

        Raises:
            RuntimeError: 窗口线程 5 秒内未就绪。
        """
        self.hwnd: int | None = None
        self.hotkey_cb = None  # def (hotkey_id: int)，在窗口线程回调，调用方自行切线程
        self.show_cb = None    # 第二实例请求时回调
        self._ready = threading.Event()
        self._wndproc_ref = WNDPROC(self._wndproc)
        self._op_lock = threading.Lock()
        self._op_done = threading.Event()
        self._op_ok = False
        self._op_err = 0
        self._op_id = 0
        self._op_mod = 0
        self._op_vk = 0
        self._thread = threading.Thread(target=self._run, name="ZPinHotkey", daemon=True)
        self._thread.start()
        if not self._ready.wait(5):
            raise RuntimeError("热键窗口线程启动失败")

    def _run(self) -> None:
        """窗口线程主体：注册窗口类、创建消息窗口并循环分发消息。"""
        wc = WNDCLASSW()
        wc.lpfnWndProc = self._wndproc_ref
        wc.lpszClassName = "ZPinHost"
        wc.hInstance = kernel32.GetModuleHandleW(None)
        if not user32.RegisterClassW(ctypes.byref(wc)):
            log.error("RegisterClassW 失败: %s", ctypes.get_last_error())
            self._ready.set()
            return
        self.hwnd = user32.CreateWindowExW(
            0, wc.lpszClassName, wc.lpszClassName, 0, 0, 0, 0, 0,
            HWND_MESSAGE, None, wc.hInstance, None,
        )
        self._ready.set()
        if not self.hwnd:
            log.error("CreateWindowExW 失败: %s", ctypes.get_last_error())
            return
        msg = wintypes.MSG()
        while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            if msg.message == WM_HOTKEY:
                self._fire(int(msg.wParam))
                continue
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))

    def _fire(self, hotkey_id: int) -> None:
        if self.hotkey_cb is not None:
            try:
                self.hotkey_cb(hotkey_id)
            except Exception:
                log.exception("热键回调失败")

    def _wndproc(self, hwnd: int, msg: int, wparam: int, lparam: int) -> int:
        """窗口过程：在窗口线程内执行注册/反注册请求并响应唤起消息。"""
        if msg == WM_APP_REGISTER_HOTKEY:
            ok = bool(user32.RegisterHotKey(self.hwnd, self._op_id, self._op_mod, self._op_vk))
            self._op_ok = ok
            self._op_err = 0 if ok else ctypes.get_last_error()
            self._op_done.set()
            return 0
        if msg == WM_APP_UNREGISTER_HOTKEY:
            ok = bool(user32.UnregisterHotKey(self.hwnd, self._op_id))
            self._op_ok = ok
            self._op_err = 0 if ok else ctypes.get_last_error()
            self._op_done.set()
            return 0
        if msg == WM_APP_SHOW:
            if self.show_cb is not None:
                try:
                    self.show_cb()
                except Exception:
                    log.exception("唤起回调失败")
            return 0
        return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

    def _submit(self, register: bool, hotkey_id: int,
                mods: int = 0, vk: int = 0) -> tuple[bool, int]:
        """把注册/反注册请求投递到窗口线程，同步等待执行结果。

        Args:
            register: True 为注册，False 为反注册。
            hotkey_id: 热键 id。
            mods: 修饰键掩码（仅注册时使用）。
            vk: 虚拟键码（仅注册时使用）。

        Returns:
            tuple[bool, int]: (是否成功, 错误码)；成功为 0，未执行或超时为 -1，
            失败为 GetLastError 的值。
        """
        if not self.hwnd:
            # 窗口线程创建失败：消息会投进调用线程队列无人消费，必然白等 3 秒
            return False, -1
        # 注册/反注册必须在窗口线程内执行，用锁串行化
        with self._op_lock:
            self._op_id = hotkey_id
            self._op_mod = mods
            self._op_vk = vk
            self._op_done.clear()
            wm = WM_APP_REGISTER_HOTKEY if register else WM_APP_UNREGISTER_HOTKEY
            user32.PostMessageW(self.hwnd, wm, 0, 0)
            if not self._op_done.wait(3):
                log.error("热键操作请求超时（窗口线程无响应）")
                return False, -1
        return self._op_ok, self._op_err


_WINDOW: HotkeyWindow | None = None
_WINDOW_LOCK = threading.Lock()


def get_window() -> HotkeyWindow:
    """返回进程级唯一的 HotkeyWindow，首次调用时懒创建。"""
    global _WINDOW
    with _WINDOW_LOCK:
        if _WINDOW is None:
            _WINDOW = HotkeyWindow()
        return _WINDOW


# ---- 多热键注册表 ----

ACTION_IDS: dict[str, int] = {
    "capture_full": 0xA001,   # 截图（一键全屏）
    "capture": 0xA003,        # 框选截图
    "toggle_pins": 0xA005,
    "switch_group": 0xA006,
}
_ID_TO_ACTION = {v: k for k, v in ACTION_IDS.items()}

ACTION_LABELS: dict[str, str] = {
    "capture_full": "截图",
    "capture": "框选截图",
    "toggle_pins": "隐藏/显示所有贴图",
    "switch_group": "切换到另一贴图组",
}


class HotkeyManager:
    """action -> 加速键 的注册/换绑/总开关。回调在窗口线程触发，经调用方 Signal 回主线程。"""

    def __init__(self) -> None:
        self._win = get_window()
        self._callbacks: dict[str, object] = {}
        self._accels: dict[str, tuple[int, int]] = {}
        self._disabled = False
        # 关键接线：窗口线程的 WM_HOTKEY 由 _fire 交给这里，否则热键注册成功也没人消费
        self._win.hotkey_cb = self._on_hotkey_id

    def _on_hotkey_id(self, hotkey_id: int) -> None:
        """在热键窗口线程被回调：id -> action -> 调用方回调（回调自行切回主线程）。"""
        action = _ID_TO_ACTION.get(hotkey_id)
        if not action:
            log.warning("收到未知热键 id=%s", hex(hotkey_id))
            return
        cb = self._callbacks.get(action)
        if cb is not None:
            cb()

    def apply_bindings(self, text_map: dict[str, str],
                       callbacks: dict[str, object]) -> dict[str, bool]:
        """按 text_map 全量重绑。

        先反注册当前已知的全部动作（含被清空、被改键的），再注册新表；
        否则「清除快捷键」只会被跳过、旧注册一直留着。

        Args:
            text_map: action -> 加速键文本（非法文本会被跳过）。
            callbacks: action -> 触发时调用的回调。

        Returns:
            dict[str, bool]: action -> 本次注册是否成功（只含尝试注册的动作）。
        """
        self._callbacks = callbacks
        results: dict[str, bool] = {}
        wanted: dict[str, tuple[int, int]] = {}
        for action, text in text_map.items():
            parsed = parse_accel(text)
            if parsed:
                wanted[action] = parsed
        if self._disabled:
            # 总开关停用中：只记账不注册，恢复时按新表注册（保持与托盘「已停用」一致）
            self._accels = wanted
            return results
        for action in set(self._accels) | set(wanted):
            self._win._submit(False, ACTION_IDS[action])
        for action, (mods, vk) in wanted.items():
            hid = ACTION_IDS[action]
            ok, err = self._win._submit(True, hid, mods, vk)
            if not ok:
                log.warning("热键 %s=%s 注册失败 err=%s", action, format_accel(mods, vk), err)
            results[action] = ok
        self._accels = {a: p for a, p in wanted.items() if results.get(a)}
        return results

    def set_disabled(self, disabled: bool) -> None:
        """总开关（托盘「禁用快捷键」）：反注册但保留绑定，恢复时按原 mods/vk 重注册。

        注意：注册必须带 mods/vk。RegisterHotKey(hwnd, id, 0, 0) 会**成功**并占住该 id，
        把一个空热键顶替真实绑定，导致恢复后热键永久失效。

        Args:
            disabled: True 反注册全部已绑定热键；False 按保留的绑定重注册。
        """
        if disabled == self._disabled:
            return  # 幂等：重复注册同一组合键会得到 err 1409（已被注册）
        self._disabled = disabled
        for action, (mods, vk) in self._accels.items():
            if disabled:
                self._win._submit(False, ACTION_IDS[action])
            else:
                ok, err = self._win._submit(True, ACTION_IDS[action], mods, vk)
                if not ok:
                    log.warning("恢复热键 %s 失败 err=%s", action, err)
