// 原地文本编辑器 —— 无边框输入框：Ctrl+Enter 提交，Esc 取消，失焦提交。
// 框体随文字自适应收缩/长大（贴着内容），避免一大块色块盖住截图。
#pragma once

#include <QColor>
#include <QPlainTextEdit>
#include <QPointF>
#include <QString>

class QFocusEvent;
class QKeyEvent;

namespace zpin {

class TextEditor : public QPlainTextEdit {
    Q_OBJECT

public:
    TextEditor(const QColor& color, int fontPx, QWidget* parent = nullptr);

    void commit();
    void fitSize();
    // 编辑器内的文字相对窗口左上角的偏移（样式表 padding 1px 7px）
    static constexpr int kPadLeft = 7;
    static constexpr int kPadTop = 1;
    // 把窗口摆到「文字左上角 = globalTextTl」：提交后形状从同一位置画字，
    // 打字时和落定后文字不跳位（之前编辑框按窗口左上角对位，文字相对内缩
    // 一个 padding，提交瞬间往左上跳一截，前后看着「框不一致」）
    void placeAt(const QPointF& globalTextTl) {
        move(qRound(globalTextTl.x()) - kPadLeft, qRound(globalTextTl.y()) - kPadTop);
    }

signals:
    void committed(const QString& text);
    void cancelled();

protected:
    void keyPressEvent(QKeyEvent* ev) override;
    void focusOutEvent(QFocusEvent* ev) override;
    // 面板画在 viewport 上：开逐像素 alpha 后样式表的底就不画了，而画在外框上又会被
    // viewport 盖掉（都实测过）
    bool eventFilter(QObject* watched, QEvent* ev) override;

private:
    int m_padW = 14;  // 左右 padding 各 7
    int m_minW = 96;
    int m_maxW = 480;
    QColor m_edge;
    bool m_done = false;  // 已提交/已取消：close() 途中失焦会再触发一次 commit，防重入
};

}  // namespace zpin
