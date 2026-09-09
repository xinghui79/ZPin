"""多屏截图合成 —— 物理画布 + 分段的逻辑<->物理坐标映射。

坐标约定：
  逻辑坐标   Qt 全局逻辑坐标，UI（选区/命中/事件）一律用它；
  绝对物理   Win32 屏幕坐标（GetWindowRect / rcMonitor 那套，原点在主屏 0,0）；
  画布坐标   抓出来的底图像素坐标 = 绝对物理 - 画布原点（物理虚拟桌面左上角）。
底图画布就是物理虚拟桌面本身，每屏按自己的原生像素 1:1 贴入，缩放倍率恒为 1；
早期版本用「全屏幕最大 dpr」统一比例，混合 DPI 下低缩放屏会被上采样、截出来发糊。
所有换算只走 DesktopMap 这几个方法。
"""
from __future__ import annotations

import ctypes
import logging
from ctypes import POINTER, WINFUNCTYPE, wintypes

from PySide6.QtCore import QPoint, QPointF, QRect, QRectF, QSize
from PySide6.QtGui import QGuiApplication, QImage, QPainter

log = logging.getLogger("zpin.capture")

user32 = ctypes.WinDLL("user32", use_last_error=True)


class _MonitorInfo(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT),
                ("rcWork", wintypes.RECT), ("dwFlags", wintypes.DWORD),
                ("szDevice", ctypes.c_wchar * 32)]


# 句柄/LPARAM 一律 c_void_p：按默认 c_int 会把 64 位值截断
_ENUM_PROC = WINFUNCTYPE(wintypes.BOOL, wintypes.HMONITOR, ctypes.c_void_p,
                         POINTER(wintypes.RECT), ctypes.c_void_p)
user32.GetMonitorInfoW.argtypes = [wintypes.HMONITOR, POINTER(_MonitorInfo)]
user32.GetMonitorInfoW.restype = wintypes.BOOL
user32.EnumDisplayMonitors.argtypes = [wintypes.HDC, ctypes.c_void_p, _ENUM_PROC,
                                       ctypes.c_void_p]
user32.EnumDisplayMonitors.restype = wintypes.BOOL


def monitor_rects() -> dict[str, tuple[int, int, int, int]]:
    """枚举所有显示器并返回它们的矩形。

    Returns:
        键为显示器设备名（与 QScreen.name() 一致），值为该屏的**绝对物理**
        矩形 (x, y, w, h)。
    """
    out: dict[str, tuple[int, int, int, int]] = {}

    def on_monitor(hmon: int | None, _dc: int | None, _rc: object,
                   _lp: int | None) -> bool:
        mi = _MonitorInfo()
        mi.cbSize = ctypes.sizeof(mi)
        if user32.GetMonitorInfoW(hmon, ctypes.byref(mi)):
            r = mi.rcMonitor
            out[mi.szDevice] = (r.left, r.top, r.right - r.left, r.bottom - r.top)
        return True

    user32.EnumDisplayMonitors(None, None, _ENUM_PROC(on_monitor), None)
    return out


class ScreenPiece:
    """一块屏的三种坐标：逻辑矩形、绝对物理矩形、自身缩放比。

    Attributes:
        logical: 该屏的 Qt 全局逻辑矩形。
        abs_phys: 该屏的绝对物理矩形。
        dpr: 该屏逻辑 -> 物理的缩放倍率。
    """

    __slots__ = ("logical", "abs_phys", "dpr")

    def __init__(self, logical: QRect, abs_phys: QRect, dpr: float) -> None:
        """拷贝存储入参矩形并记录缩放比。

        Args:
            logical: Qt 全局逻辑矩形。
            abs_phys: 绝对物理矩形。
            dpr: 逻辑 -> 物理缩放倍率。
        """
        self.logical = QRect(logical)
        self.abs_phys = QRect(abs_phys)
        self.dpr = float(dpr)

    def __repr__(self) -> str:
        return (f"ScreenPiece(logical={tuple(self.logical.getRect())}, "
                f"abs={tuple(self.abs_phys.getRect())}, dpr={self.dpr})")


class DesktopMap:
    """逻辑 <-> 画布物理 的分段映射（每屏用自己的比例）。

    Attributes:
        pieces: 每块屏的坐标信息，见 ScreenPiece。
        canvas: 物理画布矩形（像素坐标，原点在虚拟桌面左上角）。
        origin: 画布原点对应的绝对物理坐标。
    """

    __slots__ = ("pieces", "canvas", "origin")

    def __init__(self, pieces: list[ScreenPiece], canvas: QRect, origin: QPoint) -> None:
        """组装分段映射（入参按引用存储，不做拷贝）。

        Args:
            pieces: 每块屏的 ScreenPiece 列表。
            canvas: 物理画布矩形。
            origin: 画布原点的绝对物理坐标。
        """
        self.pieces = pieces
        self.canvas = QRect(canvas)
        self.origin = QPoint(origin)

    # ---- 选屏 ----
    def piece_at(self, pt: QPointF) -> ScreenPiece:
        """返回逻辑点所在的屏。

        落在 L 形排布的空白区时取最近的屏，保证换算永远有限。

        Args:
            pt: Qt 全局逻辑坐标点。

        Returns:
            命中的 ScreenPiece；无任何屏时返回空的占位 ScreenPiece。
        """
        for pc in self.pieces:
            if pc.logical.contains(pt.toPoint()):
                return pc
        if not self.pieces:
            return ScreenPiece(QRect(), QRect(), 1.0)
        return min(self.pieces,
                   key=lambda pc: (QPointF(pc.logical.center()) - pt).manhattanLength())

    def piece_at_abs(self, x: float, y: float) -> ScreenPiece:
        """返回绝对物理点所在的屏（窗口矩形用）。

        不在任何屏内时取中心最近的屏。

        Args:
            x: 绝对物理 X 坐标。
            y: 绝对物理 Y 坐标。

        Returns:
            命中的 ScreenPiece；无任何屏时返回空的占位 ScreenPiece。
        """
        p = QPointF(x, y)
        for pc in self.pieces:
            if QRectF(pc.abs_phys).contains(p):
                return pc
        if not self.pieces:
            return ScreenPiece(QRect(), QRect(), 1.0)
        return min(self.pieces,
                   key=lambda pc: (QPointF(pc.abs_phys.center()) - p).manhattanLength())

    # ---- 逻辑 -> 画布 ----
    def phys_of(self, pt: QPointF) -> QPointF:
        """把逻辑点换算为画布坐标。

        Args:
            pt: Qt 全局逻辑坐标点。

        Returns:
            画布像素坐标点（绝对物理减去画布原点）。
        """
        pc = self.piece_at(pt)
        return QPointF((pt.x() - pc.logical.left()) * pc.dpr + pc.abs_phys.left(),
                       (pt.y() - pc.logical.top()) * pc.dpr + pc.abs_phys.top()) \
            - QPointF(self.origin)

    def abs_of(self, pt: QPointF) -> QPointF:
        """把逻辑坐标换算为绝对物理坐标。

        Win32/UIA 用的那套，不带画布原点偏移。

        Args:
            pt: Qt 全局逻辑坐标点。

        Returns:
            绝对物理坐标点。
        """
        return self.phys_of(pt) + QPointF(self.origin)

    def rect_of(self, r: QRectF) -> QRect:
        """把逻辑矩形换算为画布矩形。

        四角各自按所在屏换算后取包围盒：跨屏选区每段的行数由各自比例决定，
        只映射左上/右下会让上边按 A 屏、下边按 B 屏算，高度两边都不是（丢内容）。
        单屏时四角同比例，结果与 round(x*dpr)/round(w*dpr) 完全一致。

        Args:
            r: 逻辑矩形。

        Returns:
            画布像素坐标下的整数矩形。
        """
        pts = [self.phys_of(c) for c in (r.topLeft(), r.topRight(),
                                         r.bottomLeft(), r.bottomRight())]
        l = min(p.x() for p in pts)
        t = min(p.y() for p in pts)
        rr = max(p.x() for p in pts)
        bb = max(p.y() for p in pts)
        return QRect(QPoint(round(l), round(t)),
                     QSize(round(rr - l), round(bb - t)))

    def dpr_of(self, r: QRectF) -> float:
        """返回选区里占面积最大的那块屏的比例。

        标注层与尺寸标签统一用它，跨屏也不会把标注画歪。

        Args:
            r: 逻辑矩形选区。

        Returns:
            主导屏的缩放倍率；无重叠屏时为 1.0。
        """
        best, area = 0.0, -1.0
        for pc in self.pieces:
            inter = r.intersected(QRectF(pc.logical))
            a = inter.width() * inter.height()
            if a > area:
                area, best = a, pc.dpr
        return best or 1.0

    # ---- 绝对物理（窗口矩形）-> 逻辑 ----
    def logical_of_abs_rect(self, l: float, t: float, r: float, b: float) -> QRectF:
        """把绝对物理矩形换算为逻辑矩形（按该矩形所在屏的原点与比例）。

        用 left+width 而不是 QRectF.right()：后者是"最右像素下标"，会整体偏 1px。

        Args:
            l: 绝对物理左边界。
            t: 绝对物理上边界。
            r: 绝对物理右边界。
            b: 绝对物理下边界。

        Returns:
            对应的 Qt 全局逻辑矩形。
        """
        pc = self.piece_at_abs((l + r) / 2.0, (t + b) / 2.0)
        d = pc.dpr or 1.0
        return QRectF((l - pc.abs_phys.left()) / d + pc.logical.left(),
                      (t - pc.abs_phys.top()) / d + pc.logical.top(),
                      (r - l) / d, (b - t) / d)

    def __repr__(self) -> str:
        return (f"DesktopMap(canvas={tuple(self.canvas.getRect())}, "
                f"origin={self.origin}, {self.pieces})")


def build_map() -> DesktopMap:
    """按当前屏幕组合构建分段映射。

    Returns:
        覆盖全部屏幕的 DesktopMap；检测不到任何屏幕时返回空的映射。
    """
    screens = QGuiApplication.screens()
    mons = monitor_rects()
    pieces: list[ScreenPiece] = []
    for s in screens:
        g = QRect(s.geometry())
        d = float(s.devicePixelRatio()) or 1.0
        m = mons.get(s.name())
        if m:
            abs_phys = QRect(*m)
        else:
            log.warning("显示器 %s 没匹配到物理矩形，回退 逻辑x%.3f", s.name(), d)
            abs_phys = QRect(round(g.x() * d), round(g.y() * d),
                             round(g.width() * d), round(g.height() * d))
        pieces.append(ScreenPiece(g, abs_phys, d))
    if not pieces:
        return DesktopMap([], QRect(), QPoint())
    ox = min(p.abs_phys.left() for p in pieces)
    oy = min(p.abs_phys.top() for p in pieces)
    canvas = QRect(0, 0,
                   max(p.abs_phys.left() + p.abs_phys.width() for p in pieces) - ox,
                   max(p.abs_phys.top() + p.abs_phys.height() for p in pieces) - oy)
    return DesktopMap(pieces, canvas, QPoint(ox, oy))


def virtual_bounds() -> QRect:
    """返回所有屏幕几何的逻辑包围盒。

    Returns:
        联合后的逻辑矩形，可能含负坐标。
    """
    bounds = QRect()
    for s in QGuiApplication.screens():
        bounds = bounds.united(s.geometry())
    return bounds


def grab_desktop() -> tuple[QImage, DesktopMap]:
    """抓取全部屏幕并合成物理虚拟桌面画布。

    每屏贴的是它自己的原生抓取图，倍率 1:1，不做任何重采样。

    Returns:
        (画布 QImage, 换算映射 DesktopMap) 二元组。
    """
    mp = build_map()
    canvas = QImage(max(1, mp.canvas.width()), max(1, mp.canvas.height()),
                    QImage.Format_ARGB32)
    canvas.fill(0xFF202020)
    painter = QPainter(canvas)
    for screen, pc in zip(QGuiApplication.screens(), mp.pieces):
        img = screen.grabWindow(0).toImage()   # 不转格式：drawImage 支持任意源格式，省一份整屏拷贝
        if img.isNull():
            log.warning("屏幕 %s 抓取失败，该区域留底色", screen.name())
            continue
        dst = QRect(pc.abs_phys.topLeft() - mp.origin, img.size())
        if img.size() != pc.abs_phys.size():
            # 抓取图与显示器矩形对不上时以原生抓取尺寸为准：宁可留边，也不缩放
            log.warning("屏幕 %s 抓取 %dx%d 与显示器矩形 %dx%d 不一致，按抓取尺寸贴",
                        screen.name(), img.width(), img.height(),
                        pc.abs_phys.width(), pc.abs_phys.height())
        painter.drawImage(dst, img)
    painter.end()
    return canvas, mp
