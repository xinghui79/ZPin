#include "win32util.hpp"

#include <windows.h>

#include <array>
#include <string>

#include "logging.hpp"

namespace zpin::win32 {

namespace {
// 单实例互斥体句柄：acquire 之后一直持有，releaseSingleInstance 显式放开
HANDLE g_instanceMutex = nullptr;

// 注册表键路径统一成反斜杠（调用图方便写 '/'）
std::wstring regPathW(const QString& keyPath) {
    QString p = keyPath;
    p.replace(QLatin1Char('/'), QLatin1Char('\\'));
    return p.toStdWString();
}

// 写路径用的打开：失败一定有话可说（键不存在=环境不对），落日志并返回 nullptr。
HKEY regOpenForWrite(const QString& keyPath, const QString& who) {
    HKEY root = nullptr;
    const LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, regPathW(keyPath).c_str(), 0,
                                  KEY_SET_VALUE, &root);
    if (rc != ERROR_SUCCESS) {
        log::warn("zpin.win32", QStringLiteral("%1：打开 HKCU\\%2 失败 rc=%3")
                                    .arg(who, keyPath)
                                    .arg(rc));
        return nullptr;
    }
    return root;
}
}  // namespace

bool setTopmost(Hwnd hwnd) {
    constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
    return SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags) != 0;
}

bool setClickThrough(Hwnd hwnd, bool on) {
    const auto cur = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    // 点击穿透 = WS_EX_TRANSPARENT 作用在 layered 窗口上；非 layered 窗口要补
    // WS_EX_LAYERED 才生效（老 SWP 文档的要求）。反过来撤掉穿透时**只**清
    // TRANSPARENT：贴图窗用 WA_TranslucentBackground，WS_EX_LAYERED 是 Qt 自己
    // 设的，一并清掉会让整个窗口变成不透明。
    const auto next = on ? (cur | WS_EX_TRANSPARENT | WS_EX_LAYERED)
                         : (cur & ~WS_EX_TRANSPARENT);
    SetLastError(0);
    // 旧样式恰为 0 时 SetWindowLongPtrW 返回 0 与失败无法区分，两个条件取或：
    // 返回非零即成功；返回零时再看 GetLastError（「成功不动 last error」虽无
    // 文档承诺，但比只看它稳）。
    const bool styleSet = SetWindowLongPtrW(hwnd, GWL_EXSTYLE, next) != 0 ||
                          GetLastError() == 0;
    // 改完扩展样式必须通知系统重载，否则改动可能拖到下次重绘才生效
    // （UIA 查询期间的临时穿透只有一次查询的窗口，依赖它立刻生效）
    return styleSet &&
           SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                            SWP_FRAMECHANGED) != 0;
}

qintptr exStyleGet(Hwnd hwnd) {
    return GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
}

bool exStyleSet(Hwnd hwnd, qintptr style) {
    // 光改 GWL_EXSTYLE 不通知系统重载，样式可能拖到下次重绘才生效——而
    // UIA 查询期间的临时穿透「只有这一次查询的窗口」，等不到下次重绘，
    // ElementFromPoint 仍会命中遮罩自身，元素吸附就静默失效了。
    // 必须补 SetWindowPos(SWP_FRAMECHANGED)（同 setClickThrough）。
    // 不补 repaint()：那是 layered 窗口的坑（见 setClickThrough 注释），
    // 遮罩窗不是 layered 的，且这里跑在 snap 工作线程上，不能碰 GUI 重绘。
    SetLastError(0);
    const bool styleSet = SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style) != 0 ||
                          GetLastError() == 0;   // 旧样式恰为 0 时返回值无法区分成败
    return styleSet &&
           SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                            SWP_FRAMECHANGED) != 0;
}

bool replaceFile(const QString& from, const QString& to) {
    return MoveFileExW(reinterpret_cast<const wchar_t*>(from.utf16()),
                       reinterpret_cast<const wchar_t*>(to.utf16()),
                       MOVEFILE_REPLACE_EXISTING) != 0;
}

bool asyncKeyDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool excludeFromCapture(Hwnd hwnd) {
    // WDA_EXCLUDEFROMCAPTURE 让该窗口对 BitBlt/PrintWindow/WGC 抓屏完全不可见，
    // 而不是被涂成一块纯色——这正是「排除自己的浮窗」需要的语义。
    // 需要 Win10 2004（19041）以上，SetLastError 会给出 ERROR_INVALID_PARAMETER。
    constexpr DWORD kExcludeFromCapture = 0x00000011;
    return SetWindowDisplayAffinity(hwnd, kExcludeFromCapture) != 0;
}

bool isWindow(Hwnd hwnd) {
    return IsWindow(hwnd) != 0;
}

Hwnd foregroundWindow() {
    return GetForegroundWindow();
}

QString windowClassName(Hwnd hwnd) {
    wchar_t buf[64] = {};
    GetClassNameW(hwnd, buf, int(sizeof(buf) / sizeof(buf[0])));
    return QString::fromWCharArray(buf);
}

bool anyMouseButtonDown() {
    // 五个鼠标键：侧键（XBUTTON1/2）也算，浏览器/资源管理器把它们当「前进/后退」
    for (const int vk : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2})
        if (asyncKeyDown(vk))
            return true;
    return false;
}

QHash<QString, QRect> monitorRects() {
    QHash<QString, QRect> out;
    auto onMonitor = [](HMONITOR hmon, HDC, LPRECT, LPARAM lparam) -> BOOL {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(hmon, &mi)) {
            auto* map = reinterpret_cast<QHash<QString, QRect>*>(lparam);
            map->insert(QString::fromWCharArray(mi.szDevice),
                        QRect(mi.rcMonitor.left, mi.rcMonitor.top,
                              mi.rcMonitor.right - mi.rcMonitor.left,
                              mi.rcMonitor.bottom - mi.rcMonitor.top));
        }
        return TRUE;
    };
    EnumDisplayMonitors(nullptr, nullptr, +onMonitor, reinterpret_cast<LPARAM>(&out));
    return out;
}

QString shortPath(const QString& path) {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetShortPathNameW(reinterpret_cast<const WCHAR*>(path.utf16()), buf,
                                      DWORD(sizeof(buf) / sizeof(buf[0])));
    // 0 = 拿不到（路径不存在/卷关闭了短名创建）；超长同理。原样返回，
    // 行为退回改动前，不会因为「求短名失败」把更新流程整个掐断。
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0]))
        return path;
    return QString::fromWCharArray(buf, int(n));
}

QString regReadString(const QString& keyPath, const QString& name) {
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, regPathW(keyPath).c_str(), 0, KEY_READ, &root)
        != ERROR_SUCCESS)
        return QString();   // 键不存在是常态（首次运行/旧系统），不惊动日志
    wchar_t buf[1024] = {};
    DWORD cb = sizeof(buf), type = 0;
    const LONG rc = RegQueryValueExW(root, reinterpret_cast<const WCHAR*>(name.utf16()),
                                     nullptr, &type, reinterpret_cast<LPBYTE>(buf), &cb);
    RegCloseKey(root);
    if (rc != ERROR_SUCCESS || type != REG_SZ)
        return QString();
    return QString::fromWCharArray(buf);
}

QStringList regSubKeys(const QString& keyPath) {
    QStringList names;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, regPathW(keyPath).c_str(), 0, KEY_READ, &root)
        != ERROR_SUCCESS)
        return names;
    wchar_t subName[256] = {};
    for (DWORD i = 0;; ++i) {
        DWORD cch = DWORD(sizeof(subName) / sizeof(subName[0]));
        if (RegEnumKeyExW(root, i, subName, &cch, nullptr, nullptr, nullptr, nullptr)
            != ERROR_SUCCESS)
            break;
        names.append(QString::fromWCharArray(subName, int(cch)));
    }
    RegCloseKey(root);
    return names;
}

bool regWriteString(const QString& keyPath, const QString& name, const QString& value) {
    HKEY root = regOpenForWrite(keyPath, QStringLiteral("写注册表"));
    if (!root)
        return false;
    const DWORD cb = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LONG rc = RegSetValueExW(root, reinterpret_cast<const WCHAR*>(name.utf16()), 0,
                                   REG_SZ, reinterpret_cast<const BYTE*>(value.utf16()), cb);
    RegCloseKey(root);
    if (rc != ERROR_SUCCESS) {
        log::warn("zpin.win32", QStringLiteral("写 HKCU\\%1\\%2 失败 rc=%3")
                                    .arg(keyPath, name)
                                    .arg(rc));
        return false;
    }
    return true;
}

bool regWriteDword(const QString& keyPath, const QString& name, quint32 value) {
    HKEY root = regOpenForWrite(keyPath, QStringLiteral("写注册表"));
    if (!root)
        return false;
    const DWORD v = value;
    const LONG rc = RegSetValueExW(root, reinterpret_cast<const WCHAR*>(name.utf16()), 0,
                                   REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
    RegCloseKey(root);
    if (rc != ERROR_SUCCESS) {
        log::warn("zpin.win32", QStringLiteral("写 HKCU\\%1\\%2 失败 rc=%3")
                                    .arg(keyPath, name)
                                    .arg(rc));
        return false;
    }
    return true;
}

bool regDeleteValue(const QString& keyPath, const QString& name) {
    HKEY root = regOpenForWrite(keyPath, QStringLiteral("删注册表值"));
    if (!root)
        return false;
    const LONG rc = RegDeleteValueW(root, reinterpret_cast<const WCHAR*>(name.utf16()));
    RegCloseKey(root);
    if (rc == ERROR_SUCCESS)
        return true;
    if (rc == ERROR_FILE_NOT_FOUND)
        return true;   // 本来就没注册过：删除的目标状态已达成，不是故障
    log::warn("zpin.win32", QStringLiteral("删 HKCU\\%1\\%2 失败 rc=%3")
                                .arg(keyPath, name)
                                .arg(rc));
    return false;
}

bool trimWorkingSet() {
    return SetProcessWorkingSetSize(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1)) != 0;
}

qintptr lastError() {
    return static_cast<qintptr>(GetLastError());
}

bool acquireSingleInstance() {
    if (g_instanceMutex)
        return true;
    g_instanceMutex = CreateMutexW(nullptr, FALSE, L"ZPin.SingleInstance");
    if (!g_instanceMutex)
        return false;  // 连互斥体都拿不到（极端环境）：按「已有实例」退出，避免双实例并存
    return GetLastError() != ERROR_ALREADY_EXISTS;
}

void releaseSingleInstance() {
    if (!g_instanceMutex)
        return;
    CloseHandle(g_instanceMutex);
    g_instanceMutex = nullptr;
}

bool registerRawWheelSink(Hwnd hwnd) {
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;   // Generic Desktop
    rid.usUsage = 0x02;       // Mouse
    rid.dwFlags = RIDEV_INPUTSINK;   // 无焦点也投递：用户滚的是别的进程的窗口
    rid.hwndTarget = static_cast<HWND>(hwnd);
    return RegisterRawInputDevices(&rid, 1, sizeof(rid)) != FALSE;
}

int rawWheelDelta(void* message) {
    const MSG* m = static_cast<const MSG*>(message);
    if (!m || m->message != WM_INPUT)
        return 0;
    RAWINPUT raw{};
    UINT size = sizeof(raw);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(m->lParam), RID_INPUT, &raw, &size,
                        sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
        return 0;
    if (raw.header.dwType != RIM_TYPEMOUSE ||
        (raw.data.mouse.usButtonFlags & RI_MOUSE_WHEEL) == 0)
        return 0;
    // usButtonData 是 SHORT 有符号增量：正 = 向前推（看回上面的内容），
    // 负 = 向后拉（往文档尾部走）。拼接只消费负方向。
    return static_cast<short>(raw.data.mouse.usButtonData);
}

// ---- 全局热键宿主 ----

namespace {
// 宿主窗的类名。固定字面量，不做成参数：单实例互唤要按类名跨进程找到它，
// 两边必须一致，而全项目只有这一个宿主。
constexpr const wchar_t* kMessageHostClass = L"ZPinHost";
// HWND_MESSAGE 是消息专用窗口的父窗柄（是个魔数 -3，不是真实窗口）。它是
// ((HWND)-3) 这种带指针转换的宏，不能进 constexpr，所以按原样用在调用处。
// RegisterClassW 对同一个类名只会成功一次；第二次返回 ERROR_CLASS_ALREADY_EXISTS。
// 用文件级标志记住，免得把「已注册」误当成失败（那会让整个宿主建不起来）。
bool g_messageHostClassReady = false;
}  // namespace

class MessageHost::Impl {
public:
    explicit Impl(Handler handler) : handler_(std::move(handler)) {
        if (!g_messageHostClassReady) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = &Impl::wndProc;
            wc.lpszClassName = kMessageHostClass;
            wc.hInstance = GetModuleHandleW(nullptr);
            if (!RegisterClassW(&wc)) {
                log::error("zpin.win32", QStringLiteral("注册宿主窗口类失败 err=%1")
                                                .arg(GetLastError()));
                return;
            }
            g_messageHostClassReady = true;
        }
        hwnd_ = CreateWindowExW(0, kMessageHostClass, kMessageHostClass, 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr),
                                nullptr);
        if (!hwnd_) {
            log::error("zpin.win32", QStringLiteral("创建宿主消息窗失败 err=%1")
                                            .arg(GetLastError()));
            return;
        }
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }

    ~Impl() {
        // 必须先断回调：窗口销毁期间可能还有消息在派发，handler_ 里的 Qt 对象
        // （Manager）可能已经没了。
        handler_ = nullptr;
        if (hwnd_)
            DestroyWindow(hwnd_);
    }

    bool valid() const { return hwnd_ != nullptr; }

    bool bindHotkey(int id, quint32 mods, quint32 vk) {
        return hwnd_ && RegisterHotKey(hwnd_, id, mods, vk) != FALSE;
    }

    void unbindHotkey(int id) {
        if (hwnd_)
            UnregisterHotKey(hwnd_, id);
    }

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self && self->handler_ &&
            self->handler_(msg, static_cast<quint64>(wParam), static_cast<qint64>(lParam)))
            return 0;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    Handler handler_;
    HWND hwnd_ = nullptr;
};

MessageHost::MessageHost(Handler handler)
    : m_impl(std::make_unique<Impl>(std::move(handler))) {}

MessageHost::~MessageHost() = default;

bool MessageHost::valid() const {
    return m_impl && m_impl->valid();
}

bool MessageHost::bindHotkey(int id, quint32 mods, quint32 vk) {
    return m_impl && m_impl->bindHotkey(id, mods, vk);
}

void MessageHost::unbindHotkey(int id) {
    if (m_impl)
        m_impl->unbindHotkey(id);
}

bool postToMessageHost(const QString& className, unsigned msg) {
    const HWND hwnd = FindWindowExW(HWND_MESSAGE, nullptr,
                                    reinterpret_cast<LPCWSTR>(className.utf16()), nullptr);
    return hwnd && PostMessageW(hwnd, msg, 0, 0) != 0;
}

}  // namespace zpin::win32
