use core::slice;

const MBR_SECTOR_MIN_LEN: usize = 512;
const MBR_TABLE_OFFSET: usize = 446;
const MBR_ENTRY_LEN: usize = 16;
const MBR_PARTITION_COUNT: usize = 4;
const MBR_SIGNATURE: u16 = 0xAA55;

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct MbrPartitionC {
    pub start_lba: u32,
    pub total_sectors: u32,
    pub part_type: u8,
    pub active: bool,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct MbrParseResultC {
    pub valid: bool,
    pub is_pmbr: bool,
    pub is_hybrid_mbr: bool,
    pub count: usize,
    pub partitions: [MbrPartitionC; MBR_PARTITION_COUNT],
}

pub fn parse_mbr_sector(sector: &[u8]) -> MbrParseResultC {
    let mut out = MbrParseResultC::default();
    if sector.len() < MBR_SECTOR_MIN_LEN {
        return out;
    }

    let sig = u16::from_le_bytes([sector[510], sector[511]]);
    if sig != MBR_SIGNATURE {
        return out;
    }

    out.valid = true;
    let mut has_ee = false;
    let mut has_other = false;

    for i in 0..MBR_PARTITION_COUNT {
        let off = MBR_TABLE_OFFSET + i * MBR_ENTRY_LEN;
        let entry = &sector[off..off + MBR_ENTRY_LEN];
        let part_type = entry[4];
        let start_lba = u32::from_le_bytes([entry[8], entry[9], entry[10], entry[11]]);
        let total_sectors = u32::from_le_bytes([entry[12], entry[13], entry[14], entry[15]]);

        if part_type == 0xEE {
            has_ee = true;
        } else if part_type != 0x00 {
            has_other = true;
        }

        out.partitions[i] = MbrPartitionC {
            start_lba,
            total_sectors,
            part_type,
            active: (entry[0] & 0x80) != 0,
        };
        if part_type != 0 && total_sectors != 0 {
            out.count += 1;
        }
    }

    out.is_pmbr = has_ee && !has_other;
    out.is_hybrid_mbr = has_ee && has_other;
    out
}

#[no_mangle]
pub unsafe extern "C" fn rust_mbr_parse_sector(
    sector: *const u8,
    len: usize,
    out: *mut MbrParseResultC,
) -> bool {
    if sector.is_null() || out.is_null() || len < MBR_SECTOR_MIN_LEN {
        return false;
    }
    let bytes = unsafe { slice::from_raw_parts(sector, len) };
    let parsed = parse_mbr_sector(bytes);
    unsafe {
        *out = parsed;
    }
    parsed.valid
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_valid_mbr_and_detects_pmbr_hybrid() {
        let mut sec = [0u8; 512];
        sec[510] = 0x55;
        sec[511] = 0xAA;
        // Entry 0: Linux partition at LBA 2048, 4096 sectors
        sec[446] = 0x80;
        sec[446 + 4] = 0x83;
        sec[446 + 8..446 + 12].copy_from_slice(&2048u32.to_le_bytes());
        sec[446 + 12..446 + 16].copy_from_slice(&4096u32.to_le_bytes());

        let res = parse_mbr_sector(&sec);
        assert!(res.valid);
        assert!(!res.is_pmbr);
        assert!(!res.is_hybrid_mbr);
        assert_eq!(res.count, 1);
        assert_eq!(res.partitions[0].start_lba, 2048);
        assert_eq!(res.partitions[0].total_sectors, 4096);
        assert!(res.partitions[0].active);

        // Add 0xEE entry -> hybrid MBR
        sec[446 + 16 + 4] = 0xEE;
        let hyb = parse_mbr_sector(&sec);
        assert!(hyb.is_hybrid_mbr);
        assert!(!hyb.is_pmbr);
    }

    #[test]
    fn rejects_bad_signature_or_short_sector() {
        let sec = [0u8; 512];
        assert!(!parse_mbr_sector(&sec).valid);
        assert!(!parse_mbr_sector(&sec[..100]).valid);
    }
}
