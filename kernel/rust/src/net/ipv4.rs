use core::slice;

const ETH_HDR_LEN: usize = 14;
const ARP_PKT_LEN: usize = 28;
const IPV4_MIN_HDR_LEN: usize = 20;

const ETH_TYPE_IPV4: u16 = 0x0800;
const ETH_TYPE_ARP: u16 = 0x0806;
const ETH_TYPE_IPV6: u16 = 0x86dd;

const ARP_HTYPE_ETHERNET: u16 = 1;

pub const ETH_INGRESS_IGNORE: i32 = 0;
pub const ETH_INGRESS_ARP: i32 = 1;
pub const ETH_INGRESS_IPV4: i32 = 2;
pub const ETH_INGRESS_IPV6: i32 = 3;

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct ArpParsedC {
    pub operation: u16,
    pub sender_mac: [u8; 6],
    pub sender_ip: u32,
    pub target_ip: u32,
    pub address_conflict: bool,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Ipv4ParsedC {
    pub src_ip: u32,
    pub dst_ip: u32,
    pub header_len: usize,
    pub total_len: usize,
    pub frag: u16,
    pub protocol: u8,
    pub is_fragment: bool,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct EthIngressC {
    pub kind: i32,
    pub eth_src_mac: [u8; 6],
    pub payload_offset: usize,
    pub payload_len: usize,
    pub arp: ArpParsedC,
    pub ipv4: Ipv4ParsedC,
}

#[inline]
fn read_be16(buf: &[u8], off: usize) -> Option<u16> {
    let b = buf.get(off..off + 2)?;
    Some(u16::from_be_bytes([b[0], b[1]]))
}

#[inline]
fn read_be32(buf: &[u8], off: usize) -> Option<u32> {
    let b = buf.get(off..off + 4)?;
    Some(u32::from_be_bytes([b[0], b[1], b[2], b[3]]))
}

fn ipv4_header_checksum(hdr: &[u8]) -> u16 {
    let mut sum: u32 = 0;
    let mut chunks = hdr.chunks_exact(2);
    for pair in &mut chunks {
        sum += u16::from_be_bytes([pair[0], pair[1]]) as u32;
    }
    if let Some(&rem) = chunks.remainder().first() {
        sum += (rem as u32) << 8;
    }
    while (sum >> 16) != 0 {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    !(sum as u16)
}

fn parse_arp(payload: &[u8], device_mac: &[u8; 6], local_ip: u32) -> Option<ArpParsedC> {
    if payload.len() < ARP_PKT_LEN {
        return None;
    }
    if read_be16(payload, 0)? != ARP_HTYPE_ETHERNET
        || read_be16(payload, 2)? != ETH_TYPE_IPV4
        || payload[4] != 6
        || payload[5] != 4
    {
        return None;
    }

    let operation = read_be16(payload, 6)?;
    let mut sender_mac = [0u8; 6];
    sender_mac.copy_from_slice(&payload[8..14]);
    let sender_ip = read_be32(payload, 14)?;
    let target_ip = read_be32(payload, 24)?;
    let address_conflict = sender_ip == local_ip && sender_mac != *device_mac;

    Some(ArpParsedC {
        operation,
        sender_mac,
        sender_ip,
        target_ip,
        address_conflict,
    })
}

fn parse_ipv4(payload: &[u8], local_ip: u32) -> Option<Ipv4ParsedC> {
    if payload.len() < IPV4_MIN_HDR_LEN {
        return None;
    }
    let ver_ihl = payload[0];
    if (ver_ihl >> 4) != 4 {
        return None;
    }
    let header_len = ((ver_ihl & 0x0f) as usize) * 4;
    if header_len < IPV4_MIN_HDR_LEN || header_len > payload.len() {
        return None;
    }
    let total_len = read_be16(payload, 2)? as usize;
    if total_len < header_len || total_len > payload.len() {
        return None;
    }
    if ipv4_header_checksum(&payload[..header_len]) != 0 {
        return None;
    }

    let frag = read_be16(payload, 6)?;
    let is_fragment = (frag & 0x3fff) != 0;
    let src_ip = read_be32(payload, 12)?;
    let dst_ip = read_be32(payload, 16)?;

    if !is_fragment && dst_ip != local_ip && dst_ip != u32::MAX {
        return None;
    }

    Some(Ipv4ParsedC {
        src_ip,
        dst_ip,
        header_len,
        total_len,
        frag,
        protocol: payload[9],
        is_fragment,
    })
}

/// Safe Rust L2/L3 ingress parser for Ethernet -> ARP / IPv4 / IPv6 frames.
pub fn parse_ethernet_frame(
    frame: &[u8],
    device_mac: &[u8; 6],
    local_ip: u32,
) -> EthIngressC {
    let mut out = EthIngressC::default();
    if frame.len() < ETH_HDR_LEN {
        return out;
    }

    let dst_mac = &frame[0..6];
    let is_bcast = dst_mac == [0xff; 6];
    let is_v6_mcast = dst_mac[0] == 0x33 && dst_mac[1] == 0x33;
    if dst_mac != device_mac && !is_bcast && !is_v6_mcast {
        return out;
    }

    out.eth_src_mac.copy_from_slice(&frame[6..12]);
    let eth_type = u16::from_be_bytes([frame[12], frame[13]]);
    let payload = &frame[ETH_HDR_LEN..];
    out.payload_offset = ETH_HDR_LEN;
    out.payload_len = payload.len();

    match eth_type {
        ETH_TYPE_ARP => {
            if let Some(arp) = parse_arp(payload, device_mac, local_ip) {
                out.kind = ETH_INGRESS_ARP;
                out.arp = arp;
            }
        }
        ETH_TYPE_IPV4 => {
            if let Some(ipv4) = parse_ipv4(payload, local_ip) {
                out.kind = ETH_INGRESS_IPV4;
                out.ipv4 = ipv4;
            }
        }
        ETH_TYPE_IPV6 => {
            out.kind = ETH_INGRESS_IPV6;
        }
        _ => {}
    }

    out
}

/// FFI entry point called by `ethernet_receive` in `kernel/src/net/ipv4.c`.
#[no_mangle]
pub unsafe extern "C" fn rust_net_parse_ethernet_frame(
    frame: *const u8,
    len: usize,
    device_mac: *const u8,
    local_ip: u32,
    out: *mut EthIngressC,
) -> i32 {
    if frame.is_null() || device_mac.is_null() || out.is_null() || len < ETH_HDR_LEN {
        return ETH_INGRESS_IGNORE;
    }

    let bytes = unsafe { slice::from_raw_parts(frame, len) };
    let mac = unsafe { &*(device_mac as *const [u8; 6]) };
    let parsed = parse_ethernet_frame(bytes, mac, local_ip);
    unsafe {
        *out = parsed;
    }
    parsed.kind
}

#[cfg(test)]
mod tests {
    use super::*;

    fn set_ipv4_checksum(hdr: &mut [u8]) {
        hdr[10] = 0;
        hdr[11] = 0;
        let c = ipv4_header_checksum(hdr);
        hdr[10..12].copy_from_slice(&c.to_be_bytes());
    }

    #[test]
    fn parses_valid_ipv4_tcp_frame() {
        let dev_mac = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];
        let local_ip: u32 = 0x0a00_020f;
        let mut frame = vec![0u8; ETH_HDR_LEN + 24];
        frame[0..6].copy_from_slice(&dev_mac);
        frame[6..12].copy_from_slice(&[0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff]);
        frame[12..14].copy_from_slice(&ETH_TYPE_IPV4.to_be_bytes());

        let ip = &mut frame[ETH_HDR_LEN..];
        ip[0] = 0x45;
        ip[2..4].copy_from_slice(&24u16.to_be_bytes());
        ip[9] = 6; // TCP
        ip[12..16].copy_from_slice(&0x0a00_0202u32.to_be_bytes());
        ip[16..20].copy_from_slice(&local_ip.to_be_bytes());
        set_ipv4_checksum(&mut ip[..20]);

        let res = parse_ethernet_frame(&frame, &dev_mac, local_ip);
        assert_eq!(res.kind, ETH_INGRESS_IPV4);
        assert_eq!(res.ipv4.protocol, 6);
        assert_eq!(res.ipv4.header_len, 20);
        assert_eq!(res.ipv4.total_len, 24);
        assert_eq!(res.ipv4.src_ip, 0x0a00_0202);
        assert_eq!(res.ipv4.dst_ip, local_ip);
        assert!(!res.ipv4.is_fragment);
    }

    #[test]
    fn rejects_bad_ipv4_checksum_and_truncated_total_len() {
        let dev_mac = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];
        let local_ip: u32 = 0x0a00_020f;
        let mut frame = vec![0u8; ETH_HDR_LEN + 20];
        frame[0..6].copy_from_slice(&dev_mac);
        frame[12..14].copy_from_slice(&ETH_TYPE_IPV4.to_be_bytes());
        {
            let ip = &mut frame[ETH_HDR_LEN..];
            ip[0] = 0x45;
            ip[2..4].copy_from_slice(&20u16.to_be_bytes());
            ip[16..20].copy_from_slice(&local_ip.to_be_bytes());
            set_ipv4_checksum(&mut ip[..20]);
        }

        // Corrupt checksum
        frame[ETH_HDR_LEN + 10] ^= 0xff;
        assert_eq!(
            parse_ethernet_frame(&frame, &dev_mac, local_ip).kind,
            ETH_INGRESS_IGNORE
        );

        // total_len > actual payload
        frame[ETH_HDR_LEN + 2..ETH_HDR_LEN + 4].copy_from_slice(&100u16.to_be_bytes());
        set_ipv4_checksum(&mut frame[ETH_HDR_LEN..ETH_HDR_LEN + 20]);
        assert_eq!(
            parse_ethernet_frame(&frame, &dev_mac, local_ip).kind,
            ETH_INGRESS_IGNORE
        );
    }

    #[test]
    fn detects_arp_conflict_and_valid_request() {
        let dev_mac = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];
        let other_mac = [0x00, 0x11, 0x22, 0x33, 0x44, 0x55];
        let local_ip: u32 = 0x0a00_020f;

        let mut frame = vec![0u8; ETH_HDR_LEN + ARP_PKT_LEN];
        frame[0..6].copy_from_slice(&[0xff; 6]);
        frame[6..12].copy_from_slice(&other_mac);
        frame[12..14].copy_from_slice(&ETH_TYPE_ARP.to_be_bytes());

        let arp = &mut frame[ETH_HDR_LEN..];
        arp[0..2].copy_from_slice(&ARP_HTYPE_ETHERNET.to_be_bytes());
        arp[2..4].copy_from_slice(&ETH_TYPE_IPV4.to_be_bytes());
        arp[4] = 6;
        arp[5] = 4;
        arp[6..8].copy_from_slice(&1u16.to_be_bytes()); // ARP request
        arp[8..14].copy_from_slice(&other_mac);
        arp[14..18].copy_from_slice(&local_ip.to_be_bytes()); // conflict!
        arp[24..28].copy_from_slice(&local_ip.to_be_bytes());

        let res = parse_ethernet_frame(&frame, &dev_mac, local_ip);
        assert_eq!(res.kind, ETH_INGRESS_ARP);
        assert_eq!(res.arp.operation, 1);
        assert!(res.arp.address_conflict);
    }
}
