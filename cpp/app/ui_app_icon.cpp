#include "ui_app_icon.hpp"

#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>
#include <QtGlobal>

namespace zpin::appicon {

namespace {

// 主色（正常态）：品牌蓝纵向渐变；停用态：整底换琥珀渐变 + 深蓝描边——
// 托盘 16px 下只换描边色几乎看不出状态，底色变掉才是可靠的状态信号。
const char* kSvgTmpl = R"svg(<svg width="256" height="256" viewBox="0 0 256 256" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="%1"/>
      <stop offset="1" stop-color="%2"/>
    </linearGradient>
    <linearGradient id="gloss" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#FFFFFF" stop-opacity="0.30"/>
      <stop offset="0.45" stop-color="#FFFFFF" stop-opacity="0.05"/>
      <stop offset="1" stop-color="#FFFFFF" stop-opacity="0"/>
    </linearGradient>
  </defs>
  <rect x="10" y="10" width="236" height="236" rx="58" fill="url(#bg)"/>
  <rect x="10" y="10" width="236" height="236" rx="58" fill="url(#gloss)"/>
  <rect x="11.5" y="11.5" width="233" height="233" rx="56.5"
        fill="none" stroke="#FFFFFF" stroke-opacity="0.28" stroke-width="3"/>
  <g fill="none" stroke="%3" stroke-width="14"
     stroke-linecap="round" stroke-linejoin="round">
    <path d="M 62 102 L 62 84 A 22 22 0 0 1 84 62 L 102 62"/>
    <path d="M 194 154 L 194 172 A 22 22 0 0 1 172 194 L 154 194"/>
  </g>
  <g fill="none" stroke="%3" stroke-width="14"
     stroke-linecap="round" stroke-linejoin="round">
    <path d="M 92 98 L 164 98 L 92 158 L 164 158"/>
  </g>
</svg>)svg";

QString svgFor(bool disabled) {
    if (disabled)
        return QString(kSvgTmpl).arg("#FFC93C", "#F0A32B", "#1E3C8C");
    return QString(kSvgTmpl).arg("#59A2FF", "#1E56C8", "#FFFFFF");
}

QImage render(int size, const QString& svg) {
    QImage img(size, size, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    QSvgRenderer renderer(svg.toUtf8());
    renderer.render(&p, QRectF(0, 0, size, size));
    p.end();
    return img;
}

constexpr int kSizes[] = {16, 24, 32, 48, 64, 128, 256};

}  // namespace

QIcon appIcon(bool disabled) {
    const QString svg = svgFor(disabled);
    QIcon icon;
    for (int s : kSizes)
        icon.addPixmap(QPixmap::fromImage(render(s, svg)));
    return icon;
}

bool writeIco(const QString& path) {
    // PNG 压缩条目，Vista 及以上有效
    struct Entry {
        int size;
        QByteArray png;
    };
    QVector<Entry> entries;
    const QString svg = svgFor(false);
    for (int s : kSizes) {
        QBuffer buf;
        buf.open(QIODevice::WriteOnly);
        render(s, svg).save(&buf, "PNG");
        entries.append({s, buf.data()});
    }
    QByteArray out;
    auto u16 = [](int v) {
        QByteArray b(2, '\0');
        b[0] = char(v & 0xFF);
        b[1] = char((v >> 8) & 0xFF);
        return b;
    };
    auto u32 = [](int v) {
        QByteArray b(4, '\0');
        b[0] = char(v & 0xFF);
        b[1] = char((v >> 8) & 0xFF);
        b[2] = char((v >> 16) & 0xFF);
        b[3] = char((v >> 24) & 0xFF);
        return b;
    };
    out.append(u16(0));
    out.append(u16(1));  // type: icon
    out.append(u16(entries.size()));
    int offset = 6 + 16 * entries.size();
    for (const Entry& e : entries) {
        const char side = e.size >= 256 ? 0 : char(e.size);
        out.append(side);
        out.append(side);
        out.append(char(0));
        out.append(char(0));
        out.append(u16(1));   // planes
        out.append(u16(32));  // bpp
        out.append(u32(e.png.size()));
        out.append(u32(offset));
        offset += e.png.size();
    }
    for (const Entry& e : entries)
        out.append(e.png);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(out);
    return true;
}

}  // namespace zpin::appicon
