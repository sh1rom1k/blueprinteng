//! Interrupt status and mask (I_STAT / I_MASK).

#![forbid(unsafe_code)]

/// Hardware IRQ lines. The bit index matches the I_STAT bit.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum IrqLine {
    VBlank = 0,
    Gpu = 1,
    Cdrom = 2,
    Dma = 3,
    Timer0 = 4,
    Timer1 = 5,
    Timer2 = 6,
    Controller = 7,
    Sio = 8,
    Spu = 9,
    Lightpen = 10,
}

#[derive(Clone, Debug, Default)]
pub struct InterruptController {
    stat: u16,
    mask: u16,
}

impl InterruptController {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn raise(&mut self, line: IrqLine) {
        self.stat |= 1 << (line as u16);
    }

    pub fn stat(&self) -> u16 {
        self.stat
    }

    pub fn mask(&self) -> u16 {
        self.mask
    }

    pub fn set_mask(&mut self, mask: u16) {
        self.mask = mask & 0x07FF;
    }

    /// Writing I_STAT clears bits that are written as 0.
    pub fn acknowledge(&mut self, value: u16) {
        self.stat &= value;
    }

    pub fn pending(&self) -> bool {
        self.stat & self.mask != 0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mask_gates_pending() {
        let mut irq = InterruptController::new();
        irq.raise(IrqLine::VBlank);
        assert!(!irq.pending());
        irq.set_mask(1 << IrqLine::VBlank as u16);
        assert!(irq.pending());
        irq.acknowledge(!1);
        assert_eq!(irq.stat(), 0);
        assert!(!irq.pending());
    }
}
