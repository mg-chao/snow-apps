//! Sparse premultiplied surfaces; video callers retain their contiguous destination.
use std::collections::BTreeMap;
use std::sync::Arc;

pub const TILE_SIZE: u32 = 128;
const TILE_BYTES: usize = (TILE_SIZE * TILE_SIZE * 4) as usize;

/// RGB channel order of a packed 4-byte-per-pixel video buffer. Effect
/// colors and keycap bitmaps are authored as RGBA; blending kernels swap
/// channels on the fly when composing into a BGRA buffer.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum PixelOrder {
    #[default]
    Rgba,
    Bgra,
}

pub trait Surface {
    const OPAQUE: bool;
    fn size(&self) -> (u32, u32);
    /// Visits in-bounds row spans. The offset is relative to the requested x.
    fn span(&mut self, x: u32, y: u32, width: u32, visit: impl FnMut(&mut [u8], usize));

    fn blend_index(&mut self, index: usize, color: [u8; 4]) {
        let width = self.size().0 as usize;
        self.blend_pixel((index % width) as i32, (index / width) as i32, color);
    }

    fn blend_pixel(&mut self, x: i32, y: i32, color: [u8; 4]) {
        let size = self.size();
        if x < 0 || y < 0 || x as u32 >= size.0 || y as u32 >= size.1 || color[3] == 0 {
            return;
        }
        self.span(x as u32, y as u32, 1, |pixel, _| {
            let alpha = u32::from(color[3]);
            for channel in 0..3 {
                pixel[channel] = ((u32::from(color[channel]) * alpha
                    + u32::from(pixel[channel]) * (255 - alpha)
                    + 127)
                    / 255) as u8;
            }
            pixel[3] = if Self::OPAQUE {
                255
            } else {
                (alpha + (u32::from(pixel[3]) * (255 - alpha) + 127) / 255) as u8
            };
        });
    }
}

pub struct RgbaSurface<'a> {
    pub pixels: &'a mut [u8],
    pub dimensions: (u32, u32),
}

impl Surface for RgbaSurface<'_> {
    const OPAQUE: bool = true;
    fn size(&self) -> (u32, u32) {
        self.dimensions
    }
    fn span(&mut self, x: u32, y: u32, width: u32, mut visit: impl FnMut(&mut [u8], usize)) {
        let start = (y as usize * self.dimensions.0 as usize + x as usize) * 4;
        visit(&mut self.pixels[start..start + width as usize * 4], 0);
    }
    fn blend_index(&mut self, index: usize, color: [u8; 4]) {
        let Some(pixel) = self.pixels.get_mut(index * 4..index * 4 + 4) else {
            return;
        };
        let alpha = u32::from(color[3]);
        for channel in 0..3 {
            pixel[channel] = ((u32::from(color[channel]) * alpha
                + u32::from(pixel[channel]) * (255 - alpha)
                + 127)
                / 255) as u8;
        }
        pixel[3] = 255;
    }
}

#[derive(Clone)]
pub struct Tile {
    pub x: u32,
    pub y: u32,
    pub pixels: Arc<Vec<u8>>,
}

pub struct TileSurface {
    size: (u32, u32),
    tiles: BTreeMap<(u32, u32), Arc<Vec<u8>>>,
    spare: Vec<Arc<Vec<u8>>>,
    retired: Vec<Arc<Vec<u8>>>,
}

impl TileSurface {
    pub fn new(size: (u32, u32)) -> Self {
        Self {
            size,
            tiles: BTreeMap::new(),
            spare: Vec::new(),
            retired: Vec::new(),
        }
    }
    pub fn clear(&mut self) {
        // Qt can still be displaying the preceding frame. Recycle it on a later frame,
        // once both the latest-frame slot and the presentation lease have released it.
        let capacity = self.tiles.len().clamp(4, 256);
        self.spare.truncate(capacity);
        let previous = std::mem::take(&mut self.retired);
        for mut pixels in previous
            .into_iter()
            .chain(std::mem::take(&mut self.tiles).into_values())
        {
            if let Some(data) = Arc::get_mut(&mut pixels) {
                if self.spare.len() < capacity {
                    data.fill(0);
                    self.spare.push(pixels);
                }
            } else if self.retired.len() < 512 {
                self.retired.push(pixels);
            }
        }
    }
    pub fn snapshot(&self) -> Vec<Tile> {
        self.tiles
            .iter()
            .filter(|(_, pixels)| pixels.chunks_exact(4).any(|p| p[3] != 0))
            .map(|(&(x, y), pixels)| Tile {
                x,
                y,
                pixels: Arc::clone(pixels),
            })
            .collect()
    }
    pub fn allocated_bytes(&self) -> usize {
        (self.tiles.len() + self.spare.len() + self.retired.len()) * TILE_BYTES
    }
}

impl Surface for TileSurface {
    const OPAQUE: bool = false;
    fn size(&self) -> (u32, u32) {
        self.size
    }
    fn span(&mut self, x: u32, y: u32, width: u32, mut visit: impl FnMut(&mut [u8], usize)) {
        let mut offset = 0;
        while offset < width {
            let px = x + offset;
            let key = (px / TILE_SIZE * TILE_SIZE, y / TILE_SIZE * TILE_SIZE);
            let count = (TILE_SIZE - px % TILE_SIZE).min(width - offset);
            let pixels = self.tiles.entry(key).or_insert_with(|| {
                self.spare
                    .pop()
                    .unwrap_or_else(|| Arc::new(vec![0; TILE_BYTES]))
            });
            let start = ((y % TILE_SIZE * TILE_SIZE + px % TILE_SIZE) * 4) as usize;
            visit(
                &mut Arc::make_mut(pixels)[start..start + count as usize * 4],
                offset as usize,
            );
            offset += count;
        }
    }
}
