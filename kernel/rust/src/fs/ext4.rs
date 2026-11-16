use core::slice;

const EXT4_EXT_MAGIC: u16 = 0xF30A;
const EXT4_EXT_HDR_LEN: usize = 12;
const EXT4_EXT_ENTRY_LEN: usize = 12;
const EXT4_MAX_PHYS_BLOCK: u64 = (1u64 << 48) - 1;

const EXT4_FEATURE_INCOMPAT_FILETYPE: u32 = 0x0002;
const EXT4_FEATURE_INCOMPAT_RECOVER: u32 = 0x0004;
const EXT4_FEATURE_INCOMPAT_EXTENTS: u32 = 0x0040;
const EXT4_FEATURE_INCOMPAT_64BIT: u32 = 0x0080;

const EXT4_FEATURE_COMPAT_HAS_JOURNAL: u32 = 0x0004;

const EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER: u32 = 0x0001;
const EXT4_FEATURE_RO_COMPAT_LARGE_FILE: u32 = 0x0002;
const EXT4_FEATURE_RO_COMPAT_BTREE_DIR: u32 = 0x0004;

pub const EXT4_LOOKUP_ERR: i32 = 0;
pub const EXT4_LOOKUP_HIT: i32 = 1;
pub const EXT4_LOOKUP_HOLE: i32 = 2;
pub const EXT4_LOOKUP_DESCEND: i32 = 3;

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Ext4ExtentLookupC {
    pub pblock: u64,
    pub child_phys: u64,
    pub run_len: u32,
    pub hole_limit: u32,
}

#[inline]
fn le16(b: &[u8], off: usize) -> u16 {
    u16::from_le_bytes([b[off], b[off + 1]])
}

#[inline]
fn le32(b: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
}

pub fn check_feature_flags(
    log_block_size: u32,
    desc_size: u16,
    compat: u32,
    incompat: u32,
    ro_compat: u32,
    require_extents: bool,
) -> Option<bool> {
    if log_block_size > 6 {
        return None;
    }

    let supported_incompat = EXT4_FEATURE_INCOMPAT_FILETYPE
        | EXT4_FEATURE_INCOMPAT_RECOVER
        | EXT4_FEATURE_INCOMPAT_EXTENTS
        | EXT4_FEATURE_INCOMPAT_64BIT;
    if (incompat & !supported_incompat) != 0 {
        return None;
    }
    if require_extents && (incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) == 0 {
        return None;
    }
    if (incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0 {
        let block_size = 1024u32 << log_block_size;
        if (desc_size as u32) < 64 || (desc_size as u32) > block_size || (desc_size & 7) != 0 {
            return None;
        }
    }
    if (incompat & EXT4_FEATURE_INCOMPAT_RECOVER) != 0
        && (compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) == 0
    {
        return None;
    }

    let supported_ro_compat = EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER
        | EXT4_FEATURE_RO_COMPAT_LARGE_FILE
        | EXT4_FEATURE_RO_COMPAT_BTREE_DIR;
    Some((ro_compat & !supported_ro_compat) != 0)
}

pub fn validate_extent_header(node: &[u8], max_depth: u16) -> Option<(u16, u16)> {
    if node.len() < EXT4_EXT_HDR_LEN {
        return None;
    }
    let magic = le16(node, 0);
    let entries = le16(node, 2);
    let max_entries = le16(node, 4);
    let depth = le16(node, 6);

    if magic != EXT4_EXT_MAGIC || depth > max_depth || entries > max_entries {
        return None;
    }

    let needed = EXT4_EXT_HDR_LEN.checked_add((entries as usize).checked_mul(EXT4_EXT_ENTRY_LEN)?)?;
    if needed > node.len() {
        return None;
    }

    Some((entries, depth))
}

pub fn lookup_extent_node(
    node: &[u8],
    logical_block: u32,
    blocks_count: u64,
    max_depth: u16,
    mut hole_limit: u32,
) -> (i32, Ext4ExtentLookupC) {
    let mut out = Ext4ExtentLookupC {
        hole_limit,
        ..Default::default()
    };

    let Some((entries, depth)) = validate_extent_header(node, max_depth) else {
        return (EXT4_LOOKUP_ERR, out);
    };

    if depth == 0 {
        for i in 0..(entries as usize) {
            let off = EXT4_EXT_HDR_LEN + i * EXT4_EXT_ENTRY_LEN;
            let ee_block = le32(node, off);
            let ee_len_raw = le16(node, off + 4);
            let len = (ee_len_raw & 0x7FFF) as u32;

            if logical_block < ee_block {
                out.run_len = ee_block - logical_block;
                return (EXT4_LOOKUP_HOLE, out);
            }

            if let Some(end_block) = ee_block.checked_add(len) {
                if logical_block >= ee_block && logical_block < end_block {
                    let offset_in_extent = logical_block - ee_block;
                    out.run_len = len - offset_in_extent;
                    if ee_len_raw > 0x8000 {
                        return (EXT4_LOOKUP_ERR, out);
                    }
                    let start_hi = le16(node, off + 6) as u64;
                    let start_lo = le32(node, off + 8) as u64;
                    let pblock = (start_hi << 32) | start_lo;
                    if pblock == 0
                        || pblock > EXT4_MAX_PHYS_BLOCK
                        || pblock >= blocks_count
                        || (offset_in_extent as u64) >= blocks_count - pblock
                    {
                        return (EXT4_LOOKUP_ERR, out);
                    }
                    out.pblock = pblock + (offset_in_extent as u64);
                    return (EXT4_LOOKUP_HIT, out);
                }
            }
        }
        out.run_len = hole_limit;
        return (EXT4_LOOKUP_HOLE, out);
    }

    let mut best_phys: Option<u64> = None;
    for i in 0..(entries as usize) {
        let off = EXT4_EXT_HDR_LEN + i * EXT4_EXT_ENTRY_LEN;
        let ei_block = le32(node, off);
        if ei_block <= logical_block {
            let leaf_lo = le32(node, off + 4) as u64;
            let leaf_hi = le16(node, off + 8) as u64;
            best_phys = Some((leaf_hi << 32) | leaf_lo);
        } else if ei_block - logical_block < hole_limit {
            hole_limit = ei_block - logical_block;
        }
    }

    if best_phys.is_none() && entries > 0 {
        let off = EXT4_EXT_HDR_LEN;
        let leaf_lo = le32(node, off + 4) as u64;
        let leaf_hi = le16(node, off + 8) as u64;
        best_phys = Some((leaf_hi << 32) | leaf_lo);
    }

    let Some(child_phys) = best_phys else {
        return (EXT4_LOOKUP_ERR, out);
    };
    out.child_phys = child_phys;
    out.hole_limit = hole_limit;
    (EXT4_LOOKUP_DESCEND, out)
}

#[no_mangle]
pub unsafe extern "C" fn rust_ext4_check_feature_flags(
    log_block_size: u32,
    desc_size: u16,
    compat: u32,
    incompat: u32,
    ro_compat: u32,
    require_extents: bool,
    out_read_only: *mut bool,
) -> i32 {
    if out_read_only.is_null() {
        return -1;
    }
    match check_feature_flags(
        log_block_size,
        desc_size,
        compat,
        incompat,
        ro_compat,
        require_extents,
    ) {
        Some(ro) => {
            unsafe {
                *out_read_only = ro;
            }
            0
        }
        None => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn rust_ext4_lookup_extent_node(
    node: *const u8,
    node_bytes: usize,
    logical_block: u32,
    blocks_count: u64,
    max_depth: u16,
    hole_limit: u32,
    out: *mut Ext4ExtentLookupC,
) -> i32 {
    if node.is_null() || out.is_null() || node_bytes < EXT4_EXT_HDR_LEN {
        return EXT4_LOOKUP_ERR;
    }
    let bytes = unsafe { slice::from_raw_parts(node, node_bytes) };
    let (status, res) =
        lookup_extent_node(bytes, logical_block, blocks_count, max_depth, hole_limit);
    unsafe {
        *out = res;
    }
    status
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn validates_feature_flags_and_ro_compat() {
        let incompat = EXT4_FEATURE_INCOMPAT_FILETYPE
            | EXT4_FEATURE_INCOMPAT_EXTENTS
            | EXT4_FEATURE_INCOMPAT_64BIT;
        assert_eq!(
            check_feature_flags(2, 64, EXT4_FEATURE_COMPAT_HAS_JOURNAL, incompat, 0x0001, true),
            Some(false)
        );
        // Unknown ro_compat -> read_only = true
        assert_eq!(
            check_feature_flags(2, 64, 0, incompat, 0x8000, true),
            Some(true)
        );
        // Invalid 64-bit desc_size
        assert_eq!(check_feature_flags(2, 32, 0, incompat, 0, true), None);
        // RECOVER without HAS_JOURNAL
        assert_eq!(
            check_feature_flags(
                2,
                64,
                0,
                incompat | EXT4_FEATURE_INCOMPAT_RECOVER,
                0,
                true
            ),
            None
        );
    }

    #[test]
    fn rejects_corrupt_extent_entries_count_past_node_bounds() {
        let mut inode_blk = [0u8; 60];
        inode_blk[0..2].copy_from_slice(&EXT4_EXT_MAGIC.to_le_bytes());
        inode_blk[2..4].copy_from_slice(&100u16.to_le_bytes()); // eh_entries = 100 > 4!
        inode_blk[4..6].copy_from_slice(&4u16.to_le_bytes());   // eh_max = 4
        let (status, _) = lookup_extent_node(&inode_blk, 0, 10000, 5, 100);
        assert_eq!(status, EXT4_LOOKUP_ERR);
    }

    #[test]
    fn looks_up_valid_leaf_extent_and_hole() {
        let mut inode_blk = [0u8; 60];
        inode_blk[0..2].copy_from_slice(&EXT4_EXT_MAGIC.to_le_bytes());
        inode_blk[2..4].copy_from_slice(&1u16.to_le_bytes());
        inode_blk[4..6].copy_from_slice(&4u16.to_le_bytes());
        // Extent 0: ee_block=2, ee_len=4, pblock=500
        inode_blk[12..16].copy_from_slice(&2u32.to_le_bytes());
        inode_blk[16..18].copy_from_slice(&4u16.to_le_bytes());
        inode_blk[20..24].copy_from_slice(&500u32.to_le_bytes());

        let (st_hole, res_hole) = lookup_extent_node(&inode_blk, 0, 10000, 5, 100);
        assert_eq!(st_hole, EXT4_LOOKUP_HOLE);
        assert_eq!(res_hole.run_len, 2);

        let (st_hit, res_hit) = lookup_extent_node(&inode_blk, 3, 10000, 5, 100);
        assert_eq!(st_hit, EXT4_LOOKUP_HIT);
        assert_eq!(res_hit.pblock, 501);
        assert_eq!(res_hit.run_len, 3);
    }
}
