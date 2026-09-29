//! Frame presentation API. Backends live in other crates.

#![forbid(unsafe_code)]

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Color {
    pub r: f32,
    pub g: f32,
    pub b: f32,
    pub a: f32,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Frame {
    pub width: u32,
    pub height: u32,
    pub pixels_rgba: Vec<u8>,
    pub clear: Color,
}

#[derive(Debug, PartialEq, Eq)]
pub enum FrameError {
    Size { expected: usize, actual: usize },
}

impl Frame {
    pub fn blank(width: u32, height: u32, clear: Color) -> Self {
        Self {
            width,
            height,
            pixels_rgba: Vec::new(),
            clear,
        }
    }

    pub fn from_rgba(
        width: u32,
        height: u32,
        pixels_rgba: Vec<u8>,
        clear: Color,
    ) -> Result<Self, FrameError> {
        let expected = (width as usize)
            .saturating_mul(height as usize)
            .saturating_mul(4);
        if pixels_rgba.len() != expected {
            return Err(FrameError::Size {
                expected,
                actual: pixels_rgba.len(),
            });
        }
        Ok(Self {
            width,
            height,
            pixels_rgba,
            clear,
        })
    }
}

#[derive(Debug)]
pub enum RenderError {
    Frame(FrameError),
    Backend(String),
}

impl std::fmt::Display for RenderError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            RenderError::Frame(FrameError::Size { expected, actual }) => {
                write!(f, "frame is {actual} bytes, expected {expected}")
            }
            RenderError::Backend(msg) => write!(f, "{msg}"),
        }
    }
}

impl std::error::Error for RenderError {}

/// Host renderer. Core crates do not implement this.
pub trait Renderer {
    fn clear(&mut self, color: Color) -> Result<(), RenderError>;
    fn present(&mut self, frame: &Frame) -> Result<(), RenderError>;
    fn resize(&mut self, width: u32, height: u32);
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Record {
        color: Option<Color>,
    }

    impl Renderer for Record {
        fn clear(&mut self, color: Color) -> Result<(), RenderError> {
            self.color = Some(color);
            Ok(())
        }
        fn present(&mut self, frame: &Frame) -> Result<(), RenderError> {
            self.clear(frame.clear)
        }
        fn resize(&mut self, _width: u32, _height: u32) {}
    }

    #[test]
    fn frame_size_and_trait() {
        let err = Frame::from_rgba(2, 2, vec![0; 4], Color { r: 0.0, g: 0.0, b: 0.0, a: 1.0 });
        assert!(err.is_err());
        let mut recorder = Record { color: None };
        let frame = Frame::blank(1, 1, Color { r: 0.1, g: 0.2, b: 0.3, a: 1.0 });
        recorder.present(&frame).unwrap();
        assert_eq!(recorder.color.unwrap().b, 0.3);
    }
}
