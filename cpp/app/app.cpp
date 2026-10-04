#include "app.hpp"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QProcess>
#include <QScreen>
#include <QThread>
#include <QTimer>

#include <optional>

#include "capture.hpp"
#include "config.hpp"
#include "defaults.hpp"
#include "history.hpp"
#include "history_wall.hpp"
#include "hotkey.hpp"
#include "logging.hpp"
#include "memtrim.hpp"
#include "ocr.hpp"
#include "output.hpp"
#include "overlay.hpp"
#include "pin_window.hpp"
#include "pins.hpp"
#include "prefs_dialog.hpp"
#include "rcore.hpp"
#include "scroll_capture.hpp"
#include "startup.hpp"
#include "toolbar.hpp"
#include "tray.hpp"
#include "update.hpp"
#include "win32util.hpp"

namespace zpin {

App::App(QObject* parent) : QObject(parent) {
    const QStringList& order = hotkey::actionOrder();
    for (const QString& action : order)
        m_accels.insert(action, config::hotkeyAccel(action));

    m_tray = new TrayController(m_accels, this);
    m_selector = new SelectionController(this);
    m_selector->setPinRectsProvider([this] { return m_pins->pinSnapRects(); });
    m_pins = new PinManager(this);
    m_history = new HistoryStore(this);
    m_history->setLimits();
    // 历史条目一变，开着的历史墙立刻重铺（新截图落地、清空都走这里）
    connect(m_history, &HistoryStore::changed, this, [this] {
        if (m_wall && m_wall->isVisible())
            m_wall->refresh();
    });

    // 自动更新状态机：静默检查命中就弹一条可点的气泡
    m_updateFlow = new update::UpdateFlow(this);
    connect(m_updateFlow, &update::UpdateFlow::newVersionFound, this,
            [this](const QString& version) {
                m_tray->notifyClickable(
                    QStringLiteral("ZPin 更新"),
                    QStringLiteral("发现新版本 v%1，点此打开 设置 → 关于 安装").arg(version));
            });
}

App::~App() {
    // 长截图进行中退出：池线程裸持 Capture 跑 run()，对象必须活到线程自然退出。
    // Capture 因此不挂 parent（挂了会被 QObject 递归析构当场删掉 = use-after-free，
    // run() 最多再跑一个轮询间隔才察觉 m_stop）。stop() 让它当帧退出；此后没人
    // 再删它，泄漏到进程结束由 OS 回收——退出路径上这是最便宜的收尾。
    if (m_scroll)
        m_scroll->stop();
    // 退出前等在途写盘收尾：QImage::save 途中进程被走人会留下截断的图片
    // （文件在、内容坏），比报错更难查。放在 ~App 体内而非依赖 QObject 析构，
    // 此时各子系统的 QObject 都还活着。
    output::drain();
}

// ---- 截图入口 ----

void App::startFullCapture() {
    memtrim::touch();
    if (m_selector->isActive()) {
        // 框选界面开着时按全屏热键：先收起遮罩再抓，否则会把半透明遮罩
        // 和选框一起抓进图里（cancel 内部会 close 掉全部覆盖窗）。
        m_selector->cancel();
        // close() 同步藏掉 HWND，但 DWM 合成画面是异步更新的：紧贴着抓屏
        // 会读到仍含遮罩的上一帧。留出一次合成周期再动手。
        QThread::msleep(60);
    }
    const QRect bounds = capture::virtualBounds();
    const auto [img, map] = capture::grabDesktop();
    if (img.isNull()) {
        m_tray->notify(QStringLiteral("截图失败"), QStringLiteral("无法抓取屏幕画面"));
        return;
    }
    onCaptured(img, img, {}, QStringLiteral("copy"), QPointF(bounds.left(), bounds.top()),
               map.dprOf(QRectF(bounds)));
}

void App::startCapture(const QString& mode) {
    if (mode == QLatin1String("full")) {
        startFullCapture();
        return;
    }
    if (mode == QLatin1String("window")) {
        startWindowCapture();
        return;
    }
    memtrim::touch();
    m_selector->start(mode);
}

void App::startScrollCapture(const QImage& first, const QRect& regionAbs,
                             const QRectF& regionLogical, double dpr) {
    if (m_scroll) {
        // 上一场还在滚动：不能 deleteLater —— run() 还在线程池里裸持 this，
        // 删了就是 use-after-free。stop() 让它当帧退出并以「已取消」发
        // finished 自毁（那条 finished 会被 onScrollFinished 的 sender 守卫丢掉）。
        m_scroll->stop();
        m_scroll.clear();
    }
    // 不挂 parent：Capture 的 run() 在私有线程池里裸持 this（最多再跑一个轮询
    // 间隔才察觉 stop），挂了 parent 的话 ~App 的递归析构会当场删掉它 = 池线程
    // use-after-free。不挂则对象活到 finished 自毁（正常路径）或进程结束（退出
    // 路径，~App 会先 stop() 让 run() 当帧退出）。finished 的接收方析构时 Qt
    // 自会断连，不悬空。
    m_scroll = new scroll::Capture(first, regionAbs, regionLogical, dpr);
    connect(m_scroll, &scroll::Capture::finished, this, &App::onScrollFinished);
    m_tray->notify(QStringLiteral("长截图"),
                   QStringLiteral("手动滚动页面即可拼接，空格或「完成」出图，Esc 取消"));
    m_scroll->start();
}

void App::onScrollFinished(const QImage& img, int frames, bool cancelled, double dpr,
                           const QString& reason) {
    // Capture 经 finished 自毁（start() 里接了 deleteLater），这里不再删。
    // 被 stop() 掉的旧场也会发 finished：只认 sender 是当前这一场的收尾，
    // 防止旧场的结果盖到新场上。
    if (m_scroll != qobject_cast<scroll::Capture*>(sender()))
        return;
    m_scroll.clear();
    if (cancelled) {
        // 中途读不到屏幕（锁屏/UAC/独占全屏）是失败不是完成，得说清楚；
        // 早先这种半截内容也按「完成」出货，气泡报成功、长图缺一截。
        m_tray->notify(QStringLiteral("长截图"),
                       reason == QLatin1String("grabfail")
                           ? QStringLiteral("失败：屏幕画面读不出来了（锁屏或被独占），已中止")
                           : QStringLiteral("已取消"));
        return;
    }
    if (img.isNull()) {
        m_tray->notify(QStringLiteral("长截图"),
                       QStringLiteral("失败：没有捕获到新内容（页面没滚动，或滚动幅度太小）"));
        return;
    }
    m_tray->notify(QStringLiteral("长截图"),
                   QStringLiteral("完成：%1 帧，%2×%3，已进入标注模式")
                       .arg(frames)
                       .arg(img.width())
                       .arg(img.height()));
    // 长图收尾：按「适应屏幕」缩放贴上屏并直接进入标注模式——贴图窗
    // 的标注器有全套工具，滚轮缩放适合看长图；同时已进截图历史
    onCaptured(img, img, {}, QStringLiteral("scroll_result"), clipPivot(), dpr);
}

void App::startWindowCapture() {
    memtrim::touch();
    if (m_selector->isActive()) {
        m_selector->cancel();  // 与全屏截图同款：先收起遮罩再动手
        QThread::msleep(60);   // 同款留一次 DWM 合成周期，别把遮罩残影拍进去
    }
    const auto fg = win32::foregroundWindow();
    bool found = false;
    rcore::WindowRect rect{};
    for (const rcore::WindowRect& wr : rcore::visibleWindowRects({})) {
        if (wr.hwnd == qintptr(fg)) {
            rect = wr;
            found = true;
            break;
        }
    }
    if (!found) {
        m_tray->notify(QStringLiteral("窗口截图"), QStringLiteral("无法获取前台窗口"));
        return;
    }
    // 前台是任务栏/桌面等系统壳窗口时截了也是垃圾：提示切到目标窗口再按
    const QString cls = win32::windowClassName(reinterpret_cast<win32::Hwnd>(fg));
    if (cls == QLatin1String("Shell_TrayWnd") || cls == QLatin1String("Shell_SecondaryTrayWnd") ||
        cls == QLatin1String("Progman") || cls == QLatin1String("WorkerW") ||
        cls == QLatin1String("#32768")) {
        m_tray->notify(QStringLiteral("窗口截图"),
                       QStringLiteral("当前前台是系统界面，请切换到目标窗口后按快捷键"));
        return;
    }
    const auto [img, map] = capture::grabDesktop();
    if (img.isNull()) {
        m_tray->notify(QStringLiteral("截图失败"), QStringLiteral("无法抓取屏幕画面"));
        return;
    }
    const QRect clipped = QRect(QPoint(rect.l - map.origin.x(), rect.t - map.origin.y()),
                                QSize(rect.r - rect.l, rect.b - rect.t))
                              .intersected(img.rect());
    if (clipped.isEmpty()) {
        m_tray->notify(QStringLiteral("窗口截图"), QStringLiteral("窗口不在屏幕范围内"));
        return;
    }
    const QRectF logical = map.logicalOfAbsRect(rect.l, rect.t, rect.r, rect.b);
    const QImage shot = img.copy(clipped);
    onCaptured(shot, shot, {}, QStringLiteral("copy"), logical.topLeft(), map.dprOf(logical));
}

void App::archive(const QImage& base, const QJsonArray& doc, double dpr) {
    m_history->add(base, dpr, doc);
    historyCompressPending();  // 原图即刻交工作线程压 PNG，不等空闲（削内存峰值）
}

void App::autoSaveCopy(const QImage& img) {
    if (!config::getBool(QStringLiteral("Output/auto_save")))
        return;
    // 失败必须出声：这条路径平时不给任何回执，目录失效（手改 ini 填了不存在的盘、
    // 或者只读目录）时用户会一直以为「在自动存」，直到某天发现文件根本不在。
    output::saveImageAsync(img, {}, {}, [this](const QString& path) {
        if (path.isEmpty())
            emit notifyRequested(QStringLiteral("自动保存失败"),
                                 QStringLiteral("检查 设置 → 输出 里的默认保存目录"));
    });
}

void App::onSaveLanded(const QImage& base, const QJsonArray& doc, double dpr) {
    archive(base, doc, dpr);
    m_selector->finishPending();
}

void App::onCaptured(const QImage& img, const QImage& base, const QJsonArray& doc,
                     const QString& action, const QPointF& topLeft, double dpr) {
    // 只有「真的产出了成品」才留档：复制 / 贴图 / 保存 / 另存为成功 / 长截图。
    // 识别文字、识别表格 的结果只进剪贴板，原图不值得占历史名额，更不该在识别
    // 失败、另存为取消这种「什么都没发生」的时候悄悄塞一条进历史。
    if (action == QLatin1String("save")) {
        archive(base, doc, dpr);
        // 快速保存：直接写入默认目录（文件名模板 + 自动去重），气泡带回执
        output::saveImageAsync(img, {}, {},
                               [this](const QString& path) { notifySaved(path); });
        return;
    }
    if (action == QLatin1String("save_as")) {
        // 另存为：弹对话框自行选位置；写盘挪后台线程防卡顿。留档和收尾都等到确认
        // 写成功——取消就什么都不做，选区与已画标注原样留着继续编辑。
        //
        // 会话现在不 teardown，覆盖层因此还开着：它是「置顶 + Tool」窗，而且 start()
        // 里 grabKeyboard() 抓走了键盘（只有 teardown 会放）。不先放开抓取、不把对话框
        // 挂到一个覆盖层上当爹，用户既打不了文件名、对话框还可能被遮罩压在下面。
        m_selector->releaseKeyboardGrab();
        output::saveImageDialogAsync(
            img, m_selector->dialogParent(),
            [this, base, doc, dpr](const std::optional<QString>& path) {
                if (!path) {
                    // 取消是主线程同步回调（没起 worker），可以直接把抓取收回来
                    m_selector->refocusOverlay();
                    emit notifyRequested(QStringLiteral("保存已取消"), QString());
                    return;
                }
                // 回调在写盘线程，经信号回主线程留档+收尾
                emit saveLanded(base, doc, dpr);
                notifySaved(*path);
            });
        return;
    }
    if (action == QLatin1String("ocr")) {
        // 识别文字：本地 PP-OCR 在后台线程跑（一次几十~几百毫秒）
        ocr::recognizeAsync(img, [this](const std::optional<QString>& text) {
            ocr::publishText(text, [this](const QString& title, const QString& body) {
                m_tray->notify(title, body);
            });
            // 成功也不调 finishPending()：**不收选区**。识别结果已经进剪贴板，
            // 用户多半还想接着标注；早先成功就 teardown，把刚框的选区连同已画的
            // 标注一起收掉，只能去历史里重新贴。失败时同样留着，理由见
            // publishText 里的「没识别到就别让用户白框一场」。
        });
        return;
    }
    if (action == QLatin1String("table")) {
        // 识别表格：版面 + 结构 + 单元格文字三趟，比识字更重，同样只在后台线程跑
        ocr::recognizeTablesAsync(img, [this](const std::optional<QVector<rcore::OcrTable>>& tables) {
            ocr::publishTables(tables, [this](const QString& title, const QString& body) {
                m_tray->notify(title, body);
            });
            // 同「识别文字」：不收选区，表格已经进剪贴板（可粘进 Excel/WPS），
            // 选区与已画标注留着继续用
        });
        return;
    }
    if (action == QLatin1String("scroll_result")) {
        // 长截图产物：贴图 + 适应屏幕缩放 + 直接进入标注模式。
        // 必须留档——它是唯一「重截一次代价极高」的产物，关掉贴图就没法再拿回来。
        archive(base, doc, dpr);
        autoSaveCopy(img);
        PinWindow* w = m_pins->pinImage(img, topLeft, dpr);
        if (w) {
            w->fitToScreen();
            w->setAnnotate(true);
        }
        return;
    }
    if (action == QLatin1String("pin")) {
        archive(base, doc, dpr);
        autoSaveCopy(img);
        // ref_dpr = 这次截图实际用的比例：贴图与截图所见严格 1:1。混合 DPI 下
        // 落点屏比例 ≠ 该比例时，若按落点屏算会把图放大/缩小 dpr 倍。
        m_pins->pinImage(img, topLeft, dpr);
        return;
    }
    archive(base, doc, dpr);
    autoSaveCopy(img);
    // 用 setImage(QImage) 而非 setPixmap(QPixmap)：QImage 是纯内存数据，
    // 不持有 HBITMAP 原生句柄，可避免 Windows 在别处读取剪贴板
    // （WM_RENDERFORMAT）时因句柄失效而崩溃。
    QApplication::clipboard()->setImage(img);
    m_tray->notify(QStringLiteral("ZPin"), QStringLiteral("截图已复制到剪贴板"));
}

// ---- 贴图 / 历史（托盘菜单回调） ----

void App::togglePins() {
    const bool hidden = m_pins->toggleVisible();
    m_tray->setPinsHidden(hidden);
    if (!m_pins->allWindows().isEmpty()) {
        m_tray->notify(QStringLiteral("贴图"),
                       hidden ? QStringLiteral("已全部隐藏（可从此菜单显示）")
                              : QStringLiteral("已全部显示"));
    }
}

void App::restoreClickable() {
    // 托盘一键取消全部鼠标穿透（穿透后贴图收不到右键，这里兜底）
    const int n = m_pins->restoreClickable();
    m_tray->notify(QStringLiteral("贴图"),
                   n ? QStringLiteral("已恢复 %1 张贴图可点击").arg(n)
                     : QStringLiteral("没有处于鼠标穿透状态的贴图"));
}

void App::clearHistory() {
    m_history->clear();
    m_tray->notify(QStringLiteral("历史"), QStringLiteral("已清空截图历史"));
}

void App::openHistoryWall() {
    // 墙只建一次、关掉只是藏起来：下次打开仍是上次的滚动位置，不必重新解码缩略图
    if (!m_wall) {
        m_wall = new HistoryWall(m_history);
        connect(m_wall, &HistoryWall::repinRequested, this, &App::repinFromHistory);
        connect(m_wall, &HistoryWall::saveRequested, this, &App::saveFromHistory);
        connect(m_wall, &HistoryWall::copyRequested, this, &App::copyFromHistory);
        connect(m_wall, &HistoryWall::deleteRequested, this, &App::deleteFromHistory);
        connect(m_wall, &HistoryWall::clearRequested, this, &App::clearHistory);
    }
    m_wall->open();
}

void App::repinFromHistory(int id) {
    // 「贴图化」= 干净底图 + 标注文档一起上屏：看着与原图一致，但笔迹是活的
    // ——进标注模式就能挪、能改、能删，收工回写这条历史（缩略图跟着变）。
    // 必须用底图而不是历史的合成结果：合成图里标注已经烘进像素，再叠一层就是两笔。
    const QImage base = m_history->baseOf(id);
    if (base.isNull())
        return;
    const double ref = m_history->dprOf(id);  // 每条历史记住自己截图时用的比例
    // 落点：光标多半停在托盘/墙按钮上（屏幕右下角），按光标落会把贴图甩进
    // 角落。改成落在光标所在屏的中央，再按已有贴图数级联错开——连续贴图化
    // 不会一张张叠死在同一个点上。
    QScreen* scr = QGuiApplication::screenAt(QCursor::pos());
    if (!scr)
        scr = QGuiApplication::primaryScreen();
    QPointF pivot = clipPivot();
    if (scr) {
        const QRect av = scr->availableGeometry();
        const int cascade = int(m_pins->allWindows().size() % 8) * 24;
        pivot = QPointF(av.center().x() + cascade - 60, av.center().y() + cascade - 60);
    }
    // 物理像素 -> 逻辑坐标需除以 ref，避免 HiDPI 下贴到中心偏右下
    PinWindow* w = m_pins->pinImage(base,
                                    QPointF(pivot.x() - base.width() / 2.0 / ref,
                                            pivot.y() - base.height() / 2.0 / ref),
                                    ref);
    if (!w)
        return;
    const QJsonArray doc = m_history->docOf(id);
    w->setAnnotateWithDoc(doc, /*activate=*/false);
    // 收工回写：贴图隐藏/关闭/退出标注模式时把最新文档写回这条历史（没动过
    // 就不写）。旋转/翻转烘过像素的贴图不会回调，历史保持原样。
    QPointer<HistoryStore> store = m_history;
    w->setDocSaveCallback([store, id, doc](const QJsonArray& latest) {
        if (!store || latest == doc)
            return;
        store->updateDoc(id, latest);
    });
}

void App::saveFromHistory(int id) {
    // 直接写默认目录，与工具条的「保存」同一条路。历史条目只有
    // 「09-12 14:03  1920×1080」一行文字，看不见内容的情况下不该再让用户挑路径；
    // 要换格式或换位置，重贴之后在贴图窗的右键菜单里做——那时图就在眼前。
    const QImage img = m_history->get(id);
    if (img.isNull())
        return;
    output::saveImageAsync(img, {}, {}, [this](const QString& path) { notifySaved(path); });
}

void App::copyFromHistory(int id) {
    // 与「保存」同一张图：get() 返回用户当时看到的合成结果（含标注）
    const QImage img = m_history->get(id);
    if (img.isNull())
        return;
    QGuiApplication::clipboard()->setImage(img);
    emit notifyRequested(QStringLiteral("已复制到剪贴板"), {});
}

void App::deleteFromHistory(int id) {
    // 只删这条历史（连档上的三个文件一起）；已贴出去的贴图有自己的内存副本，
    // 不受影响
    m_history->remove(id);
}

// 后台保存结果回调（子线程调用），经信号回主线程弹气泡。
void App::notifySaved(const QString& path) {
    emit notifyRequested(path.isEmpty() ? QStringLiteral("保存失败") : QStringLiteral("已保存"),
                         path);
}

// ---- 设置 ----

void App::openPrefs() {
    if (m_prefs && m_prefs->isVisible()) {
        m_prefs->raise();
        m_prefs->activateWindow();
        return;
    }
    m_prefs = new PreferencesDialog(
        [this] { return applyHotkeys(); }, [this] { applyFont(); },
        [this] { applyLogLevel(); }, [this](bool on) { setShortcutsDisabled(on); },
        [this](bool suspended) { m_hotkeys->setSuspended(suspended); },
        m_updateFlow);
    m_prefs->show();
}

// ---- 热键 ----

void App::setupHotkeys() {
    m_hotkeys = new hotkey::Manager(this);
    const QMap<QString, bool> results = m_hotkeys->applyBindings(m_accels);
    QStringList failedLabels;
    for (const QString& action : hotkey::actionOrder()) {
        if (results.contains(action) && !results.value(action))
            failedLabels << hotkey::actionLabel(action);
    }
    if (!failedLabels.isEmpty()) {
        m_tray->notify(QStringLiteral("热键注册失败"),
                       failedLabels.join(QStringLiteral("、"))
                           + QStringLiteral(" 被占用，可在 设置 → 快捷键 中更改"));
    }
}

void App::dispatch(const QString& action) {
    if (action == QLatin1String("capture_full"))
        startCapture(QStringLiteral("full"));
    else if (action == QLatin1String("capture"))
        startCapture(QStringLiteral("normal"));
    else if (action == QLatin1String("capture_window"))
        startCapture(QStringLiteral("window"));
    else if (action == QLatin1String("toggle_pins"))
        togglePins();   // 菜单文字的同步在 togglePins 里做，不必绕托盘信号
}

void App::setShortcutsDisabled(bool disabled) {
    if (!m_hotkeys)
        return;
    m_hotkeys->setDisabled(disabled);
    config::set(QStringLiteral("Hotkeys/disabled"), disabled);
    config::sync();
    m_tray->setShortcutsDisabled(disabled);
}

// ---- 首选项回调 ----

QMap<QString, bool> App::applyHotkeys() {
    QMap<QString, QString> map;
    for (const QString& action : hotkey::actionOrder())
        map.insert(action, config::hotkeyAccel(action));
    // 托盘菜单右侧的快捷键提示、帮助弹窗随之显示新键
    m_tray->setAccels(map);
    return m_hotkeys ? m_hotkeys->applyBindings(map) : QMap<QString, bool>();
}

void App::applyFont() {
    const auto [name, size] = defaults::fontTuple(config::getStr(QStringLiteral("Interface/font")));
    // name 可能是「A,B,C」的字体栈（默认那档，见 defaults::kDefaultFontFamily）：
    // 这个构造器把整串交给 setFamilies，Qt 逐字符挑第一个有该字形的家族——拉丁与
    // 数字落 Segoe UI Variable、中文落 YaHei，两个字形体系各用各的字形。
    // 用户在设置页下拉里选过之后写回的是单一家族，栈长度 1，行为与从前完全一致。
    qApp->setFont(QFont(name.split(QLatin1Char(','), Qt::SkipEmptyParts), size));
}

void App::applyLogLevel() {
    log::setDebug(config::getStr(QStringLiteral("General/log_level")) == QStringLiteral("详细"));
}

// ---- 接线与运行 ----

void App::wire() {
    connect(m_pins, &PinManager::notify, m_tray, &TrayController::notify);

    connect(m_selector, &SelectionController::captured, this, &App::onCaptured);
    connect(m_selector, &SelectionController::scrollRequested, this, &App::startScrollCapture);
    connect(m_tray, &TrayController::captureRequested, this, &App::startCapture);
    connect(m_tray, &TrayController::togglePinsRequested, this, &App::togglePins);
    connect(m_tray, &TrayController::restoreClickableRequested, this, &App::restoreClickable);
    connect(m_tray, &TrayController::historyWallRequested, this, &App::openHistoryWall);
    connect(m_tray, &TrayController::restartRequested, this, &App::restart);

    setupHotkeys();
    connect(m_hotkeys, &hotkey::Manager::fired, this, [this](const QString& action) {
        memtrim::touch();
        dispatch(action);
    });
    // 第二实例唤起：在本进程弹一条气泡，告诉他已经在跑了
    connect(m_hotkeys, &hotkey::Manager::showRequested, this,
            [this] { m_tray->notify(QStringLiteral("ZPin"), QStringLiteral("ZPin 已在运行")); });
    connect(this, &App::notifyRequested, m_tray, &TrayController::notify);
    connect(this, &App::saveLanded, this, &App::onSaveLanded);

    connect(m_tray, &TrayController::shortcutsDisabledToggled, this,
            &App::setShortcutsDisabled);
    // 启动恢复上次的禁用状态：反注册上面刚注册的热键并同步托盘文字
    if (config::getBool(QStringLiteral("Hotkeys/disabled")))
        setShortcutsDisabled(true);

    connect(m_tray, &TrayController::prefsRequested, this, &App::openPrefs);

    // 「发现新版本」气泡可点击：跳到 设置 → 关于（其它气泡没有点击行为）
    connect(m_tray, &TrayController::bubbleClicked, this, [this] {
        openPrefs();
        if (m_prefs)
            m_prefs->showAboutPage();
    });
}

void App::restart() {
    // 以原命令行重启进程，当前实例退出。
    // 必须先放开单实例互斥体再拉新进程：新进程起来时本进程未必已经退干净，
    // 那时它会拿到 ERROR_ALREADY_EXISTS 而直接退出 —— 「重新启动」变成「直接退出」。
    // （早先这里是先 startDetached 再 release，与这段注释说的正好相反。）
    win32::releaseSingleInstance();
    QProcess::startDetached(QCoreApplication::applicationFilePath(), {},
                            QCoreApplication::applicationDirPath());
    qApp->quit();
}

int App::run() {
    // 自启对账：绿色版换目录（或整个文件夹改名）后，Run 键里存的还是老路径 ——
    // 开机不启动，而设置页那个勾选读的是 config，照样显示「已开启」。
    // 只在「配置说要开、注册表里的路径却不是当前 exe」时重写一次。
    // 不能无条件 setEnabled(true)：那会覆盖用户在 Windows「设置 → 启动应用」里
    // 主动关掉的开关（那里关掉不删 Run 值，只写 StartupApproved 标记）。
    if (config::getBool(QStringLiteral("General/autostart")) && !startup::isEnabled()) {
        startup::setEnabled(true);
        log::info("zpin", QStringLiteral("开机自启：已按当前路径重写 Run 键 %1")
                              .arg(QCoreApplication::applicationFilePath()));
    }
    // 空闲整理：先把历史里的原图压成 PNG，再裁工作集
    memtrim::registerCleaner(historyCompressPending);
    if (config::getBool(QStringLiteral("General/keep_responsive")))
        memtrim::install();
    QStringList bound;
    for (const QString& action : hotkey::actionOrder()) {
        if (!m_accels.value(action).isEmpty())
            bound << QStringLiteral("%1=%2").arg(action, m_accels.value(action));
    }
    log::info("zpin", QStringLiteral("ZPin 启动：热键{%1}，自启=%2")
                          .arg(bound.join(QStringLiteral(", ")),
                               startup::isEnabled() ? QStringLiteral("True")
                                                    : QStringLiteral("False")));
    // 事件循环起来后立刻预热工具栏图标：SVG 解析与 QSvgRenderer 的首次初始化
    // 提前付掉，第一次框选时工具栏就不用现渲染二十多个图标了。
    QTimer::singleShot(0, this, [] { warmToolbarIcons(); });
    // 空闲预热 OCR 引擎（模型加载约 150ms）：第一次「识别文字/脱敏」就是稳态速度
    QTimer::singleShot(2500, this, [] {
        QImage warm(64, 64, QImage::Format_ARGB32_Premultiplied);
        warm.fill(Qt::GlobalColor::white);
        ocr::recognizeAsync(warm, [](const std::optional<QString>&) {});
    });
    // 稍等托盘与热键都稳了再静默检查更新（内部有开关与 24h 节流）
    QTimer::singleShot(4000, this, [this] { m_updateFlow->autoCheck(); });
    return qApp->exec();
}

}  // namespace zpin
