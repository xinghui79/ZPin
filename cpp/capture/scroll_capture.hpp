// 滚动长截图驱动 —— **纯手动滚动**：程序不合成任何滚轮输入、也不挪光标，只是
// 盯着选区「等画面稳定 → 抓一帧 → 交给 Rust 拼接会话」。用户用滚轮、拖滚动条、
// PgDn、触摸板哪种方式滚都行；不滚的时候画面判为静止，不追加内容也不收工。
// 主线程维护两块 UI：屏幕遮罩（整屏压暗、捕获区挖空，透出真实窗口内容在滚）与
// 贴在选区旁的实时预览面板（缩略图 + 进度 + 完成/取消按钮）。
// 退出条件：点「完成」/ 按空格 / 按 Esc / 画布达到高度上限。
// 遮罩挖空比捕获区外扩 1px、描边再外移 1.5px，面板被 WDA 排除出抓屏——所有 UI
// 都不会进帧；遮罩对鼠标完全穿透，用户在洞里的滚动才能落到下层窗口。
#pragma once

#include <QImage>
#include <QLabel>
#include <QObject>
#include <QRect>
#include <QRectF>
#include <QWidget>

#include <atomic>

class QKeyEvent;
class QScreen;

namespace zpin {
namespace scroll {

// 屏幕遮罩：per-screen 一块，压暗 + 捕获区挖空 + 洞外描边；鼠标完全穿透。
class ScreenMask : public QWidget {
    Q_OBJECT

public:
    // holeLogical：捕获区的全局逻辑矩形（调用方已外扩过安全边距）。
    ScreenMask(QScreen* scr, const QRectF& holeLogical);

protected:
    void showEvent(QShowEvent* ev) override;
    void paintEvent(QPaintEvent* ev) override;
    // 接收本窗注册为 INPUTSINK 后投递来的 WM_INPUT：滚轮计数交给捕获线程。
    // 遮罩窗收 WM_INPUT 与鼠标穿透无关（Raw Input 不走命中测试）。
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    QRectF m_hole;  // 挖空矩形（本屏局部坐标）。不与本屏相交时它落在屏外，
                    // subtracted 等于没挖 -> 整屏压暗
};

// 贴选区的实时预览面板：状态行 + 拼接缩略图 + 完成/取消按钮。
class Progress : public QWidget {
    Q_OBJECT

public:
    explicit Progress(QWidget* parent = nullptr);

public slots:
    // idle = 画面已经不动了（提示「点完成出图」，不自动收工）；
    // missed = 至今有几帧「动了但没对上」（一次滚得太远，中间内容没接上）。
    void onUpdate(int frames, int height, bool idle, int missed);
    void onPreview(const QImage& thumb);

signals:
    void finishClicked();
    void cancelClicked();

protected:
    void keyPressEvent(QKeyEvent* ev) override;

private:
    QLabel* m_status = nullptr;
    QLabel* m_thumb = nullptr;
};

class Capture : public QObject {
    Q_OBJECT

public:
    // first: 干净的首帧（选区确认时的底图裁剪，画布物理像素）；
    // regionAbs: 区域的绝对物理矩形（后续帧照此抓取）；
    // regionLogical: 区域的全局逻辑矩形（遮罩挖空与面板定位用）；
    // dpr: 区域所在屏比例（出图后贴图/缩放要用）。
    Capture(const QImage& first, const QRect& regionAbs, const QRectF& regionLogical,
            double dpr, QObject* parent = nullptr);

    // 创建遮罩与预览面板并丢到线程池开始跑；结果经 finished 队列回主线程。
    void start();

    // 请求取消：当前帧处理完即退出，丢弃结果，仍以「已取消」发 finished 自毁。
    // 对象一次性，无需复位。
    void stop() { m_stop = true; }
    // 完成：用已捕获内容出图（预览面板「完成」按钮 / 空格走这里）。
    void finishNow() { m_finish = true; }

signals:
    void progress(int frames, int height, bool idle, int missed);
    void previewReady(const QImage& thumb);
    // reason：收尾原因记号（finish/space/esc/grabfail/cap/stopped），调用方
    // 靠它区分「用户取消」和「抓屏失败」——两者对用户的说法不能一样。
    void finished(const QImage& img, int frames, bool cancelled, double dpr,
                  const QString& reason);

    private:
    void run();

    QImage m_first;
    QRect m_region;
    QRectF m_regionLogical;
    double m_dpr = 1.0;
    // 预览面板的句柄（主线程在 start() 里取）。抓帧前要用它把该窗从屏幕捕获中
    // 排除，否则它会被逐帧拍进长图。win32::Hwnd 是不完整类型，这里用
    // std::atomic<void*> 存/取（只用它的数值身份，不解引用）。
    std::atomic<void*> m_progressHwnd{nullptr};
    // run() 在工作线程跑，progress/previewReady 交回主线程刷新；停止/完成由原子标志
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_finish{false};
};

}  // namespace scroll
}  // namespace zpin
