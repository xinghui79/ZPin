// 多屏截图合成 —— 物理画布 + 分段的逻辑<->物理坐标映射。
//
// 坐标约定：
//   逻辑坐标   Qt 全局逻辑坐标，UI（选区/命中/事件）一律用它；
//   绝对物理   Win32 屏幕坐标（GetWindowRect / rcMonitor 那套，原点在主屏 0,0）；
//   画布坐标   抓出来的底图像素坐标 = 绝对物理 - 画布原点（物理虚拟桌面左上角）。
// 底图画布就是物理虚拟桌面本身，每屏按自己的原生像素 1:1 贴入，缩放倍率恒为 1。
// 所有换算只走 DesktopMap 的方法。
#pragma once

#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <vector>

namespace zpin::capture {

// 一块屏的三种坐标：逻辑矩形、绝对物理矩形、自身缩放比。
struct ScreenPiece {
    QRect logical;
    QRect absPhys;
    double dpr = 1.0;
};

// 逻辑 <-> 画布物理 的分段映射（每屏用自己的比例）。
class DesktopMap {
public:
    std::vector<ScreenPiece> pieces;
    QRect canvas;   // 物理画布矩形（像素坐标，原点在虚拟桌面左上角）
    QPoint origin;  // 画布原点对应的绝对物理坐标

    // 逻辑点所在的屏；落在 L 形排布的空白区时取最近的屏。
    const ScreenPiece& pieceAt(const QPointF& pt) const;

    // 绝对物理点所在的屏（窗口矩形用）；不在任何屏内时取中心最近的屏。
    const ScreenPiece& pieceAtAbs(double x, double y) const;

    // 逻辑点 -> 画布像素坐标。
    QPointF physOf(const QPointF& pt) const;

    // 逻辑坐标 -> 绝对物理坐标（Win32/UIA 用，不带画布原点偏移）。
    QPointF absOf(const QPointF& pt) const;

    // 逻辑矩形 -> 画布整数矩形：四角各自按所在屏换算后**取包围盒**。
    // 注意这是包围盒近似：选区跨过 DPI 比例不同的屏时，真实映射在交界处有
    // 跳变，包围盒会把中间那段拉伸/压缩几像素。早先这条注释写的是
    // 「跨屏选区每段的行数由各自比例决定」（即分段仿射），代码从没这么实现过，
    // 注释会让人以为混合 DPI 跨界截图已经处理好了。混合 DPI 下跨界框选本身是
    // 边缘场景，保留这个近似即可，但别再声称做到了没做的事。
    QRect rectOf(const QRectF& r) const;

    // 选区里占面积最大的那块屏的比例；无重叠屏时为 1.0。
    double dprOf(const QRectF& r) const;

    // 绝对物理矩形 -> 逻辑矩形（按该矩形所在屏的原点与比例）。
    QRectF logicalOfAbsRect(double l, double t, double r, double b) const;
};

// 按当前屏幕组合构建分段映射；检测不到任何屏幕时返回空的映射。
DesktopMap buildMap();

// 所有屏幕几何的逻辑包围盒（可能含负坐标）。
QRect virtualBounds();

// 抓取全部屏幕并合成物理虚拟桌面画布（BitBlt，每屏原生像素 1:1）。
// 返回 (画布 QImage, 换算映射)；抓取失败返回空图 + 映射。
std::pair<QImage, DesktopMap> grabDesktop();

}  // namespace zpin::capture
