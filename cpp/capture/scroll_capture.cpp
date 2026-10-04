#include "scroll_capture.hpp"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QPixmap>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QThread>
#include <QThreadPool>
#include <QVBoxLayout>
#include <QtGlobal>

#include "config.hpp"
#include "logging.hpp"
#include "rcore.hpp"
#include "win32util.hpp"

namespace zpin {
namespace scroll {

namespace {

constexpr int kMaxFrames = 400;       // 入画帧数上限（只数真有新内容的帧）
constexpr int kMaxPixels = 20000;     // 长图高度上限（物理像素）
constexpr int kVkEscape = 0x1B;
constexpr int kVkSpace = 0x20;        // 空格 = 完成（用已捕获内容出图）

// 轮询节奏：外层循环只做「查退出条件 + 查滚轮计数」，kPollMs 是按键响应粒度；
// 抓帧不再按时间驱动，而是「滚轮停稳后抓一帧」（见 run() 的滚轮驱动注释）。
constexpr int kPollMs = 60;
constexpr int kSettleMs = 150;   // 连续滚轮的停稳窗口：最后一格滚轮后不再有新滚轮
                                 // 才抓帧——Chromium 平滑滚动的惯性还能再走一两百毫秒，
                                 // 抓早了拼接器对不上就丢内容
constexpr int kStableGapMs = 140;    // 验稳两次抓帧的间隔
constexpr int kStableRetries = 6;    // 验稳重试上限（常驻动画页面到顶后按最后帧入画）
constexpr double kStableDiffFrac = 0.03;  // 两帧采样差异占比低于它 = 已稳定。
                                          // 容忍计时卡/相对时间戳这类小块文字刷新
                                          // （约占 1%），拦住消息淡入/滑入的中间态
                                          //（大面积半透明重影，占比 10% 起）

// 会话内累计的滚轮格数：ScreenMask 的 nativeEvent 加，捕获线程减着用。
// 只在同进程内共享，relaxed 够。
std::atomic<quint64> g_wheelTicks{0};

// 两帧的采样差异占比：隔 2 行、隔 4 像素抽点（RGB32）。UI 是确定性渲染，
// 静止内容重抓逐字节一致，差异只来自真正在动的元素（计时卡 ~1%、入场动画
// 的半透明叠影 10%+）——3% 的门槛分开这两类。
double sampledDiffFrac(const QImage& a, const QImage& b) {
    if (a.size() != b.size() || a.isNull() || b.isNull())
        return 1.0;
    const qsizetype w = a.width(), h = a.height();
    qsizetype diffs = 0, samples = 0;
    for (qsizetype y = 0; y < h; y += 2) {
        const QRgb* pa = reinterpret_cast<const QRgb*>(a.constScanLine(int(y)));
        const QRgb* pb = reinterpret_cast<const QRgb*>(b.constScanLine(int(y)));
        for (qsizetype x = 0; x < w; x += 4) {
            ++samples;
            if (pa[x] != pb[x])
                ++diffs;
        }
    }
    return samples ? double(diffs) / double(samples) : 1.0;
}

// 长截图专用的私有线程池（单线程：同一时刻只有一场捕获）。
// 不走全局池的理由同 update.cpp：手动模式下这场捕获会一直跑到你点「完成」
// （可能几分钟），占着全局池的槽会让 OCR、历史压缩排在它后面。
// 故意 new 出来不释放：~QThreadPool 会 waitForDone()，
// 静态析构时若正有一场捕获在跑，进程退出会被它卡住。
QThreadPool& pool() {
    static QThreadPool* g = [] {
        auto* p = new QThreadPool;
        p->setMaxThreadCount(1);
        p->setExpiryTimeout(30'000);
        return p;
    }();
    return *g;
}

}  // namespace

// ---- ScreenMask：整屏压暗 + 捕获区挖空 + 洞外描边 ----

ScreenMask::ScreenMask(QScreen* scr, const QRectF& holeLogical) {
    const QRect geo = scr->geometry();
    setGeometry(geo);
    m_hole = QRectF(holeLogical.left() - geo.left(), holeLogical.top() - geo.top(),
                    holeLogical.width(), holeLogical.height());
    setWindowFlags(Qt::WindowType::FramelessWindowHint |
                   Qt::WindowType::WindowStaysOnTopHint | Qt::WindowType::Tool);
    setAttribute(Qt::WidgetAttribute::WA_ShowWithoutActivating);
    setAttribute(Qt::WidgetAttribute::WA_TranslucentBackground);
}

void ScreenMask::showEvent(QShowEvent*) {
    // WA_TransparentForMouseEvents 的 HTTRANSPARENT 只在同进程窗口间透传；
    // 捕获区下面的目标窗口在别的进程里，用户手动滚动的滚轮要真正落到它头上，
    // 必须用原生 WS_EX_TRANSPARENT（与 snap_worker 的遮罩穿透同一套机制）。
    if (!win32::setClickThrough(reinterpret_cast<win32::Hwnd>(winId()), true))
        log::warn("zpin.scroll", QStringLiteral("遮罩窗设置鼠标穿透失败，下层窗口收不到滚动"));
}

bool ScreenMask::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    // 只数「向后拉」的滚轮（delta < 0 = 视口向文档尾走）。向前推 = 往回看，
    // 那种帧的内容在画布里早已存在，而对齐搜索范围只覆盖向下重叠——拿去
    // 拼接轻则重复条带、重则触发重排回填把画布换错。往回核对内容不参与拼接。
    if (eventType == "windows_generic_MSG" && win32::rawWheelDelta(message) < 0) {
        g_wheelTicks.fetch_add(1, std::memory_order_relaxed);
        *result = 0;
        return true;   // 原始输入消息 Qt 用不上
    }
    return QWidget::nativeEvent(eventType, message, result);
}

void ScreenMask::paintEvent(QPaintEvent*) {
    QPainter p(this);
    QPainterPath full;
    full.addRect(QRectF(rect()));
    QPainterPath hole;
    hole.addRect(m_hole);
    p.fillPath(full.subtracted(hole), QColor(0, 0, 0, 110));
    // 洞外描边：路径整体外移 1.5px（线宽 2 → 覆盖洞外 0.5~2.5px），永不进入捕获区
    if (m_hole.width() > 0 && m_hole.height() > 0) {
        p.setPen(QPen(QColor(config::getStr("Interface/theme_color")), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(m_hole.adjusted(-1.5, -1.5, 1.5, 1.5));
    }
}

// ---- Progress：贴选区的实时预览面板 ----

Progress::Progress(QWidget* parent) : QWidget(parent) {
    setWindowFlags(Qt::WindowType::FramelessWindowHint |
                   Qt::WindowType::WindowStaysOnTopHint | Qt::WindowType::Tool);
    setAttribute(Qt::WidgetAttribute::WA_ShowWithoutActivating);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(8);

    m_status = new QLabel(QStringLiteral("长截图"), this);
    m_status->setAlignment(Qt::AlignmentFlag::AlignCenter);
    m_status->setStyleSheet(QStringLiteral("color:#E8E8E8; font-size:12px;"));

    m_thumb = new QLabel(this);
    m_thumb->setFixedSize(200, 320);
    m_thumb->setAlignment(Qt::AlignmentFlag::AlignCenter);
    m_thumb->setStyleSheet(QStringLiteral("background: rgba(0,0,0,120); color:#666;"));

    auto* btns = new QHBoxLayout();
    btns->setSpacing(8);
    auto* finish = new QPushButton(QStringLiteral("完成"), this);
    finish->setStyleSheet(QStringLiteral(
        "QPushButton { background:#1E78D7; color:#FFFFFF; border:none;"
        " border-radius:6px; padding:6px 0; font-size:12px; }"
        "QPushButton:hover { background:#2A86E8; }"));
    auto* cancel = new QPushButton(QStringLiteral("取消"), this);
    cancel->setStyleSheet(QStringLiteral(
        "QPushButton { background:rgba(255,255,255,28); color:#E8E8E8; border:none;"
        " border-radius:6px; padding:6px 0; font-size:12px; }"
        "QPushButton:hover { background:rgba(255,255,255,44); }"));
    btns->addWidget(finish);
    btns->addWidget(cancel);

    auto* hint = new QLabel(QStringLiteral("用滚轮滚动页面 · 空格完成 · Esc 取消"), this);
    hint->setAlignment(Qt::AlignmentFlag::AlignCenter);
    hint->setStyleSheet(QStringLiteral("color:#888; font-size:11px;"));

    lay->addWidget(m_status);
    lay->addWidget(m_thumb, 1);
    lay->addLayout(btns);
    lay->addWidget(hint);

    connect(finish, &QPushButton::clicked, this, &Progress::finishClicked);
    connect(cancel, &QPushButton::clicked, this, &Progress::cancelClicked);

    setStyleSheet(QStringLiteral("background: rgba(23,23,28,242); border-radius: 8px;"));
    setFixedSize(224, 470);
}

void Progress::onUpdate(int frames, int height, bool idle, int missed) {
    // idle = 画面停着。手动模式下这不代表到底，所以提示是「继续滚或点完成」，
    // 而不是老那样默默收工截断。
    // missed > 0 必须说出来：那几帧是真丢了的中间内容，不说就是「长图少了一截
    // 而用户不知道为什么」——早先自动模式靠控制器把步长压在对齐范围内规避，
    // 手动模式压不住，只能靠提示 + 加快探测。
    QString hint = idle ? QStringLiteral("没在动：继续滚动，或点「完成」出图")
                        : QStringLiteral("继续滚动，空格完成");
    if (missed > 0)
        hint += QStringLiteral("（%1 次滚太快没接上）").arg(missed);
    m_status->setText(QStringLiteral("长截图 · 第 %1 帧 · %2 px\n%3")
                          .arg(frames)
                          .arg(height)
                          .arg(hint));
    show();
    raise();
    update();
}

void Progress::onPreview(const QImage& thumb) {
    if (thumb.isNull())
        return;
    m_thumb->setPixmap(
        QPixmap::fromImage(thumb.scaled(m_thumb->size(), Qt::AspectRatioMode::KeepAspectRatio,
                                        Qt::TransformationMode::SmoothTransformation)));
}

void Progress::keyPressEvent(QKeyEvent* ev) {
    // 面板拿到焦点时的兜底；全局的 Esc/空格由 worker 的 GetAsyncKeyState 兜住
    if (ev->key() == Qt::Key_Escape) {
        emit cancelClicked();
        return;
    }
    if (ev->key() == Qt::Key_Space) {
        emit finishClicked();
        return;
    }
    QWidget::keyPressEvent(ev);
}

Capture::Capture(const QImage& first, const QRect& regionAbs, const QRectF& regionLogical,
                 double dpr, QObject* parent)
    : QObject(parent),
      m_first(first),
      m_region(regionAbs),
      m_regionLogical(regionLogical),
      m_dpr(dpr > 0 ? dpr : 1.0) {}

void Capture::start() {
    // 预览面板与屏幕遮罩都在主线程创建（顶层窗）；worker 的 progress/
    // previewReady 信号经队列刷新它们，finished 时一并自毁。
    auto* progress = new Progress();
    connect(this, &Capture::progress, progress, &Progress::onUpdate);
    connect(this, &Capture::previewReady, progress, &Progress::onPreview);
    connect(progress, &Progress::finishClicked, this, [this] { finishNow(); });
    connect(progress, &Progress::cancelClicked, this, [this] { stop(); });
    connect(this, &Capture::finished, progress, &QObject::deleteLater);
    // Capture 一次性：finished（自然结束或 stop() 打断）后自毁。外部对一场
    // 还在线程池里跑 run() 的捕获 deleteLater 是 use-after-free，所以销毁
    // 只能由它自己收尾。
    connect(this, &Capture::finished, this, &QObject::deleteLater);

    // 屏幕遮罩：整屏压暗、捕获区挖空（洞比捕获区外扩 1px，边界像素不被
    // 遮罩的边缘反锯齿沾到），洞外描边。全部在捕获区之外，BitBlt 选区内部
    // 不受污染；挖空处透出真实窗口，用户能看到内容在滚。
    const QRectF hole = m_regionLogical.adjusted(-1.0, -1.0, 1.0, 1.0);
    const QList<QScreen*> screens = QGuiApplication::screens();
    int maskCount = 0;
    bool wheelRegistered = false;
    for (QScreen* scr : screens) {
        auto* mask = new ScreenMask(scr, hole);
        connect(this, &Capture::finished, mask, &QObject::deleteLater);
        mask->show();
        // 滚轮检测只需注册一块遮罩窗：INPUTSINK 是全局投递，与窗口在哪块屏
        // 无关。winId() 要等 show() 之后才有效。
        if (!wheelRegistered) {
            wheelRegistered = win32::registerRawWheelSink(
                reinterpret_cast<win32::Hwnd>(mask->winId()));
            if (!wheelRegistered)
                log::warn("zpin.scroll", QStringLiteral("滚轮 Raw Input 注册失败，"
                                                        "只能靠画面变化探测滚动"));
        }
        maskCount++;
    }

    // 面板贴选区：先右后左，都放不下贴屏幕右下角
    const QRect r = m_regionLogical.toRect();
    QScreen* scr = QGuiApplication::screenAt(r.center());
    if (!scr)
        scr = QGuiApplication::primaryScreen();
    if (scr) {
        const QRect avail = scr->availableGeometry();
        const int pw = progress->width(), ph = progress->height();
        QPoint pos(r.right() + 12, r.top());
        if (pos.x() + pw > avail.right() - 4)
            pos.setX(r.left() - 12 - pw);
        if (pos.x() < avail.left() + 4)
            pos = QPoint(avail.right() - pw - 12, avail.bottom() - ph - 16);
        pos.setY(qBound(avail.top() + 4, pos.y(),
                        qMax(avail.top() + 4, avail.bottom() - ph - 4)));
        progress->move(pos);
    }
    progress->show();
    progress->raise();
    log::info("zpin.scroll", QStringLiteral("捕获 UI 就绪：遮罩 %1 块，面板 %2×%3 @ (%4,%5) 可见=%6")
                                .arg(maskCount)
                                .arg(progress->width())
                                .arg(progress->height())
                                .arg(progress->x())
                                .arg(progress->y())
                                .arg(progress->isVisible() ? 1 : 0));
    // 句柄要在 show() 之后取：窗口未 show 时 winId() 可能还没创建原生窗口，
    // 拿到的 HWND 是 0，排除就失效了。
    m_progressHwnd.store(reinterpret_cast<void*>(progress->winId()));
    pool().start([this] { run(); });
}

void Capture::run() {
    rcore::ScrollStitcher stitcher(m_region.width(), m_region.height(), kMaxPixels);
    const QImage first = m_first.convertToFormat(QImage::Format_RGB32);
    stitcher.push(first);
    int frames = 1;         // 入画的帧数（只数真带来新内容的那些）
    int missed = 0;         // 「动了但没对上」的次数 = 丢掉的中间内容次数
    int lastSticky = 0;
    bool done = false;
    bool cancelled = false;
    bool idle = false;      // 面板提示：画面是否停着
    const char* stopReason = "finish";

    // 抓帧前把自己这块进度窗从屏幕捕获里摘出去。它是 224x470 的常显顶层窗、
    // WindowStaysOnTopHint，贴在主屏右下角——用户选区一旦压到那儿，它就会
    // **逐帧**被拍进长图，在画布上留一个永远遮住内容的洞，拼接器还会被这个
    // 静止大块干扰得对不上。Snow Shot 同样把自绘浮窗的 hwnd 传给采集源排除
    // （WDA_EXCLUDEFROMCAPTURE）。
    auto* hwnd = static_cast<win32::Hwnd>(m_progressHwnd.load());
    if (hwnd && !win32::excludeFromCapture(hwnd)) {
        log::warn("zpin.scroll", QStringLiteral("进度窗无法从抓屏中排除（系统不支持 "
                                                 "WDA_EXCLUDEFROMCAPTURE），它可能被拍进长图"));
    }
    emit progress(frames, stitcher.canvasHeight(), idle, missed);

    // 滚轮驱动：只在「用户真的滚了滚轮」之后才抓帧入画。悬停预览（B 站视频卡
    // 等）、动图、轮播都会让画面自己在那里动——旧轮询模式把这种帧也推给拼接
    // 器，轻则记一堆「没接上」，重则把预览帧错接进长图。静止期的任何画面变化
    // 现在一概不入画；滚轮经 Raw Input 全局接收（遮罩窗 nativeEvent 计数），
    // 连续滚动期间不断顺延停稳窗口，最后一格滚轮后再等 kSettleMs 才抓。
    const auto stopReasonOf = [&]() -> const char* {
        if (m_stop)
            return "stopped";
        if (win32::asyncKeyDown(kVkEscape))
            return "esc";
        if (m_finish)
            return "finish";
        if (win32::asyncKeyDown(kVkSpace))
            return "space";
        return nullptr;
    };
    quint64 seen = g_wheelTicks.load(std::memory_order_relaxed);
    while (!done && frames < kMaxFrames && !m_stop) {
        QThread::msleep(kPollMs);
        if (const char* r = stopReasonOf()) {
            cancelled = QLatin1String(r) == QLatin1String("stopped") ||
                        QLatin1String(r) == QLatin1String("esc");
            done = !cancelled;
            stopReason = r;
            break;
        }
        const quint64 now = g_wheelTicks.load(std::memory_order_relaxed);
        if (now == seen)
            continue;   // 用户没滚：静止期的画面变化一概不入画
        seen = now;
        // —— 停稳 + 验稳 ——
        // 停稳：连续滚轮间顺延，最后一格后再等 kSettleMs。验稳：目标页面常有
        // 滚动入场动画（消息淡入/卡片滑入，中间态是两条文字半透明叠影）与秒级
        // 刷新的计时卡——把动画中间态烙进长图，对齐再准也是糊的。连续两抓
        // 差异占比够低才入画；常驻动画页面试到上限后按最后一帧入画（行为
        // 退回旧版）。重试途中滚轮又动了就重新停稳。
        QImage frame;
        QImage prevGrab;
        int tries = 0;
        bool grabfail = false;
        const char* stopR = nullptr;
        while (true) {
            stopR = stopReasonOf();
            if (stopR)
                break;
            const quint64 t = g_wheelTicks.load(std::memory_order_relaxed);
            if (t != seen) {   // 还在滚：重新停稳
                seen = t;
                prevGrab = QImage();
                tries = 0;
                QThread::msleep(kSettleMs);
                continue;
            }
            QImage cand = rcore::captureBgraPlain(m_region.x(), m_region.y(),
                                                  m_region.width(), m_region.height())
                              .convertToFormat(QImage::Format_RGB32);
            if (cand.isNull()) {
                grabfail = true;
                break;
            }
            if (sampledDiffFrac(prevGrab, cand) < kStableDiffFrac) {
                frame = std::move(cand);   // 连续两抓差异可忽略 = 已稳定
                break;
            }
            prevGrab = std::move(cand);
            if (++tries >= kStableRetries) {
                frame = std::move(prevGrab);   // 常驻动画兜底：按最后一帧入画
                break;
            }
            QThread::msleep(kStableGapMs);
        }
        if (grabfail) {
            log::warn("zpin.scroll", QStringLiteral("长截图第 %1 帧抓取失败").arg(frames));
            // 抓不到屏（锁屏/UAC/独占全屏）= 失败中止：照旧 cancelled=false 出货
            // 的话，App 侧气泡会报「完成」，用户拿到缺一截的长图还以为全须全尾。
            cancelled = true;
            stopReason = "grabfail";
            break;
        }
        if (stopR) {
            cancelled = QLatin1String(stopR) == QLatin1String("stopped") ||
                        QLatin1String(stopR) == QLatin1String("esc");
            done = !cancelled;
            stopReason = stopR;
            break;
        }
        const rcore::ScrollStitcher::Step step = stitcher.push(frame);
        const bool wasIdle = idle;
        const int lastMissed = missed;
        idle = step.frozen;
        if (step.offset > 0) {
            ++frames;
            lastSticky = step.sticky;
            // 逐帧埋点：对齐失败的场景靠它定位（offset/sticky 的组合形态）；
            // 重排回填单独记——那是页面在滚动间隙自己重排，不是拼接失败
            log::info("zpin.scroll",
                      QStringLiteral("帧 %1：offset=%2 canvas=%3 sticky=%4%5")
                          .arg(frames)
                          .arg(step.offset)
                          .arg(step.canvasHeight)
                          .arg(step.sticky)
                          .arg(step.rebased ? QStringLiteral("（重排回填）")
                                            : QString()));
            emit progress(frames, stitcher.canvasHeight(), false, missed);
            emit previewReady(stitcher.preview(200, 320));
        } else if (!step.frozen) {
            // 动了但没对上：一次滚得太远（超出对齐搜索范围）或悬停预览污染了帧。
            // 不截断也不收工，等下一次滚动；但要记账，否则用户只会发现长图中间
            // 少了一截，而屏幕上什么也没说。
            ++missed;
            log::debug("zpin.scroll", QStringLiteral("画面动了但没对上，跳过这一帧"));
        }
        // 面板只在状态真的变了时刷一次，别按轮询频率重绘整块面板
        if (step.offset == 0 && (idle != wasIdle || missed != lastMissed))
            emit progress(frames, stitcher.canvasHeight(), idle, missed);
        if (step.done) {   // 只有画布到高度上限才是 done
            done = true;
            stopReason = "cap";
        }
    }
    if (m_stop) {
        cancelled = true;
        stopReason = "stopped";
    }
    const QImage out = stitcher.takeCanvas();
    // 收尾原因要写清楚是「你点的完成」还是「撞到上限」——早先两种情况都只打一行
    // 「结束」，用户分不清成功和截断，长图少了一截也看不出来。
    // 原因用 ASCII 记号（cap / finish / ...）而不是中文：日志经控制台
    // 或非 UTF-8 编辑器查看时中文会变乱码，记号不会。中文说明放括号里。
    const char* reasonCn = "未知";
    if (QLatin1String(stopReason) == QLatin1String("cap"))
        reasonCn = "达到帧数/高度上限";
    else if (QLatin1String(stopReason) == QLatin1String("space"))
        reasonCn = "空格完成";
    else if (QLatin1String(stopReason) == QLatin1String("finish"))
        reasonCn = "按钮完成";
    else if (QLatin1String(stopReason) == QLatin1String("esc"))
        reasonCn = "Esc 取消";
    else if (QLatin1String(stopReason) == QLatin1String("grabfail"))
        reasonCn = "抓屏失败";
    else if (QLatin1String(stopReason) == QLatin1String("stopped"))
        reasonCn = "被外部停止";
    log::info("zpin.scroll",
              QString::fromUtf8("长截图结束 [%1]：%2 帧，%3×%4，粘性头 %5px，没接上 %6 次（%7）")
                  .arg(QLatin1String(stopReason))
                  .arg(frames)
                  .arg(out.width())
                  .arg(out.height())
                  .arg(lastSticky)
                  .arg(missed)
                  .arg(QString::fromUtf8(reasonCn))
                  .append(cancelled ? QString::fromUtf8("（已取消）") : QString()));
    emit finished(out, frames, cancelled, m_dpr, QLatin1String(stopReason));
}

}  // namespace scroll
}  // namespace zpin
