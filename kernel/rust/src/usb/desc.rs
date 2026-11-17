//! Parser for raw USB configuration and hub descriptors.
//!
//! No allocation, no panics: every read goes through `get()`, and every
//! malformed length is returned as a [`DescError`].  Not wired into the probe
//! yet (Phase 1).  Layouts follow the USB 2.0 and 3.x specs, checked against
//! the fixtures in `docs/fixtures/`.

pub const DT_CONFIG: u8 = 0x02;
pub const DT_INTERFACE: u8 = 0x04;
pub const DT_ENDPOINT: u8 = 0x05;
pub const DT_HUB: u8 = 0x29;
pub const DT_SS_HUB: u8 = 0x2A;
pub const DT_SS_COMPANION: u8 = 0x30;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DescError {
    /// Input ends before the length the descriptor declares.
    Truncated,
    /// A bLength is below the minimum for its type, or overruns its container.
    BadLength,
    /// bDescriptorType does not match the expected type.
    WrongType,
    /// A field value is outside its legal range.
    Malformed,
}

fn u8_at(b: &[u8], i: usize) -> Result<u8, DescError> {
    b.get(i).copied().ok_or(DescError::Truncated)
}

fn u16_at(b: &[u8], i: usize) -> Result<u16, DescError> {
    let lo = u8_at(b, i)?;
    let hi = u8_at(b, i + 1)?;
    Ok(u16::from_le_bytes([lo, hi]))
}

/// Configuration descriptor header plus the body that follows it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Config<'a> {
    pub total_len: u16,
    pub num_interfaces: u8,
    pub config_value: u8,
    pub attributes: u8,
    pub max_power: u8,
    body: &'a [u8],
}

impl<'a> Config<'a> {
    /// Parse the 9-byte header.  `raw` must hold the whole set
    /// (`wTotalLength` bytes); a shorter slice is `Truncated`.
    pub fn parse(raw: &'a [u8]) -> Result<Self, DescError> {
        if u8_at(raw, 0)? != 9 {
            return Err(DescError::BadLength);
        }
        if u8_at(raw, 1)? != DT_CONFIG {
            return Err(DescError::WrongType);
        }
        let total_len = u16_at(raw, 2)?;
        let total = usize::from(total_len);
        if total < 9 {
            return Err(DescError::BadLength);
        }
        if raw.len() < total {
            return Err(DescError::Truncated);
        }
        let body = raw.get(9..total).ok_or(DescError::Truncated)?;
        Ok(Self {
            total_len,
            num_interfaces: u8_at(raw, 4)?,
            config_value: u8_at(raw, 5)?,
            attributes: u8_at(raw, 7)?,
            max_power: u8_at(raw, 8)?,
            body,
        })
    }

    /// Walk the interface and endpoint descriptors in order.
    pub fn descriptors(&self) -> Descriptors<'a> {
        Descriptors {
            body: self.body,
            pos: 0,
            done: false,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Interface {
    pub number: u8,
    pub alt_setting: u8,
    pub num_endpoints: u8,
    pub class: u8,
    pub subclass: u8,
    pub protocol: u8,
}

/// SuperSpeed Endpoint Companion (0x30), attached to the endpoint before it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Companion {
    pub max_burst: u8,
    pub attributes: u8,
    pub bytes_per_interval: u16,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Endpoint {
    pub address: u8,
    pub attributes: u8,
    pub max_packet: u16,
    pub interval: u8,
    pub companion: Option<Companion>,
}

impl Endpoint {
    pub const fn is_in(&self) -> bool {
        self.address & 0x80 != 0
    }
    pub const fn number(&self) -> u8 {
        self.address & 0x0F
    }
    /// 0 control, 1 isochronous, 2 bulk, 3 interrupt.
    pub const fn transfer_type(&self) -> u8 {
        self.attributes & 0x03
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Desc<'a> {
    Interface(Interface),
    Endpoint(Endpoint),
    /// Any other descriptor (including a 0x30 not directly after an endpoint).
    Other { kind: u8, data: &'a [u8] },
}

/// Iterator over the body of a configuration.  After an error it yields
/// nothing more.
pub struct Descriptors<'a> {
    body: &'a [u8],
    pos: usize,
    done: bool,
}

impl<'a> Descriptors<'a> {
    /// Validated frame at `pos`: (bLength, bDescriptorType, full bytes).
    fn frame(&self, pos: usize) -> Result<(usize, u8, &'a [u8]), DescError> {
        let len = usize::from(u8_at(self.body, pos)?);
        if len < 2 {
            return Err(DescError::BadLength);
        }
        let end = pos.checked_add(len).ok_or(DescError::BadLength)?;
        let data = self.body.get(pos..end).ok_or(DescError::BadLength)?;
        let kind = u8_at(data, 1)?;
        Ok((len, kind, data))
    }

    fn next_item(&mut self) -> Result<Desc<'a>, DescError> {
        let (len, kind, d) = self.frame(self.pos)?;
        self.pos += len;
        match kind {
            DT_INTERFACE => {
                if len < 9 {
                    return Err(DescError::BadLength);
                }
                Ok(Desc::Interface(Interface {
                    number: u8_at(d, 2)?,
                    alt_setting: u8_at(d, 3)?,
                    num_endpoints: u8_at(d, 4)?,
                    class: u8_at(d, 5)?,
                    subclass: u8_at(d, 6)?,
                    protocol: u8_at(d, 7)?,
                }))
            }
            DT_ENDPOINT => {
                if len < 7 {
                    return Err(DescError::BadLength);
                }
                let mut ep = Endpoint {
                    address: u8_at(d, 2)?,
                    attributes: u8_at(d, 3)?,
                    max_packet: u16_at(d, 4)?,
                    interval: u8_at(d, 6)?,
                    companion: None,
                };
                // Attach a directly following SS Endpoint Companion.
                if self.pos < self.body.len() {
                    let (clen, ckind, cd) = self.frame(self.pos)?;
                    if ckind == DT_SS_COMPANION {
                        if clen < 6 {
                            return Err(DescError::BadLength);
                        }
                        ep.companion = Some(Companion {
                            max_burst: u8_at(cd, 2)?,
                            attributes: u8_at(cd, 3)?,
                            bytes_per_interval: u16_at(cd, 4)?,
                        });
                        self.pos += clen;
                    }
                }
                Ok(Desc::Endpoint(ep))
            }
            _ => Ok(Desc::Other { kind, data: d }),
        }
    }
}

impl<'a> Iterator for Descriptors<'a> {
    type Item = Result<Desc<'a>, DescError>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.done || self.pos >= self.body.len() {
            return None;
        }
        let item = self.next_item();
        if item.is_err() {
            self.done = true;
        }
        Some(item)
    }
}

/// USB 2.0 hub descriptor (0x29).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Hub20 {
    pub num_ports: u8,
    pub characteristics: u16,
    /// bPwrOn2PwrGood, in units of 2 ms.
    pub pwr_on_2_pwr_good: u8,
    /// DeviceRemovable bitmap: bit n set means port n is non-removable.
    /// Bit 0 is reserved; port 1 is bit 1.
    pub removable: u16,
}

/// SuperSpeed hub descriptor (0x2A).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct HubSs {
    pub num_ports: u8,
    pub characteristics: u16,
    pub pwr_on_2_pwr_good: u8,
    pub hub_delay: u16,
    pub removable: u16,
}

/// Parse a USB 2.0 hub descriptor.  The DeviceRemovable bitmap is
/// `ceil((ports + 1) / 8)` bytes, and bDescLength must cover it.
pub fn parse_hub20(raw: &[u8]) -> Result<Hub20, DescError> {
    let len = usize::from(u8_at(raw, 0)?);
    if u8_at(raw, 1)? != DT_HUB {
        return Err(DescError::WrongType);
    }
    let num_ports = u8_at(raw, 2)?;
    if num_ports == 0 || num_ports > 15 {
        return Err(DescError::Malformed);
    }
    let removable_bytes = (usize::from(num_ports) + 1).div_ceil(8);
    if len < 7 + removable_bytes {
        return Err(DescError::BadLength);
    }
    if raw.len() < len {
        return Err(DescError::Truncated);
    }
    let mut removable = 0u16;
    for i in 0..removable_bytes {
        removable |= u16::from(u8_at(raw, 7 + i)?) << (8 * i);
    }
    Ok(Hub20 {
        num_ports,
        characteristics: u16_at(raw, 3)?,
        pwr_on_2_pwr_good: u8_at(raw, 5)?,
        removable,
    })
}

/// Parse a SuperSpeed hub descriptor.  Fixed 12-byte layout:
/// wHubDelay at 7..9 and a 16-bit DeviceRemovable bitmap at 9..11.
pub fn parse_hub_ss(raw: &[u8]) -> Result<HubSs, DescError> {
    let len = usize::from(u8_at(raw, 0)?);
    if u8_at(raw, 1)? != DT_SS_HUB {
        return Err(DescError::WrongType);
    }
    if len < 12 {
        return Err(DescError::BadLength);
    }
    if raw.len() < len {
        return Err(DescError::Truncated);
    }
    let num_ports = u8_at(raw, 2)?;
    if num_ports == 0 || num_ports > 15 {
        return Err(DescError::Malformed);
    }
    Ok(HubSs {
        num_ports,
        characteristics: u16_at(raw, 3)?,
        pwr_on_2_pwr_good: u8_at(raw, 5)?,
        hub_delay: u16_at(raw, 8)?,
        removable: u16_at(raw, 10)?,
    })
}

#[cfg(test)]
#[allow(clippy::panic, clippy::indexing_slicing, clippy::unwrap_used)]
mod tests {
    use super::*;

    /// docs/fixtures/qemu-usb-storage-bot.md, SuperSpeed set, total_len 44.
    const STORAGE_SS: [u8; 44] = [
        // Configuration
        0x09, 0x02, 0x2C, 0x00, 0x01, 0x01, 0x06, 0xC0, 0x00,
        // Interface 0: mass storage / SCSI / bulk-only
        0x09, 0x04, 0x00, 0x00, 0x02, 0x08, 0x06, 0x50, 0x00,
        // EP1 IN bulk, wMaxPacketSize 1024
        0x07, 0x05, 0x81, 0x02, 0x00, 0x04, 0x00,
        // SS Endpoint Companion: bMaxBurst 15
        0x06, 0x30, 0x0F, 0x00, 0x00, 0x00,
        // EP2 OUT bulk, wMaxPacketSize 1024
        0x07, 0x05, 0x02, 0x02, 0x00, 0x04, 0x00,
        // SS Endpoint Companion: bMaxBurst 15
        0x06, 0x30, 0x0F, 0x00, 0x00, 0x00,
    ];

    /// docs/fixtures/qemu-usb-hub.md, hub A 0x29 (8 ports, 10 bytes as emitted).
    const HUB_20: [u8; 10] = [0x0A, 0x29, 0x08, 0x09, 0x00, 0x01, 0x00, 0x00, 0x00, 0xFF];

    #[test]
    fn parses_storage_config_header() {
        let c = Config::parse(&STORAGE_SS).unwrap();
        assert_eq!(c.total_len, 44);
        assert_eq!(c.num_interfaces, 1);
        assert_eq!(c.config_value, 1);
        assert_eq!(c.attributes, 0xC0);
        assert_eq!(c.max_power, 0);
    }

    #[test]
    fn parses_storage_interface_and_endpoints_with_companions() {
        let c = Config::parse(&STORAGE_SS).unwrap();
        let items: Vec<Desc> = c.descriptors().map(|r| r.unwrap()).collect();
        assert_eq!(items.len(), 3);

        match items[0] {
            Desc::Interface(i) => {
                assert_eq!((i.class, i.subclass, i.protocol), (0x08, 0x06, 0x50));
                assert_eq!(i.num_endpoints, 2);
            }
            _ => panic!("expected interface"),
        }
        match items[1] {
            Desc::Endpoint(e) => {
                assert!(e.is_in());
                assert_eq!(e.number(), 1);
                assert_eq!(e.transfer_type(), 2);
                assert_eq!(e.max_packet, 1024);
                assert_eq!(e.interval, 0);
                let comp = e.companion.unwrap();
                assert_eq!(comp.max_burst, 15);
                assert_eq!(comp.bytes_per_interval, 0);
            }
            _ => panic!("expected endpoint"),
        }
        match items[2] {
            Desc::Endpoint(e) => {
                assert!(!e.is_in());
                assert_eq!(e.number(), 2);
                assert_eq!(e.transfer_type(), 2);
                assert_eq!(e.max_packet, 1024);
                assert_eq!(e.companion.map(|c| c.max_burst), Some(15));
            }
            _ => panic!("expected endpoint"),
        }
    }

    #[test]
    fn truncated_config_is_rejected() {
        assert_eq!(Config::parse(&STORAGE_SS[..40]).err(), Some(DescError::Truncated));
        assert_eq!(Config::parse(&STORAGE_SS[..5]).err(), Some(DescError::Truncated));
    }

    #[test]
    fn zero_endpoint_length_stops_iteration_with_error() {
        let mut raw = STORAGE_SS;
        raw[18] = 0; // EP1 bLength
        let c = Config::parse(&raw).unwrap();
        let mut it = c.descriptors();
        assert!(matches!(it.next(), Some(Ok(Desc::Interface(_)))));
        assert_eq!(it.next(), Some(Err(DescError::BadLength)));
        assert_eq!(it.next(), None);
    }

    #[test]
    fn endpoint_overrunning_config_is_rejected() {
        let mut raw = STORAGE_SS;
        raw[31] = 0x20; // EP2 bLength 32, past the 44-byte set
        let c = Config::parse(&raw).unwrap();
        let errs: Vec<_> = c.descriptors().filter_map(|r| r.err()).collect();
        assert_eq!(errs, vec![DescError::BadLength]);
    }

    #[test]
    fn parses_usb2_hub_descriptor() {
        let h = parse_hub20(&HUB_20).unwrap();
        assert_eq!(h.num_ports, 8);
        assert_eq!(h.characteristics, 0x0009);
        assert_eq!(h.pwr_on_2_pwr_good, 1);
        assert_eq!(h.removable, 0);
    }

    #[test]
    fn hub20_removable_bitmap_and_bad_inputs() {
        // Port 3 non-removable: bit 3 of the bitmap.
        let mut b = HUB_20;
        b[7] = 0x08;
        assert_eq!(parse_hub20(&b).unwrap().removable, 0x0008);
        assert_eq!(parse_hub20(&HUB_20[..9]).err(), Some(DescError::Truncated));
        let mut zero_ports = HUB_20;
        zero_ports[2] = 0;
        assert_eq!(parse_hub20(&zero_ports).err(), Some(DescError::Malformed));
        let mut short = HUB_20;
        short[0] = 7; // below 7 + 2 removable bytes
        assert_eq!(parse_hub20(&short).err(), Some(DescError::BadLength));
    }

    /// Synthetic SuperSpeed hub (no fixture exists): 4 ports, 12 bytes.
    const HUB_SS: [u8; 12] = [0x0C, 0x2A, 0x04, 0x08, 0x00, 0x32, 0x00, 0x00, 0x0A, 0x00, 0x02, 0x00];

    #[test]
    fn parses_superspeed_hub_descriptor() {
        let h = parse_hub_ss(&HUB_SS).unwrap();
        assert_eq!(h.num_ports, 4);
        assert_eq!(h.characteristics, 0x0008);
        assert_eq!(h.pwr_on_2_pwr_good, 0x32);
        assert_eq!(h.hub_delay, 0x000A);
        assert_eq!(h.removable, 0x0002);
        assert_eq!(parse_hub_ss(&HUB_SS[..11]).err(), Some(DescError::Truncated));
        let mut short = HUB_SS;
        short[0] = 6;
        assert_eq!(parse_hub_ss(&short).err(), Some(DescError::BadLength));
        assert_eq!(parse_hub20(&HUB_SS).err(), Some(DescError::WrongType));
    }
}
