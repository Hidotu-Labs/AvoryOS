use core::slice;

const ELF_MAGIC: [u8; 4] = [0x7f, b'E', b'L', b'F'];
const ELFCLASS32: u8 = 1;
const ELFCLASS64: u8 = 2;
const ELFDATA2LSB: u8 = 1;
const EV_CURRENT: u8 = 1;

const ET_EXEC: u16 = 2;
const ET_DYN: u16 = 3;
const EM_386: u16 = 3;
const EM_X86_64: u16 = 62;

const ELF32_EHDR_SIZE: usize = 52;
const ELF64_EHDR_SIZE: usize = 64;
const ELF32_PHDR_SIZE: u16 = 32;
const ELF64_PHDR_SIZE: u16 = 56;
const ELF_MAX_PHNUM: u16 = 1024;

const ELF32_USER_LIMIT: u64 = 0xC000_0000;
const ELF64_USER_LIMIT: u64 = 0x8000_0000_0000;
const PAGE_SIZE: u64 = 4096;

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct ElfHeaderNormC {
    pub compat32: bool,
    pub e_type: u16,
    pub e_machine: u16,
    pub e_version: u32,
    pub e_entry: u64,
    pub e_phoff: u64,
    pub e_ehsize: u16,
    pub e_phentsize: u16,
    pub e_phnum: u16,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct ElfLoadLayoutC {
    pub vaddr: u64,
    pub seg_end: u64,
    pub start_page: u64,
    pub end_page: u64,
    pub vma_offset: u64,
    pub vma_file_size: u64,
}

#[inline]
fn le16(b: &[u8], off: usize) -> u16 {
    u16::from_le_bytes([b[off], b[off + 1]])
}

#[inline]
fn le32(b: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
}

#[inline]
fn le64(b: &[u8], off: usize) -> u64 {
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

/// Parse and validate an ELF32 or ELF64 executable/shared-object header.
pub fn parse_elf_header(hdr: &[u8], file_length: u64) -> Option<ElfHeaderNormC> {
    if hdr.len() < 16 {
        return None;
    }
    if hdr[0..4] != ELF_MAGIC
        || (hdr[4] != ELFCLASS32 && hdr[4] != ELFCLASS64)
        || hdr[5] != ELFDATA2LSB
        || hdr[6] != EV_CURRENT
    {
        return None;
    }

    let compat32 = hdr[4] == ELFCLASS32;
    let (e_type, e_machine, e_version, e_entry, e_phoff, e_ehsize, e_phentsize, e_phnum) =
        if compat32 {
            if hdr.len() < ELF32_EHDR_SIZE {
                return None;
            }
            (
                le16(hdr, 16),
                le16(hdr, 18),
                le32(hdr, 20),
                le32(hdr, 24) as u64,
                le32(hdr, 28) as u64,
                le16(hdr, 40),
                le16(hdr, 42),
                le16(hdr, 44),
            )
        } else {
            if hdr.len() < ELF64_EHDR_SIZE {
                return None;
            }
            (
                le16(hdr, 16),
                le16(hdr, 18),
                le32(hdr, 20),
                le64(hdr, 24),
                le64(hdr, 32),
                le16(hdr, 52),
                le16(hdr, 54),
                le16(hdr, 56),
            )
        };

    let expected_machine = if compat32 { EM_386 } else { EM_X86_64 };
    let expected_ehsize = (if compat32 {
        ELF32_EHDR_SIZE
    } else {
        ELF64_EHDR_SIZE
    }) as u16;
    let min_phentsize = if compat32 {
        ELF32_PHDR_SIZE
    } else {
        ELF64_PHDR_SIZE
    };

    if e_machine != expected_machine
        || e_version != 1
        || e_ehsize != expected_ehsize
        || (e_type != ET_EXEC && e_type != ET_DYN)
        || e_phnum == 0
        || e_phnum > ELF_MAX_PHNUM
        || e_phentsize < min_phentsize
        || e_phoff > u32::MAX as u64
    {
        return None;
    }

    let phdr_bytes = (e_phnum as u64).checked_mul(e_phentsize as u64)?;
    let phdr_end = e_phoff.checked_add(phdr_bytes)?;
    if phdr_end > file_length {
        return None;
    }

    Some(ElfHeaderNormC {
        compat32,
        e_type,
        e_machine,
        e_version,
        e_entry,
        e_phoff,
        e_ehsize,
        e_phentsize,
        e_phnum,
    })
}

/// Validate a `PT_LOAD` segment and compute its page-aligned VMA layout.
pub fn validate_pt_load(
    compat32: bool,
    load_base: u64,
    file_length: u64,
    p_offset: u64,
    p_vaddr: u64,
    p_filesz: u64,
    p_memsz: u64,
) -> Option<ElfLoadLayoutC> {
    if p_filesz > p_memsz || p_offset > u32::MAX as u64 {
        return None;
    }
    let file_end = p_offset.checked_add(p_filesz)?;
    if file_end > file_length {
        return None;
    }

    let user_limit = if compat32 {
        ELF32_USER_LIMIT
    } else {
        ELF64_USER_LIMIT
    };

    let vaddr = load_base.checked_add(p_vaddr)?;
    if vaddr >= user_limit {
        return None;
    }
    let seg_end = vaddr.checked_add(p_memsz)?;
    if seg_end > user_limit {
        return None;
    }

    if (p_vaddr & (PAGE_SIZE - 1)) != (p_offset & (PAGE_SIZE - 1)) {
        return None;
    }

    let start_page = vaddr & !(PAGE_SIZE - 1);
    let end_page = seg_end.checked_add(PAGE_SIZE - 1)? & !(PAGE_SIZE - 1);
    let page_delta = vaddr - start_page;
    let vma_offset = p_offset.checked_sub(page_delta)?;
    let vma_file_size = page_delta.checked_add(p_filesz)?;

    Some(ElfLoadLayoutC {
        vaddr,
        seg_end,
        start_page,
        end_page,
        vma_offset,
        vma_file_size,
    })
}

#[no_mangle]
pub unsafe extern "C" fn rust_elf_parse_header(
    hdr: *const u8,
    hdr_len: usize,
    file_length: u64,
    out: *mut ElfHeaderNormC,
) -> bool {
    if hdr.is_null() || out.is_null() || hdr_len < ELF32_EHDR_SIZE {
        return false;
    }
    let bytes = unsafe { slice::from_raw_parts(hdr, hdr_len) };
    match parse_elf_header(bytes, file_length) {
        Some(parsed) => {
            unsafe {
                *out = parsed;
            }
            true
        }
        None => false,
    }
}

#[no_mangle]
pub unsafe extern "C" fn rust_elf_validate_pt_load(
    compat32: bool,
    load_base: u64,
    file_length: u64,
    p_offset: u64,
    p_vaddr: u64,
    p_filesz: u64,
    p_memsz: u64,
    out: *mut ElfLoadLayoutC,
) -> bool {
    if out.is_null() {
        return false;
    }
    match validate_pt_load(
        compat32,
        load_base,
        file_length,
        p_offset,
        p_vaddr,
        p_filesz,
        p_memsz,
    ) {
        Some(layout) => {
            unsafe {
                *out = layout;
            }
            true
        }
        None => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_elf64_hdr(phoff: u64, phnum: u16) -> [u8; 64] {
        let mut h = [0u8; 64];
        h[0..4].copy_from_slice(&ELF_MAGIC);
        h[4] = ELFCLASS64;
        h[5] = ELFDATA2LSB;
        h[6] = EV_CURRENT;
        h[16..18].copy_from_slice(&ET_DYN.to_le_bytes());
        h[18..20].copy_from_slice(&EM_X86_64.to_le_bytes());
        h[20..24].copy_from_slice(&1u32.to_le_bytes());
        h[24..32].copy_from_slice(&0x1000u64.to_le_bytes());
        h[32..40].copy_from_slice(&phoff.to_le_bytes());
        h[52..54].copy_from_slice(&(ELF64_EHDR_SIZE as u16).to_le_bytes());
        h[54..56].copy_from_slice(&ELF64_PHDR_SIZE.to_le_bytes());
        h[56..58].copy_from_slice(&phnum.to_le_bytes());
        h
    }

    #[test]
    fn parses_valid_elf64_and_rejects_truncated_phdr_table() {
        let h = make_elf64_hdr(64, 4);
        let parsed = parse_elf_header(&h, 64 + 4 * 56).expect("valid elf64");
        assert!(!parsed.compat32);
        assert_eq!(parsed.e_phnum, 4);

        // File too short for phdr table
        assert!(parse_elf_header(&h, 100).is_none());
    }

    #[test]
    fn validates_pt_load_and_rejects_overflow_or_misaligned_pages() {
        let layout =
            validate_pt_load(false, 0x40_0000, 0x10_000, 0x1200, 0x2200, 0x1000, 0x3000)
                .expect("valid pt_load");
        assert_eq!(layout.vaddr, 0x40_2200);
        assert_eq!(layout.start_page, 0x40_2000);
        assert_eq!(layout.vma_offset, 0x1000);
        assert_eq!(layout.vma_file_size, 0x1200);

        // Overflow past ELF64_USER_LIMIT
        assert!(validate_pt_load(false, 0x7fff_ffff_f000, 0x10000, 0, 0x2000, 0x1000, 0x2000).is_none());

        // Misaligned vaddr vs offset mod PAGE_SIZE
        assert!(validate_pt_load(true, 0, 0x10000, 0x1000, 0x2001, 0x100, 0x100).is_none());
    }
}
