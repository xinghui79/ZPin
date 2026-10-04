// 托盘 —— 白色紧凑右键菜单 + 全项动作（截图 / 贴图动作 / 打开历史墙 / 热键开关 / 应用）。
// 只做「菜单与气泡」这一件事：所有动作都以信号抛给装配层（app.cpp）。
// 截图历史不在菜单里排条目（看不见内容的菜单项没法挑），只给一个开墙的入口。
#pragma once

#include <QMap>
#include <QString>
#include <QSystemTrayIcon>
#include <QVector>

class QMenu;
class QAction;

namespace zpin {

class TrayController : public QSystemTrayIcon {
    Q_OBJECT

public:
    explicit TrayController(const QMap<QString, QString>& accels, QObject* parent = nullptr);

    // 普通气泡：没有点击行为，会把上一条「可点气泡」的待点击状态清掉。
    void notify(const QString& title, const QString& text);
    // 可点气泡（只有「发现新版本」用）：点击它才发 bubbleClicked。
    void notifyClickable(const QString& title, const QString& text);

    // 热键被修改/恢复默认后，刷新菜单右侧的快捷键提示。
    void setAccels(const QMap<QString, QString>& accels);
    // 停用时托盘图标 Z 两端变黄（app_icon 变体），菜单文字同步。
    void setShortcutsDisabled(bool disabled);
    // 同步「全部贴图 隐藏/显示」状态：菜单文字随状态切换。
    void setPinsHidden(bool hidden);

signals:
    void captureRequested(const QString& mode);   // full / normal
    void togglePinsRequested();
    void restoreClickableRequested();             // 取消全部贴图的鼠标穿透
    void historyWallRequested();                  // 打开截图历史墙（条目动作在墙里）
    void shortcutsDisabledToggled(bool disabled);
    void prefsRequested();
    void restartRequested();
    void bubbleClicked();   // 点了 notifyClickable 那条气泡

private:
    // 「label\taccel」：菜单右侧的快捷键提示列。
    QString textFor(const QString& label, const QString& action) const;
    QAction* regItem(QAction* act, const QString& label, const QString& action);
    void buildMenu();
    void onActivated(QSystemTrayIcon::ActivationReason reason);

    QMenu* m_menu = nullptr;
    QAction* m_actToggle = nullptr;      // 隐藏/显示全部贴图
    QAction* m_actDisable = nullptr;     // 停用/启用全局快捷键
    QMap<QString, QString> m_accels;
    bool m_pinsHidden = false;
    bool m_bubbleClickable = false;   // 最近一条气泡是否响应点击
    struct AccelItem {
        QString label;
        QString action;
        QAction* act;
    };
    QVector<AccelItem> m_accelItems;
};

}  // namespace zpin
