#include "capture.hpp"

#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QScreen>
#include <limits>
#include <utility>

#include "logging.hpp"
#include "rcore.hpp"
#include "win32util.hpp"

namespace zpin::capture {

namespace {
const ScreenPiece kEmptyPiece{{}, {}, 1.0};
}  // namespace

const ScreenPiece& DesktopMap::pieceAt(const QPointF& pt) const {
    for (const ScreenPiece& pc : pieces) {
        // 用 QRectF 判：logical 是逻辑坐标，混合 DPI 的屏缝上坐标带小数，
        // toPoint() 截断会把缝上那点分到隔壁屏（拿错 dpr 与偏移）
        if (QRectF(pc.logical).contains(pt))
            return pc;
    }
    if (pieces.empty())
        return kEmptyPiece;
    const ScreenPiece* best = &pieces.front();
    double bestDist = std::numeric_limits<double>::max();
    for (const ScreenPiece& pc : pieces) {
        const QPointF c = QRectF(pc.logical).center();
        const double d = qAbs(c.x() - pt.x()) + qAbs(c.y() - pt.y());
        if (d < bestDist) {
            bestDist = d;
            best = &pc;
        }
    }
    return *best;
}

const ScreenPiece& DesktopMap::pieceAtAbs(double x, double y) const {
    const QPointF p(x, y);
    for (const ScreenPiece& pc : pieces) {
        if (QRectF(pc.absPhys).contains(p))
            return pc;
    }
    if (pieces.empty())
        return kEmptyPiece;
    const ScreenPiece* best = &pieces.front();
    double bestDist = std::numeric_limits<double>::max();
    for (const ScreenPiece& pc : pieces) {
        const QPointF c = QRectF(pc.absPhys).center();
        const double d = qAbs(c.x() - x) + qAbs(c.y() - y);
        if (d < bestDist) {
            bestDist = d;
            best = &pc;
        }
    }
    return *best;
}

QPointF DesktopMap::physOf(const QPointF& pt) const {
    const ScreenPiece& pc = pieceAt(pt);
    return QPointF((pt.x() - pc.logical.left()) * pc.dpr + pc.absPhys.left(),
                   (pt.y() - pc.logical.top()) * pc.dpr + pc.absPhys.top()) -
           QPointF(origin);
}

QPointF DesktopMap::absOf(const QPointF& pt) const {
    return physOf(pt) + QPointF(origin);
}

QRect DesktopMap::rectOf(const QRectF& r) const {
    double l = std::numeric_limits<double>::max();
    double t = l, rr = -l, bb = -l;
    for (const QPointF& c : {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight()}) {
        const QPointF p = physOf(c);
        l = qMin(l, p.x());
        t = qMin(t, p.y());
        rr = qMax(rr, p.x());
        bb = qMax(bb, p.y());
    }
    return QRect(QPoint(qRound(l), qRound(t)),
                 QSize(qRound(rr - l), qRound(bb - t)));
}

double DesktopMap::dprOf(const QRectF& r) const {
    // area 从 0 起算：与任何屏都没有重叠面积时选不出 best，落到下面 1.0 兜底。
    // 从 -1 起算的话，「第一块屏的 0 重叠」也会胜出，把完全离屏的矩形误报成它的比例。
    double best = 0.0, area = 0.0;
    for (const ScreenPiece& pc : pieces) {
        const QRectF inter = r.intersected(QRectF(pc.logical));
        const double a = inter.width() * inter.height();
        if (a > area) {
            area = a;
            best = pc.dpr;
        }
    }
    return best > 0 ? best : 1.0;
}

QRectF DesktopMap::logicalOfAbsRect(double l, double t, double r, double b) const {
    // 用 left+width 而不是 QRectF::right()：后者是"最右像素下标"，会整体偏 1px
    const ScreenPiece& pc = pieceAtAbs((l + r) / 2.0, (t + b) / 2.0);
    const double d = pc.dpr > 0 ? pc.dpr : 1.0;
    return QRectF((l - pc.absPhys.left()) / d + pc.logical.left(),
                  (t - pc.absPhys.top()) / d + pc.logical.top(),
                  (r - l) / d, (b - t) / d);
}

DesktopMap buildMap() {
    DesktopMap mp;
    // 显示器物理矩形走 win32util（Win32 边界，AGENTS 规则 5）
    const QHash<QString, QRect> mons = win32::monitorRects();
    for (QScreen* s : QGuiApplication::screens()) {
        ScreenPiece pc;
        pc.logical = s->geometry();
        pc.dpr = s->devicePixelRatio() > 0 ? s->devicePixelRatio() : 1.0;
        const auto it = mons.constFind(s->name());
        if (it != mons.constEnd()) {
            pc.absPhys = it.value();
        } else {
            log::warn("capture", QString("显示器 %1 没匹配到物理矩形，回退 逻辑x%2")
                                     .arg(s->name()).arg(pc.dpr, 0, 'f', 3));
            pc.absPhys = QRect(qRound(pc.logical.x() * pc.dpr),
                               qRound(pc.logical.y() * pc.dpr),
                               qRound(pc.logical.width() * pc.dpr),
                               qRound(pc.logical.height() * pc.dpr));
        }
        mp.pieces.push_back(pc);
    }
    if (mp.pieces.empty()) {
        mp.canvas = QRect();
        mp.origin = QPoint();
        return mp;
    }
    int ox = std::numeric_limits<int>::max(), oy = ox;
    int rx = std::numeric_limits<int>::min(), ry = rx;
    for (const ScreenPiece& pc : mp.pieces) {
        ox = qMin(ox, pc.absPhys.left());
        oy = qMin(oy, pc.absPhys.top());
        rx = qMax(rx, pc.absPhys.left() + pc.absPhys.width());
        ry = qMax(ry, pc.absPhys.top() + pc.absPhys.height());
    }
    mp.origin = QPoint(ox, oy);
    mp.canvas = QRect(0, 0, rx - ox, ry - oy);
    return mp;
}

QRect virtualBounds() {
    QRect bounds;
    for (QScreen* s : QGuiApplication::screens())
        bounds = bounds.united(s->geometry());
    return bounds;
}

std::pair<QImage, DesktopMap> grabDesktop() {
    const DesktopMap mp = buildMap();
    // 画布用 RGB32（不透明）：贴 RGB32 抓图时 drawImage 走纯拷贝路径，省掉
    // 每屏一次整屏格式转换；桌面照片本来也没有 alpha 可言。
    QImage canvas(qMax(1, mp.canvas.width()), qMax(1, mp.canvas.height()),
                  QImage::Format_RGB32);
    canvas.fill(0xFF202020);
    QPainter p(&canvas);
    const QList<QScreen*> screens = QGuiApplication::screens();
    int blitted = 0;
    for (size_t i = 0; i < mp.pieces.size() && i < size_t(screens.size()); ++i) {
        const ScreenPiece& pc = mp.pieces[i];
        // 每屏按自己的绝对物理矩形整块 BitBlt，原生像素 1:1，不做重采样
        const QImage img = rcore::captureBgra(pc.absPhys.x(), pc.absPhys.y(),
                                              pc.absPhys.width(), pc.absPhys.height());
        if (img.isNull()) {
            log::warn("capture", QString("屏幕 %1 抓取失败，该区域留底色").arg(screens[i]->name()));
            continue;
        }
        const QRect dst(pc.absPhys.topLeft() - mp.origin, img.size());
        if (img.size() != pc.absPhys.size()) {
            // 抓取图与显示器矩形对不上时以原生抓取尺寸为准：宁可留边，也不缩放
            log::warn("capture", QString("屏幕 %1 抓取 %2x%3 与矩形 %4x%5 不一致，按抓取尺寸贴")
                                     .arg(screens[i]->name())
                                     .arg(img.width()).arg(img.height())
                                     .arg(pc.absPhys.width()).arg(pc.absPhys.height()));
        }
        p.drawImage(dst, img);
        ++blitted;
    }
    p.end();
    // 一块屏都没贴上就是抓取失败：返回空图，让调用方那句「截图失败」的气泡真的有
    // 机会弹。早先这里恒返回填过底色的画布（最差 1x1），全屏抓失败时用户得到的
    // 是一张纯黑截图并提示「已复制到剪贴板」，而 capture.hpp 承诺的「失败返回空图」
    // 与各调用方的 isNull() 判断全成了死代码。
    if (blitted == 0) {
        log::warn("capture", QStringLiteral("没有任何屏幕被抓取（屏数 %1）")
                                 .arg(screens.size()));
        return {};
    }
    return {canvas, mp};
}

}  // namespace zpin::capture
