"""应用图标 —— 单一来源为内嵌 SVG（渐变玻璃底 + 取景框对角角标 + 居中 Z）。

设计语言（2026-09 重绘，对齐 Win11/macOS 时代的立体质感）：
  层次：顶部高光的蓝青渐变底 → 液态玻璃式镜面层 → 1px 低透明度细描边 →
  白色取景框对角弧与居中 Z。16px 下靠角标 + Z 剪影识别。
运行时用 QSvgRenderer 按需渲染多尺寸；assets/app_icon.ico 由 build.bat 打包前的
图标同步步骤（write_ico）生成，供 exe 资源使用。
"""
from __future__ import annotations

import struct

from PySide6.QtCore import QBuffer, QRectF, Qt
from PySide6.QtGui import QIcon, QImage, QPainter, QPixmap
from PySide6.QtSvg import QSvgRenderer

# 主色（正常态）：品牌蓝纵向渐变；停用态：整底换琥珀渐变 + 深蓝描边——
# 托盘 16px 下只换描边色几乎看不出状态，底色变掉才是可靠的状态信号。
_NORMAL = {"top": "#59A2FF", "bottom": "#1E56C8", "stroke": "#FFFFFF"}
_DISABLED = {"top": "#FFC93C", "bottom": "#F0A32B", "stroke": "#1E3C8C"}

_SVG_TMPL = r'''<svg width="256" height="256" viewBox="0 0 256 256" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="{top}"/>
      <stop offset="1" stop-color="{bottom}"/>
    </linearGradient>
    <linearGradient id="gloss" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#FFFFFF" stop-opacity="0.30"/>
      <stop offset="0.45" stop-color="#FFFFFF" stop-opacity="0.05"/>
      <stop offset="1" stop-color="#FFFFFF" stop-opacity="0"/>
    </linearGradient>
  </defs>
  <rect x="10" y="10" width="236" height="236" rx="58" fill="url(#bg)"/>
  <rect x="10" y="10" width="236" height="236" rx="58" fill="url(#gloss)"/>
  <rect x="11.5" y="11.5" width="233" height="233" rx="56.5"
        fill="none" stroke="#FFFFFF" stroke-opacity="0.28" stroke-width="3"/>
  <!-- 取景框：左上角与右下角的外凸圆角弧度边（居中于 128,128，点对称） -->
  <g fill="none" stroke="{stroke}" stroke-width="14"
     stroke-linecap="round" stroke-linejoin="round">
    <path d="M 62 102 L 62 84 A 22 22 0 0 1 84 62 L 102 62"/>
    <path d="M 194 154 L 194 172 A 22 22 0 0 1 172 194 L 154 194"/>
  </g>
  <!-- 框内居中的 Z -->
  <g fill="none" stroke="{stroke}" stroke-width="14"
     stroke-linecap="round" stroke-linejoin="round">
    <path d="M 92 98 L 164 98 L 92 158 L 164 158"/>
  </g>
</svg>'''

SVG = _SVG_TMPL.format(**_NORMAL)

# 停用全局快捷键时的托盘图标变体：琥珀底 + 深蓝描边（黄=暂停/警告的惯用语义）。
SVG_DISABLED = _SVG_TMPL.format(**_DISABLED)

_SIZES = (16, 24, 32, 48, 64, 128, 256)
_icon: QIcon | None = None
_disabled_icon: QIcon | None = None


def _render(size: int, svg: str = SVG) -> QImage:
    img = QImage(size, size, QImage.Format_ARGB32)
    img.fill(Qt.transparent)
    p = QPainter(img)
    p.setRenderHint(QPainter.Antialiasing)
    QSvgRenderer(svg.encode()).render(p, QRectF(0, 0, size, size))
    p.end()
    return img


def app_icon(disabled: bool = False) -> QIcon:
    """返回多尺寸预渲染的应用/托盘图标（带缓存）。

    Args:
        disabled: True 返回停用全局快捷键的变体（白色描边换亮黄）。

    Returns:
        可直接用于窗口/托盘的多尺寸 QIcon。
    """
    global _icon, _disabled_icon
    if disabled:
        if _disabled_icon is None:
            icon = QIcon()
            for s in _SIZES:
                icon.addPixmap(QPixmap.fromImage(_render(s, SVG_DISABLED)))
            _disabled_icon = icon
        return _disabled_icon
    if _icon is None:
        icon = QIcon()
        for s in _SIZES:
            icon.addPixmap(QPixmap.fromImage(_render(s)))
        _icon = icon
    return _icon


def write_ico(path: str) -> None:
    """把 SVG 渲染为多尺寸 .ico 并写入指定路径。

    PNG 压缩条目，Vista 及以上有效；build.bat 打包前的图标同步步骤调用。

    Args:
        path: .ico 输出路径。
    """
    pngs: list[tuple[int, bytes]] = []
    for s in _SIZES:
        buf = QBuffer()
        buf.open(QBuffer.OpenModeFlag.WriteOnly)
        _render(s).save(buf, "PNG")
        pngs.append((s, bytes(buf.data())))
    header = struct.pack("<HHH", 0, 1, len(pngs))
    entries = b""
    body = b""
    offset = 6 + 16 * len(pngs)
    for s, data in pngs:
        side = 0 if s >= 256 else s
        entries += struct.pack("<BBBBHHII", side, side, 0, 0, 1, 32, len(data), offset)
        body += data
        offset += len(data)
    with open(path, "wb") as f:
        f.write(header + entries + body)
