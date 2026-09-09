"""Win32 原生调用封装 —— 全项目唯一的通用 ctypes 边界。

热键消息窗口的绑定内聚在 hotkey.py（母本模式），其余模块要用 win32
一律走这里。所有跨模块指针/句柄参数必须写全 argtypes（64 位截断教训）。
"""
from __future__ import annotations

import ctypes
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

GWL_EXSTYLE = -20
GWL_STYLE = -16
WS_CHILD = 0x40000000
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_TRANSPARENT = 0x00000020
DWMWA_CLOAKED = 14
DWMWA_EXTENDED_FRAME_BOUNDS = 9

# 虚拟桌面物理包围盒（本进程是 PMv2，GetSystemMetrics 返回物理像素）
SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN = 76, 77
SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN = 78, 79

user32.GetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
user32.GetWindowLongPtrW.restype = ctypes.c_ssize_t
user32.SetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_ssize_t]
user32.SetWindowLongPtrW.restype = ctypes.c_ssize_t
user32.GetAsyncKeyState.argtypes = [ctypes.c_int]
user32.GetAsyncKeyState.restype = ctypes.c_short
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.IsWindowVisible.restype = wintypes.BOOL
user32.IsIconic.argtypes = [wintypes.HWND]
user32.IsIconic.restype = wintypes.BOOL
user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
user32.GetWindowRect.restype = wintypes.BOOL
user32.GetClassNameW.argtypes = [wintypes.HWND, ctypes.c_wchar_p, ctypes.c_int]
user32.GetClassNameW.restype = ctypes.c_int
user32.SetWindowPos.argtypes = [
    wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
    ctypes.c_int, ctypes.c_int, ctypes.c_uint,
]
user32.SetWindowPos.restype = wintypes.BOOL
kernel32.GetCurrentProcess.restype = ctypes.c_void_p  # 伪句柄 (HANDLE)-1，按默认 c_int 会截断
kernel32.SetProcessWorkingSetSize.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t]

user32.GetSystemMetrics.argtypes = [ctypes.c_int]
user32.GetSystemMetrics.restype = ctypes.c_int

_dwmapi = ctypes.WinDLL("dwmapi", use_last_error=True)
_dwmapi.DwmGetWindowAttribute.argtypes = [
    wintypes.HWND, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
]
_dwmapi.DwmGetWindowAttribute.restype = ctypes.c_long


def visible_frame_rect(hwnd: int) -> tuple[int, int, int, int] | None:
    """窗口真正可见的物理矩形（不含 Win10/11 的 DWM 不可见边框）。

    GetWindowRect 在本机实测比可见范围大 左8/上0/右8/下8，最大化窗口还含
    屏外 -8，直接用它做吸附会「每圈都多一点」。

    Args:
        hwnd: 目标顶层窗口句柄。

    Returns:
        tuple[int, int, int, int] | None: (left, top, right, bottom)；
        取不到 DWM 属性时返回 None，由调用方回退。
    """
    rc = wintypes.RECT()
    hr = _dwmapi.DwmGetWindowAttribute(
        hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, ctypes.byref(rc), ctypes.sizeof(rc))
    if hr != 0 or rc.right <= rc.left or rc.bottom <= rc.top:
        return None
    return rc.left, rc.top, rc.right, rc.bottom


def set_topmost(hwnd: int) -> bool:
    """把窗口设为 TOPMOST（不移动、不改变大小、不激活）。

    Args:
        hwnd: 目标窗口句柄。

    Returns:
        bool: SetWindowPos 是否成功。
    """
    HWND_TOPMOST = -1
    SWP_NOMOVE = 0x0002
    SWP_NOSIZE = 0x0001
    SWP_NOACTIVATE = 0x0010
    return bool(user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE))


def _is_cloaked(hwnd: int) -> bool:
    cloaked = wintypes.DWORD(0)
    _dwmapi.DwmGetWindowAttribute(
        hwnd, DWMWA_CLOAKED, ctypes.byref(cloaked), ctypes.sizeof(cloaked))
    return bool(cloaked.value)


def _virtual_desktop() -> tuple[int, int, int, int] | None:
    """物理像素下的虚拟桌面包围盒；取不到时返回 None（跳过裁剪）。"""
    w = user32.GetSystemMetrics(SM_CXVIRTUALSCREEN)
    h = user32.GetSystemMetrics(SM_CYVIRTUALSCREEN)
    if w < 1 or h < 1:
        return None
    l = user32.GetSystemMetrics(SM_XVIRTUALSCREEN)
    t = user32.GetSystemMetrics(SM_YVIRTUALSCREEN)
    return l, t, l + w, t + h


def visible_window_rects(exclude: set[int] | None = None) -> list[tuple[int, int, int, int, int]]:
    """按 z 序（最上在前）枚举可见顶层窗口的物理像素矩形 (hwnd, l, t, r, b)。

    矩形是「真正看得见」的范围：优先用 DWM 扩展边框，回退 GetWindowRect，
    再裁进虚拟桌面，保证吸附结果不会比窗口大一圈或跑到屏外。

    跳过：不可见/最小化/子窗口/工具窗/透明窗/UWP 隐藏(cloaked)窗，零尺寸窗。

    Args:
        exclude: 需要排除的窗口句柄集合，可为 None。

    Returns:
        list[tuple[int, int, int, int, int]]: (hwnd, left, top, right, bottom) 列表，
        按 z 序从最上到最下排列。
    """
    exclude = exclude or set()
    out: list[tuple[int, int, int, int, int]] = []
    desk = _virtual_desktop()

    def collect_rect(hwnd: int, rc: wintypes.RECT) -> None:
        """按可见性/样式/DWM 条件过滤单个窗口，合格则记入吸附矩形列表。"""
        if hwnd in exclude:
            return
        if not user32.IsWindowVisible(hwnd) or user32.IsIconic(hwnd):
            return
        # 跳过桌面壳窗口（Progman/WorkerW）：否则光标停在桌面空白处会"吸附整屏"
        buf = ctypes.create_unicode_buffer(64)
        if user32.GetClassNameW(hwnd, buf, 64) and buf.value in ("Progman", "WorkerW"):
            return
        style = user32.GetWindowLongPtrW(hwnd, GWL_STYLE)
        ex = user32.GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
        if style & WS_CHILD or ex & (WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT):
            return
        if _is_cloaked(hwnd):
            return
        rect = visible_frame_rect(hwnd)
        if rect is None:
            if not user32.GetWindowRect(hwnd, rc):
                return
            rect = (rc.left, rc.top, rc.right, rc.bottom)
        l, t, r, b = rect
        if desk:
            l, t = max(l, desk[0]), max(t, desk[1])
            r, b = min(r, desk[2]), min(b, desk[3])
        if r - l < 1 or b - t < 1:
            return
        out.append((hwnd, l, t, r, b))

    rc = wintypes.RECT()
    WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = [WNDENUMPROC, wintypes.LPARAM]
    user32.EnumWindows.restype = wintypes.BOOL

    def on_window(hwnd: int, _lp: int) -> bool:
        collect_rect(hwnd, rc)
        return True

    cb = WNDENUMPROC(on_window)
    user32.EnumWindows(cb, 0)
    return out


