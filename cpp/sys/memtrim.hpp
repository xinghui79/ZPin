// 空闲内存裁剪 —— 空闲约 1 分钟后先跑注册的缓存清理回调，再把可换出页移出工作集。
// 对应首选项「保持快速响应」开关。
#pragma once

#include <cstddef>
#include <functional>

namespace zpin::memtrim {

// 清理回调的注销句柄。清理器是进程级全局表，不随注册者析构自动摘除：
// 回调捕获了注册者的 this 却不注销，空闲定时器一响就是一次 use-after-free。
// 因此凡是捕获 this 的注册者，析构里必须调 unregisterCleaner。
using CleanerId = std::size_t;

// 注册一个空闲时执行的清理回调（如把历史原图压成 PNG）。
CleanerId registerCleaner(std::function<void()> fn);

// 注销上面拿到的句柄（注销后该回调不再被调用；重复注销同一 id 无害）。
void unregisterCleaner(CleanerId id);

// 标记「刚刚有用户活动」，重新计算空闲时间。
void touch();

// 启动空闲检测定时器（约 15s 检查一次）；重复调用不会重复安装。
void install();

}  // namespace zpin::memtrim
