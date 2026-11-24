use core::slice;

const RSDP_SIGNATURE: [u8; 8] = *b"RSD PTR ";
const RSDP_V1_LEN: usize = 20;
const RSDP_V2_MIN_LEN: usize = 36;
const SDT_HDR_LEN: usize = 36;
const MADT_HDR_LEN: usize = 44;

const MAX_ISOS: usize = 24;
const MAX_CPUS_ACPI: usize = 64;

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct AcpiIsoEntryC {
    pub irq_source: u8,
    pub _pad: u8,
    pub flags: u16,
    pub gsi: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct AcpiMadtParsedC {
    pub lapic_base_address: u32,
    pub ioapic_address: u32,
    pub ioapic_gsi: u32,
    pub has_ioapic: bool,
    pub cpu_count: u32,
    pub cpu_apic_ids: [u32; MAX_CPUS_ACPI],
    pub iso_count: u32,
    pub iso_entries: [AcpiIsoEntryC; MAX_ISOS],
}

impl Default for AcpiMadtParsedC {
    fn default() -> Self {
        Self {
            lapic_base_address: 0,
            ioapic_address: 0,
            ioapic_gsi: 0,
            has_ioapic: false,
            cpu_count: 0,
            cpu_apic_ids: [0u32; MAX_CPUS_ACPI],
            iso_count: 0,
            iso_entries: [AcpiIsoEntryC::default(); MAX_ISOS],
        }
    }
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

pub fn checksum_valid(bytes: &[u8]) -> bool {
    bytes.iter().fold(0u8, |acc, &b| acc.wrapping_add(b)) == 0
}

pub fn parse_rsdp(rsdp: &[u8]) -> Option<(bool, u64)> {
    if rsdp.len() < RSDP_V1_LEN || rsdp[0..8] != RSDP_SIGNATURE {
        return None;
    }
    let revision = rsdp[15];
    if revision >= 2 && rsdp.len() >= RSDP_V2_MIN_LEN {
        let ext_len = le32(rsdp, 20) as usize;
        if ext_len >= RSDP_V2_MIN_LEN && ext_len <= rsdp.len() && checksum_valid(&rsdp[..ext_len]) {
            return Some((true, le64(rsdp, 24)));
        }
    }
    if checksum_valid(&rsdp[..RSDP_V1_LEN]) {
        return Some((false, le32(rsdp, 16) as u64));
    }
    None
}

fn record_madt_cpu(out: &mut AcpiMadtParsedC, apic_id: u32) {
    let limit = (out.cpu_count as usize).min(MAX_CPUS_ACPI);
    if out.cpu_apic_ids[..limit].contains(&apic_id) {
        return;
    }
    if limit < MAX_CPUS_ACPI {
        out.cpu_apic_ids[limit] = apic_id;
    }
    out.cpu_count += 1;
}

pub fn parse_madt(madt: &[u8]) -> Option<AcpiMadtParsedC> {
    if madt.len() < MADT_HDR_LEN || &madt[0..4] != b"APIC" {
        return None;
    }
    let total_len = le32(madt, 4) as usize;
    if total_len < MADT_HDR_LEN || total_len > madt.len() || !checksum_valid(&madt[..total_len]) {
        return None;
    }

    let mut out = AcpiMadtParsedC {
        lapic_base_address: le32(madt, SDT_HDR_LEN),
        ..Default::default()
    };

    let mut offset = MADT_HDR_LEN;
    while offset + 2 <= total_len {
        let entry_type = madt[offset];
        let entry_len = madt[offset + 1] as usize;
        if entry_len < 2 {
            break;
        }
        let Some(end) = offset.checked_add(entry_len) else {
            break;
        };
        let Some(entry) = madt.get(offset..end) else {
            break;
        };
        if end > total_len {
            break;
        }

        match entry_type {
            0 if entry_len >= 8 => {
                // Local APIC: acpi_id(2), apic_id(3), flags(4..8)
                if (le32(entry, 4) & 1) != 0 {
                    record_madt_cpu(&mut out, entry[3] as u32);
                }
            }
            1 if entry_len >= 12 => {
                // I/O APIC: ioapic_addr(4..8), gsi_base(8..12)
                if !out.has_ioapic {
                    out.ioapic_address = le32(entry, 4);
                    out.ioapic_gsi = le32(entry, 8);
                    out.has_ioapic = true;
                }
            }
            2 if entry_len >= 10 => {
                // Interrupt Source Override: irq_source(3), gsi(4..8), flags(8..10)
                let idx = out.iso_count as usize;
                if idx < MAX_ISOS {
                    out.iso_entries[idx] = AcpiIsoEntryC {
                        irq_source: entry[3],
                        _pad: 0,
                        flags: le16(entry, 8),
                        gsi: le32(entry, 4),
                    };
                    out.iso_count += 1;
                }
            }
            9 if entry_len >= 16 => {
                // Processor Local x2APIC: x2apic_id(4..8), flags(8..12)
                let x2apic_id = le32(entry, 4);
                if (le32(entry, 8) & 1) != 0 && x2apic_id != u32::MAX {
                    record_madt_cpu(&mut out, x2apic_id);
                }
            }
            _ => {}
        }

        offset = end;
    }

    Some(out)
}

#[no_mangle]
pub unsafe extern "C" fn rust_acpi_validate_checksum(table: *const u8, len: usize) -> bool {
    if table.is_null() || len == 0 {
        return false;
    }
    let bytes = unsafe { slice::from_raw_parts(table, len) };
    checksum_valid(bytes)
}

#[no_mangle]
pub unsafe extern "C" fn rust_acpi_parse_rsdp(
    rsdp: *const u8,
    out_use_xsdt: *mut bool,
    out_sdt_phys: *mut u64,
) -> bool {
    if rsdp.is_null() || out_use_xsdt.is_null() || out_sdt_phys.is_null() {
        return false;
    }
    let bytes = unsafe { slice::from_raw_parts(rsdp, RSDP_V2_MIN_LEN) };
    match parse_rsdp(bytes) {
        Some((use_xsdt, addr)) => {
            unsafe {
                *out_use_xsdt = use_xsdt;
                *out_sdt_phys = addr;
            }
            true
        }
        None => false,
    }
}

#[no_mangle]
pub unsafe extern "C" fn rust_acpi_parse_madt(
    madt: *const u8,
    madt_len: usize,
    out: *mut AcpiMadtParsedC,
) -> bool {
    if madt.is_null() || out.is_null() || madt_len < MADT_HDR_LEN {
        return false;
    }
    let bytes = unsafe { slice::from_raw_parts(madt, madt_len) };
    match parse_madt(bytes) {
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

    fn set_checksum(buf: &mut [u8], csum_off: usize) {
        buf[csum_off] = 0;
        let sum = buf.iter().fold(0u8, |acc, &b| acc.wrapping_add(b));
        buf[csum_off] = 0u8.wrapping_sub(sum);
    }

    #[test]
    fn parses_rsdp_v2_and_falls_back_to_v1() {
        let mut rsdp = [0u8; RSDP_V2_MIN_LEN];
        rsdp[0..8].copy_from_slice(&RSDP_SIGNATURE);
        rsdp[15] = 2; // ACPI 2.0
        rsdp[16..20].copy_from_slice(&0x1000_0000u32.to_le_bytes());
        rsdp[20..24].copy_from_slice(&(RSDP_V2_MIN_LEN as u32).to_le_bytes());
        rsdp[24..32].copy_from_slice(&0x2000_0000_1000u64.to_le_bytes());
        set_checksum(&mut rsdp[..RSDP_V1_LEN], 8);
        set_checksum(&mut rsdp, 32);

        assert_eq!(parse_rsdp(&rsdp), Some((true, 0x2000_0000_1000)));

        // Corrupt extended checksum -> falls back to v1 RSDT
        rsdp[32] ^= 1;
        assert_eq!(parse_rsdp(&rsdp), Some((false, 0x1000_0000)));
    }

    #[test]
    fn parses_madt_entries_and_stops_on_corrupt_length() {
        let mut madt = vec![0u8; MADT_HDR_LEN + 8 + 12 + 10];
        madt[0..4].copy_from_slice(b"APIC");
        let total_len = madt.len() as u32;
        madt[4..8].copy_from_slice(&total_len.to_le_bytes());
        madt[SDT_HDR_LEN..SDT_HDR_LEN + 4].copy_from_slice(&0xFEE0_0000u32.to_le_bytes());

        // Type 0: Local APIC (id=3, enabled)
        let e0 = &mut madt[MADT_HDR_LEN..MADT_HDR_LEN + 8];
        e0[0] = 0;
        e0[1] = 8;
        e0[3] = 3;
        e0[4..8].copy_from_slice(&1u32.to_le_bytes());

        // Type 1: I/O APIC
        let e1 = &mut madt[MADT_HDR_LEN + 8..MADT_HDR_LEN + 20];
        e1[0] = 1;
        e1[1] = 12;
        e1[4..8].copy_from_slice(&0xFEC0_0000u32.to_le_bytes());

        // Type 2: ISO (IRQ0 -> GSI2)
        let e2 = &mut madt[MADT_HDR_LEN + 20..MADT_HDR_LEN + 30];
        e2[0] = 2;
        e2[1] = 10;
        e2[3] = 0;
        e2[4..8].copy_from_slice(&2u32.to_le_bytes());
        e2[8..10].copy_from_slice(&5u16.to_le_bytes());

        set_checksum(&mut madt, 9);

        let parsed = parse_madt(&madt).expect("valid madt");
        assert_eq!(parsed.lapic_base_address, 0xFEE0_0000);
        assert_eq!(parsed.cpu_count, 1);
        assert_eq!(parsed.cpu_apic_ids[0], 3);
        assert!(parsed.has_ioapic);
        assert_eq!(parsed.ioapic_address, 0xFEC0_0000);
        assert_eq!(parsed.iso_count, 1);
        assert_eq!(parsed.iso_entries[0].gsi, 2);

        // Corrupt entry length to 1 (< 2) -> stops safely without looping
        madt[MADT_HDR_LEN + 9] = 1;
        set_checksum(&mut madt, 9);
        let partial = parse_madt(&madt).expect("still valid header");
        assert_eq!(partial.cpu_count, 1);
        assert!(!partial.has_ioapic);
    }

    #[test]
    fn parses_madt_x2apic_and_deduplicates() {
        let mut madt = vec![0u8; MADT_HDR_LEN + 8 + 16 + 16 + 16];
        madt[0..4].copy_from_slice(b"APIC");
        let total_len = madt.len() as u32;
        madt[4..8].copy_from_slice(&total_len.to_le_bytes());

        // Type 0: Local APIC (id=16, enabled)
        let e0 = &mut madt[MADT_HDR_LEN..MADT_HDR_LEN + 8];
        e0[0] = 0;
        e0[1] = 8;
        e0[3] = 16;
        e0[4..8].copy_from_slice(&1u32.to_le_bytes());

        // Type 9: Duplicate Local x2APIC (id=16, enabled) -> ignored
        let e1 = &mut madt[MADT_HDR_LEN + 8..MADT_HDR_LEN + 24];
        e1[0] = 9;
        e1[1] = 16;
        e1[4..8].copy_from_slice(&16u32.to_le_bytes());
        e1[8..12].copy_from_slice(&1u32.to_le_bytes());

        // Type 9: Local x2APIC (id=260, enabled) -> kept
        let e2 = &mut madt[MADT_HDR_LEN + 24..MADT_HDR_LEN + 40];
        e2[0] = 9;
        e2[1] = 16;
        e2[4..8].copy_from_slice(&260u32.to_le_bytes());
        e2[8..12].copy_from_slice(&1u32.to_le_bytes());

        // Type 9: Disabled Local x2APIC (id=261, flags=0) -> ignored
        let e3 = &mut madt[MADT_HDR_LEN + 40..MADT_HDR_LEN + 56];
        e3[0] = 9;
        e3[1] = 16;
        e3[4..8].copy_from_slice(&261u32.to_le_bytes());
        e3[8..12].copy_from_slice(&0u32.to_le_bytes());

        set_checksum(&mut madt, 9);

        let parsed = parse_madt(&madt).expect("valid madt");
        assert_eq!(parsed.cpu_count, 2);
        assert_eq!(parsed.cpu_apic_ids[0], 16);
        assert_eq!(parsed.cpu_apic_ids[1], 260);
    }
}
