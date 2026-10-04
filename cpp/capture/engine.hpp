// 标注引擎 —— 对象栈 + 撤销/重做 + 马赛克像素化/模糊底图。
// 坐标全部是**本引擎 base 那张图**的物理像素：截图会话的 base 是整屏（虚拟桌面）
// 抓到的底图，贴图的 base 是贴图内容本身。撤销模型：操作栈回退即撤销；重做栈在新操作时清空。
#pragma once

#include <QImage>
#include <QJsonArray>
#include <QList>
#include <QPainter>
#include <QPointF>

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "shapes.hpp"

namespace zpin {

// 由 AnnotationController / PinAnnotator 以 shared_ptr 持有：后台预热线程要能用
// weak_ptr 挡住「预热还没跑完、宿主已经把引擎析构」这种情况。
// 因此两处都必须 make_shared —— 换成 make_unique 的话 weak_from_this() 恒为空，
// 预热会静默失效（只是少了预热，功能本身不坏，所以最难被发现）。
class AnnotateEngine : public std::enable_shared_from_this<AnnotateEngine> {
public:
    QImage base;
    std::vector<ShapePtr> shapes;

    explicit AnnotateEngine(const QImage& baseImg) : base(baseImg) {}

    // 马赛克/模糊特效底图：只在真用到时才生成（一整屏 ARGB32），带缓存。
    const QImage& pixelated();
    const QImage& blurred();

    // 后台线程预热马赛克/模糊底图：选中工具时提前生成，首笔不等主线程。
    // 只预热本次真正选中的那个：两张底图各是一整屏 ARGB32（双 4K 约 63MB），
    // 而高斯模糊的耗时又是马赛克的 8 倍，全算一遍等于用户选马赛克却白付
    // 一份可能整场都用不上的模糊代价。
    void warmEffects(bool wantPixelated, bool wantBlurred);

    // 标注被烘焙进新底图后换用之：派生的马赛克/模糊缓存同时作废，
    // 否则特效还按旧像素取色（贴图旋转/翻转/灰度前必先烘焙）。
    void rebase(const QImage& img);

    // ---- 编辑 ----
    void add(ShapePtr shape);                  // 追加并记一步撤销（注入特效底图）
    void remove(const ShapePtr& shape);        // 移除并记一步撤销（保留 z 序下标）
    bool eraseAt(const QPointF& pt, double tol = 8.0);  // 擦除命中点所在最上层形状
    void beginEraseStroke();                   // 一段连续橡皮涂抹 = 一步撤销
    void endEraseStroke();
    // 批量追加：begin/end 之间的所有 add 合成一步撤销（一键脱敏一次盖 N 块用）。
    void beginAddBatch();
    void endAddBatch();
    void commitMove(const ShapePtr& shape, double dx, double dy);
    void commitResize(const ShapePtr& shape, QList<QPointF> before, QList<QPointF> after);
    void clear();
    void translateAll(double dx, double dy);   // 选区整体平移时标注跟移（不占撤销步）

    void undo();
    void redo();
    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }

    // 按栈顺序把全部形状绘制到画布上。
    void draw(QPainter& p) const;

    // ---- 标注文档（历史再编辑用）----
    // 导出按栈顺序；载入会清空现有形状与两个撤销栈（刚恢复的文档没有「上一步」）。
    // 坐标是 base 的物理像素，所以调用方要保证 base 与导出时同尺寸。
    // origin 非空 = 导出一张「以 base 里 origin 那一点为新原点」的文档：截图会话
    // 在整屏底图上作画，留档存的却是选区那一段，换个原点才能换底图重编辑。
    QJsonArray documentJson(const QPointF& origin = QPointF()) const;
    bool loadDocument(const QJsonArray& doc);

    bool contains(const ShapePtr& s) const;

private:
    // 特效底图注入 + 入栈，不记撤销（add 与 loadDocument 共用）
    void appendShape(ShapePtr shape);

    struct UndoEntry {
        enum class Kind { Add, BatchAdd, Remove, Erase, Move, Resize };
        Kind kind;
        ShapePtr shape;                                 // Add/Remove/Move/Resize
        int idx = -1;                                   // Remove 的原下标
        std::vector<std::pair<ShapePtr, int>> batch;    // Erase/BatchAdd 的 [(shape, 下标)]
        double dx = 0, dy = 0;                          // Move
        QList<QPointF> before, after;                   // Resize
    };

    QImage m_pixelated;
    QImage m_blurred;
    bool m_hasPixelated = false;
    bool m_hasBlurred = false;
    std::mutex m_effectMutex;
    std::vector<UndoEntry> m_undo;
    std::vector<UndoEntry> m_redo;
    std::vector<std::pair<ShapePtr, int>> m_stroke;  // 涂抹中攒着被擦的形状
    bool m_inStroke = false;                         // 正在一段橡皮涂抹里（m_stroke 有效性判据）
    bool m_batching = false;                                   // 批量追加中
    std::vector<std::pair<ShapePtr, int>> m_batch;             // 批量追加攒的 (shape, 下标)
};

}  // namespace zpin
