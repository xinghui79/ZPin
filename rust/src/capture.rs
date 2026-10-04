//! 桌面抓图的一块矩形区域：BitBlt 到 32 位 DIB，输出 BGRA 物理像素。
//!
//! 坐标是绝对物理像素（虚拟桌面原点系）；多屏画布的拼接与换算在 C++ 侧
//! （capture.cpp 的 DesktopMap）。第 4 字节是 GDI 保留位，C++ 侧按
//! Format_RGB32 读入即视为不透明。

use windows::Win32::Foundation::HANDLE;
use windows::Win32::Graphics::Gdi::{
    BitBlt, CreateCompatibleDC, CreateDIBSection, DeleteDC, DeleteObject, GetDC, ReleaseDC,
    SelectObject, BITMAPINFO, BITMAPINFOHEADER, BI_RGB, CAPTUREBLT, DIB_RGB_COLORS, SRCCOPY,
};

/// 抓取虚拟桌面 (x,y,w,h) 物理像素区域，返回 BGRA 自上而下缓冲。
pub fn grab_bgra(x: i32, y: i32, w: i32, h: i32) -> Vec<u8> {
    grab_impl(x, y, w, h, true)
}

/// 同 grab_bgra，但不带 CAPTUREBLT：滚动长截图 60~300ms 一次的轮询抓帧用。
/// CAPTUREBLT 每次都会触发分层窗重绘，部分系统上光标跟着闪——用户要盯着
/// 选区滚完整个页面，反复闪烁很扰人。代价是区域内的分层窗（半透明悬浮窗）
/// 不入帧，对滚动截取的网页/文档场景无所谓。
pub fn grab_bgra_plain(x: i32, y: i32, w: i32, h: i32) -> Vec<u8> {
    grab_impl(x, y, w, h, false)
}

fn grab_impl(x: i32, y: i32, w: i32, h: i32, cap_blt: bool) -> Vec<u8> {
    if w <= 0 || h <= 0 {
        return Vec::new();
    }
    let len = w as usize * h as usize * 4;
    unsafe {
        let src = GetDC(None);
        let mem = CreateCompatibleDC(src);
        let mut bi = BITMAPINFO::default();
        bi.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h; // 负高 = 自上而下
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB.0;
        let mut bits: *mut core::ffi::c_void = std::ptr::null_mut();
        // 抓屏失败在这几种场景都常见：锁屏 / UAC 安全桌面 / 独占全屏 / GDI 资源紧张。
        // 这里绝不能 unwrap —— panic 穿过 cxx 生成的 extern "C" 边界会直接 abort，
        // 连 lib.rs 里那个 panic hook 都来不及写全。返回空缓冲，由 C++ 侧按
        // 抓取失败处理（capture.cpp 已有该分支）。
        let bmp = match CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &mut bits, HANDLE::default(), 0)
        {
            Ok(bmp) => bmp,
            Err(_) => {
                let _ = DeleteDC(mem);
                ReleaseDC(None, src);
                return Vec::new();
            }
        };
        let old = SelectObject(mem, bmp);
        // 上面的 CreateDIBSection 既然成功，bits 一定非空
        let mode = if cap_blt { SRCCOPY | CAPTUREBLT } else { SRCCOPY };
        let data = if BitBlt(mem, 0, 0, w, h, src, x, y, mode).is_ok() {
            std::slice::from_raw_parts(bits as *const u8, len).to_vec()
        } else {
            Vec::new()
        };
        SelectObject(mem, old);
        let _ = DeleteObject(bmp);
        let _ = DeleteDC(mem);
        ReleaseDC(None, src);
        data
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 只验尺寸：不做「两次抓取逐字节相同」的确定性断言 —— 两次 BitBlt 之间
    /// 屏幕内容（闪烁光标 / 任务栏时钟 / 动画）可能变化，那断言是偶发挂的
    /// flaky 测试。
    #[test]
    fn 抓屏尺寸正确() {
        let a = grab_bgra(0, 0, 120, 80);
        assert_eq!(a.len(), 120 * 80 * 4);
    }

    #[test]
    fn 非法尺寸返回空() {
        assert!(grab_bgra(0, 0, 0, 10).is_empty());
        assert!(grab_bgra(0, 0, 10, -1).is_empty());
    }
}
