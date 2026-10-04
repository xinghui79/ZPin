//! ZPin 核心 Rust 库 —— C++/Qt6 界面层通过 cxx 桥接调用。
//!
//! 只承接 C++ 侧跑不快的纯计算和 Win32 交互深的部分：热点像素运算、
//! 屏幕抓取、UIA 元素吸附、可见窗口矩形枚举。界面、事件、绘制全部留在
//! C++/Qt 侧。所有桥接项集中在本文件的唯一 bridge 里声明，实现分散在
//! 各子模块，由本文件的包装函数转发。

#[cxx::bridge(namespace = zpin)]
mod ffi {
    /// UIA 命中元素：物理像素矩形。
    /// 早先还带一个 control_type（控制类型 ID），但 C++ 侧从收到就 Q_UNUSED
    /// 掉了——它一路穿过 cxx 结构体和每查询一次的跨线程信号，没有任何人读。
    /// 「中文名由 C++ 侧映射」这个说法也只是当初的设想，从没实现。真要用元素
    /// 分级（按钮/编辑框/链接各自不同吸附策略）时再加回来。
    #[derive(Debug)]
    struct ElementInfo {
        x: i32,
        y: i32,
        w: i32,
        h: i32,
    }

    /// 一行识别结果：图像局部像素外接框 + 文本（一键脱敏用）。
    #[derive(Debug)]
    struct OcrTextBox {
        x: i32,
        y: i32,
        w: i32,
        h: i32,
        text: String,
    }

    /// 一张识别出来的表格：原始 HTML（给剪贴板的 text/html）+ 行列对齐的 TSV（text/plain）。
    #[derive(Debug)]
    struct OcrTable {
        html: String,
        text: String,
    }

    /// 滚动长截图的单帧对齐结果。
    #[derive(Debug)]
    struct StitchStep {
        /// 本帧顶部被消耗掉的行数（含粘性头，物理 px；0 = 画面没有滚动 / 没对上）。
        /// 与「和画布底部的重叠像素」不是一回事：重叠还要再减去粘性头那一段。
        offset: i32,
        /// 追加后的画布高度。
        canvas_h: i32,
        /// 画布达到高度上限（「要不要收工」由调用方决定）。
        done: bool,
        /// 本帧与上一帧几乎逐像素相同 —— 画面已经静止（滚轮没生效，或内容
        /// 还没加载出来 / 动画已停）。C++ 侧据此**轮询到画面稳定**再抓下一帧，
        /// 而不是拍一个固定 sleep 就抓。这个判据本来只被当作「收工」条件
        /// （no_move >= 2），不往外传，驱动端只能盲等固定毫秒数：等少了抓到
        /// 动画中间态（拼接出来发糊），等多了整段操作明显发木。
        frozen: bool,
        /// 本帧顶部被判为粘性头（固定栏）的原像素高度。驱动端要靠它把「每帧
        /// 滚多少」算在**可用内容高**上：粘性头占掉的高度滚不过去，早先按整帧
        /// 高算目标步长会迈得比可匹配范围还大，于是每帧都 offset=0 直接收工。
        sticky_px: i32,
        /// 本帧接缝处发现页面自己重排（两次抓帧之间内容重新排版），已把画布
        /// 重叠带截掉、用本帧的最新渲染重铺 —— 文字不再被切在两版渲染的接缝
        /// 上。C++ 侧据此记日志（重排是页面行为，不是拼接失败）。
        rebased: bool,
    }

    /// 一个可见顶层窗口的物理像素矩形（z 序最上在前）。
    #[derive(Debug)]
    struct WindowRect {
        hwnd: isize,
        l: i32,
        t: i32,
        r: i32,
        b: i32,
    }

    extern "Rust" {
        /// 三轮盒滤波近似高斯：BGRA(premultiplied) 进，返回同尺寸新缓冲。
        /// radius 建议取 min(w,h)/64 上下，可按手感调；参数非法返回空缓冲。
        fn blur_bgra(data: &[u8], width: usize, height: usize, radius: usize) -> Vec<u8>;

        /// 抓取虚拟桌面 (x,y,w,h) 物理像素区域，返回 BGRA 自上而下缓冲。
        fn capture_bgra(x: i32, y: i32, w: i32, h: i32) -> Vec<u8>;

        /// 同 capture_bgra，但不带 CAPTUREBLT（滚动长截图轮询抓帧用，
        /// 避免分层窗重绘引发的光标闪烁）。
        fn capture_bgra_plain(x: i32, y: i32, w: i32, h: i32) -> Vec<u8>;

        /// 吸附 (x,y) 处的界面元素；无命中或 UIA 失败时返回 Err。
        fn element_from_point(x: i32, y: i32) -> Result<ElementInfo>;

        /// 按 z 序枚举可见顶层窗口的物理像素矩形；exclude 里的 hwnd 跳过。
        fn visible_window_rects(exclude: &CxxVector<isize>) -> Vec<WindowRect>;

        /// 识别这块 BGRA(预乘) 图像里的文字，按行返回（PP-OCRv6 small + 本地模型）。
        /// 模型缺失/识别失败返回 Err（C++ 侧表现为 rust::Error 异常）。
        fn ocr_bgra(data: &[u8], width: usize, height: usize) -> Result<String>;

        /// 同上，但返回每行的外接框（图像局部像素坐标）与文本。
        fn ocr_boxes(data: &[u8], width: usize, height: usize) -> Result<Vec<OcrTextBox>>;

        /// 识别图里的表格（版面模型找出表格区域 + 框内文字按投影分行分列）。
        /// 没有表格返回空 Vec；模型缺失/推理失败返回 Err。
        fn ocr_tables(data: &[u8], width: usize, height: usize) -> Result<Vec<OcrTable>>;

        /// 视觉内容块包围盒：从 (seed_x, seed_y) 处在 BGRA(预乘) 缓冲里长出，
        /// 返回 [l, t, r, b]（缓冲局部像素，r/b 开区间）；认不出返回空 Vec。
        /// merge 是把碎字粘成行的膨胀半径（物理像素）；trust 的 4 个位表示取景框
        /// 左/上/右/下是否为真实边界（窗口边或屏幕边），块被非真实边切断即判不可信。
        fn content_rect(
            data: &[u8],
            width: usize,
            height: usize,
            seed_x: usize,
            seed_y: usize,
            merge: usize,
            trust: usize,
        ) -> Vec<i32>;

        /// HTTP(S) GET 整个响应体；网络失败或非 2xx 返回 Err。
        /// 同步阻塞，C++ 侧必须放在工作线程调用。
        fn http_get(url: &str, headers: &str) -> Result<Vec<u8>>;

        /// 把 URL 内容流式写入本地文件（覆盖写），返回字节数；失败返回 Err。
        fn download_to_file(url: &str, dest: &str, headers: &str) -> Result<u64>;

        /// 滚动长截图拼接会话：维护画布，逐帧按内容对齐追加（见 stitch.rs）。
        type StitchSession;

        /// width/frame_h 是帧尺寸（物理像素），max_h 是画布高度上限。
        fn stitch_session_new(width: usize, frame_h: usize, max_h: usize) -> Box<StitchSession>;

        /// 追加一帧 BGRA，返回对齐结果。
        fn push(self: &mut StitchSession, frame: &[u8]) -> StitchStep;

        /// 取走拼接完成的画布（BGRA），会话随之清空。
        fn take_canvas(self: &mut StitchSession) -> Vec<u8>;

        /// 只读画布切片（进度窗实时缩略图用）。
        fn canvas(self: &StitchSession) -> &[u8];

        fn canvas_height(self: &StitchSession) -> i32;
    }
}

mod blur;
mod capture;
mod content;
mod lines;
mod net;
mod ocr;
mod ocr_layout;
mod stitch;
mod table;
mod uia;
mod winrect;

pub use stitch::StitchSession;

use std::io::Write;
use std::sync::Once;

use windows::Win32::System::SystemInformation::GetLocalTime;

static PANIC_HOOK: Once = Once::new();

/// 本地时间 `HH:mm:ss.zzz`，与 C++ `logging.cpp` 的行首格式对齐，两边的日志
/// 混在同一个文件里才对得上行。没有 chrono（不引新依赖），直接问 Win32 要。
fn local_timestamp() -> String {
    // SAFETY: GetLocalTime 无参数、只返回值，不涉及裸指针
    let st = unsafe { GetLocalTime() };
    format!(
        "{:02}:{:02}:{:02}.{:03}",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds
    )
}

/// 桥接层的 panic 会在 `extern "C"` 边界直接 abort——进程无声消失、日志一行不剩。
/// 这里把 panic 追加进 C++ 侧同一份 zpin.log。
///
/// 已知取舍：本钩子直接 append 文件，**不**走 `logging.cpp` 的 QMutex，与 Qt
/// 侧并发写理论上仍可能交错。但这是崩溃路径（此刻一切已不可信），为一个必然
/// 异常的场景做跨语言互斥不值得；行格式与类目名对齐，保证事后仍能 grep 定位。
fn ensure_panic_log() {
    PANIC_HOOK.call_once(|| {
        let Ok(dir) = std::env::var("LOCALAPPDATA") else {
            return;
        };
        let path = std::path::PathBuf::from(dir).join("ZPin").join("zpin.log");
        std::panic::set_hook(Box::new(move |info| {
            let location = info
                .location()
                .map(|l| format!("{}:{}", l.file(), l.line()))
                .unwrap_or_else(|| String::from("<未知位置>"));
            let msg = info
                .payload()
                .downcast_ref::<&str>()
                .map(|s| (*s).to_string())
                .or_else(|| info.payload().downcast_ref::<String>().cloned())
                .unwrap_or_else(|| String::from("<非字符串 panic 负载>"));
            if let Ok(mut f) = std::fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open(&path)
            {
                // 与 logging.cpp 的 writeLine 同形：时间 [级别] 类目: 正文
                let _ = writeln!(
                    f,
                    "{} [PANIC] zpin.rust: {location}: {msg}",
                    local_timestamp()
                );
            }
        }));
    });
}

/// 桥接入口：把 C++ 切片交给纯计算实现，结果打包成 C++ 持有的向量。
fn blur_bgra(data: &[u8], width: usize, height: usize, radius: usize) -> Vec<u8> {
    ensure_panic_log();
    if width == 0 || height == 0 || radius == 0 {
        return Vec::new();
    }
    let need = width * height * 4;
    if data.len() < need {
        return Vec::new();
    }
    blur::three_box_blur(&data[..need], width, height, radius)
}

/// 桥接入口：单区域 BitBlt 抓屏。
fn capture_bgra(x: i32, y: i32, w: i32, h: i32) -> Vec<u8> {
    ensure_panic_log();
    capture::grab_bgra(x, y, w, h)
}

/// 桥接入口：不带 CAPTUREBLT 的抓屏（滚动长截图轮询用）。
fn capture_bgra_plain(x: i32, y: i32, w: i32, h: i32) -> Vec<u8> {
    ensure_panic_log();
    capture::grab_bgra_plain(x, y, w, h)
}

/// 桥接入口：UIA 元素查询。
fn element_from_point(x: i32, y: i32) -> Result<ffi::ElementInfo, windows::core::Error> {
    ensure_panic_log();
    uia::query(x, y)
}

/// 桥接入口：可见窗口矩形枚举。
fn visible_window_rects(exclude: &cxx::CxxVector<isize>) -> Vec<ffi::WindowRect> {
    ensure_panic_log();
    let set: std::collections::HashSet<isize> = exclude.iter().copied().collect();
    winrect::visible_window_rects(&set)
        .into_iter()
        .map(|(hwnd, l, t, r, b)| ffi::WindowRect { hwnd, l, t, r, b })
        .collect()
}

/// 桥接入口：PP-OCR 文字识别。
fn ocr_bgra(
    data: &[u8],
    width: usize,
    height: usize,
) -> Result<String, Box<dyn std::error::Error + Send + Sync>> {
    ensure_panic_log();
    ocr::recognize(data, width, height)
}

/// 桥接入口：PP-OCR 文字识别（带每行外接框）。
fn ocr_boxes(
    data: &[u8],
    width: usize,
    height: usize,
) -> Result<Vec<ffi::OcrTextBox>, Box<dyn std::error::Error + Send + Sync>> {
    ensure_panic_log();
    Ok(ocr::recognize_boxes(data, width, height)?
        .into_iter()
        .map(|l| ffi::OcrTextBox {
            x: l.x,
            y: l.y,
            w: l.w,
            h: l.h,
            text: l.text,
        })
        .collect())
}

/// 桥接入口：表格识别（版面模型找出表格区域 → 框内文字按投影分行分列）。
fn ocr_tables(
    data: &[u8],
    width: usize,
    height: usize,
) -> Result<Vec<ffi::OcrTable>, Box<dyn std::error::Error + Send + Sync>> {
    ensure_panic_log();
    Ok(table::recognize(data, width, height)?
        .into_iter()
        .map(|t| ffi::OcrTable {
            html: t.html,
            text: t.text,
        })
        .collect())
}

/// 桥接入口：滚动长截图拼接会话。
fn stitch_session_new(width: usize, frame_h: usize, max_h: usize) -> Box<StitchSession> {
    ensure_panic_log();
    Box::new(StitchSession::new(width, frame_h, max_h))
}

/// 桥接入口：视觉内容块包围盒。
fn content_rect(
    data: &[u8],
    width: usize,
    height: usize,
    seed_x: usize,
    seed_y: usize,
    merge: usize,
    trust: usize,
) -> Vec<i32> {
    ensure_panic_log();
    content::content_rect(data, width, height, seed_x, seed_y, merge, trust)
}

/// 桥接入口：HTTP(S) GET。
fn http_get(
    url: &str,
    headers: &str,
) -> Result<Vec<u8>, Box<dyn std::error::Error + Send + Sync>> {
    ensure_panic_log();
    net::http_get(url, headers)
}

/// 桥接入口：流式下载到本地文件。
fn download_to_file(
    url: &str,
    dest: &str,
    headers: &str,
) -> Result<u64, Box<dyn std::error::Error + Send + Sync>> {
    ensure_panic_log();
    net::download_to_file(url, dest, headers)
}
