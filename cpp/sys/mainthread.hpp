// 主线程投递 —— 后台线程得出结论后，把「碰界面、碰 App 成员」的落地动作排回主线程。
//
// 在后台线程上直接做完有两处会翻车：界面对象本身只在主线程安全；而调用方对象
// （App、贴图窗、管理器）都是 main 栈对象的子孙，退出期 App 析构之后回调再碰
// 成员就是 use-after-free。排队的落脚点是个活到进程结束的哨兵，所以投递永远
// 不会打进已销毁的接收者——捕获对象请一律用 QPointer，存活判断发生在落地那一刻。
#pragma once

#include <functional>

namespace zpin::mainthread {

// 在主线程把落脚点先建出来（启动时调一次即可）。不调也能用，首次 post 时才建。
void warm();

// 把 fn 排进主线程的事件循环。在主线程调用同样安全，只是执行时机推迟到当前
// 事件处理完之后。
void post(std::function<void()> fn);

}  // namespace zpin::mainthread
