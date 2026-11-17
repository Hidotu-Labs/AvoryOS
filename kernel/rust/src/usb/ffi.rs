//! `extern "C"` declarations for the C shim in `drivers/usb/usb_rust_glue.c`.
//! Only compiled for the kernel target; host tests use the stubs in `kernel.rs`.

#[cfg(not(test))]
use core::ffi::{c_int, c_void};

#[cfg(not(test))]
extern "C" {
    pub fn usb_rust_control_transfer(
        handle: *mut c_void,
        request_type: u8,
        request: u8,
        value: u16,
        index: u16,
        data: *mut c_void,
        len: u16,
    ) -> c_int;
    pub fn usb_rust_device_descriptor(handle: *mut c_void, out: *mut u8, cap: usize) -> usize;
    pub fn usb_rust_get_config_descriptor(
        handle: *mut c_void,
        index: u8,
        out: *mut u8,
        cap: u16,
    ) -> c_int;

    pub fn usb_rust_interrupt_open(
        handle: *mut c_void,
        endpoint: u8,
        max_packet: u16,
        interval: u8,
        buf: *mut c_void,
    ) -> *mut c_void;
    pub fn usb_rust_interrupt_completed(pipe: *mut c_void) -> bool;
    pub fn usb_rust_interrupt_resubmit(pipe: *mut c_void) -> c_int;
    pub fn usb_rust_interrupt_cancel(pipe: *mut c_void);

    pub fn usb_rust_hub_configure(
        handle: *mut c_void,
        nports: u8,
        multi_tt: bool,
        tt_think_time: u8,
    ) -> c_int;
    pub fn usb_rust_device_speed(handle: *mut c_void) -> c_int;
    pub fn usb_rust_device_depth(handle: *mut c_void) -> u8;
    pub fn usb_rust_hub_child_discovered(handle: *mut c_void, port: u8, speed: c_int) -> c_int;
    pub fn usb_rust_hub_child_removed(handle: *mut c_void, port: u8);

    pub fn usb_rust_bulk_open(
        handle: *mut c_void,
        endpoint: u8,
        max_packet: u16,
        max_burst: u8,
    ) -> *mut c_void;
    pub fn usb_rust_bulk_transfer(
        pipe: *mut c_void,
        buf: *mut c_void,
        phys: u64,
        len: u32,
        actual: *mut u32,
        timeout_ms: u32,
    ) -> c_int;
    pub fn usb_rust_bulk_clear_halt(pipe: *mut c_void) -> c_int;
    pub fn usb_rust_bulk_close(pipe: *mut c_void);

    pub fn usb_rust_dma_alloc(size: usize, phys_out: *mut u64, handle_out: *mut *mut c_void)
        -> *mut c_void;
    pub fn usb_rust_dma_free(handle: *mut c_void);

    pub fn usb_rust_kmalloc(size: usize) -> *mut c_void;
    pub fn usb_rust_kfree(ptr: *mut c_void);
    pub fn usb_rust_mutex_create() -> *mut c_void;
    pub fn usb_rust_mutex_lock(mutex: *mut c_void);
    pub fn usb_rust_mutex_unlock(mutex: *mut c_void);
    pub fn usb_rust_mutex_destroy(mutex: *mut c_void);
    pub fn usb_rust_dma_phys(buf: *mut c_void) -> u64;
    pub fn usb_rust_thread_spawn(entry: extern "C" fn()) -> c_int;
    pub fn usb_rust_msleep(ms: u32);
    pub fn usb_rust_now_ms() -> u64;
    pub fn usb_rust_klog(s: *const u8, len: usize);
    pub fn usb_rust_diag_append(diag: *mut c_void, s: *const u8, len: usize);

    pub fn usb_rust_block_register(
        usb_dev: *mut c_void,
        sector_size: u32,
        total_sectors: u64,
        read_only: bool,
        ctx: *mut c_void,
        ops: *const BlockOps,
    ) -> *mut c_void;
    pub fn usb_rust_block_unregister(block: *mut c_void);
}

/// Mirror of `struct usb_rust_block_ops` in usb_rust_glue.h.
#[cfg(not(test))]
#[repr(C)]
pub struct BlockOps {
    pub read: extern "C" fn(ctx: *mut c_void, lba: u64, count: u32, buf: *mut c_void) -> c_int,
    pub write: extern "C" fn(ctx: *mut c_void, lba: u64, count: u32, buf: *const c_void) -> c_int,
    pub flush: extern "C" fn(ctx: *mut c_void) -> c_int,
}

