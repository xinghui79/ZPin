#include "config.hpp"

#include <QDir>
#include <QFile>
#include <QtGlobal>
#include <QSettings>

#include "defaults.hpp"
#include "logging.hpp"
#include "win32util.hpp"

namespace zpin::config {

static QSettings* g_settings = nullptr;

namespace {
void migrate(QSettings& s, int from);
void backupConfig(const QString& ini);
}

QString appDir() {
    QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (base.isEmpty())
        base = QDir::homePath();
    return base + "/" + kAppName;
}

QString configPath() {
    return appDir() + "/config.ini";
}

void init() {
    QDir().mkpath(appDir());
    const QString ini = configPath();
    g_settings = new QSettings(ini, QSettings::IniFormat);
    // 备份必须在任何 setValue/sync 之前做，才能留下上一轮的原始配置；
    // auto_backup 走 getBool() 读用户已存的值，硬编码默认会让人关不掉备份
    if (getBool("General/auto_backup") && QFile::exists(ini))
        backupConfig(ini);
    // 迁移在补默认值**之后**跑，但要求只有一条：storedVersion 必须在补默认值
    // 之前读出来。补默认值会把所有缺失键写进 ini，那时再读版本号，新装的配置
    // （本来就一个键都没有、刚被补全）会被误判成「已经迁移过」而跳过全部步骤。
    // 至于迁移本身在补默认值之后跑，是有意的：老配置里 capture 这类键是
    // 「存在但值为空串」，补默认值的 contains() 判断会跳过它（key 在），正好
    // 留给下面的 v2 迁移去填。先补后迁的顺序不能对调。
    const int storedVersion = g_settings->value("General/config_version", 0).toInt();
    // 把全部默认值落盘，config.ini 首次运行即存在且用户可见可改
    for (const auto& entry : defaults::table()) {
        if (!g_settings->contains(QLatin1String(entry.key)))
            g_settings->setValue(QLatin1String(entry.key), entry.value);
    }
    if (storedVersion < defaults::CONFIG_VERSION)
        migrate(*g_settings, storedVersion);
    g_settings->setValue("General/config_version", defaults::CONFIG_VERSION);
    g_settings->sync();
}

namespace {

// 滚动备份：先写临时文件，写成了才换掉上一份 .bak。
// 直接「remove 旧的 → copy 新的」在 copy 失败时（.bak 被杀软占着、磁盘满、
// 只读权限）会连上一份好备份一起丢掉，结果是用户既没有新备份、也没有旧
// 备份，而且全程无声——配置被迁移改坏时连回退的余地都没有。
void backupConfig(const QString& ini) {
    const QString bak = ini + ".bak";
    const QString tmp = bak + ".tmp";
    QFile::remove(tmp);   // 上次没走完的残留
    if (!QFile::copy(ini, tmp)) {
        log::warn("config", QString("备份配置失败（%1）：%2")
                                 .arg(tmp, QFile(tmp).errorString()));
        return;
    }
    // 原子换装（MoveFileEx REPLACE_EXISTING）：不能先 remove 旧的再改名——
    // 改名一旦失败（杀软占着 .bak 等）上一份好备份已经没了，注释声称的
    // 保护就成了空话。失败时 .tmp 里还有新鲜副本。
    if (!win32::replaceFile(tmp, bak))
        log::warn("config", QString("备份改名失败（%1 -> %2）：%3")
                                 .arg(tmp, bak, QFile(bak).errorString()));
}

// 一次性迁移。**每一步都必须带 from < N 的版本门槛**：只判断「值等于旧默认」
// 分不清「从没改过」和「用户故意改回去」——跨版本升级（如 v7 → v8）时没有
// 门槛的步骤会全部重放，把用户故意改回旧默认的设置静默改掉。N = 该步骤
// 引入时的 CONFIG_VERSION。
void migrate(QSettings& s, int from) {
    const auto fixHotkey = [&](const QString& action, const QString& legacy) {
        const QString key = "Hotkeys/" + action;
        if (s.value(key).toString() == legacy) {
            const QString nv = defaults::defaultValue(key).toString();
            s.setValue(key, nv);
            log::info("config", QString("热键 %1：旧默认 %2 常被占用，改为 %3").arg(action, legacy, nv));
        }
    };
    // v2：把有问题的旧默认值启动时换成新默认（只动用户从没改过的项）。
    if (from < 2) {
        for (const char* action : {"toggle_pins"})
            if (const QString legacy = defaults::legacyHotkeyOld(QLatin1String(action)); !legacy.isEmpty())
                fixHotkey(QLatin1String(action), legacy);
        // 旧版里 capture 根本没给默认键（空串），v2 才补上
        if (defaults::legacyEmptyHotkey("capture")) {
            const QString key = "Hotkeys/capture";
            if (s.value(key).toString().trimmed().isEmpty()) {
                const QString nv = defaults::defaultValue(key).toString();
                s.setValue(key, nv);
                log::info("config", QString("热键 capture：补充默认键 %1").arg(nv));
            }
        }
    }
    // v3：capture 的 Ctrl+Shift+A 让位给新增的全屏截取（只动仍是旧默认的值）
    if (from < 3)
        fixHotkey("capture", defaults::v3CaptureOldDefault());
    // v4：贴图描边默认从白色改为品牌蓝
    if (from < 4 && s.value("Pin/border_color").toString() == "#FFFFFF") {
        s.setValue("Pin/border_color", defaults::defaultValue("Pin/border_color"));
        log::info("config", "贴图描边：v4 起默认改为蓝色高亮");
    }
    // v5：JPEG 质量旧默认 -1 实际等于 75（截图文字发糊），改成 95。
    // 兜底值取 defaults 而不是再写一遍 95：defaults 才是唯一事实来源
    if (from < 5 &&
        s.value("Output/quality", defaults::defaultValue("Output/quality")).toInt() < 0) {
        s.setValue("Output/quality", defaults::defaultValue("Output/quality").toInt());
        log::info("config", "JPEG 质量：v5 起默认 95");
    }
    // v6：按用户要求关掉「启动时自动检查更新」。在 from < 6 的前提下**故意
    // 不看用户是否改过**——旧默认是 true，而 table() 只在键缺失时落盘，所以
    // 那时 config.ini 里躺着的 true 只会是「一直保持默认」的产物（设置页里
    // 关着时存的 false 不会被误伤）。v6 之后用户重新勾开的不再翻回去。
    if (from < 6 &&
        s.value("General/update_auto_check",
                defaults::defaultValue("General/update_auto_check")).toBool()) {
        s.setValue("General/update_auto_check", false);
        log::info("config", "自动检查更新：v6 起默认关闭（可在 设置 → 关于 手动开启）");
    }
    // v7：贴图分组整体去掉（托盘「贴图管理」子菜单、切组快捷键、组名列表）。
    // 老配置里这两个键再没人读，留着就是 config.ini 里两行永远解释不通的残留。
    if (from < 7 && (s.contains("Groups/names") || s.contains("Hotkeys/switch_group"))) {
        s.remove("Groups/names");
        s.remove("Hotkeys/switch_group");
        log::info("config", "v7：已移除贴图分组相关配置（Groups/names、Hotkeys/switch_group）");
    }
    // v8：托盘「贴剪贴板图片」按用户要求整体删除（菜单项、动作、热键），老配置里
    // 这个键不会再被注册，留着只会让 设置 → 快捷键 与 ini 对不上。
    if (from < 8 && s.contains("Hotkeys/pin_clipboard")) {
        s.remove("Hotkeys/pin_clipboard");
        log::info("config", "v8：已移除「贴剪贴板图片」的热键配置（Hotkeys/pin_clipboard）");
    }
    // v9：清掉 PyQt6 时代遗留、C++ 版已无任何实现的配置键。这些键不在 table() 里，
    // 所以「补默认值」不碰它们、「恢复默认」按前缀遍历时也扫不到它们 —— 唯一能
    // 收掉它们的地方就是这里。留着的具体危害：用户手改 ini 会撞上「改了没反应」，
    // 而 设置 → 快捷键 只有 4 行、ini 里却躺着 7 行热键，看不出哪个是活的。
    if (from < 9) {
        static const char* kStale[] = {
            "General/language",         "Interface/tray_icon",
            "Capture/show_magnifier",   "Capture/magnifier_mask",
            "Capture/magnifier_border", "Capture/magnifier_anchors",
            "Capture/default_action",   "Pin/thumb_size",
            "Pin/show_mnemonics",       "Output/quick_save_dir",
            "Output/quick_save_notify", "Hotkeys/capture_copy",
            "Hotkeys/pin",              "Hotkeys/toggle_passthrough",
        };
        int removed = 0;
        for (const char* key : kStale) {
            if (!s.contains(QLatin1String(key)))
                continue;
            s.remove(QLatin1String(key));
            ++removed;
        }
        if (removed > 0)
            log::info("config", QStringLiteral("v9：清掉 %1 个已无实现的遗留配置键")
                                    .arg(removed));
    }
    // v10：自动保存不再单独设目录，统一写进 Output/default_dir（两个目录默认都是
    // 桌面，分开配只会让「保存」撞名去重、在同一处留下 _1 这种重复文件）。
    if (from < 10 && s.contains("Output/auto_save_dir")) {
        s.remove("Output/auto_save_dir");
        log::info("config", "v10：自动保存改用「默认保存目录」，已移除 Output/auto_save_dir");
    }
    // v11：截图历史默认档位改为 50 条 / 30 MB（用户要求）。与 v6 同一逻辑：
    // ini 里的值分不出「一直保持默认」还是「用户改过」，这轮整体翻新一次，
    // 数值取自 defaults 表（唯一事实来源）；这轮之后用户再怎么改都尊重——
    // defaults 表只管新装机，老配置全靠迁移。
    if (from < 11) {
        const int limit = defaults::defaultValue("General/history_limit").toInt();
        const int mb = defaults::defaultValue("General/history_max_mb").toInt();
        s.setValue("General/history_limit", limit);
        s.setValue("General/history_max_mb", mb);
        log::info("config", QString("v11：截图历史默认档位改为 %1 条 / %2 MB")
                                .arg(limit)
                                .arg(mb));
    }
}

}  // namespace

QVariant get(const QString& key) {
    const QVariant def = defaults::defaultValue(key);
    return settings()->value(key, def);
}

bool getBool(const QString& key) {
    return get(key).toBool();
}

int getInt(const QString& key) {
    return get(key).toInt();
}

QString getStr(const QString& key) {
    return get(key).toString();
}

void set(const QString& key, const QVariant& value) {
    settings()->setValue(key, value);
}

void sync() {
    settings()->sync();
}

QString hotkeyAccel(const QString& action) {
    return get("Hotkeys/" + action).toString();
}

QSettings* settings() {
    return g_settings;
}

}  // namespace zpin::config
