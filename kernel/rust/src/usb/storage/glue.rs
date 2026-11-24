//! Kernel side of the mass-storage driver: the slot table, the probe and
//! removal entry points called from usb.c, the worker thread, the block bridge
//! callbacks, and the diag output.
//!
//! Concurrency:
//! - `rust_usb_storage_probe` and `rust_usb_storage_removed` never block.
//!   They move atomics only.  A probe owns its slot (CLAIMED) until it
//!   publishes PENDING with Release.
//! - The worker alone does bring-up and teardown.  It creates the slot mutex
//!   once, before the slot first goes ACTIVE, and never replaces it.
//! - The device sits in `lun`, which is written and taken only under the slot
//!   mutex.  Block callbacks check the state without the lock, then again under
//!   it, so a removed disk fails at once.
//! - The worker calls block_register and block_unregister without holding the
//!   mutex, because the block layer calls back into the read path while it
//!   scans partitions.

use core::cell::UnsafeCell;
use core::ffi::{c_int, c_void};
use core::fmt::{self, Write};
use core::ptr;
use core::sync::atomic::{
    AtomicBool, AtomicPtr, AtomicU16, AtomicU32, AtomicU64, AtomicU8, Ordering,
};

use super::bot::Outcome;
use super::device::hw::{self, DmaArea, Endpoint, UsbBulk};
use super::device::{self, Device, Snapshot};
use super::scsi::{ScsiError, Sense};
use crate::usb::desc;
use crate::usb::ffi::{self, BlockOps};
use crate::usb::kernel::{Clock, Diag, Handle, KMutex, KernelClock, KernelUsb};

const MAX_DEVICES: usize = 8;
const POLL_MS: u32 = 100;
const CONFIG_MAX: usize = 512;
const DEVICE_DESC_LEN: usize = 18;

const CLASS_MASS_STORAGE: u8 = 0x08;
const SUBCLASS_SCSI: u8 = 0x06;
const PROTOCOL_BOT: u8 = 0x50;
const TRANSFER_BULK: u8 = 2;

/// Slot states.  Moves: FREE -> CLAIMED (probe) -> PENDING (probe) -> ACTIVE
/// or FAILED (worker) -> REMOVING (removal) -> FREE (worker).
const FREE: u8 = 0;
const CLAIMED: u8 = 1;
const PENDING: u8 = 2;
const ACTIVE: u8 = 3;
const FAILED: u8 = 4;
const REMOVING: u8 = 5;

/// Top bit of the packed sense word: set when sense data is present.
const SENSE_PRESENT: u32 = 1 << 31;

type Lun = Device<UsbBulk, DmaArea>;

/// Bulk interface chosen from the configuration descriptor.
#[derive(Clone, Copy, Debug)]
struct Link {
    iface: u8,
    cfg_value: u8,
    ins: Endpoint,
    outs: Endpoint,
}

struct Slot {
    state: AtomicU8,
    usb: AtomicPtr<c_void>,
    vid: AtomicU16,
    pid: AtomicU16,
    /// Block handle from usb_rust_block_register, or null.
    block: AtomicPtr<c_void>,
    capacity: AtomicU64,
    sector_size: AtomicU32,
    read_only: AtomicBool,
    last_opcode: AtomicU8,
    last_sense: AtomicU32,
    resets: AtomicU32,
    stalls: AtomicU32,
    timeouts: AtomicU32,
    /// Written by the claiming probe before PENDING; read by the worker after.
    link: UnsafeCell<Option<Link>>,
    /// Created once by the worker before the first ACTIVE; never replaced.
    mutex: UnsafeCell<Option<KMutex>>,
    /// Written and taken only under `mutex`.
    lun: UnsafeCell<Option<Lun>>,
}

// SAFETY: every UnsafeCell field follows the protocol in the module comment:
// `link` is written before PENDING, `mutex` once before ACTIVE, and `lun`
// only under `mutex`.
unsafe impl Sync for Slot {}

impl Slot {
    const fn new() -> Self {
        Self {
            state: AtomicU8::new(FREE),
            usb: AtomicPtr::new(ptr::null_mut()),
            vid: AtomicU16::new(0),
            pid: AtomicU16::new(0),
            block: AtomicPtr::new(ptr::null_mut()),
            capacity: AtomicU64::new(0),
            sector_size: AtomicU32::new(0),
            read_only: AtomicBool::new(false),
            last_opcode: AtomicU8::new(0),
            last_sense: AtomicU32::new(0),
            resets: AtomicU32::new(0),
            stalls: AtomicU32::new(0),
            timeouts: AtomicU32::new(0),
            link: UnsafeCell::new(None),
            mutex: UnsafeCell::new(None),
            lun: UnsafeCell::new(None),
        }
    }

    fn publish(&self, s: &Snapshot) {
        self.capacity.store(s.capacity_sectors, Ordering::Relaxed);
        self.sector_size.store(s.sector_size, Ordering::Relaxed);
        self.read_only.store(s.read_only, Ordering::Relaxed);
        self.last_opcode
            .store(s.stats.last_opcode, Ordering::Relaxed);
        self.last_sense
            .store(pack_sense(s.stats.last_sense), Ordering::Relaxed);
        self.resets.store(s.stats.resets, Ordering::Relaxed);
        self.stalls.store(s.stats.stalls, Ordering::Relaxed);
        self.timeouts.store(s.stats.timeouts, Ordering::Relaxed);
    }

    /// Run `f` on the device, under the slot mutex, if the disk is ACTIVE.
    /// Anything else is `Gone`.
    fn io<R>(&self, f: impl FnOnce(&mut Lun) -> Result<R, ScsiError>) -> Result<R, ScsiError> {
        let gone = ScsiError::Transport(Outcome::Gone);
        if self.state.load(Ordering::Acquire) != ACTIVE {
            return Err(gone);
        }
        let m = mutex_of(self).ok_or(gone)?;
        let _guard = m.lock();
        if self.state.load(Ordering::Acquire) != ACTIVE {
            return Err(gone);
        }
        // SAFETY: `lun` is only written or taken under `mutex`, which this
        // thread holds.
        let dev = unsafe { (*self.lun.get()).as_mut() }.ok_or(gone)?;
        let r = f(dev);
        self.publish(&dev.snapshot());
        r
    }
}

static SLOTS: [Slot; MAX_DEVICES] = [const { Slot::new() }; MAX_DEVICES];
static STARTED: AtomicBool = AtomicBool::new(false);

static BLOCK_OPS: BlockOps = BlockOps {
    read: blk_read,
    write: blk_write,
    flush: blk_flush,
};

fn le16(b: &[u8], at: usize) -> u16 {
    b.get(at..at + 2)
        .and_then(|s| <[u8; 2]>::try_from(s).ok())
        .map_or(0, u16::from_le_bytes)
}

fn pack_sense(s: Option<Sense>) -> u32 {
    s.map_or(0, |s| {
        SENSE_PRESENT | (u32::from(s.key) << 16) | (u32::from(s.asc) << 8) | u32::from(s.ascq)
    })
}

fn mutex_of(slot: &Slot) -> Option<&KMutex> {
    // SAFETY: written once before the slot is ACTIVE and never replaced, so a
    // shared reference stays valid for the life of the slot.
    unsafe { (*slot.mutex.get()).as_ref() }
}

/// Find the first mass-storage interface (08/06/50, alternate 0) and its bulk
/// endpoints in configuration 0.
fn find_link(usb: &KernelUsb) -> Option<Link> {
    let mut raw = [0u8; CONFIG_MAX];
    let n = usb.config_descriptor(0, &mut raw).ok()?;
    let cfg = desc::Config::parse(raw.get(..n)?).ok()?;
    let mut iface: Option<u8> = None;
    let mut ins: Option<Endpoint> = None;
    let mut outs: Option<Endpoint> = None;
    for item in cfg.descriptors() {
        match item.ok()? {
            desc::Desc::Interface(i) => {
                if iface.is_some() {
                    break;
                }
                if i.class == CLASS_MASS_STORAGE
                    && i.subclass == SUBCLASS_SCSI
                    && i.protocol == PROTOCOL_BOT
                    && i.alt_setting == 0
                {
                    iface = Some(i.number);
                }
            }
            desc::Desc::Endpoint(e) if iface.is_some() && e.transfer_type() == TRANSFER_BULK => {
                let ep = Endpoint {
                    addr: e.address,
                    max_packet: e.max_packet,
                    max_burst: e.companion.map_or(0, |c| c.max_burst),
                };
                if e.is_in() {
                    ins = ins.or(Some(ep));
                } else {
                    outs = outs.or(Some(ep));
                }
            }
            _ => {}
        }
    }
    Some(Link {
        iface: iface?,
        cfg_value: cfg.config_value,
        ins: ins?,
        outs: outs?,
    })
}

fn claim() -> Option<&'static Slot> {
    SLOTS.iter().find(|s| {
        s.state
            .compare_exchange(FREE, CLAIMED, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
    })
}

/// Record a device with a mass-storage interface.  Returns false when the
/// device has none, or no slot is free.  Never blocks beyond the config
/// descriptor read that enumeration already does for other class drivers.
pub fn probe(dev: Handle) -> bool {
    if dev.is_null() {
        return false;
    }
    let usb = KernelUsb::new(dev);
    let Some(link) = find_link(&usb) else {
        return false;
    };
    let mut dd = [0u8; DEVICE_DESC_LEN];
    let _ = usb.device_descriptor(&mut dd);
    let (vid, pid) = (le16(&dd, 8), le16(&dd, 10));
    let Some(slot) = claim() else {
        crate::klog!("usb storage: no free slot for {:04x}:{:04x}\n", vid, pid);
        return false;
    };
    // SAFETY: the slot is CLAIMED by this thread alone.  The worker reads
    // `link` only after it sees PENDING.
    unsafe { *slot.link.get() = Some(link) };
    slot.usb.store(dev, Ordering::Relaxed);
    slot.vid.store(vid, Ordering::Relaxed);
    slot.pid.store(pid, Ordering::Relaxed);
    slot.state.store(PENDING, Ordering::Release);
    start_worker();
    crate::klog!("usb storage: claimed {:04x}:{:04x}\n", vid, pid);
    true
}

/// Flag a removed device's slot.  Never blocks.  I/O fails from now on, and
/// the worker tears the disk down on its next pass.
pub fn removed(dev: Handle) {
    if dev.is_null() {
        return;
    }
    for slot in SLOTS.iter() {
        let st = slot.state.load(Ordering::Acquire);
        if matches!(st, PENDING | ACTIVE | FAILED) && slot.usb.load(Ordering::Acquire) == dev {
            slot.state.store(REMOVING, Ordering::Release);
        }
    }
}

fn start_worker() {
    if STARTED.swap(true, Ordering::AcqRel) {
        return;
    }
    if crate::usb::kernel::spawn_kernel_thread(worker).is_err() {
        STARTED.store(false, Ordering::Release);
        crate::klog!("usb storage: worker thread spawn failed\n");
    }
}

extern "C" fn worker() {
    loop {
        for slot in SLOTS.iter() {
            match slot.state.load(Ordering::Acquire) {
                PENDING => bring_up(slot),
                REMOVING => tear_down(slot),
                _ => {}
            }
        }
        KernelClock.sleep_ms(POLL_MS);
    }
}

/// Move a PENDING slot to FAILED.  If removal already moved it to REMOVING,
/// the worker's next pass handles the teardown.
fn fail(slot: &Slot) {
    let _ = slot
        .state
        .compare_exchange(PENDING, FAILED, Ordering::AcqRel, Ordering::Acquire);
}

/// Open the pipes, start the LUN, and register the disk.  Worker only.
fn bring_up(slot: &Slot) {
    // SAFETY: the mutex is created here, once, while the slot is PENDING.
    // Nothing reads it before the slot is ACTIVE.
    unsafe {
        if (*slot.mutex.get()).is_none() {
            match KMutex::new() {
                Ok(m) => *slot.mutex.get() = Some(m),
                Err(_) => {
                    crate::klog!("usb storage: mutex allocation failed\n");
                    fail(slot);
                    return;
                }
            }
        }
    }
    // SAFETY: `link` was written before PENDING, and it is not written again.
    let Some(link) = (unsafe { *slot.link.get() }) else {
        fail(slot);
        return;
    };
    let usb = slot.usb.load(Ordering::Acquire);
    // SET_CONFIGURATION must come before the bulk endpoints are configured.
    // SAFETY: `usb` is the device handle this slot claimed; it stays live
    // until removal, which the worker handles.
    if !unsafe { hw::set_configuration(usb, link.cfg_value) } {
        crate::klog!("usb storage: SET_CONFIGURATION failed\n");
        fail(slot);
        return;
    }
    let Some(area) = DmaArea::new() else {
        crate::klog!("usb storage: DMA allocation failed\n");
        fail(slot);
        return;
    };
    let win = area.window();
    // SAFETY: as for set_configuration above.
    let Some(transport) = (unsafe { UsbBulk::open(usb, link.ins, link.outs, win) }) else {
        crate::klog!("usb storage: bulk pipe open failed\n");
        fail(slot);
        return;
    };
    let flags = device::quirk_flags(
        slot.vid.load(Ordering::Relaxed),
        slot.pid.load(Ordering::Relaxed),
    );
    let lun = match Device::start(transport, area, link.iface, flags, &KernelClock) {
        Ok(d) => d,
        Err(e) => {
            crate::klog!("usb storage: start failed: {:?}\n", e);
            fail(slot);
            return;
        }
    };
    let snap = lun.snapshot();
    let Some(m) = mutex_of(slot) else {
        fail(slot);
        return;
    };
    {
        let _guard = m.lock();
        // SAFETY: written under `mutex`; no I/O can reach the device before
        // the slot is ACTIVE.
        unsafe { *slot.lun.get() = Some(lun) };
    }
    slot.publish(&snap);
    // I/O must work during partition scan, so go ACTIVE before registering.
    // If removal already won the race, the next pass tears the disk down.
    if slot
        .state
        .compare_exchange(PENDING, ACTIVE, Ordering::AcqRel, Ordering::Acquire)
        .is_err()
    {
        return;
    }
    // SAFETY: `usb` is the live device handle, `ctx` is this static slot, and
    // BLOCK_OPS is static.
    let block = unsafe {
        ffi::usb_rust_block_register(
            usb,
            snap.sector_size,
            snap.capacity_sectors,
            snap.read_only,
            (slot as *const Slot).cast_mut().cast(),
            &BLOCK_OPS,
        )
    };
    if block.is_null() {
        crate::klog!("usb storage: block registration failed\n");
        slot.state.store(REMOVING, Ordering::Release);
        return;
    }
    slot.block.store(block, Ordering::Release);
    crate::klog!(
        "usb storage: {} sectors of {} bytes{}\n",
        snap.capacity_sectors,
        snap.sector_size,
        if snap.read_only { " (read-only)" } else { "" }
    );
}

/// Unregister the disk, then drop the LUN and its pipes and DMA.  Worker only.
fn tear_down(slot: &Slot) {
    let block = slot.block.swap(ptr::null_mut(), Ordering::AcqRel);
    if !block.is_null() {
        // SAFETY: the handle came from usb_rust_block_register and is
        // unregistered once.  No callback runs after this returns.
        unsafe { ffi::usb_rust_block_unregister(block) };
    }
    drop_lun(slot);
    slot.usb.store(ptr::null_mut(), Ordering::Release);
    slot.state.store(FREE, Ordering::Release);
}

fn drop_lun(slot: &Slot) {
    let Some(m) = mutex_of(slot) else {
        return;
    };
    let _guard = m.lock();
    // SAFETY: taken under `mutex`.
    let old = unsafe { (*slot.lun.get()).take() };
    drop(old);
}

fn slot_of(ctx: *mut c_void) -> Option<&'static Slot> {
    if ctx.is_null() {
        None
    } else {
        // SAFETY: `ctx` is the address of an entry in the static SLOTS.
        Some(unsafe { &*ctx.cast::<Slot>() })
    }
}

fn status(r: Result<(), ScsiError>) -> c_int {
    match r {
        Ok(()) => 0,
        Err(_) => -1,
    }
}

extern "C" fn blk_read(ctx: *mut c_void, lba: u64, count: u32, buf: *mut c_void) -> c_int {
    let Some(slot) = slot_of(ctx) else {
        return -1;
    };
    let r = slot.io(|dev| {
        let len = dev.span_bytes(count).ok_or(ScsiError::Range)?;
        if buf.is_null() {
            return Err(ScsiError::Range);
        }
        // SAFETY: the block layer passes `count` sectors that stay valid for
        // the duration of this call.
        let dst = unsafe { core::slice::from_raw_parts_mut(buf.cast::<u8>(), len) };
        dev.read(lba, count, dst)
    });
    if let Err(e) = &r {
        crate::klog!("usb storage: read failed lba={} count={} err={:?}\n", lba, count, e);
    }
    status(r)
}

extern "C" fn blk_write(ctx: *mut c_void, lba: u64, count: u32, buf: *const c_void) -> c_int {
    let Some(slot) = slot_of(ctx) else {
        return -1;
    };
    let r = slot.io(|dev| {
        let len = dev.span_bytes(count).ok_or(ScsiError::Range)?;
        if buf.is_null() {
            return Err(ScsiError::Range);
        }
        // SAFETY: as in blk_read; the block layer keeps `count` sectors valid.
        let src = unsafe { core::slice::from_raw_parts(buf.cast::<u8>(), len) };
        dev.write(lba, count, src)
    });
    if let Err(e) = &r {
        crate::klog!("usb storage: write failed lba={} count={} err={:?}\n", lba, count, e);
    }
    status(r)
}

extern "C" fn blk_flush(ctx: *mut c_void) -> c_int {
    let Some(slot) = slot_of(ctx) else {
        return -1;
    };
    let r = slot.io(|dev| dev.flush());
    if let Err(e) = &r {
        crate::klog!("usb storage: flush failed err={:?}\n", e);
    }
    status(r)
}

struct SenseText(u32);

impl fmt::Display for SenseText {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.0 & SENSE_PRESENT == 0 {
            return f.write_str("none");
        }
        write!(
            f,
            "{:x}/{:02x}/{:02x}",
            (self.0 >> 16) & 0xF,
            (self.0 >> 8) & 0xFF,
            self.0 & 0xFF
        )
    }
}

fn state_name(state: u8) -> &'static str {
    match state {
        CLAIMED => "claimed",
        PENDING => "pending",
        ACTIVE => "active",
        FAILED => "failed",
        REMOVING => "removing",
        _ => "free",
    }
}

/// Per-device lines for /proc/usb.
pub fn write_diag(raw: Handle) {
    let Some(mut out) = Diag::new(raw) else {
        return;
    };
    for (i, slot) in SLOTS.iter().enumerate() {
        let state = slot.state.load(Ordering::Acquire);
        if state == FREE {
            continue;
        }
        let _ = writeln!(
            out,
            "  storage slot {}: state={} usb={:p} vid_pid={:04x}:{:04x} capacity_sectors={} sector_size={} read_only={} last_opcode={:#04x} last_sense={} resets={} stalls={} timeouts={}",
            i,
            state_name(state),
            slot.usb.load(Ordering::Relaxed),
            slot.vid.load(Ordering::Relaxed),
            slot.pid.load(Ordering::Relaxed),
            slot.capacity.load(Ordering::Relaxed),
            slot.sector_size.load(Ordering::Relaxed),
            slot.read_only.load(Ordering::Relaxed),
            slot.last_opcode.load(Ordering::Relaxed),
            SenseText(slot.last_sense.load(Ordering::Relaxed)),
            slot.resets.load(Ordering::Relaxed),
            slot.stalls.load(Ordering::Relaxed),
            slot.timeouts.load(Ordering::Relaxed),
        );
    }
}

// --- C entry points (declared in usb_rust_glue.h / usb.c) ------------------

#[no_mangle]
pub extern "C" fn rust_usb_storage_probe(dev: *mut c_void) -> bool {
    probe(dev)
}

#[no_mangle]
pub extern "C" fn rust_usb_storage_removed(dev: *mut c_void) {
    removed(dev);
}
