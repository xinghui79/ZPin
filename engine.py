"""标注引擎 —— 对象栈 + 撤销/重做 + 马赛克像素化底图。

坐标全部为选区底图物理像素。撤销模型：操作栈 [("add", shape) / ("remove", shape, 原下标)
/ ("erase", [(shape, 下标)...]) / ("move", shape, dx, dy) / ("resize", shape, 前, 后)]，
指针回退即撤销；重做栈在新操作时清空。
"""
from __future__ import annotations

from PySide6.QtCore import QPointF, Qt
from PySide6.QtGui import QImage, QPainter

from shapes import Mosaic, Shape


def _pixelate(img: QImage, block: int = 8) -> QImage:
    small = img.scaled(
        max(1, img.width() // block), max(1, img.height() // block),
        Qt.IgnoreAspectRatio, Qt.FastTransformation,
    )
    return small.scaled(
        img.width(), img.height(), Qt.IgnoreAspectRatio, Qt.FastTransformation
    )


class AnnotateEngine:
    """标注引擎：形状对象栈 + 撤销/重做记账 + 马赛克像素化底图。

    Args:
        base: 选区底图（物理像素），像素化图的来源。
    """

    def __init__(self, base: QImage) -> None:
        self.base = base
        self._pixelated: QImage | None = None
        self.shapes: list[Shape] = []
        self._undo: list[tuple] = []
        self._redo: list[tuple] = []
        self._stroke: list[tuple[Shape, int]] | None = None   # 本次橡皮涂抹擦掉的形状

    @property
    def pixelated(self) -> QImage:
        """马赛克用的整屏像素化图：只在真用到马赛克时才生成（一整屏 ARGB32）。"""
        if self._pixelated is None:
            self._pixelated = _pixelate(self.base)
        return self._pixelated

    # ---- 编辑 ----
    def add(self, shape: Shape) -> None:
        """把新形状追加进栈并记一步撤销。

        Args:
            shape: 要添加的形状（马赛克会自动注入像素化底图）。
        """
        if isinstance(shape, Mosaic):
            shape.pixelated = self.pixelated
        self.shapes.append(shape)
        self._undo.append(("add", shape))
        self._redo.clear()

    def remove(self, shape: Shape) -> None:
        """从栈中移除形状并记一步撤销（记录原下标，撤销时恢复 z 序）。

        Args:
            shape: 要移除的形状（不在栈中时不做任何事）。
        """
        if shape in self.shapes:
            idx = self.shapes.index(shape)
            self.shapes.remove(shape)
            self._undo.append(("remove", shape, idx))
            self._redo.clear()

    def erase_at(self, pt: QPointF, tol: float = 8.0) -> bool:
        """擦除命中点所在的最上层形状。

        Args:
            pt: 擦除点（图像物理像素坐标）。
            tol: 命中容差（物理像素）。

        Returns:
            是否擦除了形状。
        """
        for shape in reversed(self.shapes):
            if shape.hit(pt, tol):
                idx = self.shapes.index(shape)
                self.shapes.remove(shape)
                if self._stroke is None:
                    self._undo.append(("remove", shape, idx))
                    self._redo.clear()
                else:
                    # 涂抹中：先攒着，抬手时合成一步撤销
                    self._stroke.append((shape, idx))
                return True
        return False

    def begin_erase_stroke(self) -> None:
        """开始一段连续橡皮涂抹（期间被擦形状先攒着，不入撤销栈）。"""
        self._stroke = []

    def end_erase_stroke(self) -> None:
        """结束涂抹，把整段擦除合成一步撤销记账。"""
        batch, self._stroke = self._stroke, None
        if batch:
            self._undo.append(("erase", batch))
            self._redo.clear()

    def commit_move(self, shape: Shape, dx: float, dy: float) -> None:
        """记录一次已完成的平移（形状位置已实时更新过，此处只记账供撤销）。"""
        if abs(dx) < 0.5 and abs(dy) < 0.5:
            return
        self._undo.append(("move", shape, float(dx), float(dy)))
        self._redo.clear()

    def commit_resize(self, shape: Shape, before: list[QPointF],
                      after: list[QPointF]) -> None:
        """记录一次已应用的端点改动（二次编辑改形状）。"""
        if len(before) != len(after):
            return
        if all(abs(a.x() - b.x()) < 0.5 and abs(a.y() - b.y()) < 0.5
               for a, b in zip(before, after)):
            return
        self._undo.append(("resize", shape, before, after))
        self._redo.clear()

    def translate_all(self, dx: float, dy: float) -> None:
        """选区整体平移时把全部形状跟着位移（物理像素，不占撤销步）。

        标注锚定在选区上（Snipaste 语义）：框移动标注跟随。注意这是画布坐标
        的整体平移，撤销栈不感知——跨一次选区平移后撤销旧编辑，形状会回到
        平移前的画布位置（与"钉在画布上"的旧语义一致的边界表现）。

        Args:
            dx: X 方向位移（物理像素）。
            dy: Y 方向位移（物理像素）。
        """
        for s in self.shapes:
            s.translate(dx, dy)

    def undo(self) -> None:
        """撤销最近一步操作（弹出撤销栈、应用逆向效果并压入重做栈）。"""
        if not self._undo:
            return
        kind, *rest = self._undo.pop()
        if kind == "add":
            shape = rest[0]
            if shape in self.shapes:
                self.shapes.remove(shape)
            self._redo.append(("add", shape))
        elif kind == "remove":
            shape, idx = rest
            self.shapes.insert(min(idx, len(self.shapes)), shape)
            self._redo.append(("remove", shape, idx))
        elif kind == "erase":
            for shape, idx in reversed(rest[0]):
                self.shapes.insert(idx, shape)
            self._redo.append(("erase", rest[0]))
        elif kind == "resize":
            shape, before, after = rest
            shape.set_endpoints(before)
            self._redo.append(("resize", shape, before, after))
        else:  # move
            shape, dx, dy = rest
            shape.translate(-dx, -dy)
            self._redo.append(("move", shape, dx, dy))

    def redo(self) -> None:
        """重做最近一步被撤销的操作（弹出重做栈、重新应用并压回撤销栈）。"""
        if not self._redo:
            return
        kind, *rest = self._redo.pop()
        if kind == "add":
            shape = rest[0]
            self.shapes.append(shape)
            self._undo.append(("add", shape))
        elif kind == "remove":
            shape, idx = rest
            if shape in self.shapes:
                self.shapes.remove(shape)
            self._undo.append(("remove", shape, idx))
        elif kind == "erase":
            for shape, _idx in rest[0]:
                if shape in self.shapes:
                    self.shapes.remove(shape)
            self._undo.append(("erase", rest[0]))
        elif kind == "resize":
            shape, before, after = rest
            shape.set_endpoints(after)
            self._undo.append(("resize", shape, before, after))
        else:  # move
            shape, dx, dy = rest
            shape.translate(dx, dy)
            self._undo.append(("move", shape, dx, dy))

    def can_undo(self) -> bool:
        """是否还有可撤销的操作。"""
        return bool(self._undo)

    def can_redo(self) -> bool:
        """是否还有可重做的操作。"""
        return bool(self._redo)

    # ---- 绘制 ----
    def draw(self, p: QPainter) -> None:
        """按栈顺序把全部形状绘制到画布上。

        Args:
            p: 画笔。
        """
        for shape in self.shapes:
            shape.draw(p)
