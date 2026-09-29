//! Host services. Desktop and future ports implement these types.

#![forbid(unsafe_code)]

use std::collections::VecDeque;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Size {
    pub width: u32,
    pub height: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HostKey {
    Escape,
    Other,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum HostEvent {
    CloseRequested,
    Resized(Size),
    KeyPressed(HostKey),
    RedrawRequested,
}

#[derive(Clone, Debug)]
pub struct InputState {
    pub close_requested: bool,
    pub size: Size,
    pub keys: Vec<HostKey>,
}

impl InputState {
    pub fn new(size: Size) -> Self {
        Self {
            close_requested: false,
            size,
            keys: Vec::new(),
        }
    }

    pub fn apply(&mut self, event: &HostEvent) {
        match event {
            HostEvent::CloseRequested => self.close_requested = true,
            HostEvent::Resized(size) => self.size = *size,
            HostEvent::KeyPressed(key) => self.keys.push(*key),
            HostEvent::RedrawRequested => {}
        }
    }
}

pub trait HostWindow {
    fn title(&self) -> &str;
    fn size(&self) -> Size;
}

pub trait AudioOutput {
    fn sample_rate(&self) -> u32;
    fn push_interleaved_stereo(&mut self, samples: &[i16]);
    fn queued_frames(&self) -> usize;
}

/// Stereo queue used by the desktop host until a device backend is attached.
#[derive(Clone, Debug)]
pub struct RingAudio {
    rate: u32,
    capacity_frames: usize,
    samples: VecDeque<i16>,
}

impl RingAudio {
    pub fn new(sample_rate: u32, capacity_frames: usize) -> Self {
        Self {
            rate: sample_rate,
            capacity_frames,
            samples: VecDeque::new(),
        }
    }
}

impl AudioOutput for RingAudio {
    fn sample_rate(&self) -> u32 {
        self.rate
    }

    fn push_interleaved_stereo(&mut self, samples: &[i16]) {
        self.samples.extend(samples.iter().copied());
        let max_samples = self.capacity_frames.saturating_mul(2);
        while self.samples.len() > max_samples {
            self.samples.pop_front();
        }
    }

    fn queued_frames(&self) -> usize {
        self.samples.len() / 2
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn input_tracks_resize_and_close() {
        let mut input = InputState::new(Size { width: 640, height: 480 });
        input.apply(&HostEvent::Resized(Size { width: 320, height: 240 }));
        input.apply(&HostEvent::KeyPressed(HostKey::Escape));
        input.apply(&HostEvent::CloseRequested);
        assert!(input.close_requested);
        assert_eq!(input.size.width, 320);
        assert_eq!(input.keys, vec![HostKey::Escape]);
    }

    #[test]
    fn ring_audio_drops_oldest_frames() {
        let mut audio = RingAudio::new(44100, 2);
        audio.push_interleaved_stereo(&[1, 2, 3, 4, 5, 6]);
        assert_eq!(audio.queued_frames(), 2);
        assert_eq!(audio.sample_rate(), 44100);
    }
}
