//! 热点像素运算：三轮盒滤波近似高斯（横+纵 ×3，O(n) 与半径无关）。

/// 一趟盒滤波：4 通道前缀和均值，端点窗口收缩（无边缘增亮）。
/// outer*inner 个像素；外/内层字节步长分离，横纵两向共用同一实现。
fn box_pass(
    src: &[u8],
    dst: &mut [u8],
    outer: usize,
    inner: usize,
    outer_stride: usize,
    inner_stride: usize,
    radius: usize,
) {
    let mut pre = vec![0u32; (inner + 1) * 4];
    // 定点倒数表：除法（~20 周期）换 64 位乘+移位（~3 周期），误差 ≤1 LSB
    let max_n = (2 * radius + 1).min(inner);
    let inv: Vec<u32> = (1..=max_n)
        .map(|n| ((1u32 << 20) + n as u32 - 1) / n as u32)
        .collect();
    for o in 0..outer {
        let base = o * outer_stride;
        for i in 0..inner {
            let p = base + i * inner_stride;
            for c in 0..4 {
                pre[(i + 1) * 4 + c] = pre[i * 4 + c] + src[p + c] as u32;
            }
        }
        for i in 0..inner {
            let lo = i.saturating_sub(radius);
            let hi = (i + radius + 1).min(inner);
            let inv_n = inv[hi - lo - 1] as u64; // 表按 n=1..=max_n 存，下标 n-1
            let p = base + i * inner_stride;
            for c in 0..4 {
                let sum = (pre[hi * 4 + c] - pre[lo * 4 + c]) as u64;
                dst[p + c] = ((sum * inv_n) >> 20) as u8;
            }
        }
    }
}

/// 三轮盒滤波（横+纵 ×3）近似高斯；六趟后结果回到返回缓冲。
pub fn three_box_blur(data: &[u8], width: usize, height: usize, radius: usize) -> Vec<u8> {
    let stride = width * 4;
    let mut a: Vec<u8> = data[..stride * height].to_vec();
    let mut b: Vec<u8> = vec![0u8; stride * height];
    for _ in 0..3 {
        box_pass(&a, &mut b, height, width, stride, 4, radius);
        box_pass(&b, &mut a, width, height, 4, stride, radius);
    }
    a
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 常数图保持常数() {
        let data = vec![200u8; 32 * 16 * 4];
        let out = three_box_blur(&data, 32, 16, 6);
        assert!(out.chunks_exact(4).all(|px| px == [200, 200, 200, 200]));
    }

    #[test]
    fn 深黑底白斑被摊开且边缘过渡() {
        let (w, h) = (64usize, 64usize);
        let mut data = vec![0u8; w * h * 4];
        for y in 28..36 {
            for x in 28..36 {
                let p = (y * w + x) * 4;
                data[p..p + 3].copy_from_slice(&[255, 255, 255]);
                data[p + 3] = 255;
            }
        }
        let out = three_box_blur(&data, w, h, 4);
        let at = |x: usize, y: usize| {
            let p = (y * w + x) * 4;
            (out[p], out[p + 1], out[p + 2])
        };
        // 斑心被摊薄但非零；角落保持深黑；边缘存在过渡
        assert!(at(32, 32).0 > 0 && at(32, 32).0 < 255);
        assert_eq!(at(2, 2).0, 0);
        assert!(at(24, 32).0 > 0 && at(24, 32).0 < at(32, 32).0);
    }
}
