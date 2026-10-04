//! 表格线检测：从像素里找出表格的横竖边框，还原成真正的格子。
//!
//! 为什么在 ZPin 里划算：输入永远是屏幕截图 —— 线是 1~2px、横平竖直、颜色均匀，
//! 没有照片/扫描件那些透视与光照问题。零模型、零字节、纯 CPU，而且是唯一能免费
//! 给出合并单元格与对齐信息的来源（OCR 框做不到：检测框左右各有几像素富余，
//! 吞掉的对齐信号比留下的多）。
//!
//! 检不出来（无边框表、线太浅、只有一两条线）时返回 None，由 `table.rs` 回落到
//! OCR 框投影。

/// BGRA（预乘）缓冲的只读视图；行距固定 `width*4`，与 `rcore` 送进来的约定一致。
pub struct Pixels<'a> {
    data: &'a [u8],
    width: usize,
    height: usize,
}

impl<'a> Pixels<'a> {
    pub fn new(data: &'a [u8], width: usize, height: usize) -> Pixels<'a> {
        Pixels {
            data,
            width,
            height,
        }
    }

    /// 亮度 0..=255；越界按白算，省掉调用方每处边界判断。
    fn lum(&self, x: usize, y: usize) -> u32 {
        if x >= self.width || y >= self.height {
            return 255;
        }
        let i = (y * self.width + x) * 4;
        // GDI 的 BGRA 小端序；系数和 = 256，右移 8 位即除以 256
        (77 * u32::from(self.data[i + 2]) + 150 * u32::from(self.data[i + 1]) + 29 * u32::from(self.data[i])) >> 8
    }
}

/// 待检测的矩形（含左上、不含右下）。
#[derive(Clone, Copy)]
pub struct Area {
    pub x0: usize,
    pub y0: usize,
    pub x1: usize,
    pub y1: usize,
}

/// 比背景暗这么多才算"有墨"：Excel/WPS 的网格线是浅灰（约 217），
/// 绝对阈值会漏，所以相对背景量。
const CONTRAST: u32 = 22;
/// 一条线至少要连续跨过多大比例的区域。
///
/// 不能用"暗像素占比"：一列竖着排的数字覆盖率能有七成七（真图实测，把 8 列
/// 表格检成 42 列）。真边框是一整条连续游程，字堆每行之间必有空档，
/// 所以量最长游程而不是总量。先做闭运算（向两侧各扩 CLOSING 像素）补掉抗锯齿缝。
const MIN_SPAN: f64 = 0.35;
const CLOSING: usize = 2;
/// 线最粗几个像素。再粗就是一整行文字（CJK 字面高十几像素）而不是线。
const MAX_THICK: usize = 3;
/// 深色填充行至少这么厚才按「上下沿是边框」处理（表头底色实测 25 像素）。
/// 比 MAX_THICK 厚又不到这个数的按粗线丢弃 —— 那是条重边框，不是一格底色。
const MIN_FILL: usize = 8;
/// 实心判据的分子：分母固定 5，即"横跨八成以上幅面"。
const SOLID: usize = 4;
/// 一条线至少在这么多个带里真画过才算数。
const MIN_LINE_BANDS: usize = 2;

/// 一个单元格：左上角行列 + 合并跨度 + 像素矩形。
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CellRect {
    pub row: usize,
    pub col: usize,
    pub row_span: usize,
    pub col_span: usize,
    pub x0: f64,
    pub y0: f64,
    pub x1: f64,
    pub y1: f64,
}

/// 线还原出的网格：线的位置 + 每段线在不在。
pub struct LineGrid {
    xs: Vec<f64>,
    ys: Vec<f64>,
    /// has_v[行带][竖线序号]
    has_v: Vec<Vec<bool>>,
    /// has_h[横线序号][列带]
    has_h: Vec<Vec<bool>>,
}

impl LineGrid {
    pub fn rows(&self) -> usize {
        self.ys.len().saturating_sub(1)
    }

    pub fn cols(&self) -> usize {
        self.xs.len().saturating_sub(1)
    }

    /// 网格的外接矩形（线心坐标）：调用方拿它判断有没有文字落在格子外面。
    pub fn bounds(&self) -> (f64, f64, f64, f64) {
        (
            self.xs[0],
            self.ys[0],
            *self.xs.last().unwrap_or(&self.xs[0]),
            *self.ys.last().unwrap_or(&self.ys[0]),
        )
    }

    /// 横坐标落在第几列带；越界贴到首列/末列。
    pub fn col_of(&self, x: f64) -> usize {
        let c = self.xs.iter().filter(|s| **s <= x).count().saturating_sub(1);
        c.min(self.cols().saturating_sub(1))
    }

    /// 纵坐标落在第几行带；越界贴到首行/末行。
    pub fn row_of(&self, y: f64) -> usize {
        let r = self.ys.iter().filter(|s| **s <= y).count().saturating_sub(1);
        r.min(self.rows().saturating_sub(1))
    }

    /// 第 r 行带、第 c 列带的格子矩形（含合并撑开的跨度）。
    /// 返回 None 表示这一格被左上角某个合并格盖住了。
    pub fn cells(&self) -> Vec<CellRect> {
        let (rows, cols) = (self.rows(), self.cols());
        let mut covered = vec![vec![false; cols]; rows];
        let mut out = Vec::new();
        for r in 0..rows {
            for c in 0..cols {
                if covered[r][c] {
                    continue;
                }
                // 只沿"这一段线不存在"的方向撑开：线被合并格吃掉才叫合并
                let mut cs = 1;
                while c + cs < cols && !self.has_v[r][c + cs] {
                    cs += 1;
                }
                let mut rs = 1;
                while r + rs < rows && !self.has_h[r + rs][c] {
                    rs += 1;
                }
                for rr in r..r + rs {
                    for cc in c..c + cs {
                        covered[rr][cc] = true;
                    }
                }
                out.push(CellRect {
                    row: r,
                    col: c,
                    row_span: rs,
                    col_span: cs,
                    x0: self.xs[c],
                    y0: self.ys[r],
                    x1: self.xs[c + cs],
                    y1: self.ys[r + rs],
                });
            }
        }
        out
    }
}

/// 检出至少 2×2 的网格才返回 Some。
pub fn detect(px: &Pixels, area: Area) -> Option<LineGrid> {
    if area.x1 <= area.x0 + 8 || area.y1 <= area.y0 + 8 {
        return None;
    }
    let bg = background(px, area);
    let x_cand = lines_along(px, area, bg, true);
    let y_cand = lines_along(px, area, bg, false);
    if x_cand.len() < 3 || y_cand.len() < 3 {
        return None;
    }
    // 先按"整条线有没有画过"筛掉杂色，再逐段判定 —— 顺序反了就把合并格当没有线
    let xs = keep_lines(&x_cand, &y_cand, px, bg, true);
    let ys = keep_lines(&y_cand, &x_cand, px, bg, false);
    if xs.len() < 3 || ys.len() < 3 {
        return None;
    }
    let rows = ys.len() - 1;
    let cols = xs.len() - 1;
    let mut has_v = vec![vec![false; xs.len()]; rows];
    for (r, band) in ys.windows(2).enumerate() {
        for (j, x) in xs.iter().enumerate() {
            has_v[r][j] = line_in_band(px, bg, *x as usize, true, band[0] as usize, band[1] as usize);
        }
    }
    let mut has_h = vec![vec![false; cols]; ys.len()];
    for (i, y) in ys.iter().enumerate() {
        for (c, band) in xs.windows(2).enumerate() {
            has_h[i][c] = line_in_band(px, bg, *y as usize, false, band[0] as usize, band[1] as usize);
        }
    }
    // 最外圈竖线至少在半数行带里真画过，否则是把表格外的杂色当成了边界
    let half = rows.div_ceil(2).max(1);
    if has_v.iter().filter(|row| row[0]).count() < half
        || has_v.iter().filter(|row| row[cols]).count() < half
    {
        return None;
    }
    if has_h[0].iter().any(|v| !v) || has_h[ys.len() - 1].iter().any(|v| !v) {
        return None;
    }
    Some(LineGrid {
        xs,
        ys,
        has_v,
        has_h,
    })
}

/// 候选线里保留"至少在 `MIN_LINE_BANDS` 个相邻带里真画过"的那些。
///
/// 只在一个带里出现的线是假线：深色填充表头整块都是暗的，任何一列文字边缘在
/// 那一格里都算"画过"，可它在别的数据行里立刻断掉 —— 销售表多出来的第 9 列
/// （居中「数码电子」那一列的右边缘）就是这么混进来的。
fn keep_lines(lines: &[f64], bands: &[f64], px: &Pixels, bg: u32, vertical: bool) -> Vec<f64> {
    // 两三行的短表里"只在一个带出现过"很可能就是真合并格的那条边；带多了才可信
    let min_bands = if bands.len() >= 4 { MIN_LINE_BANDS } else { 1 };
    lines
        .iter()
        .filter(|pos| {
            bands
                .windows(2)
                .filter(|b| line_in_band(px, bg, **pos as usize, vertical, b[0] as usize, b[1] as usize))
                .count()
                >= min_bands
        })
        .copied()
        .collect()
}

/// 背景亮度：整块区域按 4px 采样取中位数（截图通常是白或近白）。
fn background(px: &Pixels, area: Area) -> u32 {
    let mut samples: Vec<u32> = Vec::new();
    let mut y = area.y0;
    while y < area.y1 {
        let mut x = area.x0;
        while x < area.x1 {
            samples.push(px.lum(x, y));
            x += 4;
        }
        y += 4;
    }
    if samples.is_empty() {
        return 255;
    }
    samples.sort_unstable();
    samples[samples.len() * 3 / 4]
}

/// 最长连续 true 游程：返回（长度，起点）。
fn longest_run(flags: &[bool]) -> (usize, usize) {
    let mut best = (0usize, 0usize);
    let mut start = 0usize;
    let mut in_run = false;
    for (i, f) in flags.iter().enumerate() {
        if *f {
            if !in_run {
                start = i;
                in_run = true;
            }
            if i - start + 1 > best.0 {
                best = (i - start + 1, start);
            }
        } else {
            in_run = false;
        }
    }
    best
}

/// 沿一个方向找线：vertical=true 找竖线（返回 x），否则找横线（返回 y）。
fn lines_along(px: &Pixels, area: Area, bg: u32, vertical: bool) -> Vec<f64> {
    let (outer, inner) = if vertical {
        (area.x1 - area.x0, area.y1 - area.y0)
    } else {
        (area.y1 - area.y0, area.x1 - area.x0)
    };
    if inner < 4 || outer < 8 {
        return Vec::new();
    }
    let mut out: Vec<f64> = Vec::new();
    let mut run: Vec<usize> = Vec::new();
    let mut solid = 0usize;
    let min_span = (inner as f64 * MIN_SPAN) as usize;
    let mut dark = vec![false; inner];
    let mut closed = vec![false; inner];
    for o in 0..=outer {
        let span = if o < outer {
            for i in 0..inner {
                let (x, y) = if vertical {
                    (area.x0 + o, area.y0 + i)
                } else {
                    (area.x0 + i, area.y0 + o)
                };
                dark[i] = px.lum(x, y) + CONTRAST < bg;
            }
            for i in 0..inner {
                closed[i] = (i.saturating_sub(CLOSING)..=(i + CLOSING).min(inner - 1)).any(|k| dark[k]);
            }
            longest_run(&closed).0
        } else {
            0
        };
        if span >= min_span {
            run.push(if vertical { area.x0 + o } else { area.y0 + o });
            if span * 5 >= inner * SOLID as usize {
                solid += 1;
            }
        } else if !run.is_empty() {
            flush_run(&mut out, &run, solid);
            run.clear();
            solid = 0;
        }
    }
    out
}

/// 一段连续的"有长暗游程"的位置出几条线。
///
/// 细的就是线，取线心。又厚又实（横跨八成以上幅面）的是**深色填充行**：
/// Excel/WPS 的彩色表头整块都是暗的，粗边框判据会把它连同上下两条真边框一起
/// 扔掉 —— 实测销售表因此丢掉表头和第一行，粘出去少两行。这种块的上下沿
/// 正好就是那两条边框。不实心的厚块（一整行密字）仍然丢弃。
fn flush_run(out: &mut Vec<f64>, run: &[usize], solid: usize) {
    let last = run.len() - 1;
    if run.len() <= MAX_THICK {
        out.push(run.iter().sum::<usize>() as f64 / run.len() as f64);
    } else if run.len() >= MIN_FILL && solid * 5 >= run.len() * SOLID as usize {
        out.push(run[0] as f64);
        out.push(run[last] as f64);
    }
}

/// 这一段带里线画没画过：暗游程（允许 ±1 补抗锯齿缝）要几乎铺满整段带高。
///
/// 判据不能用"暗像素占比"：同一列居中的文字右边缘几乎对齐，一竖排下来占比能到
/// 0.7~0.83（销售表实测把 8 列切出第 9 列）。真边框在带内是一整条不断的游程
/// （实测 24/24），字边只有字面高（实测 ≤16/24）。
fn line_in_band(px: &Pixels, bg: u32, pos: usize, vertical: bool, from: usize, to: usize) -> bool {
    let band = to.saturating_sub(from);
    let need = band - (band / 6).max(2);
    if need < 4 {
        return false;
    }
    (pos.saturating_sub(1)..=pos + 1).any(|p| {
        let dark: Vec<bool> = (from..to)
            .map(|i| {
                let (x, y) = if vertical { (p, i) } else { (i, p) };
                px.lum(x, y) + CONTRAST < bg
            })
            .collect();
        let closed: Vec<bool> = (0..band)
            .map(|i| (i.saturating_sub(1)..=(i + 1).min(band - 1)).any(|k| dark[k]))
            .collect();
        longest_run(&closed).0 >= need
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 画一张全白的 BGRA 画布
    fn canvas(w: usize, h: usize) -> Vec<u8> {
        vec![255u8; w * h * 4]
    }

    fn put(buf: &mut [u8], w: usize, x: usize, y: usize, v: u8) {
        let i = (y * w + x) * 4;
        buf[i] = v;
        buf[i + 1] = v;
        buf[i + 2] = v;
        buf[i + 3] = 255;
    }

    fn hline(buf: &mut [u8], w: usize, y: usize, x0: usize, x1: usize, v: u8) {
        for x in x0..x1 {
            put(buf, w, x, y, v);
        }
    }

    fn vline(buf: &mut [u8], w: usize, x: usize, y0: usize, y1: usize, v: u8) {
        for y in y0..y1 {
            put(buf, w, x, y, v);
        }
    }

    /// 3 列 × 2 行的完整网格
    fn grid3x2() -> (Vec<u8>, usize, usize) {
        let (w, h) = (120usize, 60usize);
        let mut buf = canvas(w, h);
        for y in [5usize, 30, 55] {
            hline(&mut buf, w, y, 10, 110, 60);
        }
        for x in [10usize, 50, 80, 110] {
            vline(&mut buf, w, x, 5, 56, 60);
        }
        (buf, w, h)
    }

    fn detect3x2(buf: &[u8], w: usize, h: usize) -> Option<LineGrid> {
        detect(&Pixels::new(buf, w, h), Area { x0: 0, y0: 0, x1: w, y1: h })
    }

    #[test]
    fn 完整网格检出三条横线四条竖线() {
        let (buf, w, h) = grid3x2();
        let g = detect3x2(&buf, w, h).expect("该检出网格");
        assert_eq!((g.rows(), g.cols()), (2, 3));
        assert!(g.xs[0] > 8.0 && g.xs[0] < 12.0, "{:?}", g.xs);
        assert!(g.ys.last().unwrap() - 55.0 < 1.5, "{:?}", g.ys);
    }

    #[test]
    fn 每个格子都铺满网格不重叠不遗漏() {
        let (buf, w, h) = grid3x2();
        let g = detect3x2(&buf, w, h).expect("该检出网格");
        let cells = g.cells();
        assert_eq!(cells.len(), 6);
        let mut seen = vec![vec![false; 3]; 2];
        for c in &cells {
            assert_eq!((c.row_span, c.col_span), (1, 1));
            assert!(!seen[c.row][c.col]);
            seen[c.row][c.col] = true;
        }
    }

    #[test]
    fn 缺一段竖线就判成横向合并() {
        let (mut buf, w, h) = grid3x2();
        // 抹掉第一行带里 x=50 那段竖线 → (0,0) 与 (0,1) 合并
        for y in 6..30 {
            put(&mut buf, w, 50, y, 255);
        }
        let g = detect3x2(&buf, w, h).expect("该检出网格");
        let cells = g.cells();
        let merged = cells.iter().find(|c| c.row == 0 && c.col == 0).expect("有左上格");
        assert_eq!(merged.col_span, 2, "第一行首格该跨两列");
        assert_eq!(merged.x1, g.xs[2]);
        // 第二行那条线还在 → 不合并
        let second = cells.iter().find(|c| c.row == 1 && c.col == 0).expect("有左下格");
        assert_eq!(second.col_span, 1);
        // 覆盖格不再单独出格
        assert_eq!(cells.len(), 5);
    }

    #[test]
    fn 缺一段横线就判成纵向合并() {
        let (mut buf, w, h) = grid3x2();
        for x in 80..110 {
            put(&mut buf, w, x, 30, 255);
        }
        let g = detect3x2(&buf, w, h).expect("该检出网格");
        let cells = g.cells();
        let tall = cells.iter().find(|c| c.row == 0 && c.col == 2).expect("有右上格");
        assert_eq!(tall.row_span, 2);
        assert_eq!(tall.y1, *g.ys.last().unwrap());
    }

    #[test]
    fn 浅灰网格线也认得() {
        let (mut buf, w, h) = grid3x2();
        // 把 60 的深线换成 Excel 默认的浅灰（约 217）
        for y in 0..h {
            for x in 0..w {
                let i = (y * w + x) * 4;
                if buf[i] < 128 {
                    buf[i] = 217;
                    buf[i + 1] = 217;
                    buf[i + 2] = 217;
                }
            }
        }
        assert!(detect3x2(&buf, w, h).is_some(), "浅灰线不该漏");
    }

    #[test]
    fn 一行密字不被误判成横线() {
        // 真实的一行字是一段墨一段空：闭运算也接不上，最长暗游程远短于幅面
        let (mut buf, w, h) = grid3x2();
        for y in 12..24 {
            for x in (12..108).step_by(12) {
                hline(&mut buf, w, y, x, x + 6, 40);
            }
        }
        let g = detect3x2(&buf, w, h).expect("该检出网格");
        assert_eq!((g.rows(), g.cols()), (2, 3));
        assert_eq!(fmt(&g.ys), "5,30,55");
    }

    #[test]
    fn 深色填充表头出上下两条边线() {
        // 彩色表头整块是暗的：按"太粗不是线"处理会把这一整块连同它的两条真边框一起
        // 扔掉 —— 销售表实测因此丢掉表头和第一行。这种块的上下沿就是边框。
        let (w, h) = (120usize, 110usize);
        let mut buf = canvas(w, h);
        for y in [5usize, 30, 55, 80, 105] {
            hline(&mut buf, w, y, 10, 110, 60);
        }
        for x in [10usize, 50, 80, 110] {
            vline(&mut buf, w, x, 5, 106, 60);
        }
        for y in 6..30 {
            hline(&mut buf, w, y, 11, 110, 50);
        }
        let g = detect(&Pixels::new(&buf, w, h), Area { x0: 0, y0: 0, x1: w, y1: h })
            .expect("该检出网格");
        assert_eq!((g.rows(), g.cols()), (4, 3));
        assert_eq!(fmt(&g.ys), "5,30,55,80,105");
    }

    #[test]
    fn 只在一个带里画过的竖线不算线() {
        // 深色填充那一格里处处都暗，任何一列文字边缘在表头里都算"画过"，
        // 可它在下面的数据行里全断 —— 收进网格就白多一列（销售表实测第 9 列）
        let (w, h) = (140usize, 90usize);
        let mut buf = canvas(w, h);
        for y in [5usize, 45, 65, 85] {
            hline(&mut buf, w, y, 10, 131, 60);
        }
        for x in [10usize, 40, 70, 100, 130] {
            vline(&mut buf, w, x, 5, 86, 60);
        }
        vline(&mut buf, w, 55, 5, 45, 60);
        let g = detect(&Pixels::new(&buf, w, h), Area { x0: 0, y0: 0, x1: w, y1: h })
            .expect("该检出网格");
        assert_eq!((g.rows(), g.cols()), (3, 4));
    }

    #[test]
    fn 无边框表返回_none_让投影法接手() {
        let (w, h) = (120usize, 60usize);
        let buf = canvas(w, h);
        assert!(detect3x2(&buf, w, h).is_none());
    }

    #[test]
    fn 只有横线没有竖线时也不硬凑网格() {
        let (w, h) = (120usize, 60usize);
        let mut buf = canvas(w, h);
        for y in [5usize, 30, 55] {
            hline(&mut buf, w, y, 0, w, 60);
        }
        assert!(detect3x2(&buf, w, h).is_none());
    }

    fn fmt(v: &[f64]) -> String {
        v.iter().map(|x| format!("{x:.0}")).collect::<Vec<_>>().join(",")
    }
}
