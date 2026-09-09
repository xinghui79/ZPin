"""UI Automation 界面元素吸附 —— 纯 ctypes 直调 IUIAutomation，不引第三方依赖。

ElementFromPoint 是跨进程调用：本机实测多数 App ~3ms/次，个别实现的 provider
会卡几百毫秒。因此这里所有失败与异常一律降级成「无元素」，并在出现慢调用时
熔断一段时间，绝不让选区交互被别的进程拖住。

COM 槽位（照 Windows SDK UIAutomationClient.h 的实际声明顺序，别凭记忆改）：
  IUIAutomation        : 1 CompareElements 2 CompareRuntimeIds 3 GetRootElement
                         4 ElementFromHandle 5 ElementFromPoint
  IUIAutomationElement : 1 SetFocus 2 GetRuntimeId 3 FindFirst 4 FindAll
                         5 FindFirstBuildCache 6 FindAllBuildCache
                         7 BuildUpdatedCache 8 GetCurrentPropertyValue
IUnknown 占 0..2，故 vtable 下标 = 2 + 上面的序号。
"""
from __future__ import annotations

import ctypes
import logging
import time
from ctypes import (POINTER, Structure, WINFUNCTYPE, byref, c_double, c_long, c_ubyte,
                    c_uint, c_void_p, cast)

log = logging.getLogger("zpin.uia")

UIA_BoundingRectangle = 30001
UIA_ControlType = 30003
UIA_IsOffscreen = 30008
VT_I4, VT_BOOL, VT_R8, VT_ARRAY = 3, 11, 5, 0x2000

_SLOW_MS = 250.0        # 单次超过即熔断
_COOLDOWN_S = 30.0      # 熔断时长

ole32 = ctypes.WinDLL("ole32")
oleaut32 = ctypes.WinDLL("oleaut32")
oleaut32.SafeArrayAccessData.argtypes = [c_void_p, POINTER(c_void_p)]
oleaut32.SafeArrayAccessData.restype = c_long
oleaut32.SafeArrayUnaccessData.argtypes = [c_void_p]
oleaut32.SafeArrayUnaccessData.restype = c_long
oleaut32.VariantClear.argtypes = [c_void_p]
oleaut32.VariantClear.restype = c_long


class _GUID(Structure):
    _fields_ = [("l", ctypes.c_uint32), ("d1", ctypes.c_uint16), ("d2", ctypes.c_uint16),
                ("d3", c_ubyte * 8)]


class _POINT(Structure):
    _fields_ = [("x", c_long), ("y", c_long)]


class _VARIANT(Structure):
    """x64 VARIANT：vt 之后 6 字节保留，联合体从偏移 8 起、占 16 字节。"""
    _fields_ = [("vt", ctypes.c_ushort), ("reserved", c_ubyte * 6),
                ("llVal", ctypes.c_int64), ("extra", ctypes.c_int64)]


_CLSID = _GUID(0xFF48DBA4, 0x60EF, 0x4201,
               (c_ubyte * 8)(0xAA, 0x87, 0x54, 0x10, 0x3E, 0xEF, 0x59, 0x4E))
_IID = _GUID(0x30CBE57D, 0xD9D0, 0x452A,
             (c_ubyte * 8)(0xAB, 0x13, 0x7A, 0xC5, 0xAC, 0x48, 0x25, 0xEE))

CONTROL_NAMES = {
    50000: "按钮", 50002: "复选框", 50004: "组合框", 50007: "编辑框", 50008: "链接",
    50009: "图片", 50011: "列表", 50012: "列表项", 50013: "菜单", 50015: "菜单项",
    50018: "滚动条", 50020: "滑块", 50021: "数字调节", 50025: "选项卡", 50026: "选项卡页",
    50028: "文本", 50029: "工具栏", 50031: "文档", 50033: "窗口", 50034: "面板",
    50035: "标题", 50036: "标题项", 50037: "表格", 50038: "标题栏", 50040: "分组",
}


def _slot(obj: c_void_p, idx: int) -> int:
    return cast(obj, POINTER(POINTER(c_void_p))).contents[idx]


def _method(obj: c_void_p, n: int, restype: object, *argtypes: object) -> object:
    """IUnknown 之后第 n 个方法（1-based）。"""
    return WINFUNCTYPE(restype, c_void_p, *argtypes)(_slot(obj, 2 + n))


def _release(obj: c_void_p) -> None:
    WINFUNCTYPE(c_long, c_void_p)(_slot(obj, 2))(obj)


_uia: c_void_p | None = None
_dead = False
_cooldown_until = 0.0
_efp = None


def _init() -> bool:
    """惰性创建 CUIAutomation 实例并绑定 ElementFromPoint；失败后不再重试。"""
    global _uia, _dead, _efp
    if _uia is not None:
        return True
    if _dead:
        return False
    try:
        ole32.CoInitializeEx(None, 2)   # Qt 主线程通常已初始化；返回码无关，下面才是判据
        p = c_void_p()
        if ole32.CoCreateInstance(byref(_CLSID), None, 1, byref(_IID), byref(p)) or not p.value:
            raise OSError("CoCreateInstance(CUIAutomation) 失败")
        _uia = p
        _efp = _method(p, 5, c_long, _POINT, POINTER(c_void_p))   # ElementFromPoint
    except OSError:
        log.exception("UIA 初始化失败，界面元素吸附不可用")
        _dead = True
        return False
    log.info("UIA 客户端就绪")
    return True


def _property(el: c_void_p, prop_id: int) -> int | bool | list[float] | None:
    """读取元素的当前属性值；仅支持 I4/BOOL/R8 数组（矩形），其余返回 None。"""
    get_current = _method(el, 8, c_long, c_uint, POINTER(_VARIANT))  # GetCurrentPropertyValue
    v = _VARIANT()
    if get_current(el, prop_id, byref(v)):
        return None
    try:
        if v.vt == VT_I4:
            return ctypes.cast(byref(v, 8), POINTER(c_long)).contents.value
        if v.vt == VT_BOOL:
            return bool(ctypes.cast(byref(v, 8), POINTER(ctypes.c_short)).contents.value)
        if v.vt == (VT_R8 | VT_ARRAY):
            data = c_void_p()
            if oleaut32.SafeArrayAccessData(c_void_p(v.llVal), byref(data)):
                return None
            try:
                arr = cast(data, POINTER(c_double))
                return [arr[i] for i in range(4)]      # left, top, width, height
            finally:
                oleaut32.SafeArrayUnaccessData(c_void_p(v.llVal))
    finally:
        oleaut32.VariantClear(byref(v))
    return None


def element_at(px: float, py: float) -> tuple[int, int, int, int, str] | None:
    """物理坐标处的界面元素；无则 None。

    矩形是物理像素（本进程 Per-Monitor v2 DPI aware，UIA 不缩放）。

    Args:
        px: 物理像素 x 坐标。
        py: 物理像素 y 坐标。

    Returns:
        tuple[int, int, int, int, str] | None: (left, top, right, bottom,
        控件类型名)；无元素、调用失败或熔断冷却中返回 None。
    """
    global _cooldown_until
    if time.monotonic() < _cooldown_until or not _init():
        return None
    t0 = time.perf_counter()
    el = c_void_p()
    try:
        if _efp(_uia, _POINT(int(px), int(py)), byref(el)) or not el.value:
            return None
        try:
            if _property(el, UIA_IsOffscreen):
                return None
            rect = _property(el, UIA_BoundingRectangle)
            ctype = _property(el, UIA_ControlType)
        finally:
            _release(el)
    except Exception:
        log.exception("UIA 取元素失败，界面元素吸附暂停 %.0fs", _COOLDOWN_S)
        _cooldown_until = time.monotonic() + _COOLDOWN_S
        return None
    ms = (time.perf_counter() - t0) * 1000
    if ms > _SLOW_MS:
        log.warning("UIA provider 响应 %.0fms，界面元素吸附暂停 %.0fs", ms, _COOLDOWN_S)
        _cooldown_until = time.monotonic() + _COOLDOWN_S
        return None
    if not isinstance(rect, list) or len(rect) != 4:
        return None
    l, t, w, h = rect
    if w < 1 or h < 1:
        return None
    return int(l), int(t), int(l + w), int(t + h), CONTROL_NAMES.get(int(ctype or 0), "元素")
