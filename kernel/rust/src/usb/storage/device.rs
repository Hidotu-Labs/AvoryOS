//! One mass-storage LUN (LUN 0): start-up, quirks, and sector I/O split to the
//! command limit through a bounce buffer.
//!
//! [`Device`] is generic over the transport and the bounce buffer, so host
//! tests run it against a scripted device.  The kernel instantiation lives in
//! [`hw`] (kernel target only).

use super::bot::{Bot, BotStats, BulkTransport};
use super::scsi::{self, Inquiry, ScsiError};
use crate::usb::kernel::Clock;

/// Largest data phase the bounce buffer holds.
pub const BOUNCE_LEN: usize = 128 * 1024;
/// Sectors per command when no quirk applies.
pub const MAX_CMD_SECTORS: u32 = 240;
const MAX_CMD_SECTORS_QUIRK: u32 = 64;

/// Skip SYNCHRONIZE CACHE; flush succeeds without touching the device.
pub const QUIRK_NO_SYNC_CACHE: u32 = 1 << 0;
/// Cap commands at 64 sectors.
pub const QUIRK_MAX_SECTORS_64: u32 = 1 << 1;
/// Accept a CSW residue larger than the requested length.
pub const QUIRK_IGNORE_RESIDUE: u32 = 1 << 2;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Quirk {
    pub vid: u16,
    pub pid: u16,
    pub flags: u32,
}

/// Known-bad devices.  Empty until a device needs one.
pub const QUIRKS: &[Quirk] = &[];

pub fn quirk_flags(vid: u16, pid: u16) -> u32 {
    QUIRKS
        .iter()
        .filter(|q| q.vid == vid && q.pid == pid)
        .fold(0, |acc, q| acc | q.flags)
}

/// Storage for the bounce buffer: at least [`BOUNCE_LEN`] bytes of DMA memory.
pub trait Bounce {
    fn bounce(&mut self) -> &mut [u8];
}

/// Copy of the device facts the kernel side publishes for diagnostics.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Snapshot {
    pub capacity_sectors: u64,
    pub sector_size: u32,
    pub read_only: bool,
    pub max_lun: u8,
    pub stats: BotStats,
}

/// Mass-storage LUN.  Field order matters: `bot` (which owns the transport and
/// its pipes) drops before `buf` (the DMA memory the pipes report into).
pub struct Device<T: BulkTransport, B: Bounce> {
    pub(super) bot: Bot<T>,
    buf: B,
    pub inquiry: Inquiry,
    /// Addressable sectors (last LBA + 1).
    pub capacity_sectors: u64,
    pub sector_size: u32,
    pub read_only: bool,
    pub max_lun: u8,
    cmd_sectors: u32,
    flags: u32,
}

fn valid_sector_size(size: u32) -> bool {
    (512..=4096).contains(&size) && size.is_power_of_two()
}

/// GET MAX LUN.  A stall (or any failure) means one LUN, numbered 0.
fn get_max_lun<T: BulkTransport>(bot: &mut Bot<T>, iface: u8) -> u8 {
    let mut b = [0u8; 1];
    match bot
        .transport_mut()
        .control(0xA1, 0xFE, 0, u16::from(iface), &mut b)
    {
        Ok(_) => b.first().copied().unwrap_or(0),
        Err(_) => 0,
    }
}

impl<T: BulkTransport, B: Bounce> Device<T, B> {
    /// Bring the LUN up: GET MAX LUN, INQUIRY, TEST UNIT READY (with spin-up
    /// backoff), READ CAPACITY, and the write-protect bit.
    pub fn start<C: Clock>(
        transport: T,
        buf: B,
        iface: u8,
        flags: u32,
        clock: &C,
    ) -> Result<Self, ScsiError> {
        let mut bot = Bot::new(transport, iface);
        bot.ignore_residue = flags & QUIRK_IGNORE_RESIDUE != 0;
        let max_lun = get_max_lun(&mut bot, iface);
        let inquiry = scsi::inquiry(&mut bot)?;
        scsi::test_unit_ready(&mut bot, clock)?;
        let cap = scsi::read_capacity(&mut bot)?;
        if !valid_sector_size(cap.block_size) {
            return Err(ScsiError::BadResponse);
        }
        let capacity_sectors = cap.last_lba.checked_add(1).ok_or(ScsiError::BadResponse)?;
        let read_only = scsi::write_protected(&mut bot).unwrap_or(false);

        let limit = if flags & QUIRK_MAX_SECTORS_64 != 0 {
            MAX_CMD_SECTORS_QUIRK
        } else {
            MAX_CMD_SECTORS
        };
        let sector_bytes = usize::try_from(cap.block_size).map_err(|_| ScsiError::BadResponse)?;
        let fit = u32::try_from(BOUNCE_LEN / sector_bytes).unwrap_or(1);
        Ok(Self {
            bot,
            buf,
            inquiry,
            capacity_sectors,
            sector_size: cap.block_size,
            read_only,
            max_lun,
            cmd_sectors: limit.min(fit),
            flags,
        })
    }

    pub fn snapshot(&self) -> Snapshot {
        Snapshot {
            capacity_sectors: self.capacity_sectors,
            sector_size: self.sector_size,
            read_only: self.read_only,
            max_lun: self.max_lun,
            stats: self.bot.stats,
        }
    }

    /// Bytes in `count` sectors, or None on overflow.
    pub fn span_bytes(&self, count: u32) -> Option<usize> {
        let sector = usize::try_from(self.sector_size).ok()?;
        usize::try_from(count).ok()?.checked_mul(sector)
    }

    /// Sectors per command after the quirk and bounce-buffer limits.
    pub fn command_sectors(&self) -> u32 {
        self.cmd_sectors
    }

    fn check_range(&self, lba: u64, count: u32) -> Result<(), ScsiError> {
        let end = lba.checked_add(u64::from(count)).ok_or(ScsiError::Range)?;
        if end > self.capacity_sectors {
            return Err(ScsiError::Range);
        }
        Ok(())
    }

    /// Read `count` sectors at `lba` into `dst`, which must be exactly
    /// `count` sectors long.
    pub fn read(&mut self, lba: u64, count: u32, dst: &mut [u8]) -> Result<(), ScsiError> {
        self.check_range(lba, count)?;
        if self.span_bytes(count) != Some(dst.len()) {
            return Err(ScsiError::Range);
        }
        let mut done = 0u32;
        while done < count {
            let n = (count - done).min(self.cmd_sectors);
            let nbytes = self.span_bytes(n).ok_or(ScsiError::Range)?;
            let off = self.span_bytes(done).ok_or(ScsiError::Range)?;
            let chunk = self
                .buf
                .bounce()
                .get_mut(..nbytes)
                .ok_or(ScsiError::Range)?;
            scsi::read_blocks(&mut self.bot, lba + u64::from(done), n, chunk)?;
            let out = dst.get_mut(off..off + nbytes).ok_or(ScsiError::Range)?;
            out.copy_from_slice(chunk);
            done += n;
        }
        Ok(())
    }

    /// Write `count` sectors at `lba` from `src`, which must be exactly
    /// `count` sectors long.
    pub fn write(&mut self, lba: u64, count: u32, src: &[u8]) -> Result<(), ScsiError> {
        if self.read_only {
            return Err(ScsiError::ReadOnly);
        }
        self.check_range(lba, count)?;
        if self.span_bytes(count) != Some(src.len()) {
            return Err(ScsiError::Range);
        }
        let mut done = 0u32;
        while done < count {
            let n = (count - done).min(self.cmd_sectors);
            let nbytes = self.span_bytes(n).ok_or(ScsiError::Range)?;
            let off = self.span_bytes(done).ok_or(ScsiError::Range)?;
            let chunk = self
                .buf
                .bounce()
                .get_mut(..nbytes)
                .ok_or(ScsiError::Range)?;
            let input = src.get(off..off + nbytes).ok_or(ScsiError::Range)?;
            chunk.copy_from_slice(input);
            scsi::write_blocks(&mut self.bot, lba + u64::from(done), n, chunk)?;
            done += n;
        }
        Ok(())
    }

    /// Flush the device cache, unless the quirk table says the device rejects
    /// SYNCHRONIZE CACHE.
    pub fn flush(&mut self) -> Result<(), ScsiError> {
        if self.flags & QUIRK_NO_SYNC_CACHE != 0 {
            return Ok(());
        }
        scsi::sync_cache(&mut self.bot)
    }
}

/// Kernel instantiation: the C shim's bulk pipes and a DMA bounce area.
#[cfg(not(test))]
pub mod hw {
    use core::ffi::c_void;
    use core::ptr;

    use super::super::bot::{BulkTransport, Xfer};
    use super::{Bounce, BOUNCE_LEN};
    use crate::usb::ffi;

    /// Bytes past the bounce buffer, for command and status blocks that live
    /// on the Rust stack (not DMA memory).
    const STAGE_LEN: usize = 64;

    /// One bulk endpoint as parsed from the configuration descriptor.
    #[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
    pub struct Endpoint {
        pub addr: u8,
        pub max_packet: u16,
        pub max_burst: u8,
    }

    /// Virtual and physical view of the DMA area.  Plain pointers, so it is
    /// `Copy` and the transport can hold it while the bounce slice is borrowed.
    #[derive(Clone, Copy, Debug)]
    pub struct Window {
        virt: *mut u8,
        phys: u64,
    }

    impl Window {
        /// Physical address of `p..p+len` if it lies in the bounce buffer.
        fn phys_of(&self, p: *const u8, len: usize) -> Option<u64> {
            let base = self.virt as usize;
            let addr = p as usize;
            let end = addr.checked_add(len)?;
            if addr < base || end > base + BOUNCE_LEN {
                return None;
            }
            Some(self.phys + (addr - base) as u64)
        }

        fn stage(&self) -> (*mut u8, u64) {
            // SAFETY: the area is BOUNCE_LEN + STAGE_LEN bytes long, so the
            // offset stays inside it.
            let virt = unsafe { self.virt.add(BOUNCE_LEN) };
            (virt, self.phys + BOUNCE_LEN as u64)
        }
    }

    /// Physically contiguous DMA memory: the bounce buffer plus a stage area.
    pub struct DmaArea {
        win: Window,
        handle: *mut c_void,
    }

    impl DmaArea {
        pub fn new() -> Option<Self> {
            let mut phys = 0u64;
            let mut handle: *mut c_void = ptr::null_mut();
            // SAFETY: out-pointers are valid locals.
            let virt =
                unsafe { ffi::usb_rust_dma_alloc(BOUNCE_LEN + STAGE_LEN, &mut phys, &mut handle) };
            if virt.is_null() {
                return None;
            }
            Some(Self {
                win: Window {
                    virt: virt.cast(),
                    phys,
                },
                handle,
            })
        }

        pub fn window(&self) -> Window {
            self.win
        }
    }

    impl Drop for DmaArea {
        fn drop(&mut self) {
            // SAFETY: handle came from usb_rust_dma_alloc and is freed once.
            unsafe { ffi::usb_rust_dma_free(self.handle) }
        }
    }

    impl Bounce for DmaArea {
        fn bounce(&mut self) -> &mut [u8] {
            // SAFETY: `virt` is valid for BOUNCE_LEN bytes for the life of
            // `self`, and `&mut self` gives exclusive access.
            unsafe { core::slice::from_raw_parts_mut(self.win.virt, BOUNCE_LEN) }
        }
    }

    /// Bulk pipes and control endpoint of one device.
    pub struct UsbBulk {
        dev: *mut c_void,
        in_pipe: *mut c_void,
        out_pipe: *mut c_void,
        win: Window,
    }

    impl UsbBulk {
        /// # Safety
        /// `dev` must be a live device handle from the USB core, and it must
        /// stay live for as long as the returned pipes exist.
        pub unsafe fn open(
            dev: *mut c_void,
            ins: Endpoint,
            outs: Endpoint,
            win: Window,
        ) -> Option<Self> {
            // SAFETY: the C shim checks the handle and returns NULL on failure.
            let in_pipe =
                unsafe { ffi::usb_rust_bulk_open(dev, ins.addr, ins.max_packet, ins.max_burst) };
            if in_pipe.is_null() {
                return None;
            }
            let out_pipe =
                unsafe { ffi::usb_rust_bulk_open(dev, outs.addr, outs.max_packet, outs.max_burst) };
            if out_pipe.is_null() {
                // SAFETY: in_pipe is non-null and not yet owned by anything.
                unsafe { ffi::usb_rust_bulk_close(in_pipe) };
                return None;
            }
            Some(Self {
                dev,
                in_pipe,
                out_pipe,
                win,
            })
        }

        /// Pointer and physical address for a transfer of `len` bytes at `p`.
        /// Data outside the bounce buffer goes through the stage area.
        fn locate(&self, p: *mut u8, len: usize) -> Result<(*mut u8, u64, bool), Xfer> {
            if let Some(phys) = self.win.phys_of(p, len) {
                return Ok((p, phys, false));
            }
            if len > STAGE_LEN {
                return Err(Xfer::Error);
            }
            let (virt, phys) = self.win.stage();
            Ok((virt, phys, true))
        }
    }

    fn map_rc(rc: i32) -> Result<(), Xfer> {
        match rc {
            0 => Ok(()),
            -2 => Err(Xfer::Stall),
            -3 => Err(Xfer::Timeout),
            -4 => Err(Xfer::Gone),
            _ => Err(Xfer::Error),
        }
    }

    impl Drop for UsbBulk {
        fn drop(&mut self) {
            // SAFETY: each pipe is closed once, here.
            unsafe {
                ffi::usb_rust_bulk_close(self.in_pipe);
                ffi::usb_rust_bulk_close(self.out_pipe);
            }
        }
    }

    impl BulkTransport for UsbBulk {
        fn bulk_out(&mut self, data: &[u8], timeout_ms: u32) -> Result<(), Xfer> {
            let len = data.len();
            let (p, phys, staged) = self.locate(data.as_ptr().cast_mut(), len)?;
            if staged {
                // SAFETY: the stage area holds STAGE_LEN bytes and len <= STAGE_LEN.
                unsafe { ptr::copy_nonoverlapping(data.as_ptr(), p, len) };
            }
            let mut actual = 0u32;
            let rc = unsafe {
                ffi::usb_rust_bulk_transfer(
                    self.out_pipe,
                    p.cast(),
                    phys,
                    u32::try_from(len).map_err(|_| Xfer::Error)?,
                    &mut actual,
                    timeout_ms,
                )
            };
            map_rc(rc)
        }

        fn bulk_in(&mut self, data: &mut [u8], timeout_ms: u32) -> Result<usize, Xfer> {
            let len = data.len();
            let (p, phys, staged) = self.locate(data.as_mut_ptr(), len)?;
            let mut actual = 0u32;
            let rc = unsafe {
                ffi::usb_rust_bulk_transfer(
                    self.in_pipe,
                    p.cast(),
                    phys,
                    u32::try_from(len).map_err(|_| Xfer::Error)?,
                    &mut actual,
                    timeout_ms,
                )
            };
            map_rc(rc)?;
            let got = (actual as usize).min(len);
            if staged {
                // SAFETY: `got` bytes were just received into the stage area.
                unsafe { ptr::copy_nonoverlapping(p, data.as_mut_ptr(), got) };
            }
            Ok(got)
        }

        fn clear_halt_in(&mut self) -> Result<(), Xfer> {
            map_rc(unsafe { ffi::usb_rust_bulk_clear_halt(self.in_pipe) })
        }

        fn clear_halt_out(&mut self) -> Result<(), Xfer> {
            map_rc(unsafe { ffi::usb_rust_bulk_clear_halt(self.out_pipe) })
        }

        fn control(
            &mut self,
            request_type: u8,
            request: u8,
            value: u16,
            index: u16,
            data: &mut [u8],
        ) -> Result<usize, Xfer> {
            let len = u16::try_from(data.len()).map_err(|_| Xfer::Error)?;
            let rc = unsafe {
                ffi::usb_rust_control_transfer(
                    self.dev,
                    request_type,
                    request,
                    value,
                    index,
                    data.as_mut_ptr().cast(),
                    len,
                )
            };
            if rc < 0 {
                Err(Xfer::Error)
            } else {
                Ok(rc as usize)
            }
        }
    }

    /// SET_CONFIGURATION, which must precede opening the bulk pipes.
    /// # Safety
    /// `dev` must be a live device handle from the USB core.
    pub unsafe fn set_configuration(dev: *mut c_void, value: u8) -> bool {
        let mut none = [0u8; 1];
        // SAFETY: zero-length OUT transfer; the buffer is a valid local.
        let rc = unsafe {
            ffi::usb_rust_control_transfer(
                dev,
                0x00,
                0x09,
                u16::from(value),
                0,
                none.as_mut_ptr().cast(),
                0,
            )
        };
        rc >= 0
    }
}
