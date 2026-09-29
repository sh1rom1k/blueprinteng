//! Three programmable timers. Counting is a stub, not cycle-accurate.

#![forbid(unsafe_code)]

#[derive(Clone, Debug, Default)]
pub struct Timer {
    counter: u16,
    mode: u16,
    target: u16,
}

#[derive(Clone, Debug)]
pub struct Timers {
    timers: [Timer; 3],
}

impl Default for Timers {
    fn default() -> Self {
        Self::new()
    }
}

impl Timers {
    pub fn new() -> Self {
        Self {
            timers: Default::default(),
        }
    }

    pub fn read32(&self, addr: u32) -> u32 {
        let Some((timer, reg)) = locate(addr) else {
            return 0;
        };
        match reg {
            0 => u32::from(self.timers[timer].counter),
            4 => u32::from(self.timers[timer].mode),
            8 => u32::from(self.timers[timer].target),
            _ => 0,
        }
    }

    pub fn write32(&mut self, addr: u32, value: u32) {
        let Some((timer, reg)) = locate(addr) else {
            return;
        };
        let timer = &mut self.timers[timer];
        match reg {
            0 => timer.counter = value as u16,
            4 => timer.mode = value as u16,
            8 => timer.target = value as u16,
            _ => {}
        }
    }

    /// Advance one timer.
    ///
    /// Mode bits honored here: bit 3 resets the counter when it reaches the
    /// target, bit 4 reports an IRQ on that compare. Other bits are stored.
    pub fn tick(&mut self, index: usize, ticks: u32) -> bool {
        let timer = &mut self.timers[index];
        let target = u32::from(timer.target);
        if target == 0 {
            timer.counter = timer.counter.wrapping_add(ticks as u16);
            return false;
        }
        let sum = u32::from(timer.counter) + ticks;
        let hit = sum >= target;
        if hit && timer.mode & (1 << 3) != 0 {
            timer.counter = (sum % target) as u16;
        } else {
            timer.counter = (sum & 0xFFFF) as u16;
        }
        hit && timer.mode & (1 << 4) != 0
    }
}

fn locate(addr: u32) -> Option<(usize, u32)> {
    const BASE: u32 = 0x1F80_1100;
    let addr = addr & !3;
    if !(BASE..BASE + 0x30).contains(&addr) {
        return None;
    }
    let delta = addr - BASE;
    Some(((delta / 0x10) as usize, delta % 0x10))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn target_irq_and_reset() {
        let mut timers = Timers::new();
        timers.write32(0x1F80_1108, 10);
        timers.write32(0x1F80_1104, (1 << 3) | (1 << 4));
        assert!(timers.tick(0, 10));
        assert_eq!(timers.read32(0x1F80_1100), 0);
    }
}
