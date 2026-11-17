//! Downstream port state machine.  Pure logic: the caller supplies the sampled
//! port status and the current time, and gets back the one action to perform.
//! No I/O, no clock, so the whole machine runs under host `cargo test`.

/// Connection must stay up this long before a reset (sampled every
/// [`SAMPLE_MS`]).
pub const DEBOUNCE_MS: u64 = 100;
pub const SAMPLE_MS: u64 = 25;
/// Time allowed for C_PORT_RESET after SET_FEATURE(PORT_RESET).
pub const RESET_TIMEOUT_MS: u64 = 500;
/// Total reset attempts (the first plus retries) before the port gives up.
pub const RESET_ATTEMPTS: u8 = 3;
/// Reset recovery interval before the child is handed to the core.
pub const RECOVERY_MS: u64 = 10;
/// Wait before re-powering a port after an over-current event.
pub const OVER_CURRENT_BACKOFF_MS: u64 = 1000;
/// Interval for polling a port that is in a timed phase.
pub const POLL_MS: u64 = 10;

/// Device speed as the C core's `enum usb_speed` sees it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Speed {
    Low,
    Full,
    High,
    Super,
}

impl Speed {
    /// Value of the matching `enum usb_speed` constant.
    pub const fn c_value(self) -> i32 {
        match self {
            Speed::Low => 1,
            Speed::Full => 2,
            Speed::High => 3,
            Speed::Super => 4,
        }
    }
}

/// wPortStatus bits.  USB 2.0 hubs use bit 9/10 for speed; SuperSpeed hubs
/// use bit 9 for power and report speed elsewhere (not decoded here).
pub const ST_CONNECTION: u16 = 1 << 0;
pub const ST_ENABLE: u16 = 1 << 1;
pub const ST_OVER_CURRENT: u16 = 1 << 3;
pub const ST_LOW_SPEED: u16 = 1 << 9;
pub const ST_HIGH_SPEED: u16 = 1 << 10;

/// wPortChange bits.  Bit n is the change for feature selector `CLEAR_SELECTOR[n]`.
pub const CH_CONNECTION: u16 = 1 << 0;
pub const CH_OVER_CURRENT: u16 = 1 << 3;
pub const CH_RESET: u16 = 1 << 4;
pub const CH_BH_RESET: u16 = 1 << 5;
pub const CH_LINK_STATE: u16 = 1 << 6;
pub const CH_CONFIG_ERROR: u16 = 1 << 7;

/// CLEAR_FEATURE selector for each wPortChange bit (0..=7).
pub const CLEAR_SELECTOR: [u16; 8] = [16, 17, 18, 19, 20, 29, 25, 26];

/// What the state machine needs from one port status read.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PortSample {
    pub connected: bool,
    pub enabled: bool,
    pub over_current: bool,
    /// C_PORT_RESET is set: a reset issued earlier has completed.
    pub reset_done: bool,
    pub low_speed: bool,
    pub high_speed: bool,
}

/// Decode a raw GET_PORT_STATUS reply.  `ss` selects the SuperSpeed layout,
/// where the USB 2.0 speed bits mean nothing.
pub const fn decode(status: u16, change: u16, ss: bool) -> PortSample {
    PortSample {
        connected: status & ST_CONNECTION != 0,
        enabled: status & ST_ENABLE != 0,
        over_current: status & ST_OVER_CURRENT != 0,
        reset_done: change & CH_RESET != 0,
        low_speed: !ss && status & ST_LOW_SPEED != 0,
        high_speed: !ss && status & ST_HIGH_SPEED != 0,
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Phase {
    Empty,
    /// Connection seen; waiting for it to stay up.
    Debounce { since: u64 },
    /// SET_FEATURE(PORT_RESET) issued at `since`; `attempt` counts from 1.
    Reset { since: u64, attempt: u8 },
    /// Reset completed; waiting out the recovery interval.
    Recovery { until: u64, attempt: u8 },
    /// Child handed to the core.
    Attached(Speed),
    /// Over-current; port stays unpowered until `until`.
    Faulted { until: u64 },
}

/// The one thing the driver must do after a step.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Action {
    None,
    /// Issue SET_FEATURE(PORT_RESET) now.
    IssueReset,
    /// Issue SET_FEATURE(PORT_POWER) now, then sample the port again.
    PowerCycle,
    /// Call the core's child-discovered hook.
    Attached(Speed),
    /// Call the core's child-removed hook.
    Detached,
    DebounceFailed,
    /// Reset attempts exhausted; the port is left Empty.
    ResetFailed,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PortCounters {
    pub resets: u32,
    pub reset_failures: u32,
    pub debounce_failures: u32,
    pub over_current: u32,
    pub attached: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PortMachine {
    phase: Phase,
    ss: bool,
    next_poll: u64,
    pub counters: PortCounters,
    /// Last raw status and change words read from the hub (diagnostics).
    pub raw_status: u16,
    pub raw_change: u16,
}

impl PortMachine {
    pub const fn new(ss: bool) -> Self {
        Self {
            phase: Phase::Empty,
            ss,
            next_poll: 0,
            counters: PortCounters {
                resets: 0,
                reset_failures: 0,
                debounce_failures: 0,
                over_current: 0,
                attached: 0,
            },
            raw_status: 0,
            raw_change: 0,
        }
    }

    pub const fn phase(&self) -> Phase {
        self.phase
    }

    /// True when the port is in a timed phase and should be sampled now, even
    /// without an interrupt report.
    pub fn poll_due(&self, now: u64) -> bool {
        match self.phase {
            Phase::Empty | Phase::Attached(_) => false,
            Phase::Debounce { .. } | Phase::Reset { .. } => now >= self.next_poll,
            Phase::Recovery { until, .. } | Phase::Faulted { until } => now >= until,
        }
    }

    /// Advance the machine with a fresh sample taken at `now`.
    pub fn step(&mut self, now: u64, s: PortSample) -> Action {
        // Over-current wins over every other phase except the backoff itself.
        if s.over_current && !matches!(self.phase, Phase::Faulted { .. }) {
            let was_attached = matches!(self.phase, Phase::Attached(_));
            self.counters.over_current += 1;
            self.phase = Phase::Faulted {
                until: now + OVER_CURRENT_BACKOFF_MS,
            };
            return if was_attached { Action::Detached } else { Action::None };
        }

        match self.phase {
            Phase::Empty => {
                if s.connected {
                    self.phase = Phase::Debounce { since: now };
                    self.next_poll = now + SAMPLE_MS;
                }
                Action::None
            }
            Phase::Debounce { since } => {
                if !s.connected {
                    self.phase = Phase::Empty;
                    self.counters.debounce_failures += 1;
                    return Action::DebounceFailed;
                }
                if now.saturating_sub(since) >= DEBOUNCE_MS {
                    self.counters.resets += 1;
                    self.phase = Phase::Reset {
                        since: now,
                        attempt: 1,
                    };
                    self.next_poll = now + POLL_MS;
                    return Action::IssueReset;
                }
                self.next_poll = now + SAMPLE_MS;
                Action::None
            }
            Phase::Reset { since, attempt } => {
                if !s.connected {
                    self.phase = Phase::Empty;
                    return Action::None;
                }
                if s.reset_done {
                    self.phase = Phase::Recovery {
                        until: now + RECOVERY_MS,
                        attempt,
                    };
                    self.next_poll = now + RECOVERY_MS;
                    return Action::None;
                }
                if now.saturating_sub(since) >= RESET_TIMEOUT_MS {
                    return self.reset_failed(now, attempt);
                }
                self.next_poll = now + POLL_MS;
                Action::None
            }
            Phase::Recovery { until, attempt } => {
                if now < until {
                    return Action::None;
                }
                if !s.connected {
                    self.phase = Phase::Empty;
                    return Action::None;
                }
                if !s.enabled {
                    return self.reset_failed(now, attempt);
                }
                let speed = if self.ss {
                    Speed::Super
                } else if s.low_speed {
                    Speed::Low
                } else if s.high_speed {
                    Speed::High
                } else {
                    Speed::Full
                };
                self.phase = Phase::Attached(speed);
                self.counters.attached += 1;
                Action::Attached(speed)
            }
            Phase::Attached(_) => {
                if s.connected {
                    Action::None
                } else {
                    self.phase = Phase::Empty;
                    Action::Detached
                }
            }
            Phase::Faulted { until } => {
                if now < until {
                    return Action::None;
                }
                self.phase = Phase::Empty;
                Action::PowerCycle
            }
        }
    }

    fn reset_failed(&mut self, now: u64, attempt: u8) -> Action {
        if attempt < RESET_ATTEMPTS {
            self.counters.resets += 1;
            self.phase = Phase::Reset {
                since: now,
                attempt: attempt + 1,
            };
            self.next_poll = now + POLL_MS;
            Action::IssueReset
        } else {
            self.counters.reset_failures += 1;
            self.phase = Phase::Empty;
            Action::ResetFailed
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const CONNECTED: PortSample = PortSample {
        connected: true,
        enabled: false,
        over_current: false,
        reset_done: false,
        low_speed: false,
        high_speed: false,
    };
    const NONE: PortSample = PortSample {
        connected: false,
        enabled: false,
        over_current: false,
        reset_done: false,
        low_speed: false,
        high_speed: false,
    };

    #[test]
    fn decode_usb2_and_ss_layouts() {
        let s = decode(ST_CONNECTION | ST_ENABLE | ST_HIGH_SPEED, 0, false);
        assert!(s.connected && s.enabled && s.high_speed && !s.low_speed);
        // Bit 9 is LOW speed on USB 2 but power on SuperSpeed: never a speed.
        let s = decode(ST_LOW_SPEED, 0, true);
        assert!(!s.low_speed);
        assert!(decode(0, CH_RESET, true).reset_done);
    }

    #[test]
    fn bouncing_connect_is_rejected_by_debounce() {
        let mut m = PortMachine::new(false);
        assert_eq!(m.step(0, CONNECTED), Action::None);
        assert_eq!(m.step(25, CONNECTED), Action::None);
        // Drops out before the 100 ms window closes.
        assert_eq!(m.step(50, NONE), Action::DebounceFailed);
        assert_eq!(m.phase(), Phase::Empty);
        assert_eq!(m.counters.debounce_failures, 1);
        assert_eq!(m.counters.resets, 0);
    }

    #[test]
    fn stable_connect_resets_after_debounce() {
        let mut m = PortMachine::new(false);
        m.step(0, CONNECTED);
        assert_eq!(m.step(75, CONNECTED), Action::None);
        assert_eq!(m.step(100, CONNECTED), Action::IssueReset);
        assert!(matches!(m.phase(), Phase::Reset { attempt: 1, .. }));
    }

    #[test]
    fn reset_timeout_retries_then_gives_up() {
        let mut m = PortMachine::new(false);
        m.step(0, CONNECTED);
        assert_eq!(m.step(100, CONNECTED), Action::IssueReset);
        assert_eq!(m.step(499, CONNECTED), Action::None);
        assert_eq!(m.step(600, CONNECTED), Action::IssueReset); // attempt 2
        assert_eq!(m.step(1200, CONNECTED), Action::IssueReset); // attempt 3
        assert_eq!(m.step(1800, CONNECTED), Action::ResetFailed);
        assert_eq!(m.phase(), Phase::Empty);
        assert_eq!(m.counters.resets, 3);
        assert_eq!(m.counters.reset_failures, 1);
    }

    #[test]
    fn reset_completion_attaches_with_speed() {
        let mut m = PortMachine::new(false);
        m.step(0, CONNECTED);
        m.step(100, CONNECTED);
        let done = PortSample {
            reset_done: true,
            ..CONNECTED
        };
        assert_eq!(m.step(120, done), Action::None);
        assert!(matches!(m.phase(), Phase::Recovery { .. }));
        let enabled = PortSample {
            enabled: true,
            high_speed: true,
            ..CONNECTED
        };
        assert_eq!(m.step(130, enabled), Action::Attached(Speed::High));
        assert_eq!(m.counters.attached, 1);
    }

    #[test]
    fn superspeed_port_attaches_as_super() {
        let mut m = PortMachine::new(true);
        m.step(0, CONNECTED);
        m.step(100, CONNECTED);
        m.step(
            120,
            PortSample {
                reset_done: true,
                ..CONNECTED
            },
        );
        let enabled = PortSample {
            enabled: true,
            ..CONNECTED
        };
        assert_eq!(m.step(130, enabled), Action::Attached(Speed::Super));
    }

    #[test]
    fn disconnect_after_attach_reports_detached() {
        let mut m = PortMachine::new(false);
        m.step(0, CONNECTED);
        m.step(100, CONNECTED);
        m.step(
            120,
            PortSample {
                reset_done: true,
                ..CONNECTED
            },
        );
        m.step(
            130,
            PortSample {
                enabled: true,
                low_speed: true,
                ..CONNECTED
            },
        );
        assert_eq!(m.step(200, NONE), Action::Detached);
        assert_eq!(m.phase(), Phase::Empty);
    }

    #[test]
    fn over_current_detaches_then_backs_off_before_power_cycle() {
        let mut m = PortMachine::new(false);
        m.step(0, CONNECTED);
        m.step(100, CONNECTED);
        m.step(
            120,
            PortSample {
                reset_done: true,
                ..CONNECTED
            },
        );
        m.step(
            130,
            PortSample {
                enabled: true,
                ..CONNECTED
            },
        );
        let oc = PortSample {
            over_current: true,
            ..NONE
        };
        assert_eq!(m.step(200, oc), Action::Detached);
        assert!(matches!(m.phase(), Phase::Faulted { .. }));
        assert_eq!(m.counters.over_current, 1);
        assert!(!m.poll_due(500));
        assert!(m.poll_due(1200));
        assert_eq!(m.step(1200, NONE), Action::PowerCycle);
        assert_eq!(m.phase(), Phase::Empty);
    }

    #[test]
    fn device_leaving_mid_reset_returns_to_empty_quietly() {
        let mut m = PortMachine::new(false);
        m.step(0, CONNECTED);
        m.step(100, CONNECTED);
        assert_eq!(m.step(150, NONE), Action::None);
        assert_eq!(m.phase(), Phase::Empty);
    }
}
