//! GPU register and VRAM stub. GP0 packets are stored, not rasterized.

#![forbid(unsafe_code)]

pub const VRAM_WIDTH: usize = 1024;
pub const VRAM_HEIGHT: usize = 512;

/// Status value installed by the GP1 reset command. Not a full bit model.
pub const RESET_STATUS: u32 = 0x1480_2000;

#[derive(Clone, Debug)]
pub struct Gpu {
    vram: Vec<u16>,
    status: u32,
    gp0: Vec<u32>,
}

impl Default for Gpu {
    fn default() -> Self {
        Self::new()
    }
}

impl Gpu {
    pub fn new() -> Self {
        Self {
            vram: vec![0; VRAM_WIDTH * VRAM_HEIGHT],
            status: RESET_STATUS,
            gp0: Vec::new(),
        }
    }

    pub fn status(&self) -> u32 {
        self.status
    }

    pub fn write_gp0(&mut self, word: u32) {
        self.gp0.push(word);
    }

    pub fn gp0_len(&self) -> usize {
        self.gp0.len()
    }

    pub fn write_gp1(&mut self, word: u32) {
        if word >> 24 == 0x00 {
            self.reset();
        }
    }

    pub fn reset(&mut self) {
        self.gp0.clear();
        self.status = RESET_STATUS;
    }

    pub fn set_pixel(&mut self, x: usize, y: usize, pixel: u16) {
        if x < VRAM_WIDTH && y < VRAM_HEIGHT {
            self.vram[y * VRAM_WIDTH + x] = pixel;
        }
    }

    pub fn pixel(&self, x: usize, y: usize) -> u16 {
        self.vram[y * VRAM_WIDTH + x]
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn gp1_reset_clears_gp0_fifo() {
        let mut gpu = Gpu::new();
        gpu.write_gp0(0xE100_0000);
        gpu.write_gp1(0x0000_0000);
        assert_eq!(gpu.gp0_len(), 0);
        assert_eq!(gpu.status(), RESET_STATUS);
    }

    #[test]
    fn vram_pixel_roundtrip() {
        let mut gpu = Gpu::new();
        gpu.set_pixel(3, 4, 0x7C1F);
        assert_eq!(gpu.pixel(3, 4), 0x7C1F);
    }
}
