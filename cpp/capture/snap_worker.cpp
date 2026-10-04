#include "snap_worker.hpp"

#include <QThread>
#include <QtGlobal>
#include <algorithm>
#include <vector>

#include "logging.hpp"
#include "rcore.hpp"
#include "win32util.hpp"

namespace zpin {
namespace snap {

namespace {

using clock = std::chrono::steady_clock;

// 与旧同步版 elementAtGuarded 相同的熔断阈值：单次超过 250ms 暂停 30s。
constexpr double kBreakerMs = 250.0;
constexpr double kCooldownSec = 30.0;

}  // namespace

double LatencySamples::percentile(double p) const {
    const int n = count();
    if (n == 0)
        return 0.0;
    std::vector<double> sorted(m_buf.begin(), m_buf.begin() + n);
    std::sort(sorted.begin(), sorted.end());
    const int idx = int(p * (n - 1));
    return sorted[size_t(idx)];
}

double LatencySamples::max() const {
    const int n = count();
    double best = 0.0;
    for (int i = 0; i < n; ++i)
        best = qMax(best, m_buf[size_t(i)]);
    return best;
}

SnapWorker* worker() {
    // 进程级常驻（故意不退）：线程销毁会连带丢掉线程缓存的 COM apartment，
    // 下次会话重建反而多付几十毫秒；进程退出由操作系统回收。
    static SnapWorker* s_worker = [] {
        auto* thread = new QThread;
        thread->setObjectName(QStringLiteral("zpin.snap"));
        auto* w = new SnapWorker;
        w->moveToThread(thread);
        QObject::connect(thread, &QThread::finished, w, &QObject::deleteLater);
        thread->start();
        return w;
    }();
    return s_worker;
}

void SnapWorker::setWindows(const QVector<qintptr>& hwnds) {
    m_hwnds = hwnds;
}

void SnapWorker::query(int id, int x, int y) {
    const auto now = clock::now();
    if (now < m_cooldown) {
        m_cooldownSkips++;
        emit queried(id, false, 0, 0, 0, 0);
        return;
    }
    // 遮罩窗盖着整个屏幕，不穿透时 ElementFromPoint 永远返回遮罩自身。
    // 存样式 → 穿透 → 查询 → 还原都在本次调用内完成；还原前先验窗口
    // 还活着（期间 UI 线程可能已结束会话销毁窗口，HWND 可能被复用）。
    QVector<std::pair<qintptr, qintptr>> saved;
    saved.reserve(m_hwnds.size());
    for (qintptr hwnd : m_hwnds) {
        auto* hwndPtr = reinterpret_cast<win32::Hwnd>(hwnd);
        const qintptr cur = win32::exStyleGet(hwndPtr);
        // 临时穿透走现成封装（它做的正是 |= WS_EX_TRANSPARENT|WS_EX_LAYERED 并补
        // SWP_FRAMECHANGED），别再在这里写裸样式位。
        if (win32::setClickThrough(hwndPtr, true))
            saved.append({hwnd, cur});
    }
    const auto t0 = clock::now();
    const std::optional<rcore::ElementInfo> hit = rcore::elementFromPoint(x, y);
    const double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    for (const auto& [hwnd, style] : saved) {
        auto* hwndPtr = reinterpret_cast<win32::Hwnd>(hwnd);
        if (win32::isWindow(hwndPtr))
            win32::exStyleSet(hwndPtr, style);
    }

    m_total++;
    m_ms.add(ms);
    if (ms > kBreakerMs) {
        m_cooldown = clock::now() + std::chrono::duration_cast<clock::duration>(
                                       std::chrono::duration<double>(kCooldownSec));
        log::warn("overlay", QStringLiteral("UIA provider 响应 %1ms，界面元素吸附暂停 30s")
                                 .arg(qRound(ms)));
        emit queried(id, false, 0, 0, 0, 0);
        return;
    }
    if (hit)
        emit queried(id, true, hit->rect.x(), hit->rect.y(), hit->rect.width(),
                     hit->rect.height());
    else
        emit queried(id, false, 0, 0, 0, 0);
}

void SnapWorker::dumpTimings() {
    if (m_total == 0 && m_cooldownSkips == 0)
        return;
    log::info("overlay", QStringLiteral("UIA 查询埋点：%1 次（熔断跳过 %2 次），"
                                        "p50 %3ms / p95 %4ms / p99 %5ms / max %6ms")
                             .arg(m_total)
                             .arg(m_cooldownSkips)
                             .arg(m_ms.percentile(0.50), 0, 'f', 1)
                             .arg(m_ms.percentile(0.95), 0, 'f', 1)
                             .arg(m_ms.percentile(0.99), 0, 'f', 1)
                             .arg(m_ms.max(), 0, 'f', 1));
    m_total = 0;
    m_cooldownSkips = 0;
    m_ms.reset();
}

}  // namespace snap
}  // namespace zpin
