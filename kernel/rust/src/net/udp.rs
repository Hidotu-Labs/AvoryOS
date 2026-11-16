use core::slice;

const UDP_HDR_LEN: usize = 8;
const IP_PROTO_UDP: u64 = 17;

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct UdpHeaderC {
    pub src_port: u16,
    pub dst_port: u16,
    pub udp_len: u16,
    pub payload_len: u16,
}

#[inline]
fn fold_checksum(mut sum: u64) -> u16 {
    while (sum >> 16) != 0 {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    !(sum as u16)
}

fn udp_checksum_with_pseudo(mut sum: u64, seg: &[u8]) -> u16 {
    let mut chunks = seg.chunks_exact(2);
    for pair in &mut chunks {
        sum += u16::from_be_bytes([pair[0], pair[1]]) as u64;
    }
    if let Some(&rem) = chunks.remainder().first() {
        sum += (rem as u64) << 8;
    }
    fold_checksum(sum)
}

#[inline]
fn ipv4_pseudo_sum(src: u32, dst: u32, udp_len: usize) -> u64 {
    ((src >> 16) as u64)
        + ((src & 0xffff) as u64)
        + ((dst >> 16) as u64)
        + ((dst & 0xffff) as u64)
        + IP_PROTO_UDP
        + (udp_len as u64)
}

#[inline]
fn ipv6_pseudo_sum(src: &[u8; 16], dst: &[u8; 16], udp_len: usize) -> u64 {
    let mut sum = IP_PROTO_UDP + (udp_len as u64);
    for pair in src.chunks_exact(2) {
        sum += u16::from_be_bytes([pair[0], pair[1]]) as u64;
    }
    for pair in dst.chunks_exact(2) {
        sum += u16::from_be_bytes([pair[0], pair[1]]) as u64;
    }
    sum
}

pub fn parse_udp_v4(src_ip: u32, dst_ip: u32, seg: &[u8]) -> Option<UdpHeaderC> {
    if seg.len() < UDP_HDR_LEN {
        return None;
    }
    let udp_len = u16::from_be_bytes([seg[4], seg[5]]) as usize;
    if udp_len < UDP_HDR_LEN || udp_len > seg.len() {
        return None;
    }
    let csum_field = u16::from_be_bytes([seg[6], seg[7]]);
    if csum_field != 0 {
        let udp_slice = &seg[..udp_len];
        if udp_checksum_with_pseudo(ipv4_pseudo_sum(src_ip, dst_ip, udp_len), udp_slice) != 0 {
            return None;
        }
    }

    Some(UdpHeaderC {
        src_port: u16::from_be_bytes([seg[0], seg[1]]),
        dst_port: u16::from_be_bytes([seg[2], seg[3]]),
        udp_len: udp_len as u16,
        payload_len: (udp_len - UDP_HDR_LEN) as u16,
    })
}

pub fn parse_udp_v6(src_ip: &[u8; 16], dst_ip: &[u8; 16], seg: &[u8]) -> Option<UdpHeaderC> {
    if seg.len() < UDP_HDR_LEN {
        return None;
    }
    let udp_len = u16::from_be_bytes([seg[4], seg[5]]) as usize;
    if udp_len < UDP_HDR_LEN || udp_len > seg.len() {
        return None;
    }
    let csum_field = u16::from_be_bytes([seg[6], seg[7]]);
    if csum_field == 0 {
        return None;
    }
    let udp_slice = &seg[..udp_len];
    if udp_checksum_with_pseudo(ipv6_pseudo_sum(src_ip, dst_ip, udp_len), udp_slice) != 0 {
        return None;
    }

    Some(UdpHeaderC {
        src_port: u16::from_be_bytes([seg[0], seg[1]]),
        dst_port: u16::from_be_bytes([seg[2], seg[3]]),
        udp_len: udp_len as u16,
        payload_len: (udp_len - UDP_HDR_LEN) as u16,
    })
}

#[no_mangle]
pub unsafe extern "C" fn rust_udp_parse_v4(
    src_ip: u32,
    dst_ip: u32,
    seg: *const u8,
    len: usize,
    out: *mut UdpHeaderC,
) -> bool {
    if seg.is_null() || out.is_null() || len < UDP_HDR_LEN {
        return false;
    }
    let bytes = unsafe { slice::from_raw_parts(seg, len) };
    match parse_udp_v4(src_ip, dst_ip, bytes) {
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
pub unsafe extern "C" fn rust_udp_parse_v6(
    src_ip: *const u8,
    dst_ip: *const u8,
    seg: *const u8,
    len: usize,
    out: *mut UdpHeaderC,
) -> bool {
    if src_ip.is_null() || dst_ip.is_null() || seg.is_null() || out.is_null() || len < UDP_HDR_LEN
    {
        return false;
    }
    let src = unsafe { &*(src_ip as *const [u8; 16]) };
    let dst = unsafe { &*(dst_ip as *const [u8; 16]) };
    let bytes = unsafe { slice::from_raw_parts(seg, len) };
    match parse_udp_v6(src, dst, bytes) {
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

    fn make_udp_v4(src: u32, dst: u32, sp: u16, dp: u16, payload: &[u8], with_csum: bool) -> Vec<u8> {
        let udp_len = (UDP_HDR_LEN + payload.len()) as u16;
        let mut seg = vec![0u8; udp_len as usize];
        seg[0..2].copy_from_slice(&sp.to_be_bytes());
        seg[2..4].copy_from_slice(&dp.to_be_bytes());
        seg[4..6].copy_from_slice(&udp_len.to_be_bytes());
        seg[8..].copy_from_slice(payload);
        if with_csum {
            let mut c = udp_checksum_with_pseudo(ipv4_pseudo_sum(src, dst, udp_len as usize), &seg);
            if c == 0 {
                c = 0xffff;
            }
            seg[6..8].copy_from_slice(&c.to_be_bytes());
        }
        seg
    }

    #[test]
    fn parses_valid_v4_udp_with_and_without_checksum() {
        let src = 0x0a00_0202;
        let dst = 0x0a00_020f;
        let seg = make_udp_v4(src, dst, 53, 50000, b"dns", true);
        let parsed = parse_udp_v4(src, dst, &seg).expect("valid v4 udp");
        assert_eq!(parsed.src_port, 53);
        assert_eq!(parsed.dst_port, 50000);
        assert_eq!(parsed.udp_len, 11);
        assert_eq!(parsed.payload_len, 3);

        let no_csum = make_udp_v4(src, dst, 53, 50000, b"dns", false);
        assert!(parse_udp_v4(src, dst, &no_csum).is_some());
    }

    #[test]
    fn rejects_bad_checksum_and_truncated_udp() {
        let src = 0x0a00_0202;
        let dst = 0x0a00_020f;
        let mut seg = make_udp_v4(src, dst, 53, 50000, b"dns", true);
        seg[8] ^= 0xff;
        assert!(parse_udp_v4(src, dst, &seg).is_none());

        let short = &seg[..6];
        assert!(parse_udp_v4(src, dst, short).is_none());
    }
}
