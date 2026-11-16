use core::slice;

use crate::net::udp::parse_udp_v6;

const IPV6_HDR_LEN: usize = 40;
const IPV6_NEXT_TCP: u8 = 6;
const IPV6_NEXT_UDP: u8 = 17;
const IPV6_NEXT_ICMP: u8 = 58;

const ICMP6_ECHO_REQUEST: u8 = 128;
const ICMP6_RA: u8 = 134;
const ICMP6_NS: u8 = 135;
const ICMP6_NA: u8 = 136;

pub const IPV6_INGRESS_IGNORE: i32 = 0;
pub const IPV6_INGRESS_UDP: i32 = 1;
pub const IPV6_INGRESS_TCP: i32 = 2;
pub const IPV6_INGRESS_ICMP_ECHO_REQ: i32 = 3;
pub const IPV6_INGRESS_ICMP_NS: i32 = 4;
pub const IPV6_INGRESS_ICMP_NA: i32 = 5;
pub const IPV6_INGRESS_ICMP_RA: i32 = 6;

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Ipv6IngressC {
    pub kind: i32,
    pub src_ip: [u8; 16],
    pub dst_ip: [u8; 16],
    pub nd_target: [u8; 16],
    pub ra_prefix: [u8; 8],
    pub has_ra_prefix: bool,
    pub hop_limit: u8,
    pub payload_offset: usize,
    pub payload_len: usize,
}

fn icmp6_checksum(src: &[u8; 16], dst: &[u8; 16], icmp: &[u8]) -> u16 {
    let mut sum: u64 = (IPV6_NEXT_ICMP as u64) + (icmp.len() as u64);
    for pair in src.chunks_exact(2) {
        sum += u16::from_be_bytes([pair[0], pair[1]]) as u64;
    }
    for pair in dst.chunks_exact(2) {
        sum += u16::from_be_bytes([pair[0], pair[1]]) as u64;
    }
    let mut chunks = icmp.chunks_exact(2);
    for pair in &mut chunks {
        sum += u16::from_be_bytes([pair[0], pair[1]]) as u64;
    }
    if let Some(&rem) = chunks.remainder().first() {
        sum += (rem as u64) << 8;
    }
    while (sum >> 16) != 0 {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    !(sum as u16)
}

fn parse_ra_prefix(icmp: &[u8]) -> Option<[u8; 8]> {
    let mut offset = 16usize;
    let mut found: Option<[u8; 8]> = None;

    while offset + 2 <= icmp.len() {
        let opt_len = (icmp[offset + 1] as usize) * 8;
        if opt_len == 0 {
            return None;
        }
        let end = offset.checked_add(opt_len)?;
        let opt = icmp.get(offset..end)?;
        if opt[0] == 3 && opt_len == 32 && opt[2] == 64 && (opt[3] & 0x40) != 0 {
            // Only 2000::/3 addresses are global unicast routable to the internet (RFC 4291)
            if (opt[16] & 0xe0) == 0x20 {
                let mut prefix = [0u8; 8];
                prefix.copy_from_slice(&opt[16..24]);
                found = Some(prefix);
            }
        }
        offset = end;
    }

    found
}

/// Safe Rust parser for IPv6 + UDPv6 + ICMPv6/ND/RA ingress packets.
pub fn parse_ipv6_packet(packet: &[u8]) -> Ipv6IngressC {
    let mut out = Ipv6IngressC::default();
    if packet.len() < IPV6_HDR_LEN || (packet[0] >> 4) != 6 {
        return out;
    }

    let payload_len = u16::from_be_bytes([packet[4], packet[5]]) as usize;
    if payload_len > packet.len() - IPV6_HDR_LEN {
        return out;
    }

    let next_header = packet[6];
    let hop_limit = packet[7];
    let mut src_ip = [0u8; 16];
    let mut dst_ip = [0u8; 16];
    src_ip.copy_from_slice(&packet[8..24]);
    dst_ip.copy_from_slice(&packet[24..40]);

    out.src_ip = src_ip;
    out.dst_ip = dst_ip;
    out.hop_limit = hop_limit;
    out.payload_offset = IPV6_HDR_LEN;
    out.payload_len = payload_len;

    let payload = &packet[IPV6_HDR_LEN..IPV6_HDR_LEN + payload_len];

    match next_header {
        IPV6_NEXT_UDP => {
            if parse_udp_v6(&src_ip, &dst_ip, payload).is_some() {
                out.kind = IPV6_INGRESS_UDP;
            }
        }
        IPV6_NEXT_TCP => {
            out.kind = IPV6_INGRESS_TCP;
        }
        IPV6_NEXT_ICMP => {
            if payload.len() < 4 || icmp6_checksum(&src_ip, &dst_ip, payload) != 0 {
                return out;
            }
            match payload[0] {
                ICMP6_ECHO_REQUEST => {
                    out.kind = IPV6_INGRESS_ICMP_ECHO_REQ;
                }
                ICMP6_NS if payload.len() >= 24 => {
                    out.nd_target.copy_from_slice(&payload[8..24]);
                    out.kind = IPV6_INGRESS_ICMP_NS;
                }
                ICMP6_NA if payload.len() >= 24 => {
                    out.nd_target.copy_from_slice(&payload[8..24]);
                    out.kind = IPV6_INGRESS_ICMP_NA;
                }
                ICMP6_RA if hop_limit == 255 && payload.len() >= 16 => {
                    if let Some(prefix) = parse_ra_prefix(payload) {
                        out.ra_prefix = prefix;
                        out.has_ra_prefix = true;
                    }
                    out.kind = IPV6_INGRESS_ICMP_RA;
                }
                _ => {}
            }
        }
        _ => {}
    }

    out
}

/// FFI entry point called by `ipv6_receive` in `kernel/src/net/ipv6.c`.
#[no_mangle]
pub unsafe extern "C" fn rust_ipv6_parse_packet(
    packet: *const u8,
    len: usize,
    out: *mut Ipv6IngressC,
) -> i32 {
    if packet.is_null() || out.is_null() || len < IPV6_HDR_LEN {
        return IPV6_INGRESS_IGNORE;
    }

    let bytes = unsafe { slice::from_raw_parts(packet, len) };
    let parsed = parse_ipv6_packet(bytes);
    unsafe {
        *out = parsed;
    }
    parsed.kind
}

#[cfg(test)]
mod tests {
    use super::*;

    fn set_icmp6_csum(src: &[u8; 16], dst: &[u8; 16], icmp: &mut [u8]) {
        icmp[2] = 0;
        icmp[3] = 0;
        let c = icmp6_checksum(src, dst, icmp);
        icmp[2..4].copy_from_slice(&c.to_be_bytes());
    }

    #[test]
    fn parses_valid_ra_with_global_prefix_option() {
        let src = [0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1];
        let dst = [0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1];
        let icmp_len = 16 + 32;
        let mut pkt = vec![0u8; IPV6_HDR_LEN + icmp_len];
        pkt[0] = 0x60;
        pkt[4..6].copy_from_slice(&(icmp_len as u16).to_be_bytes());
        pkt[6] = IPV6_NEXT_ICMP;
        pkt[7] = 255;
        pkt[8..24].copy_from_slice(&src);
        pkt[24..40].copy_from_slice(&dst);

        {
            let icmp = &mut pkt[IPV6_HDR_LEN..];
            icmp[0] = ICMP6_RA;
            // Prefix Info option: type=3, len=4 (32B), prefix_len=64, A=0x40
            let opt = &mut icmp[16..48];
            opt[0] = 3;
            opt[1] = 4;
            opt[2] = 64;
            opt[3] = 0x40;
            opt[16..24].copy_from_slice(&[0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 1]);
            set_icmp6_csum(&src, &dst, icmp);
        }

        let res = parse_ipv6_packet(&pkt);
        assert_eq!(res.kind, IPV6_INGRESS_ICMP_RA);
        assert!(res.has_ra_prefix);
        assert_eq!(res.ra_prefix, [0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 1]);
    }

    #[test]
    fn rejects_bad_icmp6_csum_and_zero_len_ra_opt() {
        let src = [0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1];
        let dst = [0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1];
        let mut pkt = vec![0u8; IPV6_HDR_LEN + 24];
        pkt[0] = 0x60;
        pkt[4..6].copy_from_slice(&24u16.to_be_bytes());
        pkt[6] = IPV6_NEXT_ICMP;
        pkt[7] = 255;
        pkt[8..24].copy_from_slice(&src);
        pkt[24..40].copy_from_slice(&dst);
        pkt[IPV6_HDR_LEN] = ICMP6_RA;
        pkt[IPV6_HDR_LEN + 16] = 3;
        pkt[IPV6_HDR_LEN + 17] = 0; // zero option length -> infinite loop guard
        set_icmp6_csum(&src, &dst, &mut pkt[IPV6_HDR_LEN..]);

        let res = parse_ipv6_packet(&pkt);
        assert_eq!(res.kind, IPV6_INGRESS_ICMP_RA);
        assert!(!res.has_ra_prefix);

        pkt[IPV6_HDR_LEN + 2] ^= 0xff; // corrupt checksum
        assert_eq!(parse_ipv6_packet(&pkt).kind, IPV6_INGRESS_IGNORE);
    }
}
