#include "controller.hpp"

#include <QKeyEvent>
#include <QMetaObject>
#include <QPainter>
#include <QPointer>
#include <QThreadPool>

#include "config.hpp"
#include "engine.hpp"
#include "logging.hpp"
#include "smart_erase.hpp"
#include "text_edit.hpp"
#include "toolbar.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>

namespace zpin {

AnnotationController::AnnotationController(AnnotHost* host)
    : QObject(),
      m_host(host),
      m_color(config::getStr("Interface/theme_color")) {}

AnnotationController::~AnnotationController() {
    // 必须走 reset()：TextEditor 与工具条都是无 parent 的顶层窗，只被 QPointer
    // 记着。析构不收它们，会话结束时它们比控制器活得久，而那两个 committed /
    // cancelled 闭包捕获的是裸 this —— 用户随后一敲回车就是 use-after-free。
    reset();
}

// ---- 已画图形的抓取与二次编辑 ----

ShapePtr AnnotationController::shapeAt(const QPointF& ip) const {
    if (!m_engine)
        return nullptr;
    const double tol = 8.0 * m_host->dpr();
    const auto& shapes = m_engine->shapes;
    for (auto it = shapes.rbegin(); it != shapes.rend(); ++it) {
        if ((*it)->hit(ip, tol))
            return *it;
    }
    return nullptr;
}

int AnnotationController::endpointAt(const Shape& s, const QPointF& ip) const {
    // 端点命中（比整体抓取更优先，容忍半径略大）。手柄本身不大（见 draw 的
    // hs），命中圈要比它大一圈才点得中。
    const double tol = 14.0 * m_host->dpr();
    QList<QPointF> pts = s.endpoints();
    // 曲线只认两端的端点手柄：中间控制点没有手柄（弯它走拖线身），点在原来
    // 控制点的位置不该有「拽住看不见的东西」的手感
    if (const auto* cv = dynamic_cast<const Curve*>(&s))
        pts = {cv->p0, cv->p3};
    for (int i = 0; i < pts.size(); ++i) {
        if (qAbs(pts[i].x() - ip.x()) + qAbs(pts[i].y() - ip.y()) <= tol)
            return i;
    }
    return -1;
}

void AnnotationController::selectShape(const ShapePtr& s) {
    m_editShape = s;
    m_editPt = -1;
    m_editBefore.clear();
    m_editDx = m_editDy = 0.0;
    m_editDrag = false;
}

bool AnnotationController::tryGrabShape(const QPointF& pos) {
    // 点中已画图形 -> 选中并进入拖动/改形状（state=drawing），返回 true
    ShapePtr s = shapeAt(m_host->imgPt(pos));
    if (!s) {
        selectShape(nullptr);
        return false;
    }
    selectShape(s);
    const QPointF ip = m_host->imgPt(pos);
    m_editPt = endpointAt(*s, ip);
    // 曲线抓到「肚子」上 = 弯它：就近拿一颗控制点当拖动目标，而不是整体平移
    // ——用户对着曲线中间按下去想要的就是这个。挪两端抓两颗端点手柄。
    if (m_editPt < 0) {
        if (const auto* cv = dynamic_cast<const Curve*>(s.get()))
            m_editPt = (ip - cv->p1).manhattanLength() <= (ip - cv->p2).manhattanLength()
                           ? 1
                           : 2;
    }
    m_editBefore = s->endpoints();
    m_editLast = ip;
    m_editDrag = true;
    m_host->setState("drawing");
    return true;
}

bool AnnotationController::deleteSelected() {
    // Del：删掉当前选中的已画图形（可撤销）
    if (!m_editShape || !m_engine)
        return false;
    m_engine->remove(m_editShape);
    selectShape(nullptr);
    syncToolbarState();
    positionToolbar();
    m_host->updateAll();
    return true;
}

void AnnotationController::applyStyle(const QColor* color, const double* width) {
    // 选中图形时改颜色/粗细：直接重新上色（不占撤销步）
    if (!m_editShape)
        return;
    if (color)
        m_editShape->color = *color;
    if (width)
        m_editShape->width = *width * m_host->dpr();
    m_host->updateAll();
}

// ---- 生命周期 ----

void AnnotationController::reset() {
    // 清空全部标注状态（start / teardown 时调用）
    closeTextEditor(false);
    if (m_toolbar) {
        m_toolbar->close();
        m_toolbar->deleteLater();
        m_toolbar.clear();
    }
    m_engine.reset();
    m_activeShape.reset();
    selectShape(nullptr);
    m_tool.clear();
}

void AnnotationController::ensure() {
    // 确保标注引擎与工具栏就绪，并把工具栏摆到选区旁
    if (!m_engine)
        m_engine = std::make_shared<AnnotateEngine>(m_host->baseImage());
    if (m_toolbar.isNull()) {
        m_toolbar = new CaptureToolbar(m_color, m_width, kTools, {},
                                       {"cancel", "copy", "pin", "save", "save_as"},
                                       {"ocr", "table", "sanitize", "scroll"});
        connect(m_toolbar, &CaptureToolbar::action, this,
                [this](const QString& a) { onToolbarAction(a); });
        connect(m_toolbar, &CaptureToolbar::toolSelected, this,
                [this](const QString& n) { onToolSelected(n); });
        connect(m_toolbar, &CaptureToolbar::colorSelected, this,
                [this](const QColor& c) { onColorSelected(c); });
        connect(m_toolbar, &CaptureToolbar::widthSelected, this,
                [this](double w) { onWidthSelected(w); });
    }
    positionToolbar();
}

void AnnotationController::positionToolbar() {
    // 把工具栏摆到选区下缘（放不下改上缘），并夹在宿主 bounds 内
    if (m_toolbar.isNull() || !m_host->selRect().isValid())
        return;
    CaptureToolbar* tb = m_toolbar.data();
    tb->adjustSize();
    const QRectF b(m_host->bounds());
    double x = m_host->selRect().left();
    double y = m_host->selRect().bottom() + 10;
    if (y + tb->height() > b.bottom())
        y = m_host->selRect().top() - tb->height() - 10;
    x = qBound(b.left() + 4, x, b.right() - tb->width() - 4);
    y = qMax(b.top() + 4, y);
    tb->move(qRound(x), qRound(y));
    tb->show();
    tb->raise();
}

// ---- 工具栏回调 ----

void AnnotationController::onToolbarAction(const QString& action) {
    if (action == "undo" || action == "redo") {
        if (!m_engine)
            return;
        if (action == "undo")
            m_engine->undo();
        else
            m_engine->redo();
        selectShape(nullptr);
        syncToolbarState();
        m_host->updateAll();
    } else if (action == "cancel") {
        m_host->cancel();
    } else if (action == "sanitize" || action == "scroll") {
        m_host->runCommand(action);
    } else if (action == "copy" || action == "pin" || action == "save" ||
               action == "save_as" || action == "ocr" || action == "table") {
        m_host->confirm(action);
    }
}

void AnnotationController::onToolSelected(const QString& name) {
    // "move"（双向箭头）与空工具同义 = 「选择 / 编辑」态：点已画图形抓取编辑、
    // 空白处拖动整个选区。与贴图侧 onTool 的归一保持一致，否则 "move" 会掉进
    // startDrawing 的创建分支变成再画一个的死按钮。
    m_tool = (name.isEmpty() || name == "none" || name == "move") ? QString() : name;
    selectShape(nullptr);
    m_activeShape.reset();  // 换工具 = 放弃没画完的图形（气泡编辑中断时尤其要紧，否则会一直悬画）
    closeTextEditor(false);
    if ((m_tool == "mosaic" || m_tool == "blur") && m_engine)
        m_engine->warmEffects(m_tool == "mosaic", m_tool == "blur");  // 提前在后台生成特效底图，首笔不卡
    m_host->updateAll();
}

void AnnotationController::onColorSelected(const QColor& color) {
    m_color = color;
    applyStyle(&m_color);
}

void AnnotationController::onWidthSelected(double width) {
    m_width = width;
    applyStyle(nullptr, &m_width);
}

// ---- 文字批注 ----

void AnnotationController::closeTextEditor(bool commit) {
    if (m_textEditor.isNull())
        return;
    TextEditor* editor = m_textEditor.data();
    m_textEditor.clear();
    // TextEditor 是无 parent 的顶层窗，close() 只隐藏不析构：不显式 deleteLater，
    // 每写一条文字/气泡标注就会漏一个窗口（连 HWND 一起）
    if (commit) {
        editor->commit();  // committed 闭包内自行解绑、收尾并 deleteLater
    } else {
        editor->disconnect();
        editor->close();
        editor->deleteLater();
    }
}

void AnnotationController::openTextEditor(const QPointF& globalTextTl,
                                          const std::shared_ptr<TextShape>& shape) {
    closeTextEditor(false);
    TextEditor* editor =
        new TextEditor(m_color, qMax(13, qRound(shape->font_size / m_host->dpr())));
    m_textEditor = editor;
    // globalTextTl 是「文字左上角」的全局逻辑坐标（文字工具=落点，气泡=气泡体
    // 内的文字位）：编辑器按它对位，提交后形状从同一位置画字，前后不跳
    editor->placeAt(globalTextTl);
    editor->show();
    editor->raise();
    editor->activateWindow();
    editor->setFocus();
    // 截图会话期间覆盖层抓着键盘（SelectionController::start 里 grabKeyboard，
    // 为的是设置窗/贴图抢不走按键）。文字编辑器是独立顶层窗，不放开的话键全被
    // 遮罩吃掉、一个字也打不出来；编辑器收尾时 refocusOverlay 会把抓取还回去。
    m_host->releaseKeyboardGrab();

    // 上下文对象用 this（不是 editor）：闭包捕获了裸 this，控制器先没的话
    // 这条连接必须跟着断，否则编辑器后触发 committed/cancelled 就是野指针
    connect(editor, &TextEditor::committed, this,
            [this, editor, shape](const QString& text) {
                // 先放手 QPointer 再全断信号：无参 disconnect 会连 QPointer 的
                // destroyed 跟踪一起断掉，顺序反了 deleteLater 析构后 QPointer
                // 就悬在已释放的编辑器上（日志里那两条 destroyed-signal 警告）
                m_textEditor.clear();
                editor->disconnect();
                m_activeShape.reset();  // 入栈后不能再以 active 身份重画
                if (!text.trimmed().isEmpty() && m_engine) {
                    shape->lines = text.split(QLatin1Char('\n'));
                    if (shape->lines.isEmpty())
                        shape->lines.append(text);
                    m_engine->add(shape);
                    syncToolbarState();
                }
                m_host->refocusOverlay();
                // 置顶必须在 refocus 之后：activateWindow 会把遮罩重新抬到工具栏上面
                positionToolbar();
                m_host->updateAll();
                editor->deleteLater();
            });
    connect(editor, &TextEditor::cancelled, this, [this, editor]() {
        m_textEditor.clear();  // 同上：先清跟踪再断信号
        editor->disconnect();
        m_activeShape.reset();
        m_host->refocusOverlay();
        positionToolbar();
        m_host->updateAll();
        editor->deleteLater();
    });
}

// ---- 绘制 ----

double AnnotationController::fontPx() const {
    // 文字/气泡字号（物理像素）：跟随粗细档，中档仍是 16px 基准
    return qMax(13.0, 10.0 + m_width * 1.5) * m_host->dpr();
}

void AnnotationController::startDrawing(const QPointF& pos) {
    const QPointF ip = m_host->imgPt(pos);
    const QString tool = m_tool;
    // 曲线画完保持选中（见 finishDrawing）：带着曲线工具点它/点手柄是想弯它，
    // 不是再画一条——命中选中曲线或其端点就直接转入编辑。要在它身上另起一条：
    // 从空白处起笔，或先换别的工具再换回来。
    if (tool == "curve") {
        const auto cv = std::dynamic_pointer_cast<Curve>(m_editShape);
        if (cv && m_engine && m_engine->contains(cv) &&
                (endpointAt(*cv, ip) >= 0 || cv->hit(ip, 2.0 * m_host->dpr())) &&
                tryGrabShape(pos))
            return;
    }
    if (tool != "text" && tool != "callout")
        selectShape(nullptr);  // 画新东西前先取消二次编辑选中
    const double w = m_width * m_host->dpr();
    const double fontPxV = fontPx();

    if (tool == "pen" || tool == "marker") {
        auto s = std::make_shared<Stroke>(m_color, w, tool == "marker");
        s->addPoint(ip);
        m_activeShape = s;
        m_host->setState("drawing");
    } else if (tool == "smart_erase") {
        // 框选一块区域，抬手后异步重建背景（毫秒到秒级）；计算落地前画红框占位
        auto s = std::make_shared<SmartErase>();
        s->p1 = ip;
        s->p2 = ip;
        m_activeShape = s;
        m_host->setState("drawing");
    } else if (tool == "eraser") {
        m_host->setState("erasing");
        if (m_engine) {
            m_engine->beginEraseStroke();  // 一整段涂抹 = 一步撤销
            m_engine->eraseAt(ip, 8.0 * m_host->dpr());
        }
    } else if (tool == "text") {
        if (tryGrabShape(pos))  // 点中已落下的文字块 -> 拖动换位
            return;
        auto shape = std::make_shared<TextShape>(m_color, fontPxV);
        shape->pos = ip;
        openTextEditor(pos, shape);
    } else if (tool == "callout") {
        if (tryGrabShape(pos))  // 点中已落下的气泡块 -> 拖动换位
            return;
        auto s = std::make_shared<Callout>(m_color, fontPxV);
        s->tail = ip;
        s->pos = ip;
        m_activeShape = s;
        m_host->setState("drawing");
    } else if (tool == "step") {
        if (tryGrabShape(pos))  // 点中已有序号 -> 拖动换位
            return;
        // 序号 = 已落序号的最大值 + 1：撤销/擦掉中间的序号后，新序号会补位
        int num = 1;
        if (m_engine) {
            for (const ShapePtr& sh : m_engine->shapes) {
                if (auto* badge = dynamic_cast<StepBadge*>(sh.get()))
                    num = qMax(num, badge->number + 1);
            }
        }
        auto s = std::make_shared<StepBadge>(m_color, fontPxV, num);
        s->pos = ip;
        m_activeShape = s;
        m_host->setState("drawing");
    } else if (tool == "line" || tool == "dash_line" || tool == "arrow" ||
               tool == "double_arrow" || tool == "rect" || tool == "ellipse" ||
               tool == "mosaic" || tool == "blur" || tool == "spotlight" ||
               tool == "curve") {
        ShapePtr s;
        if (tool == "line")
            s = std::make_shared<Line>(m_color, w);
        else if (tool == "dash_line")
            s = std::make_shared<DashLine>(m_color, w);
        else if (tool == "arrow")
            s = std::make_shared<Arrow>(m_color, w);
        else if (tool == "double_arrow")
            s = std::make_shared<DoubleArrow>(m_color, w);
        else if (tool == "curve")
            s = std::make_shared<Curve>(m_color, w);
        else if (tool == "rect")
            s = std::make_shared<Rect>(m_color, w);
        else if (tool == "ellipse")
            s = std::make_shared<Ellipse>(m_color, w);
        else if (tool == "spotlight")
            s = std::make_shared<Spotlight>();
        else if (tool == "mosaic") {
            auto mo = std::make_shared<Mosaic>();
            // 像素化底图现在就注入：拖拽过程中的 draw() 要能看见效果
            if (m_engine)
                mo->pixelated = &m_engine->pixelated();
            s = mo;
        } else {
            auto bl = std::make_shared<Blur>();
            if (m_engine)
                bl->blurred = &m_engine->blurred();  // 同马赛克：拖拽过程就要能看见
            s = bl;
        }
        // 马赛克/模糊是框选形状（继承 Rect），几何与矩形同款；线类另有一套；
        // 曲线四个点先全落起点（拖动中 dragTo 会保持控制点三等分）
        if (auto* two = dynamic_cast<Line*>(s.get())) {
            two->p1 = ip;
            two->p2 = ip;
        } else if (auto* two = dynamic_cast<Rect*>(s.get())) {
            two->p1 = ip;
            two->p2 = ip;
        } else if (auto* cv = dynamic_cast<Curve*>(s.get())) {
            cv->p0 = ip;
            cv->p1 = ip;
            cv->p2 = ip;
            cv->p3 = ip;
        }
        m_activeShape = s;
        m_host->setState("drawing");
    }
}

void AnnotationController::updateDrawing(const QPointF& pos) {
    const QPointF ip = m_host->imgPt(pos);
    if (m_editDrag && m_editShape) {
        if (m_editPt >= 0) {
            QList<QPointF> pts = m_editShape->endpoints();
            if (m_editPt < pts.size()) {
                pts[m_editPt] = ip;  // 拖端点改形状
                m_editShape->setEndpoints(pts);
            }
        } else {
            const double dx = ip.x() - m_editLast.x(), dy = ip.y() - m_editLast.y();
            if (dx != 0 || dy != 0) {
                m_editShape->translate(dx, dy);  // 整体拖动，实时跟随光标
                m_editDx += dx;
                m_editDy += dy;
                m_editLast = ip;
            }
        }
        return;
    }
    if (!m_activeShape)
        return;
    m_activeShape->dragTo(ip);
}

void AnnotationController::finishDrawing(const QPointF& pos) {
    m_host->setState("selected");
    if (m_editDrag && m_editShape) {
        ShapePtr s = m_editShape;
        m_editDrag = false;
        if (m_engine) {
            if (m_editPt >= 0) {
                m_engine->commitResize(s, m_editBefore, s->endpoints());
                m_editPt = -1;
                m_editBefore = s->endpoints();
            } else {
                m_engine->commitMove(s, m_editDx, m_editDy);
                m_editDx = m_editDy = 0.0;
            }
            syncToolbarState();
        }
        positionToolbar();
        m_host->updateAll();
        return;
    }
    ShapePtr s = m_activeShape;
    m_activeShape.reset();
    if (!s)
        return;
    if (s->kind() == ShapeKind::Callout) {
        auto* callout = static_cast<Callout*>(s.get());
        // 气泡与尾巴重合时斜移的最小可感知距离：不移的话文字编辑框压住引线终点
        constexpr double kCalloutNudge = 16.0;
        if (qAbs(callout->pos.x() - callout->tail.x()) +
                qAbs(callout->pos.y() - callout->tail.y()) <
            4.0) {
            callout->pos += QPointF(kCalloutNudge, kCalloutNudge);
        }
        m_activeShape = s;  // 编辑期间保持气泡可见，提交/取消时清除
        // 编辑框开在气泡体的文字位置（早先开在松手点——那是尾巴附近，提交后
        // 气泡体在别处，「打字时的框」和「画完的气泡」对不上）
        const QPointF delta =
            (callout->textTopLeft() - m_host->imgPt(pos)) / m_host->dpr();
        openTextEditor(pos + delta, std::static_pointer_cast<TextShape>(s));
        return;
    }
    if (m_engine && s->validOnFinish()) {
        m_engine->add(s);
        if (s->kind() == ShapeKind::SmartErase)
            kickSmartErase(std::static_pointer_cast<SmartErase>(s));
        // 曲线建完保持选中：端点手柄立刻可见，拖线身把直线弯出弧度——否则
        // 用户只会看到一条直线，不知道从哪下手弯
        if (s->kind() == ShapeKind::Curve)
            selectShape(s);
        syncToolbarState();
    }
    // 画完把工具栏重新置顶显示：防止绘制过程 z 序变化把它压到覆盖层下面
    positionToolbar();
    m_host->updateAll();
}

void AnnotationController::eraseAt(const QPointF& pos) {
    if (m_engine)
        m_engine->eraseAt(m_host->imgPt(pos), 8.0 * m_host->dpr());
}

void AnnotationController::kickSmartErase(const std::shared_ptr<SmartErase>& shape) {
    // 修复跑在全局线程池（单次几百毫秒到两秒）。取消标志从形状上取：形状被撤销/
    // 删除/换会话时析构会置位，工作线程下一处 checkCancelled 就中止，不必白算完。
    // base 是 QImage 隐式共享，拷进线程只读安全。
    if (!m_engine)
        return;
    const QImage base = m_engine->base;
    // 框外扩 2px：修复结果贴着硬边会有一圈原图残留，微扩盖住边缘
    QPainterPath mask;
    mask.addRect(shape->rect().adjusted(-2, -2, 2, 2).intersected(QRectF(base.rect())));
    QPointer<AnnotationController> self(this);
    std::weak_ptr<Shape> target = shape;
    const std::shared_ptr<std::atomic_bool> cancel = shape->cancel;
    const QRectF srcRect = shape->rect();
    QThreadPool::globalInstance()->start([self, target, cancel, base, mask, srcRect] {
        const auto t0 = std::chrono::steady_clock::now();
        const QImage patched = smarterase::erase(base, mask, *cancel);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        // 取消不是失败：形状已经不在了（cancel 由它的析构置位），空图是预期结果，
        // 别记 warn、更别去 drop 一个已经消失的形状
        if (cancel->load(std::memory_order_relaxed)) {
            log::debug("controller", QStringLiteral("智能擦除已取消（%1 ms）").arg(qRound(ms)));
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
            log::info("controller", QStringLiteral("智能擦除完成：%1 ms").arg(qRound(ms)));
        else
            log::warn("controller", QStringLiteral("智能擦除失败（%1 ms）").arg(qRound(ms)));
        if (self)
            QMetaObject::invokeMethod(
                self,
                [self, target, ok] {
                    if (ok)
                        self->m_host->updateAll();
                    else
                        self->dropSmartErase(target);  // 失败：红框占位别留在画布上
                },
                Qt::QueuedConnection);
    });
}

void AnnotationController::dropSmartErase(const std::weak_ptr<Shape>& target) {
    // 智能擦除算失败：红框永远等不来结果，把形状从画布撤掉。静默移除不进
    // 撤销栈——栈里对应的 Add 撤销时 find 不中，自然退化成空操作。
    const auto s = target.lock();
    if (!s || !m_engine)
        return;
    auto& v = m_engine->shapes;
    const auto it = std::find(v.begin(), v.end(), s);
    if (it == v.end())
        return;
    v.erase(it);
    m_host->updateAll();
}

void AnnotationController::translateFollowing(double dx, double dy) {
    // 选区整体平移时让全部标注跟移（物理像素）；未提交的活动形状也一起跟移
    if (m_engine)
        m_engine->translateAll(dx, dy);
    if (m_activeShape)
        m_activeShape->translate(dx, dy);
}

void AnnotationController::finishErasing() {
    // 抬手：一整段涂抹合成一步撤销，并把工具栏重新置顶
    if (m_engine) {
        m_engine->endEraseStroke();
        syncToolbarState();
    }
    positionToolbar();
    m_host->updateAll();
}

void AnnotationController::syncToolbarState() {
    if (m_toolbar && m_engine)
        m_toolbar->setEnabledActions(m_engine->canUndo(), m_engine->canRedo());
}

// ---- 键盘：撤销/重做/删除 ----

bool AnnotationController::handleKey(QKeyEvent* ev) {
    const int key = ev->key();
    const Qt::KeyboardModifiers mod = ev->modifiers();
    if (key == Qt::Key_Delete || key == Qt::Key_Backspace)
        return deleteSelected();
    if (key == Qt::Key_Z && (mod & Qt::ControlModifier) && m_engine) {
        if (mod & Qt::ShiftModifier)
            m_engine->redo();
        else
            m_engine->undo();
        selectShape(nullptr);  // 栈一动，选中的形状可能已被删/被重做掉
        syncToolbarState();
        positionToolbar();
        m_host->updateAll();
        return true;
    }
    if (key == Qt::Key_Y && (mod & Qt::ControlModifier) && m_engine) {
        m_engine->redo();
        selectShape(nullptr);
        syncToolbarState();
        positionToolbar();
        m_host->updateAll();
        return true;
    }
    return false;
}

// ---- 标注层绘制（选区视口局部坐标 r） ----

void AnnotationController::draw(QPainter& p, const QRectF& r) {
    if (!m_engine)
        return;
    const double dpr = m_host->dpr();
    const QPointF ptl = m_host->physTl();
    p.save();
    p.setRenderHint(QPainter::RenderHint::Antialiasing, true);
    p.setClipRect(r);
    p.translate(r.topLeft());
    p.scale(1.0 / dpr, 1.0 / dpr);
    p.translate(-ptl.x(), -ptl.y());
    m_engine->draw(p);
    if (m_activeShape)
        m_activeShape->draw(p);
    // 正在创建/调整的马赛克、模糊框：借橡皮预览同款红虚线把边界画出来（这两个
    // 工具自身只贴效果图、没有轮廓）。智能擦除不需要——它在计算落地前本来就画
    // 半透明红框占位。抬手提交后 m_editShape 清空/不再 active，框自动消失，
    // 成品图里不留描边。
    if (auto* pr = dynamic_cast<const PixelRegion*>(
            (m_activeShape ? m_activeShape : m_editShape).get());
        pr && pr->needsSelectionFrame()) {
        p.setPen(QPen(QColor("#E53935"), 1.6 * dpr, Qt::PenStyle::DashLine));
        p.setBrush(QColor(229, 57, 53, 34));
        p.drawRect(pr->rect());
    }
    // 橡皮擦悬停预览：红框标出这一下会被擦掉的对象
    if (m_tool == "eraser" && (m_host->state() == "selected" || m_host->state() == "erasing")) {
        const ShapePtr target = shapeAt(m_host->imgPt(m_host->cursorPos()));
        if (target) {
            const QRectF box = target->boundingRect();
            if (!box.isEmpty()) {
                QPen pen(QColor("#E53935"), 1.6 * dpr, Qt::PenStyle::DashLine);
                p.setPen(pen);
                p.setBrush(QColor(229, 57, 53, 34));
                p.drawRect(box);
            }
        }
    }
    if (m_editShape && m_engine->contains(m_editShape)) {
        const QColor accent(config::getStr("Interface/theme_color"));
        const QRectF box = m_editShape->boundingRect();
        if (!box.isEmpty()) {
            QPen pen(accent, 1, Qt::PenStyle::DashLine);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawRect(box);
        }
        QList<QPointF> pts = m_editShape->endpoints();
        // 曲线只露两端的端点手柄：中间控制点藏起来，弯它靠拖线身（点哪弯哪），
        // 一排四个方块既乱、又让人以为每颗都得单独拖一遍
        if (const auto* cv = dynamic_cast<const Curve*>(m_editShape.get()))
            pts = {cv->p0, cv->p3};
        if (!pts.isEmpty()) {
            const double hs = 6.5 * dpr;
            p.setPen(QPen(QColor(0, 0, 0, 160), 1));
            p.setBrush(QColor("#FFFFFF"));
            for (const QPointF& q : pts)
                p.drawRect(QRectF(q.x() - hs, q.y() - hs, hs * 2, hs * 2));
        }
    }
    p.restore();
    if (m_host->state() == "erasing") {
        // 橡皮头指示圈：红色空心圆标出擦除作用范围，随光标实时移动
        const QPointF ip = m_host->imgPt(m_host->cursorPos());
        const double rr = 8.0 * dpr;
        p.save();
        p.translate(r.topLeft());
        p.scale(1.0 / dpr, 1.0 / dpr);
        p.translate(-ptl.x(), -ptl.y());
        p.setPen(QPen(QColor("#E53935"), 1.5 * dpr, Qt::PenStyle::SolidLine,
                      Qt::PenCapStyle::RoundCap));
        p.setBrush(QColor(229, 57, 53, 26));
        p.drawEllipse(QRectF(ip.x() - rr, ip.y() - rr, rr * 2, rr * 2));
        p.restore();
    }
}

}  // namespace zpin
