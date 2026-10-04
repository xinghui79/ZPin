// 日志 —— 文件(%LOCALAPPDATA%\ZPin\zpin.log) + stderr 双写，带时间戳与级别。
// qInstallMessageHandler 兜住 qWarning/qCritical；未捕获异常由 main 落盘。
#pragma once

#include <QString>

namespace zpin::log {

// 打开日志文件并接管 Qt 消息输出；debug=true 时 DEBUG 级也落盘。
void init(bool debug);

// 只切详细级别（首选项改「日志级别」时用），不重新接管输出。
void setDebug(bool debug);

void debug(const char* category, const QString& text);
void info(const char* category, const QString& text);
void warn(const char* category, const QString& text);
void error(const char* category, const QString& text);

}  // namespace zpin::log
