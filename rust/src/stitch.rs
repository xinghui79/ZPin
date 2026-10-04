//! 滚动长截图拼接 —— 竖向滚动帧按像素内容对齐后追加进画布。
//!
//! 对齐用 2x 下采样灰度做 SAD 搜索（粗搜 4 原像素步进 + 细搜 ±2 原像素），
//! 不做特征匹配：文档/网页/聊天记录这类纵向滚动场景足够，且每帧几毫秒。
//! 对齐只在偶数像素上做（2x 灰度的分辨率就是 2px）。
//!
//! 固定栏（粘性头 / 底部输入条）按**行级**检测：逐行比较上一帧与本帧同一行，
//! 顶部/底部连续未变的行段分别认成粘性头与固定底栏。早先按帧高 8 等分带判，
//! 带高 = 帧高 1/4——头栏小于帧高 1/4 时（浏览器 110px 工具栏 @1080p 才 10%）
//! 必然检不出，恒定失配把所有对齐候选推过接受线，长图只长一帧就停摆；帧底
//! 固定栏则每帧被整段盖进画布。行级检测没有这个量化盲区。
//!
//! 接缝回溯（重排回填）：聊天窗口这类页面会在**两次抓帧之间自己重排**——气泡
//! 重新折行、计时卡刷新，同一段文字在画布（旧渲染）与本帧（新渲染）里错开几
//! 像素。整体位移仍对得上，但接缝会把文字切在两版渲染中间（串行/重影）。对策：
//! 入画前从接缝往回逐行回溯，数出画布与本帧还逐行一致的长度 k——k 覆盖整个
//! 重叠带就照旧只追加新行；k 更短说明分叉了，把画布的重叠带整段截掉、用本帧
//! 的最新渲染重铺（相当于把接缝"重基于"到当前页面状态）。主流开源拼接
//! （模板匹配 / 相位相关）都没有这一层。
//!
//! 只有「画布达到高度上限」才置 done；画面有没有在动只由 frozen 报告 —— 收工
//! 时机归调用方（手动滚动的驱动端不能拿「连续几帧没位移」当到底）。

use crate::ffi::StitchStep;

pub struct StitchSession {
    width: usize,
    frame_h: usize,
    max_h: usize,
    canvas: Vec<u8>, // BGRA
    gray: Vec<u8>,   // 画布的 2x 下采样灰度（gw * gray_h）
    last_gray: Vec<u8>,
    canvas_h: usize,
}

const MIN_OVERLAP: usize = 8; // 少于 8px 的重叠不参与对齐（噪声）
const MIN_NEW: usize = 8; // 一帧至少要新增 8px 才算有效滚动
const MAX_STRIP: usize = 64; // 对齐比较条的最大灰度行数
const SAD_ACCEPT: f64 = 26.0; // 平均灰度差超过它视为「对不上」
/// 单行「未变」判定：平均灰度差低于它算同一行。固定栏真未变时是 0 上下
/// （抗锯齿抖动也只有个位数）；滚动中的噪声内容行间平均差几十，余量极大。
const ROW_SAME: f64 = 2.0;
/// 粘性头/固定底栏最多各占多少比例的帧高（再高就不像固定栏，更像整页没滚）。
const MAX_BAR_FRAC: usize = 3; // 各 1/3

impl StitchSession {
    /// frame_h 一律归偶（向下取偶，且至少 2）：画布按原像素行推进，而对齐金字塔
    /// 按 2 行一组下采样（`downsample_gray` 的 `gh = frame_h / 2` 向下取整）。
    /// 奇数帧高会让画布比灰度图多出半行——首帧最后一行永远不参与对齐，之后每帧
    /// 又只补 `(append_to/2 - app_from/2)` 个灰行、却补 `append_to - app_from` 个
    /// 画布行，半行错位逐帧累积，最终整张长图从第一段拼接起就重复/丢行。
    /// 实测帧高 65 时画布比正确值高 26 行且第 65 行起全错；帧高 64 完美还原。
    /// 向下取偶最多丢帧底一行，远好过整幅错位。C++ 侧也把选区取偶
    /// （`overlay.cpp` 的 `startScrollCapture`），两边都兜。
    pub fn new(width: usize, frame_h: usize, max_h: usize) -> StitchSession {
        let frame_h = (frame_h / 2).max(1) * 2;
        StitchSession {
            width,
            frame_h,
            max_h,
            canvas: Vec::new(),
            gray: Vec::new(),
            last_gray: Vec::new(),
            canvas_h: 0,
        }
    }

    /// 追加一帧 BGRA（宽 = 会话宽，高 = 会话帧高）。
    pub fn push(&mut self, frame: &[u8]) -> StitchStep {
        let need = self.width * self.frame_h * 4;
        if frame.len() < need {
            return StitchStep {
                offset: 0,
                canvas_h: self.canvas_h as i32,
                done: true,
                frozen: true,
                sticky_px: 0,
                rebased: false,
            };
        }
        if self.canvas_h == 0 {
            self.canvas = frame[..need].to_vec();
            self.canvas_h = self.frame_h;
            self.gray = downsample_gray(frame, self.width, self.frame_h);
            self.last_gray = self.gray.clone();
            return StitchStep {
                offset: 0,
                canvas_h: self.canvas_h as i32,
                done: false,
                frozen: false, // 首帧没有「上一帧」可比，一律当作尚未稳定
                sticky_px: 0,
                rebased: false,
            };
        }
        let gw = self.width / 2;
        let gframe = downsample_gray(frame, self.width, self.frame_h);
        // 行级判动：顶部/底部连续未变的行段分别是粘性头与固定底栏，都不参与对齐
        let (frozen, sticky, footer) = self.motion(&gframe, gw);
        let bar_px = footer * 2; // 固定底栏的物理行数
        // 首帧整帧入画时可能已把底栏带了进来（那时还无从知道它是底栏）。
        // 本帧判出了底栏、且画布底部正好是这段恒定图案（内容行不可能逐字节等于
        // 它）→ 先回删再对齐，否则对齐拿「内容+底栏」的画布底去比本帧的内容带，
        // 从根上就对不上；不回删的话底栏还会永远嵌在画布中部。
        if !frozen && bar_px > 0 && self.canvas_h > bar_px {
            let cs = (self.canvas_h - bar_px) * self.width * 4;
            let fs = (self.frame_h - bar_px) * self.width * 4;
            if self.canvas[cs..] == frame[fs..need] {
                let new_h = self.canvas_h - bar_px;
                self.canvas.truncate(new_h * self.width * 4);
                self.gray.truncate(new_h / 2 * gw);
                self.canvas_h = new_h;
            }
        }
        let offset = if frozen {
            0
        } else {
            self.find_offset(&gframe, sticky, footer)
        };
        if offset == 0 {
            self.last_gray = gframe;
            // 没位移：可能真的没动（frozen），也可能动了但对不上。谁该收工由
            // 调用方判断 —— 手动滚动时「没动」是常态，拿次数当到底会当场截断。
            let done = self.canvas_h >= self.max_h;
            return StitchStep {
                offset: 0,
                canvas_h: self.canvas_h as i32,
                done,
                frozen,
                sticky_px: (sticky * 2) as i32,
                rebased: false,
            };
        }
        // 接缝回溯：整体位移对上了，但页面可能在两次抓帧之间重排过——从接缝
        // 往回数画布与本帧仍逐行一致的长度，短于重叠带就把画布重叠带整段截掉
        // 用本帧渲染重铺（见模块注释）。回填后帧内容带从头入画。
        let og = offset / 2 - sticky; // 重叠带灰行数
        let seam_k = self.seam_agreement(&gframe, sticky, og);
        let rebased = seam_k < og;
        if rebased {
            let cut = og * 2;
            let new_h = self.canvas_h - cut;
            self.canvas.truncate(new_h * self.width * 4);
            self.gray.truncate(new_h / 2 * gw);
            self.canvas_h = new_h;
        }
        // 追加 [本帧入画起点, 帧底栏上缘)：顶部落进重叠区的行不重复，底栏不重复
        // 入画；重排回填时入画起点退回内容带头（重叠带已截掉，整带重铺）
        let app_from = if rebased { sticky * 2 } else { offset };
        let append_to = self.frame_h - bar_px;
        self.canvas
            .extend_from_slice(&frame[app_from * self.width * 4..append_to * self.width * 4]);
        self.gray
            .extend_from_slice(&gframe[(app_from / 2) * gw..(append_to / 2) * gw]);
        self.canvas_h += append_to - app_from;
        self.last_gray = gframe;
        let done = self.canvas_h >= self.max_h;
        StitchStep {
            offset: offset as i32,
            canvas_h: self.canvas_h as i32,
            done,
            frozen: false, // 对上了新内容 = 画面确实动过
            sticky_px: (sticky * 2) as i32,
            rebased,
        }
    }

    pub fn canvas_height(&self) -> i32 {
        self.canvas_h as i32
    }

    /// 只读画布切片（进度窗实时缩略图用）。
    pub fn canvas(&self) -> &[u8] {
        &self.canvas
    }

    /// 取走拼接完成的画布（BGRA），会话随之清空。
    pub fn take_canvas(&mut self) -> Vec<u8> {
        self.canvas_h = 0;
        self.last_gray = Vec::new();
        std::mem::take(&mut self.gray);
        std::mem::take(&mut self.canvas)
    }

    /// 逐行比较「上一帧 vs 本帧」，返回
    ///   (frozen, sticky_gray_rows, footer_gray_rows)
    /// frozen：所有行都没变（滚轮没生效 / 滚到底了）。
    /// sticky_gray_rows：顶部连续未变的行（粘性头/固定工具条），不参与对齐。
    /// footer_gray_rows：底部连续未变的行（聊天输入条/底部工具栏），不参与
    /// 对齐、也不追加进画布。两者上限各为帧高的 1/3，再高就不像固定栏。
    fn motion(&self, gframe: &[u8], gw: usize) -> (bool, usize, usize) {
        let gh = self.frame_h / 2;
        let lh = self.last_gray.len() / gw;
        if lh == 0 || gh < 4 {
            return (false, 0, 0);
        }
        let same = |r: usize| row_sad(&self.last_gray, gframe, gw, r) < ROW_SAME;
        let mut any_changed = false;
        let mut sticky = 0usize;
        let mut still_leading = true;
        for r in 0..gh {
            if same(r) {
                if still_leading {
                    sticky += 1;
                }
            } else {
                any_changed = true;
                still_leading = false;
            }
        }
        if !any_changed {
            return (true, 0, 0); // 整帧未变 = 静止，不属于固定栏
        }
        let mut footer = 0usize;
        for r in (0..gh).rev() {
            if same(r) {
                footer += 1;
            } else {
                break;
            }
        }
        let cap = gh / MAX_BAR_FRAC;
        (false, sticky.min(cap), footer.min(cap))
    }

    /// 在画布灰度底部找新帧内容带 [skip, gh - footer) 的重叠。
    /// 返回**原像素**的「本帧顶部被消耗掉的总行数」= (skip + og) * 2，
    /// 调用方据此裁掉这一段再把余下的追加进画布。0 = 没对上。
    fn find_offset(&self, gframe: &[u8], skip: usize, footer: usize) -> usize {
        let gw = self.width / 2;
        let gh = self.frame_h / 2;
        let ch = self.gray.len() / gw;
        if ch < 2 || gh < 2 || skip + footer >= gh {
            return 0;
        }
        // 内容带里留给新增量的下限：不能把整条内容带都当重叠吃掉
        let content_rows = gh - skip - footer;
        let max_og = content_rows.saturating_sub(MIN_NEW / 2).min(ch);
        if max_og < MIN_OVERLAP / 2 {
            return 0;
        }
        let strip = ((gh / 4).max(4)).min(MAX_STRIP);
        // 全范围逐灰行搜（步进 1 灰行 = 2 原像素）。早先 2 灰行步进 + 只在最优点旁
        // 细搜 ±1，碰上「真实重叠是奇数灰行」且内容对比度尖锐时（SAD 谷是针尖形），
        // 粗搜只能看到两侧 ±1 行的失败值，真值恰在缝里漏掉。改成步进 1 之后每帧
        // 不到 1ms，细搜那一遍挑的两个点本来就在扫描里，属于死代码，删掉。
        //
        // 对齐能力实测（tests::各种滚动量都逐字节还原）：帧高 1024 时 8..1016px 的
        // 每次滚动量都能逐字节还原 —— 位移范围不是瓶颈，不值得为它换特征匹配。
        // 真正的盲区是**内容本身没有信息**的两种：整段空白（位移在信息上就无法确定，
        // 特征匹配同样取不到特征）、像素级严格重复的等距行（错一个周期 SAD 同为 0）。
        // 试过给谷底加「并列即弃」的歧义判据：大粘性头那种合法场景（重叠带本来就短）
        // 会被一起判死，得不偿失，已回退。
        let mut best: (usize, f64) = (0, f64::INFINITY);
        let mut og = MIN_OVERLAP / 2;
        while og <= max_og {
            let sad = self.strip_sad(gframe, skip, og, strip.min(og));
            if sad < best.1 {
                best = (og, sad);
            }
            og += 1;
        }
        if best.0 == 0 || best.1 > SAD_ACCEPT {
            return 0;
        }
        (skip + best.0) * 2
    }

    /// 接缝回溯：按 find_offset 选定的重叠（帧内容带 from 起 og 灰行 ↔ 画布
    /// 底 og 灰行），从接缝（画布底 ↔ 帧内容带底）往回逐行比较，返回仍逐行
    /// 一致的灰行数 k。k == og 说明整个重叠带未变（正常追加即可）；k 更短 =
    /// 页面在两次抓帧之间重排过，靠接缝的行先分叉。
    fn seam_agreement(&self, gframe: &[u8], from: usize, og: usize) -> usize {
        let gw = self.width / 2;
        let ch = self.gray.len() / gw;
        let mut k = 0usize;
        while k < og {
            let cr = ch - 1 - k; // 画布倒数第 k+1 行
            let fr = from + og - 1 - k; // 帧内容带倒数第 k+1 行
            if cr >= ch || fr * gw + gw > gframe.len() {
                break;
            }
            if row_sad_at(&self.gray, cr, gframe, fr, gw) >= ROW_SAME {
                break;
            }
            k += 1;
        }
        k
    }

    /// 画布底部 og 灰度行 与 新帧自 from 起的 og 灰度行 的平均绝对差（列隔 2 采样）。
    fn strip_sad(&self, gframe: &[u8], from: usize, og: usize, rows: usize) -> f64 {
        let gw = self.width / 2;
        let ch = self.gray.len() / gw;
        if ch < og || from + rows > gframe.len() / gw {
            return f64::INFINITY;
        }
        let mut sum: u64 = 0;
        let mut cnt: u64 = 0;
        for r in 0..rows {
            let cr = (ch - og + r) * gw;
            let fr = (from + r) * gw;
            for x in (0..gw).step_by(2) {
                sum += (self.gray[cr + x] as i32 - gframe[fr + x] as i32).unsigned_abs() as u64;
                cnt += 1;
            }
        }
        if cnt == 0 {
            return f64::INFINITY;
        }
        sum as f64 / cnt as f64
    }
}

/// 两块灰度图指定行的平均绝对差（列隔 2 采样）。行级固定栏检测与接缝回溯用。
fn row_sad_at(a: &[u8], a_row: usize, b: &[u8], b_row: usize, gw: usize) -> f64 {
    let off_a = a_row * gw;
    let off_b = b_row * gw;
    if off_a + gw > a.len() || off_b + gw > b.len() {
        return f64::INFINITY;
    }
    let mut sum: u64 = 0;
    let mut cnt: u64 = 0;
    for x in (0..gw).step_by(2) {
        sum += (a[off_a + x] as i32 - b[off_b + x] as i32).unsigned_abs() as u64;
        cnt += 1;
    }
    if cnt == 0 {
        return f64::INFINITY;
    }
    sum as f64 / cnt as f64
}

/// 两块灰度图第 r 行的平均绝对差（同下行号）。
fn row_sad(a: &[u8], b: &[u8], gw: usize, r: usize) -> f64 {
    row_sad_at(a, r, b, r, gw)
}

/// BGRA → 2x 下采样灰度（2x2 平均；宽高向下取偶）。
fn downsample_gray(bgra: &[u8], w: usize, h: usize) -> Vec<u8> {
    let gw = w / 2;
    let gh = h / 2;
    let mut out = vec![0u8; gw * gh];
    for gy in 0..gh {
        for gx in 0..gw {
            let x = gx * 2;
            let y = gy * 2;
            let mut acc = 0u32;
            for dy in 0..2 {
                for dx in 0..2 {
                    let i = ((y + dy) * w + x + dx) * 4;
                    acc += (u32::from(bgra[i]) * 114
                        + u32::from(bgra[i + 1]) * 587
                        + u32::from(bgra[i + 2]) * 299)
                        / 1000;
                }
            }
            out[gy * gw + gx] = (acc / 4) as u8;
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::StitchSession;

    const W: usize = 64;

    /// 从合成内容里裁一帧 BGRA（top 是内容纵向偏移）。
    fn frame_of(content: &[u8], top: usize, fh: usize) -> Vec<u8> {
        let mut f = vec![0u8; W * fh * 4];
        for y in 0..fh {
            let src = (top + y) * W * 4;
            let dst = y * W * 4;
            f[dst..dst + W * 4].copy_from_slice(&content[src..src + W * 4]);
        }
        f
    }

    /// 非周期伪随机内容。早先的 `(x*3+y*7)%251` 沿 y 自近周期（错 18 灰行几乎
    /// 逐像素相同），真实重叠不足时「错位对齐」的 SAD 只有 1.8，能赢过根本
    /// 不存在的真重叠——测试对静默丢内容免疫。噪声内容的错位 SAD 在几十，
    /// 与对齐位（0）拉开数量级，错对必被 SAD_ACCEPT 拒掉。
    fn make_content(total: usize) -> Vec<u8> {
        let mut content = vec![0u8; W * total * 4];
        let mut g: u32 = 0x1234_5678;
        for y in 0..total {
            for x in 0..W {
                g = g.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
                let v = ((g >> 16) & 0xFF) as u8;
                let i = (y * W + x) * 4;
                content[i] = v;
                content[i + 1] = v.wrapping_add(13);
                content[i + 2] = v.wrapping_add(29);
                content[i + 3] = 255;
            }
        }
        content
    }

    #[test]
    fn 拼接还原长图() {
        let fh = 256;
        let total = 1000;
        let content = make_content(total);
        let mut s = StitchSession::new(W, fh, 4000);
        let mut top = 0;
        let mut frames = 0;
        loop {
            let step = s.push(&frame_of(&content, top, fh));
            frames += 1;
            assert!(frames < 30, "迟迟没有对齐到底（画布没长到内容高）");
            // 模拟每次滚 120px，到底后画面停住。纯手动驱动下「停住」的信号是
            // frozen（done 只表示撞到画布高度上限）
            if step.frozen {
                break;
            }
            top = (top + 120).min(total - fh);
        }
        let canvas = s.take_canvas();
        // 强断言：画布必须与理想拼接**逐字节一致**（此前只查高度容差与首 64 字节，
        // 静默丢内容/错位对齐都测不出来）
        assert_eq!(
            canvas.len(),
            W * total * 4,
            "画布高度应精确等于内容高度"
        );
        assert_eq!(&canvas[..], &content[..], "画布内容与理想拼接不一致");
    }

    /// 一次能跟住多大的滚动量：帧高 1024，把每次滚动量从 8px 一路拉到接近整帧，
    /// 全部要求逐字节还原。这张表就是「要不要换成特征匹配」的判据——表里出现的
    /// 空洞，正是用户反馈的「滚快了拼接断掉」。
    #[test]
    fn 各种滚动量都逐字节还原() {
        let fh = 1024usize;
        for step_px in [8usize, 40, 128, 256, 500, 700, 900, 1000, 1016] {
            let total = fh + step_px * 4; // 滚 4 次到底
            let content = make_content(total);
            let mut s = StitchSession::new(W, fh, 20000);
            let mut top = 0usize;
            let mut frames = 0;
            let t0 = std::time::Instant::now();
            loop {
                let got = s.push(&frame_of(&content, top, fh));
                frames += 1;
                assert!(
                    frames < 40,
                    "步长 {step_px}：迟迟没到底（画布 {} 行）",
                    got.canvas_h
                );
                if got.frozen {
                    break;
                }
                top = (top + step_px).min(total - fh);
            }
            let ms = t0.elapsed().as_secs_f64() * 1000.0 / frames as f64;
            let canvas = s.take_canvas();
            assert_eq!(
                canvas.len(),
                W * total * 4,
                "步长 {step_px}：画布高度不对（滚太快把内容丢了？）"
            );
            assert_eq!(&canvas[..], &content[..], "步长 {step_px}：内容不一致");
            println!("步长 {step_px:5}px：{frames} 帧，每帧 {ms:.2}ms");
        }
    }

    /// 空白带（翻页间隙 / 段落大留白）：整段逐像素相同，任何位移的 SAD 都是 0。
    /// 这种位移在信息上就无法确定（特征匹配同样拿不到特征），所以要求的不是
    /// 「接对」而是**不许瞎接**：画布必须始终是真实内容的前缀。实测它会在进空白
    /// 后停下（2000 行的内容停在第 492 行），这是可接受的保守失败。
    #[test]
    fn 滚过大片空白不编内容() {
        let fh = 512usize;
        let total = 2000usize;
        let mut content = make_content(total);
        for y in 400..1200usize {
            for x in 0..W {
                let i = (y * W + x) * 4;
                content[i] = 255;
                content[i + 1] = 255;
                content[i + 2] = 255;
            }
        }
        let mut s = StitchSession::new(W, fh, 20000);
        let mut top = 0usize;
        let mut frames = 0;
        loop {
            let got = s.push(&frame_of(&content, top, fh));
            frames += 1;
            assert!(frames < 40, "空白带：迟迟没到底（画布 {} 行）", got.canvas_h);
            if got.frozen {
                break;
            }
            top = (top + 160).min(total - fh);
        }
        let canvas = s.take_canvas();
        let h = canvas.len() / (W * 4);
        assert_eq!(&canvas[..], &content[..W * h * 4], "空白带：画布必须是真内容的前缀");
        println!("空白带：{frames} 帧后停在 {h} 行（内容共 {total} 行）");
    }

    /// 奇数帧高必须与偶数帧高还原得同样好：会话内部把 frame_h 归偶，画布跟着
    /// 偶数帧高走。此前画布按奇数原像素行推进、灰度金字塔按偶数行下采样，半行
    /// 错位逐帧累积——实测帧高 65 时从第一段拼接起内容就错、画布比正确值高
    /// 26 行。C++ 侧选区若不取偶，用户的普通框选就会踩到。
    #[test]
    fn 奇数帧高照样逐字节还原() {
        for fh in [65usize, 63, 67] {
let even = (fh / 2).max(1) * 2; // 会话实际使用的帧高（向下取偶）
            // 末尾落在 20 的整数倍上，于是最后一步的滚动量也是偶数。对齐只在
            // 2px 栅格上搜（offset 恒为偶数），奇数滚动量只能取最近的偶数，会静默
            // 吞掉或重复 1 行——那是栅格的固有代价，不在本题里断言。
            let total = fh + 540;
            let content = make_content(total);
            let mut s = StitchSession::new(W, fh, 8000);
            let mut top = 0usize;
            let mut frames = 0;
            loop {
                // 帧按调用方给的奇数高度切（模拟 C++ 抓到的真实奇数高度帧）
                let step = s.push(&frame_of(&content, top, fh));
                frames += 1;
                // 步长 20px 必须远小于帧高：帧内的对齐搜索范围上限是
                // 「内容带 - MIN_NEW/2」个灰行，滚太远就真的没重叠了
                assert!(frames < 60, "帧高 {fh}：迟迟没有对齐到底");
                if step.frozen {
                    break;
                }
                // 夹取按调用方的 fh（不是 even），否则最后一块帧会切出内容之外
                top = (top + 20).min(total - fh);
            }
            let canvas = s.take_canvas();
            // 画布覆盖 content[0 .. 540 + even]
            let expected = 540 + even;
            assert_eq!(
                canvas.len(),
                W * expected * 4,
                "帧高 {fh}：画布高度应等于 {expected} 行"
            );
            assert_eq!(
                &canvas[..],
                &content[..W * expected * 4],
                "帧高 {fh}：画布内容与理想拼接不一致"
            );
        }
    }

    /// 手动滚动下「画面没动」是常态：连续相同的帧只能报 frozen，不能收工。
    /// 早先是连三帧无位移就 done，纯手动驱动一空闲就被当场截断。
    #[test]
    fn 画面冻结只报静止不收工() {
        let fh = 64;
        let content = make_content(200);
        let mut s = StitchSession::new(W, fh, 4000);
        let frame = frame_of(&content, 0, fh);
        let first = s.push(&frame);
        assert!(!first.done);
        for i in 0..5 {
            let step = s.push(&frame);
            assert!(step.frozen, "相同帧应被认成静止（第 {} 次）", i + 1);
            assert!(!step.done, "空闲 {} 次就判收工，手动滚动等着被截断", i + 1);
            assert_eq!(step.canvas_h as usize, fh, "静止帧不该往画布追加内容");
        }
    }

    #[test]
    fn 高度上限强制收工() {
        let fh = 64;
        let content = make_content(400);
        let mut s = StitchSession::new(W, fh, 64); // 上限 = 帧高
        s.push(&frame_of(&content, 0, fh));
        let step = s.push(&frame_of(&content, 56, fh)); // 滚 56px → 超上限
        assert!(step.done);
    }

    /// 粘性头场景回归：网页顶部固定一条栏，只有它不动，正文在滚。
    /// 早先只用「帧顶 frame_h/4」判静止，那条固定栏恒定 -> 恒判 frozen ->
    /// 强制 offset=0 -> 每帧都「没位移」-> 静默截断。这里确保不再发生。
    fn frame_with_sticky_header(
        content: &[u8],
        top: usize,
        fh: usize,
        sticky_px: usize,
    ) -> Vec<u8> {
        let mut f = vec![0u8; W * fh * 4];
        for y in 0..fh {
            let src = (top + y) * W * 4;
            let dst = y * W * 4;
            if y < sticky_px {
                // 固定栏：内容无关，恒定图案
                for x in 0..W {
                    let i = dst + x * 4;
                    let v = ((x * 11) % 251) as u8;
                    f[i] = v;
                    f[i + 1] = v;
                    f[i + 2] = v;
                    f[i + 3] = 255;
                }
            } else {
                f[dst..dst + W * 4].copy_from_slice(&content[src..src + W * 4]);
            }
        }
        f
    }

    #[test]
    fn 粘性顶栏不误判为静止也不污染对齐() {
        let fh = 256;
        let total = 1000;
        let sticky = 48; // 48px 固定顶栏
        let content = make_content(total + sticky);
        let mut s = StitchSession::new(W, fh, 4000);
        let mut top = 0usize;
        let mut frames = 0;
        loop {
            let step = s.push(&frame_with_sticky_header(
                &content,
                top,
                fh,
                sticky,
            ));
            frames += 1;
            assert!(frames < 30, "粘性顶栏导致迟迟不收工/提前收工");
            assert!(
                !step.frozen || top + fh >= total,
                "正文还在滚却被判为静止（粘性顶栏污染了判据）"
            );
            if step.frozen {   // 手动驱动的信号：画面停住 = 可以收工
                break;
            }
            top = (top + 120).min(total.saturating_sub(fh));
        }
        // 关键断言：画布必须真的长起来了，而不是只拿到第一帧就被截断
        let h = s.canvas_height() as usize;
        assert!(
            h > fh * 3,
            "画布只有 {h}px（帧高 {fh}），说明被粘性顶栏带偏后提前收工了"
        );
    }

    #[test]
    fn 粘性顶栏存在时仍能正确对齐() {
        let fh = 128;
        let sticky = 32;
        let content = make_content(600 + sticky);
        let mut s = StitchSession::new(W, fh, 4000);
        s.push(&frame_with_sticky_header(&content, 0, fh, sticky));
        // 滚 64px（必须小于可视内容带 fh-sticky=96px，否则真实重叠为空、
        // 本来就对不上——旧实现靠近周期内容的错误对齐假装对上了）：
        // 帧内容带 = content[64+32 .. 64+128]，画布底已有 content[..128]，
        // 真实重叠 = content[96..128] 共 32px，新增 64px。
        let step = s.push(&frame_with_sticky_header(&content, 64, fh, sticky));
        assert!(step.offset > 0, "对不上：offset={}", step.offset);
        assert!(
            step.offset >= sticky as i32,
            "offset {} 小于粘性栏高度 {sticky}",
            step.offset
        );
        assert_eq!(
            step.canvas_h as usize,
            fh + (fh - step.offset as usize),
            "画布增量应恰为 帧高 - offset（顶栏不得重复入画）"
        );
    }

    /// 回归：粘性头占掉大半帧高时，仍然要能对齐。
    /// 这个组合真出过事——粘性头把可搜索的重叠范围压到 gh-skip，而驱动端还按
    /// 整帧高算滚动步长，于是每帧都滑过头、offset 恒为 0，长图只剩一帧高。
    /// 这里锁住「大半帧高是固定栏」时依旧对得上。
    #[test]
    fn 粘性头占大半帧高仍能对齐() {
        let fh = 128;
        let sticky = 96; // 固定栏吃掉 75%
        let content = make_content(600);
        let mut s = StitchSession::new(W, fh, 4000);
        s.push(&frame_with_sticky_header(&content, 0, fh, sticky));
        // 只滚 20px：可用内容只有 32px，步子必须很小
        let step = s.push(&frame_with_sticky_header(&content, 20, fh, sticky));
        assert!(step.offset > 0, "大半帧是固定栏时对不上：offset={}", step.offset);
        assert!(step.canvas_h > fh as i32, "画布没增长：{}", step.canvas_h);
        assert!(!step.done);
    }

    /// 回归（行级检测的核心动机）：头栏**小于**帧高 1/4 时也必须能整场对齐。
    /// 分带检测的时代 band0 = 帧高 1/4，110px@1080p（10%）的头栏必然漏检，
    /// 恒定失配把所有候选推过接受线，14 次滚动 miss 12 次、画布只长一帧高。
    #[test]
    fn 小头栏不足帧高四分之一也能整场还原() {
        let fh = 1080;
        let sticky = 110; // 浏览器工具栏典型高度，占帧高 10%
        let total = 4000;
        let content = make_content(total);
        let mut s = StitchSession::new(W, fh, 20000);
        let mut top = 0usize;
        let mut frames = 0;
        loop {
            let step = s.push(&frame_with_sticky_header(&content, top, fh, sticky));
            frames += 1;
            assert!(frames < 60, "小头栏场景迟迟不收工");
            if step.frozen {
                break;
            }
            top = (top + 120).min(total - fh);
        }
        let canvas = s.take_canvas();
        // 理想结果：顶栏遮住的 content[0..sticky] 永远不可见，画布 = 顶栏（一次）
        // + content[sticky..total]，总高恰为 total。
        assert_eq!(
            canvas.len(),
            W * total * 4,
            "画布高度应精确等于内容可视高度（顶栏只出现一次）"
        );
        assert_eq!(
            &canvas[sticky * W * 4..],
            &content[sticky * W * 4..],
            "顶栏之后的画布内容与理想拼接不一致"
        );
    }

    /// 回归：帧底固定栏（聊天输入条/底部工具栏）不得重复入画，内容不得丢失。
    /// 早先每帧整段追加帧底，底栏在画布中部被重复盖 N 次、正文大量丢失。
    #[test]
    fn 固定底栏只出现一次且内容不丢() {
        let fh = 540;
        let bar = 60; // 底部输入条
        let total = 2000;
        let content = make_content(total);
        let mut s = StitchSession::new(W, fh, 8000);
        let mut top = 0usize;
        let mut frames = 0;
        loop {
            // 视口：上部 [0, fh-bar) 是滚动内容，底部 [fh-bar, fh) 是固定栏
            let mut f = vec![0u8; W * fh * 4];
            for y in 0..fh {
                let dst = y * W * 4;
                if y >= fh - bar {
                    for x in 0..W {
                        let i = dst + x * 4;
                        let v = ((x * 7 + 5) % 251) as u8;
                        f[i] = v;
                        f[i + 1] = v;
                        f[i + 2] = v;
                        f[i + 3] = 255;
                    }
                } else {
                    let src = (top + y) * W * 4;
                    f[dst..dst + W * 4].copy_from_slice(&content[src..src + W * 4]);
                }
            }
            let step = s.push(&f);
            frames += 1;
            assert!(frames < 40, "底栏场景迟迟不收工");
            if step.frozen {
                break;
            }
            top = (top + 120).min(total - (fh - bar));
        }
        let canvas = s.take_canvas();
        // 理想：首帧带来的底栏被回删，画布 = 内容逐字节原样
        assert_eq!(canvas.len(), W * total * 4, "画布高度应精确等于内容高度");
        assert_eq!(&canvas[..], &content[..], "画布内容与理想拼接不一致");
    }

    /// 回归：页面在两次抓帧之间**自己重排**（聊天气泡重新折行、计时卡刷新）。
    /// 重排带横跨接缝时，旧实现把新帧直接接在旧渲染后面——同一段文字前半是旧
    /// 版排版、后半是新版排版，接缝切在字的半中间（串行/重影）。现在接缝回溯
    /// 发现分叉后，把画布重叠带整段用本帧的最新渲染重铺，接缝两侧同版。
    #[test]
    fn 滚动间隙重排用最新渲染回填接缝() {
        let fh = 128;
        let total = 1000;
        let v1 = make_content(total);
        let mut v2 = v1.clone();
        // 重排带：内容 340..390px 的气泡在滚动间隙被重新折行（整段换渲染）
        for y in 340..390 {
            for x in 0..W {
                let i = (y * W + x) * 4;
                let u = ((x * 29 + y * 13) % 251) as u8;
                v2[i] = u;
                v2[i + 1] = u.wrapping_add(7);
                v2[i + 2] = u.wrapping_add(31);
            }
        }
        let mut s = StitchSession::new(W, fh, 8000);
        s.push(&frame_of(&v1, 0, fh));
        s.push(&frame_of(&v1, 120, fh));
        let clean = s.push(&frame_of(&v1, 240, fh));
        assert!(!clean.rebased, "页面没重排时不应触发回填");
        // 页面重排：此后帧来自 v2（340..390 换了渲染），滚动位置小幅回退
        let step = s.push(&frame_of(&v2, 280, fh));
        assert!(step.offset > 0, "重排帧应对得上：offset={}", step.offset);
        assert!(step.rebased, "横跨接缝的重排帧应触发回填");
        let mut top = 400usize;
        let mut frames = 0;
        loop {
            let st = s.push(&frame_of(&v2, top, fh));
            frames += 1;
            assert!(frames < 30, "重排后迟迟不收工");
            assert!(!st.rebased, "重排之后的正常滚动不应再触发回填");
            if st.frozen {
                break;
            }
            top = (top + 120).min(total - fh);
        }
        let canvas = s.take_canvas();
        // 理想：重排带落入的重叠区之前保持 v1（那部分早已定稿），其后整段是
        // v2 的最新渲染——接缝两侧同版，文字不再被切
        let mut ideal = v1[..280 * W * 4].to_vec();
        ideal.extend_from_slice(&v2[280 * W * 4..]);
        assert_eq!(canvas.len(), ideal.len(), "画布高度应精确等于内容高度");
        assert_eq!(&canvas[..], &ideal[..], "重排回填后的画布与理想拼接不一致");
    }
}
