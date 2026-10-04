#include "memtrim.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <utility>
#include <vector>

#include "logging.hpp"
#include "win32util.hpp"

namespace zpin::memtrim {
namespace {

constexpr qint64 kIdleMs = 60'000;
constexpr qint64 kCheckMs = 15'000;

// 单调时钟：改系统时间不该让空闲判定跳变。
qint64 nowMs() {
    static QElapsedTimer clock = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return clock.elapsed();
}

qint64 g_lastActive = nowMs();
QTimer* g_timer = nullptr;
CleanerId g_nextId = 1;
// 只有两个清理器（历史压缩、贴图缩略图），线性扫足够；用 vector 存是为了
// 注销时按 id 精确摘除、不动其它回调的对象地址
std::vector<std::pair<CleanerId, std::function<void()>>> g_cleaners;

void trimNow() {
    for (const auto& [id, fn] : g_cleaners)
        fn();
    if (win32::trimWorkingSet())
        log::info("zpin.memtrim", QStringLiteral("空闲超时，已清理缓存并裁剪工作集"));
    else
        log::warn("zpin.memtrim", QStringLiteral("SetProcessWorkingSetSize 失败 err=%1")
                                     .arg(win32::lastError()));
}

void check() {
    if (nowMs() - g_lastActive < kIdleMs)
        return;
    trimNow();
    touch();
}

}  // namespace

CleanerId registerCleaner(std::function<void()> fn) {
    const CleanerId id = g_nextId++;
    g_cleaners.emplace_back(id, std::move(fn));
    return id;
}

void unregisterCleaner(CleanerId id) {
    for (auto it = g_cleaners.begin(); it != g_cleaners.end(); ++it) {
        if (it->first == id) {
            g_cleaners.erase(it);
            return;
        }
    }
}

void touch() {
    g_lastActive = nowMs();
}

void install() {
    if (g_timer)
        return;
    g_timer = new QTimer(qApp);
    g_timer->setInterval(static_cast<int>(kCheckMs));
    QObject::connect(g_timer, &QTimer::timeout, check);
    g_timer->start();
}

}  // namespace zpin::memtrim
