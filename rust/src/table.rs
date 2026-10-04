//! 表格识别：版面模型只负责找出「哪里有表格」，行列网格由 OCR 框投影推断。
//!
//! 为什么不用 SLANet 的结构输出：实测（他的成绩表截图 733×403、8 列 15 行）结构模型
//! 能数对格子（119 个、置信度 0.996），但 e2e 模式（不配 129MB 的 RT-DETR 单元格检测）
//! 给出的**格子坐标**每个都约两倍行高，缝合器按 IoA 把相邻两行的文字塞进同一格、
//! 后一半格子全空。文字本身识别全对，坏的只是坐标归属 —— 所以行列改由自己投影。
//!
//! 模型只剩 `picodet_layout_1x_table.onnx`（7.2MB）加文字识别那三件；
//! `slanet.onnx` 与它的标签词典随这次重写删除。

use std::sync::Mutex;

use oar_ocr::predictors::layout_detection::LayoutDetectionPredictorBuilder;
use oar_ocr::predictors::LayoutDetectionPredictor;

use crate::lines::{self, Area, Pixels};
use crate::ocr::{self, BoxError};

/// 只检表格这一类的版面模型；名字要与 oar-ocr 的预设对上。
const LAYOUT_FILE: &str = "picodet_layout_1x_table.onnx";
const LAYOUT_NAME: &str = "picodet_layout_1x_table";

/// 一个待归格的框：外接矩形（补白画布坐标）+ 文本。
#[derive(Clone, Debug)]
pub struct CellBox {
    pub x: f64,
    pub y: f64,
    pub w: f64,
    pub h: f64,
    pub text: String,
}

/// 一格的对齐（只有线检测给得出：OCR 框左右各有几像素富余，判不了）。
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Align {
    Center,
    Right,
}

/// 一格：文本 + 跨度 + 对齐。同一格命中多个框时按阅读顺序拼接。
#[derive(Clone, Debug, Default)]
pub struct Cell {
    pub text: String,
    pub row_span: usize,
    pub col_span: usize,
    pub align: Option<Align>,
}

/// 格子槽位：空格、合并锚点、被锚点覆盖的位置。
#[derive(Clone, Debug)]
enum Slot {
    Empty,
    Anchor(Cell),
    Covered,
}

/// 投影出来的表格网格。
pub struct Grid {
    rows: usize,
    cols: usize,
    slots: Vec<Vec<Slot>>,
}

impl Grid {
    pub fn is_empty(&self) -> bool {
        self.rows == 0 || self.cols == 0
    }

    /// 只出 `<table>…</table>` 片段：整份文档的外壳由 C++ 包一次。
    /// 每张表各自带 `<html><body>` 的话，多表拼起来就是多个文档首尾相接，
    /// CF_HTML 的 StartFragment 偏移会算歪，Excel 大概率只认第一张。
    fn to_html(&self) -> String {
        let mut out = String::from("<table>");
        for r in 0..self.rows {
            out.push_str("<tr>");
            for c in 0..self.cols {
                match &self.slots[r][c] {
                    Slot::Covered => {}
                    Slot::Empty => out.push_str("<td></td>"),
                    Slot::Anchor(cell) => {
                        out.push_str("<td");
                        if cell.col_span > 1 {
                            out.push_str(&format!(" colspan=\"{}\"", cell.col_span));
                        }
                        if cell.row_span > 1 {
                            out.push_str(&format!(" rowspan=\"{}\"", cell.row_span));
                        }
                        let mut decls: Vec<&str> = Vec::new();
                        if needs_text_format(&cell.text) {
                            // Excel 的 General 格式会把 0001 变成 1、长数字串变科学计数法
                            decls.push("mso-number-format:'\\@'");
                        }
                        if let Some(align) = cell.align {
                            // 左对齐是文本的默认，不标；只标默认做不到的两种。
                            // CSS 之外再补一份 HTML 老属性：剪贴板里的 CF_HTML 由
                            // 各电子表格自己的 importer 解析，只写 style 时
                            // WPS 一类会直接忽略居中（只认 align=）。
                            let (attr, decl) = match align {
                                Align::Center => ("center", "text-align:center"),
                                Align::Right => ("right", "text-align:right"),
                            };
                            out.push_str(&format!(" align=\"{attr}\""));
                            decls.push(decl);
                        }
                        if !decls.is_empty() {
                            out.push_str(&format!(" style=\"{}\"", decls.join(";")));
                        }
                        out.push('>');
                        out.push_str(&escape_html(&cell.text));
                        out.push_str("</td>");
                    }
                }
            }
            out.push_str("</tr>");
        }
        out.push_str("</table>");
        out
    }

    /// 制表符版：合并覆盖的位置留空，保证列号对齐。
    fn to_tsv(&self) -> String {
        let mut lines = Vec::with_capacity(self.rows);
        for r in 0..self.rows {
            let mut cells = Vec::with_capacity(self.cols);
            for c in 0..self.cols {
                let text = match &self.slots[r][c] {
                    Slot::Anchor(cell) => cell.text.replace('\n', " "),
                    _ => String::new(),
                };
                cells.push(text);
            }
            lines.push(cells.join("\t"));
        }
        while lines
            .last()
            .is_some_and(|l| l.replace(['\t', ' '], "").is_empty())
        {
            lines.pop();
        }
        lines.join("\n")
    }
}

/// 会被 Excel 改坏的内容：带前导零的数字、长度 ≥12 的纯数字、以及日期样式。
///
/// 日期单独说：`2026-01-15` 被 Excel 当日期吃掉前导零（变 `2026-1-15`），列一窄
/// 干脆显示成 `######`，看着就像识别丢了字。
fn needs_text_format(text: &str) -> bool {
    let t = text.trim();
    if t.is_empty() {
        return false;
    }
    if looks_like_date(t) {
        return true;
    }
    if !t.chars().all(|c| c.is_ascii_digit()) {
        return false;
    }
    (t.starts_with('0') && t.chars().count() > 1) || t.chars().count() >= 12
}

/// `2026-01-15` / `2026/1/5` / `2026.01.15`，后面带时间也算。
fn looks_like_date(t: &str) -> bool {
    let short_digits = |s: &str| {
        !s.is_empty() && s.chars().count() <= 2 && s.chars().all(|c| c.is_ascii_digit())
    };
    let b = t.as_bytes();
    // 最短是「4 位年 + 分隔 + 1 位月 + 分隔 + 1 位日」= 8 字节
    if b.len() < 8 || !b[..4].iter().all(|c| c.is_ascii_digit()) {
        return false;
    }
    let sep = b[4];
    if !matches!(sep, b'-' | b'/' | b'.') {
        return false;
    }
    let Some(at) = t[5..].find(sep as char) else {
        return false;
    };
    if !short_digits(&t[5..5 + at]) {
        return false;
    }
    let day: String = t[6 + at..]
        .chars()
        .take_while(|c| c.is_ascii_digit())
        .collect();
    short_digits(&day)
}

fn escape_html(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    for c in text.chars() {
        match c {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            // 同格多段用 \n 记：HTML 里裸换行会被当成空白折叠掉，真两行的单元格
            // （地址、备注）在 Excel 里就挤成一行了
            '\n' => out.push_str("<br/>"),
            _ => out.push(c),
        }
    }
    out
}

/// 把一维点聚成簇：与**簇均值**相差不到 tolerance 就归入同簇，返回各簇均值。
///
/// 取均值而不是簇的第一个点：点集在容差内散布时，簇首会把整簇钉死在极端值上。
/// 行带与列缝共用这一个函数（早先行用簇首、列用均值，同一文件两套算法）。
fn cluster(points: &[f64], tolerance: f64) -> Vec<f64> {
    let mut sorted = points.to_vec();
    sorted.sort_by(f64::total_cmp);
    let mut out: Vec<f64> = Vec::new();
    let mut sum = 0.0;
    let mut n = 0usize;
    for p in sorted {
        match out.last_mut() {
            Some(last) if p - *last <= tolerance => {
                sum += p;
                n += 1;
                *last = sum / n as f64;
            }
            _ => {
                out.push(p);
                sum = p;
                n = 1;
            }
        }
    }
    out
}

/// 带中心列表里，离 `value` 最近的那一条。
fn nearest_band(list: &[f64], value: f64) -> usize {
    list.iter()
        .enumerate()
        .min_by(|a, b| (a.1 - value).abs().total_cmp(&(b.1 - value).abs()))
        .map(|(i, _)| i)
        .unwrap_or(0)
}

/// 列缝：把每一行里相邻框之间的空档收集起来投票。
///
/// 不能按「x 覆盖区间」切带 —— 合并单元格产生的宽框会把中间的列缝填平，
/// 整表并成一列（实测踩过）。也不能只看某个框的左边界：一格文字被切成两段时
/// 会多出一列。所以取全部行内空档的中点，丢掉明显比典型列缝窄的（格内碎片），
/// 再把靠得近的空档并成一条列缝。
fn column_separators(boxes: &[CellBox], row_of_box: &[usize], rows: usize, min_gap: f64) -> Vec<f64> {
    // 每行各自的 (缝中点, 缝宽)：缝宽用来筛掉格内碎片，中点用来定列缝
    let mut per_row: Vec<Vec<(f64, f64)>> = vec![Vec::new(); rows];
    for r in 0..rows {
        let mut line: Vec<&CellBox> = boxes
            .iter()
            .enumerate()
            .filter(|(i, _)| row_of_box[*i] == r)
            .map(|(_, b)| b)
            .collect();
        line.sort_by(|a, b| a.x.total_cmp(&b.x));
        for pair in line.windows(2) {
            let (left, right) = (pair[0], pair[1]);
            let width = right.x - (left.x + left.w);
            if width >= min_gap {
                per_row[r].push(((left.x + left.w + right.x) / 2.0, width));
            }
        }
    }
    let all: Vec<(f64, f64)> = per_row.iter().flatten().copied().collect();
    if all.is_empty() {
        return Vec::new();
    }
    let typical = median(&(all.iter().map(|g| g.1).collect::<Vec<_>>()));
    let kept: Vec<Vec<f64>> = per_row
        .iter()
        .map(|row| {
            row.iter()
                .filter(|g| g.1 >= typical * 0.5)
                .map(|g| g.0)
                .collect()
        })
        .collect();
    // 列间距：同一行里相邻列缝的中位距离。缝的位置取决于左右两个框的边缘，文字长短
    // 一不同，同一条缝就会在十几像素内晃；拿半行高当容差会把一列劈成两列
    // （销售表实测 8 列变 11 列，多出来的列全空）
    let mut pitches: Vec<f64> = Vec::new();
    for row in &kept {
        for pair in row.windows(2) {
            pitches.push(pair[1] - pair[0]);
        }
    }
    let tolerance = if pitches.is_empty() {
        min_gap
    } else {
        (median(&pitches) * 0.45).max(min_gap)
    };

    let mids: Vec<f64> = kept.iter().flatten().copied().collect();
    cluster(&mids, tolerance)
}

/// 框集合 → 网格。跨多条列缝/行带的框按 colspan/rowspan 记。
pub fn build_grid(boxes: &[CellBox]) -> Grid {
    let heights: Vec<f64> = boxes.iter().filter(|b| b.h > 0.0).map(|b| b.h).collect();
    // 阈值取半行高：实测成绩表字高约 14~25px、行距 25px
    let median_h = median(&heights).max(1.0);
    let min_gap = median_h * 0.5;

    // 行按「框中心」聚类：同一行里名字框和数字框的顶边能差十几像素，
    // 按顶边切会把一行劈成两行（真图验收时踩到，表现为整表 rowspan=2）
    let centers: Vec<f64> = boxes.iter().map(|b| b.y + b.h / 2.0).collect();
    let row_centers = cluster(&centers, min_gap);
    let rows = row_centers.len();
    let row_of_box: Vec<usize> = centers
        .iter()
        .map(|c| nearest_band(&row_centers, *c))
        .collect();
    let separators = column_separators(boxes, &row_of_box, rows, min_gap);
    let cols = separators.len() + 1;
    // 行距：相邻行中心的中位数。跨几行用「框高 ÷ 行距」算，比拿底边去撞带起点稳
    let pitch = if rows >= 2 {
        let diffs: Vec<f64> = row_centers.windows(2).map(|w| w[1] - w[0]).collect();
        median(&diffs).max(min_gap)
    } else {
        median_h
    };
    let mut slots = vec![vec![Slot::Empty; cols]; rows];
    // 每格归属哪个锚点（锚点指向自己）。没有这张表就发现不了"落点已被合并格占掉"，
    // 那种情况下会另开一格，本行的 td 数超过列数，粘进 Excel 整表错位
    let mut owner: Vec<Vec<Option<(usize, usize)>>> = vec![vec![None; cols]; rows];

    // 同一格可能命中多个框（标题被切成两段）：先按行带，同行内宽框先占位，
    // 再按 x 排（同格文字按左右顺序拼接）
    let mut order: Vec<usize> = (0..boxes.len()).collect();
    order.sort_by(|&i, &j| {
        let (a, b) = (&boxes[i], &boxes[j]);
        row_of_box[i]
            .cmp(&row_of_box[j])
            .then_with(|| (b.w * b.h).total_cmp(&(a.w * a.h)))
            .then_with(|| a.x.total_cmp(&b.x))
    });

    for i in order {
        let b = &boxes[i];
        let text = b.text.trim();
        if text.is_empty() || rows == 0 {
            continue;
        }
        // 行归属用「顶边往下推半个行距」再找最近带：检测框常常比行距还高
        // （实测 h≈34 而行距 25），直接拿顶边或中心都会串到相邻行去
        let at_row = nearest_band(&row_centers, b.y + b.h.min(pitch) / 2.0);
        let at_col = separators.iter().filter(|s| **s < b.x).count();
        // 右边界往回缩半个阈值：框的右边常正好压在列缝上
        let right_col = separators
            .iter()
            .filter(|s| **s < b.x + b.w - min_gap * 0.5)
            .count()
            .max(at_col)
            .min(cols - 1);
        if let Some((anchor_row, anchor_col)) = owner[at_row][at_col] {
            // 这一格已有主：文字并进它的锚点，绝不另开一格
            if let Slot::Anchor(cell) = &mut slots[anchor_row][anchor_col] {
                if !cell.text.is_empty() {
                    cell.text.push('\n');
                }
                cell.text.push_str(text);
            }
            continue;
        }
        // 跨度只吞还空着的格子，碰到有主的就停 —— 锚点的 span 因此永不重叠
        let mut col_span = 1;
        while at_col + col_span <= right_col && owner[at_row][at_col + col_span].is_none() {
            col_span += 1;
        }
        let want_row = (b.h / pitch).round() as usize;
        let mut row_span = 1;
        while at_row + row_span < rows
            && (at_row + row_span) < at_row + want_row.max(1)
            && (at_col..at_col + col_span).all(|c| owner[at_row + row_span][c].is_none())
        {
            row_span += 1;
        }
        slots[at_row][at_col] = Slot::Anchor(Cell {
            text: text.to_string(),
            row_span,
            col_span,
            align: None,
        });
        for r in at_row..at_row + row_span {
            for c in at_col..at_col + col_span {
                owner[r][c] = Some((at_row, at_col));
                if r == at_row && c == at_col {
                    continue;
                }
                slots[r][c] = Slot::Covered;
            }
        }
    }
    Grid {
        rows,
        cols,
        slots,
    }
}

/// 有线可依时的网格：格子矩形来自线，文字按中心落格，对齐按"框贴哪条边"判。
fn build_grid_from_lines(lg: &lines::LineGrid, boxes: &[CellBox]) -> Grid {
    let (_gx0, gy0, _gx1, gy1) = lg.bounds();
    let (cols, inner_rows) = (lg.cols(), lg.rows());
    // 截图常把最外圈那行切掉半截（销售表的「合计」只剩 7 像素高），那一行没有底线，
    // 中心落在网格外。直接丢掉就比投影法少一行内容，所以给它补一行。
    let above = boxes
        .iter()
        .filter(|b| b.y + b.h / 2.0 < gy0)
        .count();
    let below = boxes
        .iter()
        .filter(|b| b.y + b.h / 2.0 >= gy1)
        .count();
    let top = usize::from(above > 0);
    let rows = inner_rows + top + usize::from(below > 0);
    if rows == 0 || cols == 0 {
        return Grid {
            rows: 0,
            cols: 0,
            slots: Vec::new(),
        };
    }
    let mut slots = vec![vec![Slot::Empty; cols]; rows];
    // 每个槽位归哪个锚点（合并格把它盖住的槽位也指过去）
    let mut owner: Vec<Vec<Option<(usize, usize)>>> = vec![vec![None; cols]; rows];
    let mut edges = vec![vec![(0.0_f64, 0.0_f64); cols]; rows];
    for cell in lg.cells() {
        let (r, c) = (cell.row + top, cell.col);
        if r >= rows || c >= cols {
            continue;
        }
        // 跨度在这里就写死：合并格没落到文字时也要留一个带 colspan 的空格，
        // 否则那一行的 td 单位数不够，粘进 Excel 整表左移
        slots[r][c] = Slot::Anchor(Cell {
            text: String::new(),
            row_span: cell.row_span,
            col_span: cell.col_span,
            align: None,
        });
        edges[r][c] = (cell.x0, cell.x1);
        for rr in r..(r + cell.row_span).min(rows) {
            for cc in c..(c + cell.col_span).min(cols) {
                owner[rr][cc] = Some((r, c));
                if (rr, cc) != (r, c) {
                    slots[rr][cc] = Slot::Covered;
                }
            }
        }
    }
    let mut parts: Vec<Vec<Vec<&CellBox>>> = vec![vec![Vec::new(); cols]; rows];
    for b in boxes {
        let text = b.text.trim();
        if text.is_empty() {
            continue;
        }
        let (cx, cy) = (b.x + b.w / 2.0, b.y + b.h / 2.0);
        let c = lg.col_of(cx);
        if cy < gy0 {
            parts[0][c].push(b);
        } else if cy >= gy1 {
            parts[rows - 1][c].push(b);
        } else if let Some((r, col)) = owner[lg.row_of(cy) + top][c] {
            parts[r][col].push(b);
        }
    }
    for r in 0..rows {
        for c in 0..cols {
            if parts[r][c].is_empty() {
                continue;
            }
            let (row_span, col_span) = match &slots[r][c] {
                Slot::Anchor(cell) => (cell.row_span.max(1), cell.col_span.max(1)),
                _ => (1, 1),
            };
            let (x0, x1) = edges[r][c];
            slots[r][c] = Slot::Anchor(Cell {
                text: parts[r][c]
                    .iter()
                    .map(|p| p.text.trim())
                    .collect::<Vec<&str>>()
                    .join("\n"),
                row_span,
                col_span,
                // 残留行本来就残缺，不判对齐
                align: if x1 > x0 {
                    alignment(&parts[r][c], x0, x1)
                } else {
                    None
                },
            });
        }
    }
    Grid { rows, cols, slots }
}

/// 文字整体贴格子哪条边 → 对齐。检测框比字宽几个像素，所以留一份随格宽放大的余量。
fn alignment(parts: &[&CellBox], x0: f64, x1: f64) -> Option<Align> {
    let left = parts.iter().map(|b| b.x).fold(f64::INFINITY, f64::min);
    let right = parts
        .iter()
        .map(|b| b.x + b.w)
        .fold(f64::NEG_INFINITY, f64::max);
    let width = (x1 - x0).max(1.0);
    let slack = (width * 0.08).max(6.0);
    let left_flush = left - x0 <= slack;
    let right_flush = x1 - right <= slack;
    match (left_flush, right_flush) {
        (false, true) => Some(Align::Right),
        // 两头都贴（文字撑满）或两头都不贴（悬在中间）都算居中
        (true, true) | (false, false) => Some(Align::Center),
        (true, false) => None,
    }
}

fn median(values: &[f64]) -> f64 {
    let mut v = values.to_vec();
    v.sort_by(f64::total_cmp);
    match v.len() {
        0 => 0.0,
        n => v[n / 2],
    }
}

/// 一张识别出来的表格：HTML（给 text/html）+ TSV（给 text/plain）。
pub struct RecognizedTable {
    pub html: String,
    pub text: String,
}

/// 版面预测器每次现建、用完释放：常驻缓存实测把进程顶到 629MB 且不回落，
/// 现建 + drop 后常驻回到 36MB，代价只有零点几秒建模时间。
/// 文字识别不另开会话 —— 复用 `ocr.rs` 那个已缓存（且启动就预热）的引擎，
/// 所以表格路径只比单纯识字多一个版面模型。
fn build_layout() -> Result<LayoutDetectionPredictor, BoxError> {
    let dir = ocr::model_dir()?;
    LayoutDetectionPredictorBuilder::new()
        .model_name(LAYOUT_NAME)
        .build(dir.join(LAYOUT_FILE))
        .map_err(|e| -> BoxError { format!("{}：{e}", dir.display()).into() })
}

/// 同一时刻只放一次表格识别进来（连点两次会同时占两份会话）。
static BUSY: Mutex<()> = Mutex::new(());

/// 识别这张 BGRA 图里的表格；一张都没找到返回空 Vec（不算失败）。
pub fn recognize(
    data: &[u8],
    width: usize,
    height: usize,
) -> Result<Vec<RecognizedTable>, BoxError> {
    let _guard = BUSY
        .lock()
        .map_err(|e| -> BoxError { e.to_string().into() })?;
    // 缓冲长度与空尺寸校验：这是桥接层唯一没做这项检查的入口（blur 在 lib.rs、
    // 文字识别在 ocr::predict_lines、content_rect 在 content.rs 都查了）。
    // 下面 to_rgb_padded 与 Pixels::lum 都按 width*height*4 直接下标索引，短缓冲
    // 会越界 panic；跨 cxx 边界 panic 可能直接 abort，进程无声消失。
    if width == 0 || height == 0 {
        return Err("图像尺寸为空".into());
    }
    if data.len() < width * height * 4 {
        return Err("图像缓冲长度不足".into());
    }
    let rgb = ocr::to_rgb_padded(data, width, height)?;
    let layout = build_layout()?.predict(vec![rgb])?;
    let all_boxes = ocr::recognize_boxes(data, width, height)?;
    // 版面框是补白画布坐标，文字框已减过偏移，先统一到原图坐标再比
    let pad = ocr::canvas_pad_top(height);
    let mut tables = Vec::new();
    for element in layout.elements.first().into_iter().flatten() {
        if element.element_type != "table" {
            continue;
        }
        let tx0 = f64::from(element.bbox.x_min());
        let ty0 = f64::from(element.bbox.y_min()) - pad;
        let tx1 = f64::from(element.bbox.x_max());
        let ty1 = f64::from(element.bbox.y_max()) - pad;
        let boxes: Vec<CellBox> = all_boxes
            .iter()
            .filter(|b| {
                let cx = f64::from(b.x) + f64::from(b.w) / 2.0;
                let cy = f64::from(b.y) + f64::from(b.h) / 2.0;
                cx >= tx0 && cx <= tx1 && cy >= ty0 && cy <= ty1
            })
            .map(|b| CellBox {
                x: f64::from(b.x),
                y: f64::from(b.y),
                w: f64::from(b.w),
                h: f64::from(b.h),
                text: b.text.clone(),
            })
            .collect();
        // 有边框就先按线还原真格子（能给出合并跨度与对齐）；无边框表检不出线时
        // 回落到 OCR 框投影
        let px = Pixels::new(data, width, height);
        let area = Area {
            x0: tx0.clamp(0.0, width as f64) as usize,
            y0: ty0.clamp(0.0, height as f64) as usize,
            x1: tx1.clamp(0.0, width as f64) as usize,
            y1: ty1.clamp(0.0, height as f64) as usize,
        };
        let grid = match lines::detect(&px, area) {
            Some(lg) => build_grid_from_lines(&lg, &boxes),
            None => build_grid(&boxes),
        };
        if !grid.is_empty() {
            tables.push(RecognizedTable {
                html: grid.to_html(),
                text: grid.to_tsv(),
            });
        }
    }
    Ok(tables)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 造一个框：第 col 列、第 row 行，列距 86、行距 25、字高 14。
    fn cell(col: usize, row: usize, text: &str) -> CellBox {
        at(
            20.0 + col as f64 * 86.0,
            20.0 + row as f64 * 25.0,
            60.0,
            14.0,
            text,
        )
    }

    fn at(x: f64, y: f64, w: f64, h: f64, text: &str) -> CellBox {
        CellBox {
            x,
            y,
            w,
            h,
            text: text.to_string(),
        }
    }

    #[test]
    fn 日期样式也按文本存不被excel改格式() {
        let boxes = vec![
            cell(0, 0, "2026-01-15"),
            cell(1, 0, "2026/2/3"),
            cell(2, 0, "2026.01.15 12:30"),
            cell(3, 0, "299"),
        ];
        let html = build_grid(&boxes).to_html();
        assert!(html.contains("mso-number-format:'\\@'\">2026-01-15<"), "{html}");
        assert!(html.contains("mso-number-format:'\\@'\">2026/2/3<"), "{html}");
        assert!(html.contains("mso-number-format:'\\@'\">2026.01.15 12:30<"), "{html}");
        // 普通数字仍然是数值，能直接求和
        assert!(html.contains("<td>299</td>"), "{html}");
    }

    #[test]
    fn 缝的位置随文字长短晃动时不多出空列() {
        // 复刻销售表：同一列缝在不同行的中点能差 16px（文字长短不同），
        // 容差只按半行高时会把 3 列切成 4~5 列，多出来的列全空
        let boxes = vec![
            at(0.0, 20.0, 60.0, 14.0, "甲甲"),
            at(100.0, 20.0, 60.0, 14.0, "乙乙"),
            at(200.0, 20.0, 60.0, 14.0, "丙丙"),
            at(0.0, 45.0, 40.0, 14.0, "1"),
            at(88.0, 45.0, 70.0, 14.0, "22"),
            at(205.0, 45.0, 50.0, 14.0, "3"),
            at(0.0, 70.0, 55.0, 14.0, "4"),
            at(95.0, 70.0, 60.0, 14.0, "55"),
            at(202.0, 70.0, 58.0, 14.0, "6"),
        ];
        let grid = build_grid(&boxes);
        assert_eq!(grid.cols, 3, "列数={}", grid.cols);
        let tsv = grid.to_tsv();
        assert_eq!(tsv.lines().count(), 3);
        assert!(tsv.lines().all(|l| l.split('\t').count() == 3), "{tsv}");
        assert!(!tsv.contains("\t\t"), "不该出现空列:\n{tsv}");
    }

    /// 按 HTML 把占位还原一遍：任何一行放不下、或有格子没被交代，就是结构坏了。
    /// 这是 Excel 眼里的东西，比数 td 个数准。
    fn html_fits(html: &str, cols: usize) -> Result<(), String> {
        let mut carry = vec![0usize; cols];
        for (r, row_html) in html.split("<tr>").skip(1).enumerate() {
            let mut col = 0usize;
            for cell in row_html.split("<td").skip(1) {
                while col < cols && carry[col] > 0 {
                    carry[col] -= 1;
                    col += 1;
                }
                if col >= cols {
                    return Err(format!("第 {r} 行的 td 溢出（列数 {cols}）"));
                }
                let head = cell.split('>').next().unwrap_or("");
                let span = |key: &str| {
                    head.split(&format!("{key}=\""))
                        .nth(1)
                        .and_then(|s| s.split('"').next())
                        .and_then(|s| s.parse::<usize>().ok())
                        .unwrap_or(1)
                };
                let (cs, rs) = (span("colspan"), span("rowspan"));
                if col + cs > cols {
                    return Err(format!("第 {r} 行 colspan={cs} 从第 {col} 列起超出 {cols} 列"));
                }
                for c in col..col + cs {
                    carry[c] = rs.saturating_sub(1);
                }
                col += cs;
            }
            while col < cols {
                if carry[col] == 0 {
                    return Err(format!("第 {r} 行第 {col} 列既没有 td 也没被 rowspan 盖住"));
                }
                carry[col] -= 1;
                col += 1;
            }
        }
        Ok(())
    }

    #[test]
    fn 合并格覆盖区里再落框时表格仍然合法() {
        // OCR 把前三格并成一个宽框，同时第四格的文字又落在这个宽框的覆盖区里。
        // 早先 _ => 分支会把 Covered 改写成新锚点 → 该行多出一个列单位 → Excel 整表错位
        let boxes = vec![
            at(20.0, 20.0, 250.0, 14.0, "甲乙丙"),
            at(180.0, 20.0, 60.0, 14.0, "丁"),
            at(20.0, 45.0, 40.0, 14.0, "1"),
            at(100.0, 45.0, 40.0, 14.0, "2"),
            at(180.0, 45.0, 40.0, 14.0, "3"),
            at(300.0, 45.0, 40.0, 14.0, "4"),
        ];
        let grid = build_grid(&boxes);
        assert_eq!((grid.rows, grid.cols), (2, 4));
        let html = grid.to_html();
        html_fits(&html, 4).unwrap_or_else(|e| panic!("{e}\n{html}"));
        // 落进覆盖区的文字并进了那个锚点，没被丢掉
        assert!(html.contains("甲乙丙<br/>丁"), "{html}");
    }

    #[test]
    fn html只是片段且同格多段用换行标签() {
        let boxes = vec![
            at(20.0, 20.0, 40.0, 14.0, "第一段"),
            at(64.0, 20.0, 40.0, 14.0, "第二段"),
        ];
        let html = build_grid(&boxes).to_html();
        assert!(html.starts_with("<table>"), "{html}");
        assert!(html.ends_with("</table>"), "{html}");
        assert!(!html.contains("<html") && !html.contains("<body"), "{html}");
        assert!(html.contains("<br/>"), "{html}");
    }

    #[test]
    fn 聚类取簇均值而非簇首() {
        // 0 与 9 差 9 ≤ 容差 10 → 一簇，均值 4.5（用簇首会钉死在 0 上）
        assert_eq!(cluster(&[9.0, 0.0, 30.0], 10.0), vec![4.5, 30.0]);
    }

    /// 画一条暗线到 BGRA 画布（0=黑，255=白）
    fn paint(buf: &mut [u8], w: usize, x: usize, y: usize) {
        let i = (y * w + x) * 4;
        buf[i] = 60;
        buf[i + 1] = 60;
        buf[i + 2] = 60;
        buf[i + 3] = 255;
    }

    #[test]
    fn 合并格没落到文字也保留跨度() {
        // 两行三列，第一行带里没画竖线（整行合并）且那一行没有文字。
        // 锚点必须带着 colspan 出现：只留一个 <td> 的话那一行的列单位数不够，
        // 粘进 Excel 后面所有格子整体左移。
        let (w, h) = (120usize, 60usize);
        let mut buf = vec![255u8; w * h * 4];
        for y in [5usize, 30, 55] {
            for x in 10..110 {
                paint(&mut buf, w, x, y);
            }
        }
        for x in [10usize, 110] {
            for y in 5..56 {
                paint(&mut buf, w, x, y);
            }
        }
        // 内部分隔线只画在下面那一行带里
        for x in [50usize, 80] {
            for y in 30..56 {
                paint(&mut buf, w, x, y);
            }
        }
        let lg = lines::detect(
            &Pixels::new(&buf, w, h),
            Area { x0: 0, y0: 0, x1: w, y1: h },
        )
        .expect("该检出网格");
        assert_eq!((lg.rows(), lg.cols()), (2, 3));
        let boxes = vec![
            at(15.0, 36.0, 30.0, 14.0, "甲"),
            at(55.0, 36.0, 20.0, 14.0, "乙"),
            at(85.0, 36.0, 20.0, 14.0, "丙"),
        ];
        let grid = build_grid_from_lines(&lg, &boxes);
        let html = grid.to_html();
        assert!(html.contains("<td colspan=\"3\"></td>"), "{html}");
        // 对齐要 CSS 和老属性各写一份：只写 style 时 WPS 一类会忽略
        assert!(
            html.contains("<td align=\"center\" style=\"text-align:center\">甲</td>"),
            "{html}"
        );
        assert_eq!(grid.to_tsv(), "\t\t\n甲\t乙\t丙");
        html_fits(&html, 3).unwrap_or_else(|e| panic!("{e}\n{html}"));
    }

    #[test]
    fn 三行两列的框投影成三行两列() {
        let boxes = vec![
            cell(0, 0, "编号"),
            cell(1, 0, "姓名"),
            cell(0, 1, "0001"),
            cell(1, 1, "梁海平"),
            cell(0, 2, "0002"),
            cell(1, 2, "欧海军"),
        ];
        let grid = build_grid(&boxes);
        assert_eq!((grid.rows, grid.cols), (3, 2));
        assert_eq!(grid.to_tsv(), "编号\t姓名\n0001\t梁海平\n0002\t欧海军");
    }

    #[test]
    fn 成绩表整表投影成十四行八列且落位正确() {
        // 复刻他那张图：标题一格 + 表头八格 + 12 行数据
        let mut boxes = vec![at(20.0, 22.0, 120.0, 16.0, "学生成绩表")];
        for (c, t) in ["编号", "姓名", "政治", "语文", "数学", "英语", "物理", "化学"]
            .iter()
            .enumerate()
        {
            boxes.push(cell(c, 1, t));
        }
        for r in 0..12 {
            for c in 0..8 {
                let text = match c {
                    0 => format!("{:04}", r + 1),
                    1 => format!("姓名{r}"),
                    _ => format!("{}", 61 + c),
                };
                boxes.push(cell(c, r + 2, &text));
            }
        }
        let grid = build_grid(&boxes);
        assert_eq!((grid.rows, grid.cols), (14, 8));
        let tsv = grid.to_tsv();
        let rows: Vec<&str> = tsv.split('\n').collect();
        assert_eq!(rows[1], "编号\t姓名\t政治\t语文\t数学\t英语\t物理\t化学");
        assert_eq!(rows[2], "0001\t姓名0\t63\t64\t65\t66\t67\t68");
        assert_eq!(rows[13], "0012\t姓名11\t63\t64\t65\t66\t67\t68");
    }

    #[test]
    fn 跨列的宽框记成colspan且被覆盖格不再出格() {
        let boxes = vec![
            at(20.0, 20.0, 170.0, 14.0, "合计"),
            cell(0, 1, "甲"),
            cell(1, 1, "乙"),
            cell(2, 1, "丙"),
        ];
        let grid = build_grid(&boxes);
        assert_eq!((grid.rows, grid.cols), (2, 3));
        let html = grid.to_html();
        assert!(html.contains("<td colspan=\"3\">合计</td>"), "{html}");
        // 第一行只有一个合并格 + 第二行三个格 = 4 个 td
        assert_eq!(html.matches("<td").count(), 4, "{html}");
        html_fits(&html, 3).unwrap_or_else(|e| panic!("{e}\n{html}"));
        assert_eq!(grid.to_tsv(), "合计\t\t\n甲\t乙\t丙");
    }

    #[test]
    fn 跨行的高框记成rowspan() {
        let boxes = vec![
            at(20.0, 20.0, 60.0, 62.0, "竖合并"),
            at(106.0, 20.0, 60.0, 14.0, "右上"),
            at(106.0, 45.0, 60.0, 14.0, "右下"),
        ];
        let grid = build_grid(&boxes);
        let html = grid.to_html();
        assert!(html.contains("rowspan=\"2\""), "{html}");
        assert_eq!(grid.to_tsv(), "竖合并\t右上\n\t右下");
    }

    #[test]
    fn 框底边压在下一行起点上不算跨行() {
        // 检测框常常占满整行高（25px），底边正好等于下一带起点；
        // 不加回缩就会把每一格都标成 rowspan=2（真图验收时踩到）
        let boxes = vec![
            at(20.0, 20.0, 60.0, 25.0, "甲"),
            at(120.0, 20.0, 60.0, 25.0, "丙"),
            at(20.0, 45.0, 60.0, 25.0, "乙"),
            at(120.0, 45.0, 60.0, 25.0, "丁"),
        ];
        let grid = build_grid(&boxes);
        assert!(!grid.to_html().contains("rowspan"), "{}", grid.to_html());
        assert_eq!(grid.to_tsv(), "甲\t丙\n乙\t丁");
    }

    #[test]
    fn 同格内两个相邻框合成一段文本() {
        // 间隙 4px < 阈值（14×0.5=7px）→ 同一列带
        let boxes = vec![
            at(20.0, 20.0, 40.0, 14.0, "重要"),
            at(64.0, 20.0, 40.0, 14.0, "提示"),
            at(200.0, 20.0, 40.0, 14.0, "另一格"),
        ];
        let grid = build_grid(&boxes);
        assert_eq!((grid.rows, grid.cols), (1, 2));
        assert_eq!(grid.to_tsv(), "重要 提示\t另一格");
    }

    #[test]
    fn 带前导零的格加文本格式普通数字不加() {
        let boxes = vec![
            cell(0, 0, "0001"),
            cell(1, 0, "123456789012"),
            cell(2, 0, "89"),
            cell(3, 0, "0.5"),
        ];
        let html = build_grid(&boxes).to_html();
        assert!(
            html.contains("<td style=\"mso-number-format:'\\@'\">0001</td>"),
            "{html}"
        );
        assert!(
            html.contains("<td style=\"mso-number-format:'\\@'\">123456789012</td>"),
            "{html}"
        );
        assert!(html.contains(">89</td>"), "{html}");
        assert!(html.contains(">0.5</td>"), "{html}");
        assert_eq!(html.matches("mso-number-format").count(), 2, "{html}");
    }

    #[test]
    fn 文本按html转义() {
        let html = build_grid(&[cell(0, 0, "A & B <tag>")]).to_html();
        assert!(html.contains("A &amp; B &lt;tag&gt;"), "{html}");
    }

    #[test]
    fn 空输入判空且不崩() {
        let grid = build_grid(&[]);
        assert!(grid.is_empty());
        assert_eq!(grid.to_tsv(), "");
    }

    #[test]
    fn 短缓冲与空尺寸判错而不越界() {
        // 这个入口原先没查缓冲长度：to_rgb_padded 与 Pixels::lum 都按
        // width*height*4 直接下标，短缓冲会 panic，而跨 cxx 边界的 panic
        // 可能直接 abort（进程无声消失）。检查在加载模型之前，所以这三条
        // 不需要 assets/models 也在毫秒内返回。
        assert!(recognize(&[], 0, 0).is_err(), "空尺寸应报错");
        assert!(recognize(&[0u8; 16], 64, 64).is_err(), "缓冲远短于需求应报错");
        assert!(
            recognize(&[0u8; 64 * 64 * 4 - 1], 64, 64).is_err(),
            "差一个字节也应报错"
        );
    }

    #[test]
    fn 半行高阈值把相邻两行与同行两格分开() {
        // 字高 14 → 阈值 7：y 间隙 11px 算两行
        let two_rows = vec![
            at(20.0, 20.0, 60.0, 14.0, "上"),
            at(20.0, 45.0, 60.0, 14.0, "下"),
        ];
        assert_eq!(build_grid(&two_rows).rows, 2);
        // 列缝由行内空档投票：40px 的间隙算两格（3px 那种算同一格，见上一个用例）
        let one_row = vec![
            at(20.0, 20.0, 60.0, 14.0, "左"),
            at(120.0, 20.0, 60.0, 14.0, "右"),
        ];
        let grid = build_grid(&one_row);
        assert_eq!((grid.rows, grid.cols), (1, 2));
    }
}
