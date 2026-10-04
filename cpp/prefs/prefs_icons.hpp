// 设置页侧栏图标 —— macOS 系统设置式「彩色圆角方块 + 白色字形」。
// 图标用内嵌 SVG 现渲，不加资源文件：字形都是几条线，而 QSvgRenderer 已随
// ui_app_icon 链进可执行文件，新增一套 .svg 反而要动 CMake 和打包脚本。
#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>

namespace zpin::prefsicon {

// 按导航项命名（不是按形状）：一个页面一个图标，形状与配色都在实现里成对决定。
enum class Glyph { General, Capture, Pin, Output, Hotkey, Help, About };

// 边长 px 的瓦片；内部按 scale 倍超采样绘制并写回 DPR，高 DPI 下不发虚。
// 配色由字形决定（每个导航项一色），调用方只管要哪个图标。
QPixmap tile(Glyph glyph, int px, int scale = 2);
// 侧栏用的 20px 图标（含 1x/2x 两档，QListWidget 自己按屏幕挑）。
QIcon icon(Glyph glyph);

// 尖角：下拉框与数值步进器都要一个，而 QSS 的 ::down-arrow 只能引用文件路径、
// 引用不到运行期画出来的位图，所以自己画好塞进图标/画进 paintEvent。
QPixmap chevron(bool up, const QColor& color, const QSize& px);

// 色板方块：先铺棋盘格再压颜色。半透明色（遮罩色）直接画在白底上看不出浓度，
// 棋盘格就是「这一格带 Alpha」的通用记号。
QIcon swatch(const QColor& color, int px);

}  // namespace zpin::prefsicon
