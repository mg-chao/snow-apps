//! Backend contract for image-processing primitives.
//!
//! The detector stages are deliberately written against [`Grid`](crate::grid::Grid)
//! values.  This module is the only place where a processing backend is
//! selected: the default implementation is [`pure_rust`], while the
//! `opencv` feature selects [`opencv_impl`].  Keeping this decision here means
//! the semantic pipeline does not grow OpenCV-specific types or conditional
//! branches.

use crate::error::Result;
use crate::geometry::Rect;
use crate::grid::{Grid, Image, Mask};

#[derive(Clone, Copy, Debug)]
pub(crate) enum MorphOp {
    Dilate,
    Open,
    Close,
    Gradient,
}

#[derive(Clone, Copy, Debug)]
pub(crate) enum KernelShape {
    Rect,
    Ellipse,
}

/// One foreground component and its OpenCV-compatible statistics.
#[derive(Clone, Copy, Debug)]
pub(crate) struct Component {
    pub(crate) rect: Rect,
    pub(crate) area: i32,
    pub(crate) id: i32,
}

/// Operations required by the shared detection stages.
pub(crate) trait Backend {
    const NAME: &'static str;

    fn morphology(
        input: &Mask,
        op: MorphOp,
        kernel_width: i32,
        kernel_height: i32,
        shape: KernelShape,
    ) -> Result<Mask>;

    fn connected_components(input: &Mask) -> Result<(Grid<i32>, Vec<Component>)>;

    fn median_blur_image(input: &Image, kernel: i32) -> Result<Image>;

    fn blur_row(input: &[f32], kernel: i32) -> Result<Vec<f32>>;

    fn median_row(input: &[u8], kernel: i32) -> Result<Vec<u8>>;

    fn outlined_controls(input: &Image) -> Result<(Vec<Rect>, Mask)>;
}

#[cfg(not(feature = "opencv"))]
mod pure_rust;

#[cfg(feature = "opencv")]
mod opencv_impl;

#[cfg(feature = "opencv")]
type ActiveBackend = opencv_impl::OpenCvBackend;
#[cfg(not(feature = "opencv"))]
type ActiveBackend = pure_rust::PureRustBackend;

pub(crate) fn name() -> &'static str {
    ActiveBackend::NAME
}

pub(crate) fn morphology(
    input: &Mask,
    op: MorphOp,
    kernel_width: i32,
    kernel_height: i32,
    shape: KernelShape,
) -> Result<Mask> {
    ActiveBackend::morphology(input, op, kernel_width, kernel_height, shape)
}

pub(crate) fn connected_components(input: &Mask) -> Result<(Grid<i32>, Vec<Component>)> {
    ActiveBackend::connected_components(input)
}

pub(crate) fn median_blur_image(input: &Image, kernel: i32) -> Result<Image> {
    ActiveBackend::median_blur_image(input, kernel)
}

pub(crate) fn blur_row(input: &[f32], kernel: i32) -> Result<Vec<f32>> {
    ActiveBackend::blur_row(input, kernel)
}

pub(crate) fn median_row(input: &[u8], kernel: i32) -> Result<Vec<u8>> {
    ActiveBackend::median_row(input, kernel)
}

pub(crate) fn outlined_controls(input: &Image) -> Result<(Vec<Rect>, Mask)> {
    ActiveBackend::outlined_controls(input)
}
