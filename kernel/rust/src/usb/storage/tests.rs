//! Host tests: the BOT engine, SCSI commands, and the device layer, run
//! against a scripted mock device and a fake disk.
#![allow(
    clippy::unwrap_used,
    clippy::expect_used,
    clippy::indexing_slicing,
    clippy::panic
)]

use core::cell::Cell;
use std::collections::VecDeque;
use std::vec::Vec;

use super::bot::{
    decode_csw, encode_cbw, Bot, BulkTransport, Completion, Csw, CswError, Dir, Outcome, Xfer,
    CBW_SIGNATURE, CSW_LEN, CSW_SIGNATURE,
};
use super::device::{
    quirk_flags, Bounce, Device, BOUNCE_LEN, QUIRK_MAX_SECTORS_64, QUIRK_NO_SYNC_CACHE,
};
use super::scsi::{self, needs_16, ScsiError, Sense};
use crate::usb::kernel::Clock;

// --- Scripted mock device ---------------------------------------------------

type Responder = Box<dyn FnMut(&[u8]) -> Plan>;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Op {
    /// CBW accepted, with its opcode.
    Cbw(u8),
    DataIn,
    DataOut,
    Csw,
    ClearIn,
    ClearOut,
    /// (request type, request, index)
    Control(u8, u8, u16),
}

#[derive(Clone, Debug)]
enum CswStep {
    Status { status: u8, residue: u32 },
    Raw(Vec<u8>),
    Fault(Xfer),
}

/// What the device does for one command.
#[derive(Default)]
struct Plan {
    /// Bytes returned in the IN data phase.
    data: Vec<u8>,
    /// Fault injected into the next IN data phase.
    data_fault: Option<Xfer>,
    /// CSW answers, one per attempt.  Empty means Passed with residue 0.
    csw: VecDeque<CswStep>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Phase {
    Idle,
    Data,
    Status,
}

struct Mock {
    respond: Responder,
    plan: Plan,
    phase: Phase,
    tag: u32,
    data_in: bool,
    max_lun: Option<u8>,
    ops: Vec<Op>,
    cdbs: Vec<Vec<u8>>,
    written: Vec<u8>,
}

impl Mock {
    fn new(respond: Responder) -> Self {
        Self {
            respond,
            plan: Plan::default(),
            phase: Phase::Idle,
            tag: 0,
            data_in: false,
            max_lun: Some(0),
            ops: Vec::new(),
            cdbs: Vec::new(),
            written: Vec::new(),
        }
    }

    fn send_csw(&mut self, data: &mut [u8]) -> Result<usize, Xfer> {
        let step = self.plan.csw.pop_front().unwrap_or(CswStep::Status {
            status: 0,
            residue: 0,
        });
        let raw = match step {
            CswStep::Fault(x) => return Err(x),
            CswStep::Raw(v) => v,
            CswStep::Status { status, residue } => {
                let mut v = Vec::with_capacity(CSW_LEN);
                v.extend_from_slice(&CSW_SIGNATURE.to_le_bytes());
                v.extend_from_slice(&self.tag.to_le_bytes());
                v.extend_from_slice(&residue.to_le_bytes());
                v.push(status);
                v
            }
        };
        self.ops.push(Op::Csw);
        self.phase = Phase::Idle;
        let n = data.len().min(raw.len());
        data[..n].copy_from_slice(&raw[..n]);
        Ok(n)
    }
}

impl BulkTransport for Mock {
    fn bulk_out(&mut self, data: &[u8], _timeout_ms: u32) -> Result<(), Xfer> {
        match self.phase {
            Phase::Idle => {
                let tag = le32(data, 4);
                let tlen = le32(data, 8);
                let flags = data[12];
                let cb_len = usize::from(data[14]);
                let cdb = data[15..15 + cb_len].to_vec();
                self.ops.push(Op::Cbw(cdb[0]));
                self.cdbs.push(cdb.clone());
                self.plan = (self.respond)(&cdb);
                self.tag = tag;
                self.data_in = flags & 0x80 != 0;
                self.phase = if tlen == 0 {
                    Phase::Status
                } else {
                    Phase::Data
                };
                Ok(())
            }
            Phase::Data if !self.data_in => {
                self.ops.push(Op::DataOut);
                self.written.extend_from_slice(data);
                self.phase = Phase::Status;
                Ok(())
            }
            _ => Err(Xfer::Error),
        }
    }

    fn bulk_in(&mut self, data: &mut [u8], _timeout_ms: u32) -> Result<usize, Xfer> {
        match self.phase {
            Phase::Data if self.data_in => {
                self.ops.push(Op::DataIn);
                self.phase = Phase::Status;
                if let Some(f) = self.plan.data_fault.take() {
                    return Err(f);
                }
                let n = data.len().min(self.plan.data.len());
                data[..n].copy_from_slice(&self.plan.data[..n]);
                Ok(n)
            }
            Phase::Status => self.send_csw(data),
            _ => Err(Xfer::Error),
        }
    }

    fn clear_halt_in(&mut self) -> Result<(), Xfer> {
        self.ops.push(Op::ClearIn);
        Ok(())
    }

    fn clear_halt_out(&mut self) -> Result<(), Xfer> {
        self.ops.push(Op::ClearOut);
        Ok(())
    }

    fn control(
        &mut self,
        request_type: u8,
        request: u8,
        _value: u16,
        index: u16,
        data: &mut [u8],
    ) -> Result<usize, Xfer> {
        self.ops.push(Op::Control(request_type, request, index));
        match (request_type, request) {
            // Bulk-Only Mass Storage Reset: the device returns to command wait.
            (0x21, 0xFF) => {
                self.phase = Phase::Idle;
                Ok(0)
            }
            (0xA1, 0xFE) => match (self.max_lun, data.first_mut()) {
                (Some(v), Some(d)) => {
                    *d = v;
                    Ok(1)
                }
                (Some(_), None) => Ok(0),
                (None, _) => Err(Xfer::Stall),
            },
            _ => Ok(0),
        }
    }
}

fn le32(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}

fn ok_data(data: Vec<u8>) -> Plan {
    Plan {
        data,
        ..Plan::default()
    }
}

fn failed() -> Plan {
    let mut csw = VecDeque::new();
    csw.push_back(CswStep::Status {
        status: 1,
        residue: 0,
    });
    Plan {
        csw,
        ..Plan::default()
    }
}

/// A bot with a fixed plan for every command.
fn bot_with(respond: impl FnMut(&[u8]) -> Plan + 'static) -> Bot<Mock> {
    Bot::new(Mock::new(Box::new(respond)), 2)
}

const READ_CDB: [u8; 6] = [0x12, 0, 0, 0, 36, 0];

fn csw_bytes(tag: u32, residue: u32, status: u8) -> Vec<u8> {
    let mut v = Vec::new();
    v.extend_from_slice(&CSW_SIGNATURE.to_le_bytes());
    v.extend_from_slice(&tag.to_le_bytes());
    v.extend_from_slice(&residue.to_le_bytes());
    v.push(status);
    v
}

fn reset_ops() -> [Op; 3] {
    [Op::Control(0x21, 0xFF, 2), Op::ClearIn, Op::ClearOut]
}

// --- CBW / CSW framing ------------------------------------------------------

#[test]
fn cbw_layout_is_little_endian_and_pads_cdb() {
    let cdb = [0x28, 0, 1, 2, 3, 4, 5, 6, 7, 8];
    let cbw = encode_cbw(0x1122_3344, 0x200, Dir::In, 0, &cdb).unwrap();
    assert_eq!(&cbw[0..4], &[0x55, 0x53, 0x42, 0x43]);
    assert_eq!(&cbw[4..8], &[0x44, 0x33, 0x22, 0x11]);
    assert_eq!(&cbw[8..12], &[0x00, 0x02, 0x00, 0x00]);
    assert_eq!(cbw[12], 0x80);
    assert_eq!(cbw[13], 0);
    assert_eq!(cbw[14], 10);
    assert_eq!(&cbw[15..25], &cdb);
    assert!(cbw[25..].iter().all(|&b| b == 0));
}

#[test]
fn cbw_out_flag_and_cdb_bounds() {
    let cbw = encode_cbw(1, 0, Dir::Out, 0, &[0x35]).unwrap();
    assert_eq!(cbw[12], 0x00);
    assert!(encode_cbw(1, 0, Dir::None, 0, &[]).is_none());
    assert!(encode_cbw(1, 0, Dir::None, 0, &[0u8; 17]).is_none());
    assert!(encode_cbw(1, 0, Dir::None, 0, &[0u8; 16]).is_some());
}

#[test]
fn csw_validation_reports_each_defect() {
    let good = csw_bytes(7, 12, 0);
    assert_eq!(
        decode_csw(&good, 7),
        Ok(Csw {
            status: 0,
            residue: 12
        })
    );
    let mut bad_sig = good.clone();
    bad_sig[0] = 0;
    assert_eq!(decode_csw(&bad_sig, 7), Err(CswError::Signature));
    assert_eq!(decode_csw(&good, 8), Err(CswError::Tag));
    assert_eq!(decode_csw(&good[..12], 7), Err(CswError::Length));
    assert_eq!(decode_csw(&csw_bytes(7, 0, 3), 7), Err(CswError::Status(3)));
}

// --- BOT engine -------------------------------------------------------------

#[test]
fn passed_command_moves_data_then_reads_csw() {
    let mut bot = bot_with(|_| ok_data(vec![0xAA; 36]));
    let mut buf = [0u8; 36];
    let c = bot.command(&READ_CDB, Dir::In, &mut buf);
    assert_eq!(
        c,
        Completion {
            outcome: Outcome::Passed,
            residue: 0
        }
    );
    assert_eq!(buf, [0xAA; 36]);
    assert_eq!(
        bot.transport_mut().ops,
        vec![Op::Cbw(0x12), Op::DataIn, Op::Csw]
    );
}

#[test]
fn data_phase_stall_clears_halt_then_still_reads_csw() {
    let mut bot = bot_with(|_| Plan {
        data: vec![1; 36],
        data_fault: Some(Xfer::Stall),
        ..Plan::default()
    });
    let mut buf = [0u8; 36];
    let c = bot.command(&READ_CDB, Dir::In, &mut buf);
    assert_eq!(c.outcome, Outcome::Passed);
    assert_eq!(
        bot.transport_mut().ops,
        vec![Op::Cbw(0x12), Op::DataIn, Op::ClearIn, Op::Csw]
    );
    assert_eq!(bot.stats.stalls, 1);
    assert_eq!(bot.stats.resets, 0);
}

#[test]
fn csw_stall_is_retried_once() {
    let mut bot = bot_with(|_| {
        let mut csw = VecDeque::new();
        csw.push_back(CswStep::Fault(Xfer::Stall));
        Plan {
            csw,
            ..Plan::default()
        }
    });
    let mut buf = [0u8; 36];
    assert_eq!(
        bot.command(&READ_CDB, Dir::In, &mut buf).outcome,
        Outcome::Passed
    );
    assert_eq!(
        bot.transport_mut().ops,
        vec![Op::Cbw(0x12), Op::DataIn, Op::ClearIn, Op::Csw]
    );
}

#[test]
fn csw_stalling_twice_resets_and_reports_stall() {
    let mut bot = bot_with(|_| {
        let mut csw = VecDeque::new();
        csw.push_back(CswStep::Fault(Xfer::Stall));
        csw.push_back(CswStep::Fault(Xfer::Stall));
        Plan {
            csw,
            ..Plan::default()
        }
    });
    let mut buf = [0u8; 36];
    let c = bot.command(&READ_CDB, Dir::In, &mut buf);
    assert_eq!(c.outcome, Outcome::Stall);
    let mut expect = vec![Op::Cbw(0x12), Op::DataIn, Op::ClearIn];
    expect.extend_from_slice(&reset_ops());
    assert_eq!(bot.transport_mut().ops, expect);
}

#[test]
fn phase_error_runs_reset_sequence_in_order() {
    let mut bot = bot_with(|_| {
        let mut csw = VecDeque::new();
        csw.push_back(CswStep::Status {
            status: 2,
            residue: 0,
        });
        Plan {
            csw,
            ..Plan::default()
        }
    });
    let mut buf = [0u8; 36];
    let c = bot.command(&READ_CDB, Dir::In, &mut buf);
    assert_eq!(c.outcome, Outcome::PhaseError);
    assert_eq!(bot.stats.phase_errors, 1);
    assert_eq!(bot.stats.resets, 1);
    let mut expect = vec![Op::Cbw(0x12), Op::DataIn, Op::Csw];
    expect.extend_from_slice(&reset_ops());
    assert_eq!(bot.transport_mut().ops, expect);
}

#[test]
fn bad_csw_signature_tag_and_length_are_phase_errors() {
    let cases: [Vec<u8>; 3] = [
        {
            let mut v = csw_bytes(1, 0, 0);
            v[0] = 0xFF;
            v
        },
        csw_bytes(99, 0, 0),
        csw_bytes(1, 0, 0)[..12].to_vec(),
    ];
    for raw in cases {
        let mut bot = bot_with(move |_| {
            let mut csw = VecDeque::new();
            csw.push_back(CswStep::Raw(raw.clone()));
            Plan {
                csw,
                ..Plan::default()
            }
        });
        let mut buf = [0u8; 36];
        let c = bot.command(&READ_CDB, Dir::In, &mut buf);
        assert_eq!(c.outcome, Outcome::PhaseError);
        assert_eq!(bot.stats.resets, 1);
    }
}

#[test]
fn residue_beyond_request_is_phase_error_unless_quirked() {
    let plan = || {
        let mut csw = VecDeque::new();
        csw.push_back(CswStep::Status {
            status: 0,
            residue: 600,
        });
        Plan {
            csw,
            ..Plan::default()
        }
    };
    let mut strict = bot_with(move |_| plan());
    let mut buf = [0u8; 512];
    assert_eq!(
        strict.command(&READ_CDB, Dir::In, &mut buf).outcome,
        Outcome::PhaseError
    );

    let mut lenient = bot_with(move |_| plan());
    lenient.ignore_residue = true;
    let c = lenient.command(&READ_CDB, Dir::In, &mut buf);
    assert_eq!(c.outcome, Outcome::Passed);
    assert_eq!(c.residue, 600);
}

#[test]
fn data_timeout_recovers_and_reports_timeout() {
    let mut bot = bot_with(|_| Plan {
        data: vec![0; 36],
        data_fault: Some(Xfer::Timeout),
        ..Plan::default()
    });
    let mut buf = [0u8; 36];
    assert_eq!(
        bot.command(&READ_CDB, Dir::In, &mut buf).outcome,
        Outcome::Timeout
    );
    assert_eq!(bot.stats.timeouts, 1);
    let mut expect = vec![Op::Cbw(0x12), Op::DataIn];
    expect.extend_from_slice(&reset_ops());
    assert_eq!(bot.transport_mut().ops, expect);
}

#[test]
fn removal_is_reported_without_recovery() {
    let mut bot = bot_with(|_| {
        let mut csw = VecDeque::new();
        csw.push_back(CswStep::Fault(Xfer::Gone));
        Plan {
            csw,
            ..Plan::default()
        }
    });
    let mut buf = [0u8; 36];
    assert_eq!(
        bot.command(&READ_CDB, Dir::In, &mut buf).outcome,
        Outcome::Gone
    );
    assert_eq!(bot.stats.resets, 0);
    assert_eq!(bot.transport_mut().ops, vec![Op::Cbw(0x12), Op::DataIn]);
}

#[test]
fn failed_status_requests_sense_and_records_it() {
    // TEST UNIT READY fails; REQUEST SENSE answers NOT READY / becoming ready.
    let mut bot = bot_with(|cdb| {
        if cdb[0] == 0x00 {
            failed()
        } else {
            let mut d = vec![0u8; 18];
            d[0] = 0x70;
            d[2] = 0x02;
            d[12] = 0x04;
            d[13] = 0x01;
            ok_data(d)
        }
    });
    let clock = FakeClock::new();
    let err = scsi::test_unit_ready(&mut bot, &clock);
    assert_eq!(
        err,
        Err(ScsiError::Check(Sense {
            key: 2,
            asc: 0x04,
            ascq: 0x01
        }))
    );
    assert_eq!(bot.transport_mut().cdbs[1][0], 0x03);
    assert_eq!(
        bot.stats.last_sense,
        Some(Sense {
            key: 2,
            asc: 0x04,
            ascq: 0x01
        })
    );
}

#[test]
fn sense_decodes_fixed_format_only() {
    let mut d = [0u8; 18];
    d[0] = 0x70;
    d[2] = 0x25; // ILI bit set, key 5
    d[12] = 0x20;
    d[13] = 0x00;
    assert_eq!(
        Sense::decode(&d),
        Some(Sense {
            key: 5,
            asc: 0x20,
            ascq: 0
        })
    );
    d[0] = 0x72;
    assert_eq!(Sense::decode(&d), None);
}

// --- Fake disk --------------------------------------------------------------

struct Disk {
    sectors: u64,
    block: u32,
    /// READ CAPACITY(10) reports 0xFFFFFFFF, forcing the (16) path.
    saturate10: bool,
    /// TEST UNIT READY fails this many times first.
    tur_fail: u32,
    read_only: bool,
    periph: u8,
    mode_fail: bool,
    /// SYNCHRONIZE CACHE fails with this sense key.
    sync_key: Option<u8>,
    sense: (u8, u8, u8),
}

impl Disk {
    fn new(sectors: u64, block: u32) -> Self {
        Self {
            sectors,
            block,
            saturate10: false,
            tur_fail: 0,
            read_only: false,
            periph: 0,
            mode_fail: false,
            sync_key: None,
            sense: (0, 0, 0),
        }
    }

    fn respond(&mut self, cdb: &[u8]) -> Plan {
        let bs = self.block as usize;
        match cdb[0] {
            0x12 => {
                let mut d = vec![0u8; 36];
                d[0] = self.periph;
                d[1] = 0x80;
                d[8..16].copy_from_slice(b"AVORY   ");
                d[16..32].copy_from_slice(b"TESTDISK        ");
                d[32..36].copy_from_slice(b"1.0 ");
                ok_data(d)
            }
            0x00 => {
                if self.tur_fail > 0 {
                    self.tur_fail -= 1;
                    self.sense = (2, 0x04, 0x01);
                    failed()
                } else {
                    ok_data(Vec::new())
                }
            }
            0x03 => {
                let mut d = vec![0u8; 18];
                d[0] = 0x70;
                d[2] = self.sense.0;
                d[12] = self.sense.1;
                d[13] = self.sense.2;
                ok_data(d)
            }
            0x25 => {
                let last = if self.saturate10 {
                    u32::MAX
                } else {
                    u32::try_from(self.sectors - 1).unwrap_or(u32::MAX)
                };
                let mut d = vec![0u8; 8];
                d[0..4].copy_from_slice(&last.to_be_bytes());
                d[4..8].copy_from_slice(&self.block.to_be_bytes());
                ok_data(d)
            }
            0x9E => {
                let mut d = vec![0u8; 32];
                d[0..8].copy_from_slice(&(self.sectors - 1).to_be_bytes());
                d[8..12].copy_from_slice(&self.block.to_be_bytes());
                ok_data(d)
            }
            0x1A => {
                if self.mode_fail {
                    return failed();
                }
                let mut d = vec![0u8; 4];
                d[0] = 3;
                d[2] = if self.read_only { 0x80 } else { 0 };
                ok_data(d)
            }
            0x35 => match self.sync_key {
                Some(k) => {
                    self.sense = (k, 0x20, 0);
                    failed()
                }
                None => ok_data(Vec::new()),
            },
            0x28 | 0x88 => {
                let (lba, count) = parse_rw(cdb);
                let mut d = vec![0u8; count as usize * bs];
                for (i, sector) in d.chunks_mut(bs).enumerate() {
                    sector.fill(((lba + i as u64) % 251) as u8);
                }
                ok_data(d)
            }
            _ => ok_data(Vec::new()),
        }
    }
}

fn parse_rw(cdb: &[u8]) -> (u64, u32) {
    if cdb[0] == 0x28 {
        (
            u64::from(u32::from_be_bytes([cdb[2], cdb[3], cdb[4], cdb[5]])),
            u32::from(u16::from_be_bytes([cdb[7], cdb[8]])),
        )
    } else {
        (
            u64::from_be_bytes([
                cdb[2], cdb[3], cdb[4], cdb[5], cdb[6], cdb[7], cdb[8], cdb[9],
            ]),
            u32::from_be_bytes([cdb[10], cdb[11], cdb[12], cdb[13]]),
        )
    }
}

struct FakeClock {
    now: Cell<u64>,
}

impl FakeClock {
    fn new() -> Self {
        Self { now: Cell::new(0) }
    }
}

impl Clock for FakeClock {
    fn now_ms(&self) -> u64 {
        self.now.get()
    }
    fn sleep_ms(&self, ms: u32) {
        self.now.set(self.now.get() + u64::from(ms));
    }
}

struct VecBounce(Vec<u8>);

impl Bounce for VecBounce {
    fn bounce(&mut self) -> &mut [u8] {
        &mut self.0
    }
}

fn device(disk: Disk, flags: u32) -> Device<Mock, VecBounce> {
    let mut disk = disk;
    let mock = Mock::new(Box::new(move |cdb: &[u8]| disk.respond(cdb)));
    Device::start(
        mock,
        VecBounce(vec![0; BOUNCE_LEN]),
        0,
        flags,
        &FakeClock::new(),
    )
    .unwrap()
}

// --- SCSI commands ----------------------------------------------------------

#[test]
fn capacity10_is_used_when_it_fits() {
    let mut bot = bot_with({
        let mut disk = Disk::new(2048, 512);
        move |cdb| disk.respond(cdb)
    });
    let cap = scsi::read_capacity(&mut bot).unwrap();
    assert_eq!(cap.last_lba, 2047);
    assert_eq!(cap.block_size, 512);
    assert_eq!(bot.transport_mut().cdbs.len(), 1);
    assert_eq!(bot.transport_mut().cdbs[0][0], 0x25);
}

#[test]
fn capacity16_is_used_when_capacity10_saturates() {
    let big = 1u64 << 33;
    let mut bot = bot_with({
        let mut disk = Disk::new(big, 512);
        disk.saturate10 = true;
        move |cdb| disk.respond(cdb)
    });
    let cap = scsi::read_capacity(&mut bot).unwrap();
    assert_eq!(cap.last_lba, big - 1);
    assert_eq!(cap.block_size, 512);
    let ops: Vec<u8> = bot.transport_mut().cdbs.iter().map(|c| c[0]).collect();
    assert_eq!(ops, vec![0x25, 0x9E]);
}

#[test]
fn sixteen_byte_cdb_only_past_2_pow_32_sectors() {
    assert!(!needs_16(0xFFFF_FFFF, 1));
    assert!(needs_16(0xFFFF_FFFF, 2));
    assert!(needs_16(1 << 32, 1));
    assert!(needs_16(u64::MAX, 1));

    let big = (1u64 << 32) + 16;
    let mut dev = device(Disk::new(big, 512), 0);
    let mut buf = vec![0u8; 512];
    dev.read((1 << 32) + 4, 1, &mut buf).unwrap();
    let last = dev.bot.transport_mut().cdbs.last().unwrap().clone();
    assert_eq!(last[0], 0x88);
    assert_eq!(buf[0], (((1u64 << 32) + 4) % 251) as u8);
}

#[test]
fn inquiry_decodes_identity_and_rejects_other_peripherals() {
    let mut bot = bot_with({
        let mut disk = Disk::new(1, 512);
        move |cdb| disk.respond(cdb)
    });
    let inq = scsi::inquiry(&mut bot).unwrap();
    assert!(inq.removable);
    assert_eq!(&inq.vendor, b"AVORY   ");
    assert_eq!(&inq.revision, b"1.0 ");

    let mut bot = bot_with({
        let mut disk = Disk::new(1, 512);
        disk.periph = 0x05;
        move |cdb| disk.respond(cdb)
    });
    assert_eq!(scsi::inquiry(&mut bot), Err(ScsiError::BadResponse));
}

#[test]
fn write_protect_bit_and_mode_sense_failure() {
    let mut bot = bot_with({
        let mut disk = Disk::new(1, 512);
        disk.read_only = true;
        move |cdb| disk.respond(cdb)
    });
    assert_eq!(scsi::write_protected(&mut bot), Ok(true));

    let dev = device(
        {
            let mut d = Disk::new(64, 512);
            d.mode_fail = true;
            d
        },
        0,
    );
    assert!(!dev.read_only);
}

#[test]
fn sync_cache_treats_illegal_request_as_success() {
    let mut bot = bot_with({
        let mut disk = Disk::new(1, 512);
        disk.sync_key = Some(5);
        move |cdb| disk.respond(cdb)
    });
    assert_eq!(scsi::sync_cache(&mut bot), Ok(()));

    let mut bot = bot_with({
        let mut disk = Disk::new(1, 512);
        disk.sync_key = Some(4); // hardware error
        move |cdb| disk.respond(cdb)
    });
    assert!(matches!(
        scsi::sync_cache(&mut bot),
        Err(ScsiError::Check(s)) if s.key == 4
    ));
}

#[test]
fn test_unit_ready_backs_off_then_succeeds() {
    let mut disk = Disk::new(64, 512);
    disk.tur_fail = 3;
    let mut bot = bot_with(move |cdb| disk.respond(cdb));
    let clock = FakeClock::new();
    scsi::test_unit_ready(&mut bot, &clock).unwrap();
    let turs = bot
        .transport_mut()
        .cdbs
        .iter()
        .filter(|c| c[0] == 0x00)
        .count();
    assert_eq!(turs, 4);
    // Backoff: 100 + 200 + 400 ms.
    assert_eq!(clock.now.get(), 700);
}

#[test]
fn test_unit_ready_gives_up_after_budget() {
    let mut disk = Disk::new(64, 512);
    disk.tur_fail = u32::MAX;
    let mut bot = bot_with(move |cdb| disk.respond(cdb));
    let clock = FakeClock::new();
    let err = scsi::test_unit_ready(&mut bot, &clock);
    assert!(matches!(err, Err(ScsiError::Check(s)) if s.key == 2));
    assert!(clock.now.get() >= scsi::TUR_BUDGET_MS);
}

// --- Device layer -----------------------------------------------------------

#[test]
fn device_start_reads_geometry_and_max_lun() {
    let dev = device(Disk::new(4096, 512), 0);
    assert_eq!(dev.capacity_sectors, 4096);
    assert_eq!(dev.sector_size, 512);
    assert_eq!(dev.max_lun, 0);
    assert!(!dev.read_only);
    assert_eq!(dev.command_sectors(), 240);
}

#[test]
fn get_max_lun_stall_means_lun_zero() {
    let mut disk = Disk::new(64, 512);
    let mock = {
        let mut m = Mock::new(Box::new(move |cdb: &[u8]| disk.respond(cdb)));
        m.max_lun = None;
        m
    };
    let dev = Device::start(
        mock,
        VecBounce(vec![0; BOUNCE_LEN]),
        0,
        0,
        &FakeClock::new(),
    )
    .unwrap();
    assert_eq!(dev.max_lun, 0);
}

#[test]
fn start_rejects_unusable_sector_size() {
    let mut disk = Disk::new(64, 1000);
    let mock = Mock::new(Box::new(move |cdb: &[u8]| disk.respond(cdb)));
    let r = Device::start(
        mock,
        VecBounce(vec![0; BOUNCE_LEN]),
        0,
        0,
        &FakeClock::new(),
    );
    assert!(matches!(r, Err(ScsiError::BadResponse)));
}

#[test]
fn read_splits_at_240_sectors() {
    let mut dev = device(Disk::new(4096, 512), 0);
    let mut buf = vec![0u8; 600 * 512];
    dev.read(0, 600, &mut buf).unwrap();
    let counts: Vec<u16> = dev
        .bot
        .transport_mut()
        .cdbs
        .iter()
        .filter(|c| c[0] == 0x28)
        .map(|c| u16::from_be_bytes([c[7], c[8]]))
        .collect();
    assert_eq!(counts, vec![240, 240, 120]);
    // Each sector carries its own LBA pattern, so the data landed in order.
    assert_eq!(buf[0], 0);
    assert_eq!(buf[240 * 512], 240);
    assert_eq!(buf[599 * 512], 97);
}

#[test]
fn max_sectors_quirk_caps_commands_at_64() {
    let mut dev = device(Disk::new(4096, 512), QUIRK_MAX_SECTORS_64);
    let mut buf = vec![0u8; 130 * 512];
    dev.read(0, 130, &mut buf).unwrap();
    let n = dev
        .bot
        .transport_mut()
        .cdbs
        .iter()
        .filter(|c| c[0] == 0x28)
        .count();
    assert_eq!(n, 3);
}

#[test]
fn large_sectors_fit_the_bounce_buffer() {
    // 4096-byte sectors: 128 KiB holds 32 of them, below the 240 limit.
    let dev = device(Disk::new(1024, 4096), 0);
    assert_eq!(dev.command_sectors(), 32);
}

#[test]
fn write_splits_and_round_trips_through_the_bounce_buffer() {
    let mut dev = device(Disk::new(4096, 512), 0);
    let src: Vec<u8> = (0..300 * 512).map(|i| (i % 199) as u8).collect();
    dev.write(10, 300, &src).unwrap();
    assert_eq!(dev.bot.transport_mut().written, src);
    let counts: Vec<u16> = dev
        .bot
        .transport_mut()
        .cdbs
        .iter()
        .filter(|c| c[0] == 0x2A)
        .map(|c| u16::from_be_bytes([c[7], c[8]]))
        .collect();
    assert_eq!(counts, vec![240, 60]);
}

#[test]
fn read_only_device_refuses_writes_and_ranges_are_checked() {
    let mut disk = Disk::new(64, 512);
    disk.read_only = true;
    let mut dev = device(disk, 0);
    assert_eq!(dev.write(0, 1, &[0u8; 512]), Err(ScsiError::ReadOnly));
    let mut buf = [0u8; 512];
    assert_eq!(dev.read(64, 1, &mut buf), Err(ScsiError::Range));
    assert_eq!(dev.read(0, 2, &mut buf), Err(ScsiError::Range));
}

#[test]
fn flush_skips_sync_cache_when_quirked() {
    let mut dev = device(Disk::new(64, 512), QUIRK_NO_SYNC_CACHE);
    dev.flush().unwrap();
    assert!(!dev.bot.transport_mut().cdbs.iter().any(|c| c[0] == 0x35));

    let mut dev = device(Disk::new(64, 512), 0);
    dev.flush().unwrap();
    assert!(dev.bot.transport_mut().cdbs.iter().any(|c| c[0] == 0x35));
}

#[test]
fn quirk_table_is_empty_and_flags_combine() {
    assert_eq!(quirk_flags(0x1234, 0x5678), 0);
}

#[test]
fn csw_size_matches_bot_spec() {
    assert_eq!(CBW_SIGNATURE, 0x4342_5355);
    assert_eq!(CSW_LEN, 13);
}
