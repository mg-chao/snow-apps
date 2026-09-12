//! OpenCV fixture checks for the pure-Rust primitives.

use super::canny::{canny_edges, grayscale};
use super::contours::{
    bounding_rect_for_test, contour_area_for_test, find_contours, outlined_controls,
};
use super::draw::draw_contours;
use super::median::median_blur_image;
use super::morph::{blur_row, connected_components, ellipse_kernel, median_row, morphology};
use super::{KernelShape, MorphOp};
use crate::geometry::Rect;
use crate::grid::{Image, Mask};

fn fixtures() -> serde_json::Value {
    serde_json::from_str(include_str!("opencv_fixtures.json")).expect("opencv fixtures")
}

fn fnv64_hex(data: &[u8]) -> String {
    let mut hash = 0xcbf2_9ce4_8422_2325u64;
    for &byte in data {
        hash ^= u64::from(byte);
        hash = hash.wrapping_mul(0x0100_0000_01b3);
    }
    format!("{hash:016x}")
}

fn mask_digest(mask: &Mask) -> (i64, usize, String) {
    let sum = mask.data.iter().map(|value| i64::from(*value)).sum();
    let nonzero = mask.data.iter().filter(|value| **value != 0).count();
    (sum, nonzero, fnv64_hex(&mask.data))
}

fn image_digest(image: &Image) -> (i64, usize, String) {
    let bytes: Vec<u8> = image.data.iter().flatten().copied().collect();
    let sum = bytes.iter().map(|value| i64::from(*value)).sum();
    let nonzero = bytes.iter().filter(|value| **value != 0).count();
    (sum, nonzero, fnv64_hex(&bytes))
}

fn i32_digest(data: &[i32]) -> (i64, usize, String) {
    let bytes: Vec<u8> = data.iter().flat_map(|value| value.to_le_bytes()).collect();
    let sum = data.iter().map(|value| i64::from(*value)).sum();
    let nonzero = data.iter().filter(|value| **value != 0).count();
    (sum, nonzero, fnv64_hex(&bytes))
}

fn assert_digest(name: &str, sum: i64, nonzero: usize, fnv: &str, expected: &serde_json::Value) {
    assert_eq!(sum, expected["sum"].as_i64().unwrap(), "{name} sum");
    assert_eq!(
        nonzero,
        expected["nonzero"].as_u64().unwrap() as usize,
        "{name} nonzero"
    );
    assert_eq!(fnv, expected["fnv64"].as_str().unwrap(), "{name} fnv64");
}

fn morph_mask() -> Mask {
    let mut input = Mask::new(11, 9, 0);
    for y in 2..7 {
        for x in 3..8 {
            input.set(x, y, 255);
        }
    }
    input.set(5, 4, 0);
    input
}

fn u_mask() -> Mask {
    let mut input = Mask::new(24, 20, 0);
    for y in 2..18 {
        for x in 2..6 {
            input.set(x, y, 255);
        }
        for x in 14..18 {
            input.set(x, y, 255);
        }
    }
    for y in 14..18 {
        for x in 2..18 {
            input.set(x, y, 255);
        }
    }
    for y in 6..8 {
        for x in 10..12 {
            input.set(x, y, 255);
        }
    }
    input
}

fn gray_image(width: i32, height: i32) -> Image {
    Image {
        w: width,
        h: height,
        data: (0..height)
            .flat_map(|y| {
                (0..width).map(move |x| {
                    [
                        ((x * 13 + y * 7) % 256) as u8,
                        ((x * 3 + y * 17) % 256) as u8,
                        ((x * 29 + y * 5) % 256) as u8,
                    ]
                })
            })
            .collect(),
    }
}

fn line_mask() -> Mask {
    let mut lines = Mask::new(200, 40, 0);
    for (y, x0) in [(8, 10), (18, 12), (28, 8)] {
        for row in y..y + 6 {
            for x in x0..x0 + 120 {
                lines.set(x, row, 255);
            }
        }
        for x in x0 + 40..x0 + 48 {
            lines.set(x, y + 2, 0);
        }
    }
    lines
}

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
fn opencv_fixtures_match_pure_rust_primitives() {
    let cases = &fixtures()["cases"];

    for (name, expected) in cases["ellipse"].as_object().unwrap() {
        let (width, height) = name.split_once('x').unwrap();
        let width: i32 = width.parse().unwrap();
        let height: i32 = height.parse().unwrap();
        let expected: Vec<u8> = expected
            .as_array()
            .unwrap()
            .iter()
            .map(|value| value.as_u64().unwrap() as u8)
            .collect();
        assert_eq!(
            ellipse_kernel(width, height).data,
            expected,
            "ellipse {name}"
        );
    }

    let input = morph_mask();
    let morph_cases = [
        (
            "dilate3",
            morphology(&input, MorphOp::Dilate, 3, 3, KernelShape::Rect),
        ),
        (
            "open3",
            morphology(&input, MorphOp::Open, 3, 3, KernelShape::Rect),
        ),
        (
            "close3",
            morphology(&input, MorphOp::Close, 3, 3, KernelShape::Rect),
        ),
        (
            "grad5",
            morphology(&input, MorphOp::Gradient, 5, 5, KernelShape::Rect),
        ),
        (
            "dilate4",
            morphology(&input, MorphOp::Dilate, 4, 4, KernelShape::Rect),
        ),
        (
            "close17x9",
            morphology(&input, MorphOp::Close, 17, 9, KernelShape::Rect),
        ),
        (
            "ellipse_close3",
            morphology(&input, MorphOp::Close, 3, 3, KernelShape::Ellipse),
        ),
        (
            "ellipse_grad5",
            morphology(&input, MorphOp::Gradient, 5, 5, KernelShape::Ellipse),
        ),
    ];
    for (name, output) in morph_cases {
        let (sum, nonzero, fnv) = mask_digest(&output);
        assert_digest(name, sum, nonzero, &fnv, &cases["morph"][name]);
    }

    let lines = line_mask();
    let line_cases = [
        (
            "open80x1",
            morphology(&lines, MorphOp::Open, 80, 1, KernelShape::Rect),
        ),
        (
            "open1x20",
            morphology(&lines, MorphOp::Open, 1, 20, KernelShape::Rect),
        ),
        (
            "close11",
            morphology(&lines, MorphOp::Close, 11, 11, KernelShape::Rect),
        ),
        (
            "ellipse3x11",
            morphology(&lines, MorphOp::Close, 3, 11, KernelShape::Ellipse),
        ),
    ];
    for (name, output) in line_cases {
        let (sum, nonzero, fnv) = mask_digest(&output);
        assert_digest(name, sum, nonzero, &fnv, &cases["morph_lines"][name]);
    }

    let (labels, components) = connected_components(&{
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
        input
    });
    let expected_labels: Vec<i32> = cases["cc_simple"]["labels"]
        .as_array()
        .unwrap()
        .iter()
        .map(|value| value.as_i64().unwrap() as i32)
        .collect();
    assert_eq!(labels.data, expected_labels);
    assert_eq!(
        components.len(),
        cases["cc_simple"]["n"].as_u64().unwrap() as usize - 1
    );

    let (labels, components) = connected_components(&u_mask());
    let (sum, nonzero, fnv) = i32_digest(&labels.data);
    assert_digest("cc_u", sum, nonzero, &fnv, &cases["cc_u"]["labels"]);
    let expected_stats = cases["cc_u"]["stats"].as_array().unwrap();
    assert_eq!(components.len(), expected_stats.len());
    for (component, expected) in components.iter().zip(expected_stats) {
        let row = expected.as_array().unwrap();
        assert_eq!(component.rect.x, row[0].as_i64().unwrap() as i32);
        assert_eq!(component.rect.y, row[1].as_i64().unwrap() as i32);
        assert_eq!(component.rect.w, row[2].as_i64().unwrap() as i32);
        assert_eq!(component.rect.h, row[3].as_i64().unwrap() as i32);
        assert_eq!(component.area, row[4].as_i64().unwrap() as i32);
    }

    let mut checker = Mask::new(16, 16, 0);
    for y in 0..16 {
        for x in 0..16 {
            if (x + y) % 2 == 1 {
                checker.set(x, y, 255);
            }
        }
    }
    let (labels, components) = connected_components(&checker);
    let expected_labels: Vec<i32> = cases["cc_checker"]["labels"]
        .as_array()
        .unwrap()
        .iter()
        .map(|value| value.as_i64().unwrap() as i32)
        .collect();
    assert_eq!(labels.data, expected_labels);
    assert_eq!(components[0].area, 128);

    let row = [0.0, 10.0, 20.0, 30.0, 40.0, 50.0, 60.0];
    for (name, kernel) in [("k3", 3), ("k4", 4), ("k5", 5)] {
        let got = blur_row(&row, kernel);
        let expected: Vec<f32> = cases["blur"][name]
            .as_array()
            .unwrap()
            .iter()
            .map(|value| value.as_f64().unwrap() as f32)
            .collect();
        for (got, expected) in got.iter().zip(&expected) {
            assert!((got - expected).abs() < 1e-5, "{name} {got} vs {expected}");
        }
    }
    assert_eq!(
        median_row(&[0, 0, 1, 1, 1, 0, 0, 1, 0, 0], 5),
        cases["median_row"]
            .as_array()
            .unwrap()
            .iter()
            .map(|value| value.as_u64().unwrap() as u8)
            .collect::<Vec<_>>()
    );

    let mut image = Image::new(7, 5, [0; 3]);
    for y in 0..5 {
        for x in 0..7 {
            let base = (y * 10 + x) as u8;
            image.set(x, y, [base, base + 1, base + 2]);
        }
    }
    let blurred = median_blur_image(&image, 3);
    let (sum, nonzero, fnv) = image_digest(&blurred);
    assert_digest("median3", sum, nonzero, &fnv, &cases["median3"]["digest"]);

    let noisy = gray_image(80, 64);
    let (sum, nonzero, fnv) = image_digest(&median_blur_image(&noisy, 31));
    assert_digest("median31", sum, nonzero, &fnv, &cases["median31"]);
    let (sum, nonzero, fnv) = image_digest(&median_blur_image(&noisy, 5));
    assert_digest("median5", sum, nonzero, &fnv, &cases["median5"]);

    let color = gray_image(64, 48);
    let gray: Vec<u8> = color.data.iter().copied().map(grayscale).collect();
    let sum = gray.iter().map(|value| i64::from(*value)).sum();
    let nonzero = gray.iter().filter(|value| **value != 0).count();
    assert_digest("gray", sum, nonzero, &fnv64_hex(&gray), &cases["gray"]);
    let (sum, nonzero, fnv) = mask_digest(&canny_edges(&color));
    assert_digest("canny_color", sum, nonzero, &fnv, &cases["canny_color"]);

    let mut rows = vec![vec![0u8; 8]; 5];
    for row in &mut rows {
        row[4..].fill(200);
    }
    let refs: Vec<&[u8]> = rows.iter().map(|row| row.as_slice()).collect();
    let edges = canny_edges(&image_from_gray(&refs));
    let expected: Vec<u8> = cases["canny_step"]
        .as_array()
        .unwrap()
        .iter()
        .map(|value| value.as_u64().unwrap() as u8)
        .collect();
    assert_eq!(edges.data, expected, "canny_step");

    let mut wide = vec![vec![0u8; 16]; 12];
    for row in &mut wide {
        row[8..].fill(255);
    }
    let refs: Vec<&[u8]> = wide.iter().map(|row| row.as_slice()).collect();
    let edges = canny_edges(&image_from_gray(&refs));
    let expected: Vec<u8> = cases["canny_wide"]
        .as_array()
        .unwrap()
        .iter()
        .map(|value| value.as_u64().unwrap() as u8)
        .collect();
    assert_eq!(edges.data, expected, "canny_wide");

    let mut mask = Mask::new(30, 20, 0);
    draw_contours(&mut mask, &[vec![(5, 5), (24, 5), (24, 14), (5, 14)]], 3);
    let (sum, nonzero, fnv) = mask_digest(&mask);
    assert_eq!(
        nonzero,
        cases["draw_rect"]["sum"].as_u64().unwrap() as usize
    );
    assert_digest(
        "draw_rect",
        sum,
        nonzero,
        &fnv,
        &cases["draw_rect"]["digest"],
    );

    let mut horizontal = Mask::new(21, 11, 0);
    draw_contours(&mut horizontal, &[vec![(2, 5), (18, 5)]], 3);
    assert_eq!(
        horizontal
            .data
            .iter()
            .filter(|value| **value == 255)
            .count(),
        cases["draw_h"].as_u64().unwrap() as usize
    );

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
    let expected = cases["contours_rect"]["points"].as_array().unwrap();
    assert_eq!(contours.len(), expected.len());
    for (contour, expected) in contours.iter().zip(expected) {
        let expected: Vec<(i32, i32)> = expected
            .as_array()
            .unwrap()
            .iter()
            .map(|point| {
                let point = point.as_array().unwrap();
                (
                    point[0].as_i64().unwrap() as i32,
                    point[1].as_i64().unwrap() as i32,
                )
            })
            .collect();
        assert_eq!(contour, &expected);
    }
    let expected_areas = cases["contours_rect"]["areas"].as_array().unwrap();
    for (contour, expected) in contours.iter().zip(expected_areas) {
        let area = contour_area_for_test(contour);
        assert!((area - expected.as_f64().unwrap()).abs() < 1e-6);
    }
    let expected_rects = cases["contours_rect"]["rects"].as_array().unwrap();
    for (contour, expected) in contours.iter().zip(expected_rects) {
        let rect = bounding_rect_for_test(contour).unwrap();
        let expected = expected.as_array().unwrap();
        assert_eq!(
            [rect.x, rect.y, rect.w, rect.h],
            [
                expected[0].as_i64().unwrap() as i32,
                expected[1].as_i64().unwrap() as i32,
                expected[2].as_i64().unwrap() as i32,
                expected[3].as_i64().unwrap() as i32
            ]
        );
    }

    let mut image = Image::new(1200, 600, [245; 3]);
    let mut stroke = Mask::new(1200, 600, 0);
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
    let (sum, nonzero, fnv) = mask_digest(&canny_edges(&image));
    assert_digest(
        "outlined_edges",
        sum,
        nonzero,
        &fnv,
        &cases["outlined"]["edge_digest"],
    );
    let contours = find_contours(&canny_edges(&image));
    assert_eq!(
        contours.len(),
        cases["outlined"]["n_contours"].as_u64().unwrap() as usize
    );
    let expected_first: Vec<(i32, i32)> = cases["outlined"]["first_contour"]
        .as_array()
        .unwrap()
        .iter()
        .map(|point| {
            let point = point.as_array().unwrap();
            (
                point[0].as_i64().unwrap() as i32,
                point[1].as_i64().unwrap() as i32,
            )
        })
        .collect();
    assert_eq!(contours[0], expected_first);
    let (controls, outlines) = outlined_controls(&image);
    let (sum, nonzero, fnv) = mask_digest(&outlines);
    assert_digest(
        "outlined",
        sum,
        nonzero,
        &fnv,
        &cases["outlined"]["outline_digest"],
    );
    let expected = cases["outlined"]["controls"].as_array().unwrap();
    assert_eq!(controls.len(), expected.len());
    for (rect, expected) in controls.iter().zip(expected) {
        let expected = expected.as_array().unwrap();
        assert_eq!(
            *rect,
            Rect::new(
                expected[0].as_i64().unwrap() as i32,
                expected[1].as_i64().unwrap() as i32,
                expected[2].as_i64().unwrap() as i32,
                expected[3].as_i64().unwrap() as i32
            )
        );
    }

    for (name, width, height, kernel) in [
        ("median31_w170", 170, 64, 31),
        ("median31_w171", 171, 48, 31),
        ("median31_w340", 340, 40, 31),
        ("median9", 80, 80, 9),
        ("median31_h5", 200, 5, 31),
        ("median5_h1", 64, 1, 5),
        ("median5_w1", 1, 64, 5),
        ("median31_speckle", 128, 96, 31),
    ] {
        let (sum, nonzero, fnv) =
            image_digest(&median_blur_image(&gray_image(width, height), kernel));
        assert_digest(name, sum, nonzero, &fnv, &cases[name]);
    }
    let (sum, nonzero, fnv) = mask_digest(&canny_edges(&gray_image(128, 96)));
    assert_digest("canny_speckle", sum, nonzero, &fnv, &cases["canny_speckle"]);

    let mut blobs = Mask::new(80, 80, 0);
    for (x, y, s) in [
        (5, 5, 8),
        (30, 8, 6),
        (50, 40, 12),
        (10, 50, 7),
        (60, 10, 5),
    ] {
        for yy in y..y + s {
            for xx in x..x + s {
                blobs.set(xx, yy, 255);
            }
        }
    }
    for x in 5..70 {
        for y in 20..23 {
            blobs.set(x, y, 255);
        }
    }
    let (labels, components) = connected_components(&blobs);
    let (sum, nonzero, fnv) = i32_digest(&labels.data);
    assert_digest("cc_blobs", sum, nonzero, &fnv, &cases["cc_blobs"]["labels"]);
    let expected_stats = cases["cc_blobs"]["stats"].as_array().unwrap();
    assert_eq!(components.len(), expected_stats.len());
    for (component, expected) in components.iter().zip(expected_stats) {
        let row = expected.as_array().unwrap();
        assert_eq!(component.rect.x, row[0].as_i64().unwrap() as i32);
        assert_eq!(component.rect.y, row[1].as_i64().unwrap() as i32);
        assert_eq!(component.rect.w, row[2].as_i64().unwrap() as i32);
        assert_eq!(component.rect.h, row[3].as_i64().unwrap() as i32);
        assert_eq!(component.area, row[4].as_i64().unwrap() as i32);
    }

    let mut hole = Mask::new(40, 40, 0);
    for y in 5..35 {
        for x in 5..35 {
            hole.set(x, y, 255);
        }
    }
    for y in 12..28 {
        for x in 12..28 {
            hole.set(x, y, 0);
        }
    }
    let contours = find_contours(&hole);
    let expected = cases["contours_hole"]["points"].as_array().unwrap();
    assert_eq!(contours.len(), expected.len());
    for (contour, expected) in contours.iter().zip(expected) {
        let expected: Vec<(i32, i32)> = expected
            .as_array()
            .unwrap()
            .iter()
            .map(|point| {
                let point = point.as_array().unwrap();
                (
                    point[0].as_i64().unwrap() as i32,
                    point[1].as_i64().unwrap() as i32,
                )
            })
            .collect();
        assert_eq!(contour, &expected);
    }

    let mut diag = Mask::new(50, 40, 0);
    draw_contours(&mut diag, &[vec![(2, 2), (47, 37)]], 3);
    let (sum, nonzero, fnv) = mask_digest(&diag);
    assert_digest("draw_diag", sum, nonzero, &fnv, &cases["draw_diag"]);
    let mut edge = Mask::new(20, 20, 0);
    draw_contours(&mut edge, &[vec![(-2, 10), (25, 10)]], 3);
    let (sum, nonzero, fnv) = mask_digest(&edge);
    assert_digest("draw_clip", sum, nonzero, &fnv, &cases["draw_clip"]);
}
