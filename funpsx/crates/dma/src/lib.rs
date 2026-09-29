//! Seven DMA channels plus the control and interrupt registers.

#![forbid(unsafe_code)]

pub const CHANNELS: usize = 7;

#[derive(Clone, Debug, Default)]
pub struct Channel {
    base: u32,
    block: u32,
    control: u32,
}

#[derive(Clone, Debug)]
pub struct Dma {
    channels: [Channel; CHANNELS],
    control: u32,
    interrupt: u32,
}

impl Default for Dma {
    fn default() -> Self {
        Self::new()
    }
}

impl Dma {
    pub fn new() -> Self {
        Self {
            channels: std::array::from_fn(|_| Channel::default()),
            control: 0,
            interrupt: 0,
        }
    }

    pub fn read32(&self, addr: u32) -> u32 {
        let addr = addr & !3;
        match addr {
            0x1F80_10F0 => self.control,
            0x1F80_10F4 => self.interrupt,
            addr if (0x1F80_1080..0x1F80_10F0).contains(&addr) => {
                let (index, reg) = channel_reg(addr);
                let channel = &self.channels[index];
                match reg {
                    0 => channel.base,
                    4 => channel.block,
                    8 => channel.control,
                    _ => 0,
                }
            }
            _ => 0,
        }
    }

    pub fn write32(&mut self, addr: u32, value: u32) {
        let addr = addr & !3;
        match addr {
            0x1F80_10F0 => self.control = value,
            0x1F80_10F4 => self.interrupt = value,
            addr if (0x1F80_1080..0x1F80_10F0).contains(&addr) => {
                let (index, reg) = channel_reg(addr);
                let channel = &mut self.channels[index];
                match reg {
                    0 => channel.base = value & 0x00FF_FFFF,
                    4 => channel.block = value,
                    8 => channel.control = value,
                    _ => {}
                }
            }
            _ => {}
        }
    }

    /// CHCR bit 24 is the start/busy flag. The stub records it and does not copy bytes.
    pub fn channel_busy(&self, index: usize) -> bool {
        self.channels[index].control & (1 << 24) != 0
    }

    pub fn channel_base(&self, index: usize) -> u32 {
        self.channels[index].base
    }
}

fn channel_reg(addr: u32) -> (usize, u32) {
    let delta = addr - 0x1F80_1080;
    ((delta / 0x10) as usize, delta % 0x10)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn gpu_channel_base_and_start_bit() {
        let mut dma = Dma::new();
        dma.write32(0x1F80_10A0, 0x0001_2340);
        dma.write32(0x1F80_10A8, 1 << 24);
        assert_eq!(dma.channel_base(2), 0x0001_2340);
        assert!(dma.channel_busy(2));
        assert!(!dma.channel_busy(0));
    }
}
