// 标注控制器 —— 与选区控制器解耦的画图子系统。
// 选区进入「已选定区域」阶段后，所有画图职责（引擎/工具条/文本编辑、当前绘制
// 状态、撤销重做、文字批注、二次编辑、标注层绘制）都委托到这里。坐标锚定在
// 截图底图物理像素（移动选区时标注始终跟随截图内容）；经宿主只读访问选区/
// 底图/光标等共享状态，避免循环依赖。
#pragma once

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QPointF>
#include <QString>

#include "shapes.hpp"

class QKeyEvent;

namespace zpin {

class AnnotateEngine;
class CaptureToolbar;
class TextEditor;

// 宿主（SelectionController）向标注控制器暴露的只读/受控接口。
class AnnotHost {
public:
    virtual ~AnnotHost() = default;
    virtual QPointF imgPt(const QPointF& globalPos) const = 0;  // 全局逻辑 -> 底图物理
    virtual double dpr() const = 0;
    virtual QPointF cursorPos() const = 0;
    virtual QString state() const = 0;
    virtual void setState(const QString& s) = 0;
    virtual void updateAll() = 0;
    virtual void refocusOverlay() = 0;
    virtual void cancel() = 0;
    virtual void confirm(const QString& action) = 0;
    // 工具条一次性动作（脱敏/长截图）；贴图标注没有这些动作，默认忽略。
    virtual void runCommand(const QString& cmd) { Q_UNUSED(cmd); }
    // 覆盖层会话抓着键盘时放掉，好让文字编辑器收到按键（贴图标注没有抓取）。
    virtual void releaseKeyboardGrab() {}
    virtual QImage baseImage() const = 0;
    virtual QRectF selRect() const = 0;   // 当前选区（全局逻辑）
    virtual QRect bounds() const = 0;     // 虚拟桌面逻辑包围盒
    virtual QPointF physTl() const = 0;   // 选区在底图（画布）中的原点
};

class AnnotationController : public QObject {
    Q_OBJECT

public:
    explicit AnnotationController(AnnotHost* host);
    ~AnnotationController() override;  // m_engine 等成员的定义对调用方不可见

    AnnotateEngine* engine() const { return m_engine.get(); }
    ShapePtr activeShape() const { return m_activeShape; }
    QString tool() const { return m_tool; }
    bool isGrabbing() const { return m_editDrag; }

    // 已画图形的抓取与二次编辑
    ShapePtr shapeAt(const QPointF& ip) const;
    bool tryGrabShape(const QPointF& pos);
    bool deleteSelected();
    void applyStyle(const QColor* color = nullptr, const double* width = nullptr);
    void selectShape(const ShapePtr& s);

    // 生命周期
    void reset();
    void ensure();
    void positionToolbar();

    // 绘制交互
    double fontPx() const;
    void startDrawing(const QPointF& pos);
    void updateDrawing(const QPointF& pos);
    void finishDrawing(const QPointF& pos);
    void eraseAt(const QPointF& pos);
    void finishErasing();
    void translateFollowing(double dx, double dy);

    // 键盘：撤销/重做/删除；返回事件是否被消费
    bool handleKey(QKeyEvent* ev);

    // 标注层绘制（选区视口局部坐标 r）
    void draw(QPainter& p, const QRectF& r);

private:
    void onToolbarAction(const QString& action);
    void onToolSelected(const QString& name);
    void onColorSelected(const QColor& color);
    void onWidthSelected(double width);
    void closeTextEditor(bool commit);
    void openTextEditor(const QPointF& globalPos, const std::shared_ptr<TextShape>& shape);
    int endpointAt(const Shape& s, const QPointF& ip) const;
    void syncToolbarState();
    void kickSmartErase(const std::shared_ptr<class SmartErase>& shape);
    void dropSmartErase(const std::weak_ptr<class Shape>& target);

    AnnotHost* m_host = nullptr;
    // shared_ptr：后台预热线程握它的 weak_ptr，engine 被析构后能自行空转退出
    std::shared_ptr<AnnotateEngine> m_engine;
    QPointer<CaptureToolbar> m_toolbar;
    QString m_tool;
    QColor m_color;
    double m_width = 4.0;
    ShapePtr m_activeShape;
    QPointer<TextEditor> m_textEditor;
    ShapePtr m_editShape;        // 选中/拖动中的已画图形（二次编辑）
    int m_editPt = -1;           // >=0 = 正在拖某个端点；-1 = 整体拖动
    QList<QPointF> m_editBefore; // 按下时的端点快照，抬手时记账
    QPointF m_editLast;          // 上一帧光标（图像坐标）
    double m_editDx = 0.0, m_editDy = 0.0;
    bool m_editDrag = false;     // 正在拖（松手后仅保持选中）
};

}  // namespace zpin
