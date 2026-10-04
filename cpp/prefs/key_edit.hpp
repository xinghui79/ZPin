// 热键捕获输入框 —— 点击进入捕获态，按下组合键即时提交。
#pragma once

#include <QLineEdit>
#include <QString>

class QFocusEvent;
class QKeyEvent;
class QMouseEvent;

namespace zpin {

class HotkeyEdit : public QLineEdit {
    Q_OBJECT

public:
    explicit HotkeyEdit(const QString& accel = QString(), QWidget* parent = nullptr);

    // 清空绑定并发出空文本的 accelChanged。
    void clearAccel();

signals:
    void accelChanged(const QString& text);
    // 捕获开始/结束：设置页据此挂起/恢复全局热键——否则用户想绑的组合键
    // 若已被本程序占用，按键会被 WM_HOTKEY 吞掉，输入框毫无反应
    void captureStarted();
    void captureEnded();

protected:
    void mousePressEvent(QMouseEvent* ev) override;
    void keyPressEvent(QKeyEvent* ev) override;
    void focusOutEvent(QFocusEvent* ev) override;

private:
    void cancel();

    bool m_capturing = false;
    QString m_prev;
};

}  // namespace zpin
