// C++ 侧的薄 Win32 边界：窗口扩展样式 / 置顶 / 鼠标穿透 / 按键状态 /
// 工作集裁剪 / 单实例互斥体。抓屏、UIA、窗口枚举这些深的交互在 Rust 核心
// （rcore.hpp），这里只留界面层绕不开的几个小调用。
#pragma once

#include <QHash>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <functional>
#include <memory>

struct HWND__;

namespace zpin::win32 {

using Hwnd = HWND__*;

// 把窗口设为 TOPMOST（不移动、不改变大小、不激活）。
bool setTopmost(Hwnd hwnd);

// 鼠标穿透（WS_EX_TRANSPARENT|WS_EX_LAYERED）；返回是否成功。
bool setClickThrough(Hwnd hwnd, bool on);

// 扩展样式读取/恢复（UIA 查询期间遮罩窗临时穿透用）。
// exStyleSet 内部带 SetWindowPos(SWP_FRAMECHANGED)，样式即刻生效——
// 临时穿透的窗口生命周期只有这一次查询，不能等「下次重绘」。
qintptr exStyleGet(Hwnd hwnd);
bool exStyleSet(Hwnd hwnd, qintptr style);

// 指定虚拟键当前是否按下（GetAsyncKeyState & 0x8000）。
bool asyncKeyDown(int vk);

// 任意一个鼠标键正被按住（左/右/中/两个侧键）：吸附查询用它挡掉「按住就点」
// 的竞态——穿透期间到达的点击会落进下层应用。VK 码收在这里，界面层不写裸数字。
bool anyMouseButtonDown();

// 把本进程自己的窗口从屏幕抓取里排除（WDA_EXCLUDEFROMCAPTURE，Win10 2004+）。
// 滚动长截图必须在抓帧时把进度窗摘出去，否则那块 224x470 的面板会被逐帧
// 拍进长图，在画布上留一个永远遮住内容的洞。返回是否成功（不支持的旧系统
// 返回 false，调用方据此决定退到「把窗挪开」的兜底）。
bool excludeFromCapture(Hwnd hwnd);

// 句柄是否仍指向一个存活窗口（跨线程查询回来后恢复样式前先验一下，
// 期间窗口可能已被销毁，HWND 可能被系统复用）。
bool isWindow(Hwnd hwnd);

// 把可换出页移出工作集（SetProcessWorkingSetSize(-1,-1)）。
bool trimWorkingSet();

// 最近一次 Win32 调用的 GetLastError（界面层要报错时用，省得各处裸调
// <windows.h>——Win32 边界一律走本文件）。
qintptr lastError();

// CREATE_NO_WINDOW：QProcess 拉控制台子进程（tar / cmd）时不闪黑框。
// 收在这里是为了让界面层不用为了一个常量 include <windows.h>。
inline constexpr unsigned kCreateNoWindow = 0x08000000;

// 进程级单实例：创建并持有命名互斥体；False = 已有实例在运行。
bool acquireSingleInstance();

// 提前放开单实例互斥体（托盘「重新启动」在退出前调用）。
void releaseSingleInstance();

// 前台窗口句柄（焦点窗口截图用）。
Hwnd foregroundWindow();

// 窗口类名（判断前台是不是任务栏/桌面等系统壳窗口用）。
QString windowClassName(Hwnd hwnd);

// 枚举显示器：设备名 -> 绝对物理矩形 (x,y,w,h)。Win32 一手数据，避开 Qt 逻辑
// 坐标 round-trip 在分数缩放下的 1px 偏差。
QHash<QString, QRect> monitorRects();

// 8.3 短路径（GetShortPathNameW）：拿不到（卷关了短名创建）就原样返回。
// 自动更新的换装批处理用它——cmd 按本机 OEM 码页读 .bat，安装目录里一旦有中文
// （%USERPROFILE%、D:\软件\...），非中文码页的系统会把路径写成 '?'，xcopy 静默
// 失败、用户留下半套文件。短名是纯 ASCII，绕开整个码页问题。
QString shortPath(const QString& path);

// 用 from 原子换掉 to（to 可不存在；同卷内就是一次改名）。QFile::rename 不
// 覆盖已存在的目标，而「先删旧再改名」在改名失败时两头都空——旧文件已经
// 没了、新的又没就位。覆盖写一律走这里。
bool replaceFile(const QString& from, const QString& to);

// ---- HKCU 注册表小工具（开机自启、Win11 托盘提升）----
// 界面层不裸调 <windows.h>：这两个子系统原先各抄一份 RegOpenKeyExW/RegSetValueExW，
// 失败路径还不一样。keyPath 用反斜杠分隔、相对 HKEY_CURRENT_USER。
// 读不到（键或值不存在）返回空串；写/删失败落一条 warn 并返回 false。
QString regReadString(const QString& keyPath, const QString& name);
bool regWriteString(const QString& keyPath, const QString& name, const QString& value);
bool regDeleteValue(const QString& keyPath, const QString& name);
bool regWriteDword(const QString& keyPath, const QString& name, quint32 value);
QStringList regSubKeys(const QString& keyPath);

// ---- 鼠标滚轮 Raw Input（长截图的滚动检测）----
// 注册 hwnd 为鼠标 Raw Input 的全局接收者（RIDEV_INPUTSINK）：无论焦点在哪个
// 窗口，滚轮事件都会以 WM_INPUT 投递到它——长截图时用户滚的是目标窗口
// （浏览器等），没有 INPUTSINK 根本看不到滚动。同一 usage 的注册会被下一次
// RegisterRawInputDevices 替换，每场捕获注册一次；结束时不必注销：hwnd 销毁
// 后投递自然落空，下一场捕获会重新注册。
bool registerRawWheelSink(Hwnd hwnd);
// 判断一条原生消息（MSG*）是不是鼠标滚轮的 WM_INPUT，是则返回滚轮增量：
// 正 = 向前推（视口向文档头移动），负 = 向后拉（视口向文档尾移动，长截图
// 只认这个方向）；非滚轮消息或取数据失败返回 0。
int rawWheelDelta(void* message);

// ---- 全局热键宿主 ----
// 一个「只收消息、不可见」的 HWND_MESSAGE 窗口：全局热键（WM_HOTKEY）与单实例
// 互唤都投递到这里。原先 hotkey.cpp 直接 include <windows.h> 自己写 WNDCLASSW
// 与 WndProc，Win32 细节散在界面层；这里收成一处，hotkey.cpp 只留加速键文本
// 解析与 Qt 信号发射。
class MessageHost {
public:
    // 返回 true = 已处理（不再走 DefWindowProcW）。
    using Handler = std::function<bool(unsigned msg, quint64 wParam, qint64 lParam)>;

    explicit MessageHost(Handler handler);
    ~MessageHost();
    MessageHost(const MessageHost&) = delete;
    MessageHost& operator=(const MessageHost&) = delete;

    bool valid() const;
    // 注册全局热键（RegisterHotKey）。mods/vk 的组合由系统全局仲裁，失败时
    // 多半是别的软件占了；lastError() 有话可说。id 只要求在本进程内唯一。
    bool bindHotkey(int id, quint32 mods, quint32 vk);
    void unbindHotkey(int id);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

// 按窗口类名找到宿主窗并投一条消息过去（单实例互唤：「已经在运行了」）。
// 找不到返回 false（没有别的实例在跑）。
bool postToMessageHost(const QString& className, unsigned msg);

}  // namespace zpin::win32
