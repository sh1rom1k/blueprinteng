use funpsx_bus::Mmio;
use funpsx_cdrom::Cdrom;
use funpsx_dma::Dma;
use funpsx_gpu::Gpu;
use funpsx_interrupts::{InterruptController, IrqLine};
use funpsx_spu::Spu;
use funpsx_timers::Timers;

pub struct Peripherals {
    irq: InterruptController,
    timers: Timers,
    dma: Dma,
    gpu: Gpu,
    spu: Spu,
    cdrom: Cdrom,
}

impl Peripherals {
    pub fn new() -> Self {
        Self {
            irq: InterruptController::new(),
            timers: Timers::new(),
            dma: Dma::new(),
            gpu: Gpu::new(),
            spu: Spu::new(),
            cdrom: Cdrom::new(),
        }
    }

    pub fn interrupts(&mut self) -> &mut InterruptController {
        &mut self.irq
    }

    pub fn timers(&mut self) -> &mut Timers {
        &mut self.timers
    }

    pub fn dma(&mut self) -> &mut Dma {
        &mut self.dma
    }

    pub fn gpu(&mut self) -> &mut Gpu {
        &mut self.gpu
    }

    pub fn spu(&mut self) -> &mut Spu {
        &mut self.spu
    }

    pub fn cdrom(&mut self) -> &mut Cdrom {
        &mut self.cdrom
    }
}

impl Default for Peripherals {
    fn default() -> Self {
        Self::new()
    }
}

impl Mmio for Peripherals {
    fn read8(&mut self, phys: u32) -> u8 {
        match phys {
            0x1F80_1800..=0x1F80_1803 => self.cdrom.read8(phys),
            0x1F80_1C00..=0x1F80_1FFF => self.spu.read8(phys),
            _ => (self.read32(phys & !3) >> ((phys & 3) * 8)) as u8,
        }
    }

    fn read16(&mut self, phys: u32) -> u16 {
        let phys = phys & !1;
        if (0x1F80_1800..=0x1F80_1803).contains(&phys) || (0x1F80_1C00..=0x1F80_1FFF).contains(&phys)
        {
            return u16::from(self.read8(phys)) | (u16::from(self.read8(phys + 1)) << 8);
        }
        self.read32(phys & !3) as u16
    }

    fn read32(&mut self, phys: u32) -> u32 {
        let phys = phys & !3;
        match phys {
            0x1F80_1070 => u32::from(self.irq.stat()),
            0x1F80_1074 => u32::from(self.irq.mask()),
            0x1F80_1810 => 0,
            0x1F80_1814 => self.gpu.status(),
            addr if (0x1F80_1080..=0x1F80_10F4).contains(&addr) => self.dma.read32(addr),
            addr if (0x1F80_1100..0x1F80_1130).contains(&addr) => self.timers.read32(addr),
            addr if (0x1F80_1800..=0x1F80_1803).contains(&addr) => {
                u32::from(self.cdrom.read8(addr))
                    | (u32::from(self.cdrom.read8(addr + 1)) << 8)
                    | (u32::from(self.cdrom.read8(addr + 2)) << 16)
                    | (u32::from(self.cdrom.read8(addr + 3)) << 24)
            }
            addr if (0x1F80_1C00..=0x1F80_1FFF).contains(&addr) => {
                u32::from(self.spu.read8(addr))
                    | (u32::from(self.spu.read8(addr + 1)) << 8)
                    | (u32::from(self.spu.read8(addr + 2)) << 16)
                    | (u32::from(self.spu.read8(addr + 3)) << 24)
            }
            _ => 0,
        }
    }

    fn write8(&mut self, phys: u32, value: u8) {
        match phys {
            0x1F80_1800..=0x1F80_1803 => self.cdrom.write8(phys, value),
            0x1F80_1C00..=0x1F80_1FFF => self.spu.write8(phys, value),
            0x1F80_1070 | 0x1F80_1071 => {
                let shift = (phys & 1) * 8;
                let keep = !((0xFFu32) << shift) | (u32::from(value) << shift);
                self.irq.acknowledge((u32::from(self.irq.stat()) & keep) as u16);
            }
            0x1F80_1074 | 0x1F80_1075 => {
                let shift = (phys & 1) * 8;
                let mask = 0xFFu32 << shift;
                let next = (u32::from(self.irq.mask()) & !mask) | (u32::from(value) << shift);
                self.irq.set_mask(next as u16);
            }
            _ => {}
        }
    }

    fn write16(&mut self, phys: u32, value: u16) {
        let phys = phys & !1;
        if (0x1F80_1800..=0x1F80_1803).contains(&phys) || (0x1F80_1C00..=0x1F80_1FFF).contains(&phys)
        {
            self.write8(phys, value as u8);
            self.write8(phys + 1, (value >> 8) as u8);
            return;
        }
        if phys == 0x1F80_1070 {
            self.irq.acknowledge(value);
        } else if phys == 0x1F80_1074 {
            self.irq.set_mask(value);
        }
    }

    fn write32(&mut self, phys: u32, value: u32) {
        let phys = phys & !3;
        match phys {
            0x1F80_1070 => self.irq.acknowledge(value as u16),
            0x1F80_1074 => self.irq.set_mask(value as u16),
            0x1F80_1810 => self.gpu.write_gp0(value),
            0x1F80_1814 => self.gpu.write_gp1(value),
            addr if (0x1F80_1080..=0x1F80_10F4).contains(&addr) => self.dma.write32(addr, value),
            addr if (0x1F80_1100..0x1F80_1130).contains(&addr) => self.timers.write32(addr, value),
            addr if (0x1F80_1800..=0x1F80_1803).contains(&addr) => {
                self.cdrom.write8(addr, value as u8);
                self.cdrom.write8(addr + 1, (value >> 8) as u8);
                self.cdrom.write8(addr + 2, (value >> 16) as u8);
                self.cdrom.write8(addr + 3, (value >> 24) as u8);
            }
            addr if (0x1F80_1C00..=0x1F80_1FFF).contains(&addr) => {
                self.spu.write8(addr, value as u8);
                self.spu.write8(addr + 1, (value >> 8) as u8);
                self.spu.write8(addr + 2, (value >> 16) as u8);
                self.spu.write8(addr + 3, (value >> 24) as u8);
            }
            _ => {}
        }
    }
}

pub fn raise(peripherals: &mut Peripherals, line: IrqLine) {
    peripherals.irq.raise(line);
}
