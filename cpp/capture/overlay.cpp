#include "overlay.hpp"

#include <QClipboard>
#include <QCursor>
#include <QGuiApplication>
#include <QJsonArray>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTimer>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <optional>

#include "config.hpp"
#include "controller.hpp"
#include "engine.hpp"
#include "logging.hpp"
#include "ocr.hpp"
#include "win32util.hpp"

namespace zpin {

namespace {

using clock = std::chrono::steady_clock;

constexpr double kHandleHit = 6.0;
constexpr int kMagSrc = 15;       // 放大镜取样区边长（物理像素，奇数，中心即光标像素）
constexpr int kMagView = 150;     // 放大区显示尺寸（10x 放大）
constexpr int kMagInfoH = 22;     // 坐标/色值信息条高度
constexpr double kMinSize = 2.0;
constexpr double kElMove = 6.0;   // 逻辑 px：光标移动不足则沿用上次的元素矩形
constexpr double kElGap = 0.02;   // 秒：两次 UIA 调用的最小间隔（约 50 次/秒封顶）
constexpr double kElMinPx = 6.0;  // 更小的元素是噪声，不值得吸附
constexpr double kElMaxCover = 0.85;  // 元素盖住窗口 85% 以上 = 整页容器，不如吸附窗口
constexpr double kGuideTol = 8.0;     // 逻辑 px：边/角离参考线多近就吸上去
constexpr double kWinCacheTtl = 0.25;  // 秒：窗口矩形缓存有效期
constexpr double kMinSnapArea = 1600.0;  // 面积小于它的窗口不当吸附/对齐基准（提示气泡之类）
constexpr double kClickSnapPad = 3.0;    // 单击吸附：按下点允许落在候选框外扩这么多（精确踩线太难）
constexpr double kElMinOverlap = 0.6;    // 元素至少六成落在吸附窗口内才算它的
constexpr double kHandleHitRadius = kHandleHit * 2.0;  // 手柄命中判定半径（视觉手柄的两倍，抓取容差）

// 8 个手柄：左上/上中/右上/右中/右下/下中/左下/左中（Tab 循环顺序）
enum Handle { TL, TM, TR, MR, BR, BM, BL, ML };

// 拖某个手柄时在动的边 (左, 右, 上, 下) —— 对齐吸附只作用于这些边
std::array<bool, 4> resizeEdges(int idx) {
    switch (idx) {
        case TL: return {true, false, true, false};
        case TM: return {false, false, true, false};
        case TR: return {false, true, true, false};
        case MR: return {false, true, false, false};
        case BR: return {false, true, false, true};
        case BM: return {false, false, false, true};
        case BL: return {true, false, false, true};
        default: return {true, false, false, false};  // ML
    }
}

// UIA 元素查询已挪到 snap 工作线程（snap_worker.cpp），此处的 hover 链路
// 只读缓存、不再有 COM 调用。

}  // namespace

// ---- OverlayWindow ----

OverlayWindow::OverlayWindow(SelectionController* controller, QScreen* screen)
    : QWidget(nullptr, Qt::WindowType::FramelessWindowHint |
                           Qt::WindowType::WindowStaysOnTopHint | Qt::WindowType::Tool),
      m_controller(controller) {
    setScreen(screen);
    setGeometry(screen->geometry());
    setMouseTracking(true);
    setFocusPolicy(Qt::FocusPolicy::StrongFocus);
    setCursor(Qt::CursorShape::CrossCursor);
}

void OverlayWindow::paintEvent(QPaintEvent*) {
    const auto t0 = clock::now();
    QPainter p(this);
    m_controller->paintScreen(this, p);
    // 慢路径探针：全屏遮罩窗单帧重绘 >30ms 记一笔（定位「拖动选框卡一下」用）
    const double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    if (ms > 30)
        log::info("zpin.sel", QStringLiteral("遮罩窗重绘 %1ms（%2×%3）")
                                  .arg(ms, 0, 'f', 1)
                                  .arg(width())
                                  .arg(height()));
}

void OverlayWindow::mousePressEvent(QMouseEvent* ev) {
    if (ev->button() == Qt::MouseButton::LeftButton)
        m_controller->onPress(ev->globalPosition());
    else if (ev->button() == Qt::MouseButton::RightButton)
        m_controller->onRightPress();
}

void OverlayWindow::mouseMoveEvent(QMouseEvent* ev) {
    m_controller->onMove(ev->globalPosition());
}

void OverlayWindow::mouseReleaseEvent(QMouseEvent* ev) {
    if (ev->button() == Qt::MouseButton::LeftButton)
        m_controller->onRelease(ev->globalPosition());
}

void OverlayWindow::mouseDoubleClickEvent(QMouseEvent* ev) {
    if (ev->button() == Qt::MouseButton::LeftButton)
        m_controller->confirm();
}

void OverlayWindow::keyPressEvent(QKeyEvent* ev) {
    if (m_controller->onKey(ev))
        return;
    QWidget::keyPressEvent(ev);
}

// ---- Magnifier ----

Magnifier::Magnifier(SelectionController* controller)
    : QWidget(nullptr, Qt::WindowType::FramelessWindowHint |
                           Qt::WindowType::WindowStaysOnTopHint | Qt::WindowType::Tool),
      m_controller(controller) {
    setAttribute(Qt::WidgetAttribute::WA_ShowWithoutActivating);
    setAttribute(Qt::WidgetAttribute::WA_TranslucentBackground);
    resize(kMagView + 2, kMagView + kMagInfoH + 2);
}

void Magnifier::updateAt(const QPointF& cursor) {
    // 按光标位置刷新放大内容、信息条与窗口位置（贴边自动翻转）
    const QImage& base = m_controller->baseImage();
    if (base.isNull())
        return;
    // 底图是画布坐标（绝对物理 - 画布原点），必须用 physOf；absOf 在虚拟桌面
    // 原点非 (0,0)（存在位于主屏左/上方的显示器）时会整体错位
    const QPointF cp = m_controller->imgPt(cursor);
    const int px = int(cp.x()), py = int(cp.y());
    const int half = kMagSrc / 2;
    const QRect srcFull(px - half, py - half, kMagSrc, kMagSrc);
    m_src = srcFull.intersected(base.rect());
    // 取样区贴边被裁小时记录偏移：绘制时按偏移原位摆放，中心红框始终对准光标像素
    m_srcOff = QPoint(m_src.left() - srcFull.left(), m_src.top() - srcFull.top());
    const int cx = qBound(0, px, base.width() - 1);
    const int cy = qBound(0, py, base.height() - 1);
    m_info = QString("x:%1 y:%2  %3")
                 .arg(px)
                 .arg(py)
                 .arg(base.pixelColor(cx, cy).name(QColor::HexRgb).toUpper());
    QPoint pos(int(cursor.x()) + 24, int(cursor.y()) + 24);
    QScreen* scr = QGuiApplication::screenAt(pos);
    if (!scr)
        scr = QGuiApplication::primaryScreen();
    if (scr) {
        const QRect g = scr->availableGeometry();
        if (pos.x() + width() > g.right())
            pos.setX(int(cursor.x()) - 24 - width());
        if (pos.y() + height() > g.bottom())
            pos.setY(int(cursor.y()) - 24 - height());
    }
    move(pos);
    raise();
    update();
    show();
}

void Magnifier::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(23, 23, 28, 242));
    p.drawRoundedRect(QRectF(rect()).adjusted(0, 0, -1, -1), 6, 6);
    if (m_src.isEmpty())
        return;
    const QRectF view(1, 1, kMagView, kMagView);
    // 不开平滑变换 = Nearest：放大后是干净的马赛克像素块
    const double zoom = double(kMagView) / kMagSrc;
    const QRectF dst(view.left() + m_srcOff.x() * zoom, view.top() + m_srcOff.y() * zoom,
                     m_src.width() * zoom, m_src.height() * zoom);
    p.drawImage(dst, m_controller->baseImage(), QRectF(m_src));
    if (zoom >= 8) {  // 每个源像素 ≥8 显示像素才画网格，小尺寸时是噪声
        p.setPen(QPen(QColor(255, 255, 255, 36), 1));
        for (int i = 1; i < kMagSrc; ++i) {
            const double x = view.left() + i * zoom;
            p.drawLine(QPointF(x, view.top()), QPointF(x, view.bottom()));
            const double y = view.top() + i * zoom;
            p.drawLine(QPointF(view.left(), y), QPointF(view.right(), y));
        }
    }
    // 选区/吸附框的边界与框外遮罩：贴着选框放大时，仅凭像素本身分不清
    // 这颗像素属于框内还是框外——遮罩和边界线给出与主画面一致的参照
    if (const QRect selPhys = m_controller->magnifierMaskRect(); selPhys.isValid()) {
        // 画布物理 -> 视图坐标：srcFull = m_src 还未被画布裁剪时的取样区
        const double srcL = m_src.left() - m_srcOff.x();
        const double srcT = m_src.top() - m_srcOff.y();
        const QRectF selView(view.left() + (selPhys.left() - srcL) * zoom,
                             view.top() + (selPhys.top() - srcT) * zoom,
                             selPhys.width() * zoom, selPhys.height() * zoom);
        if (selView.intersects(view)) {
            QPainterPath viewPath;
            viewPath.addRect(view);
            QPainterPath selPath;
            selPath.addRect(selView);
            p.setPen(Qt::NoPen);
            p.setBrush(QBrush(QColor(config::getStr("Capture/mask_color"))));
            p.drawPath(viewPath.subtracted(selPath));
            p.setPen(QPen(QColor(config::getStr("Interface/theme_color")), 1));
            p.setBrush(Qt::NoBrush);
            p.drawRect(selView);
        }
    }
    p.setPen(QPen(QColor("#FF2D55"), 2));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(view.center().x() - zoom / 2, view.center().y() - zoom / 2, zoom, zoom));
    p.setPen(QColor(235, 235, 235));
    p.drawText(QRectF(1, view.bottom() + 1, kMagView, kMagInfoH), Qt::AlignCenter, m_info);
}

// ---- SelectionController ----

SelectionController::SelectionController(QObject* parent) : QObject(parent) {
    m_annot = std::make_unique<AnnotationController>(this);
}

void SelectionController::updateAll() {
    for (OverlayWindow* w : m_windows)
        w->update();
}

void SelectionController::refocusOverlay() {
    if (OverlayWindow* w = windowAt(m_cursor)) {
        w->activateWindow();
        w->setFocus();
        w->grabKeyboard();   // 文字编辑器关闭后把抓取收回来（openTextEditor 放的）
    }
    // 原生对话框（另存为）开一轮会抢走激活：工具条是「不接焦点的 Tool 浮窗」，
    // 之后要么被重新激活的遮罩窗压在下面（它坐在遮罩区域里，盖住 = 看不见），
    // 要么还挂着模态阻塞。这里重新定位 + 置顶，取消另存为回到场里工具条才回得来。
    m_annot->positionToolbar();
}

void SelectionController::releaseKeyboardGrab() {
    // releaseKeyboard 是实例方法且只有真正的抓取者会响应，逐个调即可。
    for (OverlayWindow* w : m_windows)
        w->releaseKeyboard();
}

void SelectionController::cancel() {
    if (!m_active)
        return;
    teardown();
    emit cancelled();
}

void SelectionController::confirm(const QString& action) {
    if (!m_active || m_rect.width() < kMinSize || m_rect.height() < kMinSize)
        return;
    m_dpr = m_map.dprOf(m_rect);  // 标注/成品统一用选区主屏的比例
    const QRect crop = m_map.rectOf(m_rect).intersected(m_base.rect());
    if (crop.isEmpty()) {
        cancel();
        return;
    }
    QImage base = m_base.copy(crop);
    QImage img = base;  // 隐式共享：没画标注时两者同一份缓冲，一次拷贝都不多花
    QJsonArray doc;
    if (m_annot->engine() && !m_annot->engine()->shapes.empty()) {
        // 留档要的是「干净底图 + 以选区为原点」的标注文档，重开历史条目才能接着改；
        // 导出与烘焙互不影响，所以先取文档再往 img 上画（画的那一笔会在这里 detach）
        doc = m_annot->engine()->documentJson(QPointF(crop.topLeft()));
        QPainter p(&img);
        p.setRenderHint(QPainter::RenderHint::Antialiasing, true);
        p.translate(-crop.left(), -crop.top());
        m_annot->engine()->draw(p);
        p.end();
    }
    // 这三条动作存在「没拿到结果」的出口（另存为对话框取消 / 没识别到文字或表格）：
    // 先不收界面，等 App 确认结果真落地了再调 finishPending()。否则用户取消一次
    // 另存为，选区和已画标注就一起没了，只能去截图历史里重贴。
    const QString act = action.isEmpty() ? QStringLiteral("copy") : action;
    if (act == QLatin1String("save_as")) {
        m_keepSessionPending = true;
    } else if (act == QLatin1String("ocr") || act == QLatin1String("table")) {
        // 会话留着了就得给就地反馈：否则整屏压着遮罩原地不动最长两秒多（实测整屏
        // 密集识别 2.5s），看着像卡死。与 sanitize() 同一套做法。
        m_keepSessionPending = true;
        showToast(act == QLatin1String("ocr") ? QStringLiteral("正在识别文字……")
                                              : QStringLiteral("正在识别表格……"));
    } else {
        teardown();
    }
    emit captured(img, base, doc, act, m_rect.topLeft(), m_dpr);
}

void SelectionController::finishPending() {
    if (m_keepSessionPending)
        teardown();
}

QWidget* SelectionController::dialogParent() {
    return windowAt(m_cursor);
}

void SelectionController::start(const QString& mode) {
    if (m_active)
        cancel();
    // 重入防护：若上一次截图的覆盖窗还在 deleteLater 排队中（尚未真正销毁），
    // 先同步关掉，避免第二次抓取桌面时与正在销毁的全屏置顶窗竞态（可能导致崩溃）。
    for (OverlayWindow* w : m_windows) {
        w->close();
        w->deleteLater();
    }
    m_windows.clear();
    m_hwnds.clear();
    m_cursor = QPointF(QCursor::pos());
    m_bounds = capture::virtualBounds();
    const auto tStart = clock::now();
    auto [base, map] = capture::grabDesktop();
    const auto tGrab = clock::now();
    m_base = std::move(base);
    m_map = std::move(map);
    m_dpr = m_map.pieceAt(m_cursor).dpr;
    if (m_base.isNull()) {
        log::error("overlay", "截图底图抓取失败");
        return;
    }
    m_state = "hover";
    m_rect = QRectF();
    m_suggest = QRectF();
    m_winCache.clear();
    m_winCacheAt = clock::now() - std::chrono::hours(1);
    m_elRect.reset();
    m_ctRect.reset();
    m_ctAtTime = clock::now() - std::chrono::hours(1);  // 第一次 hover 立刻扫一遍
    m_snapFree = false;                  // Space 状态按次复位，别带进下一张截图
    m_detect = "auto";                   // Tab 检测层级同理
    m_magPinned = false;                 // Alt 召唤同理
    m_toast.clear();                     // 1.6 秒提示同理：Esc 重开的新遮罩上不能还画着上一场的提示
    m_guides.clear();
    m_focusIdx = -1;
    m_annot->reset();
    // 异步元素查询：推进请求号作废旧会话结果，清在途与埋点
    m_elReqId++;
    m_session++;  // 异步回调（脱敏 OCR 等）按它识别旧会话的结果并丢弃
    m_elInFlight = false;
    m_elReqCursor = QPointF(-1e6, -1e6);
    m_applyMs.reset();
    m_elStale = 0;
    m_animActive = false;
    if (m_animTimer)
        m_animTimer->stop();
    m_active = true;
    m_magnifier = new Magnifier(this);
    for (QScreen* screen : screensOrdered()) {
        auto* w = new OverlayWindow(this, screen);
        w->show();
        m_hwnds.append(qintptr(w->winId()));
        m_windows.append(w);
    }
    for (OverlayWindow* w : m_windows)
        win32::setTopmost(reinterpret_cast<win32::Hwnd>(w->winId()));
    // 慢路径探针：会话启动各阶段耗时（定位「首次框选卡一下」用）
    log::info("zpin.sel", QStringLiteral("会话启动：抓屏 %1ms / 建窗 %2ms（%3 块遮罩）")
                              .arg(std::chrono::duration<double, std::milli>(tGrab - tStart)
                                       .count(),
                                   0, 'f', 1)
                              .arg(std::chrono::duration<double, std::milli>(clock::now() -
                                                                             tGrab)
                                       .count(),
                                   0, 'f', 1)
                              .arg(m_windows.size()));
    // 常驻吸附线程：本会话的遮罩窗列表交给它（穿透切换在查询线程上做）
    if (!m_snap) {
        m_snap = snap::worker();
        connect(m_snap, &snap::SnapWorker::queried, this,
                &SelectionController::onQueried);
    }
    QMetaObject::invokeMethod(
        m_snap, [snap = m_snap, hwnds = m_hwnds] { snap->setWindows(hwnds); },
        Qt::QueuedConnection);
    if (OverlayWindow* focused = windowAt(m_cursor)) {
        focused->activateWindow();
        focused->setFocus();
        // 光 activateWindow 不够：设置对话框（非模态 QDialog）和贴图窗都是 Tool 型
        // 置顶窗，系统的 ForegroundLock 常让我们抢不到前台，于是 Esc/Enter 仍然
        // 落进那个旧焦点窗（对话框把 Esc 当 reject 关掉、贴图把 Esc 当退出标注），
        // 表现就是「必须先按一次 Esc 才能正常截图」。键盘抓取绕过焦点归属，
        // 会话期间所有按键都进覆盖层。
        focused->grabKeyboard();
    }
}

void SelectionController::teardown() {
    m_active = false;
    // 会话一收（含 Esc、含被新截图顶掉）就清掉欠账，让之后迟到的
    // finishPending() 自动变空操作——否则旧动作的结果会关掉新截图的会话。
    m_keepSessionPending = false;
    m_annot->reset();
    if (m_magnifier) {
        m_magnifier->close();
        m_magnifier->deleteLater();
        m_magnifier.clear();
    }
    for (OverlayWindow* w : m_windows) {
        // 与 start 的 grabKeyboard 配对：Qt6 的 releaseKeyboard 是实例方法，
        // 且只有真正持有抓取的那个窗会生效，逐个调不会误伤。
        w->releaseKeyboard();
        w->close();
        w->deleteLater();
    }
    m_windows.clear();
    m_hwnds.clear();
    m_animActive = false;
    if (m_animTimer)
        m_animTimer->stop();
    // 会话收尾：埋点落日志（先测量再优化），并把穿透窗口列表归还给工作线程。
    // 队列调用与在途查询串行：在途那查完后样式即已还原，随后列表才被清空。
    if (m_applyMs.count() || m_elStale)
        log::info("overlay", QStringLiteral("元素吸附埋点：请求→应用 %1 次，p50 %2ms / "
                                            "p95 %3ms / p99 %4ms；落地过时丢弃 %5 次")
                                 .arg(m_applyMs.count())
                                 .arg(m_applyMs.percentile(0.50), 0, 'f', 1)
                                 .arg(m_applyMs.percentile(0.95), 0, 'f', 1)
                                 .arg(m_applyMs.percentile(0.99), 0, 'f', 1)
                                 .arg(m_elStale));
    if (m_snap) {
        QMetaObject::invokeMethod(
            m_snap,
            [snap = m_snap] {
                snap->setWindows({});
                snap->dumpTimings();
            },
            Qt::QueuedConnection);
    }
}

QList<QScreen*> SelectionController::screensOrdered() const {
    // 所有屏幕，光标所在屏排最前（决定覆盖窗创建顺序与初始焦点）
    QList<QScreen*> screens;
    if (QScreen* cursorScreen = screenAt(m_cursor))
        screens.append(cursorScreen);
    for (QScreen* s : QGuiApplication::screens())
        if (!screens.contains(s))
            screens.append(s);
    return screens;
}

QScreen* SelectionController::screenAt(const QPointF& pt) const {
    for (QScreen* s : QGuiApplication::screens()) {
        if (QRectF(s->geometry()).contains(pt))
            return s;
    }
    return nullptr;
}

OverlayWindow* SelectionController::windowAt(const QPointF& pt) const {
    for (OverlayWindow* w : m_windows) {
        if (QRectF(w->geometry()).contains(pt))
            return w;
    }
    return m_windows.isEmpty() ? nullptr : m_windows.first();
}

// ---- 窗口吸附检测 ----

const QVector<rcore::WindowRect>& SelectionController::ensureWinCache() {
    const auto now = clock::now();
    if (m_winCache.isEmpty() ||
        std::chrono::duration<double>(now - m_winCacheAt).count() > kWinCacheTtl) {
        // 贴图窗本体比可见内容大一圈阴影边距，DWM 枚举拿到的是整个窗口——
        // 先按句柄排除整窗矩形，再把内容矩形补回列表。贴图是置顶窗，排最前
        // 保证与其它窗口重叠时优先命中贴图。
        QVector<rcore::WindowRect> pins;
        if (m_pinRects)
            pins = m_pinRects();
        QVector<qintptr> excl = m_hwnds;
        for (const rcore::WindowRect& pr : pins)
            excl.append(pr.hwnd);
        m_winCache = rcore::visibleWindowRects(excl);
        for (const rcore::WindowRect& pr : pins)
            m_winCache.prepend(pr);
        m_winCacheAt = now;
    }
    return m_winCache;
}

void SelectionController::setPinRectsProvider(
    std::function<QVector<rcore::WindowRect>()> provider) {
    m_pinRects = std::move(provider);
}

QRectF SelectionController::windowUnder() {
    // 光标所在窗口的逻辑矩形（自动吸附目标）；窗口矩形是绝对物理像素，
    // 必须走 DesktopMap 的分段换算，不能整体除以某个统一 dpr（副屏会错位）
    for (const rcore::WindowRect& wr : ensureWinCache()) {
        const QRectF lr = m_map.logicalOfAbsRect(wr.l, wr.t, wr.r, wr.b);
        if (lr.contains(m_cursor) && lr.width() * lr.height() >= kMinSnapArea)
            return lr;  // _win_cache 已按 z 序，取最上层
    }
    return QRectF();
}

QRectF SelectionController::snapTarget() {
    // hover 吸附目标；受 Space（关掉悬停吸附）与 Tab（检测层级）控制，只影响
    // 悬停高亮与单击选中——按下之后的拖拽走 snapEdges，与此无关。
    // 全部读缓存立即返回（UI 线程不等 COM）：元素矩形由工作线程异步刷新，
    // 光标离开旧结果先落到下一层（内容块/整窗），精确元素随后收敛。
    if (m_snapFree)
        return QRectF();
    const QRectF win = windowUnder();
    if (m_detect == "win" || !config::getBool("Capture/snap_elements"))
        return win;
    if (const auto el = elementUnder())
        return *el;
    if (m_detect == "el")
        return QRectF();
    if (const auto ct = contentUnder(win))
        return *ct;
    return win;
}

std::optional<QRectF> SelectionController::elementUnder() {
    // 光标处的元素矩形（缓存）。光标还在上次结果里（含少量缓冲）就沿用；
    // 出门立即放弃——宁可见下一层的窗口/内容块吸附，也不让高亮吊在
    // 光标已经离开的旧元素上。精确结果由 snap 工作线程异步补上。
    if (!m_elRect)
        return std::nullopt;
    constexpr double kPad = 4.0;
    return m_elRect->adjusted(-kPad, -kPad, kPad, kPad).contains(m_cursor) ? m_elRect
                                                                           : std::nullopt;
}

void SelectionController::requestElement() {
    // 异步 UIA 查询的投递门槛（与旧同步版一致）：光标挪出上次查询点 6px、
    // 距上次发起 ≥20ms、无鼠标按键按住（穿透期间到达的点击会落进下层应用）、
    // 一次只放一个请求在途。慢 provider 的熔断在工作线程内做。
    if (!m_snap || m_elInFlight)
        return;
    if (qAbs(m_cursor.x() - m_elReqCursor.x()) + qAbs(m_cursor.y() - m_elReqCursor.y())
        <= kElMove)
        return;
    if (std::chrono::duration<double>(clock::now() - m_elReqAt).count() < kElGap)
        return;
    if (win32::anyMouseButtonDown())
        return;   // 按键期间的穿透点击会落进下层应用：这一趟干脆不发
    m_elReqId++;
    m_elInFlight = true;
    m_elReqCursor = m_cursor;
    m_elReqAt = clock::now();
    const QPointF ap = m_map.absOf(m_cursor);  // UIA 用的是绝对物理坐标
    QMetaObject::invokeMethod(
        m_snap,
        [snap = m_snap, id = m_elReqId, x = int(ap.x()), y = int(ap.y())] {
            snap->query(id, x, y);
        },
        Qt::QueuedConnection);
}

void SelectionController::onQueried(int id, bool hit, int l, int t, int w, int h) {
    if (!m_active || id != m_elReqId)
        return;  // 会话已结束/旧结果：丢弃（新会话 start 时推进了请求号）
    // 在途标志必须**先**清、再按状态丢弃：结果落地时可能已经进框选态（用户在
    // 结果回来前按下了左键），若被状态门槛挡住不清，回到 hover 后每次
    // requestElement 都被在途标志卡死，元素吸附整场失效（只剩内容块/整窗兜底）。
    m_elInFlight = false;
    if (m_state != "hover")
        return;
    m_applyMs.add(
        std::chrono::duration<double, std::milli>(clock::now() - m_elReqAt).count());
    if (!hit) {
        m_elRect.reset();  // 无命中：保持「无元素」，内容块/整窗兜底已在位
        return;
    }
    const QRect r(l, t, w, h);
    // logicalOfAbsRect 要的是 left+width 这种开区间右/下界（QRect::right() 少 1px）
    const QRectF rect = m_map.logicalOfAbsRect(r.left(), r.top(),
                                               r.left() + r.width(), r.top() + r.height());
    if (!rect.adjusted(-1, -1, 1, 1).contains(m_cursor)) {
        m_elStale++;  // 落地时光标已经走了：丢弃，等下一轮
        return;
    }
    m_elRect = elementUseful(rect, windowUnder()) ? std::optional<QRectF>(rect)
                                                  : std::nullopt;
    setSuggest(snapTarget());  // 收敛：元素优先，缺了内容块/整窗兜底
    updateAll();
}

bool SelectionController::elementUseful(const QRectF& rect, const QRectF& win) const {
    if (rect.width() < kElMinPx || rect.height() < kElMinPx)
        return false;
    if (!win.isValid()) {
        // 没有窗口基准时也不能放行离谱大的元素：桌面/任务栏空白处 UIA
        // 偶尔返回盖满整屏的"链接"类伪元素，吸上它等于全屏框选
        const QRectF b(m_bounds);
        return b.isValid() &&
               rect.width() * rect.height() < b.width() * b.height() * kElMaxCover;
    }
    const QRectF inter = rect.intersected(win);
    if (inter.isEmpty() ||
        inter.width() * inter.height() <
            rect.width() * rect.height() * kElMinOverlap)
        return false;  // 元素不在吸附窗口内（多半是别的全屏透明窗）
    if (!win.contains(rect))
        return false;  // 元素伸出窗口：被滚动视口裁剪的容器。Qt 把滚动区的内容
                       // 控件也暴露成元素，BoundingRectangle 是整份内容的高度
                       // （比可视部分高一大截、伸到窗口外），高亮会盖到窗口
                       // 外面去；控件画不出窗口，伸出去的必然不是真实可见边界，
                       // 交给内容块/整窗兜底
    const double cover = inter.width() * inter.height() / (win.width() * win.height());
    return cover <= kElMaxCover;
}

std::optional<QRectF> SelectionController::contentUnder(const QRectF& win) {
    // UIA 认不出元素时的兜底：在底图上按「与四周背景不同的连通块」长出内容边界
    // （自绘界面的卡片、网页里的图片、没有无障碍信息的控件）。
    constexpr double kCtPad = 4.0;   // 逻辑 px：收紧后的边界可能比光标差几像素
    if (m_ctRect && m_ctRect->adjusted(-kCtPad, -kCtPad, kCtPad, kCtPad).contains(m_cursor))
        return m_ctRect;  // 还在同一块里：沿用，既不重复扫描也不让边界发抖
    const auto now = clock::now();
    if (std::chrono::duration<double>(now - m_ctAtTime).count() < kElGap)
        return m_ctRect;
    m_ctAtTime = now;
    const auto giveUp = [&]() {
        m_ctRect.reset();
        return std::nullopt;
    };
    if (!config::getBool("Capture/snap_content") || m_base.isNull())
        return giveUp();
    // 取景 = 光标周围 800x800 物理像素方框 ∩ 悬停窗口 ∩ 画布。方框大小就是耗时
    // 与能认下的块大小（实测见 README 的「响应」）。方框左上角对齐到 64px 网格：
    // 光标在格内移动时取景框不跟着挪，「块是否被框切断」的判定才不会来回翻。
    const double dpr = m_map.pieceAt(m_cursor).dpr;
    const QPointF cp = m_map.physOf(m_cursor);
    const QRect canvas = m_base.rect();
    const QRect winC = win.isValid() ? m_map.rectOf(win).intersected(canvas) : QRect();
    constexpr int kHalf = 400;
    constexpr int kGrid = 64;   // 2 的幂：& -kGrid 就是向下取整到网格（负坐标也对）
    QRect crop((int(cp.x()) - kHalf) & -kGrid, (int(cp.y()) - kHalf) & -kGrid,
               kHalf * 2, kHalf * 2);
    crop = crop.intersected(canvas);
    if (winC.isValid()) {
        // 窗口只剩一条窄边时（透明辅助窗、光标贴着窗口边）求交结果没法用，忽略窗口
        const QRect isect = crop.intersected(winC);
        if (isect.width() >= 160 && isect.height() >= 160)
            crop = isect;
    }
    const QPoint seed(int(cp.x()) - crop.x(), int(cp.y()) - crop.y());  // 取景框局部坐标
    if (crop.width() < 24 || crop.height() < 24 ||
        seed.x() < 0 || seed.y() < 0 || seed.x() >= crop.width() || seed.y() >= crop.height())
        return giveUp();
    // 取景框的每条边是「真实边界」（屏幕边或窗口边）还是「人为切的」：块贴到
    // 人为切的边上还在框外延续，那时的包围盒是裁出来的假边界，Rust 侧会拒绝
    int trust = 0;
    if (crop.left() <= canvas.left() || (winC.isValid() && crop.left() <= winC.left()))
        trust |= 1;
    if (crop.top() <= canvas.top() || (winC.isValid() && crop.top() <= winC.top()))
        trust |= 2;
    if (crop.right() >= canvas.right() || (winC.isValid() && crop.right() >= winC.right()))
        trust |= 4;
    if (crop.bottom() >= canvas.bottom() || (winC.isValid() && crop.bottom() >= winC.bottom()))
        trust |= 8;
    const auto box = rcore::contentRect(m_base.copy(crop), seed, int(3.0 * dpr), trust);
    if (!box)
        return giveUp();
    // 内容缓冲坐标 -> 画布坐标 -> 绝对物理 -> 逻辑（右/下界传开区间，见 capture.hpp）
    const QRect canv = QRect(crop.topLeft() + box->topLeft(), box->size())
                           .translated(m_map.origin);
    m_ctRect = m_map.logicalOfAbsRect(canv.left(), canv.top(),
                                      canv.left() + canv.width(), canv.top() + canv.height());
    return m_ctRect;
}

// ---- 一次性命令（脱敏 / 长截图） ----

void SelectionController::runCommand(const QString& cmd) {
    if (cmd == QLatin1String("sanitize"))
        sanitize();
    else if (cmd == QLatin1String("scroll"))
        startScrollCapture();
}

void SelectionController::sanitize() {
    if (m_state != "selected" || !m_annot->engine())
        return;
    const QRect crop = m_map.rectOf(m_rect).intersected(m_base.rect());
    if (crop.width() < 8 || crop.height() < 8)
        return;
    showToast(QStringLiteral("正在识别敏感信息……"));
    const qint64 session = m_session;
    ocr::recognizeBoxesAsync(m_base.copy(crop),
                             [this, crop, session](std::optional<QVector<rcore::OcrBox>> boxes) {
            // 只挡状态变了还不够：Esc 后立刻重新截图也回到 selected，旧会话的
            // 打码框会盖到新截图上 —— 必须连会话代号一起对上
            if (session != m_session || m_state != "selected")
                return;
            AnnotateEngine* engine = m_annot->engine();
            if (!engine)
                return;
            if (!boxes || boxes->isEmpty()) {
                log::info("zpin.sanitize", QStringLiteral("OCR 没有识别到任何文本行"));
                showToast(QStringLiteral("没有识别到文字"));
                return;
            }
            // 诊断日志：正则没命中时，对着看 OCR 把文本识别成了什么（@ 误成全角等）
            for (const rcore::OcrBox& b : *boxes) {
                log::debug("zpin.sanitize", QStringLiteral("OCR 行 (%1,%2 %3×%4)：%5")
                                                .arg(b.rect.x())
                                                .arg(b.rect.y())
                                                .arg(b.rect.width())
                                                .arg(b.rect.height())
                                                .arg(b.text));
            }
            log::info("zpin.sanitize",
                      QStringLiteral("OCR 识别 %1 行，进入正则匹配").arg(boxes->size()));
            int hits = 0;
            engine->beginAddBatch();
            for (const rcore::OcrBox& box : *boxes) {
                if (!ocr::looksSensitiveText(box.text))
                    continue;
                // 文本框（选区局部）-> 底图画布坐标；外扩 3px 与裁进图内由共用函数负责
                if (ShapePtr mosaic = mosaicForTextLine(box.rect.translated(crop.topLeft()),
                                                        m_base.size())) {
                    engine->add(mosaic);
                    hits++;
                }
            }
            engine->endAddBatch();
            showToast(hits ? QStringLiteral("已自动打码 %1 处（Ctrl+Z 可撤销）").arg(hits)
                           : QStringLiteral("没有发现手机号 / 邮箱 / 身份证"));
            updateAll();
        });
}

void SelectionController::startScrollCapture() {
    if (m_state != "selected" || m_rect.width() < kMinSize || m_rect.height() < kMinSize)
        return;
    // 纯手动滚动：不再需要先认出「选区中心是哪个窗口」——程序不合成滚轮，
    // 用户把鼠标停在哪儿、用滚轮还是滚动条还是 PgDn 都行。早先这里认不出
    // 目标窗口就拒绝（"没有找到可滚动的窗口"），选区中心正好落在桌面/缝隙上
    // 时会白白挡掉一次合法的长截图。
    const QRect physRaw = m_map.rectOf(m_rect).intersected(m_base.rect());
    if (physRaw.width() < 32 || physRaw.height() < 64) {
        showToast(QStringLiteral("选区太小，不适合长截图"));
        return;
    }
    // 宽高一律向下取偶：拼接器内部把帧高归偶（Rust 侧 stitch.rs 的
    // StitchSession::new），这里同步取偶，抓帧区域与会话帧高才真正一致，
    // 不会白丢帧底那一行。奇数尺寸进拼接器会让画布与对齐用的灰度金字塔
    // 逐帧错开半行，长图从第一段起就重复/丢行。
    const QRect phys(physRaw.left(), physRaw.top(), physRaw.width() & ~1,
                     physRaw.height() & ~1);
    if (phys.width() < 32 || phys.height() < 64) {
        showToast(QStringLiteral("选区太小，不适合长截图"));
        return;
    }
    const QRect regionAbs(phys.topLeft() + m_map.origin, phys.size());
    const QImage first = m_base.copy(phys);
    const double dpr = m_map.dprOf(m_rect);
    const QRectF regionLogical = m_rect;  // teardown 会清状态，先取走
    teardown();  // 收掉选区界面，屏幕还原后才能干净地滚动抓帧
    emit scrollRequested(first, regionAbs, regionLogical, dpr);
}

// ---- 对齐参考线 ----

std::pair<QVector<double>, QVector<double>> SelectionController::guideLines() {
    // 对齐候选线（逻辑坐标）：每屏的四边与中线 + 其它可见窗口的边与中线
    QVector<double> xs, ys;
    for (QScreen* s : QGuiApplication::screens()) {
        const QRectF g(s->geometry());
        xs << g.left() << g.center().x() << g.right();
        ys << g.top() << g.center().y() << g.bottom();
    }
    for (const rcore::WindowRect& wr : ensureWinCache()) {
        const QRectF lr = m_map.logicalOfAbsRect(wr.l, wr.t, wr.r, wr.b);
        if (lr.width() * lr.height() < kMinSnapArea)
            continue;  // 小窗/提示窗当基准只会添乱
        xs << lr.left() << lr.center().x() << lr.right();
        ys << lr.top() << lr.center().y() << lr.bottom();
    }
    return {xs, ys};
}

std::pair<double, bool> SelectionController::nearest(double val, const QVector<double>& cands) {
    double best = val, dist = kGuideTol + 1.0;
    for (double c : cands) {
        if (qAbs(c - val) < dist) {
            best = c;
            dist = qAbs(c - val);
        }
    }
    return {best, dist <= kGuideTol};
}

QRectF SelectionController::snapEdges(const QRectF& rect, std::array<bool, 4> allow) {
    // 只把在动的边吸到最近的参考线上；命中的线记进 m_guides
    if (!config::getBool("Capture/snap_guides")) {
        m_guides.clear();
        return rect;
    }
    auto [xs, ys] = guideLines();
    double l = rect.left(), t = rect.top(), r = rect.right(), b = rect.bottom();
    const auto [al, ar, at, ab] = allow;
    QVector<std::pair<QChar, double>> guides;
    if (al) {
        auto [v, hit] = nearest(l, xs);
        if (hit && v < r - 1) {
            l = v;
            guides.append({'v', v});
        }
    }
    if (ar) {
        auto [v, hit] = nearest(r, xs);
        if (hit && v > l + 1) {
            r = v;
            guides.append({'v', v});
        }
    }
    if (at) {
        auto [v, hit] = nearest(t, ys);
        if (hit && v < b - 1) {
            t = v;
            guides.append({'h', v});
        }
    }
    if (ab) {
        auto [v, hit] = nearest(b, ys);
        if (hit && v > t + 1) {
            b = v;
            guides.append({'h', v});
        }
    }
    m_guides = guides;
    return QRectF(QPointF(l, t), QPointF(r, b));
}

QRectF SelectionController::snapMove(const QRectF& rect) {
    // 整体拖动：左/中/右（上/中/下）里挑最近的一条，整块平移过去，免得变形
    if (!config::getBool("Capture/snap_guides")) {
        m_guides.clear();
        return rect;
    }
    auto [xs, ys] = guideLines();
    QVector<std::pair<QChar, double>> guides;
    const auto bestOff = [](const QList<double>& values, const QVector<double>& cands)
        -> std::pair<double, std::optional<double>> {
        double off = 0.0, dist = kGuideTol + 1.0;
        std::optional<double> line;
        for (double v : values) {
            for (double c : cands) {
                if (qAbs(c - v) < dist) {
                    off = c - v;
                    line = c;
                    dist = qAbs(c - v);
                }
            }
        }
        if (dist <= kGuideTol)
            return {off, line};
        return {0.0, std::nullopt};
    };
    const auto [dx, lx] =
        bestOff({rect.left(), rect.center().x(), rect.right()}, xs);
    const auto [dy, ly] =
        bestOff({rect.top(), rect.center().y(), rect.bottom()}, ys);
    if (lx)
        guides.append({'v', *lx});
    if (ly)
        guides.append({'h', *ly});
    m_guides = guides;
    QRectF out(rect);
    out.translate(dx, dy);
    return out;
}

// ---- 命中测试 ----

int SelectionController::hitTest(const QPointF& pt) const {
    // 返回手柄索引（0-7）；-1 = 选区内；-2 = 选区外。
    // 已选工具时选区内部一律让给画图；手柄只在贴边/贴角的外侧生效。
    const QRectF& r = m_rect;
    const bool drawing = !m_annot->tool().isEmpty() &&
                         r.adjusted(2, 2, -2, -2).contains(pt);
    if (!drawing) {
        for (int i = 0; i < kHandleCount; ++i) {
            if (qAbs(handlePoint(r, i).x() - pt.x()) +
                    qAbs(handlePoint(r, i).y() - pt.y()) <=
                kHandleHitRadius)
                return i;
        }
    }
    return m_rect.contains(pt) ? -1 : -2;
}

// ---- 鼠标 ----

void SelectionController::onPress(const QPointF& pos) {
    const auto tPress = clock::now();
    m_cursor = pos;
    m_animActive = false;  // 框选/拖拽开始：高亮动画直接停，别把过渡带进新状态
    if (m_animTimer)
        m_animTimer->stop();
    if (m_state == "hover") {
        // 交互约定：按下先进入拖拽候选（自由框选起点），不立即吸附。
        // 松手时若几乎没拖动：落在吸附窗口内 -> 单击吸附该窗口；否则回到 hover。
        m_state = "creating";
        m_press = pos;
        m_rect = QRectF(pos, pos);
        m_pressSuggest = m_suggest.isValid() ? QRectF(m_suggest) : QRectF();
    } else if (m_state == "selected") {
        const int hit = hitTest(pos);
        if (hit >= 0) {
            m_state = "resizing";
            m_resizeIdx = hit;
            m_focusIdx = hit;
            m_resizeFix = fixedCorner(hit);
        } else if (hit == -1 && !m_annot->tool().isEmpty()) {
            m_annot->startDrawing(pos);
        } else if (hit == -1 && m_annot->tryGrabShape(pos)) {
            // 「选择/编辑」工具（空工具同义）点中已画图形：选中并可拖动/改形状
        } else if (hit == -1) {
            m_state = "moving";
            m_moveOffset = QPointF(pos.x() - m_rect.left(), pos.y() - m_rect.top());
        } else {
            // 选区外：自动扩选 —— 把鼠标所在方位的边/角拉向点击处（8 方位）
            m_ext = {pos.x() < m_rect.left(), pos.x() > m_rect.right(),
                     pos.y() < m_rect.top(), pos.y() > m_rect.bottom()};
            m_extOrig = QRectF(m_rect);
            applyExtend(pos);
            m_state = "extending";
        }
    }
    updateMagnifier();
    updateAll();
    // 慢路径探针：按下处理 >20ms 记一笔
    log::info("zpin.sel", QStringLiteral("onPress %1ms（state=%2）")
                              .arg(std::chrono::duration<double, std::milli>(clock::now() -
                                                                             tPress)
                                       .count(),
                                   0, 'f', 1)
                              .arg(m_state));
}

namespace {
// onMove 有多个提前返回，用 RAII 兜住所有出口：单次处理 >25ms 记一笔
class MoveProbe {
public:
    MoveProbe() : m_t0(clock::now()) {}
    ~MoveProbe() {
        const double ms =
            std::chrono::duration<double, std::milli>(clock::now() - m_t0).count();
        if (ms > 25)
            log::info("zpin.sel", QStringLiteral("onMove %1ms").arg(ms, 0, 'f', 1));
    }

private:
    clock::time_point m_t0;
};
}  // namespace

void SelectionController::onMove(const QPointF& pos) {
    MoveProbe moveProbe;
    if (pos == m_cursor)
        return;
    m_cursor = pos;
    m_magFollowHandle = -1;  // 鼠标一动，放大镜回到跟光标
    // moving/resizing/extending 三个状态里 m_rect 每帧都在变，工具条必须跟着走：
    // 它挂在选区下方（放不下就翻到上方），原先只在 onRelease 里 positionToolbar
    // 一次，于是拖动过程中工具条钉在原地不动、松手才「跳」到新位置——用户实测
    // 反馈「框拖动时工具条不实时跟随」。positionToolbar() 内部对工具条为空的
    // 情况早退（选区阶段还没建工具条），所以这里无条件调是安全的。
    if (m_state == "hover") {
        setSuggest(snapTarget());  // 立即用缓存出图（元素/内容块/整窗）
        requestElement();          // 元素结果由工作线程异步补上
    } else if (m_state == "creating") {
        const QRectF free = QRectF(m_press, pos).normalized();
        m_rect = snapEdges(free, {pos.x() < m_press.x(), pos.x() > m_press.x(),
                                  pos.y() < m_press.y(), pos.y() > m_press.y()});
    } else if (m_state == "moving") {
        const QPointF old = m_rect.topLeft();
        m_rect = snapMove(QRectF(pos - m_moveOffset, m_rect.size()));
        clampRect();
        const QPointF d = m_rect.topLeft() - old;
        // 框内已画的标注锚定在选区上，随框一起平移
        if (d != QPointF(0, 0)) {
            m_annot->translateFollowing(d.x() * m_dpr, d.y() * m_dpr);
            m_annot->positionToolbar();   // 工具条实时跟随（见下面 resizing/extending 的说明）
        }
    } else if (m_state == "resizing") {
        const QRectF before = m_rect;
        applyResize(pos);
        m_rect = snapEdges(m_rect, resizeEdges(m_resizeIdx));
        if (m_rect != before)
            m_annot->positionToolbar();
    } else if (m_state == "extending") {
        const QRectF before = m_rect;
        applyExtend(pos);
        m_rect = snapEdges(m_rect, m_ext);
        if (m_rect != before)
            m_annot->positionToolbar();
    } else if (m_state == "drawing" &&
               (m_annot->activeShape() || m_annot->isGrabbing())) {
        m_annot->updateDrawing(pos);
    } else if (m_state == "erasing" && m_annot->engine()) {
        m_annot->eraseAt(pos);
    }
    updateMagnifier();
    updateCursorShape(pos);
    updateAll();
}

void SelectionController::onRelease(const QPointF& pos) {
    m_guides.clear();
    if (m_state == "creating") {
        if (m_rect.width() < kMinSize || m_rect.height() < kMinSize) {
            // 单击未拖拽：落在吸附窗口上 -> 吸附该窗口；否则取消
            const QRectF& s = m_pressSuggest;
            if (s.isValid() && s.width() >= kMinSize && s.height() >= kMinSize &&
                s.adjusted(-kClickSnapPad, -kClickSnapPad, kClickSnapPad,
                           kClickSnapPad)
                    .contains(m_press)) {
                m_rect = QRectF(s);
                m_state = "selected";
                m_suggest = QRectF();
                m_annot->ensure();
            } else {
                m_state = "hover";
                m_rect = QRectF();
            }
        } else {
            m_state = "selected";
            m_annot->ensure();
        }
        // 选区可能落在与热键按下时不同的屏：混合 DPI 下标注线宽/字号按它算
        if (m_rect.isValid())
            m_dpr = m_map.dprOf(m_rect);
    } else if (m_state == "moving" || m_state == "resizing") {
        m_state = "selected";
        m_resizeIdx = -1;
        m_dpr = m_map.dprOf(m_rect);  // 选区可能落在别的屏，混合 DPI 位移/绘制用它
        m_annot->positionToolbar();
    } else if (m_state == "extending") {
        m_state = "selected";
        if (m_rect.width() < kMinSize || m_rect.height() < kMinSize || !m_rect.isValid())
            m_rect = m_extOrig;  // 扩过头/反向拖到极小：回滚到按下前
        m_dpr = m_map.dprOf(m_rect);
        m_annot->positionToolbar();
    } else if (m_state == "drawing") {
        m_annot->finishDrawing(pos);
    } else if (m_state == "erasing") {
        m_state = "selected";
        m_annot->finishErasing();
    }
    updateMagnifier();
    updateCursorShape(pos);
    updateAll();
}

void SelectionController::onRightPress() {
    cancel();
}

QPointF SelectionController::fixedCorner(int idx) const {
    const QRectF& r = m_rect;
    switch (idx) {
        case TL: return QPointF(r.right(), r.bottom());
        case TR: return QPointF(r.left(), r.bottom());
        case BR: return QPointF(r.left(), r.top());
        case BL: return QPointF(r.right(), r.top());
        case TM: return QPointF(r.center().x(), r.bottom());
        case BM: return QPointF(r.center().x(), r.top());
        case MR: return QPointF(r.left(), r.center().y());
        default: return QPointF(r.right(), r.center().y());  // ML
    }
}

void SelectionController::applyResize(const QPointF& pos) {
    const int idx = m_resizeIdx;
    const QPointF& f = m_resizeFix;
    if (idx == TL || idx == TR || idx == BR || idx == BL) {
        m_rect = QRectF(f, pos).normalized();
    } else if (idx == TM || idx == BM) {
        const double anchorX = f.x();
        const double top = qMin(pos.y(), f.y());
        const double h = qAbs(pos.y() - f.y());
        m_rect = QRectF(anchorX - m_rect.width() / 2, top, m_rect.width(), h);
    } else {  // ML / MR
        const double anchorY = f.y();
        const double left = qMin(pos.x(), f.x());
        const double w = qAbs(pos.x() - f.x());
        m_rect = QRectF(left, anchorY - m_rect.height() / 2, w, m_rect.height());
    }
    clampRect();
}

void SelectionController::applyExtend(const QPointF& pos) {
    // 自动扩选：把按下方位的边/角实时拉向鼠标位置（其余边保持不动）
    const auto [L, R, T, B] = m_ext;
    QRectF& r = m_rect;
    if (L)
        r.setLeft(qMin(pos.x(), r.right() - 1));
    if (R)
        r.setRight(qMax(pos.x(), r.left() + 1));
    if (T)
        r.setTop(qMin(pos.y(), r.bottom() - 1));
    if (B)
        r.setBottom(qMax(pos.y(), r.top() + 1));
    // 夹在虚拟桌面内，避免扩出屏外
    const QRectF b(m_bounds);
    if (L)
        r.setLeft(qMax(b.left(), r.left()));
    if (R)
        r.setRight(qMin(b.right(), r.right()));
    if (T)
        r.setTop(qMax(b.top(), r.top()));
    if (B)
        r.setBottom(qMin(b.bottom(), r.bottom()));
}

void SelectionController::clampRect() {
    const QRectF b(m_bounds);
    m_rect.moveLeft(qMax(b.left(), qMin(m_rect.left(), b.right() - m_rect.width())));
    m_rect.moveTop(qMax(b.top(), qMin(m_rect.top(), b.bottom() - m_rect.height())));
}

void SelectionController::updateCursorShape(const QPointF& pos) {
    OverlayWindow* w = windowAt(pos);
    if (!w)
        return;
    if (m_state == "extending") {
        // 扩选方向 -> 对应缩放光标（单边/对角）
        const auto [L, R, T, B] = m_ext;
        const bool horiz = L || R, vert = T || B;
        if (horiz && vert) {
            w->setCursor(((L && T) || (R && B)) ? Qt::CursorShape::SizeFDiagCursor
                                                : Qt::CursorShape::SizeBDiagCursor);
        } else if (horiz) {
            w->setCursor(Qt::CursorShape::SizeHorCursor);
        } else {
            w->setCursor(Qt::CursorShape::SizeVerCursor);
        }
        return;
    }
    if (m_state == "hover" || m_state == "creating" || m_state == "drawing" ||
        m_state == "erasing") {
        w->setCursor(Qt::CursorShape::CrossCursor);
        return;
    }
    const int hit = hitTest(pos);
    switch (hit) {
        case TL:
        case BR:
            w->setCursor(Qt::CursorShape::SizeFDiagCursor);
            break;
        case TR:
        case BL:
            w->setCursor(Qt::CursorShape::SizeBDiagCursor);
            break;
        case TM:
        case BM:
            w->setCursor(Qt::CursorShape::SizeVerCursor);
            break;
        case ML:
        case MR:
            w->setCursor(Qt::CursorShape::SizeHorCursor);
            break;
        case -1:
            // 选区内：未选工具（含「选择/编辑」，二者归一）= 拖动整体选区/抓图形；
            // 已选绘制工具 = 精确定位绘制起点
            w->setCursor(m_annot->tool().isEmpty() ? Qt::CursorShape::SizeAllCursor
                                                   : Qt::CursorShape::CrossCursor);
            break;
        default:
            // 选区外：点按即开新框，用十字提示可框选，光标保持可见
            w->setCursor(Qt::CursorShape::CrossCursor);
            break;
    }
}

// ---- 提示 ----

void SelectionController::showToast(const QString& text) {
    // 选区内顶部显示一条 1.6 秒的短提示（取色结果等），随后自动消失
    m_toast = text;
    m_toastUntil = clock::now() + std::chrono::milliseconds(1600);
    QTimer::singleShot(1700, this, &SelectionController::clearToast);
    updateAll();
}

void SelectionController::clearToast() {
    if (!m_toast.isEmpty() && clock::now() >= m_toastUntil) {
        m_toast.clear();
        updateAll();
    }
}

void SelectionController::updateMagnifier() {
    // 按交互时机刷新放大镜：调整边角时自动出现，Alt 常驻召唤，其余隐藏
    if (m_magnifier.isNull())
        return;
    // 取样点：鼠标拖动跟光标；键盘微调跟正在调的角/边——否则看不出边界在动
    QPointF anchor = m_cursor;
    if (m_magFollowHandle >= 0)
        anchor = handlePoint(m_rect, m_magFollowHandle);
    if (m_state == "resizing" || m_state == "extending" || m_magPinned ||
        m_magFollowHandle >= 0)
        m_magnifier->updateAt(anchor);
    else
        m_magnifier->hide();
}

QRect SelectionController::magnifierMaskRect() const {
    if (m_rect.isValid() && m_rect.width() >= 1 && m_rect.height() >= 1)
        return m_map.rectOf(m_rect);
    if (m_suggest.isValid())
        return m_map.rectOf(m_suggest);
    return QRect();
}

// ---- 键盘 ----

bool SelectionController::onKey(QKeyEvent* ev) {
    const int key = ev->key();
    const Qt::KeyboardModifiers mod = ev->modifiers();
    if (key == Qt::Key_Escape) {
        cancel();
        return true;
    }
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        if (m_state == "hover" && m_suggest.isValid()) {
            // 还没框选：Enter 直接截取当前吸附到的元素/窗口
            m_rect = QRectF(m_suggest);
            m_state = "selected";
        }
        confirm();
        return true;
    }
    if (key == Qt::Key_C && (mod & Qt::ControlModifier)) {
        confirm();
        return true;
    }
    if (key == Qt::Key_S && (mod & Qt::ControlModifier)) {
        confirm("save");
        return true;
    }
    // 光秃秃按一下 Alt = 召唤/收起放大镜。带 Ctrl/Shift/Win 的 Alt 组合另有用途
    // （Alt+F4 / Alt+Shift+Tab…），不能被吞掉；auto-repeat 也不能反复翻转。
    if (key == Qt::Key_Alt && mod == Qt::AltModifier && !ev->isAutoRepeat()) {
        m_magPinned = !m_magPinned;
        updateMagnifier();
        return true;
    }
    if (key == Qt::Key_C && mod == Qt::NoModifier) {
        // 取色：截图模式内随时按 C 复制光标处色值（不依赖放大镜可见）
        const QPointF cp = m_map.physOf(m_cursor);
        const QColor c = m_base.pixelColor(
            qBound(0, int(cp.x()), m_base.width() - 1),
            qBound(0, int(cp.y()), m_base.height() - 1));
        const QString hexc = c.name(QColor::HexRgb).toUpper();
        QGuiApplication::clipboard()->setText(hexc);
        showToast(QString("已复制 %1").arg(hexc));
        return true;
    }
    if (m_state == "hover" && key == Qt::Key_Space) {
        // 关掉 / 恢复 hover 吸附建议（拖拽一直是自由矩形，这里无需管）
        m_snapFree = !m_snapFree;
        m_suggest = snapTarget();
        updateAll();
        return true;
    }
    if (m_state == "hover" && key == Qt::Key_Tab) {
        // 检测层级轮换：自动（界面元素 → 内容块 → 整窗）-> 仅窗口 -> 仅元素
        m_detect = m_detect == "auto" ? "win" : (m_detect == "win" ? "el" : "auto");
        m_suggest = snapTarget();
        updateAll();
        return true;
    }
    if (m_annot->handleKey(ev))
        return true;
    if (m_state != "selected" || !m_rect.isValid())
        return false;
    const int step = mod & Qt::ShiftModifier ? 10 : 1;
    if (key == Qt::Key_Tab) {
        // Tab 在「无焦点 -> 8 个手柄 -> 回到无焦点」之间循环
        m_focusIdx = m_focusIdx < 0 ? 0 : (m_focusIdx + 1) % (kHandleCount + 1);
        if (m_focusIdx == kHandleCount)
            m_focusIdx = -1;
        updateAll();
        return true;
    }
    int dx = 0, dy = 0;
    switch (key) {
        case Qt::Key_Left: dx = -step; break;
        case Qt::Key_Right: dx = step; break;
        case Qt::Key_Up: dy = -step; break;
        case Qt::Key_Down: dy = step; break;
        default: return false;
    }
    if (mod & Qt::ControlModifier) {
        // 扩大选区：沿按键方向把对应边向外推（Ctrl+方向）
        QRectF grow = m_rect;
        if (dx < 0)
            grow.setLeft(grow.left() - step);
        else if (dx > 0)
            grow.setRight(grow.right() + step);
        else if (dy < 0)
            grow.setTop(grow.top() - step);
        else
            grow.setBottom(grow.bottom() + step);
        if (grow.width() >= kMinSize && grow.height() >= kMinSize) {
            m_rect = grow;
            // 放大镜跟着刚被推出去的那条边（手柄枚举里 ML/MR/TM/BM 就是四边中点）
            m_magFollowHandle = dx < 0 ? ML : dx > 0 ? MR : dy < 0 ? TM : BM;
            clampRect();
            updateMagnifier();
            updateAll();
        }
        return true;
    }
    if (m_focusIdx >= 0) {
        m_resizeIdx = m_focusIdx;
        m_resizeFix = fixedCorner(m_focusIdx);
        const QPointF hp = handlePoint(m_rect, m_focusIdx);
        applyResize(QPointF(hp.x() + dx, hp.y() + dy));
        m_resizeIdx = -1;
        m_magFollowHandle = m_focusIdx;  // 放大镜改看正在调的角/边
    } else {
        m_magFollowHandle = -1;
        const QPointF old = m_rect.topLeft();
        m_rect.translate(dx, dy);
        clampRect();
        const QPointF d = m_rect.topLeft() - old;
        // 用钳制后的实际位移：贴边时框没动，标注也不能动
        if (d != QPointF(0, 0))
            m_annot->translateFollowing(d.x() * m_dpr, d.y() * m_dpr);
    }
    // 键盘微调同样要刷新放大镜：跟随手柄的位置与放大镜内的遮罩都变了
    // （鼠标路径在 onMove 里刷新，键盘路径只有这里）
    updateMagnifier();
    updateAll();
    return true;
}

}  // namespace zpin
