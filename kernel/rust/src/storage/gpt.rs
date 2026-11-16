use core::slice;

const GPT_SIGNATURE: [u8; 8] = *b"EFI PART";
const GPT_REVISION_1_0: u32 = 0x0001_0000;
const GPT_MIN_HEADER_SIZE: usize = 92;
const GPT_ENTRY_MIN_SIZE: usize = 128;
const GPT_MAX_PARTITIONS: u32 = 128;
const GPT_MAX_TABLE_BYTES: u64 = 256 * 1024;

const PART_TYPE_UNKNOWN: u8 = 0;
const PART_TYPE_ESP: u8 = 1;
const PART_TYPE_LINUX_ROOT: u8 = 2;
const PART_TYPE_LINUX_GENERIC: u8 = 3;
const PART_TYPE_LINUX_SWAP: u8 = 4;
const PART_TYPE_BASIC_DATA: u8 = 5;

const GUID_ESP: [u8; 16] = [
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b,
];
const GUID_LINUX_ROOT_X86_64: [u8; 16] = [
    0xe3, 0xbc, 0x68, 0x4f, 0xcd, 0xe8, 0xb1, 0x4d, 0x96, 0xe7, 0xfb, 0xca, 0xf9, 0x84, 0xb7, 0x09,
];
const GUID_LINUX_GENERIC: [u8; 16] = [
    0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47, 0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4,
];
const GUID_LINUX_SWAP: [u8; 16] = [
    0x6d, 0xfd, 0x57, 0x06, 0xab, 0xa4, 0xc4, 0x43, 0x84, 0xe5, 0x09, 0x33, 0xc8, 0x4b, 0x4f, 0x4f,
];
const GUID_BASIC_DATA: [u8; 16] = [
    0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44, 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7,
];

const CRC32_TABLE: [u32; 256] = {
    let mut tab = [0u32; 256];
    let mut i = 0;
    while i < 256 {
        let mut c = i as u32;
        let mut j = 0;
        while j < 8 {
            if (c & 1) != 0 {
                c = 0xEDB8_8320 ^ (c >> 1);
            } else {
                c >>= 1;
            }
            j += 1;
        }
        tab[i] = c;
        i += 1;
    }
    tab
};

#[inline]
fn crc32_update(mut crc: u32, data: &[u8]) -> u32 {
    for &b in data {
        crc = CRC32_TABLE[((crc ^ (b as u32)) & 0xff) as usize] ^ (crc >> 8);
    }
    crc
}

pub fn crc32(data: &[u8]) -> u32 {
    crc32_update(0xFFFF_FFFF, data) ^ 0xFFFF_FFFF
}

fn gpt_header_crc32(hdr_slice: &[u8]) -> u32 {
    // Zero bytes 16..20 (header_crc32) without copying onto the stack.
    let mut crc = 0xFFFF_FFFFu32;
    crc = crc32_update(crc, &hdr_slice[0..16]);
    crc = crc32_update(crc, &[0, 0, 0, 0]);
    crc = crc32_update(crc, &hdr_slice[20..]);
    crc ^ 0xFFFF_FFFF
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct GptHeaderC {
    pub revision: u32,
    pub header_size: u32,
    pub current_lba: u64,
    pub first_usable_lba: u64,
    pub last_usable_lba: u64,
    pub partition_entry_lba: u64,
    pub num_partition_entries: u32,
    pub partition_entry_size: u32,
    pub partition_array_crc32: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GptEntryParsedC {
    pub starting_lba: u64,
    pub ending_lba: u64,
    pub total_sectors: u64,
    pub type_guid: [u8; 16],
    pub partition_type: u8,
    pub partuuid: [u8; 37],
    pub partlabel: [u8; 64],
}

#[inline]
fn read_le32(b: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
}

#[inline]
fn read_le64(b: &[u8], off: usize) -> u64 {
    u64::from_le_bytes([
        b[off],
        b[off + 1],
        b[off + 2],
        b[off + 3],
        b[off + 4],
        b[off + 5],
        b[off + 6],
        b[off + 7],
    ])
}

pub fn validate_gpt_header(
    sector: &[u8],
    expected_lba: u64,
    total_sectors: u64,
) -> Option<GptHeaderC> {
    if sector.len() < GPT_MIN_HEADER_SIZE || sector[0..8] != GPT_SIGNATURE {
        return None;
    }

    let revision = read_le32(sector, 8);
    if revision != GPT_REVISION_1_0 {
        return None;
    }

    let header_size = read_le32(sector, 12) as usize;
    if header_size < GPT_MIN_HEADER_SIZE || header_size > sector.len() {
        return None;
    }

    let header_crc32 = read_le32(sector, 16);
    let current_lba = read_le64(sector, 24);
    if current_lba != expected_lba {
        return None;
    }

    let first_usable_lba = read_le64(sector, 40);
    let last_usable_lba = read_le64(sector, 48);
    if first_usable_lba > last_usable_lba || last_usable_lba >= total_sectors {
        return None;
    }

    let partition_entry_lba = read_le64(sector, 72);
    let num_partition_entries = read_le32(sector, 80);
    let partition_entry_size = read_le32(sector, 84);
    let partition_array_crc32 = read_le32(sector, 88);

    if num_partition_entries == 0 || num_partition_entries > GPT_MAX_PARTITIONS {
        return None;
    }
    if (partition_entry_size as usize) < GPT_ENTRY_MIN_SIZE || (partition_entry_size % 8) != 0 {
        return None;
    }

    let total_table_bytes = (num_partition_entries as u64) * (partition_entry_size as u64);
    if total_table_bytes > GPT_MAX_TABLE_BYTES {
        return None;
    }

    if gpt_header_crc32(&sector[..header_size]) != header_crc32 {
        return None;
    }

    Some(GptHeaderC {
        revision,
        header_size: header_size as u32,
        current_lba,
        first_usable_lba,
        last_usable_lba,
        partition_entry_lba,
        num_partition_entries,
        partition_entry_size,
        partition_array_crc32,
    })
}

fn classify_type_guid(guid: &[u8; 16]) -> u8 {
    if *guid == GUID_LINUX_ROOT_X86_64 {
        PART_TYPE_LINUX_ROOT
    } else if *guid == GUID_ESP {
        PART_TYPE_ESP
    } else if *guid == GUID_LINUX_GENERIC {
        PART_TYPE_LINUX_GENERIC
    } else if *guid == GUID_LINUX_SWAP {
        PART_TYPE_LINUX_SWAP
    } else if *guid == GUID_BASIC_DATA {
        PART_TYPE_BASIC_DATA
    } else {
        PART_TYPE_UNKNOWN
    }
}

fn write_hex_byte(dst: &mut [u8], pos: usize, b: u8) {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    dst[pos] = HEX[(b >> 4) as usize];
    dst[pos + 1] = HEX[(b & 0x0f) as usize];
}

fn format_guid_str(guid: &[u8; 16], out: &mut [u8; 37]) {
    let mut p = 0;
    for &idx in &[3usize, 2, 1, 0] {
        write_hex_byte(out, p, guid[idx]);
        p += 2;
    }
    out[p] = b'-';
    p += 1;
    for &idx in &[5usize, 4] {
        write_hex_byte(out, p, guid[idx]);
        p += 2;
    }
    out[p] = b'-';
    p += 1;
    for &idx in &[7usize, 6] {
        write_hex_byte(out, p, guid[idx]);
        p += 2;
    }
    out[p] = b'-';
    p += 1;
    for &idx in &[8usize, 9] {
        write_hex_byte(out, p, guid[idx]);
        p += 2;
    }
    out[p] = b'-';
    p += 1;
    for idx in 10..16 {
        write_hex_byte(out, p, guid[idx]);
        p += 2;
    }
    out[36] = 0;
}

fn decode_utf16le_label(raw: &[u8], out: &mut [u8; 64]) {
    let mut pos = 0usize;
    for chunk in raw.chunks_exact(2) {
        let ch = u16::from_le_bytes([chunk[0], chunk[1]]);
        if ch == 0 {
            break;
        }
        if ch < 0x80 {
            if pos + 1 >= out.len() {
                break;
            }
            out[pos] = ch as u8;
            pos += 1;
        } else if ch < 0x800 {
            if pos + 2 >= out.len() {
                break;
            }
            out[pos] = (0xC0 | (ch >> 6)) as u8;
            out[pos + 1] = (0x80 | (ch & 0x3F)) as u8;
            pos += 2;
        } else {
            if pos + 3 >= out.len() {
                break;
            }
            out[pos] = (0xE0 | (ch >> 12)) as u8;
            out[pos + 1] = (0x80 | ((ch >> 6) & 0x3F)) as u8;
            out[pos + 2] = (0x80 | (ch & 0x3F)) as u8;
            pos += 3;
        }
    }
    out[pos] = 0;
}

pub fn parse_gpt_entry(
    entry: &[u8],
    first_usable_lba: u64,
    last_usable_lba: u64,
) -> Option<GptEntryParsedC> {
    if entry.len() < GPT_ENTRY_MIN_SIZE {
        return None;
    }
    let mut type_guid = [0u8; 16];
    type_guid.copy_from_slice(&entry[0..16]);
    if type_guid == [0u8; 16] {
        return None;
    }

    let starting_lba = read_le64(entry, 32);
    let ending_lba = read_le64(entry, 40);
    if starting_lba < first_usable_lba
        || ending_lba > last_usable_lba
        || starting_lba > ending_lba
    {
        return None;
    }

    let mut unique_guid = [0u8; 16];
    unique_guid.copy_from_slice(&entry[16..32]);

    let mut parsed = GptEntryParsedC {
        starting_lba,
        ending_lba,
        total_sectors: (ending_lba - starting_lba) + 1,
        type_guid,
        partition_type: classify_type_guid(&type_guid),
        partuuid: [0u8; 37],
        partlabel: [0u8; 64],
    };
    format_guid_str(&unique_guid, &mut parsed.partuuid);
    decode_utf16le_label(&entry[56..128], &mut parsed.partlabel);
    Some(parsed)
}

#[no_mangle]
pub unsafe extern "C" fn rust_gpt_validate_header(
    sector: *const u8,
    sector_len: usize,
    expected_lba: u64,
    total_sectors: u64,
    out_hdr: *mut GptHeaderC,
) -> bool {
    if sector.is_null() || out_hdr.is_null() || sector_len < GPT_MIN_HEADER_SIZE {
        return false;
    }
    let bytes = unsafe { slice::from_raw_parts(sector, sector_len) };
    match validate_gpt_header(bytes, expected_lba, total_sectors) {
        Some(hdr) => {
            unsafe {
                *out_hdr = hdr;
            }
            true
        }
        None => false,
    }
}

#[no_mangle]
pub unsafe extern "C" fn rust_gpt_crc32_matches(
    buf: *const u8,
    len: usize,
    expected_crc: u32,
) -> bool {
    if buf.is_null() {
        return false;
    }
    let bytes = unsafe { slice::from_raw_parts(buf, len) };
    crc32(bytes) == expected_crc
}

#[no_mangle]
pub unsafe extern "C" fn rust_gpt_parse_entry(
    entry: *const u8,
    entry_len: usize,
    first_usable: u64,
    last_usable: u64,
    out: *mut GptEntryParsedC,
) -> bool {
    if entry.is_null() || out.is_null() || entry_len < GPT_ENTRY_MIN_SIZE {
        return false;
    }
    let bytes = unsafe { slice::from_raw_parts(entry, entry_len) };
    match parse_gpt_entry(bytes, first_usable, last_usable) {
        Some(parsed) => {
            unsafe {
                *out = parsed;
            }
            true
        }
        None => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn validates_gpt_header_including_large_header_size() {
        let mut sec = [0u8; 512];
        sec[0..8].copy_from_slice(&GPT_SIGNATURE);
        sec[8..12].copy_from_slice(&GPT_REVISION_1_0.to_le_bytes());
        sec[12..16].copy_from_slice(&512u32.to_le_bytes()); // full sector header_size
        sec[24..32].copy_from_slice(&1u64.to_le_bytes());
        sec[40..48].copy_from_slice(&34u64.to_le_bytes());
        sec[48..56].copy_from_slice(&999u64.to_le_bytes());
        sec[72..80].copy_from_slice(&2u64.to_le_bytes());
        sec[80..84].copy_from_slice(&128u32.to_le_bytes());
        sec[84..88].copy_from_slice(&128u32.to_le_bytes());
        let c = gpt_header_crc32(&sec);
        sec[16..20].copy_from_slice(&c.to_le_bytes());

        let hdr = validate_gpt_header(&sec, 1, 1000).expect("valid header");
        assert_eq!(hdr.header_size, 512);
        assert_eq!(hdr.num_partition_entries, 128);

        sec[0] ^= 0xff;
        assert!(validate_gpt_header(&sec, 1, 1000).is_none());
    }

    #[test]
    fn parses_gpt_entry_guid_and_utf16_label() {
        let mut ent = [0u8; 128];
        ent[0..16].copy_from_slice(&GUID_LINUX_ROOT_X86_64);
        ent[16..32].copy_from_slice(&GUID_ESP);
        ent[32..40].copy_from_slice(&2048u64.to_le_bytes());
        ent[40..48].copy_from_slice(&4095u64.to_le_bytes());
        // "root" in UTF-16LE
        ent[56..64].copy_from_slice(&[b'r', 0, b'o', 0, b'o', 0, b't', 0]);

        let parsed = parse_gpt_entry(&ent, 34, 10000).expect("valid entry");
        assert_eq!(parsed.partition_type, PART_TYPE_LINUX_ROOT);
        assert_eq!(parsed.total_sectors, 2048);
        assert_eq!(
            core::str::from_utf8(&parsed.partuuid[..36]).unwrap(),
            "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"
        );
        assert_eq!(core::str::from_utf8(&parsed.partlabel[..4]).unwrap(), "root");
    }
}
