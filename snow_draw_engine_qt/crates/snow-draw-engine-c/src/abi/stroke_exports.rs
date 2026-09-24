use crate::abi::{handles::*, types::*};
use snow_draw_engine_core::{Point, StrokePointFilter};

pub struct SnowStrokeFilter {
    filter: StrokePointFilter,
    points: Vec<Point<f64>>,
    output: Vec<SnowArrowPoint>,
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_stroke_filter_create(
    start: SnowArrowPoint,
    sample_spacing: f64,
    response_distance: f64,
    output: *mut *mut SnowStrokeFilter,
) -> SnowError {
    ffi_error(|| {
        if output.is_null()
            || !start.x.is_finite()
            || !start.y.is_finite()
            || !sample_spacing.is_finite()
            || sample_spacing <= 0.0
            || !response_distance.is_finite()
            || response_distance <= 0.0
        {
            return SnowError::InvalidArgument;
        }
        write_out(
            output,
            Box::into_raw(Box::new(SnowStrokeFilter {
                filter: StrokePointFilter::new(
                    Point::new(start.x, start.y),
                    sample_spacing,
                    response_distance,
                ),
                points: Vec::new(),
                output: Vec::new(),
            })),
        );
        SnowError::Ok
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_stroke_filter_free(filter: *mut SnowStrokeFilter) {
    ffi_void(|| {
        if !filter.is_null() {
            drop(unsafe { Box::from_raw(filter) });
        }
    });
}

/// Processes only new samples. Output is borrowed until the next append or free.
/// Finishing recovers the exact endpoint once, rather than bypassing stabilization
/// on each preview. Empty batches are valid, including a finish-only call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_stroke_filter_append(
    filter: *mut SnowStrokeFilter,
    points: *const SnowArrowPoint,
    count: usize,
    finish: u8,
    output: *mut *const SnowArrowPoint,
    output_count: *mut usize,
) -> SnowError {
    ffi_error(|| {
        if filter.is_null()
            || output.is_null()
            || output_count.is_null()
            || (points.is_null() && count != 0)
            || count > 65536
        {
            return SnowError::InvalidArgument;
        }
        let input = if count == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(points, count) }
        };
        // Validate the entire batch before changing the filter.
        if input.iter().any(|p| !p.x.is_finite() || !p.y.is_finite()) {
            return SnowError::InvalidArgument;
        }
        let state = unsafe { &mut *filter };
        // Bound work as well as input size: a finite but enormous jump (or tiny
        // spacing) must not expand into an unbounded resampling loop.
        let mut previous = state.filter.last_raw();
        let mut sample_budget = 1.0;
        for point in input {
            sample_budget +=
                (point.x - previous.x).hypot(point.y - previous.y) / state.filter.sample_spacing();
            if sample_budget > 65536.0 {
                return SnowError::InvalidArgument;
            }
            previous = Point::new(point.x, point.y);
        }
        state.points.clear();
        state.output.clear();
        for point in input {
            state
                .filter
                .ingest(Point::new(point.x, point.y), &mut state.points);
        }
        if finish != 0
            && let Some(point) = state.filter.settle_endpoint()
        {
            state.points.push(point);
        }
        state.output.extend(
            state
                .points
                .iter()
                .map(|p| SnowArrowPoint { x: p.x, y: p.y }),
        );
        write_out(output, state.output.as_ptr());
        write_out(output_count, state.output.len());
        SnowError::Ok
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn batches_match_core_and_invalid_batch_does_not_advance_state() {
        let input: Vec<_> = (1..=600)
            .map(|i| SnowArrowPoint {
                x: i as f64 * 0.1,
                y: if i % 2 == 0 { 1.0 } else { -1.0 },
            })
            .collect();
        let mut core = StrokePointFilter::new(Point::new(0.0, 0.0), 0.75, 6.0);
        let mut expected = Vec::new();
        for point in &input {
            core.ingest(Point::new(point.x, point.y), &mut expected);
        }
        expected.extend(core.settle_endpoint());
        for batch_size in [1, 16, 128, 600] {
            unsafe {
                let mut filter = std::ptr::null_mut();
                assert_eq!(
                    snow_stroke_filter_create(
                        SnowArrowPoint { x: 0.0, y: 0.0 },
                        0.75,
                        6.0,
                        &mut filter
                    ),
                    SnowError::Ok
                );
                let mut output = std::ptr::null();
                let mut count = 0;
                let invalid = [
                    SnowArrowPoint { x: 99.0, y: 99.0 },
                    SnowArrowPoint {
                        x: f64::NAN,
                        y: 0.0,
                    },
                ];
                assert_eq!(
                    snow_stroke_filter_append(
                        filter,
                        invalid.as_ptr(),
                        invalid.len(),
                        0,
                        &mut output,
                        &mut count
                    ),
                    SnowError::InvalidArgument
                );
                let oversized = [SnowArrowPoint { x: 1e100, y: 0.0 }];
                assert_eq!(
                    snow_stroke_filter_append(
                        filter,
                        oversized.as_ptr(),
                        1,
                        0,
                        &mut output,
                        &mut count
                    ),
                    SnowError::InvalidArgument
                );
                let mut actual = Vec::new();
                for batch in input.chunks(batch_size) {
                    assert_eq!(
                        snow_stroke_filter_append(
                            filter,
                            batch.as_ptr(),
                            batch.len(),
                            0,
                            &mut output,
                            &mut count
                        ),
                        SnowError::Ok
                    );
                    actual.extend(
                        std::slice::from_raw_parts(output, count)
                            .iter()
                            .map(|p| Point::new(p.x, p.y)),
                    );
                }
                assert_eq!(
                    snow_stroke_filter_append(
                        filter,
                        std::ptr::null(),
                        0,
                        1,
                        &mut output,
                        &mut count
                    ),
                    SnowError::Ok
                );
                actual.extend(
                    std::slice::from_raw_parts(output, count)
                        .iter()
                        .map(|p| Point::new(p.x, p.y)),
                );
                assert_eq!(actual, expected);
                assert_eq!(
                    snow_stroke_filter_append(
                        filter,
                        std::ptr::null(),
                        0,
                        1,
                        &mut output,
                        &mut count
                    ),
                    SnowError::Ok
                );
                assert_eq!(count, 0, "repeated finish must not add another endpoint");
                snow_stroke_filter_free(filter);
            }
        }
    }

    #[test]
    fn rejects_invalid_parameters_and_null_buffers() {
        unsafe {
            let start = SnowArrowPoint { x: 0.0, y: 0.0 };
            let mut filter = std::ptr::null_mut();
            for spacing in [0.0, -1.0, f64::NAN, f64::INFINITY] {
                assert_eq!(
                    snow_stroke_filter_create(start, spacing, 6.0, &mut filter),
                    SnowError::InvalidArgument
                );
                assert!(filter.is_null());
            }
            assert_eq!(
                snow_stroke_filter_create(start, 0.75, 6.0, std::ptr::null_mut()),
                SnowError::InvalidArgument
            );
            assert_eq!(
                snow_stroke_filter_append(
                    filter,
                    std::ptr::null(),
                    0,
                    0,
                    std::ptr::null_mut(),
                    std::ptr::null_mut()
                ),
                SnowError::InvalidArgument
            );
            snow_stroke_filter_free(filter);
        }
    }
}
