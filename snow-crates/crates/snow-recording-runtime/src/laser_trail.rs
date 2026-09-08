//! Recording-space adaptation of Excalidraw's laser effect: streamline 0.4,
//! radius 2, faster 500 ms time decay and a quartic taper over the last 50 movements.
//! Unlike the reference, timestamps stay at the actual observation time. We smooth
//! the centerline with midpoint quadratics and round joins rather than its SVG
//! outline. This keeps sharp turns bounded and allows a reusable coverage union:
//! translucent overlaps are composited once, never darkened by repeated stamps.
use std::collections::VecDeque;

const LIFETIME_MS: u64 = 500;
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

#[derive(Default)]
pub(crate) struct LaserTrail {
    points: VecDeque<Sample>,
    last_position: Option<(i32, i32)>,
    coverage: Coverage,
    was_visible: bool,
}

impl LaserTrail {
    pub(crate) fn clear(&mut self) {
        self.points.clear();
        self.last_position = None;
        self.was_visible = false;
    }

    pub(crate) fn observe(
        &mut self,
        position: Option<(i32, i32)>,
        source_size: (u32, u32),
        output_size: (u32, u32),
        timestamp_ms: u64,
    ) {
        // Keep one expired predecessor for a continuous taper at the time boundary.
        while self.points.len() > 1
            && timestamp_ms.saturating_sub(self.points[1].timestamp_ms) >= LIFETIME_MS
        {
            self.points.pop_front();
        }
        if self
            .points
            .back()
            .is_some_and(|point| timestamp_ms.saturating_sub(point.timestamp_ms) >= LIFETIME_MS)
        {
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

    pub(crate) fn has_active_animation(&self, timestamp_ms: u64) -> bool {
        // The last visible frame needs a clean successor even if a frame interval
        // skips over the exact expiry time. A lone initial observation is invisible.
        self.was_visible
            || self.points.iter().skip(1).any(|point| {
                point.connected && timestamp_ms.saturating_sub(point.timestamp_ms) < LIFETIME_MS
            })
    }

    fn vertex(&self, index: usize, timestamp_ms: u64) -> Vertex {
        let point = self.points[index];
        let age = timestamp_ms.saturating_sub(point.timestamp_ms);
        let elapsed = age.min(LIFETIME_MS) as f32 / LIFETIME_MS as f32;
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

    pub(crate) fn draw(
        &mut self,
        rgba: &mut [u8],
        size: (u32, u32),
        timestamp_ms: u64,
        color: [u8; 4],
    ) {
        self.was_visible = false;
        if color[3] == 0 || self.points.len() < 2 || size.0 == 0 || size.1 == 0 {
            return;
        }
        self.coverage.prepare(size);
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
                    self.coverage.quadratic(from, control, to, 0);
                    from = to;
                }
                self.coverage
                    .segment(from, self.vertex(end - 1, timestamp_ms));
            }
            start = end;
        }
        self.was_visible = self.coverage.blend(rgba, color);
    }
}

#[derive(Default)]
struct Coverage {
    size: (u32, u32),
    pixels: Vec<u8>,
    touched: Vec<usize>,
}

impl Coverage {
    fn prepare(&mut self, size: (u32, u32)) {
        self.size = size;
        self.pixels.resize(size.0 as usize * size.1 as usize, 0);
    }

    fn quadratic(&mut self, from: Vertex, control: Vertex, to: Vertex, depth: u8) {
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
            self.segment(from, to);
            return;
        }
        let left = from.midpoint(control);
        let right = control.midpoint(to);
        let center = left.midpoint(right);
        self.quadratic(from, left, center, depth + 1);
        self.quadratic(center, right, to, depth + 1);
    }

    fn segment(&mut self, from: Vertex, to: Vertex) {
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
                if coverage > self.pixels[index] {
                    if self.pixels[index] == 0 {
                        self.touched.push(index);
                    }
                    self.pixels[index] = coverage;
                }
            }
        }
    }

    fn blend(&mut self, rgba: &mut [u8], color: [u8; 4]) -> bool {
        let mut visible = false;
        for index in self.touched.drain(..) {
            let alpha = (u32::from(self.pixels[index]) * u32::from(color[3]) + 127) / 255;
            self.pixels[index] = 0;
            if alpha == 0 {
                continue;
            }
            let Some(pixel) = rgba.get_mut(index * 4..index * 4 + 4) else {
                continue;
            };
            visible = true;
            for channel in 0..3 {
                pixel[channel] = ((u32::from(color[channel]) * alpha
                    + u32::from(pixel[channel]) * (255 - alpha)
                    + 127)
                    / 255) as u8;
            }
            pixel[3] = 255;
        }
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
