#include "engine.hpp"

#include <QJsonObject>
#include <QThread>
#include <QtGlobal>

#include "logging.hpp"
#include "rcore.hpp"

namespace zpin {

namespace {

QImage pixelateImage(const QImage& img, int block = 8) {
    const QImage small = img.scaled(qMax(1, img.width() / block), qMax(1, img.height() / block),
                                    Qt::IgnoreAspectRatio, Qt::FastTransformation);
    return small.scaled(img.width(), img.height(), Qt::IgnoreAspectRatio,
                        Qt::FastTransformation);
}

// 整屏模糊：Rust 三轮盒滤波（O(n)，与半径无关）。
// 早先这里挂了一条「桥接失败就退回 Qt 金字塔三段缩放」的回退链，是条按契约
// 永远走不到的死路：rcore::blurImage 已挡掉 w/h<=0，radius 由 qBound 夹在
// [4,64] 永不为 0，缓冲长度恒等于 w*h*4，three_box_blur 返回长度也恒等。
// 更糟的是它**静默换掉一张不同的图**——真出问题时用户只看到「模糊不对」，
// 日志里一个字都没有。按「无请求不防御」，删掉；真违约就打日志。
QImage blurWhole(const QImage& img) {
    QImage out = rcore::blurImage(img);
    if (out.isNull())
        log::error("engine", QStringLiteral("高斯模糊失败：Rust 桥接返回空图（%1×%2）")
                                  .arg(img.width())
                                  .arg(img.height()));
    return out;
}

}  // namespace

const QImage& AnnotateEngine::pixelated() {
    // 只生成一次；锁住首次生成与后台预热的竞态，之后引用恒有效
    std::lock_guard<std::mutex> lock(m_effectMutex);
    if (!m_hasPixelated) {
        m_pixelated = pixelateImage(base);
        m_hasPixelated = true;
    }
    return m_pixelated;
}

const QImage& AnnotateEngine::blurred() {
    std::lock_guard<std::mutex> lock(m_effectMutex);
    if (!m_hasBlurred) {
        m_blurred = blurWhole(base);
        m_hasBlurred = true;
    }
    return m_blurred;
}

void AnnotateEngine::warmEffects(bool wantPixelated, bool wantBlurred) {
    // 后台预热：属性缓存幂等，重复预热无害；竞态由 pixelated()/blurred() 内的锁兜住。
    // 线程只能握 weak_ptr —— 握住 strong 引用会拖住析构（引擎里是一整屏 ARGB32），
    // 握裸 this 则是 use-after-free：Esc 取消 / 再次发起截图都会让主线程立刻
    // m_engine.reset()，而预热可能还在跑。
    QThread* worker = QThread::create([weak = weak_from_this(), wantPixelated,
                                       wantBlurred]() {
        const std::shared_ptr<AnnotateEngine> self = weak.lock();
        if (!self)
            return;  // 引擎已经随会话结束析构了，这次预热作废
        // 不套 try/catch：pixelateImage 与 rcore::blurImage 都不抛（后者失败返回空图，
        // 由 blurWhole 落日志）。真违约就让它崩进 zpin.log，比静默少一张底图好查。
        if (wantPixelated)
            self->pixelated();
        if (wantBlurred)
            self->blurred();
    });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void AnnotateEngine::rebase(const QImage& img) {
    // 只重置「已生成」标记，不清空缓存本体：现有形状持有 &m_pixelated，
    // 置空会让它们在下次生成前画出空白。调用方（贴图烘焙）会先清空形状栈。
    std::lock_guard<std::mutex> lock(m_effectMutex);
    base = img;
    m_hasPixelated = false;
    m_hasBlurred = false;
}

bool AnnotateEngine::contains(const ShapePtr& s) const {
    return std::find(shapes.begin(), shapes.end(), s) != shapes.end();
}

void AnnotateEngine::appendShape(ShapePtr shape) {
    if (auto* m = dynamic_cast<Mosaic*>(shape.get()))
        m->pixelated = &pixelated();
    else if (auto* b = dynamic_cast<Blur*>(shape.get()))
        b->blurred = &blurred();
    shapes.push_back(std::move(shape));
}

void AnnotateEngine::add(ShapePtr shape) {
    appendShape(std::move(shape));
    if (m_batching) {
        // 批量追加：只攒 (shape, 下标)，一步撤销在 endAddBatch 里记
        m_batch.push_back({shapes.back(), int(shapes.size()) - 1});
        return;
    }
    UndoEntry e;
    e.kind = UndoEntry::Kind::Add;
    e.shape = shapes.back();
    m_undo.push_back(std::move(e));
    m_redo.clear();
}

QJsonArray AnnotateEngine::documentJson(const QPointF& origin) const {
    QJsonArray doc;
    for (const ShapePtr& s : shapes) {
        if (QJsonObject o = shapeToJson(s, origin); !o.isEmpty())
            doc.append(o);
    }
    return doc;
}

bool AnnotateEngine::loadDocument(const QJsonArray& doc) {
    std::vector<ShapePtr> loaded;
    loaded.reserve(doc.size());
    for (const QJsonValue& v : doc) {
        ShapePtr s = shapeFromJson(v.toObject(), base.size());
        if (!s) {
            log::warn("history", QStringLiteral("标注文档里有认不出的形状，整份不载入"));
            return false;  // 半套比没有更糟：形状错位没人看得出来
        }
        loaded.push_back(std::move(s));
    }
    shapes.clear();
    m_undo.clear();
    m_redo.clear();
    for (ShapePtr& s : loaded)
        appendShape(std::move(s));
    return true;
}

void AnnotateEngine::beginAddBatch() {
    m_batching = true;
    m_batch.clear();
}

void AnnotateEngine::endAddBatch() {
    m_batching = false;
    if (m_batch.empty())
        return;
    UndoEntry e;
    e.kind = UndoEntry::Kind::BatchAdd;
    e.batch = std::move(m_batch);
    m_undo.push_back(std::move(e));
    m_redo.clear();
}

void AnnotateEngine::remove(const ShapePtr& shape) {
    const auto it = std::find(shapes.begin(), shapes.end(), shape);
    if (it == shapes.end())
        return;
    UndoEntry e;
    e.kind = UndoEntry::Kind::Remove;
    e.idx = int(it - shapes.begin());
    e.shape = shape;
    shapes.erase(it);
    m_undo.push_back(std::move(e));
    m_redo.clear();
}

bool AnnotateEngine::eraseAt(const QPointF& pt, double tol) {
    for (int i = int(shapes.size()) - 1; i >= 0; --i) {
        if (shapes[size_t(i)]->hit(pt, tol)) {
            ShapePtr shape = shapes[size_t(i)];
            shapes.erase(shapes.begin() + i);
            if (!m_inStroke) {
                UndoEntry e;
                e.kind = UndoEntry::Kind::Remove;
                e.idx = i;
                e.shape = std::move(shape);
                m_undo.push_back(std::move(e));
                m_redo.clear();
            } else {
                // 涂抹中：先攒着，抬手时合成一步撤销
                m_stroke.emplace_back(std::move(shape), i);
            }
            return true;
        }
    }
    return false;
}

void AnnotateEngine::beginEraseStroke() {
    // 上一段没抬手收尾的涂抹先合成一步撤销（橡皮擦到一半右键弹菜单打断、
    // 左键接着擦的场景），否则攒着的那批被擦形状就从撤销栈里消失了
    endEraseStroke();
    m_stroke.clear();
    m_inStroke = true;
}

void AnnotateEngine::endEraseStroke() {
    if (!m_inStroke)
        return;
    m_inStroke = false;
    if (!m_stroke.empty()) {
        UndoEntry e;
        e.kind = UndoEntry::Kind::Erase;
        e.batch = std::move(m_stroke);
        m_undo.push_back(std::move(e));
        m_redo.clear();
    }
    m_stroke.clear();   // 上面的 move 只搬走内容，容量还得还给引擎（别攒着一屏的形状）
}

void AnnotateEngine::commitMove(const ShapePtr& shape, double dx, double dy) {
    if (qAbs(dx) < 0.5 && qAbs(dy) < 0.5)
        return;
    UndoEntry e;
    e.kind = UndoEntry::Kind::Move;
    e.shape = shape;
    e.dx = dx;
    e.dy = dy;
    m_undo.push_back(std::move(e));
    m_redo.clear();
}

void AnnotateEngine::commitResize(const ShapePtr& shape, QList<QPointF> before,
                                  QList<QPointF> after) {
    if (before.size() != after.size())
        return;
    bool same = true;
    for (qsizetype i = 0; i < before.size(); ++i) {
        if (qAbs(after[i].x() - before[i].x()) >= 0.5 ||
            qAbs(after[i].y() - before[i].y()) >= 0.5) {
            same = false;
            break;
        }
    }
    if (same)
        return;
    UndoEntry e;
    e.kind = UndoEntry::Kind::Resize;
    e.shape = shape;
    e.before = std::move(before);
    e.after = std::move(after);
    m_undo.push_back(std::move(e));
    m_redo.clear();
}

void AnnotateEngine::clear() {
    shapes.clear();
    m_undo.clear();
    m_redo.clear();
    m_stroke.clear();
    m_inStroke = false;
}

void AnnotateEngine::translateAll(double dx, double dy) {
    for (const ShapePtr& s : shapes)
        s->translate(dx, dy);
}

void AnnotateEngine::undo() {
    if (m_undo.empty())
        return;
    UndoEntry e = std::move(m_undo.back());
    m_undo.pop_back();
    switch (e.kind) {
        case UndoEntry::Kind::Add: {
            const auto it = std::find(shapes.begin(), shapes.end(), e.shape);
            if (it != shapes.end())
                shapes.erase(it);
            break;
        }
        case UndoEntry::Kind::BatchAdd:
            for (auto it = e.batch.rbegin(); it != e.batch.rend(); ++it) {
                const auto sit = std::find(shapes.begin(), shapes.end(), it->first);
                if (sit != shapes.end())
                    shapes.erase(sit);
            }
            break;
        case UndoEntry::Kind::Remove:
            shapes.insert(shapes.begin() + qMin(e.idx, int(shapes.size())), e.shape);
            break;
        case UndoEntry::Kind::Erase:
            for (auto it = e.batch.rbegin(); it != e.batch.rend(); ++it)
                shapes.insert(shapes.begin() + it->second, it->first);
            break;
        case UndoEntry::Kind::Resize:
            e.shape->setEndpoints(e.before);
            break;
        case UndoEntry::Kind::Move:
            e.shape->translate(-e.dx, -e.dy);
            break;
    }
    m_redo.push_back(std::move(e));
}

void AnnotateEngine::redo() {
    if (m_redo.empty())
        return;
    UndoEntry e = std::move(m_redo.back());
    m_redo.pop_back();
    switch (e.kind) {
        case UndoEntry::Kind::Add:
            shapes.push_back(e.shape);
            break;
        case UndoEntry::Kind::BatchAdd:
            for (const auto& [shape, idx] : e.batch)
                shapes.insert(shapes.begin() + qMin(idx, int(shapes.size())), shape);
            break;
        case UndoEntry::Kind::Remove: {
            const auto it = std::find(shapes.begin(), shapes.end(), e.shape);
            if (it != shapes.end())
                shapes.erase(it);
            break;
        }
        case UndoEntry::Kind::Erase:
            for (const auto& [shape, idx] : e.batch) {
                const auto it = std::find(shapes.begin(), shapes.end(), shape);
                if (it != shapes.end())
                    shapes.erase(it);
            }
            break;
        case UndoEntry::Kind::Resize:
            e.shape->setEndpoints(e.after);
            break;
        case UndoEntry::Kind::Move:
            e.shape->translate(e.dx, e.dy);
            break;
    }
    m_undo.push_back(std::move(e));
}

void AnnotateEngine::draw(QPainter& p) const {
    // 聚光灯的暗区合并成一层再画：暗区 = 画布 − 全部亮区的并集。各画各的话，
    // 第二个聚光灯会把第一个的亮区也压暗（重叠处两层 110 叠成更黑），多块
    // 「同时亮」就不成立。暗区画在栈里第一个聚光灯的位置——它之前的形状被
    // 压暗，之后的形状（亮区里补的标注）照常盖在上面；每个聚光灯只补自己的白边。
    bool dimDone = false;
    for (const ShapePtr& s : shapes) {
        if (s->kind() != ShapeKind::Spotlight) {
            s->draw(p);
            continue;
        }
        if (!dimDone) {
            dimDone = true;
            QPainterPath canvas;
            canvas.addRect(paintBounds(p));
            QPainterPath bright;
            for (const ShapePtr& t : shapes) {
                if (t->kind() == ShapeKind::Spotlight)
                    bright.addRect(static_cast<const Spotlight*>(t.get())->rect());
            }
            p.save();
            p.setPen(Qt::PenStyle::NoPen);
            p.setBrush(QColor(0, 0, 0, 110));
            p.drawPath(canvas.subtracted(bright));
            p.restore();
        }
        static_cast<const Spotlight*>(s.get())->drawBorder(p);
    }
}

}  // namespace zpin
