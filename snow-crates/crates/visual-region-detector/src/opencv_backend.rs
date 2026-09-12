//! Optional OpenCV adapter.
//!
//! Enabling the crate's `opencv` feature selects OpenCV for the primitive
//! operations used by the shared detector. This module additionally provides
//! explicit conversion helpers for applications that already own OpenCV
//! `Mat` values. The core API still uses [`BgrImage`](crate::BgrImage), so
//! callers can switch features without changing detection-stage code.

use crate::error::{Error, Result};
use crate::{BgrImage, Region};
use opencv::{
    core::{self, Mat, Vec3b},
    prelude::*,
};

/// Copy a two-dimensional `CV_8UC3` BGR Mat into the portable image type.
pub fn from_mat(mat: &Mat) -> Result<BgrImage> {
    if mat.empty() || mat.dims() != 2 || mat.typ() != core::CV_8UC3 {
        return Err(Error::InvalidImage(
            "expected a non-empty H x W x 3 uint8 BGR image".into(),
        ));
    }
    let contiguous = mat.try_clone()?;
    let bytes = contiguous.data_bytes()?.to_vec();
    BgrImage::from_bgr_bytes(contiguous.cols() as u32, contiguous.rows() as u32, bytes)
}

/// Copy a portable BGR image into a newly allocated two-dimensional Mat.
pub fn to_mat(image: &BgrImage) -> Result<Mat> {
    let pixels: Vec<Vec3b> = image.pixels().iter().copied().map(Vec3b::from).collect();
    Ok(Mat::from_slice(&pixels)?
        .reshape(0, image.height() as i32)?
        .try_clone()?)
}

/// Detect regions from an OpenCV Mat using the selected backend.
pub fn detect_regions(mat: &Mat) -> Result<Vec<Region>> {
    crate::detect_regions(&from_mat(mat)?)
}

/// Draw a preview and return it as a newly allocated OpenCV Mat.
pub fn draw_regions(mat: &Mat, regions: &[Region], thickness: i32) -> Result<Mat> {
    let image = from_mat(mat)?;
    to_mat(&crate::draw_regions(&image, regions, thickness)?)
}

/// Serialize regions while retaining the OpenCV Mat input boundary.
pub fn regions_to_jsonable(
    source: &str,
    mat: &Mat,
    regions: &[Region],
) -> Result<serde_json::Value> {
    let image = from_mat(mat)?;
    Ok(crate::regions_to_jsonable(source, &image, regions))
}

/// Export crops from an OpenCV Mat using the portable lossless writer.
pub fn write_crops(
    mat: &Mat,
    regions: &[Region],
    directory: &std::path::Path,
) -> Result<Vec<serde_json::Value>> {
    let image = from_mat(mat)?;
    crate::write_crops(&image, regions, directory)
}

/// Return the linked OpenCV runtime version.
pub fn version() -> Result<String> {
    Ok(core::get_version_string()?)
}
