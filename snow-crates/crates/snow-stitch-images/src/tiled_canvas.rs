use snow_memory::RasterBuffer;
use std::collections::VecDeque;
use std::sync::Arc;

use crate::{Frame, PixelFormat, StitchAxis, StitchError};

pub const CANVAS_TILE_SPAN: u32 = 256;
pub const CANVAS_TILE_ROWS: u32 = CANVAS_TILE_SPAN;

// Active scrolling can replace the trailing band repeatedly. Keep only a small
// owner-local working set, never tiles leased by immutable snapshots.
const MAX_SPARE_TILES: usize = 2;
const MAX_SPARE_TILE_BYTES: usize = 8 * 1024 * 1024;

fn tile_capacity_limit(length: usize) -> usize {
    // Match the raster allocator's bounded growth headroom plus its SIMD tail.
    length.saturating_add(length / 8).saturating_add(64)
}

#[derive(Debug, Clone)]
struct CanvasTile {
    pixels: RasterBuffer,
    span: u32,
}

#[derive(Debug, Clone)]
pub struct TiledCanvasSnapshot {
    axis: StitchAxis,
    cross_extent: u32,
    pixel_format: PixelFormat,
    extent: u32,
    tiles: VecDeque<Arc<CanvasTile>>,
    start: u32,
    end: u32,
}

#[derive(Debug)]
pub struct TiledCanvas {
    axis: StitchAxis,
    cross_extent: u32,
    pixel_format: PixelFormat,
    extent: u32,
    tiles: VecDeque<Arc<CanvasTile>>,
    spare_tiles: Vec<CanvasTile>,
}

impl Clone for TiledCanvas {
    fn clone(&self) -> Self {
        Self {
            axis: self.axis,
            cross_extent: self.cross_extent,
            pixel_format: self.pixel_format,
            extent: self.extent,
            tiles: self.tiles.clone(),
            spare_tiles: Vec::new(),
        }
    }
}

impl TiledCanvas {
    pub fn new(frame: Frame) -> Result<Self, StitchError> {
        Self::new_for_axis(frame, StitchAxis::Vertical)
    }

    pub fn new_for_axis(frame: Frame, axis: StitchAxis) -> Result<Self, StitchError> {
        let cross_extent = match axis {
            StitchAxis::Vertical => frame.width(),
            StitchAxis::Horizontal => frame.height(),
        };
        let frame_extent = axis.primary_extent(frame.width(), frame.height());
        let mut canvas = Self {
            axis,
            cross_extent,
            pixel_format: frame.pixel_format(),
            extent: 0,
            tiles: VecDeque::new(),
            spare_tiles: Vec::new(),
        };
        canvas.append_axis(&frame, 0, frame_extent)?;
        Ok(canvas)
    }

    pub const fn axis(&self) -> StitchAxis {
        self.axis
    }

    pub const fn width(&self) -> u32 {
        match self.axis {
            StitchAxis::Vertical => self.cross_extent,
            StitchAxis::Horizontal => self.extent,
        }
    }

    pub const fn height(&self) -> u32 {
        match self.axis {
            StitchAxis::Vertical => self.extent,
            StitchAxis::Horizontal => self.cross_extent,
        }
    }

    pub const fn extent(&self) -> u32 {
        self.extent
    }

    pub const fn pixel_format(&self) -> PixelFormat {
        self.pixel_format
    }

    fn channels(&self) -> usize {
        self.pixel_format.channels() as usize
    }

    fn validate_axis_range(&self, frame: &Frame, start: u32, end: u32) -> Result<(), StitchError> {
        let frame_cross = match self.axis {
            StitchAxis::Vertical => frame.width(),
            StitchAxis::Horizontal => frame.height(),
        };
        let frame_extent = self.axis.primary_extent(frame.width(), frame.height());
        if frame_cross != self.cross_extent
            || frame.pixel_format() != self.pixel_format
            || start > end
            || end > frame_extent
        {
            return Err(StitchError::InvalidFrame {
                message: "tiled canvas source does not match canvas geometry".to_owned(),
            });
        }
        Ok(())
    }

    fn take_tile_pixels(&mut self, span: u32) -> RasterBuffer {
        let length = span as usize * self.cross_extent as usize * self.channels();
        if let Some(index) = self.spare_tiles.iter().position(|tile| {
            tile.pixels.capacity() >= length
                && (tile.span == span || tile.pixels.capacity() <= tile_capacity_limit(length))
        }) {
            let mut pixels = self.spare_tiles.swap_remove(index).pixels;
            pixels.resize_for_overwrite(length);
            pixels
        } else {
            RasterBuffer::zeroed(length)
        }
    }

    fn recycle_tile(&mut self, tile: Arc<CanvasTile>) {
        if self.spare_tiles.len() >= MAX_SPARE_TILES {
            return;
        }
        let capacity = tile.pixels.capacity();
        let retained: usize = self
            .spare_tiles
            .iter()
            .map(|tile| tile.pixels.capacity())
            .sum();
        if capacity > MAX_SPARE_TILE_BYTES.saturating_sub(retained) {
            return;
        }
        // A snapshot or cloned canvas owns its own lease. Those pixels must stay
        // immutable; they are released by the final lease instead of recycled.
        if let Ok(tile) = Arc::try_unwrap(tile) {
            self.spare_tiles.push(tile);
        }
    }

    fn make_tiles(
        &mut self,
        frame: &Frame,
        start: u32,
        end: u32,
    ) -> Result<VecDeque<Arc<CanvasTile>>, StitchError> {
        self.validate_axis_range(frame, start, end)?;
        let channels = self.channels();
        let source = frame.pixels();
        let mut tiles = VecDeque::new();
        let mut cursor = start;
        while cursor < end {
            let span = CANVAS_TILE_SPAN.min(end - cursor);
            let mut pixels = self.take_tile_pixels(span);
            let output = pixels.as_mut_slice();
            match self.axis {
                StitchAxis::Vertical => {
                    let row_bytes = frame.width() as usize * channels;
                    output.copy_from_slice(
                        &source[cursor as usize * row_bytes..(cursor + span) as usize * row_bytes],
                    );
                }
                StitchAxis::Horizontal => {
                    let span_bytes = span as usize * channels;
                    let source_row_bytes = frame.width() as usize * channels;
                    let first = cursor as usize * channels;
                    for (row, target) in source
                        .chunks_exact(source_row_bytes)
                        .zip(output.chunks_exact_mut(span_bytes))
                    {
                        target.copy_from_slice(&row[first..first + span_bytes]);
                    }
                }
            }
            tiles.push_back(Arc::new(CanvasTile { pixels, span }));
            cursor += span;
        }
        Ok(tiles)
    }

    pub fn append_axis(&mut self, frame: &Frame, start: u32, end: u32) -> Result<(), StitchError> {
        self.validate_axis_range(frame, start, end)?;
        let new_extent = self
            .extent
            .checked_add(end - start)
            .ok_or(StitchError::Arithmetic {
                operation: "calculating tiled canvas extent",
            })?;
        let mut tiles = self.make_tiles(frame, start, end)?;
        self.tiles.append(&mut tiles);
        self.extent = new_extent;
        Ok(())
    }

    pub fn prepend_axis(&mut self, frame: &Frame, start: u32, end: u32) -> Result<(), StitchError> {
        self.validate_axis_range(frame, start, end)?;
        let new_extent = self
            .extent
            .checked_add(end - start)
            .ok_or(StitchError::Arithmetic {
                operation: "calculating tiled canvas extent",
            })?;
        let mut tiles = self.make_tiles(frame, start, end)?;
        while let Some(tile) = tiles.pop_back() {
            self.tiles.push_front(tile);
        }
        self.extent = new_extent;
        Ok(())
    }

    pub fn truncate_end(&mut self, new_extent: u32) -> Result<(), StitchError> {
        if new_extent > self.extent {
            return Err(StitchError::InvalidFrame {
                message: "cannot extend canvas while truncating its end".to_owned(),
            });
        }
        while self.extent > new_extent {
            let Some(last) = self.tiles.back() else {
                break;
            };
            let remove = self.extent - new_extent;
            if last.span <= remove {
                self.extent -= last.span;
                let removed = self.tiles.pop_back().expect("trailing tile exists");
                self.recycle_tile(removed);
                continue;
            }
            let keep = last.span - remove;
            self.slice_tile_in_place(false, 0, keep)?;
            self.extent = new_extent;
        }
        if self.extent == 0 {
            self.spare_tiles.clear();
        }
        Ok(())
    }

    pub fn truncate_start(&mut self, span: u32) -> Result<(), StitchError> {
        if span > self.extent {
            return Err(StitchError::InvalidFrame {
                message: "cannot remove more than the canvas extent".to_owned(),
            });
        }
        let mut remove = span;
        while remove > 0 {
            let Some(front) = self.tiles.front() else {
                break;
            };
            if front.span <= remove {
                remove -= front.span;
                let removed = self.tiles.pop_front().expect("leading tile exists");
                self.recycle_tile(removed);
                continue;
            }
            self.slice_tile_in_place(true, remove, front.span)?;
            remove = 0;
        }
        self.extent -= span;
        if self.extent == 0 {
            self.spare_tiles.clear();
        }
        Ok(())
    }

    fn slice_tile_in_place(
        &mut self,
        front: bool,
        start: u32,
        end: u32,
    ) -> Result<(), StitchError> {
        let channels = self.channels();
        let selected = if front {
            self.tiles.front_mut()
        } else {
            self.tiles.back_mut()
        }
        .expect("partially retained tile remains present");
        let length = (end - start) as usize * self.cross_extent as usize * channels;
        // Do not let repeated small crops leave full tiles behind tiny spans.
        // Minor trims may retain the existing active allocation; larger trims
        // release its oversized capacity once the last snapshot lease drops.
        if selected.pixels.capacity() <= tile_capacity_limit(length)
            && let Some(tile) = Arc::get_mut(selected)
        {
            let pixels = tile.pixels.as_mut_slice();
            match self.axis {
                StitchAxis::Vertical => {
                    let row_bytes = self.cross_extent as usize * channels;
                    pixels.copy_within(start as usize * row_bytes..end as usize * row_bytes, 0);
                }
                StitchAxis::Horizontal => {
                    let source_row_bytes = tile.span as usize * channels;
                    let row_bytes = (end - start) as usize * channels;
                    for y in 0..self.cross_extent as usize {
                        let source = y * source_row_bytes + start as usize * channels;
                        pixels.copy_within(source..source + row_bytes, y * row_bytes);
                    }
                }
            }
            tile.pixels.truncate(length);
            tile.span = end - start;
            return Ok(());
        }
        let tile = if front {
            self.tiles.front()
        } else {
            self.tiles.back()
        }
        .expect("partially retained tile remains present");
        let replacement = Arc::new(self.tile_slice(tile, start, end)?);
        let selected = if front {
            self.tiles.front_mut()
        } else {
            self.tiles.back_mut()
        }
        .expect("partially retained tile remains present");
        *selected = replacement;
        Ok(())
    }

    fn tile_slice(
        &self,
        tile: &CanvasTile,
        start: u32,
        end: u32,
    ) -> Result<CanvasTile, StitchError> {
        debug_assert!(start < end && end <= tile.span);
        let channels = self.channels();
        let pixels = match self.axis {
            StitchAxis::Vertical => {
                let row_bytes = self.cross_extent as usize * channels;
                RasterBuffer::from(
                    &tile.pixels[start as usize * row_bytes..end as usize * row_bytes],
                )
            }
            StitchAxis::Horizontal => {
                let source_row_bytes = tile.span as usize * channels;
                let slice_row_bytes = (end - start) as usize * channels;
                let mut pixels = RasterBuffer::zeroed(slice_row_bytes * self.cross_extent as usize);
                let source = tile.pixels.as_slice();
                for (y, target) in pixels
                    .as_mut_slice()
                    .chunks_exact_mut(slice_row_bytes)
                    .enumerate()
                {
                    let first = y * source_row_bytes + start as usize * channels;
                    target.copy_from_slice(&source[first..first + slice_row_bytes]);
                }
                pixels
            }
        };
        Ok(CanvasTile {
            pixels,
            span: end - start,
        })
    }

    pub fn materialize_axis(&self, start: u32, end: u32) -> Result<Frame, StitchError> {
        if start >= end || end > self.extent {
            return Err(StitchError::InvalidFrame {
                message: format!(
                    "axis range {start}..{end} is outside canvas extent {}",
                    self.extent
                ),
            });
        }
        let channels = self.channels();
        let selected_extent = end - start;
        let (width, height) = match self.axis {
            StitchAxis::Vertical => (self.cross_extent, selected_extent),
            StitchAxis::Horizontal => (selected_extent, self.cross_extent),
        };
        let mut pixels = RasterBuffer::zeroed(width as usize * height as usize * channels);
        self.copy_axis(start, end, pixels.as_mut_slice())?;
        Frame::from_buffer(width, height, self.pixel_format, pixels)
    }

    pub(crate) fn copy_axis(
        &self,
        start: u32,
        end: u32,
        output: &mut [u8],
    ) -> Result<(), StitchError> {
        if start >= end || end > self.extent {
            return Err(StitchError::InvalidFrame {
                message: format!(
                    "axis range {start}..{end} is outside canvas extent {}",
                    self.extent
                ),
            });
        }
        let channels = self.channels();
        let selected_extent = end - start;
        let length = (selected_extent as usize)
            .checked_mul(self.cross_extent as usize)
            .and_then(|pixels| pixels.checked_mul(channels))
            .ok_or(StitchError::Arithmetic {
                operation: "calculating canvas copy length",
            })?;
        if output.len() != length {
            return Err(StitchError::InvalidFrame {
                message: format!("canvas copy requires {length} bytes, got {}", output.len()),
            });
        }
        let mut global = 0_u32;
        for tile in &self.tiles {
            let source_pixels = tile.pixels.as_slice();
            let tile_end = global + tile.span;
            let copy_start = start.max(global);
            let copy_end = end.min(tile_end);
            if copy_start < copy_end {
                let local_start = copy_start - global;
                let span = copy_end - copy_start;
                let destination_start = copy_start - start;
                match self.axis {
                    StitchAxis::Vertical => {
                        let row_bytes = self.cross_extent as usize * channels;
                        let source = local_start as usize * row_bytes;
                        let destination = destination_start as usize * row_bytes;
                        let length = span as usize * row_bytes;
                        output[destination..destination + length]
                            .copy_from_slice(&source_pixels[source..source + length]);
                    }
                    StitchAxis::Horizontal => {
                        let tile_row_bytes = tile.span as usize * channels;
                        let output_row_bytes = selected_extent as usize * channels;
                        let copy_bytes = span as usize * channels;
                        for y in 0..self.cross_extent as usize {
                            let source = y * tile_row_bytes + local_start as usize * channels;
                            let destination =
                                y * output_row_bytes + destination_start as usize * channels;
                            output[destination..destination + copy_bytes]
                                .copy_from_slice(&source_pixels[source..source + copy_bytes]);
                        }
                    }
                }
            }
            global = tile_end;
            if global >= end {
                break;
            }
        }
        Ok(())
    }

    pub fn materialize(&self) -> Result<Frame, StitchError> {
        self.materialize_axis(0, self.extent)
    }

    pub fn snapshot_axis(&self, start: u32, end: u32) -> Result<TiledCanvasSnapshot, StitchError> {
        if start >= end || end > self.extent {
            return Err(StitchError::InvalidFrame {
                message: format!(
                    "axis range {start}..{end} is outside canvas extent {}",
                    self.extent
                ),
            });
        }
        Ok(TiledCanvasSnapshot::retained_range(
            self.axis,
            self.cross_extent,
            self.pixel_format,
            &self.tiles,
            start,
            end,
        ))
    }

    pub fn append_rows(&mut self, frame: &Frame, top: u32, bottom: u32) -> Result<(), StitchError> {
        self.require_vertical()?;
        self.append_axis(frame, top, bottom)
    }

    pub fn prepend_rows(
        &mut self,
        frame: &Frame,
        top: u32,
        bottom: u32,
    ) -> Result<(), StitchError> {
        self.require_vertical()?;
        self.prepend_axis(frame, top, bottom)
    }

    pub fn truncate_bottom(&mut self, new_height: u32) -> Result<(), StitchError> {
        self.require_vertical()?;
        self.truncate_end(new_height)
    }

    pub fn truncate_top(&mut self, rows: u32) -> Result<(), StitchError> {
        self.require_vertical()?;
        self.truncate_start(rows)
    }

    pub fn copy_rows(
        &self,
        top: u32,
        rows: u32,
        destination: &mut [u8],
    ) -> Result<(), StitchError> {
        self.require_vertical()?;
        self.snapshot_axis(0, self.extent)?
            .copy_rows(top, rows, destination)
    }

    pub fn materialize_rows(&self, top: u32, bottom: u32) -> Result<Frame, StitchError> {
        self.require_vertical()?;
        self.materialize_axis(top, bottom)
    }

    pub fn render_scaled_rows(
        &self,
        top: u32,
        rows: u32,
        width: u32,
        height: u32,
    ) -> Result<Frame, StitchError> {
        let bottom = top.checked_add(rows).ok_or(StitchError::Arithmetic {
            operation: "calculating scaled row range",
        })?;
        self.snapshot(top, bottom)?.render_scaled(width, height)
    }

    pub fn snapshot(&self, top: u32, bottom: u32) -> Result<TiledCanvasSnapshot, StitchError> {
        self.require_vertical()?;
        self.snapshot_axis(top, bottom)
    }

    fn require_vertical(&self) -> Result<(), StitchError> {
        if self.axis != StitchAxis::Vertical {
            return Err(StitchError::InvalidOptions {
                message: "row operation requires a vertical canvas".to_owned(),
            });
        }
        Ok(())
    }
}

impl TiledCanvasSnapshot {
    fn retained_range(
        axis: StitchAxis,
        cross_extent: u32,
        pixel_format: PixelFormat,
        source: &VecDeque<Arc<CanvasTile>>,
        start: u32,
        end: u32,
    ) -> Self {
        let mut tiles = VecDeque::new();
        let mut position = 0;
        let mut origin = 0;
        let mut extent = 0;
        for tile in source {
            let tile_end = position + tile.span;
            if position < end && tile_end > start {
                if tiles.is_empty() {
                    origin = position;
                }
                extent += tile.span;
                tiles.push_back(Arc::clone(tile));
            }
            position = tile_end;
            if position >= end {
                break;
            }
        }
        Self {
            axis,
            cross_extent,
            pixel_format,
            extent,
            tiles,
            start: start - origin,
            end: end - origin,
        }
    }

    pub fn from_frame(frame: Frame) -> Self {
        Self::from_frame_for_axis(frame, StitchAxis::Vertical)
    }

    pub fn from_frame_for_axis(frame: Frame, axis: StitchAxis) -> Self {
        let canvas = TiledCanvas::new_for_axis(frame, axis).expect("valid frame creates canvas");
        canvas
            .snapshot_axis(0, canvas.extent)
            .expect("non-empty frame creates snapshot")
    }

    pub const fn axis(&self) -> StitchAxis {
        self.axis
    }

    pub const fn width(&self) -> u32 {
        match self.axis {
            StitchAxis::Vertical => self.cross_extent,
            StitchAxis::Horizontal => self.end - self.start,
        }
    }

    pub const fn height(&self) -> u32 {
        match self.axis {
            StitchAxis::Vertical => self.end - self.start,
            StitchAxis::Horizontal => self.cross_extent,
        }
    }

    pub const fn axis_extent(&self) -> u32 {
        self.end - self.start
    }

    pub fn rgba_len(&self) -> Result<usize, StitchError> {
        (self.width() as usize)
            .checked_mul(self.height() as usize)
            .and_then(|n| n.checked_mul(self.pixel_format.channels() as usize))
            .ok_or(StitchError::Arithmetic {
                operation: "calculating snapshot byte length",
            })
    }

    pub fn slice_axis(&self, start: u32, end: u32) -> Result<Self, StitchError> {
        if start >= end || end > self.axis_extent() {
            return Err(StitchError::InvalidFrame {
                message: format!(
                    "axis range {start}..{end} is outside snapshot extent {}",
                    self.axis_extent()
                ),
            });
        }
        Ok(Self::retained_range(
            self.axis,
            self.cross_extent,
            self.pixel_format,
            &self.tiles,
            self.start + start,
            self.start + end,
        ))
    }

    pub fn slice_rows(&self, top: u32, bottom: u32) -> Result<Self, StitchError> {
        if self.axis != StitchAxis::Vertical {
            return Err(StitchError::InvalidOptions {
                message: "row slice requires a vertical snapshot".to_owned(),
            });
        }
        self.slice_axis(top, bottom)
    }

    pub fn materialize(&self) -> Result<Frame, StitchError> {
        self.canvas().materialize_axis(self.start, self.end)
    }

    pub fn copy_rows(
        &self,
        top: u32,
        rows: u32,
        destination: &mut [u8],
    ) -> Result<(), StitchError> {
        let row_bytes = self.width() as usize * self.pixel_format.channels() as usize;
        self.copy_rows_strided(top, rows, row_bytes, destination)
    }

    pub fn copy_rows_strided(
        &self,
        top: u32,
        rows: u32,
        destination_stride: usize,
        destination: &mut [u8],
    ) -> Result<(), StitchError> {
        let bottom = top.checked_add(rows).ok_or(StitchError::Arithmetic {
            operation: "calculating snapshot row range",
        })?;
        if rows == 0 || bottom > self.height() {
            return Err(StitchError::InvalidFrame {
                message: format!(
                    "snapshot row range {top}..{bottom} is outside image height {}",
                    self.height()
                ),
            });
        }

        let channels = self.pixel_format.channels() as usize;
        let row_bytes =
            (self.width() as usize)
                .checked_mul(channels)
                .ok_or(StitchError::Arithmetic {
                    operation: "calculating snapshot row bytes",
                })?;
        if destination_stride < row_bytes {
            return Err(StitchError::InvalidFrame {
                message: format!(
                    "snapshot destination stride needs at least {row_bytes} bytes, got {destination_stride}"
                ),
            });
        }
        let required = destination_stride
            .checked_mul(rows.saturating_sub(1) as usize)
            .and_then(|prefix| prefix.checked_add(row_bytes))
            .ok_or(StitchError::Arithmetic {
                operation: "calculating snapshot row destination size",
            })?;
        if destination.len() < required {
            return Err(StitchError::InvalidFrame {
                message: format!(
                    "snapshot row destination needs {required} bytes, got {}",
                    destination.len()
                ),
            });
        }

        match self.axis {
            StitchAxis::Vertical => {
                let global_start = self.start + top;
                let global_end = global_start + rows;
                let mut tile_start = 0_u32;
                for tile in &self.tiles {
                    let tile_end = tile_start + tile.span;
                    let copy_start = global_start.max(tile_start);
                    let copy_end = global_end.min(tile_end);
                    if copy_start < copy_end {
                        let source_row = (copy_start - tile_start) as usize;
                        let destination_row = (copy_start - global_start) as usize;
                        let row_count = (copy_end - copy_start) as usize;
                        if destination_stride == row_bytes {
                            let source = source_row * row_bytes;
                            let output = destination_row * destination_stride;
                            let length = row_count * row_bytes;
                            destination[output..output + length]
                                .copy_from_slice(&tile.pixels[source..source + length]);
                        } else {
                            for row in 0..row_count {
                                let source = (source_row + row) * row_bytes;
                                let output = (destination_row + row) * destination_stride;
                                destination[output..output + row_bytes]
                                    .copy_from_slice(&tile.pixels[source..source + row_bytes]);
                            }
                        }
                    }
                    tile_start = tile_end;
                    if tile_start >= global_end {
                        break;
                    }
                }
            }
            StitchAxis::Horizontal => {
                let mut tile_start = 0_u32;
                for tile in &self.tiles {
                    let tile_end = tile_start + tile.span;
                    let copy_start = self.start.max(tile_start);
                    let copy_end = self.end.min(tile_end);
                    if copy_start < copy_end {
                        let local_column = (copy_start - tile_start) as usize;
                        let output_column = (copy_start - self.start) as usize;
                        let copy_bytes = (copy_end - copy_start) as usize * channels;
                        let tile_row_bytes = tile.span as usize * channels;
                        for row in 0..rows as usize {
                            let source =
                                (top as usize + row) * tile_row_bytes + local_column * channels;
                            let output = row * destination_stride + output_column * channels;
                            destination[output..output + copy_bytes]
                                .copy_from_slice(&tile.pixels[source..source + copy_bytes]);
                        }
                    }
                    tile_start = tile_end;
                    if tile_start >= self.end {
                        break;
                    }
                }
            }
        }
        Ok(())
    }

    pub fn render_scaled(&self, width: u32, height: u32) -> Result<Frame, StitchError> {
        let _perf = crate::perf::Scope::new(crate::perf::Stage::PreviewScaling);
        if width == 0 || height == 0 {
            return Err(StitchError::InvalidFrame {
                message: "scaled dimensions must be non-zero".to_owned(),
            });
        }
        let channels = self.pixel_format.channels() as usize;
        let output_extent = self.axis.primary_extent(width, height) as usize;
        let mut tiles = self.tiles.iter();
        let mut tile = tiles.next().expect("non-empty snapshot has tiles");
        let mut tile_start = 0;
        // Nearest-neighbor coordinates are monotonic, so locate all sampled
        // rows/columns in one tile walk without assembling the source image.
        let locations: Vec<_> = (0..output_extent)
            .map(|position| {
                let source =
                    self.start as usize + position * self.axis_extent() as usize / output_extent;
                while source >= tile_start + tile.span as usize {
                    tile_start += tile.span as usize;
                    tile = tiles.next().expect("sample lies inside snapshot");
                }
                (
                    tile.pixels.as_slice(),
                    tile.span as usize,
                    source - tile_start,
                )
            })
            .collect();
        let mut pixels = RasterBuffer::zeroed(width as usize * height as usize * 4);
        let output = pixels.as_mut_slice();
        for y in 0..height as usize {
            let sy = y * self.height() as usize / height as usize;
            for x in 0..width as usize {
                let (source, source_offset) = match self.axis {
                    StitchAxis::Vertical => {
                        let (source, _, row) = locations[y];
                        let sx = x * self.width() as usize / width as usize;
                        (source, (row * self.cross_extent as usize + sx) * channels)
                    }
                    StitchAxis::Horizontal => {
                        let (source, span, column) = locations[x];
                        (source, (sy * span + column) * channels)
                    }
                };
                let source_pixel = &source[source_offset..source_offset + channels];
                let target = &mut output[(y * width as usize + x) * 4..][..4];
                match self.pixel_format {
                    PixelFormat::Gray8 => target.copy_from_slice(&[
                        source_pixel[0],
                        source_pixel[0],
                        source_pixel[0],
                        255,
                    ]),
                    PixelFormat::Rgb8 => target.copy_from_slice(&[
                        source_pixel[0],
                        source_pixel[1],
                        source_pixel[2],
                        255,
                    ]),
                    PixelFormat::Rgba8 => target.copy_from_slice(source_pixel),
                }
            }
        }
        Frame::from_buffer(width, height, PixelFormat::Rgba8, pixels)
    }

    fn canvas(&self) -> TiledCanvas {
        TiledCanvas {
            axis: self.axis,
            cross_extent: self.cross_extent,
            pixel_format: self.pixel_format,
            extent: self.extent,
            tiles: self.tiles.clone(),
            spare_tiles: Vec::new(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn filled_frame(width: u32, height: u32, value: u8) -> Frame {
        let mut pixels = RasterBuffer::zeroed(width as usize * height as usize * 4);
        pixels.fill(value);
        Frame::from_buffer(width, height, PixelFormat::Rgba8, pixels).unwrap()
    }

    #[test]
    fn cropped_snapshots_release_tiles_outside_their_selected_range() {
        for axis in [StitchAxis::Vertical, StitchAxis::Horizontal] {
            let (width, height) = match axis {
                StitchAxis::Vertical => (3, CANVAS_TILE_SPAN * 3),
                StitchAxis::Horizontal => (CANVAS_TILE_SPAN * 3, 3),
            };
            let source = Frame::new(
                width,
                height,
                PixelFormat::Gray8,
                (0..width as usize * height as usize)
                    .map(|index| (index % 251) as u8)
                    .collect(),
            )
            .unwrap();
            let canvas = TiledCanvas::new_for_axis(source.clone(), axis).unwrap();
            let first = Arc::downgrade(&canvas.tiles[0]);
            let middle = Arc::downgrade(&canvas.tiles[1]);
            let last = Arc::downgrade(&canvas.tiles[2]);
            let whole = canvas.snapshot_axis(0, CANVAS_TILE_SPAN * 3).unwrap();
            let selected = whole
                .slice_axis(CANVAS_TILE_SPAN + 5, CANVAS_TILE_SPAN * 2 - 7)
                .unwrap();
            assert_eq!(selected.tiles.len(), 1);
            let direct = canvas
                .snapshot_axis(CANVAS_TILE_SPAN + 5, CANVAS_TILE_SPAN * 2 - 7)
                .unwrap();
            assert_eq!(direct.tiles.len(), 1);
            assert_eq!(
                direct.materialize().unwrap(),
                selected.materialize().unwrap()
            );
            let expected = match axis {
                StitchAxis::Vertical => source
                    .crop(0, CANVAS_TILE_SPAN + 5, width, CANVAS_TILE_SPAN - 12)
                    .unwrap(),
                StitchAxis::Horizontal => source
                    .crop(CANVAS_TILE_SPAN + 5, 0, CANVAS_TILE_SPAN - 12, height)
                    .unwrap(),
            };
            assert_eq!(selected.materialize().unwrap(), expected);
            drop(canvas);
            drop(whole);
            drop(direct);
            assert!(first.upgrade().is_none());
            assert!(last.upgrade().is_none());
            assert!(middle.upgrade().is_some());
            drop(selected);
            assert!(middle.upgrade().is_none());
        }
    }

    #[test]
    fn active_canvas_reuses_unique_tiles_with_count_and_byte_limits() {
        let mut canvas = TiledCanvas::new(filled_frame(1536, CANVAS_TILE_SPAN, 17)).unwrap();
        let incoming = filled_frame(1536, CANVAS_TILE_SPAN * 3, 91);
        canvas.append_axis(&incoming, 0, incoming.height()).unwrap();
        let original: Vec<_> = canvas
            .tiles
            .iter()
            .skip(1)
            .map(|tile| tile.pixels.as_ptr())
            .collect();
        canvas.truncate_end(CANVAS_TILE_SPAN).unwrap();
        assert_eq!(canvas.spare_tiles.len(), MAX_SPARE_TILES);
        assert!(
            canvas
                .spare_tiles
                .iter()
                .map(|tile| tile.pixels.capacity())
                .sum::<usize>()
                <= MAX_SPARE_TILE_BYTES
        );
        assert!(canvas.clone().spare_tiles.is_empty());
        let spare: Vec<_> = canvas
            .spare_tiles
            .iter()
            .map(|tile| tile.pixels.as_ptr())
            .collect();
        assert!(spare.iter().all(|pointer| original.contains(pointer)));
        canvas
            .append_axis(&incoming, 0, CANVAS_TILE_SPAN * 2)
            .unwrap();
        assert!(canvas.spare_tiles.is_empty());
        let reused: Vec<_> = canvas
            .tiles
            .iter()
            .skip(1)
            .map(|tile| tile.pixels.as_ptr())
            .collect();
        assert!(spare.iter().all(|pointer| reused.contains(pointer)));
        assert_eq!(
            canvas.materialize().unwrap().pixels()[CANVAS_TILE_SPAN as usize * 1536 * 4],
            91
        );
        canvas.truncate_end(0).unwrap();
        assert!(canvas.tiles.is_empty());
        assert!(canvas.spare_tiles.is_empty());

        // The SIMD tail belongs to the byte budget too. Two nominal 4 MiB
        // mappings would exceed eight MiB once those tails are included.
        let mut wide = TiledCanvas::new(filled_frame(4096, CANVAS_TILE_SPAN, 17)).unwrap();
        let incoming = filled_frame(4096, CANVAS_TILE_SPAN * 2, 91);
        wide.append_axis(&incoming, 0, incoming.height()).unwrap();
        wide.truncate_end(CANVAS_TILE_SPAN).unwrap();
        assert!(wide.spare_tiles.len() < MAX_SPARE_TILES);
        assert!(
            wide.spare_tiles
                .iter()
                .map(|tile| tile.pixels.capacity())
                .sum::<usize>()
                <= MAX_SPARE_TILE_BYTES
        );
    }

    #[test]
    fn substantial_partial_crops_release_oversized_tile_capacity() {
        let mut canvas = TiledCanvas::new(filled_frame(1536, CANVAS_TILE_SPAN, 17)).unwrap();
        let original = canvas.tiles[0].pixels.as_ptr();
        canvas.truncate_end(3).unwrap();
        assert_ne!(canvas.tiles[0].pixels.as_ptr(), original);
        assert!(canvas.tiles[0].pixels.capacity() <= tile_capacity_limit(1536 * 3 * 4));
        let incoming = filled_frame(1536, CANVAS_TILE_SPAN, 91);
        canvas.append_axis(&incoming, 0, CANVAS_TILE_SPAN).unwrap();
        canvas.truncate_end(3).unwrap();
        assert_eq!(canvas.spare_tiles.len(), 1);
        canvas.append_axis(&incoming, 0, 1).unwrap();
        assert_eq!(canvas.spare_tiles.len(), 1);
        assert!(canvas.tiles[1].pixels.capacity() <= tile_capacity_limit(1536 * 4));
    }

    #[test]
    fn snapshots_prevent_recycling_and_partial_tile_mutation() {
        for axis in [StitchAxis::Vertical, StitchAxis::Horizontal] {
            let (width, height) = match axis {
                StitchAxis::Vertical => (1024, CANVAS_TILE_SPAN),
                StitchAxis::Horizontal => (CANVAS_TILE_SPAN, 1024),
            };
            let source = filled_frame(width, height, 17);
            let mut canvas = TiledCanvas::new_for_axis(source.clone(), axis).unwrap();
            canvas.append_axis(&source, 0, CANVAS_TILE_SPAN).unwrap();
            let snapshot = canvas.snapshot_axis(0, canvas.extent()).unwrap();
            canvas.truncate_end(CANVAS_TILE_SPAN + 7).unwrap();
            canvas.truncate_start(5).unwrap();
            assert!(canvas.spare_tiles.is_empty());
            assert!(
                snapshot
                    .materialize()
                    .unwrap()
                    .pixels()
                    .iter()
                    .all(|byte| *byte == 17)
            );
            canvas.truncate_end(CANVAS_TILE_SPAN - 5).unwrap();
            assert!(canvas.spare_tiles.len() <= 1);
            assert!(
                snapshot
                    .materialize()
                    .unwrap()
                    .pixels()
                    .iter()
                    .all(|byte| *byte == 17)
            );
            // The original leased tile is still held by the snapshot. Only the
            // replacement created for the partial crop can enter the pool.
            for spare in &canvas.spare_tiles {
                assert!(
                    snapshot
                        .tiles
                        .iter()
                        .all(|tile| tile.pixels.as_ptr() != spare.pixels.as_ptr())
                );
            }
        }
    }

    #[test]
    fn unique_partial_tiles_compact_in_place_and_match_shared_crops() {
        for axis in [StitchAxis::Vertical, StitchAxis::Horizontal] {
            let (width, height) = match axis {
                StitchAxis::Vertical => (23, 47),
                StitchAxis::Horizontal => (47, 23),
            };
            let source = Frame::new(
                width,
                height,
                PixelFormat::Rgba8,
                (0..width as usize * height as usize * 4)
                    .map(|index| (index % 251) as u8)
                    .collect(),
            )
            .unwrap();
            let mut unique = TiledCanvas::new_for_axis(source, axis).unwrap();
            let shared = unique.clone();
            let lease = shared.snapshot_axis(0, shared.extent()).unwrap();
            drop(shared);
            let pointer = unique.tiles[0].pixels.as_ptr();
            // Drop all leases before exercising the unique path.
            let reference = lease.materialize().unwrap();
            drop(lease);
            unique.truncate_start(2).unwrap();
            unique.truncate_end(43).unwrap();
            assert_eq!(pointer, unique.tiles[0].pixels.as_ptr());
            let expected = match axis {
                StitchAxis::Vertical => reference.crop(0, 2, width, 43).unwrap(),
                StitchAxis::Horizontal => reference.crop(2, 0, 43, height).unwrap(),
            };
            assert_eq!(unique.materialize().unwrap(), expected);
        }
    }

    #[test]
    fn large_tiles_materialized_crops_and_previews_keep_page_ownership() {
        let pixels = RasterBuffer::zeroed(1024 * 512 * 4);
        let pointer = pixels.as_ptr();
        let frame = Frame::from_buffer(1024, 512, PixelFormat::Rgba8, pixels).unwrap();
        assert_eq!(frame.pixels().as_ptr(), pointer);
        let canvas = TiledCanvas::new(frame).unwrap();
        #[cfg(any(windows, target_os = "macos", target_os = "linux"))]
        assert!(canvas.tiles.iter().all(|tile| tile.pixels.is_page_backed()));
        let snapshot = canvas.snapshot_axis(0, 512).unwrap();
        drop(canvas);
        let materialized = snapshot.materialize().unwrap();
        let crop = materialized.crop(0, 0, 1024, 256).unwrap();
        let preview = snapshot.render_scaled(1024, 256).unwrap();
        for output in [materialized, crop, preview] {
            assert!(output.pixels().iter().all(|&byte| byte == 0));
            #[cfg(any(windows, target_os = "macos", target_os = "linux"))]
            assert!(output.into_buffer().is_page_backed());
        }
    }

    fn rows(values: &[u8]) -> Frame {
        Frame::new(
            1,
            values.len() as u32,
            PixelFormat::Rgba8,
            values.iter().flat_map(|v| [*v, 0, 0, 255]).collect(),
        )
        .unwrap()
    }

    fn columns(values: &[u8]) -> Frame {
        Frame::new(
            values.len() as u32,
            1,
            PixelFormat::Rgba8,
            values.iter().flat_map(|v| [*v, 0, 0, 255]).collect(),
        )
        .unwrap()
    }

    #[test]
    fn append_and_prepend_retain_exact_rows() {
        let mut canvas = TiledCanvas::new(rows(&[1, 2, 3, 4])).unwrap();
        let incoming = rows(&[10, 11, 12, 13]);
        canvas.truncate_end(3).unwrap();
        canvas.append_axis(&incoming, 1, 4).unwrap();
        assert_eq!(canvas.materialize().unwrap().height(), 6);

        let mut prepended = TiledCanvas::new(rows(&[1, 2, 3, 4])).unwrap();
        prepended.truncate_start(2).unwrap();
        prepended.prepend_axis(&incoming, 0, 3).unwrap();
        assert_eq!(prepended.materialize().unwrap().height(), 5);
    }

    #[test]
    fn horizontal_tiles_append_prepend_and_slice_exact_columns() {
        let mut canvas =
            TiledCanvas::new_for_axis(columns(&[1, 2, 3, 4]), StitchAxis::Horizontal).unwrap();
        let incoming = columns(&[10, 11, 12, 13]);
        canvas.truncate_end(3).unwrap();
        canvas.append_axis(&incoming, 1, 4).unwrap();
        assert_eq!(
            canvas.materialize().unwrap().pixels(),
            columns(&[1, 2, 3, 11, 12, 13]).pixels()
        );
        canvas.truncate_start(2).unwrap();
        canvas.prepend_axis(&incoming, 0, 3).unwrap();
        assert_eq!(
            canvas.materialize().unwrap().pixels(),
            columns(&[10, 11, 12, 3, 11, 12, 13]).pixels()
        );
        assert_eq!(
            canvas
                .snapshot_axis(1, 6)
                .unwrap()
                .materialize()
                .unwrap()
                .pixels(),
            columns(&[11, 12, 3, 11, 12]).pixels()
        );
    }

    #[test]
    fn snapshots_are_stable_after_canvas_mutation() {
        let mut canvas = TiledCanvas::new(rows(&[1, 2, 3])).unwrap();
        let snapshot = canvas.snapshot_axis(0, 3).unwrap();
        let incoming = rows(&[9, 9]);
        canvas.truncate_end(1).unwrap();
        canvas.append_axis(&incoming, 0, 2).unwrap();
        assert_eq!(snapshot.materialize().unwrap().pixels()[0], 1);
        assert_eq!(canvas.materialize().unwrap().pixels()[0], 1);
    }

    #[test]
    fn vertical_snapshot_copies_strided_rows_across_tiles() {
        let values: Vec<u8> = (0..=CANVAS_TILE_ROWS)
            .map(|row| (row % 251) as u8)
            .collect();
        let canvas = TiledCanvas::new(rows(&values)).unwrap();
        let snapshot = canvas.snapshot_axis(1, CANVAS_TILE_ROWS + 1).unwrap();
        let stride = 8_usize;
        let mut output = vec![0xee; stride * 3];
        snapshot
            .copy_rows_strided(CANVAS_TILE_ROWS - 3, 3, stride, &mut output)
            .unwrap();
        for row in 0..3_usize {
            let expected = values[CANVAS_TILE_ROWS as usize - 2 + row];
            assert_eq!(
                &output[row * stride..row * stride + 4],
                &[expected, 0, 0, 255]
            );
            assert_eq!(&output[row * stride + 4..row * stride + stride], &[0xee; 4]);
        }
    }

    #[test]
    fn vertical_snapshot_copies_packed_rows_across_tiles() {
        let values: Vec<u8> = (0..=CANVAS_TILE_ROWS)
            .map(|row| (row % 251) as u8)
            .collect();
        let canvas = TiledCanvas::new(rows(&values)).unwrap();
        let snapshot = canvas.snapshot_axis(1, CANVAS_TILE_ROWS + 1).unwrap();
        let mut output = vec![0; snapshot.width() as usize * 4 * 3];
        snapshot
            .copy_rows(CANVAS_TILE_ROWS - 3, 3, &mut output)
            .unwrap();
        for row in 0..3_usize {
            let expected = values[CANVAS_TILE_ROWS as usize - 2 + row];
            assert_eq!(&output[row * 4..row * 4 + 4], &[expected, 0, 0, 255]);
        }
    }

    #[test]
    fn horizontal_snapshot_copies_selected_rows_across_tiles() {
        let width = CANVAS_TILE_SPAN + 2;
        let mut pixels = Vec::with_capacity(width as usize * 2 * 4);
        for row in 0..2_u8 {
            for column in 0..width {
                pixels.extend_from_slice(&[(column % 251) as u8, row, 0, 255]);
            }
        }
        let frame = Frame::new(width, 2, PixelFormat::Rgba8, pixels).unwrap();
        let canvas = TiledCanvas::new_for_axis(frame, StitchAxis::Horizontal).unwrap();
        let snapshot = canvas.snapshot_axis(1, width - 1).unwrap();
        let mut output = vec![0; snapshot.width() as usize * 4];
        snapshot.copy_rows(1, 1, &mut output).unwrap();
        assert_eq!(&output[..4], &[1, 1, 0, 255]);
        assert_eq!(
            &output[output.len() - 4..],
            &[((width - 2) % 251) as u8, 1, 0, 255]
        );
    }

    #[test]
    fn scaled_axis_range_uses_nearest_neighbor_and_rgba_output() {
        let canvas = TiledCanvas::new(rows(&[1, 2])).unwrap();
        let scaled = canvas
            .snapshot_axis(0, 2)
            .unwrap()
            .render_scaled(2, 1)
            .unwrap();
        assert_eq!(scaled.pixels(), &[1, 0, 0, 255, 1, 0, 0, 255]);
    }

    #[test]
    fn tiled_scaling_matches_materialized_pixels_for_formats_axes_and_trims() {
        for format in [PixelFormat::Gray8, PixelFormat::Rgb8, PixelFormat::Rgba8] {
            for axis in [StitchAxis::Vertical, StitchAxis::Horizontal] {
                let (width, height) = match axis {
                    StitchAxis::Vertical => (17, 519),
                    StitchAxis::Horizontal => (519, 17),
                };
                let channels = format.channels() as usize;
                let pixels = (0..width as usize * height as usize * channels)
                    .map(|i| (i.wrapping_mul(37) ^ (i >> 7)) as u8)
                    .collect();
                let mut canvas = TiledCanvas::new_for_axis(
                    Frame::new(width, height, format, pixels).unwrap(),
                    axis,
                )
                .unwrap();
                let snapshot = canvas
                    .snapshot_axis(1, 518)
                    .unwrap()
                    .slice_axis(2, 513)
                    .unwrap();
                let reference = snapshot.materialize().unwrap();
                canvas.truncate_start(260).unwrap();
                for (out_width, out_height) in [(1, 1), (13, 29), (31, 521), (523, 33)] {
                    let scaled = snapshot.render_scaled(out_width, out_height).unwrap();
                    let mut expected = Vec::new();
                    for y in 0..out_height {
                        let row = reference.row(y * reference.height() / out_height).unwrap();
                        for x in 0..out_width {
                            let offset = (x * reference.width() / out_width) as usize * channels;
                            let pixel = &row[offset..offset + channels];
                            match format {
                                PixelFormat::Gray8 => {
                                    expected.extend_from_slice(&[pixel[0], pixel[0], pixel[0], 255])
                                }
                                PixelFormat::Rgb8 => {
                                    expected.extend_from_slice(&[pixel[0], pixel[1], pixel[2], 255])
                                }
                                PixelFormat::Rgba8 => expected.extend_from_slice(pixel),
                            }
                        }
                    }
                    assert_eq!(scaled.pixel_format(), PixelFormat::Rgba8);
                    assert_eq!(
                        scaled.pixels(),
                        expected,
                        "{format:?} {axis:?} {out_width}x{out_height}"
                    );
                }
                assert!(snapshot.render_scaled(0, 1).is_err());
                assert!(snapshot.render_scaled(1, 0).is_err());
            }
        }
    }
}
