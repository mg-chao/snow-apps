//! Bounded overlay coverage and experimental source-damage accumulation.
#[cfg(any(test, feature = "bench-synthetic-input"))]
use snow_capture::DirtyRect;
use snow_recording_effects::surface::Surface;

/// One interval per row bounds bookkeeping independently of input/event count.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) struct Rows {
    size: (u32, u32),
    spans: Vec<(u32, u32)>,
}

impl Rows {
    pub(super) fn size(&self) -> (u32, u32) {
        self.size
    }
    pub(super) fn new(size: (u32, u32)) -> Self {
        Self {
            size,
            spans: vec![(size.0, 0); size.1 as usize],
        }
    }

    pub(super) fn mark(&mut self, x: u32, y: u32, width: u32) {
        if let Some(span) = self.spans.get_mut(y as usize) {
            let right = x.saturating_add(width).min(self.size.0);
            if x < right {
                span.0 = span.0.min(x);
                span.1 = span.1.max(right);
            }
        }
    }

    pub(super) fn restore(&self, background: &[u8], output: &mut [u8]) {
        assert_eq!(
            background.len(),
            self.size.0 as usize * self.size.1 as usize * 4
        );
        assert_eq!(output.len(), background.len());
        for (y, &(left, right)) in self.spans.iter().enumerate() {
            if left < right {
                let start = (y * self.size.0 as usize + left as usize) * 4;
                let end = (y * self.size.0 as usize + right as usize) * 4;
                output[start..end].copy_from_slice(&background[start..end]);
            }
        }
    }
}

pub(super) struct TrackedSurface<'a> {
    pub pixels: &'a mut [u8],
    pub rows: &'a mut Rows,
}

impl Surface for TrackedSurface<'_> {
    const OPAQUE: bool = true;
    fn size(&self) -> (u32, u32) {
        self.rows.size
    }
    fn span(&mut self, x: u32, y: u32, width: u32, mut visit: impl FnMut(&mut [u8], usize)) {
        self.rows.mark(x, y, width);
        let start = (y as usize * self.rows.size.0 as usize + x as usize) * 4;
        visit(&mut self.pixels[start..start + width as usize * 4], 0);
    }
}

#[derive(Clone, Debug)]
pub(super) struct History {
    pub generation: u64,
    pub overlays: Rows,
}

/// Damage is accumulated for every observed capture, including superseded ones.
/// Empty nonduplicate damage and any sequence gap invalidate partial refresh.
#[cfg(any(test, feature = "bench-synthetic-input"))]
#[derive(Default)]
pub(super) struct Damage {
    last_sequence: Option<u64>,
    size: Option<(u32, u32)>,
    pub complete: bool,
    pub rects: Vec<DirtyRect>,
}

#[cfg(any(test, feature = "bench-synthetic-input"))]
impl Damage {
    /// Use serial rectangle updates only when their source work is less than
    /// one worker's share of a complete resize. Count overlap repeatedly: that
    /// reflects actual work and conservatively rejects fragmented/dense damage.
    pub(super) fn is_sparse(&self, resize_workers: usize) -> bool {
        let Some((width, height)) = self.size else {
            return false;
        };
        if !self.complete {
            return false;
        }
        let pixels = self.rects.iter().fold(0u64, |sum, rect| {
            sum.saturating_add(u64::from(rect.width) * u64::from(rect.height))
        });
        pixels.saturating_mul(resize_workers.max(1) as u64) < u64::from(width) * u64::from(height)
    }

    pub(super) fn covers_sequence(&self, sequence: u64) -> bool {
        self.last_sequence == Some(sequence)
    }
    pub(super) fn observe(
        &mut self,
        sequence: u64,
        size: (u32, u32),
        duplicate: bool,
        rects: &[DirtyRect],
    ) {
        if self.last_sequence.and_then(|last| last.checked_add(1)) != Some(sequence)
            || self.size != Some(size)
            || (!duplicate && rects.is_empty())
            || rects.iter().any(|r| {
                r.width == 0
                    || r.height == 0
                    || r.x.checked_add(r.width).is_none_or(|right| right > size.0)
                    || r.y
                        .checked_add(r.height)
                        .is_none_or(|bottom| bottom > size.1)
            })
        {
            self.complete = false;
            self.rects.clear();
        }
        self.last_sequence = Some(sequence);
        self.size = Some(size);
        if self.complete && !duplicate {
            self.rects.extend_from_slice(rects);
            // Avoid unbounded damage history when a consumer skips many frames.
            if self.rects.len() > 128 {
                self.complete = false;
                self.rects.clear();
            }
        }
    }

    pub(super) fn consumed(&mut self) {
        self.complete = true;
        self.rects.clear();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn partial_resize_work_limit_rejects_dense_overlapping_and_unknown_damage() {
        let mut damage = Damage::default();
        assert!(!damage.is_sparse(4));
        damage.observe(1, (100, 100), false, &[]);
        damage.consumed();
        let small = DirtyRect {
            x: 0,
            y: 0,
            width: 10,
            height: 10,
        };
        damage.observe(2, (100, 100), false, &[small]);
        assert!(damage.is_sparse(4));
        let quarter = DirtyRect {
            width: 50,
            height: 50,
            ..small
        };
        damage.consumed();
        damage.observe(3, (100, 100), false, &[quarter]);
        assert!(damage.is_sparse(2));
        assert!(!damage.is_sparse(4));
        damage.observe(4, (100, 100), false, &[quarter, quarter, quarter]);
        assert!(
            !damage.is_sparse(1),
            "overlapping rectangles still cost repeated work"
        );
        damage.consumed();
        damage.observe(5, (100, 100), true, &[]);
        assert!(
            damage.is_sparse(4),
            "a complete duplicate requires no resize"
        );
        damage.observe(7, (100, 100), false, &[small]);
        assert!(
            !damage.is_sparse(4),
            "a sequence gap invalidates even tiny damage"
        );
    }

    #[test]
    fn damage_accumulates_skips_but_rejects_missing_captures_geometry_and_unknown_damage() {
        let a = DirtyRect {
            x: 1,
            y: 2,
            width: 3,
            height: 4,
        };
        let b = DirtyRect {
            x: 5,
            y: 1,
            width: 2,
            height: 2,
        };
        let mut damage = Damage::default();
        damage.observe(1, (10, 10), false, &[a]);
        assert!(!damage.complete);
        damage.consumed();
        damage.observe(2, (10, 10), false, &[a]);
        damage.observe(3, (10, 10), true, &[]);
        damage.observe(4, (10, 10), false, &[b]);
        assert!(damage.complete);
        assert!(!damage.covers_sequence(3));
        assert!(damage.covers_sequence(4));
        assert_eq!(damage.rects, [a, b]);
        damage.observe(6, (10, 10), false, &[b]);
        assert!(!damage.complete);
        damage.consumed();
        damage.observe(7, (20, 10), true, &[]);
        assert!(!damage.complete);
        damage.consumed();
        damage.observe(8, (20, 10), false, &[]);
        assert!(!damage.complete);
        damage.consumed();
        damage.observe(9, (20, 10), false, &[DirtyRect { x: u32::MAX, ..a }]);
        assert!(!damage.complete);
    }

    #[test]
    fn tracked_effects_restore_exact_pixels_including_expiry() {
        let size = (17, 9);
        let background: Vec<_> = (0..size.0 * size.1 * 4).map(|i| (i % 251) as u8).collect();
        let mut pixels = background.clone();
        let mut rows = Rows::new(size);
        let mut surface = TrackedSurface {
            pixels: &mut pixels,
            rows: &mut rows,
        };
        surface.blend_pixel(1, 2, [20, 30, 40, 90]);
        surface.blend_pixel(16, 8, [200, 4, 3, 255]);
        surface.blend_pixel(-1, 0, [255; 4]);
        surface.blend_pixel(4, 1, [255, 0, 0, 0]);
        assert_ne!(pixels, background);
        rows.restore(&background, &mut pixels);
        assert_eq!(pixels, background);
        assert_eq!(rows.spans[0], (17, 0));
        assert_eq!(rows.spans[2], (1, 2));
        assert_eq!(rows.spans[8], (16, 17));
    }
}
