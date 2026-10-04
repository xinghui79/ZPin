// 吸附工作线程 —— UIA 元素查询离开 UI 线程，选区交互不再被慢 provider 卡住
// （对照 snow-shot 的 snow-ui-selector：常驻选择线程 + 熔断，UI 只收几何结果）。
// 线程与 worker 进程级复用：COM apartment 与 IUIAutomation 按线程缓存
// （rust/src/uia.rs），常驻才让缓存有意义。遮罩窗穿透的置位/恢复在单次查询内
// 自洽完成（存样式 → 穿透 → ElementFromPoint → 还原），不跨消息残留状态。
#pragma once

#include <QObject>
#include <QVector>
#include <array>
#include <chrono>

namespace zpin {
namespace snap {

// 延迟采样器：滚动保留最近 N 条，会话结束时算分位数落日志（先测量再优化）。
class LatencySamples {
public:
    void add(double ms) {
        m_buf[m_head % kCap] = ms;
        m_head++;
    }
    void reset() { m_head = 0; }
    int count() const { return int(qMin<qint64>(m_head, kCap)); }
    // p 取 0~1（如 0.95）；无样本返回 0。
    double percentile(double p) const;
    double max() const;

private:
    static constexpr int kCap = 512;
    qint64 m_head = 0;
    std::array<double, kCap> m_buf{};
};

// 进程级单例（惰性创建，首次截图会话时起线程）。
class SnapWorker;
SnapWorker* worker();

class SnapWorker : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

public slots:
    // 会话开始/结束（队列调用，与查询串行，无需加锁）。
    void setWindows(const QVector<qintptr>& hwnds);
    // 单次 UIA 查询（绝对物理坐标）；结果经 queried 信号回 UI 线程。
    void query(int id, int x, int y);
    // 会话结束：查询耗时分布落日志（有样本才打）。
    void dumpTimings();

signals:
    // 单次查询结果回 UI 线程。参数只留真正被消费的：早先还带请求点的 x/y 与
    // 控制类型 ID，onQueried 里三个全是 Q_UNUSED——每次查询（最高 50 次/秒）
    // 白白跨线程搬三个没人看的值。
    void queried(int id, bool hit, int l, int t, int w, int h);

private:
    QVector<qintptr> m_hwnds;          // 当前会话的遮罩窗（穿透切换对象）
    std::chrono::steady_clock::time_point m_cooldown{};  // 慢 provider 熔断截止时刻
    LatencySamples m_ms;               // 单次查询耗时（含穿透切换）
    qint64 m_total = 0;
    qint64 m_cooldownSkips = 0;
};

}  // namespace snap
}  // namespace zpin
