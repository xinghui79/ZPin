#include "startup.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QString>

#include "win32util.hpp"

namespace zpin::startup {
namespace {

const QString kRunKey =
    QStringLiteral("Software/Microsoft/Windows/CurrentVersion/Run");
const QString kApprovedKey =
    QStringLiteral("Software/Microsoft/Windows/CurrentVersion/Explorer/"
                   "StartupApproved/Run");
const QString kValueName = QStringLiteral("ZPin");

// 注册表里存的启动命令：绿色版 exe 就是自身路径，带引号防目录含空格。
// 必须写原生反斜杠路径：资源管理器的登录启动处理拉不起正斜杠命令——静默失败，
// 任务管理器里看一切正常（v1.3.0 的自启「开了却不生效」就是栽在这里）。
QString target() {
    return QStringLiteral("\"%1\"").arg(
        QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
}

// 开关状态比对用：统一小写 + 分隔符归一，只关心指向是否同一个文件，
// 与存的是正斜杠还是反斜杠无关（老版本写下的正斜杠值也能对上）。
QString normcase(const QString& path) {
    QString out = path.toLower();
    out.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return out;
}

}  // namespace

bool isEnabled() {
    const QString stored = win32::regReadString(kRunKey, kValueName);
    if (stored.isEmpty())
        return false;
    return normcase(stored) == normcase(target());
}

void setEnabled(bool on) {
    // Win32 一律走 win32util（AGENTS 规则 5）：失败细节由它落日志。
    if (on) {
        win32::regWriteString(kRunKey, kValueName, target());
        // Windows 8.1+ 的「启动应用」开关有独立的禁用标记（StartupApproved），
        // 光写 Run 键不够——该项被标成禁用时，开机仍不会启动。软件内显式开启
        // 时一并清除该标记，确保真正生效（不碰开机对账，避免与用户在 Windows
        // 设置里的手动关闭打架）。
        win32::regClearStartupApproval(kApprovedKey, kValueName);
    } else {
        win32::regDeleteValue(kRunKey, kValueName);
    }
}

}  // namespace zpin::startup
