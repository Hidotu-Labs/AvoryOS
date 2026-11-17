//! USB class drivers in Rust.  Plan: docs/usb-storage-plan.md.
//!
//! Panic policy: no unwrap/expect/indexing/panic in this module tree.

#![deny(
    clippy::unwrap_used,
    clippy::expect_used,
    clippy::indexing_slicing,
    clippy::panic
)]

pub mod desc;
pub mod ffi;
pub mod hub;
pub mod storage;
pub mod kernel;

use core::ffi::c_void;

/// Called from `usb.c` after a device's VID/PID is logged.
#[no_mangle]
pub extern "C" fn rust_usb_device_probe(handle: *mut c_void, vid: u16, pid: u16) {
    let _ = handle;
    crate::klog!("rust usb: {:04x}:{:04x}\n", vid, pid);
}
