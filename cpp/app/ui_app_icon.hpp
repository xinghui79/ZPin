// 应用图标 —— 单一来源为内嵌 SVG（渐变玻璃底 + 取景框对角角标 + 居中 Z）。
// 运行时用 QSvgRenderer 按需渲染多尺寸；assets/app_icon.ico 由构建期 genico
// 工具生成（writeIco），供 exe 资源使用。
#pragma once

#include <QIcon>
#include <QString>

namespace zpin::appicon {

// 停用全局快捷键时的托盘图标变体（琥珀底 + 深蓝描边）。
QIcon appIcon(bool disabled = false);

// 把 SVG 渲染为多尺寸 .ico（PNG 压缩条目）并写入指定路径。构建期工具调用。
bool writeIco(const QString& path);

}  // namespace zpin::appicon
