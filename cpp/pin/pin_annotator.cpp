#include "pin_annotator.hpp"

#include <QBrush>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QList>
#include <QMetaObject>
#include <QPainter>
#include <QPen>
#include <QPointer>
#include <QScreen>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>

#include "config.hpp"
#include "engine.hpp"
#include "logging.hpp"
#include "pin_window.hpp"
#include "smart_erase.hpp"
#include "text_edit.hpp"
#include "toolbar.hpp"

namespace zpin {
namespace {

// 贴图工具集直接由截图那份 kTools 派生，只换「移动」的说明：贴图上没有选区，那颗
// 按钮的含义是「回到选择/移动态，拖动已画的图形」。
// 双向箭头 = 回到「选择/移动」态。贴图上这个按钮不是冗余：press() 里「点中已画图形
// 就抓起来拖」只在 m_tool 为空时生效，而工具集里除了它没有别的入口能清空工具——早先
// 选了画笔就只能一直画下去，想回去移动已画的图形没有按钮可点（截图侧靠选区那套
// overlay 逻辑兜住了，贴图没有 overlay）。
const QList<CaptureToolbar::ToolDef>& pinTools() {
    static const QList<CaptureToolbar::ToolDef> kPinTools = [] {
        QList<CaptureToolbar::ToolDef> tools = kTools;
        for (CaptureToolbar::ToolDef& def : tools)
            if (def.name == QLatin1String("move"))
                def.label = QStringLiteral("选择 / 编辑（拖动已画的图形）");
        return tools;
    }();
    return kPinTools;
}

}  // namespace

PinAnnotator::PinAnnotator(PinWindow* pin)
    : QObject(pin),
      m_pin(pin),
      m_engine(std::make_shared<AnnotateEngine>(pin->displayImage())),
      m_color(config::getStr(QStringLiteral("Interface/theme_color"))) {}

PinAnnotator::~PinAnnotator() = default;

// ---- 状态 ----

ShapePtr PinAnnotator::shapeAt(const QPointF& ip) const {
    // 命中检查：反向遍历形状栈，返回最上层被点中的图形。
    const double tol = 8.0 * m_pin->refDpr();
    const auto& shapes = m_engine->shapes;
    for (auto it = shapes.rbegin(); it != shapes.rend(); ++it) {
        if (*it && (*it)->hit(ip, tol))
            return *it;
    }
    return nullptr;
}

void PinAnnotator::selectShape(const ShapePtr& s) {
    // 设置当前二次编辑目标；改色/改粗细会作用在它身上。
    m_editShape = s;
    m_editPt = -1;
    m_editBefore.clear();
    m_editDx = m_editDy = 0.0;
}

// ---- 生命周期 ----

void PinAnnotator::start() {
    if (m_active)
        return;
    m_active = true;
    m_accent = QColor(config::getStr(QStringLiteral("Interface/theme_color")));
    if (m_toolbar.isNull()) {
        auto* tb = new CaptureToolbar(m_color, m_width, pinTools(), toolGroups(),
                                      {"finish"}, {});
        connect(tb, &CaptureToolbar::action, this, &PinAnnotator::onAction);
        connect(tb, &CaptureToolbar::toolSelected, this, &PinAnnotator::onTool);
        connect(tb, &CaptureToolbar::colorSelected, this, &PinAnnotator::onColor);
        connect(tb, &CaptureToolbar::widthSelected, this, &PinAnnotator::onWidth);
        m_toolbar = tb;
    }
    positionToolbar();
    syncToolbarState();   // 新建的工具条 undo/redo 按钮初始可用态要跟引擎对齐（空栈=都灰）
}

void PinAnnotator::stop() {
    if (!m_active)
        return;
    m_active = false;
    m_tool.clear();   // 工具选择不跨会话：新会话界面全不选中，实际状态也得归位
    closeTextEditor(false);
    dropInteraction();
    if (!m_toolbar.isNull()) {
        CaptureToolbar* tb = m_toolbar.data();
        m_toolbar = nullptr;
        tb->close();
        tb->deleteLater();
    }
}

void PinAnnotator::dropInteraction() {
    // 丢弃拖拽/擦除/活动形状等瞬时状态（不动已入栈的形状）。
    m_dragging = false;
    m_erasing = false;
    m_activeShape = nullptr;
    selectShape(nullptr);
}

void PinAnnotator::positionToolbar() {
    // 工具条贴图正上方居中（放不下改下方），并夹在屏幕内。
    if (m_toolbar.isNull())
        return;
    QWidget* tb = m_toolbar.data();
    tb->adjustSize();
    QScreen* scr = QGuiApplication::screenAt(m_pin->frameGeometry().center());
    if (!scr)
        scr = QGuiApplication::primaryScreen();
    if (!scr) {
        tb->show();
        tb->raise();
        return;
    }
    const QRect g = scr->availableGeometry();
    double x = m_pin->x() + (m_pin->width() - tb->width()) / 2.0;
    double y = m_pin->y() - tb->height() - 8;
    if (y < g.top() + 4)
        y = m_pin->y() + m_pin->height() + 8;
    x = std::min(std::max(double(g.left() + 4), x), double(g.right() - tb->width() - 4));
    y = std::min(std::max(double(g.top() + 4), y), double(g.bottom() - tb->height() - 4));
    tb->move(qRound(x), qRound(y));
    tb->show();
    tb->raise();
}

// ---- 鼠标（坐标用窗口局部逻辑坐标） ----

bool PinAnnotator::press(const QPointF& pos) {
    const QPointF ip = m_pin->localToImage(pos);
    m_cursorIp = ip;
    const QString tool = m_tool;
    if (tool.isEmpty()) {
        // 没选工具：点中已画图形 = 抓取拖动/改形状；点空白 = 交还拖窗
        if (tryGrab(ip)) {
            m_dragging = true;
            m_pin->update();
            return true;
        }
        return false;
    }
    // 曲线画完保持选中（见 release）：带着曲线工具点它/点手柄是想弯它，不是
    // 再画一条——命中选中曲线或其端点就直接转入编辑
    if (tool == QLatin1String("curve")) {
        const auto cv = std::dynamic_pointer_cast<Curve>(m_editShape);
        if (cv && (endpointAt(*cv, ip) >= 0 || cv->hit(ip, 2.0 * m_pin->refDpr())) &&
            tryGrab(ip)) {
            m_dragging = true;
            m_pin->update();
            return true;
        }
    }
    if (tool != QLatin1String("text"))
        selectShape(nullptr);  // 画新东西前先取消二次编辑选中
    const double ref = m_pin->refDpr();
    const double w = m_width * ref;
    const double fontPx = std::max(13.0, 10.0 + m_width * 1.5) * ref;

    if (tool == QLatin1String("pen") || tool == QLatin1String("marker")) {
        auto s = std::make_shared<Stroke>(m_color, w, tool == QLatin1String("marker"));
        s->addPoint(ip);
        m_activeShape = s;
        m_dragging = true;
    } else if (tool == QLatin1String("mosaic") || tool == QLatin1String("blur")) {
        // 马赛克/模糊都是框选形状（继承 Rect）：几何与矩形同款，不再是自由笔画，
        // 「粗细」档位也不再影响它们（格子大小是 pixelateImage 的全局常量 8px）
        Rect* region = nullptr;
        ShapePtr s;
        if (tool == QLatin1String("mosaic")) {
            auto mo = std::make_shared<Mosaic>();
            // 底图现在就注入，否则拖拽过程中 draw() 直接 return，框选不可见
            mo->pixelated = &m_engine->pixelated();
            region = mo.get();
            s = mo;
        } else {
            auto bl = std::make_shared<Blur>();
            bl->blurred = &m_engine->blurred();
            region = bl.get();
            s = bl;
        }
        region->p1 = ip;
        region->p2 = ip;
        m_activeShape = s;
        m_dragging = true;
    } else if (tool == QLatin1String("smart_erase")) {
        // 框选一块区域，抬手后异步重建背景；计算落地前画红框占位
        auto s = std::make_shared<SmartErase>();
        s->p1 = ip;
        s->p2 = ip;
        m_activeShape = s;
        m_dragging = true;
    } else if (tool == QLatin1String("eraser")) {
        m_erasing = true;
        m_engine->beginEraseStroke();  // 一整段涂抹 = 一步撤销
        m_engine->eraseAt(ip, 8.0 * ref);
    } else if (tool == QLatin1String("text")) {
        if (tryGrab(ip)) {
            m_dragging = true;
            m_pin->update();
            return true;
        }
        auto shape = std::make_shared<TextShape>(m_color, fontPx);
        shape->pos = ip;
        openTextEditor(m_pin->imageToGlobal(ip), shape);
        return true;
    } else if (tool == QLatin1String("callout")) {
        if (tryGrab(ip)) {  // 点中已落下的气泡 = 拖动换位
            m_dragging = true;
            m_pin->update();
            return true;
        }
        auto s = std::make_shared<Callout>(m_color, fontPx);
        s->tail = ip;
        s->pos = ip;
        m_activeShape = s;
        m_dragging = true;
    } else if (tool == QLatin1String("step")) {
        if (tryGrab(ip)) {  // 点中已有序号 = 拖动换位
            m_dragging = true;
            m_pin->update();
            return true;
        }
        // 序号 = 已有序号的最大值 + 1：撤销或擦掉中间的序号后，新序号会补位
        int num = 1;
        for (const ShapePtr& sh : m_engine->shapes) {
            if (auto* badge = dynamic_cast<StepBadge*>(sh.get()))
                num = qMax(num, badge->number + 1);
        }
        auto s = std::make_shared<StepBadge>(m_color, fontPx, num);
        s->pos = ip;
        m_activeShape = s;
        m_dragging = true;
    } else {
        ShapePtr s;
        if (tool == QLatin1String("line"))
            s = std::make_shared<Line>(m_color, w);
        else if (tool == QLatin1String("dash_line"))
            s = std::make_shared<DashLine>(m_color, w);
        else if (tool == QLatin1String("arrow"))
            s = std::make_shared<Arrow>(m_color, w);
        else if (tool == QLatin1String("double_arrow"))
            s = std::make_shared<DoubleArrow>(m_color, w);
        else if (tool == QLatin1String("rect"))
            s = std::make_shared<Rect>(m_color, w);
        else if (tool == QLatin1String("ellipse"))
            s = std::make_shared<Ellipse>(m_color, w);
        else if (tool == QLatin1String("curve"))
            s = std::make_shared<Curve>(m_color, w);
        else if (tool == QLatin1String("spotlight"))
            s = std::make_shared<Spotlight>();
        else
            return false;
        if (auto* cv = dynamic_cast<Curve*>(s.get())) {
            // 曲线四个点全落起点（拖动中 dragTo 保持控制点三等分）
            cv->p0 = ip;
            cv->p1 = ip;
            cv->p2 = ip;
            cv->p3 = ip;
        } else {
            s->setEndpoints({ip, ip});
        }
        m_activeShape = s;
        m_dragging = true;
    }
    m_pin->update();
    return true;
}

void PinAnnotator::move(const QPointF& pos) {
    // 拖动中更新：二次编辑改端点/平移，绘制中更新活动形状或擦除；
    // 无交互时仅刷新悬停预览（橡皮红框跟随光标）。
    const QPointF ip = m_pin->localToImage(pos);
    m_cursorIp = ip;
    const double tol = 8.0 * m_pin->refDpr();
    if (m_dragging && m_editShape) {
        if (m_editPt >= 0) {
            QList<QPointF> pts = m_editShape->endpoints();
            if (m_editPt < pts.size()) {
                pts[m_editPt] = ip;
                m_editShape->setEndpoints(pts);
            }
        } else {
            const double dx = ip.x() - m_editLast.x();
            const double dy = ip.y() - m_editLast.y();
            if (dx != 0.0 || dy != 0.0) {
                m_editShape->translate(dx, dy);
                m_editDx += dx;
                m_editDy += dy;
                m_editLast = ip;
            }
        }
        m_pin->update();
    } else if (m_dragging && m_activeShape) {
        m_activeShape->dragTo(ip);  // 笔画加点 / 线段与矩形移终点（与选区控制器一致）
        m_pin->update();
    } else if (m_erasing) {
        m_engine->eraseAt(ip, tol);
        m_pin->update();
    } else if (toolIsEraser()) {
        m_pin->update();
    }
}

void PinAnnotator::release() {
    // 抬手收尾：二次编辑记账入栈，新形状过面积校验后入栈。
    if (m_erasing) {
        m_erasing = false;
        m_engine->endEraseStroke();
        syncToolbarState();
        m_pin->update();
        return;
    }
    m_dragging = false;
    if (m_editShape) {
        if (m_editPt >= 0) {
            m_engine->commitResize(m_editShape, m_editBefore, m_editShape->endpoints());
            m_editBefore = m_editShape->endpoints();
        } else {
            // 文字块/序号没有端点表，整体拖动也照记一步撤销（与选区侧一致）
            m_engine->commitMove(m_editShape, m_editDx, m_editDy);
            m_editDx = m_editDy = 0.0;
        }
        syncToolbarState();
        positionToolbar();
        m_pin->update();
        return;
    }
    ShapePtr s = m_activeShape;
    m_activeShape = nullptr;
    if (!s)
        return;
    if (s->kind() == ShapeKind::Callout) {
        // 气泡：拖得太短就把气泡体挪开，免得压在尾巴起点上；然后在气泡位置开输入
        // 框，编辑期间把形状留在 activeShape 上，好让气泡在打字时保持可见。
        // 阈值与挪动量跟选区侧一致（都在图像坐标里算）。
        auto* callout = static_cast<Callout*>(s.get());
        if (qAbs(callout->pos.x() - callout->tail.x()) +
                qAbs(callout->pos.y() - callout->tail.y()) <
            4.0)
            callout->pos += QPointF(16.0, 16.0);
        m_activeShape = s;
        // 编辑框开在气泡体的文字位置（松手点是尾巴附近，气泡体可能被挪开过）
        openTextEditor(m_pin->imageToGlobal(callout->textTopLeft()),
                       std::static_pointer_cast<TextShape>(s));
        return;
    }
    if (s->validOnFinish()) {
        m_engine->add(s);
        if (s->kind() == ShapeKind::SmartErase)
            kickSmartErase(std::static_pointer_cast<SmartErase>(s));
        // 曲线建完保持选中：端点手柄立刻可见，拖线身弯弧（与选区侧一致）
        if (s->kind() == ShapeKind::Curve)
            selectShape(s);
        syncToolbarState();
    }
    positionToolbar();
    m_pin->update();
}

void PinAnnotator::kickSmartErase(const std::shared_ptr<SmartErase>& shape) {
    // 与选区侧 AnnotationController 同一套：取消标志从形状上取，形状析构即置位，
    // 工作线程下一处 checkCancelled 就中止；重绘经 m_pin（QWidget）排队回 UI 线程。
    const QImage base = m_engine->base;
    QPainterPath mask;
    mask.addRect(shape->rect().adjusted(-2, -2, 2, 2).intersected(QRectF(base.rect())));
    QPointer<PinWindow> pin = m_pin;
    QPointer<PinAnnotator> self(this);
    std::weak_ptr<Shape> target = shape;
    const std::shared_ptr<std::atomic_bool> cancel = shape->cancel;
    const QRectF srcRect = shape->rect();
    QThreadPool::globalInstance()->start([self, pin, target, cancel, base, mask, srcRect] {
        const auto t0 = std::chrono::steady_clock::now();
        const QImage patched = smarterase::erase(base, mask, *cancel);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        // 取消不是失败：形状已经不在了，空图是预期结果
        if (cancel->load(std::memory_order_relaxed)) {
            log::debug("pin", QStringLiteral("智能擦除已取消（%1 ms）").arg(qRound(ms)));
            return;
        }
        const bool ok = !patched.isNull();
        auto live = target.lock();
        // 先记源区再落 patched：绘制以 patched 非空为准，顺序反了会读到空源区
        if (live && live->kind() == ShapeKind::SmartErase) {
            auto* se = static_cast<SmartErase*>(live.get());
            se->patchRect = srcRect;
            se->patched = patched;
        }
        if (ok)
            log::info("pin", QStringLiteral("智能擦除完成：%1 ms").arg(qRound(ms)));
        else
            log::warn("pin", QStringLiteral("智能擦除失败（%1 ms）").arg(qRound(ms)));
        if (self)
            QMetaObject::invokeMethod(
                self,
                [self, pin, target, ok] {
                    if (ok)
                        pin->update();
                    else
                        self->dropSmartErase(target);  // 失败：红框占位别留在贴图上
                },
                Qt::QueuedConnection);
    });
}

void PinAnnotator::dropSmartErase(const std::weak_ptr<Shape>& target) {
    // 同选区侧 AnnotationController：失败静默移除，不进撤销栈
    const auto s = target.lock();
    if (!s || !m_engine)
        return;
    auto& v = m_engine->shapes;
    const auto it = std::find(v.begin(), v.end(), s);
    if (it == v.end())
        return;
    v.erase(it);
    m_pin->update();
}

// ---- 二次编辑 ----

int PinAnnotator::endpointAt(const Shape& s, const QPointF& ip) const {
    // 端点命中（比整体抓取更优先，容忍半径略大；手柄小、命中圈大一圈才点得中）
    const double tol = 14.0 * m_pin->refDpr();
    QList<QPointF> pts = s.endpoints();
    // 曲线只认两端的端点手柄（与选区侧一致）：控制点没有手柄，弯它走拖线身
    if (const auto* cv = dynamic_cast<const Curve*>(&s))
        pts = {cv->p0, cv->p3};
    for (int i = 0; i < pts.size(); ++i) {
        if ((pts[i] - ip).manhattanLength() <= tol)
            return i;
    }
    return -1;
}

bool PinAnnotator::tryGrab(const QPointF& ip) {
    ShapePtr s = shapeAt(ip);
    if (!s) {
        selectShape(nullptr);
        return false;
    }
    selectShape(s);
    m_editPt = endpointAt(*s, ip);
    // 曲线抓到「肚子」上 = 弯它（与选区侧 tryGrabShape 同一套）：就近控制点
    if (m_editPt < 0) {
        if (const auto* cv = dynamic_cast<const Curve*>(s.get()))
            m_editPt = (ip - cv->p1).manhattanLength() <= (ip - cv->p2).manhattanLength()
                           ? 1
                           : 2;
    }
    m_editBefore = s->endpoints();
    m_editLast = ip;
    return true;
}

void PinAnnotator::applyStyle(const QColor* color, const double* width) {
    // 选中图形时改颜色/粗细：直接重新上色（不占撤销步）。
    if (!m_editShape)
        return;
    if (color)
        m_editShape->color = *color;
    if (width)
        m_editShape->width = *width * m_pin->refDpr();
    m_pin->update();
}

bool PinAnnotator::deleteSelected() {
    if (!m_editShape)
        return false;
    m_engine->remove(m_editShape);
    selectShape(nullptr);
    syncToolbarState();
    m_pin->update();
    return true;
}

// ---- 工具条回调 ----

void PinAnnotator::onAction(const QString& action) {
    if (action == QLatin1String("undo") || action == QLatin1String("redo")) {
        if (action == QLatin1String("undo"))
            m_engine->undo();
        else
            m_engine->redo();
        selectShape(nullptr);
        syncToolbarState();
        m_pin->update();
    } else if (action == QLatin1String("finish")) {
        m_pin->setAnnotate(false);
    }
}

void PinAnnotator::onTool(const QString& name) {
    // "move"（双向箭头）和 "none" 一样都表示「回到选择/编辑态」：两侧现在都把
    // move 归一成空工具（选区侧见 overlay.cpp 的 onToolSelected），否则 "move"
    // 会掉进 press() 的创建分支，变成一个点了没反应的死按钮。
    m_tool = (name.isEmpty() || name == QLatin1String("none") ||
              name == QLatin1String("move"))
                 ? QString()
                 : name;
    selectShape(nullptr);
    m_activeShape = nullptr;  // 换工具 = 放弃没画完的图形（与选区侧一致）
    closeTextEditor(false);
    if (m_tool == QLatin1String("mosaic") || m_tool == QLatin1String("blur"))
        m_engine->warmEffects(m_tool == QLatin1String("mosaic"),
                              m_tool == QLatin1String("blur"));  // 提前在后台生成特效底图，首笔不卡
    m_pin->update();
}

void PinAnnotator::onColor(const QColor& color) {
    m_color = color;
    applyStyle(&m_color);
}

void PinAnnotator::onWidth(double width) {
    m_width = width;
    applyStyle(nullptr, &m_width);
}

void PinAnnotator::syncToolbarState() {
    if (!m_toolbar.isNull())
        m_toolbar->setEnabledActions(m_engine->canUndo(), m_engine->canRedo());
}

int PinAnnotator::maskSensitiveLines(const QVector<QRect>& imageRects) {
    // 一整批记为一步撤销：撤销一次就把这次脱敏整体退回，不会剩半截马赛克。
    const QSize bounds = m_pin->displayImage().size();
    int hits = 0;
    m_engine->beginAddBatch();
    for (const QRect& box : imageRects) {
        if (ShapePtr mosaic = mosaicForTextLine(box, bounds)) {
            m_engine->add(mosaic);
            ++hits;
        }
    }
    m_engine->endAddBatch();
    syncToolbarState();
    m_pin->update();
    return hits;
}

// ---- 文字批注 ----

void PinAnnotator::closeTextEditor(bool commit) {
    if (m_textEditor.isNull())
        return;
    TextEditor* editor = m_textEditor.data();
    m_textEditor = nullptr;
    if (commit) {
        editor->commit();  // committed 闭包内自行解绑并收尾
    } else {
        editor->disconnect();
        editor->close();
        editor->deleteLater();
    }
}

void PinAnnotator::openTextEditor(const QPointF& globalTextTl,
                                  const std::shared_ptr<TextShape>& shape) {
    // 在贴图上的指定文字位置打开编辑器（文字工具=落点，气泡=气泡体内文字位），
    // 提交时把文字写入 shape 并入栈；按「文字左上角」对位，提交前后不跳。
    closeTextEditor(false);
    auto* editor = new TextEditor(
        m_color, std::max(13, qRound(shape->font_size / m_pin->refDpr())));
    m_textEditor = editor;
    editor->placeAt(globalTextTl);
    editor->show();
    editor->raise();
    editor->activateWindow();
    editor->setFocus();

    // 闭包 context 用 this（不是 editor）：this（PinAnnotator）先没的话连接自动断，
    // 不会出现「编辑器还活着、回调里摸的却是已析构的标注器」——editor 顶多变成
    // 一个提交无人的孤儿窗，自己关闭，不会崩。
    connect(editor, &TextEditor::committed, this, [this, editor, shape](const QString& text) {
        // 先放手 QPointer 再全断信号（理由见选区侧同款注释）；m_done 守卫
        // （TextEditor::commit）保证这里只会进来一次
        m_textEditor = nullptr;
        editor->disconnect();
        if (!text.trimmed().isEmpty()) {
            shape->lines = text.split(QLatin1Char('\n'));
            m_engine->add(shape);
            syncToolbarState();
        }
        m_activeShape = nullptr;  // 气泡编辑时借放在这里，入栈后必须清，否则重复绘制
        editor->deleteLater();
        positionToolbar();
        m_pin->activateWindow();
        m_pin->setFocus();
        m_pin->update();
    });
    connect(editor, &TextEditor::cancelled, this, [this, editor]() {
        m_textEditor = nullptr;  // 同上：先清跟踪再断信号
        editor->disconnect();
        m_activeShape = nullptr;  // 取消的气泡不能留在活动形状里（会被一直画出来）
        editor->deleteLater();
        positionToolbar();
        m_pin->activateWindow();
        m_pin->setFocus();
        m_pin->update();
    });
}

// ---- 键盘：撤销/重做/删除 ----

bool PinAnnotator::handleKey(QKeyEvent* ev) {
    const int key = ev->key();
    const Qt::KeyboardModifiers mod = ev->modifiers();
    const bool ctrl = mod.testFlag(Qt::KeyboardModifier::ControlModifier);
    if (key == Qt::Key::Key_Delete || key == Qt::Key::Key_Backspace)
        return deleteSelected();
    if (key == Qt::Key::Key_Z && ctrl) {
        if (mod.testFlag(Qt::KeyboardModifier::ShiftModifier))
            m_engine->redo();
        else
            m_engine->undo();
        selectShape(nullptr);
        syncToolbarState();
        m_pin->update();
        return true;
    }
    if (key == Qt::Key::Key_Y && ctrl) {
        m_engine->redo();
        selectShape(nullptr);
        syncToolbarState();
        m_pin->update();
        return true;
    }
    return false;
}

// ---- 烘焙与合成 ----

QImage PinAnnotator::bake(const QImage& baseImg) {
    // 把当前标注层（含活动形状）画进底图副本，返回新图不改原底图。
    QImage img = baseImg.copy();
    QPainter p(&img);
    p.setRenderHint(QPainter::RenderHint::Antialiasing, true);
    m_engine->draw(p);
    if (m_activeShape)
        m_activeShape->draw(p);
    return img;
}

void PinAnnotator::clear() {
    // 清空标注层（烘焙完成后调用；引擎同时清撤销/重做栈）。
    dropInteraction();
    m_engine->clear();
    syncToolbarState();
}

bool PinAnnotator::hasContent() const {
    return !m_engine->shapes.empty() || m_activeShape != nullptr;
}

// ---- 绘制（贴图窗 paintEvent 调用，painter 为窗口逻辑坐标） ----

void PinAnnotator::draw(QPainter& p) {
    // 把标注层叠画到贴图窗：形状栈、活动形状与编辑框/橡皮预览。
    const QRectF r = m_pin->imgRect();
    const double f = m_pin->scaleFactor() / m_pin->refDpr();
    if (f <= 0)
        return;
    p.save();
    p.setRenderHint(QPainter::RenderHint::Antialiasing, true);
    p.setClipRect(r);
    p.translate(r.topLeft());
    p.scale(f, f);
    m_engine->draw(p);
    if (m_activeShape)
        m_activeShape->draw(p);
    const double lw = 1.0 / f;  // 恒定视觉线宽：画进 image 坐标系前除掉缩放
    // 正在创建的马赛克/模糊框：自身只有效果图、没有轮廓，借红虚线把边界画出来
    //（选中态下面 m_editShape 那段已经画了 accent 虚线，所以这里只管创建中）
    if (auto* pr = dynamic_cast<const PixelRegion*>(m_activeShape.get());
        pr && pr->needsSelectionFrame()) {
        p.setPen(QPen(QColor(QStringLiteral("#E53935")), 1.6 * lw,
                      Qt::PenStyle::DashLine));
        p.setBrush(QColor(229, 57, 53, 34));
        p.drawRect(pr->rect());
    }
    if (m_active && toolIsEraser() && m_cursorIp.x() >= 0) {
        ShapePtr target = shapeAt(m_cursorIp);
        if (target) {
            const QRectF box = target->boundingRect();
            if (!box.isEmpty()) {
                p.setPen(QPen(QColor(QStringLiteral("#E53935")), 1.6 * lw,
                              Qt::PenStyle::DashLine));
                p.setBrush(QColor(229, 57, 53, 34));
                p.drawRect(box);
            }
        }
    }
    if (m_active && m_editShape && m_engine->contains(m_editShape)) {
        const QRectF box = m_editShape->boundingRect();
        if (!box.isEmpty()) {
            p.setPen(QPen(m_accent, lw, Qt::PenStyle::DashLine));
            p.setBrush(Qt::BrushStyle::NoBrush);
            p.drawRect(box);
        }
        QList<QPointF> pts = m_editShape->endpoints();
        // 曲线只露两端手柄（与选区侧一致）：控制点藏起来，弯它靠拖线身
        if (const auto* cv = dynamic_cast<const Curve*>(m_editShape.get()))
            pts = {cv->p0, cv->p3};
        if (!pts.isEmpty()) {
            const double hs = 6.5 * lw;
            p.setPen(QPen(QColor(0, 0, 0, 160), lw));
            p.setBrush(QColor(QStringLiteral("#FFFFFF")));
            for (const QPointF& q : pts)
                p.drawRect(QRectF(q.x() - hs, q.y() - hs, hs * 2, hs * 2));
        }
    }
    if (m_erasing && m_cursorIp.x() >= 0) {
        const double rr = 8.0 * m_pin->refDpr();
        p.setPen(QPen(QColor(QStringLiteral("#E53935")), 1.5 * lw, Qt::PenStyle::SolidLine,
                      Qt::PenCapStyle::RoundCap));
        p.setBrush(QColor(229, 57, 53, 26));
        p.drawEllipse(QRectF(m_cursorIp.x() - rr, m_cursorIp.y() - rr, rr * 2, rr * 2));
    }
    p.restore();
}

}  // namespace zpin
