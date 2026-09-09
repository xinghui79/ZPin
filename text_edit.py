"""原地文本编辑器 —— 无边框输入框：Enter/Ctrl+Enter 提交，Esc 取消，失焦提交。

框体随文字自适应收缩/长大（贴着内容），样式为细实线小圆角，避免一大块色块盖住截图。
"""
from __future__ import annotations

import math

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QColor, QFocusEvent, QKeyEvent
from PySide6.QtWidgets import QApplication, QPlainTextEdit, QWidget


class TextEditor(QPlainTextEdit):
    """截图选区上的原地文本输入框：无边框置顶小窗，框体随文字自适应。

    Args:
        color: 文字与边框主色。
        font_px: 字号（逻辑像素）。
        parent: Qt 父控件。
    """

    committed = Signal(str)
    cancelled = Signal()

    def __init__(self, color: QColor, font_px: int, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setWindowFlags(Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool)
        self.setPlaceholderText("输入文字")
        self.setVerticalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.document().setDocumentMargin(0)
        font = self.font()
        font.setPixelSize(font_px)
        self.setFont(font)
        edge = QColor(color)
        edge.setAlpha(150)
        self.setStyleSheet(
            f"QPlainTextEdit {{ background: rgba(255,255,255,32); color: {color.name()};"
            f" border: 1px solid rgba({edge.red()},{edge.green()},{edge.blue()},{edge.alpha()});"
            f" border-radius: 5px; padding: 1px 6px;"
            f" selection-background-color: rgba({edge.red()},{edge.green()},{edge.blue()},70); }}"
        )
        self._pad_w = 14   # 左右 padding 12 + 边框 2
        self._min_w = max(96, self.fontMetrics().horizontalAdvance(self.placeholderText())
                          + self._pad_w)
        scr = QApplication.primaryScreen()
        self._max_w = max(240, int(scr.availableGeometry().width() * 0.6)) if scr else 480
        self.textChanged.connect(self._fit_size)
        self._fit_size()

    def _fit_size(self) -> None:
        """随文字自适应调整框体：宽度夹在 [_min_w, _max_w]，高度按可视行数估算。"""
        fm = self.fontMetrics()
        raw = self.toPlainText().split("\n")   # 保留空行：渲染时同样占一行高度
        widths = [fm.horizontalAdvance(l) for l in raw if l]
        if not widths:
            widths = [fm.horizontalAdvance(self.placeholderText())]
        w = min(max(max(widths) + self._pad_w, self._min_w), self._max_w)
        # 可视行数按度量估算：document().size() 在窗口未显示时不可靠
        avail = max(1.0, w - self._pad_w)
        n = sum(max(1, math.ceil(fm.horizontalAdvance(l) / avail)) for l in raw)
        self.resize(w, fm.height() * n + 6)   # 上下 padding + 边框 + 光标余量

    def keyPressEvent(self, ev: QKeyEvent) -> None:
        if ev.key() == Qt.Key_Escape:
            self.cancelled.emit()
            self.close()
            return
        if ev.key() in (Qt.Key_Return, Qt.Key_Enter) and ev.modifiers() & Qt.ControlModifier:
            self._commit()
            return
        super().keyPressEvent(ev)

    def _commit(self) -> None:
        text = self.toPlainText().rstrip("\n")
        self.committed.emit(text)
        self.close()

    def focusOutEvent(self, ev: QFocusEvent) -> None:
        super().focusOutEvent(ev)
        self._commit()
