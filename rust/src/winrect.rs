//! 可见顶层窗口矩形枚举。
//!
//! 矩形是「真正看得见」的范围：优先 DWM 扩展边框（Win10/11 的 GetWindowRect
//! 比可见范围大一圈隐形边框），回退 GetWindowRect，再裁进虚拟桌面。
//! 跳过：不可见/最小化/子窗口/工具窗/透明窗/UWP 隐藏(cloaked)窗、桌面壳窗口
//! （Progman/WorkerW）、零尺寸窗；任务栏（Shell_TrayWnd 系）虽是工具窗仍保留，
//! 作为吸附与参考线基准。结果按 z 序从最上到最下排列。

use std::collections::HashSet;

use windows::Win32::Foundation::{BOOL, HWND, LPARAM, RECT};
use windows::Win32::Graphics::Dwm::{
    DwmGetWindowAttribute, DWMWA_CLOAKED, DWMWA_EXTENDED_FRAME_BOUNDS,
};
use windows::Win32::UI::WindowsAndMessaging::{
    EnumWindows, GetClassNameW, GetSystemMetrics, GetWindowLongPtrW, GetWindowRect, IsIconic,
    IsWindowVisible, GWL_EXSTYLE, GWL_STYLE, SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN,
    SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN, WNDENUMPROC, WS_CHILD, WS_EX_TOOLWINDOW,
    WS_EX_TRANSPARENT,
};

/// 任务栏类名：带 WS_EX_TOOLWINDOW 但必须放行的合法吸附目标。
fn is_shell_bar(class: &str) -> bool {
    class == "Shell_TrayWnd" || class == "Shell_SecondaryTrayWnd"
}

fn window_class(hwnd: HWND) -> String {
    let mut buf = [0u16; 64];
    let n = unsafe { GetClassNameW(hwnd, &mut buf) }.max(0) as usize;
    String::from_utf16_lossy(&buf[..n.min(buf.len())])
}

fn is_cloaked(hwnd: HWND) -> bool {
    let mut cloaked: u32 = 0;
    let hr = unsafe {
        DwmGetWindowAttribute(
            hwnd,
            DWMWA_CLOAKED,
            &mut cloaked as *mut u32 as *mut _,
            std::mem::size_of::<u32>() as u32,
        )
    };
    hr.is_ok() && cloaked != 0
}

/// 窗口真正可见的物理矩形（不含 DWM 不可见边框）；取不到返回 None。
fn visible_frame_rect(hwnd: HWND) -> Option<RECT> {
    let mut rc = RECT::default();
    let hr = unsafe {
        DwmGetWindowAttribute(
            hwnd,
            DWMWA_EXTENDED_FRAME_BOUNDS,
            &mut rc as *mut RECT as *mut _,
            std::mem::size_of::<RECT>() as u32,
        )
    };
    if hr.is_ok() && rc.right > rc.left && rc.bottom > rc.top {
        Some(rc)
    } else {
        None
    }
}

/// 物理像素下的虚拟桌面包围盒 (l, t, r, b)；取不到时返回 None（跳过裁剪）。
fn virtual_desktop() -> Option<(i32, i32, i32, i32)> {
    unsafe {
        let w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        let h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if w < 1 || h < 1 {
            return None;
        }
        let l = GetSystemMetrics(SM_XVIRTUALSCREEN);
        let t = GetSystemMetrics(SM_YVIRTUALSCREEN);
        Some((l, t, l + w, t + h))
    }
}

/// EnumWindows 回调的收集器（薄指针，可安全经 lparam 传递）。
struct Collector<'a> {
    exclude: &'a HashSet<isize>,
    out: Vec<(isize, i32, i32, i32, i32)>,
}

impl Collector<'_> {
    fn accept(&mut self, hwnd: HWND) {
        let raw = hwnd.0 as isize;
        if self.exclude.contains(&raw) {
            return;
        }
        if !unsafe { IsWindowVisible(hwnd) }.as_bool() || unsafe { IsIconic(hwnd) }.as_bool() {
            return;
        }
        let class = window_class(hwnd);
        if class == "Progman" || class == "WorkerW" {
            return; // 桌面壳窗口：吸上它等于"吸附整屏"
        }
        let shell_bar = is_shell_bar(&class);
        let style = unsafe { GetWindowLongPtrW(hwnd, GWL_STYLE) };
        let ex = unsafe { GetWindowLongPtrW(hwnd, GWL_EXSTYLE) };
        if !shell_bar
            && ((style & WS_CHILD.0 as isize != 0)
                || (ex & ((WS_EX_TOOLWINDOW.0 | WS_EX_TRANSPARENT.0) as isize) != 0))
        {
            return;
        }
        if is_cloaked(hwnd) {
            return;
        }
        let rc = match visible_frame_rect(hwnd) {
            Some(rc) => rc,
            None => {
                let mut rc = RECT::default();
                if unsafe { GetWindowRect(hwnd, &mut rc) }.is_err() {
                    return;
                }
                rc
            }
        };
        let (mut l, mut t, mut r, mut b) = (rc.left, rc.top, rc.right, rc.bottom);
        if let Some((dl, dt, dr, db)) = virtual_desktop() {
            l = l.max(dl);
            t = t.max(dt);
            r = r.min(dr);
            b = b.min(db);
        }
        if r - l < 1 || b - t < 1 {
            return;
        }
        self.out.push((raw, l, t, r, b));
    }
}

unsafe extern "system" fn enum_proc(hwnd: HWND, lparam: LPARAM) -> BOOL {
    let collector = &mut *(lparam.0 as *mut Collector);
    collector.accept(hwnd);
    true.into()
}

/// 按 z 序枚举可见顶层窗口，返回 (hwnd, l, t, r, b) 列表（最上在前）。
pub fn visible_window_rects(exclude: &HashSet<isize>) -> Vec<(isize, i32, i32, i32, i32)> {
    let mut collector = Collector {
        exclude,
        out: Vec::new(),
    };
    unsafe {
        let cb: WNDENUMPROC = Some(enum_proc);
        let _ = EnumWindows(cb, LPARAM(&mut collector as *mut _ as isize));
    }
    collector.out
}
