#include "shapes.hpp"

#include <QBuffer>
#include <QJsonArray>
#include <QJsonObject>
#include <QtMath>

namespace zpin {

double segDist(const QPointF& pt, const QPointF& a, const QPointF& b) {
    const double vx = b.x() - a.x(), vy = b.y() - a.y();
    const double wx = pt.x() - a.x(), wy = pt.y() - a.y();
    const double l2 = vx * vx + vy * vy;
    if (l2 == 0)
        return std::hypot(wx, wy);
    const double t = qBound(0.0, (wx * vx + wy * vy) / l2, 1.0);
    return std::hypot(pt.x() - (a.x() + t * vx), pt.y() - (a.y() + t * vy));
}

bool Shape::hit(const QPointF&, double) const {
    return false;
}

// ---- Stroke ----

Stroke::Stroke(const QColor& c, double w, bool hl) : Shape(c, w), highlight(hl) {}

void Stroke::draw(QPainter& p) {
    if (points.isEmpty())
        return;
    QColor c = color;
    if (highlight)
        c.setAlpha(128);
    QPen pen(c, penWidth());
    pen.setCapStyle(Qt::PenCapStyle::RoundCap);
    pen.setJoinStyle(Qt::PenJoinStyle::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    if (points.size() == 1)
        p.drawPoint(points[0]);
    else
        p.drawPolyline(QPolygonF(points));
}

bool Stroke::hit(const QPointF& pt, double tol) const {
    const double lim = qMax(tol, penWidth() / 2 + tol / 2);
    for (int i = 0; i + 1 < points.size(); ++i) {
        if (segDist(pt, points[i], points[i + 1]) <= lim)
            return true;
    }
    return !points.isEmpty() &&
           std::hypot(pt.x() - points.last().x(), pt.y() - points.last().y()) <= lim;
}

void Stroke::translate(double dx, double dy) {
    for (QPointF& q : points)
        q += QPointF(dx, dy);
}

QRectF Stroke::boundingRect() const {
    if (points.isEmpty())
        return QRectF();
    double minx = points[0].x(), maxx = minx, miny = points[0].y(), maxy = miny;
    for (const QPointF& q : points) {
        minx = qMin(minx, q.x());
        maxx = qMax(maxx, q.x());
        miny = qMin(miny, q.y());
        maxy = qMax(maxy, q.y());
    }
    const double pad = penWidth() / 2 + 2;
    return QRectF(minx - pad, miny - pad, maxx - minx + pad * 2, maxy - miny + pad * 2);
}

// ---- Mosaic / Blur / SmartErase ----

namespace {
// 像素级区域共用的绘制：把整张特效底图按原位画上去，只让框内露出来。
// 特效图铺满画布且钉在画布坐标（引擎按整张 base 生成并缓存），所以裁剪区
// 框到哪就处理哪一块——这也正是它比智能擦除更「耐重新裁剪」的原因。
void drawPixelRegion(QPainter& p, const Rect& r, const QImage* effect) {
    if (!effect || effect->isNull())
        return;
    const QRectF box = r.rect();
    if (box.width() < 1 || box.height() < 1)
        return;
    p.save();
    // 必须取交集：选区侧外层已把标注层裁进选区框（AnnotationController::draw），
    // 这里默认的 ReplaceClip 会把外层裁剪整个换成自己的框——马赛克/模糊拖出
    // 选区后特效画到遮罩上，成了唯一能越界的工具。烘焙/贴图侧没显式裁剪时
    // 交集就是设备边界，行为不变。
    p.setClipRect(box, Qt::ClipOperation::IntersectClip);
    p.drawImage(QPointF(0, 0), *effect);
    p.restore();
}
}  // namespace

bool PixelRegion::hit(const QPointF& pt, double tol) const {
    // 框内整块都算命中：这里框住的是被处理的「面」，不是一条描边。
    // Rect::hit 只判边框环带（那是给只描边的矩形框用的），点在框内空白处
    // 会抓不住，用户会以为这块马赛克删不掉。
    return rect().adjusted(-tol, -tol, tol, tol).contains(pt);
}

Mosaic::Mosaic() = default;

void Mosaic::draw(QPainter& p) {
    drawPixelRegion(p, *this, pixelated);
}

Blur::Blur() = default;

void Blur::draw(QPainter& p) {
    drawPixelRegion(p, *this, blurred);
}

SmartErase::~SmartErase() {
    // 形状没了（在撤销栈里被移除、被 clear()、或整个会话结束）就让还在跑的
    // PatchMatch 立刻中止：它下一处 checkCancelled 就会抛，被 erase() 的
    // catch 转成空图。cancel 是 shared_ptr，与本对象无关的最后一份引用也会
    // 一并释放，但工作线程自己还攥着，所以这里置位一定传得到。
    cancel->store(true, std::memory_order_relaxed);
}

void SmartErase::draw(QPainter& p) {
    const QRectF r = rect();
    if (r.width() < 1 || r.height() < 1)
        return;
    if (patched.isNull()) {
        // 计算中（后台修复几百毫秒到两秒）：半透明红框标出「这块会重建」，
        // 与源实现的待渲染占位同语义。烘焙撞上这个窗口会把红框烙进图——
        // 窗口极短且框选通常瞬间算完，接受。
        p.save();
        p.setRenderHint(QPainter::RenderHint::Antialiasing, true);
        p.setPen(QPen(QColor(255, 77, 79), 1.5));
        p.setBrush(QColor(255, 77, 79, 51));
        p.drawRect(r);
        p.restore();
        return;
    }
    // 修复结果与底图同尺寸：目标画当前框，源采样计算时刻的框位（patchRect）。
    // 移动后两者错开（正好拿到对的像素），缩放后 Qt 把源区缩放进当前框。
    p.drawImage(r, patched, patchRect);
}

bool SmartErase::validOnFinish() const {
    const QRectF r = rect();
    return qMax(r.width(), r.height()) >= 3.0;
}

// ---- Spotlight ----

QRectF paintBounds(const QPainter& p) {
    // 画布在**当前 painter 坐标系**下的范围。三处绘制路径（选区覆盖层、贴图窗、
    // 成品合成）都先把底图坐标映射到目标坐标，所以把视口按变换反推回去，就是
    // 画布在底图坐标系里的样子。
    const QRectF dev(p.viewport());
    bool invertible = false;
    const QTransform inv = p.worldTransform().inverted(&invertible);
    return invertible ? inv.mapRect(dev) : dev;
}

void Spotlight::draw(QPainter& p) {
    const QRectF hole = rect();
    if (hole.width() < 1 || hole.height() < 1)
        return;
    p.save();
    // 奇偶填充规则（QPainterPath 默认）：外框 + 内框 → 只填两圈之间那一圈
    QPainterPath ring;
    ring.addRect(paintBounds(p));
    ring.addRect(hole);
    p.setPen(Qt::PenStyle::NoPen);
    p.setBrush(QColor(0, 0, 0, 110));  // 与截图遮罩 Capture/mask_color 同一档压暗量
    p.drawPath(ring);
    p.restore();
    drawBorder(p);
}

void Spotlight::drawBorder(QPainter& p) const {
    // 亮区描一道白边：压暗之后没有边界，那块「亮」看着像贴图缺了个角
    const QRectF hole = rect();
    if (hole.width() < 1 || hole.height() < 1)
        return;
    p.save();
    p.setBrush(Qt::BrushStyle::NoBrush);
    p.setPen(QPen(QColor(255, 255, 255, 220), 1.5));
    p.drawRect(hole);
    p.restore();
}

bool Spotlight::hit(const QPointF& pt, double tol) const {
    // 命中亮区（含白边）：用户眼里「聚光灯」就是亮的那块，橡皮擦点亮区即删、
    // 抓取也抓它。早先命中的是框外暗区——点聚光灯没反应、得去点旁边黑处才删得掉，
    // 反直觉。框内后画的图形在栈顶，会先被命中，不挡下面这步。
    return rect().adjusted(-tol, -tol, tol, tol).contains(pt);
}

ShapePtr mosaicForTextLine(const QRect& textRect, const QSize& imageBounds) {
    const QRect r = textRect.adjusted(-3, -3, 3, 3)
                        .intersected(QRect(QPoint(), imageBounds));
    if (r.width() < 6 || r.height() < 4)
        return nullptr;
    // 马赛克现在是框选形状，直接给对角点即可。早先它是 Stroke 自由笔画，这里
    // 只能拿「两点横线 + 线宽=行高」硬凑出一个矩形——为了遮一个文本行专门写
    // 的 workaround，如今可以删了。
    auto mosaic = std::make_shared<Mosaic>();
    mosaic->p1 = QPointF(r.left(), r.top());
    mosaic->p2 = QPointF(r.right() - 1, r.bottom() - 1);
    return mosaic;
}

// ---- Line ----

void Line::draw(QPainter& p) {
    QPen pen(color, width);
    pen.setCapStyle(Qt::PenCapStyle::RoundCap);
    p.setPen(pen);
    p.drawLine(p1, p2);
}

void DashLine::draw(QPainter& p) {
    const double w = qMax(1.0, width);
    QPen pen(color, w);
    pen.setCapStyle(Qt::PenCapStyle::RoundCap);
    // Qt 的虚线样式以「几个线宽」为单位，除以线宽就换成绝对像素：短划与空隙都不随
    // 笔粗变长（写死 4 的话，8 号粗笔一划 32px 就成了断续的粗棒）。
    // 关键是空隙要补偿圆头笔帽：圆帽从每段两端各伸出 w/2，实测 {6/w, 4/w} 在
    // 4 号（默认「中」）和 8 号（「粗」）笔下空隙被吃得一点不剩，画出来就是实线。
    // 按 (5 + w) 给，量到的可见空隙恒为 5px。
    pen.setDashPattern({4.0 / w, (5.0 + w) / w});
    p.setPen(pen);
    p.drawLine(p1, p2);
}

bool Line::hit(const QPointF& pt, double tol) const {
    return segDist(pt, p1, p2) <= qMax(tol, width / 2 + tol / 2);
}

void Line::translate(double dx, double dy) {
    p1 += QPointF(dx, dy);
    p2 += QPointF(dx, dy);
}

void Line::setEndpoints(const QList<QPointF>& pts) {
    if (pts.size() >= 2) {
        p1 = pts[0];
        p2 = pts[1];
    }
}

bool Line::validOnFinish() const {
    const QRectF r = QRectF(p1, p2).normalized();
    return qMax(r.width(), r.height()) >= 3.0;
}

QRectF Line::boundingRect() const {
    const double pad = width / 2 + 2;
    return QRectF(p1, p2).normalized().adjusted(-pad, -pad, pad, pad);
}

// ---- Curve ----

QPainterPath Curve::strokePath() const {
    QPainterPath path;
    path.moveTo(p0);
    path.cubicTo(p1, p2, p3);
    return path;
}

void Curve::draw(QPainter& p) {
    QPen pen(color, width);
    pen.setCapStyle(Qt::PenCapStyle::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::BrushStyle::NoBrush);
    p.drawPath(strokePath());
}

bool Curve::hit(const QPointF& pt, double tol) const {
    // 贝塞尔不解析求距：按参数均匀采样成折线段，按点到段距离判。均匀采样在
    // 控制点张角大的地方段会偏长，24 段对命中检测的粗细足够。
    const double t = qMax(tol, width / 2 + tol / 2);
    QPointF prev = p0;
    constexpr int kSteps = 24;
    for (int i = 1; i <= kSteps; ++i) {
        const double u = double(i) / kSteps;
        const double v = 1.0 - u;
        // 三次贝塞尔的伯恩斯坦展开
        const QPointF cur = v * v * v * p0 + 3.0 * v * v * u * p1 +
                            3.0 * v * u * u * p2 + u * u * u * p3;
        if (segDist(pt, prev, cur) <= t)
            return true;
        prev = cur;
    }
    return false;
}

void Curve::translate(double dx, double dy) {
    const QPointF d(dx, dy);
    p0 += d;
    p1 += d;
    p2 += d;
    p3 += d;
}

bool Curve::validOnFinish() const {
    const QRectF r = QRectF(p0, p3).normalized();
    return qMax(r.width(), r.height()) >= 3.0;
}

QRectF Curve::boundingRect() const {
    const double pad = width / 2 + 2;
    return strokePath().boundingRect().adjusted(-pad, -pad, pad, pad);
}

namespace {
// 实心箭头头部：尖端在 tip，两翼朝线段来向张开（单/双头箭头共用）。
void drawArrowHead(QPainter& p, const QColor& color, const QPointF& tip,
                   const QPointF& from, double size) {
    const double ang = qAtan2(tip.y() - from.y(), tip.x() - from.x());
    QPolygonF head;
    head << tip
         << tip + QPointF(size * qCos(ang + qDegreesToRadians(158.0)),
                          size * qSin(ang + qDegreesToRadians(158.0)))
         << tip + QPointF(size * qCos(ang - qDegreesToRadians(158.0)),
                          size * qSin(ang - qDegreesToRadians(158.0)));
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPolygon(head);
    p.restore();
}
}  // namespace

void Arrow::draw(QPainter& p) {
    Line::draw(p);
    drawArrowHead(p, color, p2, p1, width * 2.5 + 8);
}

void DoubleArrow::draw(QPainter& p) {
    Line::draw(p);
    const double size = width * 2.5 + 8;
    drawArrowHead(p, color, p2, p1, size);
    drawArrowHead(p, color, p1, p2, size);
}

// ---- Rect ----

void Rect::draw(QPainter& p) {
    p.setPen(QPen(color, width));
    p.setBrush(Qt::NoBrush);
    p.drawRect(rect());
}

bool Rect::hit(const QPointF& pt, double tol) const {
    // 只算边框环带，内部空白不算命中
    const QRectF r = rect();
    const QRectF inner = r.adjusted(width + tol, width + tol, -width - tol, -width - tol);
    return r.contains(pt) && !inner.contains(pt);
}

void Rect::translate(double dx, double dy) {
    p1 += QPointF(dx, dy);
    p2 += QPointF(dx, dy);
}

bool Ellipse::hit(const QPointF& pt, double tol) const {
    // 椭圆的边框环带按椭圆方程算：归一化半径落在 1±带内才算命中，外接矩形
    // 四角（Rect::hit 会中的「空气」）不算。带宽容差以短半轴折算，与 Rect
    // 的内缩环带同数量级。
    const QRectF r = rect();
    const double rx = r.width() / 2, ry = r.height() / 2;
    if (rx <= 0 || ry <= 0)
        return Rect::hit(pt, tol);   // 退化成线/点时没有椭圆方程可用
    const double nx = (pt.x() - r.left() - rx) / rx;
    const double ny = (pt.y() - r.top() - ry) / ry;
    const double t = std::sqrt(nx * nx + ny * ny);
    const double band = (width + tol) / qMin(rx, ry);
    return std::abs(t - 1.0) <= band;
}

void Rect::setEndpoints(const QList<QPointF>& pts) {
    if (pts.size() >= 2) {
        p1 = pts[0];
        p2 = pts[1];
    }
}

bool Rect::validOnFinish() const {
    const QRectF r = rect();
    return qMax(r.width(), r.height()) >= 3.0;
}

QRectF Rect::boundingRect() const {
    const double pad = width / 2 + 2;
    return rect().adjusted(-pad, -pad, pad, pad);
}

void Ellipse::draw(QPainter& p) {
    p.setPen(QPen(color, width));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(rect());
}

// ---- TextShape ----

QFont TextShape::font() const {
    QFont f("Microsoft YaHei UI");
    f.setPixelSize(int(font_size));
    f.setWeight(QFont::Weight::DemiBold);
    return f;
}

QFontMetrics TextShape::metrics() const {
    return QFontMetrics(font());
}

std::pair<double, double> TextShape::size() const {
    const QFontMetrics fm = metrics();
    double w = 10;
    for (const QString& t : lines)
        w = qMax(w, double(fm.horizontalAdvance(t)));
    return {w, double(fm.height() * lines.size())};
}

void TextShape::draw(QPainter& p) {
    // 逐行绘制：先描半透明白底再着色，浅色背景下也可读
    p.setFont(font());
    const QFontMetrics fm = metrics();
    const int lineH = fm.height();
    double y = pos.y() + fm.ascent();
    for (const QString& t : lines) {
        p.setPen(QPen(QColor(255, 255, 255, 170), 3));
        p.drawText(QPointF(pos.x(), y), t);
        p.setPen(QPen(color));
        p.drawText(QPointF(pos.x(), y), t);
        y += lineH;
    }
}

bool TextShape::hit(const QPointF& pt, double tol) const {
    const auto [w, h] = size();
    return QRectF(pos.x() - tol, pos.y() - tol, w + 2 * tol, h + 2 * tol).contains(pt);
}

void TextShape::translate(double dx, double dy) {
    pos += QPointF(dx, dy);
}

QRectF TextShape::boundingRect() const {
    const auto [w, h] = size();
    return QRectF(pos.x() - 4, pos.y() - 4, w + 8, h + 8);
}

// ---- StepBadge ----

QFont StepBadge::font() const {
    QFont f("Microsoft YaHei UI");
    f.setPixelSize(qMax(9, int(font_size * 0.72)));
    f.setWeight(QFont::Weight::Bold);
    return f;
}

double StepBadge::radius() const {
    // 圆半径：单字号基准，多位数字时按文字宽度撑大
    const QFontMetrics fm(font());
    return qMax(font_size * 0.72,
                fm.horizontalAdvance(QString::number(number)) / 2.0 + font_size * 0.3);
}

void StepBadge::draw(QPainter& p) {
    const double r = radius();
    p.setPen(QPen(QColor(255, 255, 255, 225), qMax(1.5, r * 0.14)));
    p.setBrush(color);
    p.drawEllipse(pos, r, r);
    p.setFont(font());
    p.setPen(QPen(QColor("#FFFFFF")));
    p.drawText(QRectF(pos.x() - r, pos.y() - r, r * 2, r * 2),
               Qt::AlignmentFlag::AlignCenter, QString::number(number));
}

bool StepBadge::hit(const QPointF& pt, double tol) const {
    // 画的是圆，判定也得是圆：按方框判会在四角「点空气」也把序号选中
    const double r = radius() + tol;
    return std::hypot(pt.x() - pos.x(), pt.y() - pos.y()) <= r;
}

void StepBadge::translate(double dx, double dy) {
    pos += QPointF(dx, dy);
}

QRectF StepBadge::boundingRect() const {
    const double r = radius() + 3;
    return QRectF(pos.x() - r, pos.y() - r, r * 2, r * 2);
}

// ---- Callout ----

namespace {
// 气泡内边距：绘制与命中判定共用，避免「看得见点不着」的偏差。
constexpr double kCalloutPadX = 8, kCalloutPadY = 5;

// 引线终点：画在气泡左边缘中部，命中判定也按同一个点算。
QPointF calloutTailEnd(const Callout& c) {
    return QPointF(c.pos.x(), c.pos.y() + c.font_size * 0.6);
}
}  // namespace

QRectF Callout::bubbleRect() const {
    const auto [w, h] = size();
    return QRectF(pos.x(), pos.y(), w + kCalloutPadX * 2, h + kCalloutPadY * 2);
}

QPointF Callout::textTopLeft() const {
    return pos + QPointF(kCalloutPadX, kCalloutPadY);
}

void Callout::draw(QPainter& p) {
    QPen pen(color, 3);
    pen.setCapStyle(Qt::PenCapStyle::RoundCap);
    p.setPen(pen);
    p.drawLine(tail, calloutTailEnd(*this));
    p.setFont(font());
    const QFontMetrics fm = metrics();
    const QRectF bubble = bubbleRect();
    p.setBrush(QColor(255, 255, 255, 235));
    p.setPen(QPen(color, 2.5));
    p.drawRoundedRect(bubble, 8, 8);
    p.setPen(QPen(color));
    double y = pos.y() + kCalloutPadY;
    for (const QString& t : lines) {
        p.drawText(QRectF(pos.x(), y, bubble.width() - kCalloutPadX * 2, fm.height()),
                   Qt::AlignmentFlag::AlignLeft | Qt::AlignmentFlag::AlignVCenter, t);
        y += fm.height();
    }
}

bool Callout::hit(const QPointF& pt, double tol) const {
    if (bubbleRect().adjusted(-tol, -tol, tol, tol).contains(pt))
        return true;
    return segDist(pt, tail, calloutTailEnd(*this)) <= tol + 2;
}

void Callout::translate(double dx, double dy) {
    tail += QPointF(dx, dy);
    TextShape::translate(dx, dy);
}

void Callout::setEndpoints(const QList<QPointF>& pts) {
    if (pts.size() >= 2) {
        tail = pts[0];
        pos = pts[1];
    }
}

QRectF Callout::boundingRect() const {
    const QRectF box = bubbleRect().adjusted(-12, -12, 4, 22);
    return box.united(QRectF(tail, tail).adjusted(-6, -6, 6, 6));
}

// ---- 标注文档序列化 ----

namespace {

QJsonArray jsonPt(const QPointF& p) {
    return QJsonArray{p.x(), p.y()};
}

QPointF jsonToPt(const QJsonArray& a) {
    return a.size() >= 2 ? QPointF{a.at(0).toDouble(), a.at(1).toDouble()} : QPointF{};
}

QJsonArray jsonRect(const QRectF& r) {
    return QJsonArray{r.x(), r.y(), r.width(), r.height()};
}

QRectF jsonToRect(const QJsonArray& a) {
    return a.size() >= 4 ? QRectF{a.at(0).toDouble(), a.at(1).toDouble(), a.at(2).toDouble(),
                                  a.at(3).toDouble()}
                         : QRectF{};
}

// 两点定框的形状（矩形/椭圆/直线族/像素区域）几何字段一样，读写各一份即可
void putP1P2(QJsonObject& o, const QPointF& p1, const QPointF& p2, const QPointF& origin) {
    o.insert(QStringLiteral("p1"), jsonPt(p1 - origin));
    o.insert(QStringLiteral("p2"), jsonPt(p2 - origin));
}

void getP1P2(const QJsonObject& o, QPointF& p1, QPointF& p2) {
    p1 = jsonToPt(o.value(QStringLiteral("p1")).toArray());
    p2 = jsonToPt(o.value(QStringLiteral("p2")).toArray());
}

QString patchToBase64(const QImage& patched, const QRectF& src) {
    const QRect r = src.toAlignedRect().intersected(patched.rect());
    if (r.isEmpty())
        return {};
    QBuffer buf;
    buf.open(QIODevice::WriteOnly);
    if (!patched.copy(r).save(&buf, "PNG", 0))
        return {};
    return QString::fromLatin1(buf.data().toBase64());
}

}  // namespace

QJsonObject shapeToJson(const ShapePtr& s, const QPointF& origin) {
    if (!s)
        return {};
    // 判序不能改：派生类必须排在基类前面 —— DashLine 的 kind() 报的是 Line，
    // 马赛克/模糊/智能擦除全都继承 Rect，先判基类就把它们全读成矩形了。
    if (const auto* st = as<Stroke>(s)) {
        QJsonObject o{{QStringLiteral("type"), st->highlight ? QStringLiteral("highlight")
                                                             : QStringLiteral("stroke")},
                      {QStringLiteral("width"), st->width}};
        QJsonArray pts;
        for (const QPointF& p : st->points)
            pts.append(jsonPt(p - origin));
        o.insert(QStringLiteral("points"), pts);
        o.insert(QStringLiteral("color"), st->color.name(QColor::HexArgb));
        return o;
    }
    if (const auto* cv = as<Curve>(s)) {
        QJsonObject o{{QStringLiteral("type"), QStringLiteral("curve")},
                      {QStringLiteral("width"), cv->width},
                      {QStringLiteral("color"), cv->color.name(QColor::HexArgb)}};
        QJsonArray pts;
        for (const QPointF& p : {cv->p0, cv->p1, cv->p2, cv->p3})
            pts.append(jsonPt(p - origin));
        o.insert(QStringLiteral("points"), pts);
        return o;
    }
    if (const auto* se = as<SmartErase>(s)) {
        QJsonObject o{{QStringLiteral("type"), QStringLiteral("smart_erase")},
                      {QStringLiteral("color"), se->color.name(QColor::HexArgb)},
                      {QStringLiteral("width"), se->width}};
        putP1P2(o, se->p1, se->p2, origin);
        // patch 是「那块像素的快照」，与原点无关；只有 patchRect 跟着框一起平移，
        // 载入后「当前框 ↔ 采样源」的相对位置才不会变。
        o.insert(QStringLiteral("patchRect"), jsonRect(se->patchRect.translated(-origin)));
        if (!se->patched.isNull())
            o.insert(QStringLiteral("patch"), patchToBase64(se->patched, se->patchRect));
        return o;
    }
    const char* tag = nullptr;
    if (as<Spotlight>(s))          // 也继承 Rect，必须在 Rect 之前判
        tag = "spotlight";
    else if (as<Mosaic>(s))
        tag = "mosaic";
    else if (as<Blur>(s))
        tag = "blur";
    else if (as<Ellipse>(s))
        tag = "ellipse";
    else if (as<Rect>(s))
        tag = "rect";
    else if (as<DashLine>(s))
        tag = "dash_line";
    else if (as<DoubleArrow>(s))
        tag = "double_arrow";
    else if (as<Arrow>(s))
        tag = "arrow";
    else if (as<Line>(s))
        tag = "line";
    if (tag) {
        QJsonObject o{{QStringLiteral("type"), QString::fromLatin1(tag)},
                      {QStringLiteral("color"), s->color.name(QColor::HexArgb)},
                      {QStringLiteral("width"), s->width}};
        // 两点定框的两族：像素区域/矩形族走 Rect，直线族走 Line，字段名相同
        if (const auto* r = as<Rect>(s))
            putP1P2(o, r->p1, r->p2, origin);
        else if (const auto* ln = as<Line>(s))
            putP1P2(o, ln->p1, ln->p2, origin);
        return o;
    }
    if (const auto* ca = as<Callout>(s)) {
        QJsonObject o{{QStringLiteral("type"), QStringLiteral("callout")},
                      {QStringLiteral("color"), ca->color.name(QColor::HexArgb)},
                      {QStringLiteral("width"), ca->width},
                      {QStringLiteral("pos"), jsonPt(ca->pos - origin)},
                      {QStringLiteral("tail"), jsonPt(ca->tail - origin)},
                      {QStringLiteral("font_size"), ca->font_size},
                      {QStringLiteral("lines"), QJsonArray::fromStringList(ca->lines)}};
        return o;
    }
    if (const auto* tx = as<TextShape>(s)) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                          {QStringLiteral("color"), tx->color.name(QColor::HexArgb)},
                          {QStringLiteral("width"), tx->width},
                          {QStringLiteral("pos"), jsonPt(tx->pos - origin)},
                          {QStringLiteral("font_size"), tx->font_size},
                          {QStringLiteral("lines"), QJsonArray::fromStringList(tx->lines)}};
    }
    if (const auto* sb = as<StepBadge>(s)) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("step")},
                          {QStringLiteral("color"), sb->color.name(QColor::HexArgb)},
                          {QStringLiteral("width"), sb->width},
                          {QStringLiteral("pos"), jsonPt(sb->pos - origin)},
                          {QStringLiteral("font_size"), sb->font_size},
                          {QStringLiteral("number"), sb->number}};
    }
    return {};
}

ShapePtr shapeFromJson(const QJsonObject& o, const QSize& baseSize) {
    const QString type = o.value(QStringLiteral("type")).toString();
    if (type.isEmpty())
        return nullptr;
    const QColor color = o.value(QStringLiteral("color")).toString();
    const double width = o.value(QStringLiteral("width")).toDouble(1.0);

    if (type == QLatin1String("stroke") || type == QLatin1String("highlight")) {
        auto st = std::make_shared<Stroke>(color, width, type == QLatin1String("highlight"));
        for (const QJsonValue& v : o.value(QStringLiteral("points")).toArray())
            st->addPoint(jsonToPt(v.toArray()));
        return st->points.isEmpty() ? nullptr : st;
    }
    if (type == QLatin1String("curve")) {
        const QJsonArray pts = o.value(QStringLiteral("points")).toArray();
        if (pts.size() < 4)
            return nullptr;
        auto cv = std::make_shared<Curve>(color, width);
        cv->p0 = jsonToPt(pts.at(0).toArray());
        cv->p1 = jsonToPt(pts.at(1).toArray());
        cv->p2 = jsonToPt(pts.at(2).toArray());
        cv->p3 = jsonToPt(pts.at(3).toArray());
        return cv;
    }
    if (type == QLatin1String("smart_erase")) {
        auto se = std::make_shared<SmartErase>();
        se->color = color;
        se->width = width;
        getP1P2(o, se->p1, se->p2);
        se->patchRect = jsonToRect(o.value(QStringLiteral("patchRect")).toArray());
        const QString b64 = o.value(QStringLiteral("patch")).toString();
        if (!b64.isEmpty() && !baseSize.isEmpty()) {
            QImage crop;
            crop.loadFromData(QByteArray::fromBase64(b64.toLatin1()), "PNG");
            if (!crop.isNull()) {
                // draw 用 drawImage(当前框, patched, patchRect)：patched 必须与底图
                // 同尺寸，crop 贴回它当初被裁下的那个整数位置
                se->patched = QImage(baseSize, QImage::Format_ARGB32);
                se->patched.fill(Qt::transparent);
                QPainter p(&se->patched);
                p.drawImage(se->patchRect.toAlignedRect().topLeft(), crop);
                p.end();
            }
        }
        return se;
    }
    auto makeRectish = [&](auto shape, const QJsonObject& src) {
        shape->color = color;
        shape->width = width;
        getP1P2(src, shape->p1, shape->p2);
        return shape;
    };
    if (type == QLatin1String("spotlight"))
        return makeRectish(std::make_shared<Spotlight>(), o);
    if (type == QLatin1String("mosaic"))
        return makeRectish(std::make_shared<Mosaic>(), o);
    if (type == QLatin1String("blur"))
        return makeRectish(std::make_shared<Blur>(), o);
    if (type == QLatin1String("ellipse"))
        return makeRectish(std::make_shared<Ellipse>(color, width), o);
    if (type == QLatin1String("rect"))
        return makeRectish(std::make_shared<Rect>(color, width), o);
    if (type == QLatin1String("dash_line"))
        return makeRectish(std::make_shared<DashLine>(color, width), o);
    if (type == QLatin1String("double_arrow"))
        return makeRectish(std::make_shared<DoubleArrow>(color, width), o);
    if (type == QLatin1String("arrow"))
        return makeRectish(std::make_shared<Arrow>(color, width), o);
    if (type == QLatin1String("line"))
        return makeRectish(std::make_shared<Line>(color, width), o);

    QStringList lines;
    for (const QJsonValue& v : o.value(QStringLiteral("lines")).toArray())
        lines.append(v.toString());
    const QPointF pos = jsonToPt(o.value(QStringLiteral("pos")).toArray());
    const double fontSize = o.value(QStringLiteral("font_size")).toDouble(16.0);
    if (type == QLatin1String("text")) {
        auto tx = std::make_shared<TextShape>(color, fontSize);
        tx->pos = pos;
        tx->lines = lines;
        return tx;
    }
    if (type == QLatin1String("callout")) {
        auto ca = std::make_shared<Callout>(color, fontSize);
        ca->pos = pos;
        ca->tail = jsonToPt(o.value(QStringLiteral("tail")).toArray());
        ca->lines = lines;
        return ca;
    }
    if (type == QLatin1String("step")) {
        auto sb = std::make_shared<StepBadge>(color, fontSize,
                                              o.value(QStringLiteral("number")).toInt(1));
        sb->pos = pos;
        return sb;
    }
    return nullptr;
}

}  // namespace zpin
