//! Geometry Transformation Engine register file. Commands are recorded, not executed.

#![forbid(unsafe_code)]

#[derive(Clone, Debug)]
pub struct Gte {
    data: [u32; 32],
    control: [u32; 32],
    last_command: Option<u32>,
}

impl Default for Gte {
    fn default() -> Self {
        Self::new()
    }
}

impl Gte {
    pub fn new() -> Self {
        Self {
            data: [0; 32],
            control: [0; 32],
            last_command: None,
        }
    }

    pub fn read_data(&self, index: u32) -> u32 {
        self.data[(index & 31) as usize]
    }

    pub fn write_data(&mut self, index: u32, value: u32) {
        self.data[(index & 31) as usize] = value;
    }

    pub fn read_control(&self, index: u32) -> u32 {
        self.control[(index & 31) as usize]
    }

    pub fn write_control(&mut self, index: u32, value: u32) {
        self.control[(index & 31) as usize] = value;
    }

    pub fn command(&mut self, word: u32) {
        self.last_command = Some(word);
    }

    pub fn last_command(&self) -> Option<u32> {
        self.last_command
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn data_reg_and_command_record() {
        let mut gte = Gte::new();
        gte.write_data(1, 0x100);
        assert_eq!(gte.read_data(1), 0x100);
        gte.command(0x0180_0001);
        assert_eq!(gte.last_command(), Some(0x0180_0001));
    }
}
