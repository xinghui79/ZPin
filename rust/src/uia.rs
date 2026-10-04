//! UI Automation 元素吸附：屏幕坐标 → 命中元素的边界矩形与控制类型。
//!
//! 只做查询；慢 provider 熔断、坐标换算与命中校验留在 C++ 侧
//! （熔断需要计时与日志，贴着交互层走）。

use std::cell::OnceCell;
use std::sync::Once;

use windows::core::Result as WResult;
use windows::Win32::Foundation::POINT;
use windows::Win32::System::Com::{
    CoCreateInstance, CoInitializeEx, CLSCTX_INPROC_SERVER, COINIT_APARTMENTTHREADED,
};
use windows::Win32::UI::Accessibility::{CUIAutomation, IUIAutomation};

use crate::ffi::ElementInfo;

thread_local! {
    /// COM 初始化按线程做：查询在哪个线程发生，哪个线程就要有自己的套间。
    /// 实际只有 snap 工作线程会调到这里（元素查询已从选区交互线程挪走）；
    /// thread_local 是为了将来若增加第二个查询线程时不必再改。Qt 主线程通常
    /// 已被 OleInitialize 成 STA；返回码无关，能查到元素才是判据。
    static COM_INIT: Once = const { Once::new() };

    /// UIA 客户端实例是套间亲和的，按线程缓存一份：每次查询重新 CoCreateInstance
    /// 实测多花 0.57ms（总 3.9ms）。
    /// 注意这是 per-thread 的：若日后真让多个线程并发查询，各线程会各建一份
    /// IUIAutomation（COM apartment 亲和，无法跨线程共用）。
    static AUTOMATION: OnceCell<IUIAutomation> = const { OnceCell::new() };
}

fn com_init() {
    COM_INIT.with(|init| {
        init.call_once(|| unsafe {
            let _ = CoInitializeEx(None, COINIT_APARTMENTTHREADED);
        });
    });
}

fn automation() -> WResult<IUIAutomation> {
    com_init();
    AUTOMATION.with(|cell| {
        if let Some(ua) = cell.get() {
            return Ok(ua.clone());
        }
        let ua: IUIAutomation =
            unsafe { CoCreateInstance(&CUIAutomation, None, CLSCTX_INPROC_SERVER) }?;
        let _ = cell.set(ua.clone());
        Ok(ua)
    })
}

/// 查询 (x,y)（绝对物理像素）处的元素；离屏元素视为无命中。
pub fn query(x: i32, y: i32) -> WResult<ElementInfo> {
    unsafe {
        let automation = automation()?;
        let el = automation.ElementFromPoint(POINT { x, y })?;
        if el.CurrentIsOffscreen()?.as_bool() {
            return Err(windows::core::Error::empty());
        }
        let rect = el.CurrentBoundingRectangle()?;
        Ok(ElementInfo {
            x: rect.left,
            y: rect.top,
            w: rect.right - rect.left,
            h: rect.bottom - rect.top,
        })
    }
}

#[cfg(test)]
mod tests {
    /// 桌面 (1,1) 处必有元素或干净失败——只验证不 panic、不卡死。
    #[test]
    fn 查询不崩溃() {
        let _ = super::query(1, 1);
    }
}
