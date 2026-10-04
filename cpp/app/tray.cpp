#include "tray.hpp"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QSignalBlocker>

#include "ui_app_icon.hpp"
#include "win32util.hpp"

namespace zpin {
namespace {

const char* kMenuStyle = R"(
QMenu {
    background: rgba(255, 255, 255, 248);
    color: #202020;
    border: 1px solid rgba(0, 0, 0, 14);
    border-radius: 7px;
    padding: 2px;
    font-size: 11px;
}
QMenu::item { padding: 2px 16px 2px 8px; border-radius: 3px; }
QMenu::item:selected { background: rgba(30, 120, 215, 70); color: #FFFFFF; }
QMenu::separator { height: 1px; background: rgba(0, 0, 0, 10); margin: 2px 5px; }
QMenu::indicator { width: 0px; height: 0px; }
QToolTip { background:#FFFFFF; color:#202020; border:1px solid rgba(0,0,0,14); }
)";

// 等价于 os.path.normcase：Win11 注册表里的 ExecutablePath 用 '\\'，Qt 给的是 '/'。
QString normcase(const QString& path) {
    QString out = path.toLower();
    out.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return out;
}

// Win11 默认把新托盘图标收进 ^ 折叠区；把自己提升为常驻显示。
void promoteVisible() {
    // Win32/注册表一律走 win32util（AGENTS 规则 5），这里只留判断逻辑
    const QString exe = normcase(QCoreApplication::applicationFilePath());
    const QString root = QStringLiteral("Control Panel/NotifyIconSettings");
    for (const QString& sub : win32::regSubKeys(root)) {
        const QString key = root + QLatin1Char('/') + sub;
        if (normcase(win32::regReadString(key, QStringLiteral("ExecutablePath"))) == exe)
            win32::regWriteDword(key, QStringLiteral("IsPromoted"), 1);
    }
}

}  // namespace

TrayController::TrayController(const QMap<QString, QString>& accels, QObject* parent)
    : QSystemTrayIcon(appicon::appIcon(), parent), m_accels(accels) {
    setToolTip(QStringLiteral("ZPin"));

    m_menu = new QMenu;
    m_menu->setAttribute(Qt::WidgetAttribute::WA_TranslucentBackground);
    m_menu->setStyleSheet(QString::fromLatin1(kMenuStyle));

    buildMenu();
    setContextMenu(m_menu);
    connect(this, &QSystemTrayIcon::activated, this, &TrayController::onActivated);
    connect(this, &QSystemTrayIcon::messageClicked, this, [this] {
        if (!m_bubbleClickable)
            return;
        m_bubbleClickable = false;
        emit bubbleClicked();
    });
    show();
    promoteVisible();
}

QString TrayController::textFor(const QString& label, const QString& action) const {
    const QString accel = m_accels.value(action);
    return accel.isEmpty() ? label : QStringLiteral("%1\t%2").arg(label, accel);
}

QAction* TrayController::regItem(QAction* act, const QString& label, const QString& action) {
    m_accelItems.append({label, action, act});
    return act;
}

void TrayController::buildMenu() {
    // 排列原则：高频在前、同类分组——截图 → 贴图 → 历史 → 设置（含快捷键
    // 开关）→ 应用生命周期（重启/退出收尾）。框选是主功能排第一，全屏其次。
    QAction* actCapture = regItem(new QAction(textFor(QStringLiteral("框选截图"),
                                                      QStringLiteral("capture")), m_menu),
                                  QStringLiteral("框选截图"), QStringLiteral("capture"));
    connect(actCapture, &QAction::triggered, this, [this] {
        emit captureRequested(QStringLiteral("normal"));
    });
    m_menu->addAction(actCapture);

    QAction* actFull = regItem(new QAction(textFor(QStringLiteral("全屏截图"),
                                                   QStringLiteral("capture_full")), m_menu),
                               QStringLiteral("全屏截图"), QStringLiteral("capture_full"));
    connect(actFull, &QAction::triggered, this, [this] {
        emit captureRequested(QStringLiteral("full"));
    });
    m_menu->addAction(actFull);

    m_menu->addSeparator();

    // ---- 贴图（不再有「贴图管理」子菜单：分组概念已整体去掉；「贴剪贴板图片」也
    //      按用户要求删除。剩下的两个动作直接平铺在根菜单里，少一层查找）----
    m_actToggle = new QAction(m_menu);
    connect(m_actToggle, &QAction::triggered, this, &TrayController::togglePinsRequested);
    m_menu->addAction(m_actToggle);
    setPinsHidden(false);

    QAction* actRestore = new QAction(QStringLiteral("恢复贴图可点击"), m_menu);
    connect(actRestore, &QAction::triggered, this, &TrayController::restoreClickableRequested);
    m_menu->addAction(actRestore);

    m_menu->addSeparator();

    // ---- 截图历史（打开缩略图墙，不再在菜单里排条目：看不见内容的菜单项没法挑）----
    QAction* actHistory = new QAction(QStringLiteral("截图历史"), m_menu);
    connect(actHistory, &QAction::triggered, this, &TrayController::historyWallRequested);
    m_menu->addAction(actHistory);
    m_menu->addSeparator();

    // ---- 设置与快捷键开关（同为配置类，放一起）----
    QAction* actPrefs = new QAction(QStringLiteral("设置"), m_menu);
    connect(actPrefs, &QAction::triggered, this, &TrayController::prefsRequested);
    m_menu->addAction(actPrefs);

    m_actDisable = new QAction(QStringLiteral("停用全局快捷键"), m_menu);
    m_actDisable->setCheckable(true);
    connect(m_actDisable, &QAction::toggled, this, [this](bool on) {
        m_actDisable->setText(on ? QStringLiteral("启用全局快捷键")
                                 : QStringLiteral("停用全局快捷键"));
        emit shortcutsDisabledToggled(on);
    });
    m_menu->addAction(m_actDisable);
    m_menu->addSeparator();

    // ---- 应用（低频与收尾放最末）----
    QAction* actRestart = new QAction(QStringLiteral("重新启动"), m_menu);
    connect(actRestart, &QAction::triggered, this, &TrayController::restartRequested);
    m_menu->addAction(actRestart);

    QAction* actQuit = new QAction(QStringLiteral("退出"), m_menu);
    connect(actQuit, &QAction::triggered, qApp, &QApplication::quit);
    m_menu->addAction(actQuit);
}

void TrayController::notify(const QString& title, const QString& text) {
    // 普通气泡不带点击行为：把上一条「可点气泡」的状态清掉，否则用户点了条
    // 「已保存」也会跳到 设置 → 关于（早先就是这个串了台的）
    m_bubbleClickable = false;
    showMessage(title, text, QSystemTrayIcon::MessageIcon::Information, 5000);
}

void TrayController::notifyClickable(const QString& title, const QString& text) {
    m_bubbleClickable = true;
    showMessage(title, text, QSystemTrayIcon::MessageIcon::Information, 5000);
}

void TrayController::setAccels(const QMap<QString, QString>& accels) {
    m_accels = accels;
    for (const AccelItem& item : m_accelItems)
        item.act->setText(textFor(item.label, item.action));
    // 「隐藏/显示全部贴图」文本含显隐状态 + 快捷键，随新键重刷
    setPinsHidden(m_pinsHidden);
}

void TrayController::setShortcutsDisabled(bool disabled) {
    setIcon(appicon::appIcon(disabled));
    // 这里是「程序同步外观」，不是用户操作：不挡住 toggled 就会反向 emit
    // shortcutsDisabledToggled，App::setShortcutsDisabled 再调回本函数，绕一圈
    // 回来把上面两步重做一遍。今天那几步恰好幂等所以看不出来，但它是颗雷。
    const QSignalBlocker blocker(m_actDisable);
    m_actDisable->setChecked(disabled);
    m_actDisable->setText(disabled ? QStringLiteral("启用全局快捷键")
                                   : QStringLiteral("停用全局快捷键"));
}

void TrayController::setPinsHidden(bool hidden) {
    m_pinsHidden = hidden;
    m_actToggle->setText(textFor(hidden ? QStringLiteral("显示全部贴图")
                                        : QStringLiteral("隐藏全部贴图"),
                                 QStringLiteral("toggle_pins")));
}

void TrayController::onActivated(QSystemTrayIcon::ActivationReason reason) {
    // 只认单击。托盘窗口带 CS_DBLCLKS，双击会先后发 Trigger 和 DoubleClick，
    // 两个都放行就是连发两次截图请求——第二发会把第一发的选区会话取消重开，
    // 屏幕闪一下、会话状态被重置。
    if (reason == QSystemTrayIcon::ActivationReason::Trigger)
        emit captureRequested(QStringLiteral("normal"));
}

}  // namespace zpin
