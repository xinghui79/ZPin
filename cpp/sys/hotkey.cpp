#include "hotkey.hpp"

#include <QHash>
#include <QSet>
#include <vector>

#include "logging.hpp"
#include "win32util.hpp"

namespace zpin::hotkey {
namespace {

constexpr unsigned kWmHotkey = 0x0312;
constexpr unsigned kWmAppShow = 0x8000 + 3;  // 第二实例请求第一实例弹气泡
const char* kHostClass = "ZPinHost";

struct Def {
    const char* action;
    int id;
    const char* label;
};

// 动作表：id 只要求进程内唯一，取可读的英文动作名便于对照日志。
const std::vector<Def>& defs() {
    static const std::vector<Def> kDefs = {
        {"capture_full", 0xA001, "全屏截图"},
        {"capture", 0xA003, "框选截图"},
        {"capture_window", 0xA008, "窗口截图"},
        {"toggle_pins", 0xA005, "隐藏/显示所有贴图"},
    };
    return kDefs;
}

// ---- 加速键文本 <-> (modifiers, VK) ----

constexpr quint32 kModAlt = 0x0001, kModControl = 0x0002, kModShift = 0x0004, kModWin = 0x0008;
constexpr quint32 kVkF1 = 0x70;

quint32 modToken(const QString& token) {
    if (token == QLatin1String("CTRL") || token == QLatin1String("CONTROL"))
        return kModControl;
    if (token == QLatin1String("SHIFT"))
        return kModShift;
    if (token == QLatin1String("ALT"))
        return kModAlt;
    if (token == QLatin1String("WIN") || token == QLatin1String("META"))
        return kModWin;
    return 0;
}

const QHash<QString, quint32>& namedVk() {
    static const QHash<QString, quint32> kNamed = {
        {"PRINTSCREEN", 0x2C}, {"SCROLLLOCK", 0x91}, {"PAUSE", 0x13}, {"INS", 0x2D},
        {"DEL", 0x2E},         {"HOME", 0x24},       {"END", 0x23},   {"PGUP", 0x21},
        {"PGDN", 0x22},        {"SPACE", 0x20},      {"TAB", 0x09},
    };
    return kNamed;
}

const QHash<QString, quint32>& oemVk() {
    static const QHash<QString, quint32> kOem = {
        {";", 0xBA}, {"=", 0xBB}, {",", 0xBC}, {"-", 0xBD}, {".", 0xBE}, {"/", 0xBF},
        {"`", 0xC0}, {"[", 0xDB}, {"\\", 0xDC}, {"]", 0xDD}, {"'", 0xDE},
    };
    return kOem;
}

const QHash<quint32, QString>& oemText() {
    static const QHash<quint32, QString> kText = [] {
        QHash<quint32, QString> m;
        for (auto it = oemVk().cbegin(); it != oemVk().cend(); ++it)
            m.insert(it.value(), it.key());
        return m;
    }();
    return kText;
}

const QHash<quint32, QString>& displayVk() {
    static const QHash<quint32, QString> kDisplay = {
        {0x2C, "PrintScreen"}, {0x91, "ScrollLock"}, {0x13, "Pause"}, {0x2D, "Ins"},
        {0x2E, "Del"},         {0x24, "Home"},       {0x23, "End"},   {0x21, "PgUp"},
        {0x22, "PgDn"},        {0x20, "Space"},      {0x09, "Tab"},
    };
    return kDisplay;
}

// 宿主窗口的 WndProc 已经搬进 win32util（Win32 边界一律走那里），这里只做
// 「消息 -> Qt 信号」的翻译：WndProc 跑在创建它的线程（主线程），所以直接
// 发信号是安全的。
}  // namespace

const QStringList& actionOrder() {
    static const QStringList kOrder = [] {
        QStringList list;
        for (const Def& d : defs())
            list << QString::fromLatin1(d.action);
        return list;
    }();
    return kOrder;
}

int actionId(const QString& action) {
    for (const Def& d : defs())
        if (action == QLatin1String(d.action))
            return d.id;
    return 0;
}

QString actionLabel(const QString& action) {
    for (const Def& d : defs())
        if (action == QLatin1String(d.action))
            return QString::fromUtf8(d.label);
    return {};
}

QString actionForId(int id) {
    for (const Def& d : defs())
        if (d.id == id)
            return QString::fromLatin1(d.action);
    return {};
}

std::optional<std::pair<quint32, quint32>> parseAccel(const QString& text) {
    if (text.trimmed().isEmpty())
        return std::nullopt;
    QString compact = text;
    compact.remove(QLatin1Char(' '));
    quint32 mods = 0;
    QString key;
    for (const QString& part : compact.split(QLatin1Char('+'))) {
        const QString upper = part.toUpper();
        const quint32 mod = modToken(upper);
        if (mod && key.isEmpty()) {
            mods |= mod;
            continue;
        }
        if (!key.isEmpty())  // 两个主键
            return std::nullopt;
        key = upper;
    }
    if (key.isEmpty())
        return std::nullopt;
    // 必须带修饰键。裸主键（mods==0）会被 RegisterHotKey 正常注册成**全局单键
    // 热键**——此后在任何软件里敲那个键都拉起 ZPin 遮罩（敲空格＝直接开始框选，
    // 基本 unusable），而且因为注册「成功」了，改键页那个回滚分支永远不会触发，
    // 用户只能进设置手动清掉，重启后还在。
    // 挡在 parseAccel 这一层（而不是 key_edit）是因为它是所有入口的必经之路：
    // 改键页的按键捕获、以及 config.ini 里被手改过的值，都从这里过。
    if (mods == 0)
        return std::nullopt;
    if (key.at(0) == QLatin1Char('F') && key.size() > 1) {
        bool ok = false;
        const int n = key.mid(1).toInt(&ok);
        if (ok && n >= 1 && n <= 24)
            return std::make_pair(mods, kVkF1 + n - 1);
    }
    if (key.size() == 1) {
        const QChar ch = key.at(0);
        if (ch.isDigit() || (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z')))
            return std::make_pair(mods, static_cast<quint32>(ch.unicode()));
    }
    if (auto it = namedVk().constFind(key); it != namedVk().cend())
        return std::make_pair(mods, it.value());
    if (auto it = oemVk().constFind(key); it != oemVk().cend())
        return std::make_pair(mods, it.value());
    return std::nullopt;
}

QString formatAccel(quint32 mods, quint32 vk) {
    QStringList parts;
    if (mods & kModControl)
        parts << "Ctrl";
    if (mods & kModShift)
        parts << "Shift";
    if (mods & kModAlt)
        parts << "Alt";
    if (mods & kModWin)
        parts << "Win";
    if (vk >= 0x70 && vk <= 0x87) {
        parts << QStringLiteral("F%1").arg(vk - 0x70 + 1);
    } else if ((vk >= 0x30 && vk <= 0x39) || (vk >= 0x41 && vk <= 0x5A)) {
        parts << QString(1, QChar(static_cast<ushort>(vk)));
    } else if (auto it = displayVk().constFind(vk); it != displayVk().cend()) {
        parts << it.value();
    } else if (auto it = oemText().constFind(vk); it != oemText().cend()) {
        parts << it.value();
    }
    return parts.join(QLatin1Char('+'));
}

bool notifyShow() {
    return win32::postToMessageHost(QString::fromLatin1(kHostClass), kWmAppShow);
}

Manager::Manager(QObject* parent) : QObject(parent) {
    m_host = std::make_unique<win32::MessageHost>([this](unsigned msg, quint64 wParam,
                                                        qint64) {
        if (msg == kWmHotkey)
            emit fired(actionForId(static_cast<int>(wParam)));
        else if (msg == kWmAppShow)
            emit showRequested();
        else
            return false;   // 不是我们的消息，交给 DefWindowProcW
        return true;
    });
    // 宿主窗建不起来时 win32util 已记 error；这里不重复报错，但 bind() 会一律
    // 返回 false，托盘与设置页照常显示「热键注册失败」。
}

Manager::~Manager() {
    for (const QString& action : m_accels.keys())
        unbind(action);
    m_host.reset();   // 析构里已清空 handler，窗口销毁期间不会再回调进来
}

bool Manager::bind(const QString& action, quint32 mods, quint32 vk) {
    if (!m_host || !m_host->valid())
        return false;
    // 必须带 mods/vk：RegisterHotKey(hwnd, id, 0, 0) 会「成功」并占住该 id，
    // 把一个空热键顶替真实绑定，导致恢复后热键永久失效。
    if (m_host->bindHotkey(actionId(action), mods, vk))
        return true;
    log::warn("zpin.hotkey", QStringLiteral("热键 %1=%2 注册失败 err=%3")
                                .arg(action, formatAccel(mods, vk))
                                .arg(win32::lastError()));
    return false;
}

void Manager::unbind(const QString& action) {
    if (m_host)
        m_host->unbindHotkey(actionId(action));
}

QMap<QString, bool> Manager::applyBindings(const QMap<QString, QString>& textMap) {
    QMap<QString, std::pair<quint32, quint32>> wanted;
    for (auto it = textMap.cbegin(); it != textMap.cend(); ++it) {
        if (auto parsed = parseAccel(it.value()))
            wanted.insert(it.key(), *parsed);
    }
    QMap<QString, bool> results;
    if (m_disabled || m_suspended) {
        // 停用/挂起中：只记账不注册，恢复时按新表注册（与托盘「已停用」保持一致）
        m_accels = wanted;
        return results;
    }
    QSet<QString> stale;
    for (auto it = m_accels.cbegin(); it != m_accels.cend(); ++it)
        stale.insert(it.key());
    for (auto it = wanted.cbegin(); it != wanted.cend(); ++it)
        stale.insert(it.key());
    for (auto it = stale.cbegin(); it != stale.cend(); ++it)
        unbind(*it);
    // 注册表整体换成「本次注册成功的那些」：失败/被清空的旧绑定不能留下记录，
    // 否则 setDisabled(false) 会去重注册一个其实没生效的键。
    QMap<QString, std::pair<quint32, quint32>> bound;
    for (auto it = wanted.cbegin(); it != wanted.cend(); ++it) {
        const bool ok = bind(it.key(), it.value().first, it.value().second);
        results.insert(it.key(), ok);
        if (ok)
            bound.insert(it.key(), it.value());
    }
    m_accels = bound;
    return results;
}

void Manager::setDisabled(bool disabled) {
    if (disabled == m_disabled)
        return;  // 幂等：重复注册同一组合键会得到 err 1409（已被注册）
    m_disabled = disabled;
    if (m_suspended)
        return;  // 捕获挂起中：保持未注册，恢复时按当前 m_disabled 落位
    for (auto it = m_accels.cbegin(); it != m_accels.cend(); ++it) {
        if (disabled)
            unbind(it.key());
        else
            bind(it.key(), it.value().first, it.value().second);
    }
}

void Manager::setSuspended(bool suspended) {
    if (suspended == m_suspended)
        return;  // 幂等
    m_suspended = suspended;
    if (m_disabled)
        return;  // 总开关停用中本来就没有注册；恢复时按 m_disabled 规则处理
    for (auto it = m_accels.cbegin(); it != m_accels.cend(); ++it) {
        if (suspended)
            unbind(it.key());
        else
            bind(it.key(), it.value().first, it.value().second);
    }
}

}  // namespace zpin::hotkey
