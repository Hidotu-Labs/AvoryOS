//! Bulk-Only Transport (BOT): CBW/CSW framing and the command engine.
//!
//! [`Bot`] is generic over [`BulkTransport`], so host tests drive it with a
//! scripted device.  Multi-byte fields in CBW and CSW are little-endian.
//! Reset recovery follows the Bulk-Only spec: Bulk-Only Mass Storage Reset,
//! then clear-halt IN, then clear-halt OUT.

use super::scsi::Sense;

pub const CBW_LEN: usize = 31;
pub const CSW_LEN: usize = 13;
pub const CBW_SIGNATURE: u32 = 0x4342_5355;
pub const CSW_SIGNATURE: u32 = 0x5342_5355;
const CBW_FLAG_IN: u8 = 0x80;
const CDB_MAX: usize = 16;

pub const TIMEOUT_CMD_MS: u32 = 5_000;
pub const TIMEOUT_DATA_MS: u32 = 30_000;
pub const TIMEOUT_CSW_MS: u32 = 5_000;

const CSW_STATUS_PASSED: u8 = 0;
const CSW_STATUS_FAILED: u8 = 1;
const CSW_STATUS_PHASE_ERROR: u8 = 2;

const RT_BOMSR: u8 = 0x21;
const REQ_BOMSR: u8 = 0xFF;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Dir {
    None,
    In,
    Out,
}

/// Failure of a single USB transfer, before any BOT interpretation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Xfer {
    Stall,
    Timeout,
    Gone,
    Error,
}

/// Bulk pipes and class control of one mass-storage interface.  Transfers
/// block until completion, stall, timeout or removal.
pub trait BulkTransport {
    fn bulk_out(&mut self, data: &[u8], timeout_ms: u32) -> Result<(), Xfer>;
    /// Returns the number of bytes received (a short packet is success).
    fn bulk_in(&mut self, data: &mut [u8], timeout_ms: u32) -> Result<usize, Xfer>;
    fn clear_halt_in(&mut self) -> Result<(), Xfer>;
    fn clear_halt_out(&mut self) -> Result<(), Xfer>;
    /// Control transfer on endpoint 0.  Returns bytes transferred.
    fn control(
        &mut self,
        request_type: u8,
        request: u8,
        value: u16,
        index: u16,
        data: &mut [u8],
    ) -> Result<usize, Xfer>;
}

/// Final state of one BOT command.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Outcome {
    /// CSW status 0.
    Passed,
    /// CSW status 1: the caller must REQUEST SENSE.
    Failed,
    /// CSW status 2, or a CSW or transfer that broke protocol.  Recovered by
    /// reset.
    PhaseError,
    /// An endpoint stalled and stayed halted after recovery.
    Stall,
    /// A transfer timed out.  Recovered by reset.
    Timeout,
    /// The device was removed.  No recovery is attempted.
    Gone,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Completion {
    pub outcome: Outcome,
    /// CSW data residue: bytes of the requested transfer that did not move.
    pub residue: u32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct BotStats {
    pub commands: u32,
    pub stalls: u32,
    pub timeouts: u32,
    pub resets: u32,
    pub phase_errors: u32,
    pub last_opcode: u8,
    pub last_sense: Option<Sense>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CswError {
    Length,
    Signature,
    Tag,
    /// A status byte outside 0..=2 (reserved values are phase errors).
    Status(u8),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Csw {
    pub status: u8,
    pub residue: u32,
}

fn put(dst: &mut [u8], at: usize, src: &[u8]) {
    if let Some(d) = dst.get_mut(at..at + src.len()) {
        d.copy_from_slice(src);
    }
}

fn le32(b: &[u8], at: usize) -> u32 {
    b.get(at..at + 4)
        .and_then(|s| <[u8; 4]>::try_from(s).ok())
        .map_or(0, u32::from_le_bytes)
}

/// Build a 31-byte Command Block Wrapper.  `cdb` must be 1 to 16 bytes.
pub fn encode_cbw(
    tag: u32,
    transfer_len: u32,
    dir: Dir,
    lun: u8,
    cdb: &[u8],
) -> Option<[u8; CBW_LEN]> {
    if cdb.is_empty() || cdb.len() > CDB_MAX {
        return None;
    }
    let cb_len = u8::try_from(cdb.len()).ok()?;
    let flags = if dir == Dir::In { CBW_FLAG_IN } else { 0 };
    let mut out = [0u8; CBW_LEN];
    put(&mut out, 0, &CBW_SIGNATURE.to_le_bytes());
    put(&mut out, 4, &tag.to_le_bytes());
    put(&mut out, 8, &transfer_len.to_le_bytes());
    put(&mut out, 12, &[flags, lun & 0x0F, cb_len]);
    put(&mut out, 15, cdb);
    Some(out)
}

/// Validate and decode a 13-byte Command Status Wrapper for `tag`.
pub fn decode_csw(raw: &[u8], tag: u32) -> Result<Csw, CswError> {
    if raw.len() != CSW_LEN {
        return Err(CswError::Length);
    }
    if le32(raw, 0) != CSW_SIGNATURE {
        return Err(CswError::Signature);
    }
    if le32(raw, 4) != tag {
        return Err(CswError::Tag);
    }
    let status = raw.get(12).copied().unwrap_or(CSW_STATUS_PHASE_ERROR);
    match status {
        CSW_STATUS_PASSED | CSW_STATUS_FAILED | CSW_STATUS_PHASE_ERROR => Ok(Csw {
            status,
            residue: le32(raw, 8),
        }),
        other => Err(CswError::Status(other)),
    }
}

/// BOT command engine for one interface.
pub struct Bot<T: BulkTransport> {
    transport: T,
    iface: u8,
    tag: u32,
    /// Accept a residue larger than the requested length (quirk).
    pub ignore_residue: bool,
    pub stats: BotStats,
}

impl<T: BulkTransport> Bot<T> {
    pub fn new(transport: T, iface: u8) -> Self {
        Self {
            transport,
            iface,
            tag: 0,
            ignore_residue: false,
            stats: BotStats::default(),
        }
    }

    pub fn transport_mut(&mut self) -> &mut T {
        &mut self.transport
    }

    /// Run one command: CBW, optional data phase, then CSW.  The data length
    /// is `data.len()` (zero when `dir` is `Dir::None`).
    pub fn command(&mut self, cdb: &[u8], dir: Dir, data: &mut [u8]) -> Completion {
        let len = if dir == Dir::None {
            0
        } else {
            u32::try_from(data.len()).unwrap_or(u32::MAX)
        };
        self.stats.commands = self.stats.commands.saturating_add(1);
        self.stats.last_opcode = cdb.first().copied().unwrap_or(0);
        self.tag = self.tag.wrapping_add(1);
        let tag = self.tag;

        let Some(cbw) = encode_cbw(tag, len, dir, 0, cdb) else {
            return Completion {
                outcome: Outcome::Failed,
                residue: len,
            };
        };
        if let Err(x) = self.transport.bulk_out(&cbw, TIMEOUT_CMD_MS) {
            return self.transfer_failed(x);
        }

        if dir != Dir::None && !data.is_empty() {
            let moved = match dir {
                Dir::In => self.transport.bulk_in(data, TIMEOUT_DATA_MS).map(|_| ()),
                _ => self.transport.bulk_out(data, TIMEOUT_DATA_MS),
            };
            match moved {
                Ok(()) => {}
                Err(Xfer::Stall) => {
                    // The status phase still follows; the CSW says what happened.
                    self.stats.stalls = self.stats.stalls.saturating_add(1);
                    let _ = if dir == Dir::In {
                        self.transport.clear_halt_in()
                    } else {
                        self.transport.clear_halt_out()
                    };
                }
                Err(x) => return self.transfer_failed(x),
            }
        }

        let mut raw = [0u8; CSW_LEN];
        let mut retried = false;
        let received = loop {
            match self.transport.bulk_in(&mut raw, TIMEOUT_CSW_MS) {
                Ok(n) => break n,
                Err(Xfer::Stall) if !retried => {
                    self.stats.stalls = self.stats.stalls.saturating_add(1);
                    retried = true;
                    let _ = self.transport.clear_halt_in();
                }
                Err(Xfer::Stall) => {
                    self.stats.stalls = self.stats.stalls.saturating_add(1);
                    return self.recover(Outcome::Stall);
                }
                Err(x) => return self.transfer_failed(x),
            }
        };
        if received != CSW_LEN {
            return self.recover(Outcome::PhaseError);
        }
        let Ok(csw) = decode_csw(&raw, tag) else {
            return self.recover(Outcome::PhaseError);
        };
        match csw.status {
            CSW_STATUS_PHASE_ERROR => self.recover(Outcome::PhaseError),
            _ if csw.residue > len && !self.ignore_residue => self.recover(Outcome::PhaseError),
            CSW_STATUS_PASSED => Completion {
                outcome: Outcome::Passed,
                residue: csw.residue,
            },
            _ => Completion {
                outcome: Outcome::Failed,
                residue: csw.residue,
            },
        }
    }

    /// Bulk-Only Mass Storage Reset, then clear-halt IN and OUT.  Every step is
    /// attempted even if an earlier one fails.
    pub fn reset_recovery(&mut self) {
        self.stats.resets = self.stats.resets.saturating_add(1);
        let _ = self
            .transport
            .control(RT_BOMSR, REQ_BOMSR, 0, u16::from(self.iface), &mut []);
        let _ = self.transport.clear_halt_in();
        let _ = self.transport.clear_halt_out();
    }

    fn transfer_failed(&mut self, x: Xfer) -> Completion {
        match x {
            Xfer::Gone => Completion {
                outcome: Outcome::Gone,
                residue: 0,
            },
            Xfer::Stall => {
                self.stats.stalls = self.stats.stalls.saturating_add(1);
                self.recover(Outcome::Stall)
            }
            Xfer::Timeout => self.recover(Outcome::Timeout),
            Xfer::Error => self.recover(Outcome::PhaseError),
        }
    }

    fn recover(&mut self, outcome: Outcome) -> Completion {
        match outcome {
            Outcome::PhaseError => {
                self.stats.phase_errors = self.stats.phase_errors.saturating_add(1);
            }
            Outcome::Timeout => {
                self.stats.timeouts = self.stats.timeouts.saturating_add(1);
            }
            _ => {}
        }
        self.reset_recovery();
        Completion {
            outcome,
            residue: 0,
        }
    }
}
