// 区域选择覆盖层 —— 每屏一个置顶覆盖窗 + SelectionController 跨屏状态机。
// 状态流：hover（遮罩 + 吸附检测：UIA 界面元素 → 视觉内容块 → 整窗）→ creating
// （拖拽出新选区）→ selected（8 手柄/方向键微调/Ctrl+方向扩选/Enter 确认/Esc
// 取消）→ moving（标注跟随平移）/ resizing（放大镜自动出现）。
// 吸附键位：Space 关掉 hover 吸附、Tab 轮换检测层级（auto/win/el），两者只在 hover
// 态生效（creating/selected 的拖拽一直是自由矩形，只受边对齐参考线影响）。UIA 元素
// 查询在独立工作线程进行（snap_worker，查询瞬间遮罩窗在工作线程临时穿透），慢
// provider 自动熔断，UI 线程只收几何结果；悬停高亮框带短过渡动画。
// 选区、命中、事件一律全局逻辑坐标（见 capture.hpp 的坐标约定）。
#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QString>
#include <QWidget>
#include <chrono>
#include <memory>
#include <optional>

#include "capture.hpp"
#include "controller.hpp"
#include "rcore.hpp"
#include "snap_worker.hpp"

class QKeyEvent;
class QScreen;
class QTimer;

namespace zpin {

// 8 个手柄（索引 0..7 = 左上/上中/右上/右中/右下/下中/左下/左中）的锚点位置；
// 交互命中（overlay.cpp）与手柄绘制（overlay_paint.cpp）共用。
inline constexpr int kHandleCount = 8;
inline QPointF handlePoint(const QRectF& r, int i) {
    switch (i) {
        case 0: return QPointF(r.left(), r.top());
        case 1: return QPointF(r.center().x(), r.top());
        case 2: return QPointF(r.right(), r.top());
        case 3: return QPointF(r.right(), r.center().y());
        case 4: return QPointF(r.right(), r.bottom());
        case 5: return QPointF(r.center().x(), r.bottom());
        case 6: return QPointF(r.left(), r.bottom());
        default: return QPointF(r.left(), r.center().y());  // ML
    }
}

class AnnotationController;
class SelectionController;

// 单屏覆盖窗：只做事件转发与绘制，状态全部在 controller。
class OverlayWindow : public QWidget {
    Q_OBJECT

public:
    OverlayWindow(SelectionController* controller, QScreen* screen);

protected:
    void paintEvent(QPaintEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;
    void mouseDoubleClickEvent(QMouseEvent* ev) override;
    void keyPressEvent(QKeyEvent* ev) override;

private:
    SelectionController* m_controller = nullptr;
};

// 截图放大镜：光标附近像素的 Nearest 放大 + 像素网格 + 坐标/色值信息条。
class Magnifier : public QWidget {
    Q_OBJECT

public:
    explicit Magnifier(SelectionController* controller);
    void updateAt(const QPointF& cursor);

protected:
    void paintEvent(QPaintEvent* ev) override;

private:
    SelectionController* m_controller = nullptr;
    QString m_info;
    QRect m_src;
    QPoint m_srcOff;
};

class SelectionController : public QObject, public AnnotHost {
    Q_OBJECT

public:
    explicit SelectionController(QObject* parent = nullptr);
    // 贴图内容矩形提供者：贴图窗本体含阴影边距，DWM 枚举的「整窗矩形」比
    // 可见内容大一圈，吸附缓存用它把整窗矩形替换成内容矩形
    void setPinRectsProvider(std::function<QVector<rcore::WindowRect>()> provider);

    void start(const QString& mode);
    void cancel() override;
    void confirm(const QString& action = QString()) override;
    // 收尾「点了但还没成」的会话：另存被取消、识别没出结果时**不要**调它，
    // 选区与已画标注就原样留着继续编辑。标志位在 teardown 里清，所以会话一旦
    // 被 Esc 收掉或已被新截图顶掉，这里自动变空操作，不会误关新会话。
    void finishPending();
    // 另存为之类对话框的父窗：覆盖层是「置顶 + Tool」窗，非置顶的对话框会被压在
    // 它下面（用户只看见一层变暗的遮罩，像卡死了）。给对话框指个覆盖层当爹，
    // 顺带让它开在光标所在那块屏上（混合 DPI 下这点尤其要紧）。
    QWidget* dialogParent();
    bool isActive() const { return m_active; }

    // AnnotHost（标注控制器只读访问共享状态）
    QPointF imgPt(const QPointF& globalPos) const override {
        return m_map.physOf(globalPos);
    }
    double dpr() const override { return m_dpr; }
    QPointF cursorPos() const override { return m_cursor; }
    QString state() const override { return m_state; }
    void setState(const QString& s) override { m_state = s; }
    void updateAll() override;
    void refocusOverlay() override;
    void releaseKeyboardGrab() override;
    QImage baseImage() const override { return m_base; }
    QRectF selRect() const override { return m_rect; }
    QRect bounds() const override { return m_bounds; }
    QPointF physTl() const override { return m_map.physOf(m_rect.topLeft()); }

    // 单屏覆盖层主流程：底图 -> 遮罩/吸附框 -> 标注 -> 辅助线 -> 边框/手柄/标签。
    void paintScreen(OverlayWindow* window, QPainter& p);

    // 悬停高亮的过渡动画：吸附目标变化时从旧框滑到新框（~110ms ease-out）。
    // 动画只是绘制层的观感，m_suggest 始终是权威目标（按下/判定都用它）。
    void setSuggest(const QRectF& rect);
    QRectF paintSuggest();

    // 放大镜内的遮罩参照（画布物理坐标）：有选区用选区，还没有选区时用悬停
    // 吸附建议框；返回无效矩形 = 放大镜内不画遮罩。
    QRect magnifierMaskRect() const;

    // 鼠标/键盘入口（OverlayWindow 转发）
    void onPress(const QPointF& pos);
    void onMove(const QPointF& pos);
    void onRelease(const QPointF& pos);
    void onRightPress();
    bool onKey(QKeyEvent* ev);
    // 工具条一次性动作（脱敏 / 长截图）。
    void runCommand(const QString& cmd) override;

    AnnotationController* annotator() const { return m_annot.get(); }

signals:
    // 成品（已烘焙标注）+ 干净底图 + 以选区为原点的标注文档 + 动作 + 选区全局左上角
    // + 该图的比例。底图与文档只有画过标注才不同于一份成品：留档存「干净底图 + 文档」，
    // 历史条目才能重新载入继续改（见 HistoryStore::add）。
    void captured(const QImage& img, const QImage& base, const QJsonArray& doc,
                  const QString& action, const QPointF& topLeft, double dpr);
    void cancelled();
    // 长截图接管：区域首帧（画布物理裁剪）+ 绝对物理裁剪矩形 + 区域的逻辑
    // 矩形（遮罩挖空/预览面板定位用）+ 滚轮落点 + 屏比例
    void scrollRequested(const QImage& first, const QRect& regionAbs,
                         const QRectF& regionLogical, double dpr);

private:
    // ---- 窗口吸附检测 ----
    QRectF windowUnder();
    QRectF snapTarget();
    std::optional<QRectF> elementUnder();
    // 元素查询在 snap 工作线程异步进行：门槛满足就投递一次（光标挪出上次
    // 查询点、无在途请求、无按键按住）；结果落地在 onQueried。
    void requestElement();
    void onQueried(int id, bool hit, int l, int t, int w, int h);
    std::optional<QRectF> contentUnder(const QRectF& win);
    bool elementUseful(const QRectF& rect, const QRectF& win) const;
    const QVector<rcore::WindowRect>& ensureWinCache();

    // ---- 一次性命令 ----
    // 一键脱敏：后台 OCR 选区 → 文本跑正则（手机号/邮箱/身份证）→ 命中框盖马
    // 赛克（一批 = 一步撤销）。
    void sanitize();
    // 长截图：取选区所在窗口，teardown 后把首帧与滚动参数抛给装配层。
    void startScrollCapture();

    // ---- 对齐参考线 ----
    std::pair<QVector<double>, QVector<double>> guideLines();
    static std::pair<double, bool> nearest(double val, const QVector<double>& cands);
    QRectF snapEdges(const QRectF& rect, std::array<bool, 4> allow);
    QRectF snapMove(const QRectF& rect);

    // ---- 命中测试与几何 ----
    int hitTest(const QPointF& pt) const;
    QPointF fixedCorner(int idx) const;
    void applyResize(const QPointF& pos);
    void applyExtend(const QPointF& pos);
    void clampRect();
    void updateCursorShape(const QPointF& pos);
    OverlayWindow* windowAt(const QPointF& pt) const;
    QScreen* screenAt(const QPointF& pt) const;
    QList<QScreen*> screensOrdered() const;

    // ---- 辅助 ----
    void showToast(const QString& text);  // 选区内顶部短提示（如取色结果）
    void clearToast();
    void updateMagnifier();
    void teardown();

    // ---- 绘制 ----
    void paintHoverLayer(OverlayWindow* window, QPainter& p, const QRect& geo,
                         const std::function<QPointF(const QPointF&)>& toLocal,
                         const QColor& accent, int borderW);
    void paintMask(OverlayWindow* window, QPainter& p, const QRectF& r);
    void paintGuides(OverlayWindow* window, QPainter& p, const QRect& geo, const QRectF& r);
    void paintHandles(QPainter& p, const QRectF& r, const QColor& accent);
    void paintSizeLabel(QPainter& p, const QRectF& r);
    void paintToast(OverlayWindow* window, QPainter& p);
    void drawCornerAnchors(QPainter& p, const QRectF& r, const QColor& color);
    void drawCrosshairIfEnabled(QPainter& p, const QRect& geo,
                                const std::function<QPointF(const QPointF&)>& toLocal);
    void drawLabel(QPainter& p, const QRectF& r, const QString& text, bool below = false,
                   bool dim = false);

    QList<OverlayWindow*> m_windows;
    bool m_active = false;
    QString m_state = "hover";
    QRectF m_rect;
    QRectF m_suggest;
    QPointF m_cursor;
    QPointF m_press;
    QPointF m_moveOffset;
    int m_resizeIdx = -1;
    int m_focusIdx = -1;
    QPointF m_resizeFix;      // 拉伸时固定的对角点
    std::array<bool, 4> m_ext = {false, false, false, false};  // 自动扩选方向 L,R,T,B
    QRectF m_extOrig;         // 扩选按下前的选区（拖坏时回滚）
    QRectF m_pressSuggest;    // 按下瞬间的窗口吸附候选（release 判定单击吸附用）
    QImage m_base;
    capture::DesktopMap m_map;
    QRect m_bounds;
    double m_dpr = 1.0;
    QVector<qintptr> m_hwnds;
    QVector<rcore::WindowRect> m_winCache;
    std::function<QVector<rcore::WindowRect>()> m_pinRects;   // 贴图内容矩形提供者
    std::chrono::steady_clock::time_point m_winCacheAt{};
    std::optional<QRectF> m_elRect;    // 最近一次 UIA 结果（逻辑矩形），nullopt = 无元素
    std::optional<QRectF> m_ctRect;    // 上次视觉内容块（逻辑矩形），nullopt = 没认出来
    std::chrono::steady_clock::time_point m_ctAtTime{};  // 上次内容扫描时刻（节流）
    // ---- 异步元素查询（snap_worker） ----
    snap::SnapWorker* m_snap = nullptr;
    qint64 m_session = 0;              // 会话代号：start 时推进，异步结果按它防串台
    bool m_keepSessionPending = false;  // 本次会话欠一次收尾（另存为/识别类动作）
    int m_elReqId = 0;                 // 单调递增的请求号（结果按号配对）
    bool m_elInFlight = false;         // 一次只放一个请求在途
    QPointF m_elReqCursor{-1e6, -1e6}; // 上次发起查询时的光标位置（逻辑）
    std::chrono::steady_clock::time_point m_elReqAt{};
    snap::LatencySamples m_applyMs;    // 埋点：请求→应用（与 worker 的查询耗时配对）
    int m_elStale = 0;                 // 结果落地时光标已离开而丢弃的次数
    // ---- 高亮过渡动画 ----
    QRectF m_animFrom;
    std::chrono::steady_clock::time_point m_animAt{};
    bool m_animActive = false;
    QTimer* m_animTimer = nullptr;
    bool m_snapFree = false;           // Space 切换：true = hover 不给吸附建议（拖拽本就自由）
    QString m_detect = "auto";         // Tab 轮换：auto / win / el
    QPointer<Magnifier> m_magnifier;
    bool m_magPinned = false;          // Alt 召唤：手动保持放大镜可见
    int m_magFollowHandle = -1;        // 键盘微调时放大镜改看的手柄索引（-1 = 跟光标）
    QString m_toast;                   // 选区内顶部短提示（如取色结果）
    std::chrono::steady_clock::time_point m_toastUntil{};
    QVector<std::pair<QChar, double>> m_guides;  // 命中的对齐参考线 ("v", x) / ("h", y)
    std::unique_ptr<AnnotationController> m_annot;  // 标注子系统
};

}  // namespace zpin
