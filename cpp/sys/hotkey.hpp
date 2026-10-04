// 全局热键 —— 主线程隐藏消息窗 + RegisterHotKey。
//
// RegisterHotKey 必须在窗口所属线程内调用（跨线程会得到误导性的错误 1408）。
// 热键窗就建在 Qt 主线程上，Qt 的事件循环本就会 DispatchMessage 到本线程的
// 任意窗口，所以注册/反注册直接在调用线程完成，WM_HOTKEY 由窗口过程按
// wParam 分发成信号——不需要为它另起一个带 GetMessage 循环的线程。
//
// 宿主消息窗与 WndProc 的 Win32 部分都收在 win32util::MessageHost（Win32 边界
// 一律走那里），本文件只剩加速键文本解析与 Qt 信号发射。
#pragma once

#include <QObject>
#include <QMap>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>
#include <utility>

namespace zpin {

namespace win32 {
class MessageHost;
}

namespace hotkey {

// 动作清单（顺序即托盘/设置/启动日志的显示顺序），以及 id、中文标签。
const QStringList& actionOrder();
int actionId(const QString& action);
QString actionLabel(const QString& action);
// 窗口过程按 wParam 反查动作；未知 id 返回空串。
QString actionForId(int id);

// 解析 "Ctrl+Shift+F1" / "Alt+L" 形式的加速键 -> (MOD_* 掩码, 虚拟键码)。
// 空串或非法文本返回 nullopt。
std::optional<std::pair<quint32, quint32>> parseAccel(const QString& text);

// 把 (MOD_* 掩码, 虚拟键码) 格式化回显示文本；主键不认识时只留修饰键部分。
QString formatAccel(quint32 mods, quint32 vk);

// 第二实例启动时调用：唤起已在运行的实例（成功投递返回 true）。
bool notifyShow();

class Manager : public QObject {
    Q_OBJECT

public:
    explicit Manager(QObject* parent = nullptr);
    ~Manager() override;

    // 按 textMap 全量重绑：先反注册「旧绑定 ∪ 新绑定」涉及到的全部 id，
    // 否则「清除快捷键」会被跳过、旧注册一直留着。
    // 返回本次尝试注册的动作 -> 是否成功（停用状态下返回空表）。
    QMap<QString, bool> applyBindings(const QMap<QString, QString>& textMap);

    // 总开关（托盘「停用全局快捷键」）：反注册但保留绑定，恢复时按原
    // mods/vk 重注册。重复调用同一状态是幂等的。
    void setDisabled(bool disabled);

    // 设置页捕获热键时临时挂起：全局热键若不解除，用户想绑的组合键只要
    // 已被自己占用（如把框选改成与全屏相同），WM_HOTKEY 会把按键直接吞掉，
    // 输入框毫无反应、冲突提示永远没机会出现。与 setDisabled 独立叠加。
    void setSuspended(bool suspended);

signals:
    // 热键触发（主线程）。
    void fired(const QString& action);
    // 第二实例请求唤起。
    void showRequested();

private:
    bool bind(const QString& action, quint32 mods, quint32 vk);
    void unbind(const QString& action);

    // 宿主消息窗（Win32 侧在 win32util 里，本文件不碰 <windows.h>）
    std::unique_ptr<win32::MessageHost> m_host;
    bool m_disabled = false;
    bool m_suspended = false;   // 设置页正在捕获热键（全局热键临时解除）
    QMap<QString, std::pair<quint32, quint32>> m_accels;  // 当前已成功注册的动作
};

}  // namespace hotkey
}  // namespace zpin
