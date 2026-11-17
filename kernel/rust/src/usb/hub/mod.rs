//! USB hub class driver (Phase 5 of docs/usb-storage-plan.md).
//!
//! [`Hub`] is generic over the transport, the host callbacks and the clock,
//! so the protocol runs under host tests against a scripted mock hub.  The
//! kernel instance is driven by one worker thread that services a static
//! table of [`MAX_HUBS`] slots.
//!
//! Concurrency: probe and removal run from IRQ or timer context, so they never
//! block.  They move the slot's atomic state and cancel the interrupt pipe.
//! The worker (the only owner of each `Hub`) frees the hub on its next pass.

pub mod port;
#[cfg(test)]
mod tests;

use core::ffi::c_void;
use core::fmt::Write;
use core::ptr;
use core::sync::atomic::{AtomicBool, AtomicPtr, AtomicU32, AtomicU8, Ordering};

use super::desc::{self, Desc};
use super::kernel::{
    self, Clock, ControlSetup, DmaBuf, Handle, KernelClock, KernelUsb, UsbError, UsbResult,
    UsbTransport, WorkerCell, PAGE_SIZE,
};
use port::{Action, PortMachine, PortSample, Phase, Speed, CLEAR_SELECTOR, POLL_MS};

/// Hub slots (also the number of hubs the worker can service at once).
pub const MAX_HUBS: usize = 16;
/// Downstream ports per hub.  Matches the core's USB_MAX_PORTS_PER_HUB.
pub const MAX_PORTS: usize = 15;

const CLASS_HUB: u8 = 0x09;
const PROTOCOL_MULTI_TT: u8 = 2;
const TRANSFER_INTERRUPT: u8 = 3;
const CONFIG_MAX: usize = 256;

const REQ_GET_STATUS: u8 = 0;
const REQ_CLEAR_FEATURE: u8 = 1;
const REQ_SET_FEATURE: u8 = 3;
const REQ_GET_DESCRIPTOR: u8 = 6;
const REQ_SET_CONFIGURATION: u8 = 9;
const REQ_SET_INTERFACE: u8 = 11;
const REQ_SET_HUB_DEPTH: u8 = 12;

const RT_DEV_STD_OUT: u8 = 0x00;
const RT_DEV_STD_IN: u8 = 0x80;
const RT_IFACE_STD_OUT: u8 = 0x01;
const RT_CLASS_DEV_OUT: u8 = 0x20;
const RT_CLASS_DEV_IN: u8 = 0xA0;
const RT_CLASS_OTHER_OUT: u8 = 0x23;
const RT_CLASS_OTHER_IN: u8 = 0xA3;

const FEATURE_PORT_RESET: u16 = 4;
const FEATURE_PORT_POWER: u16 = 8;

const SPEED_SUPER: i32 = 4;
const SPEED_SUPER_PLUS: i32 = 5;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HubError {
    Usb(UsbError),
    BadDescriptor,
    BadPortCount(u8),
    ConfigFailed,
}

impl From<UsbError> for HubError {
    fn from(e: UsbError) -> Self {
        HubError::Usb(e)
    }
}

/// Callbacks into the USB core and facts about the hub's own device.
pub trait HubHost {
    /// Copy the raw 18-byte device descriptor into `out`.  Returns bytes written.
    fn device_descriptor(&self, out: &mut [u8]) -> usize;
    fn is_super_speed(&self) -> bool;
    /// Hub tier below the root (0 for a hub on a root port).
    fn depth(&self) -> u8;
    /// Program the Slot Context (`usb_hub_configure`).
    fn configure(&mut self, nports: u8, multi_tt: bool, think: u8) -> bool;
    /// `usb_hub_child_discovered`: `port` is 1-based.
    fn child_discovered(&mut self, port: u8, speed: Speed) -> bool;
    /// `usb_hub_child_removed`: `port` is 1-based.
    fn child_removed(&mut self, port: u8);
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct HubStats {
    pub reports: u32,
    pub report_errors: u32,
    pub control_errors: u32,
    pub attach_refused: u32,
}

pub struct Hub<T: UsbTransport, H: HubHost, C: Clock> {
    usb: T,
    host: H,
    clock: C,
    nports: u8,
    ss: bool,
    ports: [PortMachine; MAX_PORTS],
    /// Interrupt IN buffer.  Must outlive the pipe, so it is dropped last.
    _report: DmaBuf,
    pub stats: HubStats,
}

fn le16(b: &[u8], i: usize) -> Option<u16> {
    match b.get(i..i + 2) {
        Some(&[lo, hi]) => Some(u16::from_le_bytes([lo, hi])),
        _ => None,
    }
}

fn ctl_in<T: UsbTransport>(
    usb: &mut T,
    request_type: u8,
    request: u8,
    value: u16,
    index: u16,
    buf: &mut [u8],
) -> UsbResult<usize> {
    let length = u16::try_from(buf.len()).map_err(|_| UsbError::Invalid)?;
    let setup = ControlSetup {
        request_type,
        request,
        value,
        index,
        length,
    };
    usb.control(setup, buf)
}

fn ctl_out<T: UsbTransport>(
    usb: &mut T,
    request_type: u8,
    request: u8,
    value: u16,
    index: u16,
) -> UsbResult<()> {
    let setup = ControlSetup {
        request_type,
        request,
        value,
        index,
        length: 0,
    };
    usb.control(setup, &mut []).map(|_| ())
}

impl<T: UsbTransport, H: HubHost, C: Clock> Hub<T, H, C> {
    /// Probe and power up a hub.  The interrupt pipe is open on return, but
    /// no port has been scanned yet; call [`Hub::coldplug`] for that.
    ///
    /// Every fallible step runs before the pipe is opened, so an error leaves
    /// no endpoint behind.
    pub fn start(mut usb: T, mut host: H, clock: C) -> Result<Self, HubError> {
        let mut dd = [0u8; 18];
        if host.device_descriptor(&mut dd) < dd.len() {
            return Err(HubError::BadDescriptor);
        }
        let ss = host.is_super_speed();
        let multi_tt_hw = !ss && dd.get(6).copied() == Some(PROTOCOL_MULTI_TT);

        // Configuration: the 9-byte header gives wTotalLength, then the set.
        let mut head = [0u8; 9];
        ctl_in(
            &mut usb,
            RT_DEV_STD_IN,
            REQ_GET_DESCRIPTOR,
            u16::from(desc::DT_CONFIG) << 8,
            0,
            &mut head,
        )?;
        let total = usize::from(le16(&head, 2).ok_or(HubError::BadDescriptor)?);
        let mut cfg_buf = [0u8; CONFIG_MAX];
        let cfg_len = {
            let whole = cfg_buf.get_mut(..total).ok_or(HubError::BadDescriptor)?;
            ctl_in(
                &mut usb,
                RT_DEV_STD_IN,
                REQ_GET_DESCRIPTOR,
                u16::from(desc::DT_CONFIG) << 8,
                0,
                whole,
            )?
        };
        let config = desc::Config::parse(cfg_buf.get(..cfg_len).ok_or(HubError::BadDescriptor)?)
            .map_err(|_| HubError::BadDescriptor)?;
        let config_value = config.config_value;

        // Hub interface (alt 0), and whether a multi-TT alt 1 exists.
        let mut iface: Option<u8> = None;
        let mut has_alt1 = false;
        for d in config.descriptors() {
            if let Ok(Desc::Interface(i)) = d {
                if i.class != CLASS_HUB {
                    continue;
                }
                match iface {
                    None if i.alt_setting == 0 => iface = Some(i.number),
                    Some(n) if n == i.number && i.alt_setting == 1 => has_alt1 = true,
                    _ => {}
                }
            }
        }
        let iface = iface.ok_or(HubError::BadDescriptor)?;
        let alt = u8::from(multi_tt_hw && has_alt1);
        let multi_tt = alt == 1;

        // Status-change endpoint of the selected alternate setting.
        let mut in_iface = false;
        let mut intr: Option<(u8, u16, u8)> = None;
        for d in config.descriptors() {
            match d {
                Ok(Desc::Interface(i)) => in_iface = i.number == iface && i.alt_setting == alt,
                Ok(Desc::Endpoint(e))
                    if in_iface
                        && intr.is_none()
                        && e.is_in()
                        && e.transfer_type() == TRANSFER_INTERRUPT =>
                {
                    intr = Some((e.address, e.max_packet, e.interval));
                }
                _ => {}
            }
        }
        let (ep_addr, ep_mps, ep_interval) = intr.ok_or(HubError::BadDescriptor)?;

        ctl_out(
            &mut usb,
            RT_DEV_STD_OUT,
            REQ_SET_CONFIGURATION,
            u16::from(config_value),
            0,
        )?;
        if alt != 0 {
            ctl_out(
                &mut usb,
                RT_IFACE_STD_OUT,
                REQ_SET_INTERFACE,
                u16::from(alt),
                u16::from(iface),
            )?;
        }

        // Hub descriptor: 0x29 for USB 2.0 hubs, 0x2A for SuperSpeed.
        let mut hd = [0u8; 32];
        let desc_type = if ss { desc::DT_SS_HUB } else { desc::DT_HUB };
        let hd_len = ctl_in(
            &mut usb,
            RT_CLASS_DEV_IN,
            REQ_GET_DESCRIPTOR,
            u16::from(desc_type) << 8,
            0,
            &mut hd,
        )?;
        let raw = hd.get(..hd_len).ok_or(HubError::BadDescriptor)?;
        let (nports, characteristics, pwr_on_2_pwr_good) = if ss {
            let h = desc::parse_hub_ss(raw).map_err(|_| HubError::BadDescriptor)?;
            (h.num_ports, h.characteristics, h.pwr_on_2_pwr_good)
        } else {
            let h = desc::parse_hub20(raw).map_err(|_| HubError::BadDescriptor)?;
            (h.num_ports, h.characteristics, h.pwr_on_2_pwr_good)
        };
        if nports == 0 || usize::from(nports) > MAX_PORTS {
            return Err(HubError::BadPortCount(nports));
        }
        // TT think time lives in wHubCharacteristics bits 6:5 (USB 2.0 only).
        let think = if ss {
            0
        } else {
            ((characteristics >> 5) & 3) as u8
        };
        let pwr_ms = u32::from(pwr_on_2_pwr_good) * 2;

        if ss {
            ctl_out(
                &mut usb,
                RT_CLASS_DEV_OUT,
                REQ_SET_HUB_DEPTH,
                u16::from(host.depth()),
                0,
            )?;
        }
        if !host.configure(nports, multi_tt, think) {
            return Err(HubError::ConfigFailed);
        }

        let report = DmaBuf::new(usize::from(ep_mps).clamp(4, PAGE_SIZE))?;
        usb.interrupt_open(ep_addr, ep_mps, ep_interval, &report)?;

        let mut hub = Self {
            usb,
            host,
            clock,
            nports,
            ss,
            ports: [PortMachine::new(ss); MAX_PORTS],
            _report: report,
            stats: HubStats::default(),
        };
        for p in 1..=nports {
            // A failed power request is counted; the port then stays dark and
            // the hub keeps running with the others.
            let _ = hub.set_feature(FEATURE_PORT_POWER, p);
        }
        hub.clock.sleep_ms(pwr_ms);
        Ok(hub)
    }

    pub fn nports(&self) -> u8 {
        self.nports
    }

    pub fn is_super_speed(&self) -> bool {
        self.ss
    }

    /// Opaque interrupt pipe handle, for cancellation from another context.
    pub fn pipe_handle(&self) -> Handle {
        self.usb.pipe_handle()
    }

    pub fn ports(&self) -> &[PortMachine] {
        &self.ports
    }

    /// Sample every port once.  Used after power-on to find devices that were
    /// present before the hub was powered and so raised no change report.
    pub fn coldplug(&mut self) {
        for p in 1..=self.nports {
            self.service(p);
        }
    }

    /// One pass of the event loop: act on a completed interrupt report (and
    /// re-arm it), then sample any port whose timed phase is due.
    pub fn tick(&mut self) {
        let now = self.clock.now_ms();
        let mut report = [0u8; 8];
        if self.usb.interrupt_report(&mut report).is_some() {
            self.stats.reports += 1;
            let byte = |i: usize| u32::from(report.get(i).copied().unwrap_or(0));
            // Bit 0 is the hub itself (over-current on the hub); ports are 1..n.
            let bitmap = byte(0) | (byte(1) << 8);
            for p in 1..=self.nports {
                if bitmap & (1u32 << u32::from(p)) != 0 {
                    self.service(p);
                }
            }
            if self.usb.interrupt_resubmit().is_err() {
                self.stats.report_errors += 1;
            }
        }
        for p in 1..=self.nports {
            if self.port(p).is_some_and(|m| m.poll_due(now)) {
                self.service(p);
            }
        }
    }

    fn port(&self, p: u8) -> Option<&PortMachine> {
        usize::from(p).checked_sub(1).and_then(|i| self.ports.get(i))
    }

    fn port_mut(&mut self, p: u8) -> Option<&mut PortMachine> {
        usize::from(p)
            .checked_sub(1)
            .and_then(|i| self.ports.get_mut(i))
    }

    /// Sample one port, step its machine and carry out the resulting action.
    fn service(&mut self, p: u8) {
        // A power cycle is followed by one more sample of the same port.
        for _ in 0..2 {
            let Some(sample) = self.sample(p) else {
                return;
            };
            let now = self.clock.now_ms();
            let Some(action) = self.port_mut(p).map(|m| m.step(now, sample)) else {
                return;
            };
            match action {
                Action::None | Action::DebounceFailed | Action::ResetFailed => return,
                Action::IssueReset => {
                    let _ = self.set_feature(FEATURE_PORT_RESET, p);
                    return;
                }
                Action::PowerCycle => {
                    let _ = self.set_feature(FEATURE_PORT_POWER, p);
                }
                Action::Attached(speed) => {
                    if !self.host.child_discovered(p, speed) {
                        self.stats.attach_refused += 1;
                    }
                    return;
                }
                Action::Detached => {
                    self.host.child_removed(p);
                    return;
                }
            }
        }
    }

    /// GET_PORT_STATUS, then acknowledge every change bit seen.  The change
    /// bits are cleared after decoding, because RESET completion is one of them.
    fn sample(&mut self, p: u8) -> Option<PortSample> {
        let (status, change) = self.read_status(p)?;
        let sample = port::decode(status, change, self.ss);
        if let Some(m) = self.port_mut(p) {
            m.raw_status = status;
            m.raw_change = change;
        }
        // USB 2.0 hubs only define change bits 0..=4.
        let bits = if self.ss { CLEAR_SELECTOR.len() } else { 5 };
        for (bit, selector) in CLEAR_SELECTOR.iter().enumerate().take(bits) {
            if change & (1u16 << bit) != 0 {
                self.clear_feature(*selector, p);
            }
        }
        Some(sample)
    }

    fn read_status(&mut self, p: u8) -> Option<(u16, u16)> {
        let mut buf = [0u8; 4];
        let res = ctl_in(
            &mut self.usb,
            RT_CLASS_OTHER_IN,
            REQ_GET_STATUS,
            0,
            u16::from(p),
            &mut buf,
        );
        if res.is_err() {
            self.stats.control_errors += 1;
            return None;
        }
        Some((le16(&buf, 0)?, le16(&buf, 2)?))
    }

    fn set_feature(&mut self, feature: u16, p: u8) -> bool {
        self.port_request(REQ_SET_FEATURE, feature, p)
    }

    fn clear_feature(&mut self, feature: u16, p: u8) -> bool {
        self.port_request(REQ_CLEAR_FEATURE, feature, p)
    }

    fn port_request(&mut self, request: u8, feature: u16, p: u8) -> bool {
        let ok = ctl_out(
            &mut self.usb,
            RT_CLASS_OTHER_OUT,
            request,
            feature,
            u16::from(p),
        )
        .is_ok();
        if !ok {
            self.stats.control_errors += 1;
        }
        ok
    }
}

// --- Kernel instance -----------------------------------------------------

/// [`HubHost`] for a real device, backed by the C core through the shim.
pub struct CoreHost {
    dev: Handle,
}

impl HubHost for CoreHost {
    fn device_descriptor(&self, out: &mut [u8]) -> usize {
        KernelUsb::new(self.dev).device_descriptor(out)
    }

    fn is_super_speed(&self) -> bool {
        matches!(
            kernel::device_speed(self.dev),
            SPEED_SUPER | SPEED_SUPER_PLUS
        )
    }

    fn depth(&self) -> u8 {
        kernel::device_depth(self.dev)
    }

    fn configure(&mut self, nports: u8, multi_tt: bool, think: u8) -> bool {
        kernel::hub_configure(self.dev, nports, multi_tt, think)
    }

    fn child_discovered(&mut self, port: u8, speed: Speed) -> bool {
        kernel::hub_child_discovered(self.dev, port, speed.c_value())
    }

    fn child_removed(&mut self, port: u8) {
        kernel::hub_child_removed(self.dev, port);
    }
}

type HubRun = Hub<KernelUsb, CoreHost, KernelClock>;

const FREE: u8 = 0;
/// Probe is filling in the slot.  Not yet visible to the worker.
const CLAIMED: u8 = 1;
const PENDING: u8 = 2;
const ACTIVE: u8 = 3;
const REMOVING: u8 = 4;

fn state_name(state: u8) -> &'static str {
    match state {
        FREE => "free",
        CLAIMED => "claimed",
        PENDING => "pending",
        ACTIVE => "active",
        REMOVING => "removing",
        _ => "?",
    }
}

fn phase_code(phase: Phase) -> u8 {
    match phase {
        Phase::Empty => 0,
        Phase::Debounce { .. } => 1,
        Phase::Reset { .. } => 2,
        Phase::Recovery { .. } => 3,
        Phase::Attached(_) => 4,
        Phase::Faulted { .. } => 5,
    }
}

fn phase_name(code: u8) -> &'static str {
    match code {
        0 => "empty",
        1 => "debounce",
        2 => "reset",
        3 => "recovery",
        4 => "attached",
        5 => "faulted",
        _ => "?",
    }
}

/// Per-port diagnostics, published by the worker and read by /proc/usb.
struct PortDiag {
    phase: AtomicU8,
    /// raw status << 16 | raw change.
    raw: AtomicU32,
    resets: AtomicU32,
    reset_failures: AtomicU32,
    debounce_failures: AtomicU32,
    over_current: AtomicU32,
    attached: AtomicU32,
}

impl PortDiag {
    const fn new() -> Self {
        Self {
            phase: AtomicU8::new(0),
            raw: AtomicU32::new(0),
            resets: AtomicU32::new(0),
            reset_failures: AtomicU32::new(0),
            debounce_failures: AtomicU32::new(0),
            over_current: AtomicU32::new(0),
            attached: AtomicU32::new(0),
        }
    }

    fn publish(&self, m: &PortMachine) {
        let c = m.counters;
        self.phase
            .store(phase_code(m.phase()), Ordering::Relaxed);
        self.raw.store(
            (u32::from(m.raw_status) << 16) | u32::from(m.raw_change),
            Ordering::Relaxed,
        );
        self.resets.store(c.resets, Ordering::Relaxed);
        self.reset_failures.store(c.reset_failures, Ordering::Relaxed);
        self.debounce_failures
            .store(c.debounce_failures, Ordering::Relaxed);
        self.over_current.store(c.over_current, Ordering::Relaxed);
        self.attached.store(c.attached, Ordering::Relaxed);
    }
}

struct Slot {
    state: AtomicU8,
    dev: AtomicPtr<c_void>,
    pipe: AtomicPtr<c_void>,
    nports: AtomicU8,
    ss: AtomicBool,
    reports: AtomicU32,
    control_errors: AtomicU32,
    attach_refused: AtomicU32,
    ports: [PortDiag; MAX_PORTS],
}

impl Slot {
    const fn new() -> Self {
        Self {
            state: AtomicU8::new(FREE),
            dev: AtomicPtr::new(ptr::null_mut()),
            pipe: AtomicPtr::new(ptr::null_mut()),
            nports: AtomicU8::new(0),
            ss: AtomicBool::new(false),
            reports: AtomicU32::new(0),
            control_errors: AtomicU32::new(0),
            attach_refused: AtomicU32::new(0),
            ports: [const { PortDiag::new() }; MAX_PORTS],
        }
    }

    fn publish(&self, hub: &HubRun) {
        self.reports.store(hub.stats.reports, Ordering::Relaxed);
        self.control_errors
            .store(hub.stats.control_errors, Ordering::Relaxed);
        self.attach_refused
            .store(hub.stats.attach_refused, Ordering::Relaxed);
        for (pd, m) in self.ports.iter().zip(hub.ports()) {
            pd.publish(m);
        }
    }
}

static SLOTS: [Slot; MAX_HUBS] = [const { Slot::new() }; MAX_HUBS];

/// Hub instances.  Owned by the worker thread alone.
static RUNS: WorkerCell<[Option<HubRun>; MAX_HUBS]> = WorkerCell::new([const { None }; MAX_HUBS]);

static WORKER_STARTED: AtomicBool = AtomicBool::new(false);

/// Start the worker thread once.  Called from usb_init, before any hub can be
/// discovered.
pub fn start_worker() {
    if WORKER_STARTED.swap(true, Ordering::AcqRel) {
        return;
    }
    if kernel::spawn_kernel_thread(hub_worker).is_err() {
        WORKER_STARTED.store(false, Ordering::Release);
        crate::klog!("usb hub: worker thread spawn failed\n");
    }
}

/// Record a newly enumerated hub.  Never blocks.
pub fn probe(dev: Handle) {
    if dev.is_null() {
        return;
    }
    for slot in SLOTS.iter() {
        if slot
            .state
            .compare_exchange(FREE, CLAIMED, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
        {
            slot.dev.store(dev, Ordering::Release);
            slot.pipe.store(ptr::null_mut(), Ordering::Release);
            slot.nports.store(0, Ordering::Relaxed);
            slot.state.store(PENDING, Ordering::Release);
            return;
        }
    }
    crate::klog!("usb hub: no free hub slot\n");
}

/// Stop servicing a hub that is being removed.  Never blocks: it flags the
/// slot and cancels the interrupt pipe.  The worker drops the hub later.  The
/// core calls this before tearing down the device.
pub fn removed(dev: Handle) {
    for slot in SLOTS.iter() {
        let state = slot.state.load(Ordering::Acquire);
        let live = state == PENDING || state == ACTIVE;
        if live && slot.dev.load(Ordering::Acquire) == dev {
            slot.state.store(REMOVING, Ordering::Release);
            let pipe = slot.pipe.swap(ptr::null_mut(), Ordering::AcqRel);
            kernel::cancel_pipe(pipe);
        }
    }
}

/// Free a slot.  Cancels the pipe if removal has not already done so.
fn retire(slot: &Slot, run: &mut Option<HubRun>) {
    let pipe = slot.pipe.swap(ptr::null_mut(), Ordering::AcqRel);
    kernel::cancel_pipe(pipe);
    drop(run.take());
    slot.nports.store(0, Ordering::Relaxed);
    slot.dev.store(ptr::null_mut(), Ordering::Release);
    slot.state.store(FREE, Ordering::Release);
}

fn service_slot(slot: &Slot, run: &mut Option<HubRun>) {
    match slot.state.load(Ordering::Acquire) {
        PENDING => {
            let dev = slot.dev.load(Ordering::Acquire);
            match Hub::start(KernelUsb::new(dev), CoreHost { dev }, KernelClock) {
                Ok(hub) => {
                    slot.nports.store(hub.nports(), Ordering::Relaxed);
                    slot.ss.store(hub.is_super_speed(), Ordering::Relaxed);
                    // Publish the pipe before ACTIVE, so a removal that lands
                    // now can still cancel it.
                    slot.pipe.store(hub.pipe_handle(), Ordering::Release);
                    *run = Some(hub);
                    hub_coldplug(run);
                    // If removal already flagged the slot, the CAS fails and
                    // the next pass retires it.
                    let _ = slot.state.compare_exchange(
                        PENDING,
                        ACTIVE,
                        Ordering::AcqRel,
                        Ordering::Acquire,
                    );
                }
                Err(e) => {
                    crate::klog!("usb hub: probe failed: {:?}\n", e);
                    slot.dev.store(ptr::null_mut(), Ordering::Release);
                    let _ = slot.state.compare_exchange(
                        PENDING,
                        FREE,
                        Ordering::AcqRel,
                        Ordering::Acquire,
                    );
                }
            }
        }
        ACTIVE => {
            if let Some(hub) = run.as_mut() {
                hub.tick();
                slot.publish(hub);
            }
        }
        REMOVING => retire(slot, run),
        _ => {}
    }
}

fn hub_coldplug(run: &mut Option<HubRun>) {
    if let Some(hub) = run.as_mut() {
        hub.coldplug();
    }
}

#[cfg_attr(test, allow(dead_code))]
extern "C" fn hub_worker() {
    loop {
        RUNS.with(|runs| {
            for (slot, run) in SLOTS.iter().zip(runs.iter_mut()) {
                service_slot(slot, run);
            }
        });
        KernelClock.sleep_ms(POLL_MS as u32);
    }
}

/// Human-readable hub state for /proc/usb.
pub fn write_diag(raw: Handle) {
    let Some(mut out) = kernel::Diag::new(raw) else {
        return;
    };
    for (i, slot) in SLOTS.iter().enumerate() {
        let state = slot.state.load(Ordering::Acquire);
        if state == FREE {
            continue;
        }
        let n = slot.nports.load(Ordering::Relaxed);
        let _ = writeln!(
            out,
            "  hub slot {}: state={} dev={:p} ports={} ss={} reports={} control_errors={} attach_refused={}",
            i,
            state_name(state),
            slot.dev.load(Ordering::Acquire),
            n,
            slot.ss.load(Ordering::Relaxed),
            slot.reports.load(Ordering::Relaxed),
            slot.control_errors.load(Ordering::Relaxed),
            slot.attach_refused.load(Ordering::Relaxed),
        );
        for (p, pd) in slot.ports.iter().enumerate().take(usize::from(n)) {
            let raw = pd.raw.load(Ordering::Relaxed);
            let _ = writeln!(
                out,
                "    port {}: {} status={:#06x} change={:#06x} resets={} reset_failures={} debounce_failures={} over_current={} attached={}",
                p + 1,
                phase_name(pd.phase.load(Ordering::Relaxed)),
                raw >> 16,
                raw & 0xffff,
                pd.resets.load(Ordering::Relaxed),
                pd.reset_failures.load(Ordering::Relaxed),
                pd.debounce_failures.load(Ordering::Relaxed),
                pd.over_current.load(Ordering::Relaxed),
                pd.attached.load(Ordering::Relaxed),
            );
        }
    }
}

// --- C entry points (declared in usb_rust_glue.h) -------------------------

#[no_mangle]
pub extern "C" fn rust_usb_hub_init() {
    start_worker();
}

#[no_mangle]
pub extern "C" fn rust_usb_hub_probe(dev: *mut c_void) {
    probe(dev);
}

#[no_mangle]
pub extern "C" fn rust_usb_hub_removed(dev: *mut c_void) {
    removed(dev);
}

#[no_mangle]
pub extern "C" fn rust_usb_diag(diag: *mut c_void) {
    write_diag(diag);
    crate::usb::storage::write_diag(diag);
}
