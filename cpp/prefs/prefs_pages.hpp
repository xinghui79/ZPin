// 首选项页面部件 —— 绑定配置键的控件 + 各导航页的构建函数。
// 所有改动即时写入 config 并 sync；恢复默认时由对话框整页重建。
// 控件形态按 macOS 系统设置取向：布尔项是开关不是勾选框、数值框配步进器、
// 颜色是圆形色板、下拉自绘尖角。像素与色值的令牌在 prefs_dialog.cpp 的样式表里。
#pragma once

#include <QAbstractButton>
#include <QColor>
#include <QComboBox>
#include <QFontComboBox>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QWidget>

#include <functional>

class QVariantAnimation;

namespace zpin {

class PreferencesDialog;

namespace update {
class UpdateFlow;
}

// QColor -> 文本：带 Alpha 时用 #AARRGGBB，否则用 #RRGGBB 大写。
QString qtColorText(const QColor& c);

// Apple 式开关：胶囊轨道 + 白色圆滑块，开启时轨道填主题色（跟着「主题色」设置走）。
class SwitchButton : public QAbstractButton {
    Q_OBJECT

public:
    explicit SwitchButton(QWidget* parent = nullptr);

    // 直接落位：不发 toggled、不走动画。用于初始状态和「系统拒绝后回滚」——
    // 打开设置页不该把开关集体动画一遍，更不能把程序化赋值当成用户改动写回配置。
    void setCheckedNow(bool on);

protected:
    void paintEvent(QPaintEvent* ev) override;

private:
    QVariantAnimation* m_slide;
    qreal m_pos = 0.0;   // 0=关 1=开，动画期间在两者之间
};

// 开关 <-> 布尔配置键（打开写 True）。标题文字由所在行的左列负责。
class BoundCheck : public SwitchButton {
public:
    explicit BoundCheck(const QString& key, std::function<void(bool)> onChange = {});
};

// 配置键 True=关闭 的反向开关（如 disable_guides）。
class InvertedCheck : public SwitchButton {
public:
    explicit InvertedCheck(const QString& key);
};

// 整数微调框 <-> 整型配置键；下限为负时把下限显示为「默认」。
// 原生上下箭头关掉，由 numberField() 配一个 macOS 式步进器。
class BoundSpin : public QSpinBox {
public:
    BoundSpin(const QString& key, int lo, int hi, const QString& suffix = QString(),
              std::function<void(int)> onChange = {});
};

// 下拉框 <-> 字符串配置键（当前值不在选项里时停在第一项）。values 与 items
// 一一对应时界面显示 items[i]、配置里存 values[i]（如格式选单显示 PNG、存
// "png"）；values 为空则显示即值（log_level 这类键）。
class BoundCombo : public QComboBox {
public:
    BoundCombo(const QString& key, const QStringList& items,
               std::function<void(const QString&)> onChange = {},
               const QStringList& values = {});

protected:
    void paintEvent(QPaintEvent* ev) override;
};

// 字体下拉：不绑配置键（值要拆成字体名+字号），但和 BoundCombo 同一个外观。
class FontPopUp : public QFontComboBox {
public:
    explicit FontPopUp(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* ev) override;
};

// 色块按钮：圆形色板，点击弹自制取色器（遮罩色带 Alpha）。
class ColorButton : public QPushButton {
    Q_OBJECT

public:
    explicit ColorButton(const QString& key);

signals:
    void colorChanged(const QColor& color);

private:
    void pick();
    void syncSwatch(const QColor& c);

    QString m_key;
};

// ---- 各导航页的正文 ----
// dialog 一律必传：它要拿 applyLogLevel / applyFont / registerHotRow / restoreRow。
// 早先这几个签名带 `= nullptr` 再在函数体里 `if (dialog)`，于是「忘了传」不会
// 编译报错，只会安静地长出一个没有「恢复默认」按钮、日志级别与字体改了不生效、
// 热键状态列不刷新的页面——这类症状比崩溃难查得多。去掉兜底。
// 「常规」页含界面外观小节（原「界面」页只有字体与主题色两项，撑不起一栏导航）。
QWidget* buildGeneral(PreferencesDialog* dialog);
QWidget* buildCapture(PreferencesDialog* dialog);
QWidget* buildPin(PreferencesDialog* dialog);
QWidget* buildOutput(PreferencesDialog* dialog);
QWidget* buildControl(PreferencesDialog* dialog);
QWidget* buildAbout(update::UpdateFlow* updateFlow);

}  // namespace zpin
