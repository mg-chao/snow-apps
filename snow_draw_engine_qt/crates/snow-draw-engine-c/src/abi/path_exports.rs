use crate::abi::{convert::snow_arrow_path_commands_from_rust, handles::*, types::*};
use snow_draw_engine_core::{PathSegmentMode, Point, catmull_rom_path_commands};

/// Builds the same curve used by closed free-draw elements. The caller owns both
/// buffers; a null output queries the required command count.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_build_catmull_rom_path(
    vertices: *const SnowArrowPoint,
    vertex_count: usize,
    closed: u8,
    commands: *mut SnowArrowPathCommand,
    capacity: usize,
    count: *mut usize,
) -> SnowError {
    ffi_error(|| {
        if count.is_null() || (vertices.is_null() && vertex_count != 0) || vertex_count > 65536 {
            return SnowError::InvalidArgument;
        }
        let points = if vertex_count == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(vertices, vertex_count) }
        };
        if points.iter().any(|p| !p.x.is_finite() || !p.y.is_finite()) {
            return SnowError::InvalidArgument;
        }
        let points: Vec<_> = points.iter().map(|p| Point::new(p.x, p.y)).collect();
        let modes = vec![PathSegmentMode::Curve; points.len()];
        let path = catmull_rom_path_commands(&points, &modes, closed != 0);
        let output = snow_arrow_path_commands_from_rust(&path);
        write_out(count, output.len());
        if !commands.is_null() {
            if capacity < output.len() {
                return SnowError::BufferTooSmall;
            }
            unsafe { std::ptr::copy_nonoverlapping(output.as_ptr(), commands, output.len()) };
        }
        SnowError::Ok
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn curve_bridge_matches_core_and_rejects_invalid_input() {
        let points = [
            SnowArrowPoint { x: 0.0, y: 0.0 },
            SnowArrowPoint { x: 80.0, y: 10.0 },
            SnowArrowPoint { x: 30.0, y: 70.0 },
        ];
        let mut output = [SnowArrowPathCommand::default(); 4];
        let mut count = 0;
        unsafe {
            assert_eq!(
                snow_build_catmull_rom_path(
                    points.as_ptr(),
                    3,
                    1,
                    output.as_mut_ptr(),
                    4,
                    &mut count
                ),
                SnowError::Ok
            );
        }
        let expected = catmull_rom_path_commands(
            &points.map(|p| Point::new(p.x, p.y)),
            &[PathSegmentMode::Curve; 3],
            true,
        );
        assert_eq!(count, 4);
        assert_eq!(
            output.as_slice(),
            snow_arrow_path_commands_from_rust(&expected)
        );
        unsafe {
            assert_eq!(
                snow_build_catmull_rom_path(
                    points.as_ptr(),
                    3,
                    1,
                    output.as_mut_ptr(),
                    1,
                    &mut count
                ),
                SnowError::BufferTooSmall
            );
            assert_eq!(
                snow_build_catmull_rom_path(
                    std::ptr::null(),
                    3,
                    1,
                    output.as_mut_ptr(),
                    4,
                    &mut count
                ),
                SnowError::InvalidArgument
            );
        }
    }
    #[test]
    fn curve_bridge_handles_open_closed_and_repeated_vertices() {
        let cases = [
            vec![],
            vec![SnowArrowPoint { x: 3.0, y: 4.0 }],
            vec![
                SnowArrowPoint { x: 0.0, y: 0.0 },
                SnowArrowPoint { x: 0.0, y: 0.0 },
                SnowArrowPoint { x: 70.0, y: 5.0 },
                SnowArrowPoint { x: 25.0, y: 90.0 },
            ],
        ];
        for points in cases {
            for closed in [0, 1] {
                let mut count = 0;
                unsafe {
                    assert_eq!(
                        snow_build_catmull_rom_path(
                            points.as_ptr(),
                            points.len(),
                            closed,
                            std::ptr::null_mut(),
                            0,
                            &mut count
                        ),
                        SnowError::Ok
                    );
                }
                let mut commands = vec![SnowArrowPathCommand::default(); count];
                unsafe {
                    assert_eq!(
                        snow_build_catmull_rom_path(
                            points.as_ptr(),
                            points.len(),
                            closed,
                            commands.as_mut_ptr(),
                            commands.len(),
                            &mut count
                        ),
                        SnowError::Ok
                    );
                }
                let vertices: Vec<_> = points.iter().map(|p| Point::new(p.x, p.y)).collect();
                let expected = catmull_rom_path_commands(
                    &vertices,
                    &vec![PathSegmentMode::Curve; vertices.len()],
                    closed != 0,
                );
                assert_eq!(commands, snow_arrow_path_commands_from_rust(&expected));
            }
        }
    }
}
