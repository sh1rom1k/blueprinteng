//! Machine glue. No windowing, audio devices, or OpenGL.

#![forbid(unsafe_code)]

mod peripherals;

use funpsx_bus::{BiosError, Bus};
use funpsx_cpu::{decode, Cpu, Opcode};
use funpsx_gte::Gte;
use peripherals::Peripherals;

pub use funpsx_bus::{BiosSource, Region};
pub use funpsx_cpu::Cpu as R3000a;
pub use funpsx_gte::Gte as Cop2;

pub struct Machine {
    cpu: Cpu,
    gte: Gte,
    bus: Bus<Peripherals>,
}

impl Default for Machine {
    fn default() -> Self {
        Self::new()
    }
}

impl Machine {
    pub fn new() -> Self {
        Self::with_bios(&[]).expect("empty BIOS image")
    }

    pub fn with_bios(image: &[u8]) -> Result<Self, BiosError> {
        let mut bus = Bus::new(Peripherals::new());
        bus.load_bios(image)?;
        Ok(Self {
            cpu: Cpu::new(),
            gte: Gte::new(),
            bus,
        })
    }

    pub fn cpu(&self) -> &Cpu {
        &self.cpu
    }

    pub fn cpu_mut(&mut self) -> &mut Cpu {
        &mut self.cpu
    }

    pub fn gte(&self) -> &Gte {
        &self.gte
    }

    pub fn gte_mut(&mut self) -> &mut Gte {
        &mut self.gte
    }

    pub fn bus(&mut self) -> &mut Bus<Peripherals> {
        &mut self.bus
    }

    /// Fetch one instruction and execute the stub opcode set. Delay slots are not modeled.
    pub fn step(&mut self) {
        let pc = self.cpu.pc();
        let word = self.bus.read32(pc);
        match decode(word) {
            Opcode::Nop | Opcode::Unknown(_) => {}
            Opcode::Lui { rt, imm } => self.cpu.set_gpr(rt, u32::from(imm) << 16),
            Opcode::Addiu { rs, rt, imm } => {
                let value = self.cpu.gpr(rs).wrapping_add(imm as u32);
                self.cpu.set_gpr(rt, value);
            }
            Opcode::Ori { rs, rt, imm } => {
                let value = self.cpu.gpr(rs) | u32::from(imm);
                self.cpu.set_gpr(rt, value);
            }
            Opcode::Lw { rs, rt, imm } => {
                let addr = self.cpu.gpr(rs).wrapping_add(imm as u32);
                let value = self.bus.read32(addr);
                self.cpu.set_gpr(rt, value);
            }
            Opcode::Sw { rs, rt, imm } => {
                let addr = self.cpu.gpr(rs).wrapping_add(imm as u32);
                let value = self.cpu.gpr(rt);
                self.bus.write32(addr, value);
            }
        }
        self.cpu.set_pc(pc.wrapping_add(4));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use funpsx_interrupts::IrqLine;

    #[test]
    fn stub_program_stores_through_the_bus() {
        let mut machine = Machine::new();
        machine.cpu_mut().set_pc(0);
        // lui r1, 1; ori r1, r1, 0x2340; sw r1, 0(r0)
        machine.bus().write32(0x0000_0000, 0x3C01_0001);
        machine.bus().write32(0x0000_0004, 0x3421_2340);
        machine.bus().write32(0x0000_0008, 0xAC21_0000);
        machine.step();
        machine.step();
        machine.step();
        assert_eq!(machine.cpu().gpr(1), 0x0001_2340);
        assert_eq!(machine.bus().read32(0), 0x0001_2340);
        assert_eq!(machine.cpu().pc(), 12);
    }

    #[test]
    fn irq_mask_is_reachable_through_kseg1() {
        let mut machine = Machine::new();
        machine.bus().hardware_mut().interrupts().raise(IrqLine::VBlank);
        machine.bus().write32(0xBF80_1074, 1);
        assert_eq!(machine.bus().read32(0x1F80_1070) & 1, 1);
        assert!(machine.bus().hardware_mut().interrupts().pending());
    }
}
