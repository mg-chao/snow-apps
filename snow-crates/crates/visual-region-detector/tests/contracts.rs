use visual_region_detector::{BgrImage, Rect, detect_regions, draw_regions, iou};

#[test]
fn rejects_invalid_images() {
    assert!(BgrImage::from_bgr_bytes(0, 10, Vec::new()).is_err());
    assert!(BgrImage::from_bgr_bytes(10, 0, Vec::new()).is_err());
    assert!(BgrImage::from_bgr_bytes(10, 10, vec![0; 10]).is_err());
    assert!(BgrImage::from_bgr_pixels(10, 10, vec![[0; 3]; 9]).is_err());
}

#[test]
fn blank_and_tiny_inputs_have_no_regions() -> Result<(), Box<dyn std::error::Error>> {
    for (width, height) in [(320, 240), (1, 1), (1, 10), (10, 1), (3, 3)] {
        for value in [0, 30, 128, 255] {
            let image = BgrImage::solid(width, height, [value; 3])?;
            assert!(
                detect_regions(&image)?.is_empty(),
                "{width} x {height}, {value}"
            );
        }
    }
    Ok(())
}

#[test]
fn caret_is_preserved_and_source_is_unchanged() -> Result<(), Box<dyn std::error::Error>> {
    let width = 800u32;
    let height = 400u32;
    let mut pixels = vec![[245; 3]; (width * height) as usize];
    for y in 200..233 {
        for x in 400..402 {
            pixels[(y * width + x) as usize] = [0; 3];
        }
    }
    let image = BgrImage::from_bgr_pixels(width, height, pixels)?;
    let original = image.to_bgr_bytes();
    let regions = detect_regions(&image)?;
    assert!(
        regions
            .iter()
            .any(|region| iou(region.rect, Rect::new(400, 200, 2, 33)) > 0.8)
    );
    let preview = draw_regions(&image, &regions, 2)?;
    assert_eq!(image.to_bgr_bytes(), original);
    assert_ne!(preview.to_bgr_bytes(), original);
    Ok(())
}

#[cfg(feature = "opencv")]
#[test]
fn opencv_adapter_round_trips_bgr_pixels() -> Result<(), Box<dyn std::error::Error>> {
    use opencv::{core, prelude::*};
    use visual_region_detector::opencv_backend;

    let mat = core::Mat::new_rows_cols_with_default(
        3,
        4,
        core::CV_8UC3,
        core::Scalar::new(7., 19., 31., 0.),
    )?;
    let image = opencv_backend::from_mat(&mat)?;
    assert_eq!((image.width(), image.height()), (4, 3));
    assert!(image.pixels().iter().all(|pixel| *pixel == [7, 19, 31]));

    let round_trip = opencv_backend::to_mat(&image)?;
    assert_eq!(round_trip.rows(), 3);
    assert_eq!(round_trip.cols(), 4);
    assert_eq!(round_trip.data_bytes()?, mat.data_bytes()?);
    Ok(())
}
