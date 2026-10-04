//! 视觉内容块边界：UIA 认不出元素时的兜底吸附（自绘界面、网页图片、无无障碍信息的应用）。
//!
//! 三步：① 取景框外圈的主色当作页面背景，与背景色差足够的像素记为内容；
//! ② 内容按 merge 半径膨胀，把一行行字、一个个碎片粘连成块，从光标处洪水填充
//! 取出连通块；③ 沿途只按本块自己的原始像素收紧包围盒，避免整圈胖一像素、
//! 也避免把恰好落在盒内的另一块东西圈进来。
//! 纯计算，输入是 C++ 侧裁好的 BGRA(预乘) 缓冲，坐标系是该缓冲自己的像素。

/// 与背景色的最大通道差超过该值才算内容像素
const COLOR_DELTA: i16 = 26;
/// 短边小于该值的块不值得吸附（一个字符碎片）
const MIN_SIDE: i32 = 8;
/// 包围盒占到取景框这么大比例 = 认出的不是「一块内容」而是整片背景的反面，
/// 判不可信（percent）
const MAX_COVER: i64 = 92;

/// 4 bit/通道粗量化直方图的一格
const HIST_BINS: usize = 4096;

/// 与背景色的最大通道差是否算内容
#[inline]
fn differs(px: &[u8], bg: [u8; 3]) -> bool {
    let d = (i16::from(px[0]) - i16::from(bg[0]))
        .abs()
        .max((i16::from(px[1]) - i16::from(bg[1])).abs())
        .max((i16::from(px[2]) - i16::from(bg[2])).abs());
    d > COLOR_DELTA
}

fn bump(hist: &mut [u32; HIST_BINS], data: &[u8], width: usize, x: usize, y: usize) {
    let p = (y * width + x) * 4;
    let key = ((u32::from(data[p] >> 4) << 8)
        | (u32::from(data[p + 1] >> 4) << 4)
        | u32::from(data[p + 2] >> 4)) as usize;
    hist[key] += 1;
}

/// 取景框外圈的主色 = 背景色（取众数格的中心，误差 ≤8，远小于 COLOR_DELTA）。
/// 外圈而不是整框：正中间那块内容再大也不该把背景带跑。
/// 众数占比低于 1/8 时返回 None —— 壁纸是照片这种「没有背景」的场景直接判不可信，
/// 省掉后面整框的掩码/膨胀扫描（实测这类取景框要 10~20ms）。
fn background(data: &[u8], width: usize, height: usize) -> Option<[u8; 3]> {
    let mut hist = [0u32; HIST_BINS];
    let mut samples = 0u32;
    let band = (width.min(height) / 12)
        .clamp(1, 10)
        .min(width.min(height) / 2);
    for y in (0..band).chain(height - band..height) {
        for x in (0..width).step_by(2) {
            bump(&mut hist, data, width, x, y);
            samples += 1;
        }
    }
    for y in band..height - band {
        for x in (0..band).chain(width - band..width).step_by(2) {
            bump(&mut hist, data, width, x, y);
            samples += 1;
        }
    }
    let (mut best, mut key) = (0u32, 0usize);
    for (i, n) in hist.iter().enumerate() {
        if *n > best {
            best = *n;
            key = i;
        }
    }
    if best * 8 < samples {
        return None;
    }
    Some([
        (((key >> 8) & 15) << 4) as u8 | 8,
        (((key >> 4) & 15) << 4) as u8 | 8,
        ((key & 15) << 4) as u8 | 8,
    ])
}

/// 滑窗膨胀（对二值图就是「附近有没有内容」），横竖各一趟，O(像素数) 与半径无关
fn dilate(mask: &[u8], width: usize, height: usize, r: usize) -> Vec<u8> {
    let mut hor = vec![0u8; width * height];
    for y in 0..height {
        let base = y * width;
        let mut cnt = 0i32;
        for x in 0..=r.min(width - 1) {
            cnt += i32::from(mask[base + x]);
        }
        for x in 0..width {
            if cnt > 0 {
                hor[base + x] = 1;
            }
            if x + 1 + r < width {
                cnt += i32::from(mask[base + x + 1 + r]);
            }
            if x >= r {
                cnt -= i32::from(mask[base + x - r]);
            }
        }
    }
    let mut out = vec![0u8; width * height];
    let mut col = vec![0i32; width];
    for y in 0..height {
        if y == 0 {
            for k in 0..=r.min(height - 1) {
                for x in 0..width {
                    col[x] += i32::from(hor[k * width + x]);
                }
            }
        } else {
            let add = y + r;
            if add < height {
                for x in 0..width {
                    col[x] += i32::from(hor[add * width + x]);
                }
            }
            if y > r {
                let sub = y - r - 1;
                for x in 0..width {
                    col[x] -= i32::from(hor[sub * width + x]);
                }
            }
        }
        let base = y * width;
        for x in 0..width {
            out[base + x] = u8::from(col[x] > 0);
        }
    }
    out
}

/// 以 (sx, sy) 为种子的连通块包围盒 [l, t, r, b]（r/b 开区间）；
/// 不可信（种子在背景上、块太小、块铺满取景框、块被人为裁断）返回空 Vec。
///
/// trust 的 4 个位分别表示取景框的 左/上/右/下 是不是「真实边界」（窗口边或屏幕边）。
/// 连通块贴到非真实的边 = 它还在框外延续，此刻的包围盒是裁出来的假边界，宁可不吸附。
pub fn content_rect(
    data: &[u8],
    width: usize,
    height: usize,
    sx: usize,
    sy: usize,
    merge: usize,
    trust: usize,
) -> Vec<i32> {
    let pixels = width * height;
    if width < 3 || height < 3 || data.len() < pixels * 4 || sx >= width || sy >= height {
        return Vec::new();
    }
    let r = merge.min(8);

    // ① 内容掩码：先认背景，再认内容
    let Some(bg) = background(data, width, height) else {
        return Vec::new(); // 外圈没有主色（照片壁纸）：这画面没有「背景」可言
    };
    // 光标四周 merge 半径内一个内容像素都没有 = 停在空白处，直接放弃：
    // 整框扫描约 5ms，鼠标在页面空白上滑动时不该反复付这笔钱
    let near = (sy.saturating_sub(r)..=(sy + r).min(height - 1)).any(|y| {
        (sx.saturating_sub(r)..=(sx + r).min(width - 1))
            .any(|x| differs(&data[(y * width + x) * 4..], bg))
    });
    if !near {
        return Vec::new();
    }
    let mut mask = vec![0u8; pixels];
    for i in 0..pixels {
        mask[i] = u8::from(differs(&data[i * 4..], bg));
    }

    // ② 膨胀后从种子生长连通块（4 邻接，访问过就清零，每个像素至多入栈一次）
    let mut dil = dilate(&mask, width, height, r);
    if dil[sy * width + sx] == 0 {
        return Vec::new(); // 光标停在背景上：没有内容可吸附
    }
    let mut l = sx as i32;
    let mut t = sy as i32;
    let mut rr = l + 1;
    let mut bb = t + 1;
    // 收紧后的边界沿途只统计本连通块自己的原始像素：省掉再扫一遍包围盒，
    // 也不会把落在盒内的其它组件算进来
    let (mut tl, mut tt, mut tr, mut tb) = (i32::MAX, i32::MAX, i32::MIN, i32::MIN);
    let mut stack = vec![(sy * width + sx) as u32];
    dil[sy * width + sx] = 0;
    while let Some(i) = stack.pop() {
        let i = i as usize;
        let (y, x) = (i / width, i % width);
        l = l.min(x as i32);
        t = t.min(y as i32);
        rr = rr.max(x as i32 + 1);
        bb = bb.max(y as i32 + 1);
        if mask[i] != 0 {
            tl = tl.min(x as i32);
            tt = tt.min(y as i32);
            tr = tr.max(x as i32 + 1);
            tb = tb.max(y as i32 + 1);
        }
        if x > 0 && dil[i - 1] != 0 {
            dil[i - 1] = 0;
            stack.push((i - 1) as u32);
        }
        if x + 1 < width && dil[i + 1] != 0 {
            dil[i + 1] = 0;
            stack.push((i + 1) as u32);
        }
        if y > 0 && dil[i - width] != 0 {
            dil[i - width] = 0;
            stack.push((i - width) as u32);
        }
        if y + 1 < height && dil[i + width] != 0 {
            dil[i + width] = 0;
            stack.push((i + width) as u32);
        }
    }
    // 块贴到「人为」的那条边：它还在框外延续，切出来的边界是假的
    let cut = (l == 0 && trust & 1 == 0)
        || (t == 0 && trust & 2 == 0)
        || (rr as usize == width && trust & 4 == 0)
        || (bb as usize == height && trust & 8 == 0);
    if cut {
        return Vec::new();
    }
    if tr - tl < MIN_SIDE || tb - tt < MIN_SIDE {
        return Vec::new();
    }
    // 包围盒铺满取景框：多半是壁纸/噪点把整屏连成了一张稀疏的网，
    // 这种框等于没认出来，交回整窗吸附
    if i64::from(tr - tl) * i64::from(tb - tt) * 100 >= pixels as i64 * MAX_COVER {
        return Vec::new();
    }
    vec![tl, tt, tr, tb]
}

#[cfg(test)]
mod tests {
    use super::content_rect;

    const W: usize = 400;
    const H: usize = 300;

    /// 白底上画一块实心色块（100,80)-(260,200)
    fn canvas() -> Vec<u8> {
        let mut d = vec![0u8; W * H * 4];
        for i in 0..W * H {
            d[i * 4..i * 4 + 4].copy_from_slice(&[250, 250, 250, 255]);
        }
        for y in 80..200 {
            for x in 100..260 {
                d[(y * W + x) * 4..(y * W + x) * 4 + 4].copy_from_slice(&[30, 40, 220, 255]);
            }
        }
        d
    }

    #[test]
    fn 光标在内容上时框住内容块() {
        let d = canvas();
        assert_eq!(
            content_rect(&d, W, H, 140, 120, 3, 0),
            vec![100, 80, 260, 200]
        );
    }

    #[test]
    fn 光标在背景上时不吸附() {
        let d = canvas();
        assert!(content_rect(&d, W, H, 20, 20, 3, 0).is_empty());
    }

    #[test]
    fn 膨胀把碎片并成一块但边界收紧回原像素() {
        // 两条 4px 高的「文字行」间隔 5px：膨胀 3 后并成一块，边界仍是两行的外沿
        let mut d = canvas();
        for y in 210..214 {
            for x in 100..260 {
                d[(y * W + x) * 4..(y * W + x) * 4 + 4].copy_from_slice(&[10, 10, 10, 255]);
            }
        }
        for y in 219..223 {
            for x in 100..260 {
                d[(y * W + x) * 4..(y * W + x) * 4 + 4].copy_from_slice(&[10, 10, 10, 255]);
            }
        }
        assert_eq!(
            content_rect(&d, W, H, 150, 220, 3, 0),
            vec![100, 210, 260, 223]
        );
    }

    /// 顶部只有一行的残块（被取景框上边缘裁掉）也要和下方隔 5 行的内容并成一块。
    /// 这条专门盯膨胀竖趟的滑窗边界：多减一行就会在这里断开，返回空。
    #[test]
    fn 贴着取景框上边缘的残行也并得进来() {
        let mut d = canvas();
        let strip = |d: &mut Vec<u8>, y0: usize, y1: usize| {
            for y in y0..y1 {
                for x in 100..200 {
                    d[(y * W + x) * 4..(y * W + x) * 4 + 4].copy_from_slice(&[10, 10, 10, 255]);
                }
            }
        };
        strip(&mut d, 0, 1);
        strip(&mut d, 6, 10);
        // 清掉 canvas() 自带的大块，只留这两条
        for y in 80..200 {
            for x in 100..260 {
                d[(y * W + x) * 4..(y * W + x) * 4 + 4].copy_from_slice(&[250, 250, 250, 255]);
            }
        }
        assert_eq!(content_rect(&d, W, H, 150, 0, 3, 2), vec![100, 0, 200, 10]);
    }

    /// 一条 1px 斜线能把整框连成一个稀疏的网：像素很少但包围盒顶满四边，
    /// 这种「块」等于没认出来，必须判不可信（真实场景是壁纸噪点连成片）。
    #[test]
    fn 斜穿取景框的细网判不可信() {
        let mut d = canvas();
        for y in 80..200 {
            for x in 100..260 {
                d[(y * W + x) * 4..(y * W + x) * 4 + 4].copy_from_slice(&[250, 250, 250, 255]);
            }
        }
        for x in 0..W {
            let y = x * H / W;
            d[(y * W + x) * 4..(y * W + x) * 4 + 4].copy_from_slice(&[20, 200, 20, 255]);
        }
        assert!(content_rect(&d, W, H, 120, 90, 3, 15).is_empty());
    }

    /// 照片壁纸这种「外圈没有主色」的画面：直接判不可信，不进整框扫描。
    /// 块延伸到取景框右缘之外：右边不是真实边界时判被裁断（那条边是人为切的），
    /// 换成「右边=窗口边」才认这个块。
    #[test]
    fn 被取景框切断的块判不可信() {
        let mut d = canvas();
        for y in 80..200 {
            for x in 100..260 {
                d[(y * W + x) * 4..(y * W + x) * 4 + 4].copy_from_slice(&[250, 250, 250, 255]);
            }
        }
        for y in 100..200 {
            for x in 150..W {
                d[(y * W + x) * 4..(y * W + x) * 4 + 4].copy_from_slice(&[30, 40, 220, 255]);
            }
        }
        assert!(content_rect(&d, W, H, 200, 150, 3, 0).is_empty());
        assert_eq!(
            content_rect(&d, W, H, 200, 150, 3, 4),
            vec![150, 100, W as i32, 200]
        );
    }

    #[test]
    fn 外圈没有主色时判不可信() {
        let mut d = vec![0u8; W * H * 4];
        for y in 0..H {
            for x in 0..W {
                let p = (y * W + x) * 4;
                d[p] = ((x * 7 + y * 3) % 256) as u8;
                d[p + 1] = ((y * 13) % 256) as u8;
                d[p + 2] = ((x * 29 + 61) % 256) as u8;
                d[p + 3] = 255;
            }
        }
        assert!(content_rect(&d, W, H, 200, 150, 3, 0).is_empty());
    }

    #[test]
    fn 内容铺满取景框时判不可信() {
        // 只有一圈白边是背景：块几乎吃满整框，说明取景框选错了，交给整窗吸附
        let mut d = canvas();
        for y in 0..H {
            for x in 0..W {
                if x < 2 || y < 2 || x >= W - 2 || y >= H - 2 {
                    continue;
                }
                d[(y * W + x) * 4..(y * W + x) * 4 + 4].copy_from_slice(&[30, 40, 220, 255]);
            }
        }
        assert!(content_rect(&d, W, H, 200, 150, 3, 15).is_empty());
    }
}
