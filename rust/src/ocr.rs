//! 文字识别：PP-OCRv6（ONNX 模型随包放在 exe 同级的 `models\`）。
//!
//! 一次识别几十到几百毫秒，只能在后台线程调用；引擎首次用时加载后常驻，
//! 用 Mutex 串行——同一时刻只可能有一次识别，且省掉每线程一份会话的内存。
//! 模型不联网、不带云端：文件缺失时返回 Err，由界面提示。
//! 当前用 small 档，文件名与体积见下方 DET_FILE 一组常量。

use std::path::PathBuf;
use std::sync::{Mutex, OnceLock};

use image::RgbImage;
use oar_ocr::prelude::*;

use crate::ffi::OcrTextBox;
use crate::ocr_layout::{self, LayoutLine};

/// 检测模型按最长边缩放输入；太扁的裁剪（如任务栏一行 1200×48）会把字高压到
/// 10px 以下，实测四段文字只认出一段。补白边到至少这么高后四段全部 0.94~1.00。
const MIN_CANVAS_H: u32 = 400;

pub(crate) type BoxError = Box<dyn std::error::Error + Send + Sync>;

/// 当前使用的模型组。
///
/// 三档都从 `oar-ocr` 内置登记表（oar-ocr-core 的 registry.rs）核对过
/// SHA-256，取自 ModelScope `greatv/oar-ocr`：
///   tiny : det 1.70MB + rec 4.26MB + tiny_dict  27KB  =  5.99 MB
///   small: det 9.42MB + rec 20.18MB + v6_dict   75KB  = 29.67 MB
/// 注意字典**不通用**：tiny 配 27KB 的精简字典，small/medium 配 75KB 的完整
/// 字典（`ppocrv6_dict.txt`），换模型时必须一起换，否则识别静默出错。
/// 实测对比见 README「画质与性能设计」一节。
/// det/rec/词典三件也被表格识别复用（`table.rs` 只加版面与结构模型，不再多一份文字模型）。
pub(crate) const DET_FILE: &str = "pp-ocrv6_small_det.onnx";
pub(crate) const REC_FILE: &str = "pp-ocrv6_small_rec.onnx";
pub(crate) const DICT_FILE: &str = "ppocrv6_dict.txt";

/// 只缓存**成功**建出来的引擎。曾经的 `OnceLock<Result<Mutex<OAROCR>, String>>`
/// 会把首次构造失败固化成一辈子的错误：模型文件被杀毒软件短暂占用、
/// `current_exe()` 在某些受限环境下失败，这些都是可恢复的瞬时故障，缓存住
/// 就等于整个进程生命周期内 OCR 全废、只能重启（而启动 2.5s 后的预热恰好会
/// 第一时间把这个失败定死）。失败不写缓存，下次调用自然重试。
static ENGINE: OnceLock<Mutex<OAROCR>> = OnceLock::new();

pub(crate) fn model_dir() -> Result<PathBuf, BoxError> {
    let exe = std::env::current_exe()?;
    let dir = exe.parent().ok_or("拿不到 exe 所在目录")?;
    Ok(dir.join("models"))
}

fn engine() -> Result<&'static Mutex<OAROCR>, BoxError> {
    if let Some(m) = ENGINE.get() {
        return Ok(m);
    }
    let dir = model_dir()?;
    let built = OAROCRBuilder::new(dir.join(DET_FILE), dir.join(REC_FILE), dir.join(DICT_FILE))
        .build()
        .map_err(|e| -> BoxError { format!("{}：{e}", dir.display()).into() })?;
    // 并发首次调用可能各自建一份，浪费的只是一次模型加载（识别本就是用户
    // 手动触发、且被下面的 Mutex 串行），换来的是失败可重试
    Ok(ENGINE.get_or_init(|| Mutex::new(built)))
}

/// BGRA（预乘 alpha）→ 铺在白底上的 RGB，并补白边到 MIN_CANVAS_H 高（原图更高就不动）。
///
/// 早期版本直接把 BGRA 三个通道当 RGB 搬过去，注释还把这写成"BGRA（预乘）→ RGB"
/// ——那不是等价变换。截图路径的画布先 `fill(0xFF202020)`、alpha 恒为 255，所以
/// 一直没暴露；但贴图可以直接来自剪贴板里的**带透明通道 PNG**，此时
///   - 半透明像素的 RGB 是被 alpha 压暗过的预乘值（直接用 = 对比度偏低、发灰）
///   - 全透明像素的 RGB 恒为 0（直接用 = 画面上凭空多出一片纯黑）
/// 送进 PP-OCR 就是一张对比度被破坏的图，表现为"识别出乱码"或干脆
/// "没有识别到文字"，而用户看到的贴图本身一切正常。必须先合成到白底。
pub(crate) fn to_rgb_padded(data: &[u8], width: usize, height: usize) -> Result<RgbImage, BoxError> {
    let canvas_h = std::cmp::max(height as u32, MIN_CANVAS_H);
    let top = (canvas_h as usize - height) / 2;
    let mut rgb = vec![255u8; width * canvas_h as usize * 3];
    for y in 0..height {
        let src_row = y * width * 4;
        let dst_row = (y + top) * width * 3;
        for x in 0..width {
            let s = src_row + x * 4;
            let d = dst_row + x * 3;
            // GDI 的 BGRA 小端序；第 4 字节是 alpha 参与下面的合成
            let b = data[s] as u32;
            let g = data[s + 1] as u32;
            let r = data[s + 2] as u32;
            let a = data[s + 3] as u32;
            // 预乘值还原成直通色再按 alpha 合成到白底：
            //   c_straight = c_premul * 255 / a；out = c_straight * a/255 + 255 * (1 - a/255)
            // 两步合起来就是 out = c_premul + 255 * (1 - a/255)，无需中间除法，
            // 且 a==0 时结果恰为 255（白），不会除零。
            let inv = 255 - a.min(255);
            rgb[d] = (r + inv) as u8;
            rgb[d + 1] = (g + inv) as u8;
            rgb[d + 2] = (b + inv) as u8;
        }
    }
    RgbImage::from_raw(width as u32, canvas_h, rgb).ok_or_else(|| "构造图像缓冲失败".into())
}

/// 补白把画布往下推了多少像素：识别结果的 y 要减掉它才是原图坐标。
/// 文字识别与表格的版面检测共用这一个算法，两边坐标才对得上。
pub(crate) fn canvas_pad_top(height: usize) -> f64 {
    f64::from((std::cmp::max(height as u32, MIN_CANVAS_H) - height as u32) / 2)
}

/// 两入口（纯文本 / 带框）共用的最小行信息：文本 + 外接框顶点。
/// 规约成自己的类型，下游就不用碰 oar-ocr 的区域结构。
struct OcrLine {
    text: String,
    points: Vec<(f32, f32)>,
}

/// 共用前置管线：校验尺寸与缓冲 → 铺白底补边 → 锁引擎识别 →
/// 规约成行列表。返回 (补白把画布往下推的 top 像素, 行列表)。
fn predict_lines(data: &[u8], width: usize, height: usize) -> Result<(f64, Vec<OcrLine>), BoxError> {
    if width == 0 || height == 0 {
        return Err("图像尺寸为空".into());
    }
    let need = width * height * 4;
    if data.len() < need {
        return Err("图像缓冲长度不足".into());
    }
    let image = to_rgb_padded(data, width, height)?;
    // 补白把画布往下推了 top 像素，框要减回去才是原图坐标
    let top = canvas_pad_top(height);
    let guard = engine()?
        .lock()
        .map_err(|e| -> BoxError { e.to_string().into() })?;
    let results = guard.predict(vec![image])?;
    let mut lines = Vec::new();
    for region in &results.first().ok_or("识别无结果")?.text_regions {
        let Some((line, _conf)) = region.text_with_confidence() else {
            continue;
        };
        let line = line.trim_end();
        if line.trim().is_empty() {
            continue;
        }
        let pts = &region.bounding_box.points;
        if pts.is_empty() {
            continue;
        }
        lines.push(OcrLine {
            text: line.to_string(),
            points: pts.iter().map(|p| (p.x, p.y)).collect(),
        });
    }
    Ok((top, lines))
}

/// 顶点集的最小外接矩形：文本框可能是斜的，归并和脱敏都按正框处理。
fn bbox_of(points: &[(f32, f32)]) -> (f32, f32, f32, f32) {
    let (mut x0, mut y0, mut x1, mut y1) = (f32::MAX, f32::MAX, f32::MIN, f32::MIN);
    for (px, py) in points {
        x0 = x0.min(*px);
        y0 = y0.min(*py);
        x1 = x1.max(*px);
        y1 = y1.max(*py);
    }
    (x0, y0, x1, y1)
}

/// 识别这块 (width×height) BGRA 图像里的文字，按版式归并成段落文本。
///
/// 逐行吐出去会把一段话的软换行拆成好几行、把两行粘连的正文照原样留着，
/// 粘到聊天框里就是一堆碎行；归并后一段一行，列表和菜单项仍逐行。
pub fn recognize(data: &[u8], width: usize, height: usize) -> Result<String, BoxError> {
    let (top, lines) = predict_lines(data, width, height)?;
    let boxes: Vec<LayoutLine> = lines
        .iter()
        .map(|l| {
            let (x0, y0, x1, y1) = bbox_of(&l.points);
            LayoutLine {
                text: l.text.clone(),
                left: x0 as f64,
                top: y0 as f64 - top,
                right: x1 as f64,
                bottom: y1 as f64 - top,
            }
        })
        .collect();
    Ok(ocr_layout::layout_text(&boxes))
}

/// 识别并返回每行的外接框（图像局部像素坐标，已剔除补白偏移）与文本。
/// 一键脱敏用：C++ 侧拿文本跑正则，命中就把框盖马赛克。
pub fn recognize_boxes(
    data: &[u8],
    width: usize,
    height: usize,
) -> Result<Vec<crate::ffi::OcrTextBox>, BoxError> {
    let (top, lines) = predict_lines(data, width, height)?;
    let mut out = Vec::new();
    for l in &lines {
        let (x0, y0, x1, y1) = bbox_of(&l.points);
        out.push(OcrTextBox {
            x: (x0 as i32).clamp(0, width as i32 - 1),
            y: ((y0 - top as f32) as i32).clamp(0, height as i32 - 1),
            w: ((x1 - x0) as i32).clamp(1, width as i32),
            h: ((y1 - y0) as i32).clamp(1, height as i32),
            text: l.text.clone(),
        });
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::to_rgb_padded;

    #[test]
    fn 扁条补白边后高度达标且像素搬运正确() {
        // 2×1 的纯红 + 纯绿两像素
        let bgra = vec![0, 0, 255, 255, 0, 255, 0, 255];
        let img = to_rgb_padded(&bgra, 2, 1).expect("构造失败");
        assert_eq!((img.width(), img.height()), (2, 400));
        assert_eq!(img.get_pixel(0, 199).0, [255, 0, 0]);
        assert_eq!(img.get_pixel(1, 199).0, [0, 255, 0]);
        // 角落仍是白边
        assert_eq!(img.get_pixel(0, 0).0, [255, 255, 255]);
    }

    #[test]
    fn 高图不补边() {
        let bgra = vec![7u8; 2 * 500 * 4];
        let img = to_rgb_padded(&bgra, 2, 500).expect("构造失败");
        assert_eq!(img.height(), 500);
    }

    #[test]
    fn 半透明合成到白底而非直接用预乘值() {
        // 一个 1x1 像素：预乘的 R=128 G=0 B=0，alpha=128。
        // 反预乘得直通色 (255,0,0)，再按 50% 合成到白底：
        //   R = 255*128/255 + 255*127/255 = 255
        //   G =   0*128/255 + 255*127/255 = 127   ← 早先直接搬通道得到 0
        let bgra = vec![0u8, 0, 128, 128];
        let img = to_rgb_padded(&bgra, 1, 1).expect("构造失败");
        assert_eq!(img.get_pixel(0, 199).0, [255, 127, 127]);
    }

    #[test]
    fn 全透明像素变白而不是黑洞() {
        // alpha=0 时预乘 RGB 必为 0，早先直接搬会得到纯黑块（凭空多一片黑）。
        let bgra = vec![0u8, 0, 0, 0];
        let img = to_rgb_padded(&bgra, 1, 1).expect("构造失败");
        assert_eq!(img.get_pixel(0, 199).0, [255, 255, 255]);
    }

    #[test]
    fn 不透明像素原样保留() {
        // alpha=255 时合成应完全透明于原值，验证没有把普通截图路径弄坏。
        let bgra = vec![11u8, 22, 33, 255];
        let img = to_rgb_padded(&bgra, 1, 1).expect("构造失败");
        assert_eq!(img.get_pixel(0, 199).0, [33, 22, 11]);
    }
}
