"""热键捕获输入框 —— 点击进入捕获态，按下组合键即时提交。"""
from __future__ import annotations

from PySide6.QtCore import QKeyCombination, Qt, Signal
from PySide6.QtGui import QFocusEvent, QKeyEvent, QKeySequence, QMouseEvent
from PySide6.QtWidgets import QLineEdit, QWidget

import hotkey

_MOD_KEYS = {
    Qt.Key_Control, Qt.Key_Shift, Qt.Key_Alt, Qt.Key_Meta,
    Qt.Key_AltGr, Qt.Key_NumLock,
}


class HotkeyEdit(QLineEdit):
    """热键捕获输入框：点击进入捕获态，按下组合键即时提交。"""

    accel_changed = Signal(str)  # 提交后的文本（可能为空串=清除）

    def __init__(self, accel: str = "", parent: QWidget | None = None) -> None:
        """初始化只读的展示态输入框。

        Args:
            accel: 初始加速键文本。
            parent: 父控件。
        """
        super().__init__(accel, parent)
        self.setReadOnly(True)
        self.setAlignment(Qt.AlignCenter)
        self.setFixedWidth(150)
        self.setPlaceholderText("未绑定（点击后按键）")
        self._capturing = False
        self._prev = accel

    def mousePressEvent(self, ev: QMouseEvent) -> None:
        self._prev = self.text()
        self._capturing = True
        self.setText("")
        self.setPlaceholderText("请按下新的快捷键（Esc 取消）")

    def keyPressEvent(self, ev: QKeyEvent) -> None:
        if not self._capturing:
            super().keyPressEvent(ev)
            return
        key = ev.key()
        if key == Qt.Key_Escape:
            self._cancel()
            return
        if key in _MOD_KEYS:
            return  # 只按了修饰键，等主键
        combo = QKeyCombination(ev.modifiers() & ~Qt.KeypadModifier, Qt.Key(key))
        text = QKeySequence(combo.toCombined()).toString(QKeySequence.PortableText)
        text = text.replace("Meta+", "Win+")
        self._capturing = False
        self.setPlaceholderText("未绑定（点击后按键）")
        if hotkey.parse_accel(text) is None:
            self.setText(self._prev)  # 不支持的组合，还原
            return
        self.setText(text)
        self.accel_changed.emit(text)

    def focusOutEvent(self, ev: QFocusEvent) -> None:
        if self._capturing:
            self._cancel()
        super().focusOutEvent(ev)

    def clear_accel(self) -> None:
        """清空绑定并发出空文本的 accel_changed 信号。"""
        self.setText("")
        self.accel_changed.emit("")

    def _cancel(self) -> None:
        self._capturing = False
        self.setPlaceholderText("未绑定（点击后按键）")
        self.setText(self._prev)
