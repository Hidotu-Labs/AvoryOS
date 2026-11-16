use core::slice;

const TCP_HDR_MIN_LEN: usize = 20;
const TCP_MIN_MSS: u16 = 536;
const TCP_MAX_MSS: u16 = 1460;
const TCP_MAX_WSCALE: u8 = 14;
const IP_PROTO_TCP: u64 = 6;

const TCP_OPT_EOL: u8 = 0;
const TCP_OPT_NOP: u8 = 1;
const TCP_OPT_MSS: u8 = 2;
const TCP_OPT_WSCALE: u8 = 3;

pub const TCP_PARSE_OK: i32 = 0;
pub const TCP_PARSE_MALFORMED: i32 = 1;
pub const TCP_PARSE_BAD_CSUM: i32 = 2;

/// Parsed TCP header options returned across the C FFI boundary.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct TcpOptionsC {
    pub mss: u16,
    pub wscale: u8,
    pub has_mss: bool,
    pub has_wscale: bool,
}

/// Parsed TCP segment header + options returned across the C FFI boundary.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct TcpSegmentC {
    pub src_port: u16,
    pub dst_port: u16,
    pub seq: u32,
    pub ack: u32,
    pub raw_wnd: u16,
    pub flags: u8,
    pub _pad: u8,
    pub header_len: usize,
    pub payload_len: usize,
    pub options: TcpOptionsC,
}

#[inline]
fn fold_checksum(mut sum: u64) -> u16 {
    while (sum >> 16) != 0 {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    !(sum as u16)
}

fn tcp_checksum_with_pseudo(mut sum: u64, seg: &[u8]) -> u16 {
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
fn ipv4_pseudo_sum(src: u32, dst: u32, seg_len: usize) -> u64 {
    ((src >> 16) as u64)
        + ((src & 0xffff) as u64)
        + ((dst >> 16) as u64)
        + ((dst & 0xffff) as u64)
        + IP_PROTO_TCP
        + (seg_len as u64)
}

#[inline]
fn ipv6_pseudo_sum(src: &[u8; 16], dst: &[u8; 16], seg_len: usize) -> u64 {
    let mut sum = IP_PROTO_TCP + (seg_len as u64);
    for pair in src.chunks_exact(2) {
        sum += u16::from_be_bytes([pair[0], pair[1]]) as u64;
    }
    for pair in dst.chunks_exact(2) {
        sum += u16::from_be_bytes([pair[0], pair[1]]) as u64;
    }
    sum
}

/// Safe Rust parser for TCP options in a TCP header slice (`header[20..header_len]`).
pub fn parse_tcp_options(header: &[u8]) -> TcpOptionsC {
    let mut out = TcpOptionsC::default();
    let mut opt = TCP_HDR_MIN_LEN;

    while let Some(&kind) = header.get(opt) {
        if kind == TCP_OPT_EOL {
            break;
        }
        if kind == TCP_OPT_NOP {
            opt += 1;
            continue;
        }

        let Some(&opt_len_u8) = header.get(opt + 1) else {
            break;
        };
        let opt_len = opt_len_u8 as usize;
        if opt_len < 2 {
            break;
        }
        let Some(end) = opt.checked_add(opt_len) else {
            break;
        };
        let Some(opt_bytes) = header.get(opt..end) else {
            break;
        };

        if kind == TCP_OPT_MSS && opt_len == 4 {
            let mss = u16::from_be_bytes([opt_bytes[2], opt_bytes[3]]);
            if (TCP_MIN_MSS..=TCP_MAX_MSS).contains(&mss) {
                out.mss = mss;
                out.has_mss = true;
            }
        } else if kind == TCP_OPT_WSCALE && opt_len == 3 {
            let mut shift = opt_bytes[2];
            if shift > TCP_MAX_WSCALE {
                shift = TCP_MAX_WSCALE;
            }
            out.wscale = shift;
            out.has_wscale = true;
        }

        opt = end;
    }

    out
}

/// Unified TCP segment validator + checksum + header/options parser.
pub fn parse_tcp_segment(pseudo_sum: u64, seg: &[u8]) -> Result<TcpSegmentC, i32> {
    if seg.len() < TCP_HDR_MIN_LEN {
        return Err(TCP_PARSE_MALFORMED);
    }

    let header_len = ((seg[12] >> 4) as usize) * 4;
    if header_len < TCP_HDR_MIN_LEN || header_len > seg.len() {
        return Err(TCP_PARSE_MALFORMED);
    }

    if tcp_checksum_with_pseudo(pseudo_sum, seg) != 0 {
        return Err(TCP_PARSE_BAD_CSUM);
    }

    let options = if header_len > TCP_HDR_MIN_LEN {
        parse_tcp_options(&seg[..header_len])
    } else {
        TcpOptionsC::default()
    };

    Ok(TcpSegmentC {
        src_port: u16::from_be_bytes([seg[0], seg[1]]),
        dst_port: u16::from_be_bytes([seg[2], seg[3]]),
        seq: u32::from_be_bytes([seg[4], seg[5], seg[6], seg[7]]),
        ack: u32::from_be_bytes([seg[8], seg[9], seg[10], seg[11]]),
        raw_wnd: u16::from_be_bytes([seg[14], seg[15]]),
        flags: seg[13],
        _pad: 0,
        header_len,
        payload_len: seg.len() - header_len,
        options,
    })
}

/// FFI entry point for IPv4 TCP segment parsing.
#[no_mangle]
pub unsafe extern "C" fn rust_tcp_parse_segment_v4(
    src_ip: u32,
    dst_ip: u32,
    seg: *const u8,
    len: usize,
    out: *mut TcpSegmentC,
) -> i32 {
    if seg.is_null() || out.is_null() || len < TCP_HDR_MIN_LEN {
        return TCP_PARSE_MALFORMED;
    }
    let bytes = unsafe { slice::from_raw_parts(seg, len) };
    match parse_tcp_segment(ipv4_pseudo_sum(src_ip, dst_ip, len), bytes) {
        Ok(parsed) => {
            unsafe {
                *out = parsed;
            }
            TCP_PARSE_OK
        }
        Err(code) => code,
    }
}

/// FFI entry point for IPv6 TCP segment parsing.
#[no_mangle]
pub unsafe extern "C" fn rust_tcp_parse_segment_v6(
    src_ip: *const u8,
    dst_ip: *const u8,
    seg: *const u8,
    len: usize,
    out: *mut TcpSegmentC,
) -> i32 {
    if src_ip.is_null() || dst_ip.is_null() || seg.is_null() || out.is_null()
        || len < TCP_HDR_MIN_LEN
    {
        return TCP_PARSE_MALFORMED;
    }
    let src = unsafe { &*(src_ip as *const [u8; 16]) };
    let dst = unsafe { &*(dst_ip as *const [u8; 16]) };
    let bytes = unsafe { slice::from_raw_parts(seg, len) };
    match parse_tcp_segment(ipv6_pseudo_sum(src, dst, len), bytes) {
        Ok(parsed) => {
            unsafe {
                *out = parsed;
            }
            TCP_PARSE_OK
        }
        Err(code) => code,
    }
}

/// FFI entry point called by `kernel/src/net/tcp.c` for standalone option parsing.
///
/// # Safety
/// - `header` must be valid for reads of `header_len` bytes.
/// - `out` must be a valid, aligned, non-null pointer.
#[no_mangle]
pub unsafe extern "C" fn rust_tcp_parse_options(
    header: *const u8,
    header_len: usize,
    out: *mut TcpOptionsC,
) -> bool {
    if header.is_null() || out.is_null() || header_len < TCP_HDR_MIN_LEN {
        return false;
    }

    let bytes = unsafe { slice::from_raw_parts(header, header_len) };
    let parsed = parse_tcp_options(bytes);
    unsafe {
        *out = parsed;
    }
    parsed.has_wscale
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_header_with_opts(opts: &[u8]) -> Vec<u8> {
        let mut h = vec![0u8; TCP_HDR_MIN_LEN];
        h.extend_from_slice(opts);
        while h.len() % 4 != 0 {
            h.push(0);
        }
        h[12] = ((h.len() / 4) as u8) << 4;
        h
    }

    fn finalize_v4_csum(src: u32, dst: u32, seg: &mut [u8]) {
        seg[16] = 0;
        seg[17] = 0;
        let csum = tcp_checksum_with_pseudo(ipv4_pseudo_sum(src, dst, seg.len()), seg);
        seg[16..18].copy_from_slice(&csum.to_be_bytes());
    }

    #[test]
    fn parses_mss_and_wscale_with_nops() {
        let h = make_header_with_opts(&[
            TCP_OPT_MSS, 4, 0x05, 0xb4, // 1460
            TCP_OPT_NOP,
            TCP_OPT_WSCALE, 3, 7,
            TCP_OPT_EOL,
        ]);
        let parsed = parse_tcp_options(&h);
        assert!(parsed.has_mss);
        assert_eq!(parsed.mss, 1460);
        assert!(parsed.has_wscale);
        assert_eq!(parsed.wscale, 7);
    }

    #[test]
    fn clamps_wscale_and_ignores_out_of_range_mss() {
        let h = make_header_with_opts(&[
            TCP_OPT_MSS, 4, 0x00, 0x64, // 100 < TCP_MIN_MSS (536)
            TCP_OPT_WSCALE, 3, 20,      // clamped to 14
        ]);
        let parsed = parse_tcp_options(&h);
        assert!(!parsed.has_mss);
        assert!(parsed.has_wscale);
        assert_eq!(parsed.wscale, 14);
    }

    #[test]
    fn stops_safely_on_truncated_or_zero_len_option() {
        let h1 = make_header_with_opts(&[TCP_OPT_MSS, 0, 0x05, 0xb4]);
        let p1 = parse_tcp_options(&h1);
        assert!(!p1.has_mss);

        let mut h2 = vec![0u8; TCP_HDR_MIN_LEN];
        h2.extend_from_slice(&[TCP_OPT_WSCALE, 3, 5, TCP_OPT_MSS, 4, 0x05]);
        let p2 = parse_tcp_options(&h2);
        assert!(p2.has_wscale);
        assert_eq!(p2.wscale, 5);
        assert!(!p2.has_mss);
    }

    #[test]
    fn parses_valid_v4_segment_and_detects_bad_csum() {
        let src = 0x0a00_020f;
        let dst = 0x0a00_0202;
        let mut seg = make_header_with_opts(&[TCP_OPT_MSS, 4, 0x05, 0xb4]);
        seg[0..2].copy_from_slice(&12345u16.to_be_bytes());
        seg[2..4].copy_from_slice(&443u16.to_be_bytes());
        seg[4..8].copy_from_slice(&0x1122_3344u32.to_be_bytes());
        seg[8..12].copy_from_slice(&0x5566_7788u32.to_be_bytes());
        seg[13] = 0x12; // SYN | ACK
        seg[14..16].copy_from_slice(&65535u16.to_be_bytes());
        seg.extend_from_slice(b"hello");
        finalize_v4_csum(src, dst, &mut seg);

        let parsed = parse_tcp_segment(ipv4_pseudo_sum(src, dst, seg.len()), &seg)
            .expect("valid v4 segment");
        assert_eq!(parsed.src_port, 12345);
        assert_eq!(parsed.dst_port, 443);
        assert_eq!(parsed.seq, 0x1122_3344);
        assert_eq!(parsed.ack, 0x5566_7788);
        assert_eq!(parsed.flags, 0x12);
        assert_eq!(parsed.raw_wnd, 65535);
        assert_eq!(parsed.header_len, 24);
        assert_eq!(parsed.payload_len, 5);
        assert!(parsed.options.has_mss);
        assert_eq!(parsed.options.mss, 1460);

        seg[24] ^= 0xff;
        assert_eq!(
            parse_tcp_segment(ipv4_pseudo_sum(src, dst, seg.len()), &seg),
            Err(TCP_PARSE_BAD_CSUM)
        );
    }

    #[test]
    fn rejects_invalid_data_offset() {
        let mut seg = vec![0u8; 20];
        seg[12] = 0x40; // doff = 16 < 20
        assert_eq!(
            parse_tcp_segment(ipv4_pseudo_sum(1, 2, seg.len()), &seg),
            Err(TCP_PARSE_MALFORMED)
        );
        seg[12] = 0x60; // doff = 24 > 20
        assert_eq!(
            parse_tcp_segment(ipv4_pseudo_sum(1, 2, seg.len()), &seg),
            Err(TCP_PARSE_MALFORMED)
        );
    }
}
