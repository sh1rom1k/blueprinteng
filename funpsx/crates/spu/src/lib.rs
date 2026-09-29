//! SPU register file and 512 KiB sound RAM.

#![forbid(unsafe_code)]

pub const SOUND_RAM_SIZE: usize = 512 * 1024;
const REG_BASE: u32 = 0x1F80_1C00;
const REG_SIZE: usize = 0x400;

#[derive(Clone, Debug)]
pub struct Spu {
    ram: Vec<u8>,
    regs: [u8; REG_SIZE],
}

impl Default for Spu {
    fn default() -> Self {
        Self::new()
    }
}

impl Spu {
    pub fn new() -> Self {
        Self {
            ram: vec![0; SOUND_RAM_SIZE],
            regs: [0; REG_SIZE],
        }
    }

    pub fn read_ram(&self, offset: usize) -> u8 {
        self.ram[offset]
    }

    pub fn write_ram(&mut self, offset: usize, value: u8) {
        self.ram[offset] = value;
    }

    pub fn read8(&self, addr: u32) -> u8 {
        self.regs
            .get(reg_index(addr))
            .copied()
            .unwrap_or(0)
    }

    pub fn write8(&mut self, addr: u32, value: u8) {
        if let Some(index) = reg_index(addr).checked_sub(0).filter(|i| *i < REG_SIZE) {
            self.regs[index] = value;
        }
    }
}

fn reg_index(addr: u32) -> usize {
    (addr.wrapping_sub(REG_BASE) as usize) % REG_SIZE
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn main_volume_and_ram() {
        let mut spu = Spu::new();
        spu.write8(0x1F80_1D80, 0x34);
        spu.write8(0x1F80_1D81, 0x12);
        assert_eq!(spu.read8(0x1F80_1D80), 0x34);
        assert_eq!(spu.read8(0x1F80_1D81), 0x12);
        spu.write_ram(0, 0xAB);
        assert_eq!(spu.read_ram(0), 0xAB);
        assert_eq!(spu.ram.len(), SOUND_RAM_SIZE);
    }
}
