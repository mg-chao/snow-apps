use std::collections::VecDeque;
use std::sync::Arc;

use crate::{Frame, PixelFormat, StitchAxis, StitchError};

pub const CANVAS_TILE_SPAN: u32 = 256;
pub const CANVAS_TILE_ROWS: u32 = CANVAS_TILE_SPAN;

#[derive(Debug, Clone)]
struct CanvasTile {
    pixels: Vec<u8>,
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

#[derive(Debug, Clone)]
pub struct TiledCanvas {
    axis: StitchAxis,
    cross_extent: u32,
    pixel_format: PixelFormat,
    extent: u32,
    tiles: VecDeque<Arc<CanvasTile>>,
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

    fn extract_axis_range(
        &self,
        frame: &Frame,
        start: u32,
        end: u32,
    ) -> Result<Vec<u8>, StitchError> {
        self.validate_axis_range(frame, start, end)?;
        let channels = self.channels();
        match self.axis {
            StitchAxis::Vertical => {
                let row_bytes = frame.width() as usize * channels;
                Ok(frame.pixels()[start as usize * row_bytes..end as usize * row_bytes].to_vec())
            }
            StitchAxis::Horizontal => {
                let span_bytes = (end - start) as usize * channels;
                let source_row_bytes = frame.width() as usize * channels;
                let mut pixels = Vec::with_capacity(span_bytes * frame.height() as usize);
                for y in 0..frame.height() as usize {
                    let row = y * source_row_bytes;
                    let first = row + start as usize * channels;
                    pixels.extend_from_slice(&frame.pixels()[first..first + span_bytes]);
                }
                Ok(pixels)
            }
        }
    }

    fn make_tiles(
        &self,
        frame: &Frame,
        start: u32,
        end: u32,
    ) -> Result<VecDeque<Arc<CanvasTile>>, StitchError> {
        self.validate_axis_range(frame, start, end)?;
        let mut tiles = VecDeque::new();
        let mut cursor = start;
        while cursor < end {
            let span = CANVAS_TILE_SPAN.min(end - cursor);
            tiles.push_back(Arc::new(CanvasTile {
                pixels: self.extract_axis_range(frame, cursor, cursor + span)?,
                span,
            }));
            cursor += span;
        }
        Ok(tiles)
    }

    pub fn append_axis(&mut self, frame: &Frame, start: u32, end: u32) -> Result<(), StitchError> {
        let mut tiles = self.make_tiles(frame, start, end)?;
        self.tiles.append(&mut tiles);
        self.extent = self
            .extent
            .checked_add(end - start)
            .ok_or(StitchError::Arithmetic {
                operation: "calculating tiled canvas extent",
            })?;
        Ok(())
    }

    pub fn prepend_axis(&mut self, frame: &Frame, start: u32, end: u32) -> Result<(), StitchError> {
        let mut tiles = self.make_tiles(frame, start, end)?;
        while let Some(tile) = tiles.pop_back() {
            self.tiles.push_front(tile);
        }
        self.extent = self
            .extent
            .checked_add(end - start)
            .ok_or(StitchError::Arithmetic {
                operation: "calculating tiled canvas extent",
            })?;
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
                self.tiles.pop_back();
                continue;
            }
            let keep = last.span - remove;
            let replacement = self.tile_slice(last, 0, keep)?;
            *self
                .tiles
                .back_mut()
                .expect("partially retained tile remains present") = Arc::new(replacement);
            self.extent = new_extent;
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
                self.tiles.pop_front();
                continue;
            }
            let replacement = self.tile_slice(front, remove, front.span)?;
            *self
                .tiles
                .front_mut()
                .expect("partially retained tile remains present") = Arc::new(replacement);
            remove = 0;
        }
        self.extent -= span;
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
                tile.pixels[start as usize * row_bytes..end as usize * row_bytes].to_vec()
            }
            StitchAxis::Horizontal => {
                let source_row_bytes = tile.span as usize * channels;
                let slice_row_bytes = (end - start) as usize * channels;
                let mut pixels = Vec::with_capacity(slice_row_bytes * self.cross_extent as usize);
                for y in 0..self.cross_extent as usize {
                    let first = y * source_row_bytes + start as usize * channels;
                    pixels.extend_from_slice(&tile.pixels[first..first + slice_row_bytes]);
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
        let mut pixels = vec![0; width as usize * height as usize * channels];
        let mut global = 0_u32;
        for tile in &self.tiles {
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
                        pixels[destination..destination + length]
                            .copy_from_slice(&tile.pixels[source..source + length]);
                    }
                    StitchAxis::Horizontal => {
                        let tile_row_bytes = tile.span as usize * channels;
                        let output_row_bytes = selected_extent as usize * channels;
                        let copy_bytes = span as usize * channels;
                        for y in 0..self.cross_extent as usize {
                            let source = y * tile_row_bytes + local_start as usize * channels;
                            let destination =
                                y * output_row_bytes + destination_start as usize * channels;
                            pixels[destination..destination + copy_bytes]
                                .copy_from_slice(&tile.pixels[source..source + copy_bytes]);
                        }
                    }
                }
            }
            global = tile_end;
            if global >= end {
                break;
            }
        }
        Frame::new(width, height, self.pixel_format, pixels)
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
        Ok(TiledCanvasSnapshot {
            axis: self.axis,
            cross_extent: self.cross_extent,
            pixel_format: self.pixel_format,
            extent: self.extent,
            tiles: self.tiles.clone(),
            start,
            end,
        })
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
        Ok(Self {
            axis: self.axis,
            cross_extent: self.cross_extent,
            pixel_format: self.pixel_format,
            extent: self.extent,
            tiles: self.tiles.clone(),
            start: self.start + start,
            end: self.start + end,
        })
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
                        for row in 0..(copy_end - copy_start) as usize {
                            let source = (source_row + row) * row_bytes;
                            let output = (destination_row + row) * destination_stride;
                            destination[output..output + row_bytes]
                                .copy_from_slice(&tile.pixels[source..source + row_bytes]);
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
        if width == 0 || height == 0 {
            return Err(StitchError::InvalidFrame {
                message: "scaled dimensions must be non-zero".to_owned(),
            });
        }
        let source = self.materialize()?;
        let channels = self.pixel_format.channels() as usize;
        let mut pixels = vec![0; width as usize * height as usize * 4];
        for y in 0..height as usize {
            let sy = y * source.height() as usize / height as usize;
            let src = source.row(sy as u32)?;
            for x in 0..width as usize {
                let sx = x * source.width() as usize / width as usize;
                let source_pixel = &src[sx * channels..sx * channels + channels];
                let target = &mut pixels[(y * width as usize + x) * 4..][..4];
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
        Frame::new(width, height, PixelFormat::Rgba8, pixels)
    }

    fn canvas(&self) -> TiledCanvas {
        TiledCanvas {
            axis: self.axis,
            cross_extent: self.cross_extent,
            pixel_format: self.pixel_format,
            extent: self.extent,
            tiles: self.tiles.clone(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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
}
