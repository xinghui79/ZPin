#include "defaults.hpp"

#include <QtGlobal>
#include <QDir>

namespace zpin::defaults {

QString defaultDesktop() {
    const QString oneDrive = qEnvironmentVariable("OneDrive");
    if (!oneDrive.isEmpty()) {
        const QString p = oneDrive + "/Desktop";
        if (QDir(p).exists())
            return p;
    }
    return QDir::homePath() + "/Desktop";
}

const std::vector<Entry>& table() {
    static const std::vector<Entry> kTable = {
        // 常规
        {"General/autostart", false},
        {"General/auto_backup", true},
        {"General/keep_responsive", true},
        {"General/log_level", "普通"},        // 普通=INFO / 详细=DEBUG
        {"General/history_limit", 50},        // 截图历史保留条数
        {"General/history_max_mb", 30},       // 截图历史占用上限（MB），超出丢最旧
        {"General/update_auto_check", false}, // 启动时自动检查更新（默认关，24 小时节流）
        {"General/last_update_check", QVariant::fromValue(qlonglong(0))},  // 上次检查的 epoch ms
        {"General/config_version", CONFIG_VERSION},  // 配置迁移用，跟着 CONFIG_VERSION 走，别手改
        // 界面
        {"Interface/font", QString::fromLatin1(kDefaultFontFamily) + QLatin1Char(',') +
                                QString::number(kDefaultFontSize)},
        {"Interface/theme_color", "#1E78D7"},
        // 截图 · 显示
        {"Capture/border_width", 3},
        {"Capture/mask_color", "#6E000000"},  // Qt 记法 #AARRGGBB：rgba(0,0,0,110)
        {"Capture/show_anchors", true},
        {"Capture/anchor_stroke_color", "#FFFFFF"},
        {"Capture/show_crosshair", false},
        {"Capture/snap_elements", true},      // hover 优先吸附界面元素，关掉只吸整窗
        {"Capture/snap_content", true},       // UIA 认不出元素时按画面找内容块边界
        {"Capture/snap_guides", true},        // 拖拽/缩放时吸到屏幕与窗口的边、中线
        {"Capture/disable_guides", false},
        {"Capture/show_hints", true},
        // 贴图 · 显示
        {"Pin/shadow", true},
        {"Pin/default_opacity", 100},
        {"Pin/max_window_size", 12000},       // 贴图窗口单边上限（逻辑 px），超出自动缩
        {"Pin/border_color", "#1E78D7"},      // 贴图描边：默认蓝色高亮（发光感）
        {"Pin/border_glow", true},            // 描边外再叠一层淡色光晕
        // 输出 · 文件
        {"Output/quality", 95},               // JPEG 保存质量（PNG/BMP 无损不受影响）
        {"Output/name_template", "ZPin_$yyyy-MM-dd_HH-mm-ss$.png"},
        {"Output/remember_ext", true},
        {"Output/last_ext", "png"},
        {"Output/default_ext", ""},           // 默认输出格式 png/jpg/bmp；空=跟随模板后缀
        {"Output/default_dir", defaultDesktop()},
        {"Output/auto_save", false},          // 每次截图自动存一份（写进上面的默认目录）
        // 控制 · 全局快捷键
        // 默认值避开 F1/F3/Alt+L 等常被系统/驱动/其它工具占用的组合
        {"Hotkeys/disabled", false},
        // v3：capture=框选入口（改名「自定义截图」，键位让给全屏截取）
        {"Hotkeys/capture", "Ctrl+Shift+X"},
        {"Hotkeys/capture_full", "Ctrl+Shift+A"},  // 截图：一键截取全屏
        {"Hotkeys/capture_window", "Ctrl+Shift+W"},  // 截图：直接截前台窗口的可见边界
        {"Hotkeys/toggle_pins", "Ctrl+Shift+H"},
    };
    return kTable;
}

QVariant defaultValue(const QString& key) {
    for (const Entry& e : table()) {
        if (key == QLatin1String(e.key))
            return e.value;
    }
    return {};
}

bool nonResettable(const QString& key) {
    // update_auto_check 的控件在「关于」页，而那一页没有恢复默认按钮。它属于
    // General/ 前缀，会被「常规」页的批量重置扫到 —— 提示语明明写着「其它页不受
    // 影响」，却越页把用户的联网开关悄悄关掉。
    return key == "General/config_version" || key == "General/last_update_check" ||
           key == "General/update_auto_check" || key == "Output/last_ext";
}

QString legacyHotkeyOld(const QString& action) {
    // 旧版默认键（实测常被占用）；v3 起 capture 换键。
    if (action == "toggle_pins")
        return "Shift+F3";
    return {};
}

bool legacyEmptyHotkey(const QString& action) {
    return action == "capture";  // 旧版里没给默认键（空串），v2 才补上
}

QString v3CaptureOldDefault() {
    return "Ctrl+Shift+A";
}

std::pair<QString, int> fontTuple(const QString& value) {
    const int comma = value.lastIndexOf(QLatin1Char(','));
    if (comma > 0) {
        bool ok = false;
        const int size = value.mid(comma + 1).trimmed().toInt(&ok);
        if (ok) {
            // 钳进设置页微调框的范围：不钳的话界面显示 6、实际 QFont 用 -3，
            // 又是一处「显示≠生效」（同 BoundSpin 的越界回写）。
            // 家族那半截可能是「A,B,C」这样的字体栈（见 kDefaultFontFamily）：
            // lastIndexOf 取最后一个逗号，尺寸仍在最后一段，前面整段都是家族列表
            return {value.left(comma).trimmed(),
                    qBound(kFontPtMin, size, kFontPtMax)};
        }
    }
    return {QString::fromLatin1(kDefaultFontFamily), kDefaultFontSize};
}

const QStringList& colorPalette() {
    // 色板里的蓝色必须就是 theme_color / Pin/border_color 的出厂默认 #1E78D7：
    // ColorButton::pick 只给「当前色在色板里」的那格画选中描边，选走之后也只在
    // 色板里能选回来。原先放的是 #1E88E5（与它肉眼难分的另一个蓝），结果是
    // 新装用户打开色板看不出当前色，且一旦改色就再也回不到出厂蓝。
    static const QStringList kPalette = {
        "#E53935", "#FB8C00", "#FDD835", "#43A047",
        "#1E78D7", "#8E24AA", "#EC407A", "#00ACC1",
        "#212121", "#7E7E7E", "#FFFFFF", "#5C6BC0",
    };
    return kPalette;
}

const QStringList& maskPalette() {
    static const QStringList kMask = {
        "#28000000", "#4C000000", "#70000000",
        "#99000000", "#BF000000", "#E5000000",
    };
    return kMask;
}

}  // namespace zpin::defaults
