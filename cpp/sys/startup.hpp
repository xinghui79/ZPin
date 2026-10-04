// 开机自启 —— HKCU 的 CurrentVersion\Run 键值（ZPin 不要求管理员，无需计划任务）。
#pragma once

namespace zpin::startup {

// 当前是否已注册自启（Run 键值与本机启动命令一致才算）。
bool isEnabled();

// 开启/关闭开机自启（写/删 Run 键值）。
void setEnabled(bool on);

}  // namespace zpin::startup
