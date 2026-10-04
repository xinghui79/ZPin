// 首选项对话框 —— 侧边栏导航 + 固定尺寸内容区（每页大小完全一致）。
// 导航 7 项：常规（含界面外观）/ 截图 / 贴图 / 输出 / 快捷键 / 帮助 / 关于。
// 对话框不自己碰热键/字体/日志/自启/更新，全部经构造传入的回调与
// UpdateFlow 交给装配层（app.cpp）。
#pragma once

#include <QDialog>
#include <QMap>
#include <QLabel>
#include <QSet>
#include <QString>

#include <functional>

class QBoxLayout;
class QListWidget;
class QShowEvent;
class QStackedWidget;

namespace zpin {

class HotkeyEdit;

namespace update {
class UpdateFlow;
}

class PreferencesDialog : public QDialog {
    Q_OBJECT

public:
    // applyHotkeys 按当前配置全量重绑，返回 {动作: 是否注册成功}。
    using ApplyHotkeys = std::function<QMap<QString, bool>()>;

    PreferencesDialog(ApplyHotkeys applyHotkeys, std::function<void()> applyFont,
                      std::function<void()> applyLogLevel,
                      std::function<void(bool)> setHotkeysDisabled,
                      std::function<void(bool)> setHotkeysSuspended,
                      update::UpdateFlow* updateFlow, QWidget* parent = nullptr);

    // 定位到帮助 / 关于页（托盘入口与更新气泡用）。
    void showHelpPage();
    void showAboutPage();

    // 登记一行热键编辑框与状态标签，并接上变更回调。
    void registerHotRow(const QString& action, HotkeyEdit* edit, QLabel* status);
    // 切换热键总开关并刷新各行状态显示。
    void setHotkeysDisabled(bool disabled);
    void applyLogLevel();
    void applyFont();
    // 主题色改了：重算设置页样式表。开关是自绘的、paintEvent 现读配置，
    // 但侧栏选中块与按钮走样式表，不重设就会停在旧色（同一页两种蓝）。
    void refreshAccent();

    // 只恢复指定前缀的设置键（如 {"Capture/"}），其它页不受影响；
    // 内部状态/用户数据键（NON_RESETTABLE）永不覆盖。
    // 副作用（自启注册表键、字体、日志级别、热键）也只重放本页覆盖到的那几项。
    void restorePage(const QString& title, const QStringList& prefixes);

    // 按当前实际注册结果刷新每行状态（打开对话框时也走一次）。
    // results 传调用方刚拿到的重绑结果可省一次全量重绑；留空则按需自己重绑。
    void refreshHotkeyStatus(const QMap<QString, bool>& results = {});

protected:
    void showEvent(QShowEvent* ev) override;

private:
    // 重建侧栏与全部页面；整块替换 body，避免旧控件叠加。
    void build();
    // 移到光标所在屏的可用区域居中（本窗无父窗，Qt 默认定位不居中）
    void centerOnCursorScreen();
    QMap<QString, bool> applyHotkeys();
    void onAccelChanged(const QString& action);
    QSet<QString> validateHotkeys();
    void syncSideEffects(const QStringList& prefixes);

    ApplyHotkeys m_applyHotkeys;
    std::function<void()> m_applyFont;
    std::function<void()> m_applyLogLevel;
    std::function<void(bool)> m_setHotkeysDisabled;
    std::function<void(bool)> m_setHotkeysSuspended;

    QBoxLayout* m_lay = nullptr;
    QWidget* m_body = nullptr;
    QListWidget* m_nav = nullptr;
    QStackedWidget* m_stack = nullptr;
    int m_helpRow = 0;
    int m_aboutRow = 0;
    update::UpdateFlow* m_updateFlow = nullptr;   // 属 App，活得比本对话框久
    QMap<QString, HotkeyEdit*> m_edits;
    QMap<QString, QLabel*> m_status;
};

}  // namespace zpin
