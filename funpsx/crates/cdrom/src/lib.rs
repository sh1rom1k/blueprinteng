//! CD-ROM controller stub: index, command byte, parameter FIFO, response FIFO.

#![forbid(unsafe_code)]

use std::collections::VecDeque;

#[derive(Clone, Debug)]
pub struct Cdrom {
    index: u8,
    last_command: Option<u8>,
    params: Vec<u8>,
    response: VecDeque<u8>,
}

impl Default for Cdrom {
    fn default() -> Self {
        Self::new()
    }
}

impl Cdrom {
    pub fn new() -> Self {
        Self {
            index: 0,
            last_command: None,
            params: Vec::new(),
            response: VecDeque::new(),
        }
    }

    pub fn last_command(&self) -> Option<u8> {
        self.last_command
    }

    pub fn push_response(&mut self, byte: u8) {
        self.response.push_back(byte);
    }

    pub fn read8(&mut self, addr: u32) -> u8 {
        match addr & 3 {
            0 => self.index,
            1 => self.response.pop_front().unwrap_or(0),
            _ => 0,
        }
    }

    pub fn write8(&mut self, addr: u32, value: u8) {
        match addr & 3 {
            0 => self.index = value & 3,
            1 if self.index == 0 => {
                self.last_command = Some(value);
                self.params.clear();
            }
            2 => self.params.push(value),
            _ => {}
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn command_and_response() {
        let mut cd = Cdrom::new();
        cd.write8(0x1F80_1800, 0);
        cd.write8(0x1F80_1801, 0x0A);
        cd.push_response(0x02);
        assert_eq!(cd.last_command(), Some(0x0A));
        assert_eq!(cd.read8(0x1F80_1801), 0x02);
        assert_eq!(cd.read8(0x1F80_1801), 0);
    }
}
