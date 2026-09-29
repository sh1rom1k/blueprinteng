//! MIPS R3000A stub. Register file, a few opcodes, no pipeline.

#![forbid(unsafe_code)]

/// Reset vector in the BIOS ROM (KSEG1).
pub const RESET_PC: u32 = 0xBFC0_0000;

/// General-purpose register and COP0 state.
#[derive(Clone, Debug)]
pub struct Cpu {
    regs: [u32; 32],
    pc: u32,
    hi: u32,
    lo: u32,
    cop0: [u32; 32],
}

impl Default for Cpu {
    fn default() -> Self {
        Self::new()
    }
}

impl Cpu {
    pub fn new() -> Self {
        let mut cpu = Self {
            regs: [0; 32],
            pc: RESET_PC,
            hi: 0,
            lo: 0,
            cop0: [0; 32],
        };
        cpu.reset();
        cpu
    }

    pub fn reset(&mut self) {
        self.regs = [0; 32];
        self.pc = RESET_PC;
        self.hi = 0;
        self.lo = 0;
        self.cop0 = [0; 32];
    }

    pub fn pc(&self) -> u32 {
        self.pc
    }

    pub fn set_pc(&mut self, pc: u32) {
        self.pc = pc;
    }

    pub fn hi(&self) -> u32 {
        self.hi
    }

    pub fn lo(&self) -> u32 {
        self.lo
    }

    pub fn gpr(&self, index: u8) -> u32 {
        self.regs[(index as usize) & 31]
    }

    /// `r0` stays zero.
    pub fn set_gpr(&mut self, index: u8, value: u32) {
        let index = (index as usize) & 31;
        if index != 0 {
            self.regs[index] = value;
        }
    }

    pub fn read_cop0(&self, index: u8) -> u32 {
        self.cop0[(index as usize) & 31]
    }

    pub fn write_cop0(&mut self, index: u8, value: u32) {
        self.cop0[(index as usize) & 31] = value;
    }
}

/// Opcodes the stub interpreter understands. Everything else is [`Opcode::Unknown`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Opcode {
    Nop,
    Lui { rt: u8, imm: u16 },
    Addiu { rs: u8, rt: u8, imm: i16 },
    Ori { rs: u8, rt: u8, imm: u16 },
    Lw { rs: u8, rt: u8, imm: i16 },
    Sw { rs: u8, rt: u8, imm: i16 },
    Unknown(u32),
}

pub fn decode(word: u32) -> Opcode {
    if word == 0 {
        return Opcode::Nop;
    }
    let op = word >> 26;
    let rs = ((word >> 21) & 31) as u8;
    let rt = ((word >> 16) & 31) as u8;
    let imm = word as u16;
    match op {
        0x09 => Opcode::Addiu {
            rs,
            rt,
            imm: imm as i16,
        },
        0x0D => Opcode::Ori { rs, rt, imm },
        0x0F => Opcode::Lui { rt, imm },
        0x23 => Opcode::Lw {
            rs,
            rt,
            imm: imm as i16,
        },
        0x2B => Opcode::Sw {
            rs,
            rt,
            imm: imm as i16,
        },
        _ => Opcode::Unknown(word),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reset_vector_and_r0() {
        let mut cpu = Cpu::new();
        assert_eq!(cpu.pc(), RESET_PC);
        cpu.set_gpr(0, 0xFFFF_FFFF);
        assert_eq!(cpu.gpr(0), 0);
        cpu.set_gpr(1, 0x11);
        assert_eq!(cpu.gpr(1), 0x11);
    }

    #[test]
    fn decode_nop_lui_and_unknown() {
        assert_eq!(decode(0), Opcode::Nop);
        assert_eq!(
            decode(0x3C01_0001),
            Opcode::Lui { rt: 1, imm: 1 }
        );
        assert_eq!(decode(0x0000_0008), Opcode::Unknown(0x0000_0008));
    }

    #[test]
    fn cop0_roundtrip() {
        let mut cpu = Cpu::new();
        cpu.write_cop0(12, 0x1000_0000);
        assert_eq!(cpu.read_cop0(12), 0x1000_0000);
    }
}
