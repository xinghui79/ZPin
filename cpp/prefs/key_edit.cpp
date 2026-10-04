#include "key_edit.hpp"

#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>

#include "hotkey.hpp"

namespace zpin {
namespace {

const char* kIdleTip = "未绑定（点击后按键）";

// 只按下修饰键（或切换类锁定键）不算一次改键——这些键没有可绑定的组合，
// 按下它只是在等主键。要挡的键必须覆盖 hotkey.cpp 的 namedVk()：漏一个
// （早先漏了 ScrollLock）就等于多一个「按一下就注册裸键」的入口。
bool modifierOnly(int key) {
    switch (key) {
    case Qt::Key_Control:
    case Qt::Key_Shift:
    case Qt::Key_Alt:
    case Qt::Key_Meta:
    case Qt::Key_AltGr:
    case Qt::Key_NumLock:
    case Qt::Key_ScrollLock:
    case Qt::Key_CapsLock:
        return true;
    default:
        return false;
    }
}

}  // namespace

HotkeyEdit::HotkeyEdit(const QString& accel, QWidget* parent) : QLineEdit(accel, parent) {
    setReadOnly(true);
    setAlignment(Qt::AlignmentFlag::AlignCenter);
    setFixedWidth(150);
    setPlaceholderText(QString::fromUtf8(kIdleTip));
    m_prev = accel;
}

void HotkeyEdit::mousePressEvent(QMouseEvent* ev) {
    Q_UNUSED(ev)
    // 只在「进入捕获」这一刻存一次回退值。捕获期间 text() 已经被清空了，
    // 若焦点还在同一行时又点一次（误双击），早先的写法会把 m_prev 覆盖成空串，
    // 之后按 Esc 取消就把这一行显示成未绑定——而 config.ini 里其实还是原值，
    // 得关掉重开设置才看得回来。
    if (!m_capturing) {
        m_prev = text();
        setText(QString());
        setPlaceholderText(QStringLiteral("请按下新的快捷键（Esc 取消）"));
    }
    m_capturing = true;
    emit captureStarted();
}

void HotkeyEdit::keyPressEvent(QKeyEvent* ev) {
    if (!m_capturing) {
        QLineEdit::keyPressEvent(ev);
        return;
    }
    if (ev->key() == Qt::Key::Key_Escape) {
        cancel();
        return;
    }
    if (modifierOnly(ev->key()))
        return;  // 只按了修饰键，等主键
    const Qt::KeyboardModifiers mods =
        ev->modifiers() & ~Qt::KeyboardModifier::KeypadModifier;
    QString text = QKeySequence(QKeyCombination(mods, Qt::Key(ev->key())))
                       .toString(QKeySequence::PortableText);
    text.replace(QLatin1String("Meta+"), QLatin1String("Win+"));
    m_capturing = false;
    setPlaceholderText(QString::fromUtf8(kIdleTip));
    if (!hotkey::parseAccel(text)) {
        setText(m_prev);  // 不支持的组合，还原
        emit captureEnded();
        return;
    }
    setText(text);
    emit accelChanged(text);
    emit captureEnded();
}

void HotkeyEdit::focusOutEvent(QFocusEvent* ev) {
    if (m_capturing)
        cancel();
    QLineEdit::focusOutEvent(ev);
}

void HotkeyEdit::clearAccel() {
    setText(QString());
    emit accelChanged(QString());
}

void HotkeyEdit::cancel() {
    if (m_capturing)
        emit captureEnded();
    m_capturing = false;
    setPlaceholderText(QString::fromUtf8(kIdleTip));
    setText(m_prev);
}

}  // namespace zpin
