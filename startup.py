"""开机自启 —— HKCU Run 键（ZPin 不要求管理员，无需计划任务提权）。"""
from __future__ import annotations

import logging
import os
import sys

log = logging.getLogger("zpin.startup")

_RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
_VALUE = "ZPin"


def _target() -> str:
    if getattr(sys, "frozen", False):
        return f'"{sys.executable}"'
    pythonw = sys.executable.replace("python.exe", "pythonw.exe")
    exe = pythonw if os.path.isfile(pythonw) else sys.executable
    return f'"{exe}" "{os.path.abspath(sys.argv[0])}"'


def is_enabled() -> bool:
    """查询当前是否已注册开机自启。

    Returns:
        Run 键值与当前启动命令一致时为 True；读注册表失败或未注册返回 False。
    """
    try:
        import winreg

        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, _RUN_KEY, 0, winreg.KEY_READ) as k:
            val, _ = winreg.QueryValueEx(k, _VALUE)
            return os.path.normcase(val) == os.path.normcase(_target())
    except OSError:
        return False


def set_enabled(on: bool) -> None:
    """开启或关闭开机自启（写/删 HKCU Run 键值）。

    Args:
        on: True 注册自启，False 取消自启。
    """
    try:
        import winreg

        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER, _RUN_KEY, 0, winreg.KEY_SET_VALUE
        ) as k:
            if on:
                winreg.SetValueEx(k, _VALUE, 0, winreg.REG_SZ, _target())
            else:
                try:
                    winreg.DeleteValue(k, _VALUE)
                except FileNotFoundError:
                    pass
    except OSError:
        log.exception("设置开机自启失败")
