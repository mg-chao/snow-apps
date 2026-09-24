//! Shared spatial stabilization for free-draw strokes and freehand selections.
use crate::Point;

const MIN_FILTER_RESPONSE: f64 = 0.18;
const MAX_FILTER_RESPONSE: f64 = 0.82;

// Resampling makes stabilization depend on stroke geometry instead of device event frequency.
#[derive(Clone, Debug, PartialEq)]
pub struct StrokePointFilter {
    last_raw: Point<f64>,
    filtered: Point<f64>,
    smoothed: Point<f64>,
    sample_spacing: f64,
    distance_until_sample: f64,
    response_distance: f64,
}

impl StrokePointFilter {
    pub fn new(start: Point<f64>, sample_spacing: f64, response_distance: f64) -> Self {
        Self {
            last_raw: start,
            filtered: start,
            smoothed: start,
            sample_spacing,
            distance_until_sample: sample_spacing,
            response_distance,
        }
    }

    pub fn reset(&mut self, point: Point<f64>) {
        self.last_raw = point;
        self.filtered = point;
        self.smoothed = point;
        self.distance_until_sample = self.sample_spacing;
    }

    pub fn ingest(&mut self, point: Point<f64>, output: &mut Vec<Point<f64>>) {
        let mut segment_start = self.last_raw;
        let mut segment_length = distance(segment_start, point);
        if segment_length <= 1e-12 {
            self.last_raw = point;
            return;
        }

        while segment_length + 1e-12 >= self.distance_until_sample {
            let ratio = self.distance_until_sample / segment_length;
            let sample = lerp_point(segment_start, point, ratio);
            output.push(self.stabilize(sample));
            segment_start = sample;
            segment_length = distance(segment_start, point);
            self.distance_until_sample = self.sample_spacing;
        }
        self.distance_until_sample = (self.distance_until_sample - segment_length).max(1e-12);
        self.last_raw = point;
    }

    fn stabilize(&mut self, sample: Point<f64>) -> Point<f64> {
        let normalized_error =
            (distance(self.filtered, sample) / self.response_distance).clamp(0.0, 1.0);
        let adaptive = smoothstep(normalized_error);
        let response = MIN_FILTER_RESPONSE + (MAX_FILTER_RESPONSE - MIN_FILTER_RESPONSE) * adaptive;
        self.filtered = lerp_point(self.filtered, sample, response);
        // A second spatial pole attenuates alternating micro-movements without
        // increasing the response distance of the first stage. The fixed spatial
        // sampling keeps this independent of pointer event frequency.
        self.smoothed = lerp_point(self.smoothed, self.filtered, 0.5);
        self.smoothed
    }

    pub fn last_raw(&self) -> Point<f64> {
        self.last_raw
    }

    pub fn sample_spacing(&self) -> f64 {
        self.sample_spacing
    }

    pub fn response_distance(&self) -> f64 {
        self.response_distance
    }

    pub fn settle_endpoint(&mut self) -> Option<Point<f64>> {
        if distance(self.smoothed, self.last_raw) <= 1e-12 {
            return None;
        }
        self.filtered = self.last_raw;
        self.smoothed = self.last_raw;
        self.distance_until_sample = self.sample_spacing;
        Some(self.filtered)
    }
}

fn distance(left: Point<f64>, right: Point<f64>) -> f64 {
    (right.x - left.x).hypot(right.y - left.y)
}

fn lerp_point(start: Point<f64>, end: Point<f64>, ratio: f64) -> Point<f64> {
    Point::new(
        start.x + (end.x - start.x) * ratio,
        start.y + (end.y - start.y) * ratio,
    )
}

fn smoothstep(value: f64) -> f64 {
    value * value * (3.0 - 2.0 * value)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stabilization_reduces_slow_pointer_jitter() {
        let mut filter = StrokePointFilter::new(Point::new(0.0, 0.0), 1.0, 6.0);
        let mut filtered = Vec::new();
        for x in 1..=120 {
            let y = if x % 2 == 0 { 1.0 } else { -1.0 };
            filter.ingest(Point::new(x as f64, y), &mut filtered);
        }

        let settled = &filtered[12..];
        let maximum_deviation = settled
            .iter()
            .map(|point| point.y.abs())
            .fold(0.0_f64, f64::max);
        assert!(
            maximum_deviation < 0.35,
            "stabilized deviation was {maximum_deviation}"
        );
    }

    #[test]
    fn spatial_resampling_is_independent_of_event_density() {
        fn filtered_polyline(subdivisions: usize) -> Vec<Point<f64>> {
            let controls = [
                Point::new(0.0, 0.0),
                Point::new(30.0, 8.0),
                Point::new(60.0, -6.0),
                Point::new(90.0, 0.0),
            ];
            let mut filter = StrokePointFilter::new(controls[0], 1.0, 6.0);
            let mut output = Vec::new();
            for segment in controls.windows(2) {
                for step in 1..=subdivisions {
                    filter.ingest(
                        lerp_point(segment[0], segment[1], step as f64 / subdivisions as f64),
                        &mut output,
                    );
                }
            }
            output
        }

        let sparse = filtered_polyline(1);
        let dense = filtered_polyline(20);
        assert_eq!(sparse.len(), dense.len());
        assert!(
            sparse
                .iter()
                .zip(dense)
                .all(|(left, right)| distance(*left, right) < 1e-9)
        );
    }

    #[test]
    fn adaptive_response_tracks_deliberate_corners() {
        let mut filter = StrokePointFilter::new(Point::new(0.0, 0.0), 1.0, 6.0);
        let mut filtered = Vec::new();
        for x in 1..=40 {
            filter.ingest(Point::new(x as f64, 0.0), &mut filtered);
        }
        for y in 1..=40 {
            filter.ingest(Point::new(40.0, y as f64), &mut filtered);
        }

        let corner = Point::new(40.0, 0.0);
        let closest_corner_distance = filtered
            .iter()
            .map(|point| distance(*point, corner))
            .fold(f64::INFINITY, f64::min);
        assert!(
            closest_corner_distance < 3.0,
            "corner miss distance was {closest_corner_distance}"
        );
        assert!(distance(*filtered.last().unwrap(), Point::new(40.0, 40.0)) < 3.0);
    }

    #[test]
    fn subpixel_forward_motion_rejects_lateral_jitter() {
        let mut filter = StrokePointFilter::new(Point::new(0.0, 0.0), 0.75, 6.0);
        let mut output = Vec::new();
        for i in 1..=1200 {
            filter.ingest(
                Point::new(i as f64 * 0.1, if i % 2 == 0 { 1.0 } else { -1.0 }),
                &mut output,
            );
        }
        let deviation = output
            .iter()
            .filter(|p| p.x > 10.0)
            .map(|p| p.y.abs())
            .fold(0.0_f64, f64::max);
        assert!(
            deviation < 0.12,
            "slow stroke lateral deviation: {deviation}"
        );
    }

    #[test]
    fn reset_and_short_strokes_preserve_endpoints() {
        let mut filter = StrokePointFilter::new(Point::new(0.0, 0.0), 0.75, 6.0);
        let mut output = Vec::new();
        filter.ingest(Point::new(0.2, 0.1), &mut output);
        assert!(output.is_empty());
        assert_eq!(filter.settle_endpoint(), Some(Point::new(0.2, 0.1)));
        assert_eq!(filter.settle_endpoint(), None);
        filter.reset(Point::new(40.0, 20.0));
        filter.ingest(Point::new(40.0, 20.0), &mut output);
        assert!(output.is_empty());
        assert_eq!(filter.settle_endpoint(), None);
        filter.ingest(Point::new(42.0, 20.0), &mut output);
        assert!(output.iter().all(|p| p.x >= 40.0 && p.y == 20.0));
    }
}
