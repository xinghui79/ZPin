// 配置层 —— QSettings(IniFormat) 读写 %LOCALAPPDATA%\ZPin\config.ini。
// get/set 均以 defaults 的键与类型为准；启动时滚动备份一份 config.ini.bak。
#pragma once

#include <QString>
#include <QVariant>

class QSettings;

namespace zpin::config {

inline constexpr const char* kAppName = "ZPin";

// 返回应用数据目录 %LOCALAPPDATA%\ZPin（无 LOCALAPPDATA 时回退主目录）。
QString appDir();

// 配置文件完整路径。
QString configPath();

// 初始化全局 QSettings 并完成启动期的配置整理：建目录、滚动备份、
// 默认值落盘、版本迁移。必须在任何 get/set 之前调用一次。
void init();

// 按配置键读取当前值；类型与该键的默认值一致，键不存在时返回默认值。
QVariant get(const QString& key);

// 便捷读取：按调用方给出的类型取值（用于内部状态键）。
bool getBool(const QString& key);
int getInt(const QString& key);
QString getStr(const QString& key);

// 写入单个配置键（不立即落盘，需另行 sync）。
void set(const QString& key, const QVariant& value);

// 把未落盘的配置立即写回 config.ini。
void sync();

// 读取指定动作的全局热键文本；空串表示未绑定。
QString hotkeyAccel(const QString& action);

// 全局 QSettings（init 之后才可用）。
QSettings* settings();

}  // namespace zpin::config
