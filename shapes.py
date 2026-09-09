"""标注对象 —— 全部坐标为选区底图的物理像素坐标。"""
from __future__ import annotations

import math

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import (
    QColor,
    QFont,
    QFontMetrics,
    QImage,
    QPainter,
    QPainterPath,
    QPainterPathStroker,
    QPen,
    QPolygonF,
)

HIGHLIGHT_ALPHA = 128


def _seg_dist(pt: QPointF, a: QPointF, b: QPointF) -> float:
    vx, vy = b.x() - a.x(), b.y() - a.y()
    wx, wy = pt.x() - a.x(), pt.y() - a.y()
    l2 = vx * vx + vy * vy
    if l2 == 0:
        return math.hypot(wx, wy)
    t = max(0.0, min(1.0, (wx * vx + wy * vy) / l2))
    return math.hypot(pt.x() - (a.x() + t * vx), pt.y() - (a.y() + t * vy))


class Shape:
    """标注形状基类：定义绘制、命中、平移与逐点编辑接口。

    所有坐标均为选区底图的物理像素。

    Args:
        color: 形状颜色。
        width: 线宽（物理像素）。
    """

    def __init__(self, color: QColor, width: float) -> None:
        self.color = QColor(color)
        self.width = float(width)

    def draw(self, p: QPainter) -> None:
        """把形状绘制到画布上（子类必须实现）。

        Args:
            p: 画笔。
        """
        raise NotImplementedError

    def hit(self, pt: QPointF, tol: float = 6.0) -> bool:
        """命中检测：判断点是否落在形状上（考虑线宽与容差）。

        Args:
            pt: 检测点（图像物理像素坐标）。
            tol: 额外容差半径（物理像素）。

        Returns:
            是否命中；基类恒为 False。
        """
        return False

    def translate(self, dx: float, dy: float) -> None:
        """整体平移形状（子类必须实现）。

        Args:
            dx: X 方向位移（物理像素）。
            dy: Y 方向位移（物理像素）。
        """
        raise NotImplementedError

    def endpoints(self) -> list[QPointF]:
        """返回可单独拖动的控制点（改形状用）。

        Returns:
            控制点列表；空表 = 不支持逐点编辑。
        """
        return []

    def set_endpoints(self, pts: list[QPointF]) -> None:
        """按 endpoints() 的顺序重设端点（子类必须实现）。

        Args:
            pts: 新端点列表。
        """
        raise NotImplementedError

    def bounding_rect(self) -> QRectF:
        """返回可视包围盒（选中框用）。

        Returns:
            包围盒；未知/无内容返回空矩形。
        """
        return QRectF()


def endpoints(shape: Shape) -> list[QPointF]:
    """取形状的端点快照。

    Args:
        shape: 目标形状。

    Returns:
        端点快照列表（不可逐点编辑的形状返回空表），供撤销栈记账。
    """
    return [QPointF(p) for p in shape.endpoints()]


class Stroke(Shape):
    """画笔 / 荧光笔（polyline）。

    荧光笔（highlight=True）按 3 倍线宽、半透明绘制。

    Args:
        color: 笔画颜色。
        width: 基准线宽（物理像素）。
        highlight: True 为荧光笔样式。
    """

    def __init__(self, color: QColor, width: float, highlight: bool = False) -> None:
        super().__init__(color, width)
        self.highlight = highlight
        self.points: list[QPointF] = []

    def add_point(self, pt: QPointF) -> None:
        """追加一个笔迹采样点。

        Args:
            pt: 追加的点（图像物理像素坐标）。
        """
        self.points.append(QPointF(pt))

    def _pen_width(self) -> float:
        return self.width * 3 if self.highlight else self.width

    def draw(self, p: QPainter) -> None:
        """绘制整条 polyline；单点时画圆点。

        Args:
            p: 画笔。
        """
        if not self.points:
            return
        color = QColor(self.color)
        if self.highlight:
            color.setAlpha(HIGHLIGHT_ALPHA)
        pen = QPen(color, self._pen_width())
        pen.setCapStyle(Qt.RoundCap)
        pen.setJoinStyle(Qt.RoundJoin)
        p.setPen(pen)
        p.setBrush(Qt.NoBrush)
        if len(self.points) == 1:
            p.drawPoint(self.points[0])
        else:
            p.drawPolyline(QPolygonF(self.points))

    def hit(self, pt: QPointF, tol: float = 6.0) -> bool:
        """命中检测：点到任一线段（或末点）的距离不超过限值。

        Args:
            pt: 检测点。
            tol: 额外容差。

        Returns:
            是否命中。
        """
        lim = max(tol, self._pen_width() / 2 + tol / 2)
        for i in range(len(self.points) - 1):
            if _seg_dist(pt, self.points[i], self.points[i + 1]) <= lim:
                return True
        return bool(self.points) and math.hypot(
            pt.x() - self.points[-1].x(), pt.y() - self.points[-1].y()) <= lim

    def translate(self, dx: float, dy: float) -> None:
        """整体平移全部笔迹点。

        Args:
            dx: X 方向位移。
            dy: Y 方向位移。
        """
        self.points = [QPointF(q.x() + dx, q.y() + dy) for q in self.points]

    def bounding_rect(self) -> QRectF:
        """返回含笔宽内边距的包围盒。

        Returns:
            包围盒；无笔迹时为空矩形。
        """
        if not self.points:
            return QRectF()
        xs = [p.x() for p in self.points]
        ys = [p.y() for p in self.points]
        pad = self._pen_width() / 2 + 2
        return QRectF(min(xs) - pad, min(ys) - pad,
                      max(xs) - min(xs) + pad * 2, max(ys) - min(ys) + pad * 2)


class Mosaic(Stroke):
    """马赛克：沿笔画路径取出像素化底图（引擎注入 pixelated）。

    Args:
        width: 笔画宽度（物理像素）。
    """

    def __init__(self, width: float) -> None:
        super().__init__(QColor("#000000"), width)
        self.pixelated: QImage | None = None

    def draw(self, p: QPainter) -> None:
        """以笔画路径描边出的区域为裁剪，绘制像素化底图。

        Args:
            p: 画笔。
        """
        if not self.points or self.pixelated is None:
            return
        path = QPainterPath()
        path.addPolygon(QPolygonF(self.points))
        stroker = QPainterPathStroker()
        stroker.setWidth(max(4.0, self.width))
        stroker.setCapStyle(Qt.RoundCap)
        stroker.setJoinStyle(Qt.RoundJoin)
        p.save()
        p.setClipPath(stroker.createStroke(path))
        p.drawImage(0, 0, self.pixelated)
        p.restore()

    def translate(self, dx: float, dy: float) -> None:
        """整体平移笔画路径（委托给 Stroke）。

        Args:
            dx: X 方向位移。
            dy: Y 方向位移。
        """
        super().translate(dx, dy)


class Line(Shape):
    """直线段。

    Args:
        color: 线条颜色。
        width: 线宽（物理像素）。
    """

    def __init__(self, color: QColor, width: float) -> None:
        super().__init__(color, width)
        self.p1 = QPointF()
        self.p2 = QPointF()

    def draw(self, p: QPainter) -> None:
        """绘制直线段。

        Args:
            p: 画笔。
        """
        pen = QPen(self.color, self.width)
        pen.setCapStyle(Qt.RoundCap)
        p.setPen(pen)
        p.drawLine(self.p1, self.p2)

    def hit(self, pt: QPointF, tol: float = 6.0) -> bool:
        """命中检测：点到线段距离不超过限值。

        Args:
            pt: 检测点。
            tol: 额外容差。

        Returns:
            是否命中。
        """
        return _seg_dist(pt, self.p1, self.p2) <= max(tol, self.width / 2 + tol / 2)

    def translate(self, dx: float, dy: float) -> None:
        """整体平移两端点。

        Args:
            dx: X 方向位移。
            dy: Y 方向位移。
        """
        self.p1 += QPointF(dx, dy)
        self.p2 += QPointF(dx, dy)

    def endpoints(self) -> list[QPointF]:
        """返回可拖动的两端点。

        Returns:
            [起点, 终点]。
        """
        return [self.p1, self.p2]

    def set_endpoints(self, pts: list[QPointF]) -> None:
        """按顺序重设两端点。

        Args:
            pts: 新端点列表（[起点, 终点]）。
        """
        self.p1, self.p2 = QPointF(pts[0]), QPointF(pts[1])

    def bounding_rect(self) -> QRectF:
        """返回含线宽内边距的归一化包围盒。

        Returns:
            包围盒。
        """
        pad = self.width / 2 + 2
        return QRectF(self.p1, self.p2).normalized().adjusted(-pad, -pad, pad, pad)


class Arrow(Line):
    """带实心箭头头部的直线段。"""

    def draw(self, p: QPainter) -> None:
        """绘制线段与实心箭头头部。

        Args:
            p: 画笔。
        """
        super().draw(p)
        ang = math.atan2(self.p2.y() - self.p1.y(), self.p2.x() - self.p1.x())
        size = self.width * 2.5 + 8
        p.save()
        p.setPen(Qt.NoPen)
        p.setBrush(self.color)
        head = QPolygonF([
            self.p2,
            QPointF(self.p2.x() + size * math.cos(ang + math.radians(158)),
                    self.p2.y() + size * math.sin(ang + math.radians(158))),
            QPointF(self.p2.x() + size * math.cos(ang - math.radians(158)),
                    self.p2.y() + size * math.sin(ang - math.radians(158))),
        ])
        p.drawPolygon(head)
        p.restore()


class Rect(Shape):
    """矩形框（只描边不填充）。

    Args:
        color: 边框颜色。
        width: 边框线宽（物理像素）。
    """

    def __init__(self, color: QColor, width: float) -> None:
        super().__init__(color, width)
        self.p1 = QPointF()
        self.p2 = QPointF()

    def _rect(self) -> QRectF:
        return QRectF(self.p1, self.p2).normalized()

    def draw(self, p: QPainter) -> None:
        """绘制矩形边框。

        Args:
            p: 画笔。
        """
        p.setPen(QPen(self.color, self.width))
        p.setBrush(Qt.NoBrush)
        p.drawRect(self._rect())

    def hit(self, pt: QPointF, tol: float = 6.0) -> bool:
        """命中检测：只算边框环带，内部空白不算命中。

        Args:
            pt: 检测点。
            tol: 额外容差。

        Returns:
            是否命中。
        """
        r = self._rect()
        inner = r.adjusted(self.width + tol, self.width + tol,
                           -self.width - tol, -self.width - tol)
        return r.contains(pt) and not inner.contains(pt)

    def translate(self, dx: float, dy: float) -> None:
        """整体平移两个对角点。

        Args:
            dx: X 方向位移。
            dy: Y 方向位移。
        """
        self.p1 += QPointF(dx, dy)
        self.p2 += QPointF(dx, dy)

    def endpoints(self) -> list[QPointF]:
        """返回可拖动的两个对角点。

        Returns:
            [对角点 1, 对角点 2]。
        """
        return [self.p1, self.p2]

    def set_endpoints(self, pts: list[QPointF]) -> None:
        """按顺序重设两个对角点。

        Args:
            pts: 新端点列表（[对角点 1, 对角点 2]）。
        """
        self.p1, self.p2 = QPointF(pts[0]), QPointF(pts[1])

    def bounding_rect(self) -> QRectF:
        """返回含线宽内边距的包围盒。

        Returns:
            包围盒。
        """
        pad = self.width / 2 + 2
        return self._rect().adjusted(-pad, -pad, pad, pad)


class Ellipse(Rect):
    """椭圆框（几何定义与矩形一致）。"""

    def draw(self, p: QPainter) -> None:
        """绘制椭圆边框。

        Args:
            p: 画笔。
        """
        p.setPen(QPen(self.color, self.width))
        p.setBrush(Qt.NoBrush)
        p.drawEllipse(self._rect())


class TextShape(Shape):
    """多行文字块。

    Args:
        color: 文字颜色。
        font_size: 字号（像素，设为字体 pixelSize）。
    """

    def __init__(self, color: QColor, font_size: float) -> None:
        super().__init__(color, 1)
        self.pos = QPointF()
        self.lines: list[str] = []
        self.font_size = font_size

    def _font(self) -> QFont:
        f = QFont("Microsoft YaHei UI")
        f.setPixelSize(int(self.font_size))
        f.setWeight(QFont.DemiBold)
        return f

    def _metrics(self) -> QFontMetrics:
        return QFontMetrics(self._font())

    def size(self) -> tuple[float, float]:
        """按字体度量估算文字块尺寸。

        Returns:
            (宽, 高)（物理像素；无内容时宽度按 10 计）。
        """
        fm = self._metrics()
        w = max((fm.horizontalAdvance(t) for t in self.lines), default=10)
        return w, fm.height() * len(self.lines)

    def draw(self, p: QPainter) -> None:
        """逐行绘制文字（先描半透明白底再着色，浅色背景下也可读）。

        Args:
            p: 画笔。
        """
        p.setFont(self._font())
        fm = self._metrics()
        line_h = fm.height()
        y = self.pos.y() + fm.ascent()
        for t in self.lines:
            p.setPen(QPen(QColor(255, 255, 255, 170), 3))
            p.drawText(QPointF(self.pos.x(), y), t)
            p.setPen(QPen(self.color))
            p.drawText(QPointF(self.pos.x(), y), t)
            y += line_h

    def hit(self, pt: QPointF, tol: float = 6.0) -> bool:
        """命中检测：整个文字块矩形（含容差外扩）。

        Args:
            pt: 检测点。
            tol: 额外容差。

        Returns:
            是否命中。
        """
        w, h = self.size()
        return QRectF(self.pos.x() - tol, self.pos.y() - tol,
                      w + 2 * tol, h + 2 * tol).contains(pt)

    def translate(self, dx: float, dy: float) -> None:
        """整体平移文字块。

        Args:
            dx: X 方向位移。
            dy: Y 方向位移。
        """
        self.pos += QPointF(dx, dy)

    def bounding_rect(self) -> QRectF:
        """返回文字块包围盒（四周各留 4px）。

        Returns:
            包围盒。
        """
        w, h = self.size()
        return QRectF(self.pos.x() - 4, self.pos.y() - 4, w + 8, h + 8)


class Callout(TextShape):
    """标注：气泡框 + 指向尾巴 + 文字。

    Args:
        color: 气泡边框与文字颜色。
        font_size: 字号（像素）。
    """

    def __init__(self, color: QColor, font_size: float) -> None:
        super().__init__(color, font_size)
        self.tail = QPointF()  # 指向点（图上）

    def draw(self, p: QPainter) -> None:
        """绘制尾巴线、白色圆角气泡框与框内文字。

        Args:
            p: 画笔。
        """
        pen = QPen(self.color, 3)
        pen.setCapStyle(Qt.RoundCap)
        p.setPen(pen)
        p.drawLine(self.tail, QPointF(self.pos.x(), self.pos.y() + self.font_size * 0.6))
        p.setFont(self._font())
        fm = self._metrics()
        pad_x, pad_y = 8, 5
        w = max((fm.horizontalAdvance(t) for t in self.lines), default=10) + pad_x * 2
        h = fm.height() * len(self.lines) + pad_y * 2
        bubble = QRectF(self.pos.x(), self.pos.y(), w, h)
        p.setBrush(QColor(255, 255, 255, 235))
        p.setPen(QPen(self.color, 2.5))
        p.drawRoundedRect(bubble, 8, 8)
        p.setPen(QPen(self.color))
        y = self.pos.y() + pad_y
        for t in self.lines:
            p.drawText(QRectF(self.pos.x(), y, w - pad_x * 2, fm.height()),
                       Qt.AlignLeft | Qt.AlignVCenter, t)
            y += fm.height()

    def hit(self, pt: QPointF, tol: float = 6.0) -> bool:
        """命中检测：气泡矩形或尾巴线段。

        Args:
            pt: 检测点。
            tol: 额外容差。

        Returns:
            是否命中。
        """
        fm = self._metrics()
        w = max((fm.horizontalAdvance(t) for t in self.lines), default=10) + 16
        h = fm.height() * len(self.lines) + 10
        if QRectF(self.pos.x() - tol, self.pos.y() - tol,
                  w + 2 * tol, h + 2 * tol).contains(pt):
            return True
        return _seg_dist(pt, self.tail, self.pos) <= tol + 2

    def translate(self, dx: float, dy: float) -> None:
        """整体平移（尾巴与气泡一起移动）。

        Args:
            dx: X 方向位移。
            dy: Y 方向位移。
        """
        self.tail += QPointF(dx, dy)
        super().translate(dx, dy)

    def bounding_rect(self) -> QRectF:
        """返回气泡与尾巴的联合包围盒。

        Returns:
            包围盒。
        """
        w, h = self.size()
        box = QRectF(self.pos.x() - 12, self.pos.y() - 12, w + 32, h + 32)
        return box.united(QRectF(self.tail, self.tail).adjusted(-6, -6, 6, 6))

    def endpoints(self) -> list[QPointF]:
        """返回可拖动的两端点。

        Returns:
            [尾巴指向点, 气泡位置]。
        """
        return [self.tail, self.pos]

    def set_endpoints(self, pts: list[QPointF]) -> None:
        """按顺序重设两端点。

        Args:
            pts: 新端点列表（[尾巴指向点, 气泡位置]）。
        """
        self.tail, self.pos = QPointF(pts[0]), QPointF(pts[1])
