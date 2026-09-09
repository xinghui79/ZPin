"""贴图管理 —— 组结构、全局隐藏/显示与切组。

组列表来自 Groups/names（顿号分隔）。PinWindow 生命周期归组管理；
全局动作对齐托盘/热键：隐藏/显示全部、切换当前组。
"""
from __future__ import annotations

import logging

from PySide6.QtCore import QObject, QPointF, Signal
from PySide6.QtGui import QCursor, QImage

import config
import memtrim
from pin_window import PinWindow

log = logging.getLogger("zpin.pins")


class PinGroup:
    """一个贴图组：组名与组内贴图窗口的容器。

    Args:
        name: 组名（来自 Groups/names 配置）。
    """

    def __init__(self, name: str) -> None:
        self.name = name
        self.windows: list[PinWindow] = []


class PinManager(QObject):
    """贴图管理器：维护组结构与贴图窗口，提供贴图入口和全局隐藏/显示、切组。

    Args:
        parent: Qt 父对象。

    Signals:
        notify(str, str): 托盘气泡通知（标题, 内容）。
        groups_changed(list, str): 组集合变化（全部组名, 当前组名）。
    """

    notify = Signal(str, str)  # 标题, 内容（接托盘气泡）
    groups_changed = Signal(list, str)  # 全部组名, 当前组名

    def __init__(self, parent: QObject | None = None) -> None:
        super().__init__(parent)
        names = [n.strip() for n in str(config.get("Groups/names")).split("、") if n.strip()]
        self.groups: list[PinGroup] = [PinGroup(n) for n in names] or [PinGroup("贴图")]
        self._cur = 0
        self._hidden = False
        # 空闲时丢掉缩略图缓存（贴图窗口会在需要时重建）
        memtrim.register_cleaner(self._drop_thumb_cache)

    def _drop_thumb_cache(self) -> None:
        for w in self.all_windows():
            w._thumb = None

    # ---- 组 ----
    @property
    def current(self) -> PinGroup:
        """当前活动组。"""
        return self.groups[self._cur]

    def group_names(self) -> list[str]:
        """全部组名（按内部顺序）。"""
        return [g.name for g in self.groups]

    def all_windows(self) -> list[PinWindow]:
        """所有组内的全部贴图窗口。"""
        return [w for g in self.groups for w in g.windows]

    def switch_group(self) -> str:
        """循环切换到下一个贴图组。

        Returns:
            切换后的当前组名。
        """
        self._cur = (self._cur + 1) % len(self.groups)
        name = self.current.name
        self._emit_groups(name)
        log.info("切换到贴图组：%s", name)
        return name

    def switch_to(self, name: str) -> None:
        """切换到指定名称的组（组名不存在时不做任何事）。

        Args:
            name: 目标组名。
        """
        for i, g in enumerate(self.groups):
            if g.name == name:
                self._cur = i
                self._emit_groups(name)
                return

    # ---- 组增删改（写入 Groups/names） ----
    def _save_names(self) -> None:
        config.set("Groups/names", "、".join(self.group_names()))
        config.sync()

    def _emit_groups(self, current: str) -> None:
        self.groups_changed.emit(self.group_names(), current)

    def add_group(self, name: str) -> bool:
        """新建组并切到该组。

        Args:
            name: 新组名（自动去首尾空白）。

        Returns:
            是否创建成功；重名或空名返回 False。
        """
        name = (name or "").strip()
        if not name or name in self.group_names():
            return False
        self.groups.append(PinGroup(name))
        self._cur = len(self.groups) - 1
        self._save_names()
        self._emit_groups(name)
        return True

    def rename_group(self, old: str, new: str) -> bool:
        """重命名组，并同步组内贴图窗口记录的组名。

        Args:
            old: 原组名。
            new: 新组名（自动去首尾空白）。

        Returns:
            是否重命名成功；空名或与其他组重名返回 False。
        """
        new = (new or "").strip()
        if not new or (new != old and new in self.group_names()):
            return False
        for g in self.groups:
            if g.name == old:
                g.name = new
                for w in g.windows:
                    w.set_group(new)
                self._save_names()
                self._emit_groups(new)
                return True
        return False

    def remove_group(self, name: str) -> bool:
        """删除组；组里还有贴图时先逐个销毁。

        Args:
            name: 要删除的组名。

        Returns:
            是否删除成功；只剩最后一个组时返回 False。
        """
        if len(self.groups) <= 1:
            return False
        for i, g in enumerate(self.groups):
            if g.name == name:
                for w in list(g.windows):
                    self.delete_pin(w)
                self.groups.pop(i)
                self._cur = min(self._cur, len(self.groups) - 1)
                self._save_names()
                self._emit_groups(self.current.name)
                return True
        return False

    def move_pin(self, win: PinWindow, name: str) -> None:
        """把贴图移入指定组并显示、置顶。

        Args:
            win: 要移动的贴图窗口。
            name: 目标组名（不存在时仅弹提示，保持原组不变）。
        """
        if all(g.name != name for g in self.groups):
            # 菜单开着时组可能被改名/删除：无效目标保持原状，避免状态错位
            self.notify.emit("贴图组", f"组「{name}」不存在")
            return
        for g in self.groups:
            if win in g.windows:
                g.windows.remove(win)
                break
        self.switch_to(name)
        self.current.windows.append(win)
        win.set_group(name)
        win.show()
        win.raise_()

    def _add(self, win: PinWindow, pos: QPointF) -> PinWindow:
        self.current.windows.append(win)
        win.show()
        win.raise_()
        win.activateWindow()
        self._hidden = False
        return win

    # ---- 贴图入口 ----
    def pin_image(self, img: QImage, pos: QPointF,
                  ref_dpr: float | None = None) -> PinWindow | None:
        """贴出一张图。

        Args:
            img: 要贴出的图像。
            pos: 落点（全局逻辑坐标）。
            ref_dpr: 图像内容的参考缩放比（截图产物 = 那次截图所用屏的比例，由
                captured 信号带过来），用于 HiDPI 下把物理像素换算成与截图时所见
                一致的逻辑尺寸；剪贴板/拖入的图不传则用落点屏比例。

        Returns:
            新建的贴图窗口；img 为空图时返回 None。
        """
        if img.isNull():
            return None
        win = PinWindow(img, self, self.current.name, pos, ref_dpr=ref_dpr)
        return self._add(win, pos)

    # ---- 全局动作 ----
    def delete_pin(self, win: PinWindow) -> None:
        """从所属组移除并销毁贴图窗口。

        Args:
            win: 要销毁的贴图窗口。
        """
        for g in self.groups:
            if win in g.windows:
                g.windows.remove(win)
                break
        win.close()
        win.deleteLater()

    def toggle_visible(self) -> bool:
        """全局隐藏/显示所有贴图（没有贴图时仅弹气泡提示）。

        Returns:
            隐藏后为 True，显示后为 False。
        """
        wins = self.all_windows()
        if not wins:
            self.notify.emit("贴图", "当前没有贴图")
            return self._hidden
        self._hidden = not self._hidden
        for w in wins:
            w.hide() if self._hidden else w.show()
        return self._hidden

    def notify_visibility(self) -> None:
        """按窗口实际可见性同步内部隐藏状态（窗口自行关闭/隐藏时调用）。"""
        self._hidden = not any(w.isVisible() for w in self.all_windows())


    def close_all(self) -> None:
        """销毁全部贴图窗口并清空各组窗口列表。"""
        for w in self.all_windows():
            w.close()
            w.deleteLater()
        for g in self.groups:
            g.windows.clear()


def clip_pivot() -> QPointF:
    """计算贴图落点。

    Returns:
        光标位置夹取到虚拟桌面内边距范围内后的坐标。
    """
    import capture

    b = capture.virtual_bounds()
    cur = QCursor.pos()
    return QPointF(
        min(max(cur.x(), b.left() + 60), b.right() - 60),
        min(max(cur.y(), b.top() + 60), b.bottom() - 60),
    )
