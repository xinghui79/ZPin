// 帮助文档 —— 4 个主题收在同一页里（页顶一排分段按钮切换），设置对话框侧栏
// 只占「帮助」一项。键位等信息均在渲染时实时读取（改键/改配置后无需重启即同步）。
// 版式：原生控件（灰小节标题 + 白色圆角卡片 + 键帽），字号走 prefsStyle 的
// 角色体系，与设置页一致；键位随配置实时刷新。
#pragma once

#include <QWidget>

namespace zpin::help {

// 帮助页（分段按钮 + 主题内容栈），默认停在「快速上手」。
QWidget* createHelpPage(QWidget* parent = nullptr);

}  // namespace zpin::help
