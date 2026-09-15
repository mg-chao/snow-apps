use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum DesktopSpace {
    Points,
    PhysicalPixels,
}

/// Top-left origin, positive Y down. Coordinates never imply output pixel dimensions.
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
pub struct DesktopRect {
    pub space: DesktopSpace,
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct PixelSize {
    pub width: u32,
    pub height: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct PixelRect {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, thiserror::Error)]
#[error("invalid, overflowing, or incompatible geometry")]
pub struct GeometryError;

impl PixelSize {
    pub fn new(width: u32, height: u32) -> Result<Self, GeometryError> {
        if width == 0 || height == 0 {
            return Err(GeometryError);
        }
        Ok(Self { width, height })
    }

    pub fn byte_len(self, bytes_per_pixel: usize) -> Result<usize, GeometryError> {
        if self.width == 0 || self.height == 0 || bytes_per_pixel == 0 {
            return Err(GeometryError);
        }
        (self.width as usize)
            .checked_mul(self.height as usize)
            .and_then(|n| n.checked_mul(bytes_per_pixel))
            .filter(|&n| n <= isize::MAX as usize)
            .ok_or(GeometryError)
    }
}

impl DesktopRect {
    pub fn validate(self) -> Result<Self, GeometryError> {
        if ![
            self.x,
            self.y,
            self.width,
            self.height,
            self.right(),
            self.bottom(),
        ]
        .into_iter()
        .all(f64::is_finite)
            || self.width <= 0.0
            || self.height <= 0.0
            || self.right() <= self.x
            || self.bottom() <= self.y
        {
            return Err(GeometryError);
        }
        Ok(self)
    }

    pub fn right(self) -> f64 {
        self.x + self.width
    }
    pub fn bottom(self) -> f64 {
        self.y + self.height
    }

    pub fn intersection(self, other: Self) -> Result<Option<Self>, GeometryError> {
        self.validate()?;
        other.validate()?;
        if self.space != other.space {
            return Err(GeometryError);
        }
        let x = self.x.max(other.x);
        let y = self.y.max(other.y);
        let right = self.right().min(other.right());
        let bottom = self.bottom().min(other.bottom());
        Ok((right > x && bottom > y).then_some(Self {
            space: self.space,
            x,
            y,
            width: right - x,
            height: bottom - y,
        }))
    }

    pub fn pixels_at_scale(self, scale: f64) -> Result<PixelSize, GeometryError> {
        self.validate()?;
        if !scale.is_finite() || scale <= 0.0 {
            return Err(GeometryError);
        }
        PixelSize::new(
            pixel_edge(self.width * scale)?,
            pixel_edge(self.height * scale)?,
        )
    }
}

fn pixel_edge(value: f64) -> Result<u32, GeometryError> {
    let rounded = value.round();
    if !rounded.is_finite() || !(0.0..=u32::MAX as f64).contains(&rounded) {
        return Err(GeometryError);
    }
    Ok(rounded as u32)
}

#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
pub struct DesktopTransform {
    pub source: DesktopRect,
    pub output: PixelSize,
}

impl DesktopTransform {
    pub fn new(source: DesktopRect, output: PixelSize) -> Result<Self, GeometryError> {
        source.validate()?;
        output.byte_len(1)?;
        Ok(Self { source, output })
    }

    /// Shared edge rounding ensures adjacent display intersections meet exactly.
    pub fn project(self, rect: DesktopRect) -> Result<Option<PixelRect>, GeometryError> {
        let Some(rect) = self.source.intersection(rect)? else {
            return Ok(None);
        };
        let sx = self.output.width as f64 / self.source.width;
        let sy = self.output.height as f64 / self.source.height;
        let x = pixel_edge((rect.x - self.source.x) * sx)?.min(self.output.width);
        let y = pixel_edge((rect.y - self.source.y) * sy)?.min(self.output.height);
        let right = pixel_edge((rect.right() - self.source.x) * sx)?.min(self.output.width);
        let bottom = pixel_edge((rect.bottom() - self.source.y) * sy)?.min(self.output.height);
        Ok((right > x && bottom > y).then_some(PixelRect {
            x,
            y,
            width: right - x,
            height: bottom - y,
        }))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn rect(x: f64, width: f64) -> DesktopRect {
        DesktopRect {
            space: DesktopSpace::Points,
            x,
            y: -100.0,
            width,
            height: 100.0,
        }
    }
    #[test]
    fn fractional_edges_join_without_seams() {
        let t =
            DesktopTransform::new(rect(-150.5, 301.0), PixelSize::new(603, 200).unwrap()).unwrap();
        let a = t.project(rect(-150.5, 150.5)).unwrap().unwrap();
        let b = t.project(rect(0.0, 150.5)).unwrap().unwrap();
        assert_eq!(a.x + a.width, b.x);
        assert_eq!(b.x + b.width, 603);
    }
    #[test]
    fn invalid_geometry_and_space_mismatch_are_rejected() {
        assert!(rect(f64::NAN, 3.0).validate().is_err());
        assert!(rect(0.0, -1.0).validate().is_err());
        assert!(rect(0.0, 1.0).pixels_at_scale(f64::INFINITY).is_err());
        let mut other = rect(0.0, 1.0);
        other.space = DesktopSpace::PhysicalPixels;
        assert!(rect(0.0, 1.0).intersection(other).is_err());
        assert!(
            PixelSize {
                width: u32::MAX,
                height: u32::MAX
            }
            .byte_len(8)
            .is_err()
        );
    }
    #[test]
    fn clipping_and_desktop_gaps() {
        let t = DesktopTransform::new(rect(0.0, 100.0), PixelSize::new(200, 200).unwrap()).unwrap();
        assert_eq!(t.project(rect(-20.0, 40.0)).unwrap().unwrap().width, 40);
        assert!(t.project(rect(200.0, 20.0)).unwrap().is_none());
    }
}

/// Fit the complete source into fixed output dimensions without stretching.
/// Integer shared edges center the image; unused pixels are composition background.
pub fn aspect_fit(source: PixelSize, output: PixelSize) -> Result<PixelRect, GeometryError> {
    source.byte_len(1)?;
    output.byte_len(1)?;
    let width_limited = u64::from(output.width) * u64::from(source.height)
        <= u64::from(output.height) * u64::from(source.width);
    let (width, height) = if width_limited {
        (
            output.width,
            (u64::from(source.height) * u64::from(output.width) / u64::from(source.width)).max(1)
                as u32,
        )
    } else {
        (
            (u64::from(source.width) * u64::from(output.height) / u64::from(source.height)).max(1)
                as u32,
            output.height,
        )
    };
    Ok(PixelRect {
        x: (output.width - width) / 2,
        y: (output.height - height) / 2,
        width,
        height,
    })
}
#[cfg(test)]
#[test]
fn aspect_fit_preserves_fixed_canvas_during_source_resizing() {
    let canvas = PixelSize::new(1920, 1080).unwrap();
    assert_eq!(
        aspect_fit(PixelSize::new(100, 100).unwrap(), canvas).unwrap(),
        PixelRect {
            x: 420,
            y: 0,
            width: 1080,
            height: 1080
        }
    );
    assert_eq!(
        aspect_fit(PixelSize::new(3840, 2160).unwrap(), canvas).unwrap(),
        PixelRect {
            x: 0,
            y: 0,
            width: 1920,
            height: 1080
        }
    );
    assert_eq!(
        aspect_fit(PixelSize::new(u32::MAX, 1).unwrap(), canvas)
            .unwrap()
            .height,
        1
    );
}
