#include "rcore.hpp"

#include <cstring>
#include <vector>

#include "logging.hpp"
#include "rust/cxx.h"
#include "zpin-core/src/lib.rs.h"
namespace zpin::rcore {

namespace {
QImage captureImpl(int x, int y, int w, int h, bool plain) {
    if (w <= 0 || h <= 0)
        return {};
    const rust::Vec<uint8_t> data =
        plain ? zpin::capture_bgra_plain(x, y, w, h) : zpin::capture_bgra(x, y, w, h);
    const qsizetype bytes = qsizetype(w) * h * 4;
    if (data.size() < size_t(bytes))
        return {};
    // GDI 32bpp 内存布局 B,G,R,x = QImage Format_RGB32 的小端字节序
    QImage img(w, h, QImage::Format_RGB32);
    std::memcpy(img.bits(), data.data(), size_t(bytes));
    return img;
}
}  // namespace

QImage captureBgra(int x, int y, int w, int h) {
    return captureImpl(x, y, w, h, false);
}

QImage captureBgraPlain(int x, int y, int w, int h) {
    return captureImpl(x, y, w, h, true);
}

QImage blurImage(const QImage& img) {
    const QImage pm = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int w = pm.width(), h = pm.height();
    if (w <= 0 || h <= 0)
        return {};
    // Qt 缓冲当只读切片直接递过去（旧写法先拷一份 std::vector，1080p 多 8MB 白搬运）
    const rust::Slice<const uint8_t> src(
        reinterpret_cast<const uint8_t*>(pm.constBits()), size_t(pm.sizeInBytes()));
    const rust::Vec<uint8_t> out =
        zpin::blur_bgra(src, size_t(w), size_t(h), size_t(qBound(4, qMin(w, h) / 64, 64)));
    const qsizetype bytes = qsizetype(w) * h * 4;
    if (out.size() < size_t(bytes))
        return {};
    QImage result(w, h, QImage::Format_ARGB32_Premultiplied);
    std::memcpy(result.bits(), out.data(), size_t(bytes));
    return result;
}

std::optional<QString> ocrText(const QImage& img) {
    const QImage pm = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (pm.width() <= 0 || pm.height() <= 0)
        return std::nullopt;
    const rust::Slice<const uint8_t> src(
        reinterpret_cast<const uint8_t*>(pm.constBits()), size_t(pm.sizeInBytes()));
    try {
        const rust::String text =
            zpin::ocr_bgra(src, size_t(pm.width()), size_t(pm.height()));
        return QString::fromUtf8(text.data(), qsizetype(text.size()));
    } catch (const rust::Error& e) {
        log::warn("zpin.ocr", QStringLiteral("识别失败：%1")
                                  .arg(QString::fromUtf8(e.what())));
        return std::nullopt;
    }
}

std::optional<QVector<OcrBox>> ocrBoxes(const QImage& img) {
    const QImage pm = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (pm.width() <= 0 || pm.height() <= 0)
        return std::nullopt;
    const rust::Slice<const uint8_t> src(
        reinterpret_cast<const uint8_t*>(pm.constBits()), size_t(pm.sizeInBytes()));
    try {
        const rust::Vec<zpin::OcrTextBox> lines =
            zpin::ocr_boxes(src, size_t(pm.width()), size_t(pm.height()));
        QVector<OcrBox> out;
        out.reserve(qsizetype(lines.size()));
        for (const zpin::OcrTextBox& line : lines) {
            out.append({QRect(line.x, line.y, line.w, line.h),
                        QString::fromUtf8(line.text.data(), qsizetype(line.text.size()))});
        }
        return out;
    } catch (const rust::Error& e) {
        log::warn("zpin.ocr", QStringLiteral("识别失败：%1")
                                  .arg(QString::fromUtf8(e.what())));
        return std::nullopt;
    }
}

std::optional<QVector<OcrTable>> ocrTables(const QImage& img) {
    const QImage pm = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (pm.width() <= 0 || pm.height() <= 0)
        return std::nullopt;
    const rust::Slice<const uint8_t> src(
        reinterpret_cast<const uint8_t*>(pm.constBits()), size_t(pm.sizeInBytes()));
    try {
        const rust::Vec<zpin::OcrTable> found =
            zpin::ocr_tables(src, size_t(pm.width()), size_t(pm.height()));
        QVector<OcrTable> out;
        out.reserve(qsizetype(found.size()));
        for (const zpin::OcrTable& t : found) {
            out.append({QString::fromUtf8(t.html.data(), qsizetype(t.html.size())),
                        QString::fromUtf8(t.text.data(), qsizetype(t.text.size()))});
        }
        return out;
    } catch (const rust::Error& e) {
        log::warn("zpin.table", QStringLiteral("表格识别失败：%1")
                                   .arg(QString::fromUtf8(e.what())));
        return std::nullopt;
    }
}

std::optional<QRect> contentRect(const QImage& src, const QPoint& seed, int merge, int trust) {
    // 底图是 ARGB32（内存布局 B,G,R,x，与 ARGB32_Premultiplied 同构且全不透明），
    // 直接原样读，省掉每次 hover 一次几 MB 的白拷贝
    const QImage pm = src.format() == QImage::Format_ARGB32_Premultiplied
                            || src.format() == QImage::Format_ARGB32
                            || src.format() == QImage::Format_RGB32
                          ? src
                          : src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (pm.width() < 3 || pm.height() < 3 || seed.x() < 0 || seed.y() < 0 ||
        seed.x() >= pm.width() || seed.y() >= pm.height())
        return std::nullopt;
    const rust::Slice<const uint8_t> buf(
        reinterpret_cast<const uint8_t*>(pm.constBits()), size_t(pm.sizeInBytes()));
    const rust::Vec<int32_t> box = zpin::content_rect(
        buf, size_t(pm.width()), size_t(pm.height()), size_t(seed.x()), size_t(seed.y()),
        size_t(qBound(1, merge, 8)), size_t(trust));
    if (box.size() < 4)
        return std::nullopt;   // 空白处 / 块太小 / 被取景框切断 / 铺满取景框
    return QRect(box[0], box[1], int(box[2] - box[0]), int(box[3] - box[1]));
}

std::optional<ElementInfo> elementFromPoint(int x, int y) {
    try {
        const zpin::ElementInfo e = zpin::element_from_point(x, y);
        if (e.w < 1 || e.h < 1)
            return std::nullopt;
        ElementInfo out;
        out.rect = QRect(e.x, e.y, int(e.w), int(e.h));
        return out;
    } catch (const rust::Error&) {
        return std::nullopt;   // 无命中 / 离屏 / UIA 失败
    }
}

QVector<WindowRect> visibleWindowRects(const QVector<qintptr>& exclude) {
    std::vector<rust::isize> excl;
    excl.reserve(size_t(exclude.size()));
    for (qintptr h : exclude)
        excl.push_back(rust::isize(h));
    rust::Vec<zpin::WindowRect> rects = zpin::visible_window_rects(excl);
    QVector<WindowRect> out;
    out.reserve(qsizetype(rects.size()));
    for (const zpin::WindowRect& r : rects)
        out.append({qintptr(r.hwnd), int(r.l), int(r.t), int(r.r), int(r.b)});
    return out;
}

namespace {
// cxx 的 rust::Str 要 (指针, 字节数)；QString 先落 UTF-8 再递。
rust::Str asStr(const QByteArray& bytes) {
    return rust::Str(bytes.constData(), size_t(bytes.size()));
}
}  // namespace

QByteArray httpGet(const QString& url, const QString& headers) {
    const QByteArray u = url.toUtf8();
    const QByteArray h = headers.toUtf8();
    const rust::Vec<uint8_t> body = zpin::http_get(asStr(u), asStr(h));
    return QByteArray(reinterpret_cast<const char*>(body.data()), qsizetype(body.size()));
}

qint64 downloadToFile(const QString& url, const QString& dest, const QString& headers) {
    const QByteArray u = url.toUtf8();
    const QByteArray d = dest.toUtf8();
    const QByteArray h = headers.toUtf8();
    return qint64(zpin::download_to_file(asStr(u), asStr(d), asStr(h)));
}

// ---- 滚动长截图拼接 ----

struct ScrollStitcher::Impl {
    explicit Impl(rust::Box<zpin::StitchSession> s) : session(std::move(s)) {}
    rust::Box<zpin::StitchSession> session;
};

ScrollStitcher::ScrollStitcher(int width, int frameH, int maxH)
    : m_impl(std::make_unique<Impl>(zpin::stitch_session_new(size_t(width), size_t(frameH),
                                                             size_t(maxH)))),
      m_width(width) {}

ScrollStitcher::~ScrollStitcher() = default;

ScrollStitcher::Step ScrollStitcher::push(const QImage& frame) {
    const rust::Slice<const uint8_t> src(reinterpret_cast<const uint8_t*>(frame.constBits()),
                                         size_t(frame.sizeInBytes()));
    const zpin::StitchStep st = m_impl->session->push(src);
    return {st.offset, st.canvas_h, st.done, st.frozen, int(st.sticky_px), st.rebased};
}

QImage ScrollStitcher::takeCanvas() {
    const int h = m_impl->session->canvas_height();
    const rust::Vec<uint8_t> data = m_impl->session->take_canvas();
    const qsizetype bytes = qsizetype(m_width) * h * 4;
    if (h <= 0 || data.size() < size_t(bytes))
        return {};
    QImage img(m_width, h, QImage::Format_RGB32);
    std::memcpy(img.bits(), data.data(), size_t(bytes));
    return img;
}

QImage ScrollStitcher::preview(int thumbWidth, int maxThumbHeight) const {
    const int h = m_impl->session->canvas_height();
    if (h <= 0 || m_width <= 0)
        return {};
    const rust::Slice<const uint8_t> data = m_impl->session->canvas();
    const qsizetype bytes = qsizetype(m_width) * h * 4;
    if (data.size() < size_t(bytes))
        return {};
    // 直接包住 Rust 画布零拷贝：早先 memcpy 整幅画布（到 2 万 px 高时一次
    // 150MB+）只为喂一次 scaled。canvas() 的缓冲在本次调用内不会被推进
    // （push 与 preview 同在捕获线程串行），scaled 产出的小图自持内存。
    const QImage img(reinterpret_cast<const uchar*>(data.data()), m_width, h,
                     qsizetype(m_width) * 4, QImage::Format_RGB32, nullptr, nullptr);
    QSize target(thumbWidth, h * thumbWidth / m_width);
    if (target.height() > maxThumbHeight)
        target = QSize(qMax(1, m_width * maxThumbHeight / h), maxThumbHeight);
    return img.scaled(target, Qt::AspectRatioMode::KeepAspectRatio,
                      Qt::TransformationMode::SmoothTransformation);
}

int ScrollStitcher::canvasHeight() const {
    return m_impl->session->canvas_height();
}

}  // namespace zpin::rcore
