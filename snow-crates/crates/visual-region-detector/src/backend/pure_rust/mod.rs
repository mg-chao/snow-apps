//! Pure Rust implementation of the backend contract.

use super::{Backend, Component, KernelShape, MorphOp};
use crate::error::Result;
use crate::geometry::Rect;
use crate::grid::{Grid, Image, Mask};

mod canny;
mod contours;
mod draw;
mod median;
mod morph;

#[cfg(test)]
mod align;

pub(crate) struct PureRustBackend;

impl Backend for PureRustBackend {
    const NAME: &'static str = "pure-rust";

    fn morphology(
        input: &Mask,
        op: MorphOp,
        kernel_width: i32,
        kernel_height: i32,
        shape: KernelShape,
    ) -> Result<Mask> {
        Ok(morph::morphology(
            input,
            op,
            kernel_width,
            kernel_height,
            shape,
        ))
    }

    fn connected_components(input: &Mask) -> Result<(Grid<i32>, Vec<Component>)> {
        Ok(morph::connected_components(input))
    }

    fn median_blur_image(input: &Image, kernel: i32) -> Result<Image> {
        Ok(median::median_blur_image(input, kernel))
    }

    fn blur_row(input: &[f32], kernel: i32) -> Result<Vec<f32>> {
        Ok(morph::blur_row(input, kernel))
    }

    fn median_row(input: &[u8], kernel: i32) -> Result<Vec<u8>> {
        Ok(morph::median_row(input, kernel))
    }

    fn outlined_controls(input: &Image) -> Result<(Vec<Rect>, Mask)> {
        Ok(contours::outlined_controls(input))
    }
}

#[cfg(test)]
mod tests {
    use super::canny::canny_edges;
    use super::contours::{
        bounding_rect_for_test, contour_area_for_test, find_contours, outlined_controls,
    };
    use super::draw::draw_contours;
    use super::median::median_blur_image;
    use super::morph::{blur_row, connected_components, ellipse_kernel, median_row, morphology};
    use super::{KernelShape, MorphOp};
    use crate::grid::{Image, Mask};

    fn image_from_gray(rows: &[&[u8]]) -> Image {
        let h = rows.len() as i32;
        let w = rows[0].len() as i32;
        Image {
            w,
            h,
            data: rows
                .iter()
                .flat_map(|row| row.iter().map(|&value| [value, value, value]))
                .collect(),
        }
    }

    #[test]
    fn ellipse_kernels_match_opencv() {
        assert_eq!(ellipse_kernel(3, 3).data, [0, 1, 0, 1, 1, 1, 0, 1, 0]);
        assert_eq!(
            ellipse_kernel(4, 4).data,
            [0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1]
        );
        assert_eq!(
            ellipse_kernel(5, 5).data,
            [
                0, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0
            ]
        );
    }

    #[test]
    fn rectangular_morphology_matches_opencv_fixtures() {
        let mut input = Mask::new(11, 9, 0);
        for y in 2..7 {
            for x in 3..8 {
                input.set(x, y, 255);
            }
        }
        input.set(5, 4, 0);
        let dilated = morphology(&input, MorphOp::Dilate, 3, 3, KernelShape::Rect);
        assert_eq!(dilated.data[11], 0);
        assert_eq!(dilated.data[13], 255);
        assert_eq!(dilated.data[19], 255);
        let opened = morphology(&input, MorphOp::Open, 3, 3, KernelShape::Rect);
        assert!(opened.data.iter().all(|value| *value == 0));
        let closed = morphology(&input, MorphOp::Close, 3, 3, KernelShape::Rect);
        for y in 2..7 {
            for x in 3..8 {
                assert_eq!(*closed.at(x, y), 255, "{x},{y}");
            }
        }
        let even = morphology(&input, MorphOp::Dilate, 4, 4, KernelShape::Rect);
        assert_eq!(*even.at(9, 1), 255);
        assert_eq!(*even.at(10, 1), 0);
        assert_eq!(*even.at(2, 8), 255);
        let wide = morphology(&input, MorphOp::Close, 17, 9, KernelShape::Rect);
        assert_eq!(wide.data.iter().filter(|value| **value > 0).count(), 99);
    }

    #[test]
    fn connected_components_use_opencv_label_order() {
        let mut input = Mask::new(10, 8, 0);
        for y in 1..4 {
            for x in 1..4 {
                input.set(x, y, 1);
            }
        }
        for y in 5..7 {
            for x in 6..9 {
                input.set(x, y, 255);
            }
        }
        let (labels, components) = connected_components(&input);
        assert_eq!(components.len(), 2);
        assert_eq!(
            labels.data,
            [
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0,
                0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
            ]
        );
        assert_eq!(components[0].rect.x, 1);
        assert_eq!(components[0].area, 9);
        assert_eq!(components[1].rect.x, 6);
        assert_eq!(components[1].area, 6);
    }

    #[test]
    fn blur_and_median_match_opencv_border_rules() {
        let row = [0.0, 10.0, 20.0, 30.0, 40.0, 50.0, 60.0];
        let blur3 = blur_row(&row, 3);
        assert!((blur3[0] - 10.0 / 3.0 * 2.0).abs() < 1e-5);
        assert!((blur3[1] - 10.0).abs() < 1e-5);
        assert_eq!(
            blur_row(&row, 4),
            [10.0, 10.0, 15.0, 25.0, 35.0, 45.0, 50.0]
        );
        assert_eq!(
            median_row(&[0, 0, 1, 1, 1, 0, 0, 1, 0, 0], 5),
            [0, 0, 1, 1, 1, 1, 0, 0, 0, 0]
        );

        let mut image = Image::new(7, 5, [0; 3]);
        for y in 0..5 {
            for x in 0..7 {
                let base = (y * 10 + x) as u8;
                image.set(x, y, [base, base + 1, base + 2]);
            }
        }
        let blurred = median_blur_image(&image, 3);
        assert_eq!(*blurred.at(0, 0), [1, 2, 3]);
        assert_eq!(*blurred.at(6, 0), [6, 7, 8]);
        assert_eq!(*blurred.at(3, 2), [23, 24, 25]);
        assert_eq!(*blurred.at(0, 4), [40, 41, 42]);
        assert_eq!(*blurred.at(6, 4), [45, 46, 47]);
    }

    #[test]
    fn canny_includes_image_borders() {
        let mut rows = vec![vec![0u8; 8]; 5];
        for row in &mut rows {
            row[4..].fill(200);
        }
        let refs: Vec<&[u8]> = rows.iter().map(|row| row.as_slice()).collect();
        let edges = canny_edges(&image_from_gray(&refs));
        for y in 0..5 {
            assert_eq!(
                &edges.data[(y * 8) as usize..(y * 8 + 8) as usize],
                [0, 0, 0, 255, 0, 0, 0, 0]
            );
        }

        let mut wide = vec![vec![0u8; 16]; 12];
        for row in &mut wide {
            row[8..].fill(255);
        }
        let refs: Vec<&[u8]> = wide.iter().map(|row| row.as_slice()).collect();
        let edges = canny_edges(&image_from_gray(&refs));
        for y in 0..12 {
            assert_eq!(edges.data[y * 16 + 7], 255);
            assert_eq!(edges.data[y * 16 + 8], 0);
        }
    }

    #[test]
    fn draw_contours_matches_opencv_thickness_three() {
        let mut mask = Mask::new(30, 20, 0);
        draw_contours(&mut mask, &[vec![(5, 5), (24, 5), (24, 14), (5, 14)]], 3);
        assert_eq!(mask.data.iter().filter(|value| **value == 255).count(), 268);

        let mut horizontal = Mask::new(21, 11, 0);
        draw_contours(&mut horizontal, &[vec![(2, 5), (18, 5)]], 3);
        assert_eq!(
            horizontal
                .data
                .iter()
                .filter(|value| **value == 255)
                .count(),
            93
        );
    }

    #[test]
    fn outlined_rectangle_matches_opencv_controls() {
        let mut image = Image::new(1200, 600, [245; 3]);
        let mut stroke = Mask::new(1200, 600, 0);
        // cv2.rectangle(..., thickness=2) is drawContours of the four corners.
        draw_contours(
            &mut stroke,
            &[vec![(650, 350), (1000, 350), (1000, 405), (650, 405)]],
            2,
        );
        for (pixel, &painted) in image.data.iter_mut().zip(&stroke.data) {
            if painted != 0 {
                *pixel = [60; 3];
            }
        }
        let (controls, outlines) = outlined_controls(&image);
        assert_eq!(
            controls,
            [
                crate::geometry::Rect::new(651, 351, 348, 53),
                crate::geometry::Rect::new(651, 351, 348, 53),
                crate::geometry::Rect::new(648, 348, 354, 59),
                crate::geometry::Rect::new(648, 348, 354, 59),
            ]
        );
        assert_eq!(
            outlines.data.iter().filter(|value| **value == 255).count(),
            6451
        );
    }

    #[test]
    fn find_contours_recovers_opencv_rectangle_rings() {
        let mut edges = Mask::new(40, 30, 0);
        for x in 10..30 {
            edges.set(x, 8, 255);
            edges.set(x, 20, 255);
        }
        for y in 8..21 {
            edges.set(10, y, 255);
            edges.set(29, y, 255);
        }
        let contours = find_contours(&edges);
        assert!(contours.len() >= 2);
        let areas: Vec<_> = contours
            .iter()
            .map(|contour| contour_area_for_test(contour))
            .collect();
        assert!(areas.iter().any(|area| *area > 100.0));
        assert!(
            bounding_rect_for_test(&contours[0]).is_some_and(|rect| rect.w >= 18 && rect.h >= 10)
        );
    }
}
