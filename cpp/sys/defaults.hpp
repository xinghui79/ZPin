// ZPin 默认配置 —— 所有键与默认值的唯一事实来源。
// 首选项界面、恢复默认、config 的类型转换都以这里的键为准。
// 热键值为显示文本（如 "Ctrl+Shift+X"），空串表示未绑定。
#pragma once

#include <QString>
#include <QVariant>
#include <vector>

namespace zpin::defaults {

// 返回当前用户的桌面目录：优先 OneDrive 重定向的 Desktop，否则主目录下的
// Desktop（即使不存在也返回该路径）。
QString defaultDesktop();

struct Entry {
    const char* key;
    QVariant value;
};

// 全部默认键（有序；恢复默认与首次落盘都按此遍历）。
const std::vector<Entry>& table();

// 按键取默认值；键不存在时返回无效 QVariant（不应发生）。
QVariant defaultValue(const QString& key);

inline constexpr const char* VERSION = "1.3.0";   // 软件版本号（关于页/README 的唯一事实来源）
inline constexpr int CONFIG_VERSION = 11;         // v11：截图历史默认档位 50 条 / 30 MB

// 界面字体默认值：table() 落盘的字符串与 fontTuple() 解析失败时的兜底共用这一份，
// 免得两处各写各的（改了一处，另一处还发旧字体）。
// 默认字体走**系统 UI 字体栈**而不是单个中文字体：Qt 逐字符挑第一个有该字形的
// 家族，所以拉丁与数字落到 Segoe UI Variable（Windows 上最接近 SF Pro 的字形），
// 中文自动落到 Microsoft YaHei UI。早先默认直接写「Microsoft YaHei UI」，
// 拉丁字形是那套中文字体自带的，跟 Apple 的系统观感差一截。
// 语义仍然与设置页的字体下拉一致：**用户在下拉里选过的就是单一家族、独占生效**，
// 这里的栈只在配置仍是默认值时起作用。
inline constexpr const char* kDefaultFontFamily =
    "Segoe UI Variable Text,Segoe UI,Microsoft YaHei UI";
inline constexpr int kDefaultFontSize = 9;
// 字号微调框范围（设置 → 常规）。控件范围与 fontTuple 的手改 ini 钳制共用。
inline constexpr int kFontPtMin = 6, kFontPtMax = 24;

// 首选项里两个 spin 的取值范围。控件范围与运行期 clamp 共用同一份：早先两边
// 各写各的，改了一边另一边还按旧范围裁，表现是「界面上调到 120 却只到 100」
// 这种查起来很费劲的偏差。
inline constexpr int kOpacityMin = 10, kOpacityMax = 100;
inline constexpr int kMaxWindowMin = 500, kMaxWindowMax = 30000;

// 自动更新的数据源：GitHub Releases。tag 规则 vX.Y.Z（或 X.Y.Z），
// 发布资产里取第一个 .zip 当安装包（build.bat zip 打出来的布局）。
// 仓库地址也放这里：关于页要显示「源代码」链接，同一个地址散在三个文件里
// 换名/转私有时必漏一处，而这里已经是自动更新 API 的唯一出处。
inline constexpr const char* REPO_URL = "https://github.com/xinghui79/ZPin";
inline constexpr const char* UPDATE_API_URL =
    "https://api.github.com/repos/xinghui79/ZPin/releases/latest";
inline constexpr const char* UPDATE_PAGE_URL =
    "https://github.com/xinghui79/ZPin/releases/latest";

// table() 里少数键不是「设置」而是内部状态或用户数据，按页恢复默认时必须跳过：
// config_version 是迁移标记、last_ext 记住上次格式、last_update_check 是自动更新
// 的节流时间戳。
bool nonResettable(const QString& key);

// 旧版默认键（实测在多数机器上被系统/驱动/其它工具占用）：若用户从未改过
// （值仍等于旧默认），启动时自动换成新默认。
// v3：capture 旧默认 Ctrl+Shift+A 让给新增的全屏截取，框选入口改到 Ctrl+Shift+X。
QString legacyHotkeyOld(const QString& action);   // 有旧默认键的动作返回旧值，否则空
bool legacyEmptyHotkey(const QString& action);    // 旧版没给默认键（空串）的动作
QString v3CaptureOldDefault();                    // "Ctrl+Shift+A"

// 把 Interface/font 的文本解析为 (字体名, 字号)；非法时回退默认字体。
std::pair<QString, int> fontTuple(const QString& value);

// 预设色板：工具栏的取色弹层与设置页的颜色按钮共用这一份，两边看到的
// 「固定几种颜色」必须一致（自定义取色对话框已按用户要求删掉）。
const QStringList& colorPalette();

// 遮罩色只有半透明才起作用（选区外要压暗而不是糊死），所以它单独一套：
// 黑色 6 档浓度，#AARRGGBB 记法与 config.ini 里存的值同格式。
const QStringList& maskPalette();

}  // namespace zpin::defaults