#include "startup.hpp"

#include <QCoreApplication>
#include <QString>

#include "win32util.hpp"

namespace zpin::startup {
namespace {

const QString kRunKey =
    QStringLiteral("Software/Microsoft/Windows/CurrentVersion/Run");
const QString kValueName = QStringLiteral("ZPin");

// 注册表里存的启动命令：绿色版 exe 就是自身路径，带引号防目录含空格。
QString target() {
    return QStringLiteral("\"%1\"").arg(QCoreApplication::applicationFilePath());
}

// 等价于 os.path.normcase：统一小写 + 反斜杠转正斜杠（Qt 的路径用 '/'，注册表用 '\\'）。
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
    if (on)
        win32::regWriteString(kRunKey, kValueName, target());
    else
        win32::regDeleteValue(kRunKey, kValueName);
}

}  // namespace zpin::startup
