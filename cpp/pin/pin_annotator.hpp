// 贴图标注交互器 —— 在贴图窗表面直接驱动 AnnotateEngine。
// 与选区标注共用 shapes/engine/toolbar/text_edit，交互语义对齐
// AnnotationController，但宿主是贴图窗：坐标换算、与拖窗/缩放的冲突、
// 工具条定位都按贴图语义实现。
// 坐标系约定：标注坐标 = 贴图底图物理像素；窗口局部 <-> 底图的换算由贴图窗
// 的 localToImage / imageToGlobal 提供。
#pragma once

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QPointF>
#include <QRect>
#include <QString>
#include <QVector>

#include "shapes.hpp"

class QKeyEvent;

namespace zpin {

class AnnotateEngine;
class CaptureToolbar;
class PinWindow;
class TextEditor;

class PinAnnotator : public QObject {
    Q_OBJECT

public:
    explicit PinAnnotator(PinWindow* pin);
    ~PinAnnotator() override;  // 定义在 .cpp：m_engine 是不完整类型

    bool isActive() const { return m_active; }
    AnnotateEngine* engine() const { return m_engine.get(); }
    ShapePtr activeShape() const { return m_activeShape; }

    // 一键脱敏的落地点：把这些 OCR 文本框（显示图坐标）压成马赛克标注，整批记为一
    // 步撤销，返回真正打码的处数。
    int maskSensitiveLines(const QVector<QRect>& imageRects);

    // 生命周期：进入/退出标注模式（退出后已画形状仍留在贴图上）
    void start();
    void stop();

    // 工具条重新贴到贴图旁（贴图被拖动/缩放时由 PinWindow 调用）
    void positionToolbar();

    // 丢弃拖拽/擦除/活动形状等瞬时状态（不动已入栈的形状）。宿主窗口在
    // 「标注中右键弹菜单」时也调它：菜单会吞掉左键 release，拖拽态悬挂的话
    // 关菜单后笔迹会跟着光标继续长。
    void dropInteraction();

    // 鼠标（坐标用窗口局部逻辑坐标）；press 返回是否消费（False = 回退拖窗）
    bool press(const QPointF& pos);
    void move(const QPointF& pos);
    void release();

    // 键盘：Del 与撤销/重做；返回事件是否被消费
    bool handleKey(QKeyEvent* ev);

    // 二次编辑
    ShapePtr shapeAt(const QPointF& ip) const;
    void applyStyle(const QColor* color = nullptr, const double* width = nullptr);
    bool deleteSelected();

    // 烘焙与合成
    QImage bake(const QImage& baseImg);
    void clear();
    bool hasContent() const;

    // 绘制（贴图窗 paintEvent 调用，painter 为窗口逻辑坐标）
    void draw(QPainter& p);

private:
    bool toolIsEraser() const { return m_tool == QLatin1String("eraser"); }
    int endpointAt(const Shape& s, const QPointF& ip) const;
    bool tryGrab(const QPointF& ip);
    void selectShape(const ShapePtr& s);
    void onAction(const QString& action);
    void onTool(const QString& name);
    void onColor(const QColor& color);
    void onWidth(double width);
    void syncToolbarState();
    void closeTextEditor(bool commit);
    void openTextEditor(const QPointF& globalPos, const std::shared_ptr<TextShape>& shape);
    void kickSmartErase(const std::shared_ptr<class SmartErase>& shape);
    void dropSmartErase(const std::weak_ptr<class Shape>& target);

    PinWindow* m_pin = nullptr;
    // shared_ptr：与选区标注一致 —— 后台预热线程要靠它的 weak_ptr 躲开析构竞态
    std::shared_ptr<AnnotateEngine> m_engine;
    QPointer<CaptureToolbar> m_toolbar;
    bool m_active = false;
    QString m_tool;
    QColor m_color;
    QColor m_accent;   // 主题色（选中框虚线用），start() 时缓存：拖动中每帧读 QSettings 太贵
    double m_width = 4.0;
    ShapePtr m_activeShape;
    QPointer<TextEditor> m_textEditor;
    ShapePtr m_editShape;        // 选中/拖动中的已画图形
    int m_editPt = -1;           // >=0 = 正在拖端点；-1 = 整体
    QList<QPointF> m_editBefore; // 按下时端点快照，抬手记账
    QPointF m_editLast;
    double m_editDx = 0.0, m_editDy = 0.0;
    bool m_dragging = false;
    bool m_erasing = false;
    QPointF m_cursorIp{-1.0, -1.0};  // 悬停预览用（图像坐标）
};

}  // namespace zpin
