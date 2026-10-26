#include "mainthread.hpp"

#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>

#include <utility>

namespace zpin::mainthread {
namespace {

// 投递的落脚对象：**故意不析构**，活到进程结束由 OS 回收——与 update.cpp /
// scroll_capture.cpp 里那两个私有线程池同一套取舍。
//
// 为什么不拿调用方对象（App、贴图窗、管理器）或 qApp 当接收者：
//   - 调用方对象全是 main 栈对象的子孙，析构后队列里排给它们的调用倒还好
//     （Qt 会连同对象一起清掉），真正的问题是「判活」与「投递」之间那一瞬：
//     worker 被系统抢占几十毫秒再回来，post 就打在一个已销毁的对象上。
//   - qApp 活得比它们久，可它自己同样有销毁点：App 析构、main 返回、qApp
//     析构都在几百毫秒内走完，而在途任务正好可能在这一段落地——那时 post
//     一样落空。
// 这个哨兵永不销毁，排给它的调用永远有合法接收者；真正决定「要不要执行」的
// 是回调里那层 QPointer。
QObject* target() {
    static QObject* const obj = [] {
        auto* o = new QObject;
        // 首次 post 若落在 worker 上，就把对象挪回主线程再用（同线程调用是
        // 空操作）。qApp 已经没了（进程最末尾）就留在原地：那时投递出去也
        // 没人会处理。
        if (auto* app = QCoreApplication::instance())
            o->moveToThread(app->thread());
        return o;
    }();
    return obj;
}

}  // namespace

void warm() {
    target();
}

void post(std::function<void()> fn) {
    QMetaObject::invokeMethod(target(), std::move(fn), Qt::QueuedConnection);
}

}  // namespace zpin::mainthread
