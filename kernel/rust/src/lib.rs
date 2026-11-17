#![cfg_attr(not(test), no_std)]

pub mod acpi;
pub mod fs;
pub mod net;
pub mod sched;
pub mod storage;
pub mod usb;

#[cfg(not(test))]
#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    // Message and location (file:line:col) go through the C shim's klog.
    crate::klog!("rust panic: {}\n", info);
    loop {
        #[cfg(target_arch = "x86_64")]
        unsafe {
            core::arch::asm!("cli; hlt", options(nomem, nostack, preserves_flags));
        }
        #[cfg(not(target_arch = "x86_64"))]
        core::hint::spin_loop();
    }
}
