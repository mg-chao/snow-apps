//! OpenCV implementation of the backend contract.

use super::{Backend, Component, KernelShape, MorphOp};
use crate::error::Result;
use crate::geometry::Rect;
use crate::grid::{Grid, Image, Mask};
use opencv::{
    core::{self, Mat, Point, Scalar, Size, Vec3b, Vector},
    imgproc,
    prelude::*,
};

pub(crate) struct OpenCvBackend;

impl Backend for OpenCvBackend {
    const NAME: &'static str = "opencv";

    fn morphology(
        input: &Mask,
        op: MorphOp,
        kernel_width: i32,
        kernel_height: i32,
        shape: KernelShape,
    ) -> Result<Mask> {
        let mut output = Mat::default();
        let op = match op {
            MorphOp::Dilate => imgproc::MORPH_DILATE,
            MorphOp::Open => imgproc::MORPH_OPEN,
            MorphOp::Close => imgproc::MORPH_CLOSE,
            MorphOp::Gradient => imgproc::MORPH_GRADIENT,
        };
        let shape = match shape {
            KernelShape::Rect => imgproc::MORPH_RECT,
            KernelShape::Ellipse => imgproc::MORPH_ELLIPSE,
        };
        let kernel = imgproc::get_structuring_element(
            shape,
            Size::new(kernel_width, kernel_height),
            Point::new(-1, -1),
        )?;
        imgproc::morphology_ex(
            &mask_mat(input)?,
            &mut output,
            op,
            &kernel,
            Point::new(-1, -1),
            1,
            core::BORDER_CONSTANT,
            imgproc::morphology_default_border_value()?,
        )?;
        mask_from_mat(&output)
    }

    fn connected_components(input: &Mask) -> Result<(Grid<i32>, Vec<Component>)> {
        let (mut labels, mut stats, mut centers) = (Mat::default(), Mat::default(), Mat::default());
        let count = imgproc::connected_components_with_stats(
            &mask_mat(input)?,
            &mut labels,
            &mut stats,
            &mut centers,
            8,
            core::CV_32S,
        )?;
        let mut components = Vec::new();
        for id in 1..count {
            let row = stats.at_row::<i32>(id)?;
            components.push(Component {
                rect: Rect::new(row[0], row[1], row[2], row[3]),
                area: row[4],
                id,
            });
        }
        Ok((grid_i32_from_mat(&labels)?, components))
    }

    fn median_blur_image(input: &Image, kernel: i32) -> Result<Image> {
        let mut output = Mat::default();
        imgproc::median_blur(&image_mat(input)?, &mut output, kernel)?;
        image_from_mat(&output)
    }

    fn blur_row(input: &[f32], kernel: i32) -> Result<Vec<f32>> {
        let mut output = Mat::default();
        let source = Mat::from_slice(input)?.reshape(1, 1)?.try_clone()?;
        imgproc::blur(
            &source,
            &mut output,
            Size::new(kernel, 1),
            Point::new(-1, -1),
            core::BORDER_DEFAULT,
        )?;
        Ok(output.data_typed::<f32>()?.to_vec())
    }

    fn median_row(input: &[u8], kernel: i32) -> Result<Vec<u8>> {
        let mut output = Mat::default();
        let source = Mat::from_slice(input)?.reshape(1, 1)?.try_clone()?;
        imgproc::median_blur(&source, &mut output, kernel)?;
        Ok(output.data_typed::<u8>()?.to_vec())
    }

    fn outlined_controls(input: &Image) -> Result<(Vec<Rect>, Mask)> {
        let (mut gray, mut edges) = (Mat::default(), Mat::default());
        imgproc::cvt_color_def(&image_mat(input)?, &mut gray, imgproc::COLOR_BGR2GRAY)?;
        imgproc::canny(&gray, &mut edges, 20., 60., 3, false)?;
        let mut contours: Vector<Vector<Point>> = Vector::new();
        imgproc::find_contours_def(
            &edges,
            &mut contours,
            imgproc::RETR_LIST,
            imgproc::CHAIN_APPROX_SIMPLE,
        )?;
        let mut outlines = mask_mat(&Mask::new(input.w, input.h, 0))?;
        let mut controls = Vec::new();
        for contour in contours {
            let rect = imgproc::bounding_rect(&contour)?;
            let aspect = f64::from(rect.width) / f64::from(rect.height.max(1));
            if rect.height < 20
                || f64::from(rect.height) > 0.15 * f64::from(input.h)
                || rect.width < 50
                || f64::from(rect.width) > 0.6 * f64::from(input.w)
                || aspect <= 1.5
                || aspect >= 15.
            {
                continue;
            }
            if imgproc::contour_area(&contour, false)? < 0.88 * f64::from(rect.width * rect.height)
            {
                continue;
            }
            controls.push(Rect::new(rect.x, rect.y, rect.width, rect.height));
            let mut one: Vector<Vector<Point>> = Vector::new();
            one.push(contour);
            imgproc::draw_contours(
                &mut outlines,
                &one,
                -1,
                Scalar::all(255.),
                3,
                imgproc::LINE_8,
                &core::no_array(),
                i32::MAX,
                Point::new(0, 0),
            )?;
        }
        Ok((controls, mask_from_mat(&outlines)?))
    }
}

fn mask_mat(mask: &Mask) -> Result<Mat> {
    Ok(Mat::from_slice(&mask.data)?
        .reshape(1, mask.h)?
        .try_clone()?)
}

fn image_mat(image: &Image) -> Result<Mat> {
    let data: Vec<Vec3b> = image.data.iter().map(|pixel| Vec3b::from(*pixel)).collect();
    Ok(Mat::from_slice(&data)?.reshape(0, image.h)?.try_clone()?)
}

fn mask_from_mat(mat: &Mat) -> Result<Mask> {
    let mat = mat.try_clone()?;
    Ok(Mask {
        w: mat.cols(),
        h: mat.rows(),
        data: mat.data_typed::<u8>()?.to_vec(),
    })
}

fn grid_i32_from_mat(mat: &Mat) -> Result<Grid<i32>> {
    let mat = mat.try_clone()?;
    Ok(Grid {
        w: mat.cols(),
        h: mat.rows(),
        data: mat.data_typed::<i32>()?.to_vec(),
    })
}

fn image_from_mat(mat: &Mat) -> Result<Image> {
    let mat = mat.try_clone()?;
    let data = mat
        .data_typed::<Vec3b>()?
        .iter()
        .map(|pixel| [pixel[0], pixel[1], pixel[2]])
        .collect();
    Ok(Image {
        w: mat.cols(),
        h: mat.rows(),
        data,
    })
}
