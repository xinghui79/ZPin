#include "prefs_icons.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QSvgRenderer>

namespace zpin::prefsicon {
namespace {

// 字形画在 40x40 设计坐标里（内容留边 10），靠 viewBox 缩放到目标尺寸，
// 描边随之缩放——20px 瓦片上约 1pt，正是 macOS 侧栏图标的观感。
// 点状笔画用「零长度路径 + round linecap」（`M20 26.6h.01`）画成圆点，
// 省掉单独的 fill 声明，和线稿共用同一个 <g> 样式。
const char* pathsFor(Glyph glyph) {
    switch (glyph) {
    case Glyph::General:
        // 「常规」用滑块而不是齿轮：齿轮的细齿在 20px 下会糊成一团，
        // 而三条带帽滑杆在任何尺寸都读得出「可调节的设置」。
        return "<path d=\"M11 13.5h18M11 20h18M11 26.5h18\"/>"
               "<circle cx=\"16.5\" cy=\"13.5\" r=\"2.7\" fill=\"#FFFFFF\" stroke=\"none\"/>"
               "<circle cx=\"24.5\" cy=\"20\" r=\"2.7\" fill=\"#FFFFFF\" stroke=\"none\"/>"
               "<circle cx=\"18.5\" cy=\"26.5\" r=\"2.7\" fill=\"#FFFFFF\" stroke=\"none\"/>";
    case Glyph::Capture:
        return "<path d=\"M11 15.5V11h4.5M24.5 11H29v4.5M29 24.5V29h-4.5M15.5 29H11v-4.5\"/>";
    case Glyph::Pin:
        // 两层叠放的菱形，不是大头针。贴图的核心行为是「置顶常驻」，两层图形
        // 直接说这件事，比硬画图钉更简；只有 2 笔，比上一版（圆头+横肩+针，
        // 3 个元素）干净得多。
        //
        // 走过两版弯路，结论记在这免得回头再犯：
        //   1) 圆点 + 一根竖线：全侧栏最细，20px 下读成「T」/棒棒糖（用户实测
        //      「太细了、很奇怪」）。
        //   2) 圆点 + 横肩 + 竖线：补了肩才立住，但 3 个元素偏 busy，而且和
        //      「关于」（圆点 + 竖线）同构，两个瓦片并排看几乎一样。
        // 坐标按名义框校过：菱形 10..30、下沿 10.6..29.4，纵向 11.2..28。
        return "<path d=\"M20 12.2 29 17.6 20 23 11 17.6z\"/>"
               "<path d=\"M11.6 22.4 20 27 28.4 22.4\"/>";
    case Glyph::Output:
        return "<path d=\"M20 10.5V24M14.6 18.8L20 24.4l5.4-5.6M11 29.5h18\"/>";
    case Glyph::Hotkey:
        return "<rect x=\"9\" y=\"13.5\" width=\"22\" height=\"13\" rx=\"3\"/>"
               "<path d=\"M13.5 17.6h.01M20 17.6h.01M26.5 17.6h.01M15 22.5h10\"/>";
    case Glyph::Help:
        return "<path d=\"M16.2 16.1a4.1 4.1 0 1 1 7.9 1.6c-.7 1.9-4.1 2.3-4.1 4.6\"/>"
               "<path d=\"M20 26.6h.01\"/>";
    case Glyph::About:
        // i 点用实心圆（比描边点略大）：20px 下描边点只有 1px 半径，
        // 和竖笔比起来头轻脚重
        return "<circle cx=\"20\" cy=\"12\" r=\"1.5\" fill=\"#FFFFFF\" stroke=\"none\"/>"
               "<path d=\"M20 16.4v9.2\"/>";
    }
    return "";
}

// 每个导航项一色，取 Apple 系统色板（systemGray/Blue/Purple/Green/Indigo/
// Teal），饱和度压在「鲜明但不荧光」的档位，白字形对比够。
QColor baseFor(Glyph glyph) {
    switch (glyph) {
    case Glyph::General:
        return QColor("#8E8E93");
    case Glyph::Capture:
        return QColor("#0A7CFF");
    case Glyph::Pin:
        return QColor("#AF52DE");
    case Glyph::Output:
        return QColor("#2CB457");
    case Glyph::Hotkey:
        return QColor("#5856D6");
    case Glyph::Help:
        return QColor("#30B0C7");
    case Glyph::About:
        return QColor("#8E8E93");
    }
    return {};
}

QString svgFor(Glyph glyph, int px) {
    const QColor base = baseFor(glyph);
    // 纯平配方：只留一道几乎看不出的纵向明暗（顶亮底沉）撑体积感，没有
    // 高光层和白色内描边——那两层是老 iOS 的拟物光泽，正是「emoji 感」的
    // 来源；现代 macOS 的侧栏瓦片就是干净的一片色。
    return QStringLiteral(R"svg(<svg width="%1" height="%1" viewBox="0 0 40 40" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="%2"/>
      <stop offset="1" stop-color="%3"/>
    </linearGradient>
  </defs>
  <rect x="0" y="0" width="40" height="40" rx="9" fill="url(#bg)"/>
  <g fill="none" stroke="#FFFFFF" stroke-width="2"
     stroke-linecap="round" stroke-linejoin="round">%4</g>
</svg>)svg")
        .arg(px)
        .arg(base.lighter(106).name(), base.darker(108).name(),
             QString::fromLatin1(pathsFor(glyph)));
}

}  // namespace

QPixmap tile(Glyph glyph, int px, int scale) {
    QSvgRenderer renderer(svgFor(glyph, px * scale).toUtf8());
    QImage img(px * scale, px * scale, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    renderer.render(&p);
    p.end();
    QPixmap pm = QPixmap::fromImage(img);
    // 逻辑尺寸仍是 px：排版按 px 走，像素密度交给 DPR
    pm.setDevicePixelRatio(scale);
    return pm;
}

QIcon icon(Glyph glyph) {
    QIcon out;
    out.addPixmap(tile(glyph, 20, 1));
    out.addPixmap(tile(glyph, 20, 2));
    return out;
}

QPixmap chevron(bool up, const QColor& color, const QSize& px) {
    constexpr int kSupersample = 2;
    QPixmap pm(px * kSupersample);
    pm.setDevicePixelRatio(kSupersample);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const qreal w = px.width(), h = px.height();
    const qreal hi = h * 0.38, lo = h * 0.62;
    p.drawPolyline(QPolygonF() << QPointF(w * 0.24, up ? lo : hi)
                               << QPointF(w * 0.5, up ? hi : lo)
                               << QPointF(w * 0.76, up ? lo : hi));
    p.end();
    return pm;
}

QIcon swatch(const QColor& color, int px) {
    QPixmap pm(px * 2, px * 2);
    pm.setDevicePixelRatio(2);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    // 圆角裁切：色块直接是圆角片，弹层里就不需要再套一层按钮边框
    QPainterPath chip;
    chip.addRoundedRect(QRectF(0, 0, px, px), px * 0.22, px * 0.22);
    p.setClipPath(chip);
    const int cell = qMax(2, px / 4);
    for (int y = 0; y * cell < px; ++y)
        for (int x = 0; x * cell < px; ++x)
            p.fillRect(x * cell, y * cell, cell, cell,
                       ((x + y) % 2) ? QColor("#E6E6EA") : QColor("#FFFFFF"));
    p.fillRect(0, 0, px, px, color);
    p.setClipping(false);
    // 细描边：白色色片压在白色弹层上会整个消失，没有这圈边就看不见它
    p.setPen(QPen(QColor(0, 0, 0, 40), 1.0));
    p.setBrush(Qt::BrushStyle::NoBrush);
    QPainterPath edge;
    edge.addRoundedRect(QRectF(0.5, 0.5, px - 1.0, px - 1.0), px * 0.22, px * 0.22);
    p.drawPath(edge);
    p.end();
    return QIcon(pm);
}

}  // namespace zpin::prefsicon
