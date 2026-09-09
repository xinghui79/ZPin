"""截图历史 —— 内存中的最近截图列表，供托盘历史菜单重贴。

条数与内存上限均可配（General/history_limit、General/history_max_mb）。
1080p 的 ARGB32 单张约 8MB，早期版本固定 20 条常驻 ≈160MB 不释放。
现在的口径：刚截完先按 QImage 存（与剪贴板/贴图共用同一块缓冲，不额外占内存），
**每次截图完成后立即**（以及空闲补压时）把最老的若干张在**工作线程**里压成 PNG
（本机实测整屏约 8 倍缩水），取用时才解回来；再按字节预算淘汰最旧条目。
"""
from __future__ import annotations

import logging
from datetime import datetime

from PySide6.QtCore import (QBuffer, QIODevice, QObject, QRunnable, Signal, QThreadPool)
from PySide6.QtGui import QImage

import config

log = logging.getLogger("zpin.history")

_PENDING_MAX = 4          # 同时在压的张数（工作线程里跑，不占主线程）

# 首选项改的是全局配置，需要通知所有已创建的 store（目前只有 main 里那一个）
_STORES: list["HistoryStore"] = []


def set_limits(*_args: object) -> None:
    """把新的条数/内存上限广播给所有已创建的 HistoryStore。

    首选项里改了条数/上限时调用（BoundSpin 会把新值传进来，忽略即可）。

    Args:
        *_args: 信号传来的新值，这里忽略。
    """
    for s in list(_STORES):
        s.set_limits()


def compress_pending(*_args: object) -> None:
    """空闲整理回调：把驻留的原图交给工作线程压成 PNG。

    Args:
        *_args: 定时器/信号传来的参数，忽略。
    """
    for s in list(_STORES):
        s.compress_pending()


class _Done(QObject):
    packed = Signal(int, object)     # (条目 id, PNG 字节 或 None)


class _PackTask(QRunnable):
    """把单张 QImage 在工作线程里压成 PNG 的任务。

    QImage.save 只读位数据，跨线程跑安全（历史里的图不会被改写）。
    """

    def __init__(self, img: QImage, idx: int, done: _Done) -> None:
        super().__init__()
        self.setAutoDelete(True)
        self._img, self._idx, self._done = img, idx, done

    def run(self) -> None:
        self._done.packed.emit(self._idx, _to_png(self._img))


class HistoryStore(QObject):
    """内存中的最近截图历史，供托盘历史菜单重贴与空闲压缩。

    Attributes:
        changed: 历史列表变化时发射，参数为 [(序号, "MM-dd HH:mm 1920×1080")]，
            最新在前。
    """

    changed = Signal(list)  # [(序号, "MM-dd HH:mm 1920×1080")]，最新在前

    def __init__(self, limit: int = 20, max_mb: int = 96) -> None:
        """创建历史存储并注册到全局广播列表。

        Args:
            limit: 最多保留的截图条数，0 表示不保留。
            max_mb: 历史允许占用的内存上限（MB）。
        """
        super().__init__()
        self._limit = limit
        self._max_bytes = max_mb * 1024 * 1024
        self._next_id = 0
        # 每项 [id, 原图或 None, PNG 字节或 None, 显示文本, 截图时的比例]
        self._items: list[list] = []
        self._pending: set[int] = set()
        self._done = _Done()
        self._done.packed.connect(self._on_packed)
        _STORES.append(self)

    # ---- 配置 ----
    def set_limits(self, *_args: object) -> None:
        """按最新配置刷新条数与内存上限。

        首选项改了条数/上限后调用；limit=0 视为清空并不再保留。

        Args:
            *_args: 调用方可能传入的新值，忽略。
        """
        self._limit = max(0, int(config.get("General/history_limit")))
        self._max_bytes = max(1, int(config.get("General/history_max_mb"))) * 1024 * 1024
        if self._limit == 0:
            self._items.clear()
        self._trim()
        self._emit()

    # ---- 增删 ----
    def add(self, img: QImage, dpr: float = 1.0) -> None:
        """把一张新截图插入历史最前，并触发淘汰与变更通知。

        Args:
            img: 要记录的截图。
            dpr: 截图时的设备缩放比，重贴时按它还原视觉大小。
        """
        if img.isNull() or self._limit <= 0:
            return
        now = datetime.now()
        text = (f"{now.month:02d}-{now.day:02d} {now.hour:02d}:{now.minute:02d}"
                f"  {img.width()}×{img.height()}")
        self._items.insert(0, [self._next_id, QImage(img), None, text, float(dpr or 1.0)])
        self._next_id += 1
        self._trim()
        self._emit()

    def dpr_of(self, idx: int) -> float:
        """返回这条历史截图当时的缩放比。

        贴图要按它还原成截见时的视觉大小。

        Args:
            idx: 历史条目 id。

        Returns:
            当时的缩放比；查不到条目或未记录时为 1.0。
        """
        for item in self._items:
            if item[0] == idx:
                return item[4] if len(item) > 4 and item[4] else 1.0
        return 1.0

    def get(self, idx: int) -> QImage:
        """按 id 取回历史截图。

        Args:
            idx: 历史条目 id。

        Returns:
            对应的 QImage（驻留原图或从 PNG 解回）；条目不存在时返回空 QImage。
        """
        for item in self._items:
            if item[0] != idx:
                continue
            if item[1] is not None:
                return item[1]
            img = QImage()
            if item[2]:
                img.loadFromData(item[2], "PNG")
            return img
        return QImage()

    def compress_pending(self) -> None:
        """把最老的未压缩条目交给工作线程压 PNG（不阻塞主线程）。"""
        room = _PENDING_MAX - len(self._pending)
        if room <= 0:
            return
        todo = [it for it in self._items
                if it[1] is not None and it[0] not in self._pending][-room:]
        for it in todo:
            self._pending.add(it[0])
            QThreadPool.globalInstance().start(_PackTask(it[1], it[0], self._done))

    def _on_packed(self, idx: int, png: bytes | None) -> None:
        """结果回到主线程应用：换掉原图，再按新字节数走一遍预算。

        Args:
            idx: 历史条目 id。
            png: 压缩得到的 PNG 字节；失败时为 None。
        """
        self._pending.discard(idx)
        for it in self._items:
            if it[0] == idx and it[1] is not None and png:
                it[2] = png
                it[1] = None
                self._trim()
                break

    def clear(self) -> None:
        """清空全部历史并通知界面。"""
        self._items.clear()
        self._emit()

    # ---- 内部 ----
    def _bytes(self, item: list) -> int:
        if item[1] is not None:
            return item[1].sizeInBytes()
        return len(item[2] or b"")

    def _total(self) -> int:
        return sum(self._bytes(i) for i in self._items)

    def _trim(self) -> None:
        del self._items[self._limit:]
        total = self._total()
        while self._items and total > self._max_bytes:
            total -= self._bytes(self._items.pop())
        if self._items:
            log.debug("截图历史：%d 条，约 %.1f MB", len(self._items), total / 1024 / 1024)

    def _emit(self) -> None:
        self.changed.emit([(i[0], i[3]) for i in self._items])


def _to_png(img: QImage) -> bytes | None:
    """把单张 QImage 压成 PNG 字节。

    质量 0 = 最大压缩；截图这种大色块图压得动，代价留给空闲时段付。

    Args:
        img: 待压缩的图像。

    Returns:
        PNG 字节；保存失败时为 None。
    """
    buf = QBuffer()
    buf.open(QIODevice.WriteOnly)
    if not img.save(buf, "PNG", 0):
        return None
    return bytes(buf.data())
