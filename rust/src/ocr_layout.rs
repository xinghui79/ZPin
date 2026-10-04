//! OCR 版式归并：把逐行识别结果整理成阅读顺序正确、软换行不碎裂、列表不粘连的文本。
//!
//! 判据按本项目的输入形态做了三处删改：
//!   - 同一视觉行的合并要求横向紧邻（间隙 ≤1.5 倍行高）。没有表格区域判据能把
//!     误并的列拆开，所以只能用保守阈值：左右两排气泡若被并成一行，输出就是彻底错乱的。
//!   - 去掉单元格判据（它只在横向合并出现同行多框时才有意义）。
//!   - 不做分栏区域切分与分栏排序：两栏版式仍按"从上到下、再从左到右"输出，
//!     列亲和判据保证左右两栏不会被粘在一起。

/// 参与归并的一行识别结果：文本 + 外接框（原图像素坐标）。
#[derive(Clone)]
pub struct LayoutLine {
    pub text: String,
    pub left: f64,
    pub top: f64,
    pub right: f64,
    pub bottom: f64,
}

/// 归并出来的"视觉行"：一行内被识别成多个相邻框（行中有加粗、图标带标签）时合成一行。
#[derive(Clone)]
struct Row {
    text: String,
    left: f64,
    top: f64,
    right: f64,
    bottom: f64,
}

/// 度量：行高与正常行间距。所有阈值都以行高为尺子，因此与图像分辨率、DPI 无关。
struct Metrics {
    line_height: f64,
    normal_line_gap: f64,
}

/// 一段：粘连在一起的若干视觉行。
struct Paragraph {
    rows: Vec<Row>,
}

impl LayoutLine {
    fn height(&self) -> f64 {
        self.bottom - self.top
    }

    fn center_y(&self) -> f64 {
        (self.top + self.bottom) / 2.0
    }
}

impl Row {
    fn from_line(l: &LayoutLine) -> Row {
        Row {
            text: l.text.trim().to_string(),
            left: l.left,
            top: l.top,
            right: l.right,
            bottom: l.bottom,
        }
    }

    fn width(&self) -> f64 {
        self.right - self.left
    }

    fn height(&self) -> f64 {
        self.bottom - self.top
    }

    fn center_y(&self) -> f64 {
        (self.top + self.bottom) / 2.0
    }
}

impl Paragraph {
    fn last(&self) -> &Row {
        // 段落创建时必带一行，空段落不存在
        self.rows.last().expect("段落至少有一行")
    }

    /// 正文左缘：除首行外各行左缘的中位数（首行常有缩进，不能算进去）。
    fn body_left(&self) -> f64 {
        if self.rows.len() == 1 {
            return self.rows[0].left;
        }
        median(self.rows[1..].iter().map(|r| r.left))
    }

    /// 明显比本段典型行宽短一截 —— 段末行的特征。
    fn is_short_line(&self, row: &Row, m: &Metrics) -> bool {
        if self.rows.len() < 2 {
            return false;
        }
        let typical = median(self.rows.iter().map(|r| r.width()));
        typical >= m.line_height * 6.0
            && row.width() <= typical * 0.72
            && typical - row.width() >= m.line_height * 2.2
    }
}

/// 把识别行归并成段落文本，段落之间用单个换行分隔（一段一行）。
pub fn layout_text(lines: &[LayoutLine]) -> String {
    let rows = build_rows(lines);
    if rows.is_empty() {
        return String::new();
    }
    let m = Metrics::build(&rows);

    let mut paragraphs: Vec<Paragraph> = Vec::new();
    for row in &rows {
        let mut best: Option<(usize, f64)> = None;
        for (i, p) in paragraphs.iter().enumerate() {
            let prev = p.last();
            // 只往下接：往回并会把阅读顺序搞乱
            if row.top < prev.top {
                continue;
            }
            if append_confidence(p, row, &m).is_none() {
                continue;
            }
            let score = horizontal_overlap(prev, row) * 3.0
                + 1.0 - ((prev.left - row.left).abs() / m.line_height.max(1.0)).min(1.0)
                + 1.0 - (vertical_gap(prev, row) / m.line_height.max(1.0)).min(1.0);
            if best.map(|(_, s)| score > s).unwrap_or(true) {
                best = Some((i, score));
            }
        }
        match best {
            Some((i, _)) => paragraphs[i].rows.push(row.clone()),
            None => paragraphs.push(Paragraph {
                rows: vec![row.clone()],
            }),
        }
    }

    paragraphs.sort_by(|a, b| {
        let (x, y) = (a.rows.first(), b.rows.first());
        // 段落至少有一行
        let (x, y) = (x.expect("段落至少有一行"), y.expect("段落至少有一行"));
        x.top.total_cmp(&y.top).then(x.left.total_cmp(&y.left))
    });
    let blocks: Vec<String> = paragraphs
        .iter()
        .map(|p| join_texts(&p.rows.iter().map(|r| r.text.as_str()).collect::<Vec<_>>()))
        .collect();
    blocks
        .iter()
        .filter(|b| !b.trim().is_empty())
        .cloned()
        .collect::<Vec<String>>()
        .join("\n")
}

/// 同一视觉行且横向紧邻的识别框合成一行，结果按阅读顺序排好。
fn build_rows(lines: &[LayoutLine]) -> Vec<Row> {
    let mut items: Vec<&LayoutLine> = lines
        .iter()
        .filter(|l| !l.text.trim().is_empty())
        .collect();
    items.sort_by(|a, b| {
        a.center_y()
            .total_cmp(&b.center_y())
            .then(a.left.total_cmp(&b.left))
    });

    let mut rows: Vec<Row> = Vec::new();
    for item in items {
        let mut best: Option<(usize, f64, f64)> = None;
        for (i, row) in rows.iter().enumerate() {
            if !same_visual_line(row, item) {
                continue;
            }
            let overlap = vertical_overlap(row.bottom, row.top, item.bottom, item.top, row.height().min(item.height()));
            let center_delta = (row.center_y() - item.center_y()).abs();
            let better = match best {
                None => true,
                Some((_, best_overlap, best_delta)) => {
                    overlap > best_overlap || (overlap == best_overlap && center_delta < best_delta)
                }
            };
            if better {
                best = Some((i, overlap, center_delta));
            }
        }
        match best {
            Some((i, _, _)) => attach_to_row(&mut rows[i], item),
            None => rows.push(Row::from_line(item)),
        }
    }

    rows.sort_by(|a, b| a.top.total_cmp(&b.top).then(a.left.total_cmp(&b.left)));
    rows
}

/// 把框并进已有视觉行：左右缘按实际位置扩展，文本保持在行内的左右顺序。
fn attach_to_row(row: &mut Row, item: &LayoutLine) {
    let text = item.text.trim();
    if item.right <= row.left {
        row.text = join_texts(&[text, row.text.as_str()]);
    } else {
        row.text = join_texts(&[row.text.as_str(), text]);
    }
    row.left = row.left.min(item.left);
    row.right = row.right.max(item.right);
    row.top = row.top.min(item.top);
    row.bottom = row.bottom.max(item.bottom);
}

impl Metrics {
    fn build(rows: &[Row]) -> Metrics {
        let line_height = median(rows.iter().map(|r| r.height())).max(1.0);
        Metrics {
            line_height,
            normal_line_gap: estimate_normal_gap(rows, line_height),
        }
    }
}

/// 正常行间距：相邻视觉行（竖向不重叠）之间那些没超过 0.65 倍行高的间隙取中位数。
fn estimate_normal_gap(rows: &[Row], line_height: f64) -> f64 {
    if rows.len() < 2 {
        return 0.0;
    }
    let gaps: Vec<f64> = rows
        .windows(2)
        .filter(|w| {
            vertical_overlap(w[0].bottom, w[0].top, w[1].bottom, w[1].top, w[0].height().min(w[1].height()))
                <= 0.25
        })
        .map(|w| vertical_gap(&w[0], &w[1]))
        .filter(|g| *g <= line_height * 0.65)
        .collect();
    median(gaps)
}

/// 核心谓词：`cur` 能否接到段落末行后面；能则给出置信度。
fn append_confidence(p: &Paragraph, cur: &Row, m: &Metrics) -> Option<f64> {
    let prev = p.last();

    let gap = vertical_gap(prev, cur);
    if gap > m.line_height * 1.25 {
        return None;
    }

    // 列表项永远另起一行，绝不粘到上一行尾巴上
    if is_list_start(&cur.text) {
        return None;
    }
    if is_list_start(&prev.text) && cur.left > prev.left + m.line_height * 0.8 {
        // 列表项自身折行要接回本项
        return Some(0.95);
    }

    // 字号差一档就是标题和正文，不是同一段的换行
    let (ha, hb) = (prev.height(), cur.height());
    if ha.max(hb) > ha.min(hb) * 1.45 {
        return None;
    }

    let overlap = horizontal_overlap(prev, cur);
    let left_delta = (prev.left - cur.left).abs();
    // 列亲和：横向大幅重叠或左缘对齐，否则就是并排的两栏
    if !(overlap >= 0.45 || left_delta <= m.line_height * 1.2) {
        return None;
    }

    if should_merge_hyphenated(&prev.text, &cur.text) {
        return Some(0.95);
    }
    if looks_like_paragraph_break(p, prev, cur, m, gap, overlap, left_delta) {
        return None;
    }
    if looks_like_standalone_control(prev) || looks_like_standalone_control(cur) {
        return None;
    }

    let indent_delta = cur.left - prev.left;
    if indent_delta.abs() > m.line_height * 2.5 && overlap < 0.7 {
        return None;
    }

    let left_affinity = 1.0 - (left_delta / (m.line_height * 1.2).max(1.0)).min(1.0);
    let gap_affinity = 1.0 - (gap / (m.line_height * 1.25).max(1.0)).min(1.0);
    let height_affinity = ha.min(hb) / ha.max(hb).max(1.0);
    let confidence = (overlap.max(left_affinity) * 0.45
        + gap_affinity * 0.35
        + height_affinity * 0.20)
        .clamp(0.0, 1.0);
    if confidence >= 0.48 {
        Some(confidence)
    } else {
        None
    }
}

/// 段末行的种种迹象：上一行太短且回到正文左缘、当前行像首行缩进、句末标点、大写开头。
fn looks_like_paragraph_break(
    p: &Paragraph,
    prev: &Row,
    cur: &Row,
    m: &Metrics,
    gap: f64,
    overlap: f64,
    left_delta: f64,
) -> bool {
    if is_list_start(&prev.text) || is_list_start(&cur.text) {
        return false;
    }

    let same_left_edge = left_delta <= m.line_height * 0.8;
    let current_is_indented = cur.left > prev.left + m.line_height * 0.8;
    let current_returns_to_previous_left = cur.left <= prev.left + m.line_height * 0.4;
    let body_left = p.body_left();
    let previous_returns_to_body_left = prev.left <= body_left + m.line_height * 0.45;
    let indent = cur.left - body_left;
    let current_looks_like_first_line_indent =
        indent >= m.line_height * 0.55 && indent <= m.line_height * 2.5;
    let previous_is_short_line = prev.width() <= cur.width() * 0.82 || p.is_short_line(prev, m);

    if previous_is_short_line
        && previous_returns_to_body_left
        && current_looks_like_first_line_indent
        && overlap >= 0.35
    {
        return true;
    }

    let break_threshold = (m.line_height * 0.72)
        .max(m.normal_line_gap.min(m.line_height * 0.35) + m.line_height * 0.45);
    if gap <= break_threshold {
        return false;
    }

    if ends_with_sentence_ending(&prev.text) && (same_left_edge || current_is_indented) {
        return true;
    }
    if starts_with_upper_latin(&cur.text) && (same_left_edge || current_looks_like_first_line_indent)
    {
        return true;
    }
    previous_is_short_line && current_returns_to_previous_left && overlap >= 0.45
}

/// 像按钮/菜单项：短、行内无空格、宽高比小、没有句末标点。
/// 中文行按 6 字左右切在这条线上，正好拦住菜单项竖排粘连；更长的正文行不受影响。
fn looks_like_standalone_control(row: &Row) -> bool {
    if is_list_start(&row.text) {
        return false;
    }
    let compact: String = row.text.chars().filter(|c| !c.is_whitespace()).collect();
    if compact.chars().count() > 14 {
        return false;
    }
    if row.text.chars().any(|c| c.is_whitespace()) {
        return false;
    }
    let width_ratio = row.width() / row.height().max(1.0);
    width_ratio <= 6.2 && !has_sentence_ending(&row.text)
}

/// 英文连字符断词：`com-` 接 `puter` 得 `computer`。
fn should_merge_hyphenated(prev: &str, cur: &str) -> bool {
    let prev_chars: Vec<char> = prev.chars().collect();
    let cur_trimmed = cur.trim_start();
    if prev_chars.len() < 2 {
        return false;
    }
    let tail = match cur_trimmed.chars().next() {
        Some(c) => c,
        None => return false,
    };
    prev_chars[prev_chars.len() - 1] == '-'
        && is_latin_letter(prev_chars[prev_chars.len() - 2])
        && is_latin_letter(tail)
        && tail.is_lowercase()
}

/// 列表起始：项目符号，或 "1." "1、" "1)" "(1)" "1 -" 这类序号。
fn is_list_start(text: &str) -> bool {
    let trimmed = text.trim_start();
    let first = match trimmed.chars().next() {
        Some(c) => c,
        None => return false,
    };
    if matches!(first, '-' | '*' | '+' | '•' | '·' | '●' | '▪') {
        return match trimmed.chars().nth(1) {
            None => true,
            Some(second) => second.is_whitespace(),
        };
    }
    ordered_list_number(trimmed)
}

/// 只认带明确分隔符的序号，"12 人排队" 这种不能算列表项。
fn ordered_list_number(text: &str) -> bool {
    let (body, parenthesized) = match text
        .strip_prefix('(')
        .or_else(|| text.strip_prefix('（'))
    {
        Some(rest) => (rest.trim_start(), true),
        None => (text, false),
    };
    let digits = body.chars().take_while(|c| c.is_ascii_digit()).count();
    if digits == 0 || digits > 3 {
        return false;
    }
    let rest = &body[digits..];
    if parenthesized {
        return rest.starts_with(')') || rest.starts_with('）');
    }
    match rest.chars().next() {
        // 小数点后面还跟着数字的是数值（"3.14 是圆周率"），不是序号
        Some('.' | '．') => !matches!(rest.chars().nth(1), Some(c) if c.is_ascii_digit()),
        Some('、' | ')' | '）' | '-' | '–') => true,
        _ => false,
    }
}

fn ends_with_sentence_ending(text: &str) -> bool {
    for c in text.chars().rev() {
        if c.is_whitespace()
            || matches!(c, '"' | '\'' | ')' | ']' | '}' | '”' | '’' | '）' | '】' | '」')
        {
            continue;
        }
        return matches!(
            c,
            '.' | '!' | '?' | ';' | ':' | '。' | '！' | '？' | '；' | '：'
        );
    }
    false
}

fn has_sentence_ending(text: &str) -> bool {
    text.chars().any(|c| {
        matches!(
            c,
            '.' | '!' | '?' | ';' | ':' | '。' | '！' | '？' | '；' | '：'
        )
    })
}

fn starts_with_upper_latin(text: &str) -> bool {
    for c in text.trim_start().chars() {
        if matches!(c, '"' | '\'' | '(' | '[' | '{' | '“' | '‘' | '（') {
            continue;
        }
        return c.is_ascii_uppercase();
    }
    false
}

fn is_latin_letter(c: char) -> bool {
    c.is_ascii_alphabetic()
}

fn is_cjk(c: char) -> bool {
    matches!(c as u32,
        0x2E80..=0x303F   // 部首、中日韩标点
        | 0x3040..=0x30FF // 平假名、片假名
        | 0x3400..=0x4DBF
        | 0x4E00..=0x9FFF
        | 0xAC00..=0xD7AF // 谚文
        | 0xF900..=0xFAFF
        | 0xFF00..=0xFF65) // 全角字符
}

fn is_punct(c: char) -> bool {
    c.is_ascii_punctuation() || matches!(c as u32, 0x2E80..=0x303F | 0xFF00..=0xFF65)
}

/// 按顺序拼接文本片段：连字符断词直接接上，其余按 CJK/标点决定是否补空格。
fn join_texts(parts: &[&str]) -> String {
    let mut out = parts.first().copied().unwrap_or_default().to_string();
    for next in parts.iter().skip(1) {
        if should_merge_hyphenated(out.as_str(), next) {
            out.pop();
            out.push_str(next);
            continue;
        }
        if needs_space(out.as_str(), next) {
            out.push(' ');
        }
        out.push_str(next);
    }
    out
}

/// 交界两侧都是拉丁词字符才补空格：中文之间、中英交界都不补。
fn needs_space(prev: &str, cur: &str) -> bool {
    if prev.trim().is_empty() || cur.trim().is_empty() {
        return false;
    }
    let left = match prev.chars().last() {
        Some(c) => c,
        None => return false,
    };
    let right = match cur.chars().next() {
        Some(c) => c,
        None => return false,
    };
    if left.is_whitespace() || right.is_whitespace() {
        return false;
    }
    if is_cjk(left) || is_cjk(right) {
        return false;
    }
    !(is_punct(left) || is_punct(right))
}

/// 竖向重叠率：两个框在垂直方向上压了多少。
fn vertical_overlap(bottom_a: f64, top_a: f64, bottom_b: f64, top_b: f64, min_height: f64) -> f64 {
    let overlap = bottom_a.min(bottom_b) - top_a.max(top_b);
    if overlap <= 0.0 {
        return 0.0;
    }
    overlap / min_height.max(1.0)
}

/// 同一视觉行：竖向压过半，或中线靠得够近，并且横向紧邻。
fn same_visual_line(row: &Row, l: &LayoutLine) -> bool {
    let near_in_vertical = vertical_overlap(
        row.bottom,
        row.top,
        l.bottom,
        l.top,
        row.height().min(l.height()),
    ) >= 0.55
        || (row.center_y() - l.center_y()).abs() <= row.height().min(l.height()) * 0.45;
    if !near_in_vertical {
        return false;
    }
    let gap = if l.left >= row.right {
        l.left - row.right
    } else if row.left >= l.right {
        row.left - l.right
    } else {
        0.0
    };
    gap <= row.height().max(l.height()) * 1.5
}

fn vertical_gap(prev: &Row, cur: &Row) -> f64 {
    (cur.top - prev.bottom).max(0.0)
}

fn horizontal_overlap(a: &Row, b: &Row) -> f64 {
    let overlap = (a.right.min(b.right) - a.left.max(b.left)).max(0.0);
    overlap / a.width().min(b.width()).max(1.0)
}

fn median(values: impl IntoIterator<Item = f64>) -> f64 {
    let mut ordered: Vec<f64> = values.into_iter().filter(|v| *v > 0.0).collect();
    if ordered.is_empty() {
        return 0.0;
    }
    ordered.sort_by(f64::total_cmp);
    let middle = ordered.len() / 2;
    if ordered.len() % 2 == 0 {
        (ordered[middle - 1] + ordered[middle]) / 2.0
    } else {
        ordered[middle]
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn line(text: &str, left: f64, top: f64, right: f64, bottom: f64) -> LayoutLine {
        LayoutLine {
            text: text.to_string(),
            left,
            top,
            right,
            bottom,
        }
    }

    #[test]
    fn 铺满右边界的中文软换行合成一段() {
        let lines = vec![
            line("这是一段被排版折成两行的中文文字内容", 100.0, 100.0, 500.0, 120.0),
            line("它的上一行已经铺满到右边界", 100.0, 126.0, 420.0, 146.0),
        ];
        assert_eq!(
            layout_text(&lines),
            "这是一段被排版折成两行的中文文字内容它的上一行已经铺满到右边界"
        );
    }

    #[test]
    fn 列表项各自成行不会被粘连() {
        let lines = vec![
            line("- 苹果", 100.0, 100.0, 180.0, 120.0),
            line("- 香蕉", 100.0, 124.0, 180.0, 144.0),
            line("- 橘子", 100.0, 148.0, 180.0, 168.0),
        ];
        assert_eq!(layout_text(&lines), "- 苹果\n- 香蕉\n- 橘子");
    }

    #[test]
    fn 序号列表的折行接回本项() {
        let lines = vec![
            line("1. 第一项的标题文字", 100.0, 100.0, 300.0, 120.0),
            line("续排在本项下方", 130.0, 124.0, 300.0, 144.0),
        ];
        assert_eq!(layout_text(&lines), "1. 第一项的标题文字续排在本项下方");
    }

    #[test]
    fn 英文连字符断词去掉连字符() {
        let lines = vec![
            line("this word is com-", 100.0, 100.0, 300.0, 120.0),
            line("puter and goes on", 100.0, 124.0, 300.0, 144.0),
        ];
        assert_eq!(layout_text(&lines), "this word is computer and goes on");
    }

    #[test]
    fn 拉丁词之间补空格中文之间不补() {
        let latin = vec![
            line("hello world", 100.0, 100.0, 300.0, 120.0),
            line("next line here", 100.0, 124.0, 300.0, 144.0),
        ];
        assert_eq!(layout_text(&latin), "hello world next line here");

        let cjk = vec![
            line("前面是中文内容", 100.0, 100.0, 300.0, 120.0),
            line("后面也是中文", 100.0, 124.0, 300.0, 144.0),
        ];
        assert_eq!(layout_text(&cjk), "前面是中文内容后面也是中文");
    }

    #[test]
    fn 两栏版式左右两块不互粘() {
        let lines = vec![
            line("左栏第一行文字内容", 100.0, 100.0, 300.0, 120.0),
            line("右栏第一行文字内容", 500.0, 100.0, 700.0, 120.0),
            line("左栏第二行文字内容", 100.0, 124.0, 300.0, 144.0),
            line("右栏第二行文字内容", 500.0, 124.0, 700.0, 144.0),
        ];
        assert_eq!(
            layout_text(&lines),
            "左栏第一行文字内容左栏第二行文字内容\n右栏第一行文字内容右栏第二行文字内容"
        );
    }

    #[test]
    fn 表格单元格逐格成行不拼成句子() {
        // 分格靠列亲和判据拦住；真正的表格结构化是第 2 条的活
        let lines = vec![
            line("姓名", 100.0, 100.0, 160.0, 120.0),
            line("年龄", 300.0, 100.0, 360.0, 120.0),
            line("城市", 500.0, 100.0, 560.0, 120.0),
            line("张三", 100.0, 124.0, 160.0, 144.0),
            line("18", 300.0, 124.0, 330.0, 144.0),
            line("苏州", 500.0, 124.0, 560.0, 144.0),
        ];
        assert_eq!(layout_text(&lines), "姓名\n年龄\n城市\n张三\n18\n苏州");
    }

    #[test]
    fn 同行紧邻的两个框合成一行() {
        // 行内被识别成两段（加粗、图标旁标签），间隙远小于行高
        let lines = vec![
            line("重要提示", 100.0, 100.0, 180.0, 120.0),
            line("请核对金额", 186.0, 100.0, 320.0, 120.0),
        ];
        assert_eq!(layout_text(&lines), "重要提示请核对金额");
    }

    #[test]
    fn 字号差一档的标题与正文不合并() {
        let lines = vec![
            line("这是一个较长的标题", 100.0, 80.0, 500.0, 130.0),
            line("正文第一行内容比较长一些", 100.0, 136.0, 500.0, 156.0),
        ];
        assert_eq!(layout_text(&lines), "这是一个较长的标题\n正文第一行内容比较长一些");
    }

    #[test]
    fn 识别乱序输入也按阅读顺序输出() {
        let lines = vec![
            line("第二行文字内容", 100.0, 130.0, 300.0, 150.0),
            line("第三行文字内容", 100.0, 160.0, 300.0, 180.0),
            line("第一行文字内容", 100.0, 100.0, 300.0, 120.0),
        ];
        assert_eq!(
            layout_text(&lines),
            "第一行文字内容第二行文字内容第三行文字内容"
        );
    }

    #[test]
    fn 行间空档把两段分开() {
        let lines = vec![
            line("第一段说完了", 100.0, 100.0, 300.0, 120.0),
            line("第二段开始", 100.0, 160.0, 300.0, 180.0),
        ];
        assert_eq!(layout_text(&lines), "第一段说完了\n第二段开始");
    }

    #[test]
    fn 段末短行加首行缩进算分段() {
        // 上一行没铺满（段末），下一行又缩进两个字符：判成两段
        let lines = vec![
            line("第一段正文内容在这里结束", 100.0, 100.0, 300.0, 120.0),
            line("第二段是另起的一行正文内容", 140.0, 150.0, 400.0, 170.0),
        ];
        assert_eq!(layout_text(&lines), "第一段正文内容在这里结束\n第二段是另起的一行正文内容");
    }

    #[test]
    fn 竖排菜单项保持逐行() {
        let lines = vec![
            line("复制", 100.0, 100.0, 160.0, 120.0),
            line("粘贴", 100.0, 124.0, 160.0, 144.0),
            line("删除", 100.0, 148.0, 160.0, 168.0),
        ];
        assert_eq!(layout_text(&lines), "复制\n粘贴\n删除");
    }

    #[test]
    fn 空输入纯空白与单行() {
        assert_eq!(layout_text(&[]), "");
        assert_eq!(layout_text(&[line("   ", 0.0, 0.0, 10.0, 10.0)]), "");
        assert_eq!(layout_text(&[line("A", 10.0, 10.0, 20.0, 30.0)]), "A");
    }

    #[test]
    fn 列表与标点判定不误伤正文() {
        assert!(is_list_start("- 条目"));
        assert!(is_list_start("1. 条目"));
        assert!(is_list_start("2、条目"));
        assert!(is_list_start("(3) 条目"));
        assert!(!is_list_start("12 人排队"));
        assert!(!is_list_start("这是一段普通中文正文"));
        assert!(!is_list_start("3.14 是圆周率"));
    }
}
