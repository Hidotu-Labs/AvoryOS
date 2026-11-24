//! SCSI transparent commands over the BOT engine: the subset a disk needs.
//!
//! SCSI multi-byte fields are big-endian (unlike the little-endian CBW/CSW).
//! Every command that fails with CSW status Failed issues REQUEST SENSE first.

use super::bot::{Bot, BulkTransport, Completion, Dir, Outcome};
use crate::usb::kernel::Clock;

pub const OP_TEST_UNIT_READY: u8 = 0x00;
pub const OP_REQUEST_SENSE: u8 = 0x03;
pub const OP_INQUIRY: u8 = 0x12;
pub const OP_MODE_SENSE6: u8 = 0x1A;
pub const OP_START_STOP_UNIT: u8 = 0x1B;
pub const OP_PREVENT_ALLOW_REMOVAL: u8 = 0x1E;
pub const OP_READ_CAPACITY10: u8 = 0x25;
pub const OP_READ10: u8 = 0x28;
pub const OP_WRITE10: u8 = 0x2A;
pub const OP_SYNCHRONIZE_CACHE10: u8 = 0x35;
pub const OP_READ16: u8 = 0x88;
pub const OP_WRITE16: u8 = 0x8A;
pub const OP_READ_CAPACITY16: u8 = 0x9E;
/// Service action for READ CAPACITY(16).
const SA_READ_CAPACITY16: u8 = 0x10;

const INQUIRY_LEN: usize = 36;
const SENSE_LEN: usize = 18;
const MODE_SENSE_LEN: usize = 4;
/// Mode page 0x3F: all pages, so the header carries the write-protect bit.
const MODE_PAGE_ALL: u8 = 0x3F;
const WRITE_PROTECT_BIT: u8 = 0x80;

/// How long TEST UNIT READY keeps retrying NOT READY / UNIT ATTENTION.
pub const TUR_BUDGET_MS: u64 = 10_000;
const TUR_BACKOFF_MAX_MS: u32 = 1_000;

const SENSE_KEY_NOT_READY: u8 = 0x2;
const SENSE_KEY_ILLEGAL_REQUEST: u8 = 0x5;
const SENSE_KEY_UNIT_ATTENTION: u8 = 0x6;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Sense {
    pub key: u8,
    pub asc: u8,
    pub ascq: u8,
}

impl Sense {
    /// "No sense" placeholder for a REQUEST SENSE that returned nothing usable.
    pub const NONE: Sense = Sense {
        key: 0,
        asc: 0,
        ascq: 0,
    };

    /// Decode fixed-format sense data (response codes 0x70 and 0x71).
    pub fn decode(raw: &[u8]) -> Option<Self> {
        let code = raw.first()? & 0x7F;
        if code != 0x70 && code != 0x71 {
            return None;
        }
        Some(Self {
            key: raw.get(2).copied().unwrap_or(0) & 0x0F,
            asc: raw.get(12).copied().unwrap_or(0),
            ascq: raw.get(13).copied().unwrap_or(0),
        })
    }

    pub fn is_spin_up(&self) -> bool {
        self.key == SENSE_KEY_NOT_READY || self.key == SENSE_KEY_UNIT_ATTENTION
    }

    pub fn is_illegal_request(&self) -> bool {
        self.key == SENSE_KEY_ILLEGAL_REQUEST
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ScsiError {
    /// The transport failed; the outcome says how.
    Transport(Outcome),
    /// CHECK CONDITION with the sense data that came back.
    Check(Sense),
    /// Response shorter or otherwise unusable.
    BadResponse,
    /// Sector range outside the device, or a buffer of the wrong size.
    Range,
    ReadOnly,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Inquiry {
    pub peripheral_type: u8,
    pub removable: bool,
    pub vendor: [u8; 8],
    pub product: [u8; 16],
    pub revision: [u8; 4],
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Capacity {
    pub last_lba: u64,
    pub block_size: u32,
}

fn byte(b: &[u8], at: usize) -> u8 {
    b.get(at).copied().unwrap_or(0)
}

fn array<const N: usize>(b: &[u8], at: usize) -> Option<[u8; N]> {
    b.get(at..at + N)?.try_into().ok()
}

fn be32(b: &[u8], at: usize) -> u32 {
    array::<4>(b, at).map_or(0, u32::from_be_bytes)
}

fn be64(b: &[u8], at: usize) -> u64 {
    array::<8>(b, at).map_or(0, u64::from_be_bytes)
}

/// Run a command and turn its CSW into a result.  Valid bytes are returned
/// on success.  A Failed CSW triggers REQUEST SENSE.
pub fn run<T: BulkTransport>(
    bot: &mut Bot<T>,
    cdb: &[u8],
    dir: Dir,
    data: &mut [u8],
) -> Result<usize, ScsiError> {
    let len = data.len();
    let Completion { outcome, residue } = bot.command(cdb, dir, data);
    match outcome {
        Outcome::Passed => {
            let residue = usize::try_from(residue).unwrap_or(usize::MAX);
            Ok(len.saturating_sub(residue))
        }
        Outcome::Failed => Err(ScsiError::Check(request_sense(bot)?)),
        other => Err(ScsiError::Transport(other)),
    }
}

/// REQUEST SENSE.  Records the result in the BOT statistics.
pub fn request_sense<T: BulkTransport>(bot: &mut Bot<T>) -> Result<Sense, ScsiError> {
    let cdb = [OP_REQUEST_SENSE, 0, 0, 0, SENSE_LEN as u8, 0];
    let mut buf = [0u8; SENSE_LEN];
    match bot.command(&cdb, Dir::In, &mut buf).outcome {
        Outcome::Passed => {}
        Outcome::Failed => return Err(ScsiError::BadResponse),
        other => return Err(ScsiError::Transport(other)),
    }
    let sense = Sense::decode(&buf).unwrap_or(Sense::NONE);
    bot.stats.last_sense = Some(sense);
    Ok(sense)
}

pub fn inquiry<T: BulkTransport>(bot: &mut Bot<T>) -> Result<Inquiry, ScsiError> {
    let cdb = [OP_INQUIRY, 0, 0, 0, INQUIRY_LEN as u8, 0];
    let mut buf = [0u8; INQUIRY_LEN];
    if run(bot, &cdb, Dir::In, &mut buf)? < INQUIRY_LEN {
        return Err(ScsiError::BadResponse);
    }
    let peripheral_type = byte(&buf, 0) & 0x1F;
    // Direct-access block device, or reduced block commands.
    if peripheral_type != 0x00 && peripheral_type != 0x0E {
        return Err(ScsiError::BadResponse);
    }
    Ok(Inquiry {
        peripheral_type,
        removable: byte(&buf, 1) & 0x80 != 0,
        vendor: array::<8>(&buf, 8).ok_or(ScsiError::BadResponse)?,
        product: array::<16>(&buf, 16).ok_or(ScsiError::BadResponse)?,
        revision: array::<4>(&buf, 32).ok_or(ScsiError::BadResponse)?,
    })
}

/// TEST UNIT READY, retried with backoff while the unit is spinning up (NOT
/// READY or UNIT ATTENTION), for up to [`TUR_BUDGET_MS`].
pub fn test_unit_ready<T: BulkTransport, C: Clock>(
    bot: &mut Bot<T>,
    clock: &C,
) -> Result<(), ScsiError> {
    let cdb = [OP_TEST_UNIT_READY, 0, 0, 0, 0, 0];
    let start = clock.now_ms();
    let mut delay_ms = 100u32;
    loop {
        match run(bot, &cdb, Dir::None, &mut []) {
            Ok(_) => return Ok(()),
            Err(ScsiError::Check(sense)) if sense.is_spin_up() => {
                if clock.now_ms().saturating_sub(start) >= TUR_BUDGET_MS {
                    return Err(ScsiError::Check(sense));
                }
                clock.sleep_ms(delay_ms);
                delay_ms = (delay_ms * 2).min(TUR_BACKOFF_MAX_MS);
            }
            Err(e) => return Err(e),
        }
    }
}

/// READ CAPACITY(10), falling back to (16) when the (10) answer saturates.
pub fn read_capacity<T: BulkTransport>(bot: &mut Bot<T>) -> Result<Capacity, ScsiError> {
    let cdb = [OP_READ_CAPACITY10, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    let mut buf = [0u8; 8];
    if run(bot, &cdb, Dir::In, &mut buf)? < 8 {
        return Err(ScsiError::BadResponse);
    }
    let last = be32(&buf, 0);
    if last != u32::MAX {
        return Ok(Capacity {
            last_lba: u64::from(last),
            block_size: be32(&buf, 4),
        });
    }
    read_capacity16(bot)
}

fn read_capacity16<T: BulkTransport>(bot: &mut Bot<T>) -> Result<Capacity, ScsiError> {
    // Allocation length is bytes 10..14; 32 is the last byte of that field.
    let cdb = [
        OP_READ_CAPACITY16,
        SA_READ_CAPACITY16,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        32,
        0,
        0,
    ];
    let mut buf = [0u8; 32];
    if run(bot, &cdb, Dir::In, &mut buf)? < 12 {
        return Err(ScsiError::BadResponse);
    }
    Ok(Capacity {
        last_lba: be64(&buf, 0),
        block_size: be32(&buf, 8),
    })
}

/// True when a request reaches past what READ/WRITE(10) can address.
pub fn needs_16(lba: u64, count: u32) -> bool {
    lba.checked_add(u64::from(count))
        .is_none_or(|end| end > 1u64 << 32)
}

fn rw10(op: u8, lba: u64, count: u32) -> [u8; 10] {
    let [l0, l1, l2, l3] = u32::try_from(lba).unwrap_or(u32::MAX).to_be_bytes();
    let [c0, c1] = u16::try_from(count).unwrap_or(u16::MAX).to_be_bytes();
    [op, 0, l0, l1, l2, l3, 0, c0, c1, 0]
}

fn rw16(op: u8, lba: u64, count: u32) -> [u8; 16] {
    let [a0, a1, a2, a3, a4, a5, a6, a7] = lba.to_be_bytes();
    let [c0, c1, c2, c3] = count.to_be_bytes();
    [op, 0, a0, a1, a2, a3, a4, a5, a6, a7, c0, c1, c2, c3, 0, 0]
}

/// READ `count` blocks at `lba` into `buf` (`buf.len()` is `count` blocks).
pub fn read_blocks<T: BulkTransport>(
    bot: &mut Bot<T>,
    lba: u64,
    count: u32,
    buf: &mut [u8],
) -> Result<(), ScsiError> {
    let moved = transfer(bot, lba, count, Dir::In, buf, OP_READ10, OP_READ16)?;
    if moved < buf.len() {
        return Err(ScsiError::BadResponse);
    }
    Ok(())
}

/// WRITE `count` blocks at `lba` from `buf`.
pub fn write_blocks<T: BulkTransport>(
    bot: &mut Bot<T>,
    lba: u64,
    count: u32,
    buf: &mut [u8],
) -> Result<(), ScsiError> {
    let moved = transfer(bot, lba, count, Dir::Out, buf, OP_WRITE10, OP_WRITE16)?;
    if moved < buf.len() {
        return Err(ScsiError::BadResponse);
    }
    Ok(())
}

fn transfer<T: BulkTransport>(
    bot: &mut Bot<T>,
    lba: u64,
    count: u32,
    dir: Dir,
    buf: &mut [u8],
    op10: u8,
    op16: u8,
) -> Result<usize, ScsiError> {
    if needs_16(lba, count) {
        run(bot, &rw16(op16, lba, count), dir, buf)
    } else {
        run(bot, &rw10(op10, lba, count), dir, buf)
    }
}

/// SYNCHRONIZE CACHE(10).  ILLEGAL REQUEST means the device has no cache to
/// flush, which counts as success.
pub fn sync_cache<T: BulkTransport>(bot: &mut Bot<T>) -> Result<(), ScsiError> {
    let cdb = [OP_SYNCHRONIZE_CACHE10, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    match run(bot, &cdb, Dir::None, &mut []) {
        Ok(_) => Ok(()),
        Err(ScsiError::Check(sense)) if sense.is_illegal_request() => Ok(()),
        Err(e) => Err(e),
    }
}

/// MODE SENSE(6), all pages: true when the write-protect bit is set.
pub fn write_protected<T: BulkTransport>(bot: &mut Bot<T>) -> Result<bool, ScsiError> {
    let cdb = [OP_MODE_SENSE6, 0, MODE_PAGE_ALL, 0, MODE_SENSE_LEN as u8, 0];
    let mut buf = [0u8; MODE_SENSE_LEN];
    if run(bot, &cdb, Dir::In, &mut buf)? < 3 {
        return Err(ScsiError::BadResponse);
    }
    Ok(byte(&buf, 2) & WRITE_PROTECT_BIT != 0)
}

/// START STOP UNIT.  `load_eject` with `start == false` ejects the medium.
pub fn start_stop<T: BulkTransport>(
    bot: &mut Bot<T>,
    start: bool,
    load_eject: bool,
) -> Result<(), ScsiError> {
    let control = u8::from(start) | (u8::from(load_eject) << 1);
    let cdb = [OP_START_STOP_UNIT, 0, 0, 0, control, 0];
    run(bot, &cdb, Dir::None, &mut []).map(|_| ())
}

/// PREVENT ALLOW MEDIUM REMOVAL.
pub fn prevent_removal<T: BulkTransport>(bot: &mut Bot<T>, prevent: bool) -> Result<(), ScsiError> {
    let cdb = [OP_PREVENT_ALLOW_REMOVAL, 0, 0, 0, u8::from(prevent), 0];
    run(bot, &cdb, Dir::None, &mut []).map(|_| ())
}
