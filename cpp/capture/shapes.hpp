// 标注对象 —— 全部坐标为「本引擎底图」的物理像素坐标（截图会话=整屏抓图，贴图=贴图内容）。
#pragma once

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QList>
#include <QPainter>
#include <QPainterPath>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QString>
#include <memory>
#include <atomic>

namespace zpin {

enum class ShapeKind {
    Stroke,     // 画笔/荧光笔
    Mosaic,
    Blur,
    SmartErase,
    Line,
    Arrow,
    DoubleArrow,
    Rect,
    Ellipse,
    Text,
    Callout,
    StepBadge,
    Spotlight,
    Curve,      // 三次贝塞尔曲线
};

class Shape {
public:
    QColor color;
    double width = 1.0;

    Shape(const QColor& c, double w) : color(c), width(w) {}
    virtual ~Shape() = default;

    virtual ShapeKind kind() const = 0;
    virtual void draw(QPainter& p) = 0;
    virtual bool hit(const QPointF& pt, double tol = 6.0) const;
    virtual void translate(double dx, double dy) = 0;

    // 可单独拖动的控制点（改形状用）；空表 = 不支持逐点编辑。
    virtual QList<QPointF> endpoints() const { return {}; }
    virtual void setEndpoints(const QList<QPointF>& pts) = 0;

    // 拖动中的跟随行为：笔画加采样点 / 点位形状移点 / 线段移终点。
    virtual void dragTo(const QPointF& /*ip*/) {}

    // 抬手时的面积校验：笔画/序号恒通过，线/矩形要求 ≥3px。
    virtual bool validOnFinish() const { return true; }

    // 可视包围盒（选中框用）；未知/无内容返回空矩形。
    virtual QRectF boundingRect() const { return QRectF(); }
};

// 点到线段的最短距离。
double segDist(const QPointF& pt, const QPointF& a, const QPointF& b);

using ShapePtr = std::shared_ptr<Shape>;

// 画笔 / 荧光笔（自由笔画，采样点连段绘制）。荧光笔按 3 倍线宽、半透明绘制。
class Stroke : public Shape {
public:
    bool highlight = false;
    QList<QPointF> points;

    Stroke(const QColor& color, double width, bool highlight = false);

    ShapeKind kind() const override { return ShapeKind::Stroke; }
    void addPoint(const QPointF& pt) { points.append(pt); }
    double penWidth() const { return highlight ? width * 3 : width; }
    void draw(QPainter& p) override;
    bool hit(const QPointF& pt, double tol = 6.0) const override;
    void translate(double dx, double dy) override;
    void setEndpoints(const QList<QPointF>&) override {}
    void dragTo(const QPointF& ip) override { addPoint(ip); }
    QRectF boundingRect() const override;
};

// 矩形框（只描边不填充）。几何定义也是两个对角点。
//
// 放在 Stroke 之后、PixelRegion 之前：像素级标注统一继承 Rect 复用这套几何，
// 免得同一份「两点定矩形」在文件里存在三份。
class Rect : public Shape {
public:
    QPointF p1, p2;

    Rect(const QColor& color, double width) : Shape(color, width) {}
    QRectF rect() const { return QRectF(p1, p2).normalized(); }
    ShapeKind kind() const override { return ShapeKind::Rect; }
    void draw(QPainter& p) override;
    bool hit(const QPointF& pt, double tol = 6.0) const override;
    void translate(double dx, double dy) override;
    QList<QPointF> endpoints() const override { return {p1, p2}; }
    void setEndpoints(const QList<QPointF>& pts) override;
    void dragTo(const QPointF& ip) override { p2 = ip; }
    bool validOnFinish() const override;
    QRectF boundingRect() const override;
};

// 椭圆框（几何定义与矩形一致）。
class Ellipse : public Rect {
public:
    Ellipse(const QColor& color, double width) : Rect(color, width) {}
    ShapeKind kind() const override { return ShapeKind::Ellipse; }
    void draw(QPainter& p) override;
    // 覆写命中：按椭圆方程判边框环带。继承 Rect::hit 会拿外接矩形判，
    // 四角「点空气」也把椭圆选中。
    bool hit(const QPointF& pt, double tol = 6.0) const override;
};

// 马赛克 / 高斯模糊 / 智能擦除都是「框选一块区域再处理」的像素级标注，几何与
// 命中规则完全一致，统一收敛到 PixelRegion；各自的声明见下。
// （原先马赛克/模糊是 Stroke 自由笔画、脱敏功能还被迫用两点横线伪造矩形，见
//  PixelRegion 的注释。）

// 智能擦除 / 马赛克 / 高斯模糊的公共基类：框选一块**填充区域**。
//
// 为什么不用 Stroke 自由笔画：这三者的诉求都是「精确遮住某个东西」，不是画画。
// 笔画有三个具体问题——
//   1. 精度不够，遮不住一个手机号/身份证号；「一键脱敏」因此被迫用两点横线 +
//      线宽=行高 在内部伪造一个矩形（mosaicForTextLine），内部模型与用户预期
//      从那时起就分裂了；
//   2. 「粗细」档位本是给画笔/箭头定线宽的，却顺带决定了马赛克块的厚度
//      （原来是 make_shared<Mosaic>(qMax(8.0, w * 2.0))），用户为了遮一行细字
//      得去猜该选哪一档，选粗细时又会莫名把马赛克一起改粗；
//   3. 同一个橡皮组里，智能擦除已是两点拖框，马赛克/模糊却是画笔，手势不统一。
// 改成框选后 width/color 都不再有意义（马赛克格子大小是 pixelateImage 的全局
// 常量 8px，与形状无关），只剩一个纯粹的「选哪一块」。
//
// 命中覆写成整块可抓：Rect::hit 只判边框环带，而这里框内是被处理的**面**，
// 用户点在框内任意位置都该能抓住它（与智能擦除一致）。
class PixelRegion : public Rect {
public:
    PixelRegion() : Rect(QColor("#000000"), 1) {}
    bool hit(const QPointF& pt, double tol = 6.0) const override;
    // 马赛克/模糊只贴效果图，自身没有轮廓（贴上去的像素就是最终结果，多一道
    // 描边既不像成品也会暴露「这里被打过码」）。代价是框选时看不到选到哪里，
    // 所以由控制器在绘制层补一个红虚线框——智能擦除自己有半透明红框占位，
    // 马赛克/模糊当时漏了，用户实测反馈「框选时看不清边界」。
    // 只在创建中/被选中调整时画，抬手提交后不画。
    bool needsSelectionFrame() const {
        return kind() == ShapeKind::Mosaic || kind() == ShapeKind::Blur;
    }
};

// 马赛克：框选区域内的像素化底图（引擎注入 pixelated，不持有）。
class Mosaic : public PixelRegion {
public:
    const QImage* pixelated = nullptr;

    Mosaic();
    ShapeKind kind() const override { return ShapeKind::Mosaic; }
    void draw(QPainter& p) override;
};

// 高斯模糊：框选区域内的模糊化底图（引擎注入 blurred，不持有）。
class Blur : public PixelRegion {
public:
    const QImage* blurred = nullptr;

    Blur();
    ShapeKind kind() const override { return ShapeKind::Blur; }
    void draw(QPainter& p) override;
};

// 智能擦除：框选一块区域，用内容感知填充重建背景（去水印/杂物）。几何与
// 马赛克/模糊同款（两点拖框）；控制器抬手后按框异步算好整幅结果注进 patched
//（与底图同尺寸），空 = 计算中，画半透明红框占位。
class SmartErase : public PixelRegion {
public:
    QImage patched;
    // patched 里对应「计算时刻本框位置」的源区：二次编辑把框挪走/缩放后，
    // 贴图仍按它采样，否则会用当前框位去快照里取错位的像素
    QRectF patchRect;
    // 本形状的取消标志，与形状同生共死。PatchMatch 是几百毫秒到两秒的重计算，
    // 形状被撤销/删除/换会话时必须能中断，否则线程池槽位白占到算完。挂在形状
    // 上（而不是调用方的局部变量）才能做到「形状没了 = 取消」：局部变量随工作
    // lambda 一起销毁，谁也置不了位，那 20 多处 checkCancelled 全是死代码。
    // 形状通常活在主线程、析构也在主线程，工作线程只读这个原子，安全。
    std::shared_ptr<std::atomic_bool> cancel =
        std::make_shared<std::atomic_bool>(false);
    ~SmartErase() override;

    ShapeKind kind() const override { return ShapeKind::SmartErase; }
    void draw(QPainter& p) override;
    bool validOnFinish() const override;   // 与矩形同款 ≥3px 校验
    QRectF boundingRect() const override { return rect(); }
};

// 聚光灯：框选一块**保持原样**的区域，框外整幅压暗 —— 讲截图/写教程时把视线钉住。
// 与马赛克/模糊同款两点拖框（继承 Rect），但它的绘制对象是「框外」，所以画的时候
// 要知道画布边界：从 painter 当前的视口反变换回底图坐标（见 shapes.cpp paintBounds），
// 于是选区覆盖层、贴图窗、成品合三条绘制路径都不用额外传尺寸。
// color/width 同马赛克一样不再参与绘制（压暗量是常量），只剩「框在哪」。
class Spotlight : public Rect {
public:
    Spotlight() : Rect(QColor("#000000"), 1) {}
    ShapeKind kind() const override { return ShapeKind::Spotlight; }
    void draw(QPainter& p) override;
    // 只描亮区白边不画暗区：引擎合并多个聚光灯的暗区时用（见 AnnotateEngine::draw）
    void drawBorder(QPainter& p) const;
    // 命中亮区（含白边）：亮的那块才是用户眼里的「聚光灯」；框外暗区是背景
    // 效果，不参与命中。框内后画的图形在栈顶，橡皮擦/抓取先命中它们。
    bool hit(const QPointF& pt, double tol = 6.0) const override;
    QRectF boundingRect() const override { return rect(); }
};

// 直线段。
class Line : public Shape {
public:
    QPointF p1, p2;

    Line(const QColor& color, double width) : Shape(color, width) {}
    ShapeKind kind() const override { return ShapeKind::Line; }
    void draw(QPainter& p) override;
    bool hit(const QPointF& pt, double tol = 6.0) const override;
    void translate(double dx, double dy) override;
    QList<QPointF> endpoints() const override { return {p1, p2}; }
    void setEndpoints(const QList<QPointF>& pts) override;
    void dragTo(const QPointF& ip) override { p2 = ip; }
    bool validOnFinish() const override;
    QRectF boundingRect() const override;
};

// 虚线段：端点/命中/包围盒全沿用直线，只把画笔换成虚线。
class DashLine : public Line {
public:
    DashLine(const QColor& color, double width) : Line(color, width) {}
    ShapeKind kind() const override { return ShapeKind::Line; }
    void draw(QPainter& p) override;
};

// 带实心箭头头部的直线段。
class Arrow : public Line {
public:
    Arrow(const QColor& color, double width) : Line(color, width) {}
    ShapeKind kind() const override { return ShapeKind::Arrow; }
    void draw(QPainter& p) override;
};

// 双头箭头：两端各带一个实心箭头头部。
class DoubleArrow : public Line {
public:
    DoubleArrow(const QColor& color, double width) : Line(color, width) {}
    ShapeKind kind() const override { return ShapeKind::DoubleArrow; }
    void draw(QPainter& p) override;
};

// 曲线：三次贝塞尔。创建手势与直线同款（拖拽抬手），控制点默认落在全线
// 1/3、2/3 处——此刻它就是直线；二次编辑暴露 4 个端点（两端 + 两控制点），
// 拖控制点把线「弯」出弧度。直线/曲线收在同一个子工具组里。
class Curve : public Shape {
public:
    QPointF p0, p1, p2, p3;   // 端点、控制点、控制点、端点

    Curve(const QColor& color, double width) : Shape(color, width) {}
    ShapeKind kind() const override { return ShapeKind::Curve; }
    QPainterPath strokePath() const;
    void draw(QPainter& p) override;
    bool hit(const QPointF& pt, double tol = 6.0) const override;
    void translate(double dx, double dy) override;
    QList<QPointF> endpoints() const override { return {p0, p1, p2, p3}; }
    void setEndpoints(const QList<QPointF>& pts) override {
        if (pts.size() >= 4) {
            p0 = pts[0];
            p1 = pts[1];
            p2 = pts[2];
            p3 = pts[3];
        }
    }
    // 创建中控制点跟着端点走（保持三等分）；二次编辑走 setEndpoints，不动它
    void dragTo(const QPointF& ip) override {
        p3 = ip;
        p1 = p0 + (p3 - p0) / 3.0;
        p2 = p0 + (p3 - p0) * 2.0 / 3.0;
    }
    bool validOnFinish() const override;
    QRectF boundingRect() const override;
};

// 多行文字块。
class TextShape : public Shape {
public:
    QPointF pos;
    QStringList lines;
    double font_size = 16.0;

    TextShape(const QColor& color, double fontSize) : Shape(color, 1), font_size(fontSize) {}
    ShapeKind kind() const override { return ShapeKind::Text; }
    QFont font() const;
    // 文字左上角（物理像素）：编辑框要按它对位，提交前后文字不跳位。
    // 气泡的文字在气泡体内缩进了内边距，见 Callout::textTopLeft。
    virtual QPointF textTopLeft() const { return pos; }
    QFontMetrics metrics() const;
    std::pair<double, double> size() const;
    void draw(QPainter& p) override;
    bool hit(const QPointF& pt, double tol = 6.0) const override;
    void translate(double dx, double dy) override;
    void setEndpoints(const QList<QPointF>&) override {}
    QRectF boundingRect() const override;
};

// 序号标记：实心圆 + 白色粗体数字，序号自动递增。
class StepBadge : public Shape {
public:
    QPointF pos;
    double font_size = 16.0;
    int number = 1;

    StepBadge(const QColor& color, double fontSize, int n)
        : Shape(color, 1), font_size(fontSize), number(n) {}
    ShapeKind kind() const override { return ShapeKind::StepBadge; }
    QFont font() const;
    double radius() const;
    void draw(QPainter& p) override;
    bool hit(const QPointF& pt, double tol = 6.0) const override;
    void translate(double dx, double dy) override;
    void setEndpoints(const QList<QPointF>&) override {}
    void dragTo(const QPointF& ip) override { pos = ip; }
    QRectF boundingRect() const override;
};

// 气泡：气泡框 + 指向尾巴 + 文字。
class Callout : public TextShape {
public:
    QPointF tail;  // 指向点（图上）

    Callout(const QColor& color, double fontSize) : TextShape(color, fontSize) {}
    ShapeKind kind() const override { return ShapeKind::Callout; }
    // 文字在气泡体内缩进了内边距（与 bubbleRect 的 kCalloutPadX/Y 同源）
    QPointF textTopLeft() const override;
    void draw(QPainter& p) override;
    bool hit(const QPointF& pt, double tol = 6.0) const override;
    void translate(double dx, double dy) override;
    QList<QPointF> endpoints() const override { return {tail, pos}; }
    void setEndpoints(const QList<QPointF>& pts) override;
    void dragTo(const QPointF& ip) override { pos = ip; }
    QRectF boundingRect() const override;

private:
    // 气泡框（含内边距）：draw/hit/boundingRect 共用一份尺寸计算，
    // 不再三处各写一套「行宽 + 内边距」。
    QRectF bubbleRect() const;
};

// 便捷向下转型（控制器与命中逻辑用）。
template <typename T>
T* as(const ShapePtr& s) {
    return dynamic_cast<T*>(s.get());
}

// ---- 标注文档序列化 ----
// 坐标一律是底图物理像素，所以文档只在「同一张底图」上有意义，尺寸由调用方核对。
// 马赛克/模糊只存框（特效底图由引擎重新注入）；智能擦除额外存一份 patchRect 处
// 的 PNG（base64）—— 重算一次要几百毫秒到两秒，而且结果未必逐像素相同，所以
// 还原时按 baseSize 铺回同尺寸画布，不重跑 PatchMatch。
// 认不出的形状导出为空对象，载入返回 nullptr（调用方整份作废，别画半套）。
//
// origin：导出前把所有几何减去这个点，让文档改以「底图里 origin 那一角」为原点。
// 截图会话的标注活在整屏底图坐标上，而留档存的是选区那一段，导出时必须换原点。
// 智能擦除的 patchRect 跟着一起平移（patch 本体是那块像素的快照，不动），
// 于是「当前框 ↔ 采样源」的相对关系原样保留，换图后仍取到同一批像素。
QJsonObject shapeToJson(const ShapePtr& shape, const QPointF& origin = QPointF());
ShapePtr shapeFromJson(const QJsonObject& obj, const QSize& baseSize);

// 笔画路径描边出的实心区域（圆帽圆角）：马赛克/模糊/智能擦除的可见范围，
// 也是智能擦除送给修复算法的 mask。drawClippedEffect 与控制器共用这一份。
QPainterPath strokeRegion(const Stroke& s);

// 画布在**当前 painter 坐标系**下的范围（把视口按世界变换反推回去）。
// 聚光灯的暗区/引擎的合并暗区都要知道画布边界才能画「框外」。
QRectF paintBounds(const QPainter& p);

// 一键脱敏用：把一个 OCR 文本框压成横贯中线的马赛克粗笔（笔宽 = 行高）。textRect 要
// 先换算到目标图像坐标，imageBounds 是那张图的尺寸；这里负责外扩 3px 盖住边缘并裁进
// 图内，小于 6×4 返回 null。选区与贴图两条脱敏路径共用这一份，免得外扩量各写各的。
// 特效底图由 AnnotateEngine::add 注入，这里不用管。
ShapePtr mosaicForTextLine(const QRect& textRect, const QSize& imageBounds);

}  // namespace zpin
