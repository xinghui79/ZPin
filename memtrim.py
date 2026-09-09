"""闲置内存裁剪 —— 空闲约 1 分钟后执行注册的缓存回调，再把可换出页移出工作集。

SetProcessWorkingSetSize(-1, -1) 让系统把本进程可换出的页从工作集移出，
任务管理器内存读数骤降；下次访问软缺页自动回来，用户无感。
对应首选项「保持快速响应」开关。
"""
from __future__ import annotations

import ctypes
import logging
import time
from collections.abc import Callable

from PySide6.QtCore import QTimer

import winapi

log = logging.getLogger("zpin.memtrim")

_MAX_SIZET = (1 << (8 * 8)) - 1  # (SIZE_T)-1 = "清空工作集"
_IDLE_SECONDS = 60
_CHECK_MS = 15_000

_last_active = time.monotonic()
_timer: QTimer | None = None
_cleaners: list[Callable[[], None]] = []


def register_cleaner(fn: Callable[[], None]) -> None:
    """注册一个空闲时执行的缓存清理回调。

    Args:
        fn: 无参回调；执行抛出的异常会被捕获并记录日志，不影响其余回调。
    """
    _cleaners.append(fn)


def touch() -> None:
    """标记"刚刚有用户活动"，重新计算空闲时间。"""
    global _last_active
    _last_active = time.monotonic()


def trim_now() -> None:
    """立即执行全部清理回调，并把可换出页移出工作集。"""
    for fn in _cleaners:
        try:
            fn()
        except Exception:
            log.exception("空闲清理回调失败")
    ok = winapi.kernel32.SetProcessWorkingSetSize(
        winapi.kernel32.GetCurrentProcess(), _MAX_SIZET, _MAX_SIZET)
    if ok:
        log.info("空闲超时，已清理缓存并裁剪工作集")
    else:
        log.warning("SetProcessWorkingSetSize 失败 err=%s", ctypes.get_last_error())


def _check() -> None:
    if time.monotonic() - _last_active >= _IDLE_SECONDS:
        trim_now()
        touch()


def install() -> None:
    """启动空闲检测定时器（约 15s 检查一次）；重复调用不会重复安装。"""
    global _timer
    if _timer is not None:
        return
    _timer = QTimer()
    _timer.setInterval(_CHECK_MS)
    _timer.timeout.connect(_check)
    _timer.start()
