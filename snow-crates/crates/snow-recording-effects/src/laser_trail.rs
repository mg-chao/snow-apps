//! Recording-space adaptation of Excalidraw's laser effect: streamline 0.4,
//! radius 2, faster 500 ms time decay and a quartic taper over the last 50 movements.
//! Unlike the reference, timestamps stay at the actual observation time. We smooth
//! the centerline with midpoint quadratics and round joins rather than its SVG
//! outline. This keeps sharp turns bounded and allows a reusable coverage union:
//! translucent overlaps are composited once, never darkened by repeated stamps.
use crate::surface::{RgbaSurface, Surface, TILE_SIZE};
use std::collections::{HashMap, HashSet, VecDeque};

const DEFAULT_LIFETIME_MS: u64 = 500;
const MAX_POINTS: usize = 50;
const RADIUS: f32 = 2.0;

#[derive(Clone, Copy)]
struct Sample {
    x: f32,
    y: f32,
    timestamp_ms: u64,
    connected: bool,
}

#[derive(Clone, Copy)]
struct Vertex {
    x: f32,
    y: f32,
    radius: f32,
}

impl Vertex {
    fn midpoint(self, other: Self) -> Self {
        Self {
            x: (self.x + other.x) * 0.5,
            y: (self.y + other.y) * 0.5,
            radius: (self.radius + other.radius) * 0.5,
        }
    }
}

pub struct LaserTrail {
    lifetime_ms: u64,
    points: VecDeque<Sample>,
    last_position: Option<(i32, i32)>,
    coverage: Coverage,
    was_visible: bool,
}

impl Default for LaserTrail {
    fn default() -> Self {
        Self::new(DEFAULT_LIFETIME_MS)
    }
}

impl LaserTrail {
    pub fn new(lifetime_ms: u64) -> Self {
        Self {
            lifetime_ms: lifetime_ms.max(1),
            points: VecDeque::new(),
            last_position: None,
            coverage: Coverage::default(),
            was_visible: false,
        }
    }

    pub fn set_lifetime_ms(&mut self, lifetime_ms: u64) {
        self.lifetime_ms = lifetime_ms.max(1);
    }

    pub fn clear(&mut self) {
        self.points.clear();
        self.last_position = None;
        self.was_visible = false;
    }

    pub fn observe(
        &mut self,
        position: Option<(i32, i32)>,
        source_size: (u32, u32),
        output_size: (u32, u32),
        timestamp_ms: u64,
    ) {
        // Keep one expired predecessor for a continuous taper at the time boundary.
        while self.points.len() > 1
            && timestamp_ms.saturating_sub(self.points[1].timestamp_ms) >= self.lifetime_ms
        {
            self.points.pop_front();
        }
        if self.points.back().is_some_and(|point| {
            timestamp_ms.saturating_sub(point.timestamp_ms) >= self.lifetime_ms
        }) {
            self.points.clear();
        }
        let previous_position = self.last_position;
        self.last_position = position;
        let Some((x, y)) = position.filter(|_| position != previous_position) else {
            return;
        };
        let mut point = Sample {
            x: x as f32 * output_size.0 as f32 / source_size.0.max(1) as f32,
            y: y as f32 * output_size.1 as f32 / source_size.1.max(1) as f32,
            timestamp_ms,
            connected: previous_position.is_some() && !self.points.is_empty(),
        };
        if point.connected {
            let previous = self.points.back().unwrap();
            point.x = previous.x + (point.x - previous.x) * 0.6;
            point.y = previous.y + (point.y - previous.y) * 0.6;
        }
        if self.points.len() == MAX_POINTS {
            self.points.pop_front();
        }
        self.points.push_back(point);
    }

    pub fn has_active_animation(&self, timestamp_ms: u64) -> bool {
        // The last visible frame needs a clean successor even if a frame interval
        // skips over the exact expiry time. A lone initial observation is invisible.
        self.was_visible
            || self.points.iter().skip(1).any(|point| {
                point.connected
                    && timestamp_ms.saturating_sub(point.timestamp_ms) < self.lifetime_ms
            })
    }

    fn vertex(&self, index: usize, timestamp_ms: u64) -> Vertex {
        let point = self.points[index];
        let age = timestamp_ms.saturating_sub(point.timestamp_ms);
        let elapsed = age.min(self.lifetime_ms) as f32 / self.lifetime_ms as f32;
        let length = 1.0 - (self.points.len() - index) as f32 / MAX_POINTS as f32;
        // Lose width earlier than Excalidraw's quartic time curve while preserving
        // its smooth spatial taper and full-width leading end during movement.
        let time_width = 1.0 - elapsed * elapsed;
        let length_width = 1.0 - (1.0 - length).powi(4);
        Vertex {
            x: point.x,
            y: point.y,
            radius: RADIUS * time_width.min(length_width).clamp(0.0, 1.0),
        }
    }

    pub fn draw(&mut self, rgba: &mut [u8], size: (u32, u32), timestamp_ms: u64, color: [u8; 4]) {
        self.render::<true>(
            &mut RgbaSurface {
                pixels: rgba,
                dimensions: size,
            },
            timestamp_ms,
            color,
        );
    }
    pub fn draw_to(&mut self, surface: &mut impl Surface, timestamp_ms: u64, color: [u8; 4]) {
        self.render::<false>(surface, timestamp_ms, color);
    }
    fn render<const DENSE: bool>(
        &mut self,
        surface: &mut impl Surface,
        timestamp_ms: u64,
        color: [u8; 4],
    ) {
        let size = surface.size();
        self.was_visible = false;
        if color[3] == 0 || self.points.len() < 2 || size.0 == 0 || size.1 == 0 {
            return;
        }
        self.coverage.prepare_with_storage::<DENSE>(size);
        let mut start = 0;
        while start < self.points.len() {
            let mut end = start + 1;
            while end < self.points.len() && self.points[end].connected {
                end += 1;
            }
            if end - start >= 2 {
                let mut from = self.vertex(start, timestamp_ms);
                for index in start + 1..end - 1 {
                    let control = self.vertex(index, timestamp_ms);
                    let to = control.midpoint(self.vertex(index + 1, timestamp_ms));
                    self.coverage.quadratic::<DENSE>(from, control, to, 0);
                    from = to;
                }
                self.coverage
                    .segment_with_storage::<DENSE>(from, self.vertex(end - 1, timestamp_ms));
            }
            start = end;
        }
        self.was_visible = self.coverage.blend::<DENSE>(surface, color);
    }
}

#[derive(Default)]
struct Coverage {
    size: (u32, u32),
    tiles: HashMap<usize, Box<[u8]>>,
    pixels: Vec<u8>,
    touched: Vec<usize>,
}

impl Coverage {
    #[cfg(test)]
    fn prepare(&mut self, size: (u32, u32)) {
        self.prepare_with_storage::<true>(size);
    }
    fn prepare_with_storage<const DENSE: bool>(&mut self, size: (u32, u32)) {
        self.size = size;
        if DENSE {
            self.pixels.resize(size.0 as usize * size.1 as usize, 0);
        }
    }

    fn quadratic<const DENSE: bool>(
        &mut self,
        from: Vertex,
        control: Vertex,
        to: Vertex,
        depth: u8,
    ) {
        let middle = from.midpoint(to);
        let dx = control.x - middle.x;
        let dy = control.y - middle.y;
        // Bound both tessellation and raster work, including off-screen extremes.
        let margin = RADIUS + 1.0;
        if from.x.max(control.x).max(to.x) < -margin
            || from.y.max(control.y).max(to.y) < -margin
            || from.x.min(control.x).min(to.x) > self.size.0 as f32 + margin
            || from.y.min(control.y).min(to.y) > self.size.1 as f32 + margin
        {
            return;
        }
        if depth == 10
            || (dx * dx + dy * dy <= 0.09 && (control.radius - middle.radius).abs() <= 0.1)
        {
            self.segment_with_storage::<DENSE>(from, to);
            return;
        }
        let left = from.midpoint(control);
        let right = control.midpoint(to);
        let center = left.midpoint(right);
        self.quadratic::<DENSE>(from, left, center, depth + 1);
        self.quadratic::<DENSE>(center, right, to, depth + 1);
    }

    #[cfg(test)]
    fn segment(&mut self, from: Vertex, to: Vertex) {
        self.segment_with_storage::<true>(from, to);
    }
    fn segment_with_storage<const DENSE: bool>(&mut self, from: Vertex, to: Vertex) {
        if from.radius.max(to.radius) <= 0.0 {
            return;
        }
        let dx = to.x - from.x;
        let dy = to.y - from.y;
        let length_squared = dx * dx + dy * dy;
        let inverse_length_squared = if length_squared > 0.000001 {
            length_squared.recip()
        } else {
            0.0
        };
        let margin = from.radius.max(to.radius) + 0.5;
        let min_y = ((from.y.min(to.y) - margin).floor() as i64).max(0);
        let max_y = ((from.y.max(to.y) + margin).ceil() as i64).min(i64::from(self.size.1) - 1);
        for y in min_y..=max_y {
            let py = y as f32 + 0.5;
            let (left, right) = if dy.abs() > 0.0001 {
                let a = ((py - margin - from.y) / dy).clamp(0.0, 1.0);
                let b = ((py + margin - from.y) / dy).clamp(0.0, 1.0);
                let xa = from.x + dx * a;
                let xb = from.x + dx * b;
                (xa.min(xb) - margin, xa.max(xb) + margin)
            } else {
                (from.x.min(to.x) - margin, from.x.max(to.x) + margin)
            };
            let min_x = (left.floor() as i64).max(0);
            let max_x = (right.ceil() as i64).min(i64::from(self.size.0) - 1);
            for x in min_x..=max_x {
                let px = x as f32 + 0.5;
                let t = if inverse_length_squared > 0.0 {
                    (((px - from.x) * dx + (py - from.y) * dy) * inverse_length_squared)
                        .clamp(0.0, 1.0)
                } else {
                    1.0
                };
                let distance_x = px - (from.x + dx * t);
                let distance_y = py - (from.y + dy * t);
                let radius = from.radius + (to.radius - from.radius) * t;
                let distance_squared = distance_x * distance_x + distance_y * distance_y;
                if distance_squared >= (radius + 0.5) * (radius + 0.5) {
                    continue;
                }
                let coverage =
                    if radius >= 0.5 && distance_squared <= (radius - 0.5) * (radius - 0.5) {
                        255
                    } else {
                        ((radius + 0.5 - distance_squared.sqrt()).clamp(0.0, 1.0)
                            * (radius * 2.0).min(1.0)
                            * 255.0)
                            .round() as u8
                    };
                let index = y as usize * self.size.0 as usize + x as usize;
                if DENSE {
                    if coverage > self.pixels[index] {
                        if self.pixels[index] == 0 {
                            self.touched.push(index);
                        }
                        self.pixels[index] = coverage;
                    }
                } else {
                    let (tile, local) = self.address(x as u32, y as u32);
                    let pixels = self.tiles.entry(tile).or_insert_with(|| {
                        vec![0; (TILE_SIZE * TILE_SIZE) as usize].into_boxed_slice()
                    });
                    if coverage > pixels[local] {
                        if pixels[local] == 0 {
                            self.touched.push(index);
                        }
                        pixels[local] = coverage;
                    }
                }
            }
        }
    }

    fn address(&self, x: u32, y: u32) -> (usize, usize) {
        let columns = self.size.0.div_ceil(TILE_SIZE);
        (
            (y / TILE_SIZE * columns + x / TILE_SIZE) as usize,
            (y % TILE_SIZE * TILE_SIZE + x % TILE_SIZE) as usize,
        )
    }

    fn blend<const DENSE: bool>(&mut self, surface: &mut impl Surface, color: [u8; 4]) -> bool {
        if DENSE {
            let mut visible = false;
            for index in self.touched.drain(..) {
                let alpha = (u32::from(self.pixels[index]) * u32::from(color[3]) + 127) / 255;
                self.pixels[index] = 0;
                if alpha != 0 {
                    visible = true;
                    surface.blend_index(index, [color[0], color[1], color[2], alpha as u8]);
                }
            }
            return visible;
        }
        let mut visible = false;
        let mut used = HashSet::new();
        let columns = self.size.0.div_ceil(TILE_SIZE);
        for index in self.touched.drain(..) {
            let x = index as u32 % self.size.0;
            let y = index as u32 / self.size.0;
            let tile = (y / TILE_SIZE * columns + x / TILE_SIZE) as usize;
            let local = (y % TILE_SIZE * TILE_SIZE + x % TILE_SIZE) as usize;
            let pixels = self.tiles.get_mut(&tile).expect("observed coverage tile");
            let alpha = (u32::from(pixels[local]) * u32::from(color[3]) + 127) / 255;
            pixels[local] = 0;
            used.insert(tile);
            if alpha != 0 {
                visible = true;
                surface.blend_pixel(
                    x as i32,
                    y as i32,
                    [color[0], color[1], color[2], alpha as u8],
                );
            }
        }
        self.tiles.retain(|key, _| used.contains(key));
        visible
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SIZE: (u32, u32) = (160, 100);
    const COLOR: [u8; 4] = [255, 0, 0, 128];

    fn observe(trail: &mut LaserTrail, position: Option<(i32, i32)>, time: u64) {
        trail.observe(position, SIZE, SIZE, time);
    }

    fn render(trail: &mut LaserTrail, time: u64) -> Vec<u8> {
        let mut pixels = [0, 0, 0, 255].repeat((SIZE.0 * SIZE.1) as usize);
        trail.draw(&mut pixels, SIZE, time, COLOR);
        pixels
    }

    fn red_sum(pixels: &[u8]) -> u64 {
        pixels
            .chunks_exact(4)
            .map(|pixel| u64::from(pixel[0]))
            .sum()
    }

    #[test]
    fn configured_lifetime_controls_decay_and_final_cleanup() {
        for lifetime in [100, 500, 2000] {
            let mut trail = LaserTrail::new(lifetime);
            observe(&mut trail, Some((10, 50)), 0);
            observe(&mut trail, Some((100, 50)), 10);
            let initial = red_sum(&render(&mut trail, 10));
            let faded = red_sum(&render(&mut trail, 10 + lifetime / 2));
            assert!(initial > faded && faded > 0);
            assert!(trail.has_active_animation(10 + lifetime));
            assert_eq!(red_sum(&render(&mut trail, 10 + lifetime)), 0);
            assert!(!trail.has_active_animation(10 + lifetime));
            observe(&mut trail, None, 10 + lifetime);
            assert!(trail.points.is_empty());
        }
    }

    #[test]
    fn sparse_and_dense_coverage_produce_identical_video_pixels() {
        for size in [(257, 131), (1920, 1080)] {
            let mut dense = LaserTrail::default();
            let mut sparse = LaserTrail::default();
            for (index, position) in [
                Some((2, 2)),
                Some((220, 110)),
                Some((5, 110)),
                Some((220, 2)),
                None,
                Some((250, 100)),
                Some((240, 80)),
            ]
            .into_iter()
            .enumerate()
            {
                dense.observe(position, (257, 131), size, index as u64 * 20);
                sparse.observe(position, (257, 131), size, index as u64 * 20);
            }
            for at in [130, 300, 490, 621] {
                for alpha in [1, 128, 255] {
                    let mut expected = [20, 40, 60, 255].repeat((size.0 * size.1) as usize);
                    let mut actual = expected.clone();
                    dense.draw(&mut expected, size, at, [220, 80, 40, alpha]);
                    sparse.draw_to(
                        &mut RgbaSurface {
                            pixels: &mut actual,
                            dimensions: size,
                        },
                        at,
                        [220, 80, 40, alpha],
                    );
                    assert_eq!(
                        actual, expected,
                        "coverage parity at {size:?}, {at}ms, alpha={alpha}"
                    );
                }
            }
        }
    }

    #[test]
    fn initial_and_repeated_stationary_observations_do_not_animate() {
        let mut trail = LaserTrail::default();
        for time in 0..1200 {
            observe(&mut trail, Some((20, 20)), time);
            assert!(!trail.has_active_animation(time));
            assert_eq!(red_sum(&render(&mut trail, time)), 0);
        }
        assert!(trail.points.is_empty());
        assert!(trail.coverage.pixels.is_empty());
    }

    #[test]
    fn stationary_tail_shrinks_expires_and_emits_a_final_clean_frame() {
        let mut trail = LaserTrail::default();
        observe(&mut trail, Some((20, 20)), 0);
        observe(&mut trail, Some((100, 20)), 100);
        let fresh = red_sum(&render(&mut trail, 100));
        let older = red_sum(&render(&mut trail, 450));
        assert!(fresh > older && older > 0);
        assert!(
            trail.has_active_animation(650),
            "last visible frame must be cleared"
        );
        observe(&mut trail, Some((100, 20)), 650);
        assert_eq!(red_sum(&render(&mut trail, 650)), 0);
        assert!(!trail.has_active_animation(650));
        observe(&mut trail, Some((100, 20)), 700);
        assert!(
            trail.points.is_empty(),
            "expiry must not renew a stationary head"
        );
        observe(&mut trail, Some((110, 20)), 1400);
        assert_eq!(
            trail.points.len(),
            1,
            "new movement must not bridge expired geometry"
        );
        observe(&mut trail, Some((120, 20)), 1500);
        assert!(red_sum(&render(&mut trail, 1500)) > 0);
    }

    #[test]
    fn movement_history_keeps_quartic_taper_with_faster_half_second_decay() {
        let mut trail = LaserTrail::default();
        for x in 0..500 {
            observe(&mut trail, Some((x, 20)), 0);
            assert!(trail.points.len() <= 50);
        }
        assert_eq!(trail.points.len(), 50);
        assert_eq!(trail.vertex(0, 0).radius, 0.0);
        assert!((trail.vertex(25, 0).radius - 1.875).abs() < 0.00001);
        assert!((trail.vertex(49, 250).radius - 1.5).abs() < 0.00001);
        assert!((trail.vertex(49, 400).radius - 0.72).abs() < 0.00001);
        assert!(trail.vertex(49, 499).radius > 0.0);
        assert_eq!(trail.vertex(49, 500).radius, 0.0);
        assert_eq!(trail.vertex(49, u64::MAX).radius, 0.0);
    }

    #[test]
    fn downscaling_keeps_distinct_raw_movements_and_subpixel_streamlining() {
        let mut trail = LaserTrail::default();
        trail.observe(Some((0, 0)), (1000, 1000), (100, 100), 0);
        trail.observe(Some((1, 1)), (1000, 1000), (100, 100), 100);
        assert_eq!(trail.points.len(), 2);
        assert!((trail.points[1].x - 0.06).abs() < 0.00001);
        assert_eq!(trail.points[1].timestamp_ms, 100);
        trail.observe(Some((1, 1)), (1000, 1000), (100, 100), 450);
        assert_eq!(trail.points.len(), 2);
        assert_eq!(trail.points[1].timestamp_ms, 100);
    }

    #[test]
    fn invisible_cursor_breaks_path_without_erasing_the_fading_tail() {
        let mut trail = LaserTrail::default();
        observe(&mut trail, Some((10, 20)), 0);
        observe(&mut trail, Some((30, 20)), 10);
        observe(&mut trail, None, 20);
        assert!(red_sum(&render(&mut trail, 20)) > 0);
        observe(&mut trail, Some((120, 20)), 30);
        observe(&mut trail, Some((140, 20)), 40);
        let pixels = render(&mut trail, 40);
        for x in 40..110 {
            assert_eq!(
                pixels[(20 * SIZE.0 as usize + x) * 4],
                0,
                "visibility gap must not connect"
            );
        }
    }

    #[test]
    fn antialiased_crossings_blend_user_opacity_once_and_clear_scratch() {
        let mut trail = LaserTrail::default();
        for (index, point) in [(20, 20), (100, 80), (20, 80), (100, 20), (20, 20)]
            .into_iter()
            .enumerate()
        {
            observe(&mut trail, Some(point), index as u64 * 20);
        }
        let pixels = render(&mut trail, 100);
        assert!(pixels.chunks_exact(4).any(|pixel| pixel[0] == 128));
        assert!(
            pixels
                .chunks_exact(4)
                .any(|pixel| pixel[0] > 0 && pixel[0] < 128)
        );
        assert!(pixels.chunks_exact(4).all(|pixel| pixel[0] <= 128));
        assert!(trail.coverage.touched.is_empty());
        assert!(trail.coverage.pixels.iter().all(|value| *value == 0));
        assert_eq!(
            pixels,
            render(&mut trail, 100),
            "scratch reuse must be deterministic"
        );
        assert_eq!(red_sum(&render(&mut trail, 1100)), 0);
    }

    #[test]
    fn coverage_union_is_invariant_under_repeated_spans() {
        let mut coverage = Coverage::default();
        coverage.prepare(SIZE);
        let from = Vertex {
            x: 10.25,
            y: 10.25,
            radius: 2.0,
        };
        let to = Vertex {
            x: 80.75,
            y: 70.75,
            radius: 1.0,
        };
        coverage.segment(from, to);
        let single = coverage.pixels.clone();
        for _ in 0..50 {
            coverage.segment(from, to);
        }
        assert_eq!(coverage.pixels, single);
        assert_eq!(
            coverage.touched.len(),
            single.iter().filter(|value| **value > 0).count()
        );
    }

    #[test]
    fn scanline_clipping_matches_exhaustive_pixel_coverage() {
        let size = (32, 24);
        for (a, b) in [
            ((-10.0, 10.2), (40.0, 11.8)),
            ((-10.0, 11.8), (40.0, 10.2)),
            ((10.2, -10.0), (11.8, 40.0)),
            ((31.7, 1.3), (0.3, 23.7)),
            ((-1.0, -1.0), (-1.0, -1.0)),
            ((32.5, 24.5), (32.5, 24.5)),
            ((1.25, 1.25), (30.75, 21.25)),
        ] {
            for (start_radius, end_radius) in [(2.0, 2.0), (0.0, 2.0), (2.0, 0.0), (0.1, 0.2)] {
                let from = Vertex {
                    x: a.0,
                    y: a.1,
                    radius: start_radius,
                };
                let to = Vertex {
                    x: b.0,
                    y: b.1,
                    radius: end_radius,
                };
                let mut coverage = Coverage::default();
                coverage.prepare(size);
                coverage.segment(from, to);
                // Independent f64 oracle visits the entire surface, with no clipping
                // shortcuts. Any omitted antialiased edge/cap pixel must still match.
                let dx = f64::from(to.x) - f64::from(from.x);
                let dy = f64::from(to.y) - f64::from(from.y);
                let length_squared = dx * dx + dy * dy;
                for y in 0..size.1 {
                    for x in 0..size.0 {
                        let px = f64::from(x) + 0.5 - f64::from(from.x);
                        let py = f64::from(y) + 0.5 - f64::from(from.y);
                        let t = if length_squared == 0.0 {
                            1.0
                        } else {
                            ((px * dx + py * dy) / length_squared).clamp(0.0, 1.0)
                        };
                        let radius =
                            f64::from(start_radius) * (1.0 - t) + f64::from(end_radius) * t;
                        let distance = (px - t * dx).hypot(py - t * dy);
                        let expected = ((radius + 0.5 - distance).clamp(0.0, 1.0)
                            * (radius * 2.0).min(1.0)
                            * 255.0)
                            .round() as u8;
                        let actual = coverage.pixels[(y * size.0 + x) as usize];
                        assert!(
                            actual.abs_diff(expected) <= 1,
                            "{a:?}->{b:?}, radius {start_radius}->{end_radius}, pixel {x},{y}: {actual} vs {expected}"
                        );
                    }
                }
            }
        }
    }

    #[test]
    fn clipped_extreme_coordinates_and_resize_remain_bounded() {
        let mut trail = LaserTrail::default();
        for (index, point) in [
            (i32::MIN, i32::MIN),
            (i32::MAX, i32::MAX),
            (i32::MIN, i32::MAX),
            (i32::MAX, i32::MIN),
        ]
        .into_iter()
        .enumerate()
        {
            observe(&mut trail, Some(point), index as u64);
        }
        render(&mut trail, 10);
        assert_eq!(trail.coverage.pixels.len(), (SIZE.0 * SIZE.1) as usize);
        for size in [(1, 1), (0, 0), (32, 16)] {
            let mut pixels = vec![0; (size.0 * size.1 * 4) as usize];
            trail.draw(&mut pixels, size, 20, COLOR);
            assert!(trail.coverage.touched.is_empty());
        }
    }

    #[test]
    fn disabled_color_is_noop_and_clear_resets_observations() {
        let mut trail = LaserTrail::default();
        observe(&mut trail, Some((10, 20)), 0);
        observe(&mut trail, Some((30, 20)), 10);
        let mut pixels = vec![37; (SIZE.0 * SIZE.1 * 4) as usize];
        trail.draw(&mut pixels, SIZE, 10, [255, 0, 0, 0]);
        assert!(pixels.iter().all(|value| *value == 37));
        assert!(trail.coverage.pixels.is_empty());
        trail.clear();
        assert!(!trail.has_active_animation(10));
        observe(&mut trail, Some((30, 20)), 20);
        assert_eq!(trail.points.len(), 1);
        assert!(!trail.has_active_animation(20));
    }
}
