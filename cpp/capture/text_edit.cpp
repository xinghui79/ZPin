#include "text_edit.hpp"

#include <QApplication>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QPainter>
#include <QScreen>
#include <QTextDocument>
#include <QtGlobal>
#include <QtMath>
#include <algorithm>

namespace zpin {

namespace {
/// 面板圆角半径（逻辑像素）
constexpr double kRadius = 5.0;
/// 面板底色：半透明白，压在截图上要读得出黑字，又不至于完全挡住底下内容
const QColor kFill(255, 255, 255, 32);
}  // namespace

TextEditor::TextEditor(const QColor& color, int fontPx, QWidget* parent)
    : QPlainTextEdit(parent) {
    setWindowFlags(Qt::WindowType::FramelessWindowHint |
                   Qt::WindowType::WindowStaysOnTopHint | Qt::WindowType::Tool);
    // 无边框顶层窗默认是矩形表面：样式表的 border-radius 只把圆角"描"出来，圆角外那
    // 四个角没人画，于是留下不透明的脏角 —— 实测那四个角是 a=46 的浅灰，这就是
    // "边缘粗糙"的来源。开逐像素 alpha 后它们才真是 a=0。
    setAttribute(Qt::WidgetAttribute::WA_TranslucentBackground);
    viewport()->setAttribute(Qt::WidgetAttribute::WA_TranslucentBackground);
    setPlaceholderText("输入文字");
    setVerticalScrollBarPolicy(Qt::ScrollBarPolicy::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarPolicy::ScrollBarAlwaysOff);
    document()->setDocumentMargin(0);
    QFont f = font();
    f.setPixelSize(fontPx);
    setFont(f);
    m_edge = color;
    m_edge.setAlpha(150);
    // 底和边线改由 eventFilter 画：开了逐像素 alpha 之后样式表的 background 就不
    // 参与了（实测三种写法都拿不到底），而画在外框上又会被 viewport 整块盖掉。
    setStyleSheet(
        QString("QPlainTextEdit { background: transparent; border: none; color: %1;"
                " padding: 1px 7px; selection-background-color: rgba(%2,%3,%4,70); }")
            .arg(color.name())
            .arg(m_edge.red())
            .arg(m_edge.green())
            .arg(m_edge.blue()));
    viewport()->installEventFilter(this);
    m_minW = qMax(96, fontMetrics().horizontalAdvance(placeholderText()) + m_padW);
    if (const QScreen* scr = QApplication::primaryScreen())
        m_maxW = qMax(240, int(scr->availableGeometry().width() * 0.6));
    connect(this, &QPlainTextEdit::textChanged, this, &TextEditor::fitSize);
    fitSize();
}

bool TextEditor::eventFilter(QObject* watched, QEvent* ev) {
    if (watched == viewport() && ev->type() == QEvent::Type::Paint) {
        QPainter p(viewport());
        p.setRenderHint(QPainter::RenderHint::Antialiasing);
        // 边线按设备像素取半格居中：1 物理像素的线在 125%/150% 下不会糊成两格
        const double hair = 1.0 / viewport()->devicePixelRatioF();
        p.setPen(QPen(m_edge, hair));
        p.setBrush(kFill);
        p.drawRoundedRect(QRectF(hair * 0.5, hair * 0.5, viewport()->width() - hair,
                                 viewport()->height() - hair),
                          kRadius, kRadius);
    }
    return QPlainTextEdit::eventFilter(watched, ev);
}

void TextEditor::fitSize() {
    // 随文字自适应调整框体：宽度夹在 [m_minW, m_maxW]，高度按可视行数估算
    const QFontMetrics fm = fontMetrics();
    const QStringList raw = toPlainText().split(QLatin1Char('\n'));  // 保留空行
    QList<int> widths;
    for (const QString& l : raw) {
        if (!l.isEmpty())
            widths.append(fm.horizontalAdvance(l));
    }
    if (widths.isEmpty())
        widths.append(fm.horizontalAdvance(placeholderText()));
    const int w = qBound(m_minW, *std::max_element(widths.cbegin(), widths.cend()) + m_padW,
                         m_maxW);
    // 可视行数按度量估算：document()->size() 在窗口未显示时不可靠
    const double avail = qMax(1.0, double(w - m_padW));
    int lines = 0;
    for (const QString& l : raw) {
        lines += qMax(1, int(qCeil(fm.horizontalAdvance(l) / avail)));
    }
    resize(w, fm.height() * lines + 6);  // 上下 padding + 边框 + 光标余量
}

void TextEditor::keyPressEvent(QKeyEvent* ev) {
    if (ev->key() == Qt::Key_Escape) {
        m_done = true;  // 取消后失焦不许再走 commit
        emit cancelled();
        close();
        return;
    }
    if ((ev->key() == Qt::Key_Return || ev->key() == Qt::Key_Enter) &&
        ev->modifiers() & Qt::ControlModifier) {
        commit();
        return;
    }
    QPlainTextEdit::keyPressEvent(ev);
}

void TextEditor::commit() {
    if (m_done)
        return;  // 已提交过：close() 途中的失焦会再次进来，重入会重复入栈
    m_done = true;
    QString text = toPlainText();
    while (text.endsWith(QLatin1Char('\n')))
        text.chop(1);
    emit committed(text);
    close();
}

void TextEditor::focusOutEvent(QFocusEvent* ev) {
    QPlainTextEdit::focusOutEvent(ev);
    commit();
}

}  // namespace zpin
