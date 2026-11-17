//! Safe wrappers over the USB C shim, plus the transport and clock traits
//! that protocol code is written against (so it runs under host `cargo test`).

use core::ffi::c_void;
use core::fmt;
use core::ptr;

#[cfg(not(test))]
use super::ffi;

/// Opaque C handle (`struct usb_device *`, `struct usb_interrupt_pipe *`,
/// `struct usb_diag *`).  Rust never dereferences these.
pub type Handle = *mut c_void;

pub const PAGE_SIZE: usize = 4096;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UsbError {
    /// Negative status from the host controller or USB core.
    Transport(i32),
    /// Allocation failed.
    NoMemory,
    /// Caller passed an unusable size or handle.
    Invalid,
    /// Output buffer smaller than the descriptor.
    BufferTooSmall,
}

pub type UsbResult<T> = Result<T, UsbError>;

/// The 8-byte SETUP packet of a control transfer.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ControlSetup {
    pub request_type: u8,
    pub request: u8,
    pub value: u16,
    pub index: u16,
    pub length: u16,
}

impl ControlSetup {
    /// Serialize to the wire layout (all multi-byte fields little-endian).
    pub const fn to_bytes(&self) -> [u8; 8] {
        let [v0, v1] = self.value.to_le_bytes();
        let [i0, i1] = self.index.to_le_bytes();
        let [l0, l1] = self.length.to_le_bytes();
        [self.request_type, self.request, v0, v1, i0, i1, l0, l1]
    }

    /// True for device-to-host transfers (bit 7 of bmRequestType).
    pub const fn is_device_to_host(&self) -> bool {
        self.request_type & 0x80 != 0
    }
}

/// Control and interrupt operations on one device.  Implemented by
/// [`KernelUsb`] for the kernel and by mock devices in host tests.
pub trait UsbTransport {
    /// Run a control transfer; `data` is the IN buffer or the OUT payload.
    fn control(&mut self, setup: ControlSetup, data: &mut [u8]) -> UsbResult<usize>;
    /// Open an interrupt IN pipe that reports into `buf`.
    fn interrupt_open(
        &mut self,
        endpoint: u8,
        max_packet: u16,
        interval: u8,
        buf: &DmaBuf,
    ) -> UsbResult<()>;
    /// If the last submitted interrupt transfer has completed, copy its
    /// report into `out` and return the number of bytes available.  Returns
    /// `None` while the transfer is still pending.
    fn interrupt_report(&mut self, out: &mut [u8]) -> Option<usize>;
    /// Opaque pipe handle for cancellation from another context (null when
    /// no pipe is open).
    fn pipe_handle(&self) -> Handle {
        ptr::null_mut()
    }
    /// Re-arm the interrupt pipe for the next report.
    fn interrupt_resubmit(&mut self) -> UsbResult<()>;
    /// Cancel the interrupt pipe.  The pipe is unusable afterwards.
    fn interrupt_cancel(&mut self);
}

/// Monotonic time and sleeping.
pub trait Clock {
    fn now_ms(&self) -> u64;
    fn sleep_ms(&self, ms: u32);
}

/// [`Clock`] backed by the kernel LAPIC timer.
#[derive(Clone, Copy, Debug, Default)]
pub struct KernelClock;

impl Clock for KernelClock {
    fn now_ms(&self) -> u64 {
        sys_now_ms()
    }
    fn sleep_ms(&self, ms: u32) {
        sys_msleep(ms);
    }
}

/// A USB device handle plus the interrupt pipe opened on it, if any.
pub struct KernelUsb {
    handle: Handle,
    pipe: Handle,
    /// DMA buffer the open pipe reports into.  Owned by the caller, which must
    /// keep it alive until the pipe is cancelled.
    report: *const u8,
    report_len: usize,
}

impl KernelUsb {
    pub const fn new(handle: Handle) -> Self {
        Self {
            handle,
            pipe: ptr::null_mut(),
            report: ptr::null(),
            report_len: 0,
        }
    }

    pub const fn handle(&self) -> Handle {
        self.handle
    }

    /// Copy the cached 18-byte device descriptor into `out`.
    pub fn device_descriptor(&self, out: &mut [u8]) -> usize {
        sys_device_descriptor(self.handle, out.as_mut_ptr(), out.len())
    }

    /// Fetch configuration descriptor `index` in full into `out`.
    pub fn config_descriptor(&self, index: u8, out: &mut [u8]) -> UsbResult<usize> {
        let cap = u16::try_from(out.len()).map_err(|_| UsbError::Invalid)?;
        match sys_config_descriptor(self.handle, index, out.as_mut_ptr(), cap) {
            n if n >= 0 => Ok(n as usize),
            -2 => Err(UsbError::BufferTooSmall),
            rc => Err(UsbError::Transport(rc)),
        }
    }
}

impl UsbTransport for KernelUsb {
    fn control(&mut self, setup: ControlSetup, data: &mut [u8]) -> UsbResult<usize> {
        let len = u16::try_from(data.len()).map_err(|_| UsbError::Invalid)?;
        let rc = sys_control(self.handle, setup, data.as_mut_ptr(), len);
        if rc < 0 {
            Err(UsbError::Transport(rc))
        } else {
            Ok(rc as usize)
        }
    }

    fn interrupt_open(
        &mut self,
        endpoint: u8,
        max_packet: u16,
        interval: u8,
        buf: &DmaBuf,
    ) -> UsbResult<()> {
        let pipe = sys_interrupt_open(self.handle, endpoint, max_packet, interval, buf.ptr.cast());
        if pipe.is_null() {
            return Err(UsbError::Transport(-1));
        }
        self.pipe = pipe;
        self.report = buf.ptr;
        self.report_len = buf.len;
        Ok(())
    }

    fn interrupt_report(&mut self, out: &mut [u8]) -> Option<usize> {
        if self.pipe.is_null() || !sys_interrupt_completed(self.pipe) {
            return None;
        }
        let n = self.report_len.min(out.len());
        // SAFETY: `report` is the DmaBuf passed to interrupt_open, which the
        // owner keeps alive for as long as the pipe is open; `n` is bounded by
        // both lengths.
        unsafe { ptr::copy_nonoverlapping(self.report, out.as_mut_ptr(), n) };
        Some(n)
    }

    fn pipe_handle(&self) -> Handle {
        self.pipe
    }

    fn interrupt_resubmit(&mut self) -> UsbResult<()> {
        if self.pipe.is_null() {
            return Err(UsbError::Invalid);
        }
        match sys_interrupt_resubmit(self.pipe) {
            0.. => Ok(()),
            rc => Err(UsbError::Transport(rc)),
        }
    }

    fn interrupt_cancel(&mut self) {
        if !self.pipe.is_null() {
            sys_interrupt_cancel(self.pipe);
            self.pipe = ptr::null_mut();
            self.report = ptr::null();
            self.report_len = 0;
        }
    }
}

/// Static storage owned by one kernel thread (the hub worker).  Stands in
/// for a stack array that would not fit a 16 KiB kernel stack.
pub struct WorkerCell<T>(core::cell::UnsafeCell<T>);

// SAFETY: the only accessor is `with`, and the design is that exactly one
// kernel thread calls it.  Nothing else touches the contents.
unsafe impl<T> Sync for WorkerCell<T> {}

impl<T> WorkerCell<T> {
    pub const fn new(value: T) -> Self {
        Self(core::cell::UnsafeCell::new(value))
    }

    /// Run `f` with exclusive access.  Must be called from the owning thread
    /// only, and must not be re-entered.
    pub fn with<R>(&self, f: impl FnOnce(&mut T) -> R) -> R {
        // SAFETY: see the type-level comment: single owner, no re-entry.
        f(unsafe { &mut *self.0.get() })
    }
}

/// Cancel a pipe by raw handle, for a remover that does not own the
/// transport.  The C side is a no-op once the pipe is inactive.
pub fn cancel_pipe(pipe: Handle) {
    if !pipe.is_null() {
        sys_interrupt_cancel(pipe);
    }
}

/// Facts about a device and the hub operations the C core provides for it.
pub fn device_speed(handle: Handle) -> i32 {
    sys_device_speed(handle)
}
pub fn device_depth(handle: Handle) -> u8 {
    sys_device_depth(handle)
}
/// Program the hub's Slot Context.  False on failure.
pub fn hub_configure(handle: Handle, nports: u8, multi_tt: bool, think: u8) -> bool {
    sys_hub_configure(handle, nports, multi_tt, think) == 0
}
/// Attach the device behind hub `port`.  False when the topology refuses it.
pub fn hub_child_discovered(handle: Handle, port: u8, speed: i32) -> bool {
    sys_hub_child_discovered(handle, port, speed) == 0
}
pub fn hub_child_removed(handle: Handle, port: u8) {
    sys_hub_child_removed(handle, port)
}

/// Spawn a kernel thread running `entry`.
pub fn spawn_kernel_thread(entry: extern "C" fn()) -> UsbResult<()> {
    match sys_thread_spawn(entry) {
        0 => Ok(()),
        rc => Err(UsbError::Transport(rc)),
    }
}

/// Kernel mutex (yield-based, may be held across sleeping transfers).
pub struct KMutex {
    raw: *mut c_void,
}

// SAFETY: the shim mutex uses an atomic owner flag and is cross-CPU safe.
unsafe impl Send for KMutex {}
unsafe impl Sync for KMutex {}

impl Drop for KMutex {
    fn drop(&mut self) {
        sys_mutex_destroy(self.raw);
    }
}

impl KMutex {
    pub fn new() -> UsbResult<Self> {
        let raw = sys_mutex_create();
        if raw.is_null() {
            Err(UsbError::NoMemory)
        } else {
            Ok(Self { raw })
        }
    }

    pub fn lock(&self) -> MutexGuard<'_> {
        sys_mutex_lock(self.raw);
        MutexGuard { mutex: self }
    }
}

/// RAII guard: the mutex is released when the guard drops.
pub struct MutexGuard<'a> {
    mutex: &'a KMutex,
}

impl Drop for MutexGuard<'_> {
    fn drop(&mut self) {
        sys_mutex_unlock(self.mutex.raw);
    }
}

/// Zeroed heap buffer from `kmalloc`, freed on drop.  Size is capped at one
/// page so the physical address derived from the HHDM mapping is contiguous.
pub struct DmaBuf {
    ptr: *mut u8,
    len: usize,
}

impl DmaBuf {
    pub fn new(len: usize) -> UsbResult<Self> {
        if len == 0 || len > PAGE_SIZE {
            return Err(UsbError::Invalid);
        }
        let ptr = sys_kmalloc(len);
        if ptr.is_null() {
            return Err(UsbError::NoMemory);
        }
        // SAFETY: kmalloc returned `len` writable bytes.
        unsafe { ptr::write_bytes(ptr, 0, len) };
        Ok(Self { ptr, len })
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn as_slice(&self) -> &[u8] {
        // SAFETY: `ptr` is valid for `len` bytes for the life of `self`.
        unsafe { core::slice::from_raw_parts(self.ptr, self.len) }
    }

    pub fn as_mut_slice(&mut self) -> &mut [u8] {
        // SAFETY: as above, and `&mut self` gives exclusive access.
        unsafe { core::slice::from_raw_parts_mut(self.ptr, self.len) }
    }
}

impl Drop for DmaBuf {
    fn drop(&mut self) {
        sys_kfree(self.ptr.cast());
    }
}

/// `core::fmt::Write` sink for `struct usb_diag`.  Output past the buffer
/// capacity is truncated by the C side.
pub struct Diag {
    raw: Handle,
}

impl Diag {
    pub fn new(raw: Handle) -> Option<Self> {
        if raw.is_null() {
            None
        } else {
            Some(Self { raw })
        }
    }
}

impl fmt::Write for Diag {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        sys_diag_append(self.raw, s);
        Ok(())
    }
}

struct KlogWriter;

impl fmt::Write for KlogWriter {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        sys_klog(s);
        Ok(())
    }
}

/// Backend of [`klog!`](crate::klog).  Public only so the macro can reach it.
#[doc(hidden)]
pub fn klog_args(args: fmt::Arguments<'_>) {
    let _ = fmt::write(&mut KlogWriter, args);
}

/// Format to the kernel log through the C shim, without allocating.
#[macro_export]
macro_rules! klog {
    ($($arg:tt)*) => {
        $crate::usb::kernel::klog_args(format_args!($($arg)*))
    };
}

// --- Shim dispatch -------------------------------------------------------
// Each helper calls the C shim on the kernel target.  Under `cargo test` the
// stub bodies below are used instead, so the wrappers above stay testable.

#[cfg(not(test))]
fn sys_klog(s: &str) {
    unsafe { ffi::usb_rust_klog(s.as_ptr(), s.len()) }
}
#[cfg(test)]
fn sys_klog(_s: &str) {}

#[cfg(not(test))]
fn sys_diag_append(diag: Handle, s: &str) {
    unsafe { ffi::usb_rust_diag_append(diag, s.as_ptr(), s.len()) }
}
#[cfg(test)]
fn sys_diag_append(_diag: Handle, _s: &str) {}

#[cfg(not(test))]
fn sys_kmalloc(size: usize) -> *mut u8 {
    unsafe { ffi::usb_rust_kmalloc(size) }.cast()
}
#[cfg(test)]
fn sys_kmalloc(size: usize) -> *mut u8 {
    // Host tests leak these buffers; they are small and the process exits.
    std::vec![0u8; size.max(1)].leak().as_mut_ptr()
}

#[cfg(not(test))]
fn sys_kfree(p: *mut c_void) {
    unsafe { ffi::usb_rust_kfree(p) }
}
#[cfg(test)]
fn sys_kfree(_p: *mut c_void) {}

#[cfg(not(test))]
fn sys_mutex_create() -> *mut c_void {
    unsafe { ffi::usb_rust_mutex_create() }
}
#[cfg(test)]
fn sys_mutex_create() -> *mut c_void {
    ptr::NonNull::<u8>::dangling().as_ptr().cast()
}

#[cfg(not(test))]
fn sys_mutex_lock(m: *mut c_void) {
    unsafe { ffi::usb_rust_mutex_lock(m) }
}
#[cfg(test)]
fn sys_mutex_lock(_m: *mut c_void) {}

#[cfg(not(test))]
fn sys_mutex_unlock(m: *mut c_void) {
    unsafe { ffi::usb_rust_mutex_unlock(m) }
}
#[cfg(test)]
fn sys_mutex_unlock(_m: *mut c_void) {}

#[cfg(not(test))]
fn sys_mutex_destroy(m: *mut c_void) {
    unsafe { ffi::usb_rust_mutex_destroy(m) }
}
#[cfg(test)]
fn sys_mutex_destroy(_m: *mut c_void) {}

#[cfg(not(test))]
fn sys_thread_spawn(entry: extern "C" fn()) -> i32 {
    unsafe { ffi::usb_rust_thread_spawn(entry) }
}
#[cfg(test)]
fn sys_thread_spawn(_entry: extern "C" fn()) -> i32 {
    -1
}

#[cfg(not(test))]
fn sys_msleep(ms: u32) {
    unsafe { ffi::usb_rust_msleep(ms) }
}
#[cfg(test)]
fn sys_msleep(_ms: u32) {}

#[cfg(not(test))]
fn sys_now_ms() -> u64 {
    unsafe { ffi::usb_rust_now_ms() }
}
#[cfg(test)]
fn sys_now_ms() -> u64 {
    0
}

#[cfg(not(test))]
fn sys_control(h: Handle, setup: ControlSetup, data: *mut u8, len: u16) -> i32 {
    unsafe {
        ffi::usb_rust_control_transfer(
            h,
            setup.request_type,
            setup.request,
            setup.value,
            setup.index,
            data.cast(),
            len,
        )
    }
}
#[cfg(test)]
fn sys_control(_h: Handle, _setup: ControlSetup, _data: *mut u8, _len: u16) -> i32 {
    -1
}

#[cfg(not(test))]
fn sys_device_descriptor(h: Handle, out: *mut u8, cap: usize) -> usize {
    unsafe { ffi::usb_rust_device_descriptor(h, out, cap) }
}
#[cfg(test)]
fn sys_device_descriptor(_h: Handle, _out: *mut u8, _cap: usize) -> usize {
    0
}

#[cfg(not(test))]
fn sys_config_descriptor(h: Handle, index: u8, out: *mut u8, cap: u16) -> i32 {
    unsafe { ffi::usb_rust_get_config_descriptor(h, index, out, cap) }
}
#[cfg(test)]
fn sys_config_descriptor(_h: Handle, _index: u8, _out: *mut u8, _cap: u16) -> i32 {
    -1
}

#[cfg(not(test))]
fn sys_interrupt_open(h: Handle, ep: u8, mps: u16, interval: u8, buf: *mut c_void) -> Handle {
    unsafe { ffi::usb_rust_interrupt_open(h, ep, mps, interval, buf) }
}
#[cfg(test)]
fn sys_interrupt_open(_h: Handle, _ep: u8, _mps: u16, _interval: u8, _buf: *mut c_void) -> Handle {
    ptr::null_mut()
}

#[cfg(not(test))]
fn sys_interrupt_completed(p: Handle) -> bool {
    unsafe { ffi::usb_rust_interrupt_completed(p) }
}
#[cfg(test)]
fn sys_interrupt_completed(_p: Handle) -> bool {
    false
}
#[cfg(not(test))]
fn sys_device_speed(h: Handle) -> i32 {
    unsafe { ffi::usb_rust_device_speed(h) }
}
#[cfg(test)]
fn sys_device_speed(_h: Handle) -> i32 {
    0
}
#[cfg(not(test))]
fn sys_device_depth(h: Handle) -> u8 {
    unsafe { ffi::usb_rust_device_depth(h) }
}
#[cfg(test)]
fn sys_device_depth(_h: Handle) -> u8 {
    0
}
#[cfg(not(test))]
fn sys_hub_configure(h: Handle, nports: u8, multi_tt: bool, think: u8) -> i32 {
    unsafe { ffi::usb_rust_hub_configure(h, nports, multi_tt, think) }
}
#[cfg(test)]
fn sys_hub_configure(_h: Handle, _nports: u8, _multi_tt: bool, _think: u8) -> i32 {
    -1
}
#[cfg(not(test))]
fn sys_hub_child_discovered(h: Handle, port: u8, speed: i32) -> i32 {
    unsafe { ffi::usb_rust_hub_child_discovered(h, port, speed) }
}
#[cfg(test)]
fn sys_hub_child_discovered(_h: Handle, _port: u8, _speed: i32) -> i32 {
    -1
}
#[cfg(not(test))]
fn sys_hub_child_removed(h: Handle, port: u8) {
    unsafe { ffi::usb_rust_hub_child_removed(h, port) }
}
#[cfg(test)]
fn sys_hub_child_removed(_h: Handle, _port: u8) {}

#[cfg(not(test))]
fn sys_interrupt_resubmit(p: Handle) -> i32 {
    unsafe { ffi::usb_rust_interrupt_resubmit(p) }
}
#[cfg(test)]
fn sys_interrupt_resubmit(_p: Handle) -> i32 {
    -1
}

#[cfg(not(test))]
fn sys_interrupt_cancel(p: Handle) {
    unsafe { ffi::usb_rust_interrupt_cancel(p) }
}
#[cfg(test)]
fn sys_interrupt_cancel(_p: Handle) {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn control_setup_serializes_little_endian() {
        let s = ControlSetup {
            request_type: 0x80,
            request: 6,
            value: 0x0200,
            index: 0x0102,
            length: 0x0012,
        };
        assert_eq!(s.to_bytes(), [0x80, 6, 0x00, 0x02, 0x02, 0x01, 0x12, 0x00]);
        assert!(s.is_device_to_host());
    }

    #[test]
    fn dma_buf_rejects_bad_sizes_before_allocating() {
        assert_eq!(DmaBuf::new(0).err(), Some(UsbError::Invalid));
        assert_eq!(DmaBuf::new(PAGE_SIZE + 1).err(), Some(UsbError::Invalid));
    }

    #[test]
    fn kernel_usb_without_pipe_reports_no_completion() {
        let mut dev = KernelUsb::new(ptr::null_mut());
        assert_eq!(dev.interrupt_report(&mut [0u8; 4]), None);
        assert_eq!(dev.interrupt_resubmit(), Err(UsbError::Invalid));
    }
}
