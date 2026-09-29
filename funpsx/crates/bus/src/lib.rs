//! Physical memory map and bus routing.
//!
//! BIOS bytes are supplied by the host. This crate does not open files.

#![forbid(unsafe_code)]

/// 2 MiB of main RAM.
pub const RAM_SIZE: usize = 2 * 1024 * 1024;
/// 1 KiB data cache scratchpad.
pub const SCRATCH_SIZE: usize = 1024;
/// 512 KiB BIOS ROM.
pub const BIOS_SIZE: usize = 512 * 1024;

/// Where a virtual address lands after the KSEG0/KSEG1 mask.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Region {
    MainRam,
    Expansion1,
    Scratchpad,
    Hardware,
    Expansion2,
    Bios,
    Unmapped,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Decoded {
    pub region: Region,
    pub offset: u32,
}

/// Mask KUSEG, KSEG0, and KSEG1 down to the 29-bit PS1 bus.
pub fn physical_address(addr: u32) -> u32 {
    addr & 0x1FFF_FFFF
}

pub fn decode(addr: u32) -> Decoded {
    let phys = physical_address(addr);
    if phys < 0x0080_0000 {
        Decoded {
            region: Region::MainRam,
            offset: phys & 0x001F_FFFF,
        }
    } else if (0x1F00_0000..0x1F80_0000).contains(&phys) {
        Decoded {
            region: Region::Expansion1,
            offset: phys - 0x1F00_0000,
        }
    } else if (0x1F80_0000..0x1F80_1000).contains(&phys) {
        Decoded {
            region: Region::Scratchpad,
            offset: phys & 0x3FF,
        }
    } else if (0x1F80_1000..0x1F80_2000).contains(&phys) {
        Decoded {
            region: Region::Hardware,
            offset: phys - 0x1F80_1000,
        }
    } else if (0x1F80_2000..0x1F80_3000).contains(&phys) {
        Decoded {
            region: Region::Expansion2,
            offset: phys - 0x1F80_2000,
        }
    } else if (0x1FC0_0000..0x1FC8_0000).contains(&phys) {
        Decoded {
            region: Region::Bios,
            offset: phys - 0x1FC0_0000,
        }
    } else {
        Decoded {
            region: Region::Unmapped,
            offset: phys,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum BiosError {
    TooLarge { len: usize },
    Io(String),
}

impl std::fmt::Display for BiosError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            BiosError::TooLarge { len } => write!(
                f,
                "BIOS image is {len} bytes; the PS1 ROM is at most {BIOS_SIZE} bytes"
            ),
            BiosError::Io(msg) => write!(f, "BIOS load failed: {msg}"),
        }
    }
}

impl std::error::Error for BiosError {}

/// Pad or reject a BIOS image. Callers that read a file live outside the core.
pub fn normalize_bios(bytes: &[u8]) -> Result<Vec<u8>, BiosError> {
    if bytes.len() > BIOS_SIZE {
        return Err(BiosError::TooLarge { len: bytes.len() });
    }
    let mut image = vec![0u8; BIOS_SIZE];
    image[..bytes.len()].copy_from_slice(bytes);
    Ok(image)
}

/// Host-supplied BIOS. The desktop binary implements this for a filesystem path.
pub trait BiosSource {
    fn read_image(&self) -> Result<Vec<u8>, BiosError>;
}

pub struct SliceBios<'a>(pub &'a [u8]);

impl BiosSource for SliceBios<'_> {
    fn read_image(&self) -> Result<Vec<u8>, BiosError> {
        normalize_bios(self.0)
    }
}

/// Memory-mapped I/O behind the hardware window. Implemented by [`funpsx_core`] peripherals.
pub trait Mmio {
    fn read8(&mut self, phys: u32) -> u8;
    fn read16(&mut self, phys: u32) -> u16;
    fn read32(&mut self, phys: u32) -> u32;
    fn write8(&mut self, phys: u32, value: u8);
    fn write16(&mut self, phys: u32, value: u16);
    fn write32(&mut self, phys: u32, value: u32);
}

/// MMIO stand-in for bus tests.
#[derive(Clone, Debug, Default)]
pub struct NullMmio;

impl Mmio for NullMmio {
    fn read8(&mut self, _phys: u32) -> u8 {
        0
    }
    fn read16(&mut self, _phys: u32) -> u16 {
        0
    }
    fn read32(&mut self, _phys: u32) -> u32 {
        0
    }
    fn write8(&mut self, _phys: u32, _value: u8) {}
    fn write16(&mut self, _phys: u32, _value: u16) {}
    fn write32(&mut self, _phys: u32, _value: u32) {}
}

#[derive(Clone, Debug)]
pub struct Bus<H> {
    ram: Vec<u8>,
    scratch: [u8; SCRATCH_SIZE],
    bios: Vec<u8>,
    hw: H,
}

impl<H: Mmio> Bus<H> {
    pub fn new(hw: H) -> Self {
        Self {
            ram: vec![0; RAM_SIZE],
            scratch: [0; SCRATCH_SIZE],
            bios: vec![0; BIOS_SIZE],
            hw,
        }
    }

    pub fn load_bios(&mut self, image: &[u8]) -> Result<(), BiosError> {
        self.bios = normalize_bios(image)?;
        Ok(())
    }

    pub fn hardware(&self) -> &H {
        &self.hw
    }

    pub fn hardware_mut(&mut self) -> &mut H {
        &mut self.hw
    }

    pub fn read8(&mut self, addr: u32) -> u8 {
        let decoded = decode(addr);
        match decoded.region {
            Region::MainRam => self.ram[decoded.offset as usize],
            Region::Scratchpad => self.scratch[decoded.offset as usize],
            Region::Bios => self.bios[decoded.offset as usize],
            Region::Hardware => self.hw.read8(physical_address(addr)),
            Region::Expansion1 | Region::Expansion2 | Region::Unmapped => 0xFF,
        }
    }

    pub fn read16(&mut self, addr: u32) -> u16 {
        let addr = addr & !1;
        if decode(addr).region == Region::Hardware {
            return self.hw.read16(physical_address(addr));
        }
        u16::from(self.read8(addr)) | (u16::from(self.read8(addr + 1)) << 8)
    }

    pub fn read32(&mut self, addr: u32) -> u32 {
        let addr = addr & !3;
        if decode(addr).region == Region::Hardware {
            return self.hw.read32(physical_address(addr));
        }
        u32::from(self.read8(addr))
            | (u32::from(self.read8(addr + 1)) << 8)
            | (u32::from(self.read8(addr + 2)) << 16)
            | (u32::from(self.read8(addr + 3)) << 24)
    }

    pub fn write8(&mut self, addr: u32, value: u8) {
        let decoded = decode(addr);
        match decoded.region {
            Region::MainRam => self.ram[decoded.offset as usize] = value,
            Region::Scratchpad => self.scratch[decoded.offset as usize] = value,
            Region::Hardware => self.hw.write8(physical_address(addr), value),
            Region::Bios | Region::Expansion1 | Region::Expansion2 | Region::Unmapped => {}
        }
    }

    pub fn write16(&mut self, addr: u32, value: u16) {
        let addr = addr & !1;
        if decode(addr).region == Region::Hardware {
            self.hw.write16(physical_address(addr), value);
            return;
        }
        self.write8(addr, value as u8);
        self.write8(addr + 1, (value >> 8) as u8);
    }

    pub fn write32(&mut self, addr: u32, value: u32) {
        let addr = addr & !3;
        if decode(addr).region == Region::Hardware {
            self.hw.write32(physical_address(addr), value);
            return;
        }
        self.write8(addr, value as u8);
        self.write8(addr + 1, (value >> 8) as u8);
        self.write8(addr + 2, (value >> 16) as u8);
        self.write8(addr + 3, (value >> 24) as u8);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn segment_aliases_and_ram_mirror() {
        assert_eq!(decode(0x0000_0000).region, Region::MainRam);
        assert_eq!(decode(0x8000_0010).offset, 0x10);
        assert_eq!(decode(0xA000_0010).region, Region::MainRam);
        assert_eq!(decode(0x0020_0004).offset, 0x4);
        assert_eq!(decode(0xBFC0_0000).region, Region::Bios);
        assert_eq!(decode(0x1F80_0008).region, Region::Scratchpad);
        assert_eq!(decode(0x1F80_1070).region, Region::Hardware);
    }

    #[test]
    fn kuseg_kseg0_and_kseg1_share_ram() {
        let mut bus = Bus::new(NullMmio);
        bus.write32(0x0000_0004, 0xAABB_CCDD);
        assert_eq!(bus.read32(0x8000_0004), 0xAABB_CCDD);
        assert_eq!(bus.read32(0xA020_0004), 0xAABB_CCDD);
    }

    #[test]
    fn bios_image_is_visible_at_the_reset_vector() {
        let mut bus = Bus::new(NullMmio);
        bus.load_bios(&[0x01, 0x02, 0x03, 0x04]).unwrap();
        assert_eq!(bus.read32(0xBFC0_0000), 0x0403_0201);
        assert!(normalize_bios(&vec![0; BIOS_SIZE + 1]).is_err());
    }

    #[test]
    fn scratchpad_mirrors_inside_its_window() {
        let mut bus = Bus::new(NullMmio);
        bus.write8(0x1F80_0001, 0x5A);
        assert_eq!(bus.read8(0x1F80_0401), 0x5A);
    }
}
