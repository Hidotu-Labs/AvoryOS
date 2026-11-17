//! Host tests: `Hub` against a scripted mock hub.  The mock is a small
//! timeline (events at fixed times, reset completion after `reset_ms`) that
//! the clock advances, so the driver's sleeps and polls drive the simulation.

#![allow(
    clippy::unwrap_used,
    clippy::expect_used,
    clippy::indexing_slicing,
    clippy::panic
)]

use std::cell::RefCell;
use std::rc::Rc;
use std::vec::Vec;

use super::port::{
    Phase, Speed, CH_BH_RESET, CH_CONFIG_ERROR, CH_CONNECTION, CH_LINK_STATE, CH_OVER_CURRENT,
    CH_RESET,
};
use super::*;
use crate::usb::kernel::{ControlSetup, UsbError, UsbResult};

#[derive(Clone, Copy, Debug)]
enum Ev {
    Plug(bool),
    OverCurrent(bool),
}

#[derive(Clone, Copy, Debug)]
struct SimPort {
    present: bool,
    powered: bool,
    oc: bool,
    enabled: bool,
    reset_at: Option<u64>,
    /// Number of upcoming resets that never complete.
    hang: u8,
    high: bool,
    change: u16,
}

impl SimPort {
    const fn new() -> Self {
        Self {
            present: false,
            powered: false,
            oc: false,
            enabled: false,
            reset_at: None,
            hang: 0,
            high: false,
            change: 0,
        }
    }
    fn connected(&self) -> bool {
        self.present && self.powered && !self.oc
    }
}

#[derive(Clone, Copy, Debug)]
struct Req {
    at: u64,
    request_type: u8,
    request: u8,
    value: u16,
    index: u16,
}

struct Sim {
    now: u64,
    ss: bool,
    reset_ms: u64,
    ports: Vec<SimPort>,
    events: Vec<(u64, usize, Ev)>,
    armed: bool,
    open: bool,
    log: Vec<Req>,
    config: Vec<u8>,
    hub_desc: Vec<u8>,
    dev_desc: [u8; 18],
    configured: Option<(u8, bool, u8)>,
    depth: Option<u16>,
    attached: Vec<(u8, Speed, u64)>,
    removed: Vec<(u8, u64)>,
    refuse_attach: bool,
}

fn config_blob(multi_tt: bool) -> Vec<u8> {
    let iface0 = [9u8, 4, 0, 0, 1, 9, 0, 0, 0];
    let iface1 = [9u8, 4, 0, 1, 1, 9, 0, 0, 0];
    let ep = [7u8, 5, 0x81, 0x03, 1, 0, 12];
    let mut body: Vec<u8> = Vec::new();
    body.extend_from_slice(&iface0);
    body.extend_from_slice(&ep);
    if multi_tt {
        body.extend_from_slice(&iface1);
        body.extend_from_slice(&ep);
    }
    let total = 9 + body.len();
    let mut v = std::vec![9u8, 2, total as u8, 0, 1, 1, 0, 0xE0, 0];
    v.extend_from_slice(&body);
    v
}

fn hub20_desc(n: u8) -> Vec<u8> {
    let rm = usize::from(n) / 8 + 1;
    let mut v = std::vec![(7 + 2 * rm) as u8, 0x29, n, 0, 0, 10, 0];
    v.resize(v.len() + rm, 0);
    v.resize(v.len() + rm, 0xFF);
    v
}

fn hub_ss_desc(n: u8) -> Vec<u8> {
    std::vec![12, 0x2A, n, 0, 0, 10, 0, 0, 0, 0, 0, 0]
}

impl Sim {
    fn new(nports: u8, ss: bool, multi_tt: bool) -> Self {
        let mut dev_desc = [0u8; 18];
        dev_desc[0] = 18;
        dev_desc[1] = 1;
        dev_desc[4] = 9;
        dev_desc[6] = if ss { 3 } else if multi_tt { 2 } else { 1 };
        Self {
            now: 0,
            ss,
            reset_ms: 20,
            ports: std::vec![SimPort::new(); usize::from(nports)],
            events: Vec::new(),
            armed: false,
            open: false,
            log: Vec::new(),
            config: config_blob(multi_tt && !ss),
            hub_desc: if ss { hub_ss_desc(nports) } else { hub20_desc(nports) },
            dev_desc,
            configured: None,
            depth: None,
            attached: Vec::new(),
            removed: Vec::new(),
            refuse_attach: false,
        }
    }

    fn port(&mut self, p: u8) -> Option<&mut SimPort> {
        usize::from(p).checked_sub(1).and_then(|i| self.ports.get_mut(i))
    }

    fn apply(&mut self, idx: usize, ev: Ev) {
        let Some(port) = self.ports.get_mut(idx) else {
            return;
        };
        match ev {
            Ev::Plug(true) => {
                port.present = true;
                if port.powered && !port.oc {
                    port.change |= CH_CONNECTION;
                }
            }
            Ev::Plug(false) => {
                port.present = false;
                port.enabled = false;
                port.reset_at = None;
                if port.powered {
                    port.change |= CH_CONNECTION;
                }
            }
            Ev::OverCurrent(true) => {
                let was = port.connected();
                port.oc = true;
                port.enabled = false;
                port.reset_at = None;
                port.change |= CH_OVER_CURRENT;
                if was {
                    port.change |= CH_CONNECTION;
                }
            }
            Ev::OverCurrent(false) => {
                port.oc = false;
                port.change |= CH_OVER_CURRENT;
            }
        }
    }

    fn step_ms(&mut self) {
        self.now += 1;
        let now = self.now;
        let mut due: Vec<(usize, Ev)> = Vec::new();
        self.events.retain(|&(t, p, ev)| {
            if t <= now {
                due.push((p, ev));
                false
            } else {
                true
            }
        });
        for (p, ev) in due {
            self.apply(p, ev);
        }
        let high = !self.ss;
        for port in self.ports.iter_mut() {
            if let Some(t) = port.reset_at {
                if now >= t {
                    port.reset_at = None;
                    port.enabled = port.present && port.powered && !port.oc;
                    port.high = high;
                    port.change |= CH_RESET;
                }
            }
        }
    }

    /// Interrupt report: bitmap of ports with pending changes, if armed.
    fn report(&mut self) -> Option<u8> {
        if !self.open || !self.armed {
            return None;
        }
        let mut bitmap = 0u8;
        for (i, port) in self.ports.iter().enumerate() {
            if port.change != 0 && i < 7 {
                bitmap |= 1 << (i + 1);
            }
        }
        if bitmap == 0 {
            return None;
        }
        self.armed = false;
        Some(bitmap)
    }

    fn status_word(&self, p: u8) -> (u16, u16) {
        let Some(port) = usize::from(p).checked_sub(1).and_then(|i| self.ports.get(i)) else {
            return (0, 0);
        };
        let mut s = 0u16;
        if port.connected() {
            s |= 1;
        }
        if port.enabled {
            s |= 2;
        }
        if port.oc {
            s |= 8;
        }
        if port.reset_at.is_some() {
            s |= 0x10;
        }
        if port.powered {
            s |= if self.ss { 0x200 } else { 0x100 };
        }
        if !self.ss && port.high {
            s |= 0x400;
        }
        (s, port.change)
    }
}

fn change_bit_for_selector(sel: u16) -> u16 {
    match sel {
        16 => CH_CONNECTION,
        17 => 1 << 1,
        18 => 1 << 2,
        19 => CH_OVER_CURRENT,
        20 => CH_RESET,
        29 => CH_BH_RESET,
        25 => CH_LINK_STATE,
        26 => CH_CONFIG_ERROR,
        _ => 0,
    }
}

type SharedSim = Rc<RefCell<Sim>>;

struct SimUsb(SharedSim);
struct SimHost(SharedSim);
struct SimClock(SharedSim);

impl UsbTransport for SimUsb {
    fn control(&mut self, s: ControlSetup, data: &mut [u8]) -> UsbResult<usize> {
        let mut sim = self.0.borrow_mut();
        let at = sim.now;
        sim.log.push(Req {
            at,
            request_type: s.request_type,
            request: s.request,
            value: s.value,
            index: s.index,
        });
        match (s.request_type, s.request) {
            (0x80, 6) => {
                let blob = sim.config.clone();
                Ok(copy_out(&blob, data))
            }
            (0xA0, 6) => {
                let blob = sim.hub_desc.clone();
                Ok(copy_out(&blob, data))
            }
            (0x00, 9) | (0x01, 11) | (0x20, 12) => {
                if s.request == 12 {
                    sim.depth = Some(s.value);
                }
                if s.request == 9 {
                    sim.configured = Some((s.value as u8, false, 0));
                }
                Ok(0)
            }
            (0xA3, 0) => {
                let (status, change) = sim.status_word(s.index as u8);
                let bytes = [
                    status.to_le_bytes()[0],
                    status.to_le_bytes()[1],
                    change.to_le_bytes()[0],
                    change.to_le_bytes()[1],
                ];
                Ok(copy_out(&bytes, data))
            }
            (0x23, 1) => {
                let bit = change_bit_for_selector(s.value);
                if let Some(port) = sim.port(s.index as u8) {
                    port.change &= !bit;
                }
                Ok(0)
            }
            (0x23, 3) => {
                let now = sim.now;
                let reset_ms = sim.reset_ms;
                if let Some(port) = sim.port(s.index as u8) {
                    if s.value == 8 {
                        port.powered = true;
                        port.change |= CH_CONNECTION;
                    } else if s.value == 4 && port.connected() {
                        port.enabled = false;
                        if port.hang > 0 {
                            port.hang -= 1;
                            port.reset_at = None;
                        } else {
                            port.reset_at = Some(now + reset_ms);
                        }
                    }
                }
                Ok(0)
            }
            _ => Err(UsbError::Transport(-1)),
        }
    }

    fn interrupt_open(
        &mut self,
        _endpoint: u8,
        _max_packet: u16,
        _interval: u8,
        _buf: &DmaBuf,
    ) -> UsbResult<()> {
        let mut sim = self.0.borrow_mut();
        sim.open = true;
        sim.armed = true;
        Ok(())
    }

    fn interrupt_report(&mut self, out: &mut [u8]) -> Option<usize> {
        let mut sim = self.0.borrow_mut();
        let bitmap = sim.report()?;
        *out.first_mut()? = bitmap;
        Some(1)
    }

    fn interrupt_resubmit(&mut self) -> UsbResult<()> {
        self.0.borrow_mut().armed = true;
        Ok(())
    }

    fn interrupt_cancel(&mut self) {
        self.0.borrow_mut().open = false;
    }
}

fn copy_out(src: &[u8], dst: &mut [u8]) -> usize {
    let n = src.len().min(dst.len());
    for (d, s) in dst.iter_mut().zip(src.iter()) {
        *d = *s;
    }
    n
}

impl HubHost for SimHost {
    fn device_descriptor(&self, out: &mut [u8]) -> usize {
        let sim = self.0.borrow();
        copy_out(&sim.dev_desc, out)
    }
    fn is_super_speed(&self) -> bool {
        self.0.borrow().ss
    }
    fn depth(&self) -> u8 {
        1
    }
    fn configure(&mut self, nports: u8, multi_tt: bool, think: u8) -> bool {
        self.0.borrow_mut().configured = Some((nports, multi_tt, think));
        true
    }
    fn child_discovered(&mut self, port: u8, speed: Speed) -> bool {
        let mut sim = self.0.borrow_mut();
        let now = sim.now;
        if sim.refuse_attach {
            return false;
        }
        sim.attached.push((port, speed, now));
        true
    }
    fn child_removed(&mut self, port: u8) {
        let mut sim = self.0.borrow_mut();
        let now = sim.now;
        sim.removed.push((port, now));
    }
}

impl Clock for SimClock {
    fn now_ms(&self) -> u64 {
        self.0.borrow().now
    }
    fn sleep_ms(&self, ms: u32) {
        for _ in 0..ms {
            self.0.borrow_mut().step_ms();
        }
    }
}

type TestHub = Hub<SimUsb, SimHost, SimClock>;

fn new_sim(nports: u8, ss: bool, multi_tt: bool) -> SharedSim {
    Rc::new(RefCell::new(Sim::new(nports, ss, multi_tt)))
}

fn start(sim: &SharedSim) -> TestHub {
    Hub::start(
        SimUsb(sim.clone()),
        SimHost(sim.clone()),
        SimClock(sim.clone()),
    )
    .expect("hub start")
}

fn now(sim: &SharedSim) -> u64 {
    sim.borrow().now
}

fn run(hub: &mut TestHub, sim: &SharedSim, ms: u64) {
    let end = now(sim) + ms;
    while now(sim) < end {
        hub.tick();
        SimClock(sim.clone()).sleep_ms(10);
    }
}

fn schedule(sim: &SharedSim, at: u64, port: usize, ev: Ev) {
    sim.borrow_mut().events.push((at, port, ev));
}

fn attached(sim: &SharedSim) -> Vec<(u8, Speed)> {
    sim.borrow().attached.iter().map(|&(p, s, _)| (p, s)).collect()
}

fn removed(sim: &SharedSim) -> Vec<u8> {
    sim.borrow().removed.iter().map(|&(p, _)| p).collect()
}

fn count_req(sim: &SharedSim, rt: u8, req: u8, value: u16, index: u16) -> usize {
    sim.borrow()
        .log
        .iter()
        .filter(|r| {
            r.request_type == rt && r.request == req && r.value == value && r.index == index
        })
        .count()
}

#[test]
fn power_on_sequence_configures_multi_tt_hub() {
    let sim = new_sim(4, false, true);
    sim.borrow_mut().ports[0].present = false;
    let start_at = now(&sim);
    let _hub = start(&sim);
    let s = sim.borrow();

    // SET_CONFIGURATION comes first, then the multi-TT alternate setting.
    let order: Vec<(u8, u8)> = s.log.iter().map(|r| (r.request_type, r.request)).collect();
    let cfg = order.iter().position(|&x| x == (0x00, 9)).unwrap();
    let alt = order.iter().position(|&x| x == (0x01, 11)).unwrap();
    let pwr = order.iter().position(|&x| x == (0x23, 3)).unwrap();
    assert!(cfg < alt && alt < pwr, "bad order: {order:?}");
    assert_eq!(s.configured, Some((4, true, 0)));

    // Every port gets PORT_POWER, and the hub waits bPwrOn2PwrGood*2 = 20 ms.
    for p in 1..=4u16 {
        assert_eq!(count_req(&sim, 0x23, 3, 8, p), 1, "port {p} power");
    }
    let last_power = s
        .log
        .iter()
        .filter(|r| r.request == 3)
        .map(|r| r.at)
        .max()
        .unwrap();
    assert!(s.now - last_power >= 20, "power-on wait too short");
    assert!(last_power >= start_at);
}

#[test]
fn single_tt_superspeed_hub_sets_depth_and_no_alt() {
    let sim = new_sim(2, true, false);
    let _hub = start(&sim);
    assert_eq!(count_req(&sim, 0x01, 11, 1, 0), 0);
    assert_eq!(sim.borrow().depth, Some(1));
    assert_eq!(sim.borrow().configured, Some((2, false, 0)));
}

#[test]
fn coldplug_attaches_ports_present_before_power_on() {
    let sim = new_sim(4, false, false);
    sim.borrow_mut().ports[0].present = true;
    sim.borrow_mut().ports[2].present = true;
    let mut hub = start(&sim);
    hub.coldplug();
    run(&mut hub, &sim, 500);
    assert_eq!(
        attached(&sim),
        std::vec![(1, Speed::High), (3, Speed::High)]
    );
    assert!(removed(&sim).is_empty());
}

#[test]
fn bouncing_connect_is_rejected_then_stable_connect_attaches() {
    let sim = new_sim(3, false, false);
    let mut hub = start(&sim);
    hub.coldplug();
    // Port 2 bounces: up at 100, down at 130, stable from 260.
    schedule(&sim, 100, 1, Ev::Plug(true));
    schedule(&sim, 130, 1, Ev::Plug(false));
    schedule(&sim, 260, 1, Ev::Plug(true));
    run(&mut hub, &sim, 700);

    assert_eq!(attached(&sim), std::vec![(2, Speed::High)]);
    let p2 = hub.ports()[1].counters;
    assert!(p2.debounce_failures >= 1, "bounce not counted");
    assert_eq!(p2.resets, 1, "bounce must not reset the port");
    let first_reset = sim
        .borrow()
        .log
        .iter()
        .find(|r| r.request == 3 && r.value == 4 && r.index == 2)
        .map(|r| r.at)
        .unwrap();
    assert!(first_reset >= 360, "reset before 100 ms of stable connect");
}

#[test]
fn reset_timeout_retries_and_succeeds() {
    let sim = new_sim(2, false, false);
    sim.borrow_mut().ports[0].present = true;
    sim.borrow_mut().ports[0].hang = 1;
    let mut hub = start(&sim);
    hub.coldplug();
    run(&mut hub, &sim, 1500);

    let c = hub.ports()[0].counters;
    assert_eq!(c.resets, 2);
    assert_eq!(c.reset_failures, 0);
    assert_eq!(attached(&sim), std::vec![(1, Speed::High)]);
    assert!(sim.borrow().attached[0].2 >= 500, "timeout not honoured");
}

#[test]
fn reset_attempts_exhausted_leaves_port_empty() {
    let sim = new_sim(2, false, false);
    sim.borrow_mut().ports[0].present = true;
    sim.borrow_mut().ports[0].hang = 3;
    let mut hub = start(&sim);
    hub.coldplug();
    run(&mut hub, &sim, 2000);

    let c = hub.ports()[0].counters;
    assert_eq!(c.resets, 3);
    assert_eq!(c.reset_failures, 1);
    assert!(attached(&sim).is_empty());
    assert_eq!(hub.ports()[0].phase(), Phase::Empty);
}

#[test]
fn disconnect_reports_removal_and_reconnect_attaches_again() {
    let sim = new_sim(2, false, false);
    sim.borrow_mut().ports[0].present = true;
    let mut hub = start(&sim);
    hub.coldplug();
    run(&mut hub, &sim, 400);
    assert_eq!(attached(&sim).len(), 1);

    schedule(&sim, now(&sim) + 50, 0, Ev::Plug(false));
    run(&mut hub, &sim, 200);
    assert_eq!(removed(&sim), std::vec![1]);
    assert_eq!(hub.ports()[0].phase(), Phase::Empty);

    schedule(&sim, now(&sim) + 50, 0, Ev::Plug(true));
    run(&mut hub, &sim, 400);
    assert_eq!(attached(&sim).len(), 2);
}

#[test]
fn connect_disconnect_storm_balances() {
    let sim = new_sim(4, false, false);
    let mut hub = start(&sim);
    hub.coldplug();
    let base = now(&sim);
    for cycle in 0..5u64 {
        let at = base + cycle * 400;
        schedule(&sim, at, 3, Ev::Plug(true));
        schedule(&sim, at + 300, 3, Ev::Plug(false));
    }
    run(&mut hub, &sim, 2200);
    assert_eq!(attached(&sim).len(), 5);
    assert_eq!(removed(&sim), std::vec![4, 4, 4, 4, 4]);
}

#[test]
fn over_current_detaches_backs_off_and_repowers() {
    let sim = new_sim(2, false, false);
    sim.borrow_mut().ports[0].present = true;
    let mut hub = start(&sim);
    hub.coldplug();
    run(&mut hub, &sim, 400);
    assert_eq!(attached(&sim).len(), 1);

    let t = now(&sim);
    schedule(&sim, t + 10, 0, Ev::OverCurrent(true));
    schedule(&sim, t + 60, 0, Ev::OverCurrent(false));
    run(&mut hub, &sim, 200);
    assert_eq!(removed(&sim), std::vec![1]);
    assert_eq!(hub.ports()[0].counters.over_current, 1);
    assert!(matches!(hub.ports()[0].phase(), Phase::Faulted { .. }));

    // Backoff is 1 s; the port is then re-powered and the device comes back.
    run(&mut hub, &sim, 1500);
    assert_eq!(count_req(&sim, 0x23, 3, 8, 1), 2, "re-power after backoff");
    assert_eq!(attached(&sim).len(), 2);
}

#[test]
fn superspeed_change_bits_are_acknowledged_with_ss_selectors() {
    let sim = new_sim(2, true, false);
    sim.borrow_mut().ports[0].present = true;
    sim.borrow_mut().ports[0].change = CH_BH_RESET | CH_LINK_STATE | CH_CONFIG_ERROR;
    let mut hub = start(&sim);
    hub.coldplug();
    run(&mut hub, &sim, 500);

    assert_eq!(attached(&sim), std::vec![(1, Speed::Super)]);
    for sel in [29u16, 25, 26] {
        assert!(
            count_req(&sim, 0x23, 1, sel, 1) >= 1,
            "CLEAR_FEATURE {sel} missing"
        );
    }
    assert_eq!(sim.borrow().ports[0].change & !CH_RESET, 0);
}

#[test]
fn usb2_hub_never_clears_superspeed_selectors() {
    let sim = new_sim(2, false, false);
    sim.borrow_mut().ports[0].change = CH_LINK_STATE;
    let mut hub = start(&sim);
    hub.coldplug();
    run(&mut hub, &sim, 100);
    assert_eq!(count_req(&sim, 0x23, 1, 25, 1), 0);
}

#[test]
fn refused_child_is_counted_and_not_retried_each_pass() {
    let sim = new_sim(2, false, false);
    sim.borrow_mut().refuse_attach = true;
    sim.borrow_mut().ports[0].present = true;
    let mut hub = start(&sim);
    hub.coldplug();
    run(&mut hub, &sim, 500);
    assert_eq!(hub.stats.attach_refused, 1);
    assert!(attached(&sim).is_empty());
}

#[test]
fn zero_port_hub_descriptor_is_rejected() {
    let sim = new_sim(2, false, false);
    sim.borrow_mut().hub_desc = hub20_desc(0);
    let r = Hub::start(
        SimUsb(sim.clone()),
        SimHost(sim.clone()),
        SimClock(sim.clone()),
    );
    // The parser rejects the descriptor (DeviceRemovable overruns it) before
    // the port-count check can run; either way the hub must not start.
    assert!(r.is_err());
}
