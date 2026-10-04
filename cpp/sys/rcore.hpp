// Rust 核心桥封装 —— C++ 侧唯一的 cxx 头文件引用点。
// 抓屏 / 高斯模糊 / UIA 元素 / 可见窗口矩形 / 文字识别，均转成 Qt 类型。
#pragma once

#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>
#include <memory>
#include <optional>

namespace zpin::rcore {

// 抓取绝对物理矩形 (x,y,w,h)，返回 BGRA→RGB32 的物理像素图。
QImage captureBgra(int x, int y, int w, int h);

// 同 captureBgra，但不带 CAPTUREBLT：滚动长截图轮询抓帧用（避免分层窗
// 重绘引发的光标闪烁；代价是区域内的分层窗不入帧）。
QImage captureBgraPlain(int x, int y, int w, int h);

// 三轮盒滤波近似高斯：ARGB32_Premultiplied 进，同尺寸新图出；失败返回空图。
QImage blurImage(const QImage& img);

// PP-OCR 识别图里的文字（同步，几十~几百毫秒，别在 UI 线程调）。
// 模型缺失或识别失败返回 nullopt。
std::optional<QString> ocrText(const QImage& img);

// 一行识别结果：图像局部像素外接框 + 文本。
struct OcrBox {
    QRect rect;
    QString text;
};

// 同 ocrText，但带每行外接框（一键脱敏用）；失败返回 nullopt。
std::optional<QVector<OcrBox>> ocrBoxes(const QImage& img);

// 一张表格：原始 HTML（带 tr/td 与 colspan/rowspan）+ 按行列对齐的 TSV。
struct OcrTable {
    QString html;
    QString text;
};

// 识别图里的表格（版面模型找出表格区域 → 框内文字按投影分行分列）。
// 没有表格返回空 QVector；模型缺失或推理失败返回 nullopt。同步调用，别在 UI 线程用。
std::optional<QVector<OcrTable>> ocrTables(const QImage& img);

// 视觉内容块：从 src 的 seed 处按「与四周背景不同的连通块」长出包围盒（返回源图
// 像素坐标）。trust 的 4 个位表示取景框 左/上/右/下 是否为真实边界（窗口边/屏幕边），
// 块被人为切出来的边切断时不吸附。merge 是粘行膨胀半径（物理 px）。认不出返回 nullopt。
std::optional<QRect> contentRect(const QImage& src, const QPoint& seed, int merge, int trust);

struct ElementInfo {
    QRect rect;       // 绝对物理像素
};

// UIA ElementFromPoint；无命中/离屏/UIA 失败返回 nullopt。
std::optional<ElementInfo> elementFromPoint(int x, int y);

// 可见顶层窗口物理矩形 (hwnd, l, t, r, b)，z 序最上在前；exclude 里的 hwnd 跳过。
struct WindowRect {
    qintptr hwnd;
    int l, t, r, b;
};
QVector<WindowRect> visibleWindowRects(const QVector<qintptr>& exclude);

// ---- 滚动长截图拼接（Rust 侧维护画布，见 rust/src/stitch.rs） ----

class ScrollStitcher {
public:
    struct Step {
        int offset = 0;       // 本帧顶部被消耗掉的行数（含粘性头，物理 px）；0 = 画面没滚动/没对上
                              // 注意它不等于「与画布的重叠像素」：重叠还要再减去粘性头
        int canvasHeight = 0; // 拼接后的画布高度
        bool done = false;    // 画布达到高度上限（收工与否由调用方决定）
        bool frozen = false;  // 本帧与上一帧几乎相同 = 画面已静止，可继续滚下一屏
        int sticky = 0;       // 顶部被判为固定栏（粘性头）的高度，物理像素；不参与对齐
        bool rebased = false; // 接缝处发现页面在两次抓帧之间自己重排，已把画布重叠带
                              // 用本帧的最新渲染重铺（文字不再被切在两版渲染中间）
    };

    // width/frameH 是帧尺寸（物理像素），maxH 是画布高度上限。
    ScrollStitcher(int width, int frameH, int maxH);
    ~ScrollStitcher();
    ScrollStitcher(const ScrollStitcher&) = delete;
    ScrollStitcher& operator=(const ScrollStitcher&) = delete;

    // 追加一帧 BGRA（宽高须与构造一致），返回对齐结果。
    Step push(const QImage& frame);
    // 取走拼接完成的画布（BGRA→RGB32）；之后会话为空。
    QImage takeCanvas();
    // 当前进度缩略图（等比缩到 thumbWidth 宽、高不超过 maxThumbHeight）。
    QImage preview(int thumbWidth, int maxThumbHeight) const;
    int canvasHeight() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    int m_width = 0;
};

// ---- 自动更新的网络层（同步阻塞，别在 UI 线程调） ----

// HTTP(S) GET 整个响应体；网络失败或非 2xx 抛 rust::Error（what() 是原因）。
QByteArray httpGet(const QString& url, const QString& headers);

// 把 URL 内容流式写入本地文件（覆盖写），返回字节数；失败抛 rust::Error。
qint64 downloadToFile(const QString& url, const QString& dest, const QString& headers);

}  // namespace zpin::rcore
