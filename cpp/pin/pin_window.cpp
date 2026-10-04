#include "pin_window.hpp"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QClipboard>
#include <QCursor>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPointer>
#include <QScreen>
#include <QTransform>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "config.hpp"
#include "defaults.hpp"
#include "engine.hpp"
#include "logging.hpp"
#include "ocr.hpp"
#include "output.hpp"
#include "pin_annotator.hpp"
#include "pins.hpp"
#include "win32util.hpp"

namespace zpin {
namespace {

constexpr double kZoomMin = 0.05;
constexpr double kZoomMax = 8.0;
constexpr double kZoomStep = 1.15;
constexpr int kMargin = 8;          // 阴影/边框留白（窗口比图像大出来的部分）
constexpr double kThumbMax = 512;   // 缩略图长边上限
constexpr double kMinSide = 24;     // 缩放下限：较长边不小于该像素

const char* kMenuStyle = R"(
QMenu { background: rgba(23,23,28,244); color:#E8E8E8; border:1px solid rgba(255,255,255,32);
        border-radius: 8px; padding: 4px; font-size: 12px; }
QMenu::item { padding: 5px 24px 5px 10px; border-radius: 5px; }
QMenu::item:selected { background: rgba(255,255,255,30); }
QMenu::item:disabled { color: rgba(232,232,232,90); }
QMenu::separator { height: 1px; background: rgba(255,255,255,28); margin: 4px 8px; }
QMenu::indicator:checked { background: rgba(111,195,255,80); border-radius: 3px; width: 6px; height: 6px; }
)";

}  // namespace

PinWindow::PinWindow(const QImage& image, PinManager* manager, const QPointF& pos,
                     double refDpr)
    : QWidget(nullptr, Qt::WindowType::FramelessWindowHint |
                           Qt::WindowType::WindowStaysOnTopHint | Qt::WindowType::Tool),
      m_manager(manager),
      m_base(image),
      m_source(image),
      m_borderColor(config::getStr(QStringLiteral("Pin/border_color"))),
      m_labelTimer(this) {
    setAttribute(Qt::WidgetAttribute::WA_TranslucentBackground);
    setMouseTracking(true);
    m_glow = config::getBool(QStringLiteral("Pin/border_glow"));
    m_shadow = config::getBool(QStringLiteral("Pin/shadow"));
    m_opacity = std::clamp(config::getInt(QStringLiteral("Pin/default_opacity")),
                            defaults::kOpacityMin, defaults::kOpacityMax) / 100.0;

    // 截图图是物理像素，Qt 窗口走逻辑坐标，要 1:1 显示必须除以参考缩放比。
    // 截图内容的参考比例 = 那次截图所用屏的比例（由调用方传进来）；混合 DPI 下
    // 它可能 ≠ 落点屏比例，默认用落点屏比例，否则「截图即贴」会大一圈。
    const QScreen* scr = QGuiApplication::screenAt(QPoint(qRound(pos.x()), qRound(pos.y())));
    double local = scr ? scr->devicePixelRatio() : 1.0;
    if (local < 1.0 && QGuiApplication::primaryScreen())
        local = QGuiApplication::primaryScreen()->devicePixelRatio();
    if (local < 1.0)
        local = 1.0;
    m_refDpr = refDpr > 0.0 ? refDpr : local;

    setWindowOpacity(m_opacity);
    setStyleSheet(QString::fromLatin1(kMenuStyle));
    // 不设焦点策略的话 QWidget 默认 NoFocus，keyPressEvent 永远收不到，
    // 菜单上标着的 Ctrl+C / Del / Esc 全是摆设
    setFocusPolicy(Qt::FocusPolicy::StrongFocus);
    updateSize();
    move(qRound(pos.x() - kMargin), qRound(pos.y() - kMargin));

    m_labelTimer.setSingleShot(true);
    connect(&m_labelTimer, &QTimer::timeout, this, [this] { update(); });
}

PinWindow::~PinWindow() = default;

// ---- 几何 ----

QPointF PinWindow::imgSize() const {
    return QPointF(m_source.width(), m_source.height());
}

QRectF PinWindow::imgRect() const {
    const QPointF sz = imgSize();
    const double f = m_scale / m_refDpr;   // 物理 px ÷ dpr = 逻辑 px，保证 1:1 视觉尺寸
    return QRectF(kMargin, kMargin, sz.x() * f, sz.y() * f);
}

QPointF PinWindow::localToImage(const QPointF& pos) const {
    const double f = m_scale / m_refDpr;
    if (f <= 0)
        return QPointF(0, 0);
    return QPointF((pos.x() - kMargin) / f, (pos.y() - kMargin) / f);
}

QPointF PinWindow::imageToGlobal(const QPointF& ip) const {
    const double f = m_scale / m_refDpr;
    return mapToGlobal(QPointF(kMargin + ip.x() * f, kMargin + ip.y() * f));
}

void PinWindow::updateSize() {
    const QRectF r = imgRect();
    resize(qRound(r.width()) + kMargin * 2, qRound(r.height()) + kMargin * 2);
}

void PinWindow::showEvent(QShowEvent* ev) {
    QWidget::showEvent(ev);
    win32::setTopmost(reinterpret_cast<win32::Hwnd>(winId()));
    setFocus();  // 贴图后立刻能吃键盘快捷键
}

// ---- 鼠标穿透 ----

void PinWindow::setClickThrough(bool on) {
    if (on == m_clickThrough)
        return;
    if (!win32::setClickThrough(reinterpret_cast<win32::Hwnd>(winId()), on))
        return;
    m_clickThrough = on;
    // 改扩展样式会让系统的 layered 呈现失效：半透明的阴影边距区被直接按预乘后的
    // RGB 画出来（透明黑 -> 纯黑），表现就是贴图四周多出一圈实心黑框。
    // 开着穿透时窗口不吃鼠标、画面对不对不容易察觉；一恢复可点击就露馅了。
    // 这里同步重绘一次，立刻让 Qt 重新走 UpdateLayeredWindow 把正确位图交给系统
    // （update() 是排队的，靠后续事件循环，静止的贴图可能永远等不到）。
    repaint();
    if (on)
        emit m_manager->notify(QStringLiteral("贴图"),
                               QStringLiteral("已开启鼠标穿透：托盘 → 恢复贴图可点击"));
}

// ---- 贴图标注 ----

bool PinWindow::annotating() const {
    return m_annot && m_annot->isActive();
}

void PinWindow::setAnnotate(bool on) {
    if (annotating() == on)
        return;
    if (on) {
        if (m_clickThrough)
            setClickThrough(false);  // 穿透中的窗口收不到鼠标
        if (!m_annot)
            m_annot = std::make_unique<PinAnnotator>(this);
        m_annot->start();
    } else {
        // 退出标注模式（完成键/Esc/菜单）也是一次收工：用户多半不会再回来改，
        // 回写要在这里做，不能只等贴图关闭——点完成之后贴图可能一直开着
        flushDocSave();
        if (m_annot)
            m_annot->stop();
    }
    update();
}

void PinWindow::setAnnotateWithDoc(const QJsonArray& doc, bool activate) {
    if (activate) {
        setAnnotate(true);
    } else if (!m_annot) {
        // 只装文档不进标注模式：构造 annotator 但不 start（无工具条、不收输入），
        // composite 会把文档形状画出来；之后从菜单进标注模式直接复用这个引擎
        m_annot = std::make_unique<PinAnnotator>(this);
    }
    // 载入失败（文档里有认不出的形状）= 整份不载入，贴图仍是那张干净底图
    if (m_annot && m_annot->engine() && !doc.isEmpty())
        m_annot->engine()->loadDocument(doc);
    m_lastFlushed = doc;  // 回写去重基准：刚载入的样子不算「改过」
    update();
}

void PinWindow::flushDocSave() {
    if (!m_docSave || m_docInvalid || !m_annot || !m_annot->engine())
        return;
    const QJsonArray doc = m_annot->engine()->documentJson();
    if (doc == m_lastFlushed)
        return;  // 没改过 / 刚回写过（完成键回写后又关贴图）：别让重复回写白跑
    m_lastFlushed = doc;
    m_docSave(doc);
}

void PinWindow::bakeAnnotations() {
    // 标注坐标锚定在**当前显示图**（m_source，已含旋转/翻转）上，而 m_base 是没
    // 有几何变换的原始图。早先这里直接 bake(m_base)：形状画在错误的坐标系里，
    // 「旋转后再标注再旋转」会把上一轮的标注糊到错位的地方。
    // 现在把几何变换随烘焙一起写进像素（烘完旋转/翻转归零），灰度则留在视图层
    // ——烘焙必须落在彩色图上，否则用户关掉灰度再也回不到彩色。
    if (!m_annot || !m_annot->hasContent())
        return;
    QTransform t;
    t.rotate(m_rotation);
    if (m_flipH)
        t.scale(-1.0, 1.0);
    if (m_flipV)
        t.scale(1.0, -1.0);
    const QImage view =
        t.isIdentity() ? m_base
                       : m_base.transformed(t, Qt::TransformationMode::SmoothTransformation);
    m_base = m_annot->bake(view);
    // 烘焙把标注烙进像素、引擎清空：此后底图与历史那条干净底图不再同源，
    // 文档坐标对不上了，标记不可回写（回写空文档会把历史条目的标注抹掉）
    m_docInvalid = true;
    m_rotation = 0.0;
    m_flipH = m_flipV = false;
    m_annot->clear();
}

QImage PinWindow::composite() const {
    // 当前输出图像 = 变换后的底图 + 未烘焙的标注层。标注坐标本来就锚定在
    // m_source（displayImage）上，所以形状画在 (0,0) 即与画面对位。
    QImage img(m_source);
    if (m_annot && m_annot->hasContent()) {
        QPainter p(&img);
        p.setRenderHint(QPainter::RenderHint::Antialiasing, true);
        m_annot->engine()->draw(p);
        if (m_annot->activeShape())
            m_annot->activeShape()->draw(p);
    }
    return img;
}

double PinWindow::maxSide() const {
    const int configured = config::getInt(QStringLiteral("Pin/max_window_size"));
    // 正常路径 config::getInt 已经回落到默认值；这里只兜「用户手改 config.ini
    // 写成 0/负数」这一种。兜底值取 defaults 而不是再写一遍字面量——早先这里
    // 硬编码 12000.0，defaults.cpp 改成别的值它会悄悄留在旧值上。
    if (configured <= 0) {
        log::warn("pin", QStringLiteral("max_window_size 配置非法（%1），改用默认值")
                              .arg(configured));
        return defaults::defaultValue("Pin/max_window_size").toInt();
    }
    return configured;
}

// ---- 变换 ----

void PinWindow::rebuildSource() {
    QImage img = m_base;
    if (m_grayscale) {
        img = img.convertToFormat(QImage::Format::Format_Grayscale8)
                  .convertToFormat(QImage::Format::Format_ARGB32);
    }
    QTransform t;
    t.rotate(m_rotation);
    if (m_flipH)
        t.scale(-1.0, 1.0);
    if (m_flipV)
        t.scale(1.0, -1.0);
    m_source = t.isIdentity() ? QImage(img)
                              : img.transformed(t, Qt::TransformationMode::SmoothTransformation);
    // 锚定图换了（旋转/翻转/灰度/把标注烘进底图），在途的「一键脱敏」文本框坐标就
    // 不再对应这张图了 —— 抬代号让那次回调作废，宁可不画也不画错地方。
    ++m_sanitizeSeq;
    // 特效底图（马赛克/模糊）跟着显示图走：它还停在旧像素上的话，旋转/翻转/灰度
    // 之后涂抹马赛克就是在采变换前的颜色，模糊也一样。
    if (m_annot)
        m_annot->engine()->rebase(m_source);
    m_thumb.reset();
    clampScale();
    updateSize();
    update();
}

void PinWindow::clampScale() {
    const QPointF sz = imgSize();
    const double m = std::max(sz.x(), sz.y());
    if (m > 0) {
        // 下限：窗口逻辑边不小于 kMinSide（换算回物理放大系数需乘 dpr）
        m_scale = std::max(m_scale, kMinSide * m_refDpr / m);
        // 上限：窗口（含阴影边距）任一边不超过 Pin/max_window_size（逻辑 px）
        const double limit = maxSide();
        const double over = m * m_scale / m_refDpr + kMargin * 2 - limit;
        if (over > 0)
            m_scale = std::max(kZoomMin, (limit - kMargin * 2) * m_refDpr / m);
    }
    m_scale = std::max(kZoomMin, std::min(kZoomMax, m_scale));
}

void PinWindow::applyScale() {
    clampScale();
    updateSize();
    const QPointF sz = imgSize();
    flashLabel(QStringLiteral("%1 × %2")
                   .arg(qRound(sz.x() * m_scale))
                   .arg(qRound(sz.y() * m_scale)));
    update();
    if (m_annot && annotating())
        m_annot->positionToolbar();  // 缩放改了几何，工具条跟着重新贴位
}

// ---- 缩放（光标锚点） ----

void PinWindow::zoomAt(double factor, const QPointF& globalPos) {
    const double newScale = m_scale * factor;
    if (newScale == m_scale)
        return;
    const double fOld = m_scale / m_refDpr;   // 缩放前：逻辑 px / image px
    const QPoint tl = frameGeometry().topLeft();
    // 光标下的图像坐标必须按旧比例算，否则 move 目标恒等于原左上角
    const double ix = (globalPos.x() - tl.x() - kMargin) / fOld;
    const double iy = (globalPos.y() - tl.y() - kMargin) / fOld;
    m_scale = newScale;
    applyScale();
    const double fNew = m_scale / m_refDpr;   // clamp 后的实际比例，落位用它
    move(qRound(globalPos.x() - ix * fNew - kMargin),
         qRound(globalPos.y() - iy * fNew - kMargin));
    keepOnScreen();
}

void PinWindow::keepOnScreen() {
    // 缩放到很大/很小后锚点换算可能把窗口推到屏幕外，这里拉回来一点。
    QScreen* scr = QGuiApplication::screenAt(frameGeometry().center());
    if (!scr)
        scr = QGuiApplication::primaryScreen();
    if (!scr)
        return;
    const QRect g = scr->availableGeometry();
    constexpr int vis = 64;  // 至少留这么多像素在屏内，方便再拖回来
    const double lo = double(g.left() + vis - width());
    const double hi = double(g.right() - vis);
    const double to = double(g.top() - height() + vis);
    const double bo = double(g.bottom() - vis);
    const int nx = qRound(std::min(std::max(double(x()), lo), hi));
    const int ny = qRound(std::min(std::max(double(y()), to), bo));
    if (nx != x() || ny != y())
        move(nx, ny);
}

QRect PinWindow::snapRect() const {
    // 可见内容（图像区）的绝对物理矩形：窗口吸附按物理坐标比对。
    // 不能直接 mapToGlobal × 本屏 dpr——Qt 的全局逻辑坐标掺着其它屏的缩放，
    // 混合 DPI 多屏下那样算横向会整体偏移；先用本屏几何把点折算成屏内逻辑
    // 坐标，再乘本屏 dpr 加上本屏物理原点（monitorRects 与 DesktopMap 同源，
    // 两边对同一块屏的物理矩形必须一致，否则吸附框错位）。
    const QRectF local = imgRect();
    const double dpr = devicePixelRatioF();
    const QScreen* scr = screen();
    const QHash<QString, QRect> mons = win32::monitorRects();
    const auto mon = scr ? mons.constFind(scr->name()) : mons.constEnd();
    if (scr && mon != mons.constEnd()) {
        const QPointF tlLocal = mapToGlobal(local.topLeft()) - scr->geometry().topLeft();
        const QPointF tlPhys = QPointF(mon->left(), mon->top()) + tlLocal * dpr;
        return QRect(qRound(tlPhys.x()), qRound(tlPhys.y()),
                     qRound(local.width() * dpr), qRound(local.height() * dpr));
    }
    // 显示器没匹配上（DesktopMap 同款兜底）：退回全局逻辑 × dpr——单屏 /
    // 等缩放多屏下它本来就是对的
    const QPointF tl = mapToGlobal(local.topLeft());
    return QRect(qRound(tl.x() * dpr), qRound(tl.y() * dpr),
                 qRound(local.width() * dpr), qRound(local.height() * dpr));
}

void PinWindow::zoomIn() {
    zoomAt(kZoomStep, QPointF(mapToGlobal(rect().center())));
}

void PinWindow::zoomOut() {
    zoomAt(1.0 / kZoomStep, QPointF(mapToGlobal(rect().center())));
}

void PinWindow::actualSize() {
    m_scale = 1.0;
    applyScale();
}

void PinWindow::fitToScreen() {
    // 长图等超屏内容入场：按所在屏可用区域（留 40px 边距）整图缩放，
    // clampScale 会再兜底上下限；位置由 keepOnScreen 拉回屏内。
    QScreen* scr = QGuiApplication::screenAt(frameGeometry().center());
    if (!scr)
        scr = QGuiApplication::primaryScreen();
    if (!scr)
        return;
    const QRect g = scr->availableGeometry();
    const QPointF sz = imgSize();
    const double wLog = sz.x() / m_refDpr;
    const double hLog = sz.y() / m_refDpr;
    if (wLog <= 0 || hLog <= 0)
        return;
    const double k = std::min({1.0, (g.width() - 40) / wLog, (g.height() - 40) / hLog});
    m_scale = k;
    clampScale();
    updateSize();
    applyScale();
    // kZoomMin 下限是给**交互缩放**的（防把窗口缩没了），fit 不受它管：长图高过
    // 20 倍屏高时 fit 结果天然低于下限，被 clampScale/applyScale 抬回去就永远
    // 进不了一屏。只往回掰「被下限抬高」的情况（上限裁剪 m_scale < k 不动）。
    if (k < kZoomMin && m_scale > k) {
        m_scale = k;
        updateSize();
        update();
    }
    // 居中落到所在屏：贴图落点往往是光标处，长图会大半
    // 悬在屏幕外；先居中再 keepOnScreen 兜底。
    const int cx = g.left() + (g.width() - width()) / 2;
    const int cy = g.top() + (g.height() - height()) / 2;
    move(cx, cy);
    keepOnScreen();
}

void PinWindow::reset() {
    if (annotating())
        return;
    // 与 rotate90/flip 同款先烘焙：rebuildSource 会经 rebase 抬掉特效缓存标记，
    // 形状栈里留着持裸指针的马赛克/模糊，下次进标注后台重生成缓存时就会和
    // paint 并发读写同一块 QImage（use-after-free）。
    bakeAnnotations();
    m_scale = 1.0;
    m_rotation = 0.0;
    m_flipH = m_flipV = false;
    m_grayscale = false;
    rebuildSource();
    applyScale();
    keepOnScreen();
}

void PinWindow::rotate90() {
    if (annotating())
        return;
    bakeAnnotations();
    // 几何从此与历史干净底图不同源：无标注时 bakeAnnotations 提前返回不置位，
    // 之后新画的标注坐标回写会整体错位，这里无条件作废文档
    m_docInvalid = true;
    m_rotation = std::fmod(m_rotation + 90.0, 360.0);
    rebuildSource();
    applyScale();
    keepOnScreen();   // 宽图转成高图后下缘可能悬在屏外，与缩放同款收拢
}

void PinWindow::flipH() {
    if (annotating())
        return;
    bakeAnnotations();
    m_docInvalid = true;   // 与 rotate90 同理：几何变了就不再回写文档
    m_flipH = !m_flipH;
    rebuildSource();
    applyScale();
    keepOnScreen();
}

void PinWindow::flipV() {
    if (annotating())
        return;
    bakeAnnotations();
    m_docInvalid = true;   // 与 rotate90 同理：几何变了就不再回写文档
    m_flipV = !m_flipV;
    rebuildSource();
    applyScale();
    keepOnScreen();
}

void PinWindow::toggleGrayscale() {
    if (annotating())
        return;
    bakeAnnotations();
    m_grayscale = !m_grayscale;
    rebuildSource();
    applyScale();
}

// ---- 外观 ----

void PinWindow::setOpacityPct(int pct) {
    m_opacity = std::clamp(pct / 100.0, defaults::kOpacityMin / 100.0,
                           defaults::kOpacityMax / 100.0);
    setWindowOpacity(m_opacity);
}

void PinWindow::setBorderOn(bool on) {
    m_border = on;
    update();
}

void PinWindow::setShadowOn(bool on) {
    m_shadow = on;
    update();
}

// ---- 输出 ----

void PinWindow::copyImage() {
    // setImage 而非 setPixmap：不持有 HBITMAP 原生句柄，规避 Windows 在别处
    // 读剪贴板（WM_RENDERFORMAT）时因句柄失效而崩溃
    QApplication::clipboard()->setImage(composite());
}

void PinWindow::ocrText() {
    // 识别在后台跑，最长约 2.5 秒；菜单刚关掉就没有任何东西指向「正在识别」，
    // 窗口看着像没反应。与 sanitizeImage 同款即时反馈：先把状态画在贴图上，
    // 完成或失败再由下面的托盘气泡接手。
    flashLabel(QStringLiteral("正在识别文字……"), 2500);
    // 识别在后台线程跑几十到几百毫秒，回调落地时贴图可能已被销毁（Del 键 /
    // 托盘销毁），裸 this 会写到已析构的对象上。QPointer 随析构自动置空。
    // 回调本身在主线程（ocr.cpp 用 invokeMethod 投递回来），可直接用 w->m_manager。
    ocr::recognizeAsync(composite(), [w = QPointer<PinWindow>(this)](
                                    const std::optional<QString>& text) {
        if (w.isNull())
            return;
        ocr::publishText(text, [w](const QString& title, const QString& body) {
            if (w.isNull())
                return;
            emit w->m_manager->notify(title, body);
        });
    });
}

void PinWindow::ocrTable() {
    // 同「识别文字」：点击后先给即时反馈，完成/失败再由托盘气泡接手
    flashLabel(QStringLiteral("正在识别表格……"), 2500);
    // 同「识别文字」：回调回来时贴图可能已被销毁，QPointer 随析构置空
    ocr::recognizeTablesAsync(
        composite(), [w = QPointer<PinWindow>(this)](
                         const std::optional<QVector<rcore::OcrTable>>& tables) {
            if (w.isNull())
                return;
            ocr::publishTables(tables, [w](const QString& title, const QString& body) {
                if (w.isNull())
                    return;
                emit w->m_manager->notify(title, body);
            });
        });
}

void PinWindow::sanitizeImage() {
    // 静默打码：**不进入标注态** —— 否则「一键脱敏」就成了「标注」的别名，工具条还会
    // 莫名弹出来。这里只借用那个引擎：马赛克仍以标注身份存在（画在图上、进 composite
    // 与另存为、随几何变换一起烘焙），要撤销得进「标注」再按 Ctrl+Z。
    if (!m_annot)
        m_annot = std::make_unique<PinAnnotator>(this);
    const QImage img = displayImage();
    if (img.width() < 8 || img.height() < 8)
        return;
    // 在途防重入：连续点两次会把两批完全重叠的马赛克压进撤销栈（seq 守卫挡不住
    // 同一张图上的重复调用），撤销一次画面纹丝不动，看起来像撤销坏了。
    if (m_sanitizePending)
        return;
    m_sanitizePending = true;
    // 反馈画在贴图自己的角标上（跟缩放时那条「1920 × 1080」同一个位置）：脱敏的
    // 结果必须当场看得见，飞角气泡容易错过。给 2.5 秒是因为识别可能要一秒多，
    // 结果回来时会用新文字把计时器重置。
    flashLabel(QStringLiteral("正在识别敏感信息"), 2500);
    // 识别在后台跑几十到几百毫秒。回调回来时如果贴图已被销毁，或者在这期间转过/
    // 翻过/灰度过/烘焙过（rebuildSource 抬过代号），这些文本框坐标就不再对应这张
    // 图 —— 宁可不打，也不打在错地方。
    const quint64 seq = m_sanitizeSeq;
    QPointer<PinWindow> self(this);
    ocr::recognizeBoxesAsync(img, [self, seq](std::optional<QVector<rcore::OcrBox>> boxes) {
        if (self.isNull())
            return;
        self->m_sanitizePending = false;   // 回调必达：无论结果新旧都放行下一次脱敏
        if (seq != self->m_sanitizeSeq || !self->m_annot)
            return;
        if (!boxes || boxes->isEmpty()) {
            self->flashLabel(QStringLiteral("没有识别到文字"));
            emit self->m_manager->notify(QStringLiteral("一键脱敏"),
                                         QStringLiteral("没有识别到文字"));
            return;
        }
        QVector<QRect> sensitive;
        for (const rcore::OcrBox& box : *boxes) {
            if (ocr::looksSensitiveText(box.text))
                sensitive.append(box.rect);
        }
        const int masked = self->m_annot->maskSensitiveLines(sensitive);
        log::info("zpin.sanitize",
                  QStringLiteral("贴图脱敏：OCR %1 行，命中 %2 行，打码 %3 处")
                      .arg(boxes->size())
                      .arg(sensitive.size())
                      .arg(masked));
        // 角标只说「打了几处」，撤销去哪说留给气泡：图上那条 1.8 秒就走，越短越好读
        self->flashLabel(masked ? QStringLiteral("已打码 %1 处")
                                   .arg(masked)
                                : QStringLiteral("没有发现敏感信息"),
                         1800);
        emit self->m_manager->notify(
            QStringLiteral("一键脱敏"),
            masked ? QStringLiteral("已自动打码 %1 处（进「标注」可撤销）").arg(masked)
                   : QStringLiteral("没有发现手机号 / 邮箱 / 身份证"));
    });
}

void PinWindow::saveAs() {
    // 写盘在工作线程（整屏 PNG 约 200ms），回调也在工作线程：两处都得管
    //   1) QPointer 挡住「回调回来时贴图已被删」
    //   2) 投递回主线程再发信号——m_manager 只在主线程用
    // m_manager 在主线程取（PinWindow 由它持有，活得比窗口久），以它作
    // invokeMethod 的 context：它若已析构，排队中的调用会随之作废。
    PinManager* mgr = m_manager;
    output::saveImageDialogAsync(composite(), this,
                                 [w = QPointer<PinWindow>(this), mgr](
                                     std::optional<QString> path) {
        if (!path || w.isNull())
            return;   // 用户取消 / 贴图已销毁：静默
        if (path->isEmpty()) {
            // 空串是约定好的「保存失败」（成功带路径、取消是 nullopt），不能跟
            // 取消一样一声不吭——用户会以为存上了。
            QMetaObject::invokeMethod(mgr, [mgr] {
                emit mgr->notify(QStringLiteral("贴图"),
                                 QStringLiteral("保存失败：文件写入不了"));
            }, Qt::QueuedConnection);
            return;
        }
        QMetaObject::invokeMethod(mgr, [mgr, p = *path] {
            emit mgr->notify(QStringLiteral("贴图"), QStringLiteral("已保存：%1").arg(p));
        }, Qt::QueuedConnection);
    });
    // 原生对话框抢走激活后，标注工具条（不接焦点的 Tool 浮窗）会被重新激活的
    // 贴图窗压住、或还挂着模态阻塞；对话框一关就重新定位 + 置顶，取消另存为
    // 也能回到标注现场
    if (annotating())
        m_annot->positionToolbar();
}

// ---- 交互 ----

void PinWindow::mousePressEvent(QMouseEvent* ev) {
    if (annotating()) {
        if (ev->button() == Qt::MouseButton::LeftButton) {
            // 未被标注层消费（没选工具且没点中形状）= 回退为拖窗
            m_annotConsumed = m_annot->press(ev->position());
            if (m_annotConsumed)
                return;
        } else if (ev->button() == Qt::MouseButton::RightButton) {
            dropPendingDrag();
            // 菜单会抓走鼠标、吞掉左键 release：不先丢标注层的拖拽态，关菜单后
            // 笔迹会跟着光标继续长（窗口自身拖拽有 dropPendingDrag 防同一件事）。
            m_annot->dropInteraction();
            showMenu();
            return;
        }
    }
    if (ev->button() == Qt::MouseButton::LeftButton) {
        const QPointF gp = ev->globalPosition();
        m_dragOffset = QPointF(gp.x() - x(), gp.y() - y());
    } else if (ev->button() == Qt::MouseButton::RightButton) {
        dropPendingDrag();
        showMenu();
    }
}

void PinWindow::mouseDoubleClickEvent(QMouseEvent* ev) {
    // 双击贴图 = 隐藏（托盘「显示全部」或隐藏/显示热键可找回）；
    // 标注模式里双击通常是连续两次编辑点击，不动作
    if (annotating())
        return;
    hide();
    m_manager->notifyVisibility();
}

void PinWindow::dropPendingDrag() {
    // QMenu::exec 会抓走鼠标：菜单开着时那次左键抬起落在菜单里，本窗的
    // mouseReleaseEvent 收不到，m_dragOffset 就一直悬着 —— 之后鼠标划过贴图
    // 它自己被拖走（真出过事：右键菜单开着按热键截图，回来发现贴图挪了位）。
    m_dragOffset.reset();
}

void PinWindow::mouseMoveEvent(QMouseEvent* ev) {
    if (annotating()) {
        m_annot->move(ev->position());  // 无交互时仅刷新橡皮悬停预览
        if (m_annotConsumed)
            return;
    }
    // 只认「左键此刻真按着」：release 可能被菜单或别处的鼠标抓取吃掉，
    // 单靠 m_dragOffset 复位就会留下粘住的拖拽
    if (m_dragOffset && (ev->buttons() & Qt::MouseButton::LeftButton)) {
        const QPointF gp = ev->globalPosition();
        move(qRound(gp.x() - m_dragOffset->x()), qRound(gp.y() - m_dragOffset->y()));
        // 工具条贴着贴图走，不留在原地
        if (m_annot)
            m_annot->positionToolbar();
    }
}

void PinWindow::mouseReleaseEvent(QMouseEvent* ev) {
    if (annotating() && m_annotConsumed) {
        m_annot->release();
        if (ev->button() == Qt::MouseButton::LeftButton)
            m_annotConsumed = false;
        return;
    }
    if (ev->button() == Qt::MouseButton::LeftButton)
        m_dragOffset.reset();
}

void PinWindow::wheelEvent(QWheelEvent* ev) {
    const int delta = ev->angleDelta().y();
    if (delta == 0)
        return;
    zoomAt(delta > 0 ? kZoomStep : 1.0 / kZoomStep, ev->globalPosition());
}

void PinWindow::keyPressEvent(QKeyEvent* ev) {
    const int k = ev->key();
    const bool ctrl = ev->modifiers().testFlag(Qt::KeyboardModifier::ControlModifier);
    if (annotating()) {
        if (k == Qt::Key::Key_Escape) {
            setAnnotate(false);
            return;
        }
        if (m_annot->handleKey(ev))
            return;
        // 标注模式下 Del/Backspace 只用于删除选中形状
        if (k == Qt::Key::Key_Delete || k == Qt::Key::Key_Backspace)
            return;
    }
    if (k == Qt::Key::Key_Escape) {
        hidePin();
    } else if (k == Qt::Key::Key_Delete) {
        m_manager->deletePin(this);
    } else if (k == Qt::Key::Key_C && ctrl) {
        copyImage();
    } else if (k == Qt::Key::Key_S && ctrl) {
        saveAs();
    } else if ((k == Qt::Key::Key_Plus || k == Qt::Key::Key_Equal) && ctrl) {
        zoomIn();
    } else if (k == Qt::Key::Key_Minus && ctrl) {
        zoomOut();
    } else if (k == Qt::Key::Key_0 && ctrl) {
        actualSize();
    } else if (k == Qt::Key::Key_R && ctrl) {
        reset();
    } else if (k == Qt::Key::Key_T && ctrl) {
        rotate90();
    } else {
        QWidget::keyPressEvent(ev);
    }
}

void PinWindow::closeEvent(QCloseEvent* ev) {
    flushDocSave();  // 回写要在标注层还活着时做
    // 销毁时收掉标注层（工具条是独立顶层窗，不收会残留）
    if (m_annot)
        m_annot->stop();
    // Alt+F4 等外部关闭也要让管理器知道可见性变了
    m_manager->notifyVisibility();
    QWidget::closeEvent(ev);
}

// ---- 菜单 ----

void PinWindow::hidePin() {
    // 隐藏（关闭）这张贴图：从屏幕收起，可从托盘「隐藏/显示所有贴图」找回
    flushDocSave();  // 隐藏也算一次收工；找回再改还会再回写
    if (m_annot)
        m_annot->stop();  // 标注工具条是独立顶层窗，一起收掉
    hide();
    m_manager->notifyVisibility();
}

void PinWindow::destroyPin() {
    // 销毁：从贴图列表与屏幕彻底移除（不弹确认，与 Del 键一致）
    m_manager->deletePin(this);
}

void PinWindow::showMenu() {
    // 右键全项菜单：标注 / 输出 / 变换 / 外观 / 隐藏与销毁
    auto* menu = new QMenu(this);
    menu->setStyleSheet(QString::fromLatin1(kMenuStyle));

    // 顺序按「用得多→用得少→危险动作垫底」：标注、输出、几何变换、外观，
    // 最后才是隐藏/销毁（销毁原先夹在输出和标注中间，误点过一次就丢贴图）。
    // 缩放（放大/缩小/实际大小/重置）不再占菜单项：滚轮以光标为中心缩放本来就
    // 更快，Ctrl++ / Ctrl+- / Ctrl+0 / Ctrl+R 键位全部保留。
    QAction* actAnn = menu->addAction(QStringLiteral("标注"));
    actAnn->setCheckable(true);
    actAnn->setChecked(annotating());
    connect(actAnn, &QAction::triggered, this, [this](bool on) {
        // 必须排队到下一个事件循环：此刻 menu->exec() 的嵌套循环还开着、鼠标
        // 仍被 QMenu 抓着，而 setAnnotate(true) 会 show() 一个新的顶层工具条窗
        // 并 raise/激活它 —— 新窗抢走激活后菜单收不到该来的释放事件，exec()
        // 永不返回，整个进程看着就卡死了（真出过事）。让菜单先干净地关掉。
        QMetaObject::invokeMethod(this, [this, on] { setAnnotate(on); }, Qt::QueuedConnection);
    });
    menu->addSeparator();

    QAction* actCopy = menu->addAction(QStringLiteral("复制\tCtrl+C"));
    connect(actCopy, &QAction::triggered, this, &PinWindow::copyImage);
    QAction* actSave = menu->addAction(QStringLiteral("另存为...\tCtrl+S"));
    connect(actSave, &QAction::triggered, this, &PinWindow::saveAs);
    QAction* actOcr = menu->addAction(QStringLiteral("识别文字"));
    connect(actOcr, &QAction::triggered, this, &PinWindow::ocrText);
    QAction* actTable = menu->addAction(QStringLiteral("识别表格"));
    connect(actTable, &QAction::triggered, this, &PinWindow::ocrTable);
    QAction* actSanitize = menu->addAction(QStringLiteral("一键脱敏"));
    connect(actSanitize, &QAction::triggered, this, &PinWindow::sanitizeImage);
    menu->addSeparator();

    QAction* actRot = menu->addAction(QStringLiteral("旋转 90°\tCtrl+T"));
    connect(actRot, &QAction::triggered, this, &PinWindow::rotate90);
    QAction* actFh = menu->addAction(QStringLiteral("水平翻转"));
    connect(actFh, &QAction::triggered, this, &PinWindow::flipH);
    QAction* actFv = menu->addAction(QStringLiteral("垂直翻转"));
    connect(actFv, &QAction::triggered, this, &PinWindow::flipV);
    QAction* actGray = menu->addAction(QStringLiteral("灰度"));
    actGray->setCheckable(true);
    actGray->setChecked(m_grayscale);
    connect(actGray, &QAction::triggered, this, [this](bool) { toggleGrayscale(); });
    if (annotating()) {
        // 标注坐标锚定在当前显示图上：标注中先禁用几何变换
        // （退出标注后可用，届时未烘焙标注会被自动烧进底图）
        for (QAction* a : {actRot, actFh, actFv, actGray})
            a->setEnabled(false);
    }
    menu->addSeparator();

    QMenu* opMenu = menu->addMenu(QStringLiteral("不透明度"));
    for (int pct : {100, 90, 80, 70, 60, 50, 40, 30, 20, 10}) {
        QAction* a = opMenu->addAction(QStringLiteral("%1%").arg(pct));
        a->setCheckable(true);
        a->setChecked(qRound(m_opacity * 100) == pct);
        connect(a, &QAction::triggered, this, [this, pct] { setOpacityPct(pct); });
    }

    QAction* actBorder = menu->addAction(QStringLiteral("边框"));
    actBorder->setCheckable(true);
    actBorder->setChecked(m_border);
    connect(actBorder, &QAction::triggered, this, [this](bool on) { setBorderOn(on); });

    QAction* actShadow = menu->addAction(QStringLiteral("阴影"));
    actShadow->setCheckable(true);
    actShadow->setChecked(m_shadow);
    connect(actShadow, &QAction::triggered, this, [this](bool on) { setShadowOn(on); });

    QAction* actThrough = menu->addAction(QStringLiteral("鼠标穿透"));
    actThrough->setCheckable(true);
    actThrough->setChecked(m_clickThrough);
    connect(actThrough, &QAction::triggered, this, [this](bool on) { setClickThrough(on); });

    menu->addSeparator();
    QAction* actHide = menu->addAction(QStringLiteral("隐藏\tEsc"));
    connect(actHide, &QAction::triggered, this, &PinWindow::hidePin);
    QAction* actDestroy = menu->addAction(QStringLiteral("销毁\tDel"));
    connect(actDestroy, &QAction::triggered, this, &PinWindow::destroyPin);

    menu->exec(QCursor::pos());
    menu->deleteLater();
}

// ---- 绘制 ----

const QImage& PinWindow::thumbFor() {
    // 取（或懒生成）缩放显示用的预览缩略图（长边不超过 kThumbMax）。
    if (m_thumb)
        return *m_thumb;
    const int w = std::max(1, m_source.width());
    const int h = std::max(1, m_source.height());
    const double k = std::min({kThumbMax / w, kThumbMax / h, 1.0});
    m_thumb = m_source.scaled(std::max(1, qRound(w * k)), std::max(1, qRound(h * k)),
                              Qt::AspectRatioMode::KeepAspectRatio,
                              Qt::TransformationMode::SmoothTransformation);
    return *m_thumb;
}

void PinWindow::drawShadow(QPainter& p, const QRectF& r) {
    constexpr int steps = 6;
    for (int i = steps; i > 0; --i) {
        const int a = static_cast<int>(26.0 * (steps - i) / steps);
        p.setPen(Qt::PenStyle::NoPen);
        p.setBrush(QColor(0, 0, 0, a));
        p.drawRoundedRect(r.adjusted(-i * 0.8, -i * 0.6 + 1, i * 0.8, i * 0.6 + 2),
                          3 + i, 3 + i);
    }
}

void PinWindow::paintEvent(QPaintEvent* ev) {
    Q_UNUSED(ev)
    QPainter p(this);
    const QRectF r = imgRect();
    if (m_shadow)
        drawShadow(p, r);
    // 平滑/缩略图的档位要按**实际采样比**判：m_scale 是相对参考 DPR 的缩放，
    // 图像像素到设备像素还要乘所在屏 DPR。混 DPI 下只看 m_scale 会错档——
    // 200% 屏的图贴到 100% 屏时 m_scale=1 实际在 0.5× 缩小，走最近邻全是锯齿。
    const double sampleRatio = m_scale * devicePixelRatioF() / m_refDpr;
    p.setRenderHint(QPainter::RenderHint::SmoothPixmapTransform, sampleRatio < 1.0);
    if (sampleRatio < 0.5)
        p.drawImage(r, thumbFor());
    else
        p.drawImage(r, m_source);
    if (m_annot)
        m_annot->draw(p);   // 未烘焙的标注层（含编辑框/橡皮预览）
    if (m_border) {
        const QColor c = m_borderColor;
        if (m_glow) {
            // 外层淡色光晕（宽度 3 的半透明同色描边，模拟发光）
            p.setPen(QPen(QColor(c.red(), c.green(), c.blue(), 70), 3.0));
            p.setBrush(Qt::BrushStyle::NoBrush);
            p.drawRect(r.adjusted(-0.5, -0.5, 0.5, 0.5));
        }
        p.setPen(QPen(c, 1.4));
        p.setBrush(Qt::BrushStyle::NoBrush);
        p.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));
    }
    drawLabel(p, r);
}

void PinWindow::flashLabel(const QString& text, int ms) {
    m_labelText = text;
    m_labelTimer.start(ms);
    update();
}

void PinWindow::drawLabel(QPainter& p, const QRectF& r) {
    // 在图像内右上角绘制闪现文字（缩放后的尺寸、一键脱敏的结果）。
    if (!m_labelTimer.isActive() || m_labelText.isEmpty())
        return;
    const QString& text = m_labelText;
    const QFontMetrics fm = p.fontMetrics();
    const double tw = fm.horizontalAdvance(text) + 12;
    const double th = fm.height() + 4;
    // 必须画在图像**内**：窗口的阴影边距只有 8px 高，装不下 ~20px 的气泡，
    // 早先画到边距区后 y 算成负值，气泡被窗口边界裁到只剩底部几像素的黑边
    // ——用户看到的就是「右上角提示显示不全」。
    const double x = r.right() - tw - 6;
    const double y = r.top() + 6;
    p.setPen(Qt::PenStyle::NoPen);
    p.setBrush(QColor(20, 20, 24, 210));
    p.drawRoundedRect(QRectF(x, y, tw, th), 4, 4);
    p.setPen(QColor(255, 255, 255, 230));
    p.drawText(QRectF(x, y, tw, th), Qt::AlignmentFlag::AlignCenter, text);
}

}  // namespace zpin
