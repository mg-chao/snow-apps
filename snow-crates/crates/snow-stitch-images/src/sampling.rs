use rayon::prelude::*;

use crate::{Geometry, StitchAxis, orb::Image};

const FULL_RESOLUTION_EXTENT: u32 = 512;

fn retained_extent(extent: u32) -> u32 {
    if extent <= FULL_RESOLUTION_EXTENT {
        return extent;
    }
    let area = u64::from(FULL_RESOLUTION_EXTENT) * u64::from(extent);
    let root = area.isqrt();
    (root + u64::from(root * root != area)) as u32
}

#[derive(Debug)]
struct AreaBin {
    first: usize,
    weights: Vec<u64>,
}

#[derive(Debug)]
pub(crate) struct SamplingPlan {
    axis: StitchAxis,
    source: u32,
    target: u32,
    bins: Vec<AreaBin>,
}

impl SamplingPlan {
    #[cfg(test)]
    pub(crate) fn full_resolution(geometry: Geometry, axis: StitchAxis) -> Self {
        let source = match axis {
            StitchAxis::Vertical => geometry.width,
            StitchAxis::Horizontal => geometry.height,
        };
        Self {
            axis,
            source,
            target: source,
            bins: Vec::new(),
        }
    }

    pub(crate) fn new(geometry: Geometry, axis: StitchAxis) -> Self {
        let source = match axis {
            StitchAxis::Vertical => geometry.width,
            StitchAxis::Horizontal => geometry.height,
        };
        let target = retained_extent(source);
        let mut bins = Vec::new();
        if target < source {
            // Integer overlap lengths share a denominator, avoiding floating-point
            // boundary errors and giving every output pixel exactly source weight.
            let source = u64::from(source);
            let target = u64::from(target);
            for index in 0..target {
                let start = index * source;
                let end = start + source;
                let first = start / target;
                let weights = (first..end.div_ceil(target))
                    .map(|pixel| end.min((pixel + 1) * target) - start.max(pixel * target))
                    .collect();
                bins.push(AreaBin {
                    first: first as usize,
                    weights,
                });
            }
        }
        Self {
            axis,
            source,
            target,
            bins,
        }
    }

    pub(crate) fn reduced(&self) -> bool {
        self.target < self.source
    }

    pub(crate) fn dimensions(&self, geometry: Geometry) -> (usize, usize) {
        match self.axis {
            StitchAxis::Vertical => (self.target as usize, geometry.height as usize),
            StitchAxis::Horizontal => (geometry.width as usize, self.target as usize),
        }
    }

    pub(crate) fn source_coordinates(&self, x: f32, y: f32) -> (f32, f32) {
        if !self.reduced() {
            return (x, y);
        }
        let map = |value: f32| (value + 0.5) * (self.source as f32 / self.target as f32) - 0.5;
        match self.axis {
            StitchAxis::Vertical => (map(x), y),
            StitchAxis::Horizontal => (x, map(y)),
        }
    }

    pub(crate) fn apply(&self, image: Image) -> Image {
        if !self.reduced() {
            return image;
        }
        let (width, height) = match self.axis {
            StitchAxis::Vertical => (self.target as usize, image.height),
            StitchAxis::Horizontal => (image.width, self.target as usize),
        };
        let mut pixels = vec![0; width * height];
        pixels
            .par_chunks_mut(width)
            .enumerate()
            .for_each(|(y, row)| {
                for (x, value) in row.iter_mut().enumerate() {
                    let (bin, base, stride) = match self.axis {
                        StitchAxis::Vertical => (&self.bins[x], y * image.width, 1),
                        StitchAxis::Horizontal => (&self.bins[y], x, image.width),
                    };
                    let sum: u64 = bin
                        .weights
                        .iter()
                        .enumerate()
                        .map(|(index, weight)| {
                            u64::from(image.pixels[base + (bin.first + index) * stride]) * weight
                        })
                        .sum();
                    *value = ((sum + u64::from(self.source) / 2) / u64::from(self.source)) as u8;
                }
            });
        Image::new(width, height, pixels)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{Frame, PixelFormat, orb};

    fn geometry(width: u32, height: u32) -> Geometry {
        Geometry {
            width,
            height,
            pixel_format: PixelFormat::Gray8,
        }
    }

    #[test]
    fn curve_boundaries_rounding_and_extremes() {
        for (source, expected) in [
            (0, 0),
            (256, 256),
            (511, 511),
            (512, 512),
            (513, 513),
            (514, 513),
            (1024, 725),
            (2048, 1024),
            (4096, 1449),
        ] {
            assert_eq!(retained_extent(source), expected);
        }
        let mut previous = 0;
        for source in (0..10_000).chain([u32::MAX - 1, u32::MAX]) {
            let target = retained_extent(source);
            assert!(target <= source && target >= previous);
            if source > 512 {
                let area = 512 * u64::from(source);
                assert!(u64::from(target).pow(2) >= area);
                assert!(u64::from(target - 1).pow(2) < area);
            }
            previous = target;
        }
    }

    #[test]
    fn area_average_preserves_primary_axis_and_pixel_centers() {
        for axis in [StitchAxis::Vertical, StitchAxis::Horizontal] {
            let geometry = match axis {
                StitchAxis::Vertical => geometry(2048, 3),
                StitchAxis::Horizontal => geometry(3, 2048),
            };
            let plan = SamplingPlan::new(geometry, axis);
            let mut source = Vec::new();
            let mut expected = Vec::new();
            for y in 0..geometry.height {
                for x in 0..geometry.width {
                    let (cross, primary) = match axis {
                        StitchAxis::Vertical => (x, y),
                        StitchAxis::Horizontal => (y, x),
                    };
                    source.push((primary * 40 + cross % 2 * 20) as u8);
                }
            }
            let (width, height) = plan.dimensions(geometry);
            for y in 0..height {
                for x in 0..width {
                    let primary = match axis {
                        StitchAxis::Vertical => y,
                        StitchAxis::Horizontal => x,
                    };
                    expected.push((primary * 40 + 10) as u8);
                }
            }
            let result = plan.apply(Image::new(
                geometry.width as usize,
                geometry.height as usize,
                source,
            ));
            assert_eq!((result.width, result.height), (width, height));
            assert_eq!(result.pixels, expected);
            let mapped = plan.source_coordinates(2.0, 2.0);
            assert_eq!(
                mapped,
                match axis {
                    StitchAxis::Vertical => (4.5, 2.0),
                    StitchAxis::Horizontal => (2.0, 4.5),
                }
            );
        }
    }

    #[test]
    fn fractional_bins_cover_every_source_pixel_and_round_consistently() {
        let plan = SamplingPlan::new(geometry(1024, 1), StitchAxis::Vertical);
        let mut coverage = vec![0_u64; 1024];
        for bin in &plan.bins {
            assert_eq!(bin.weights.iter().sum::<u64>(), 1024);
            for (index, weight) in bin.weights.iter().enumerate() {
                coverage[bin.first + index] += weight;
            }
        }
        assert!(coverage.iter().all(|weight| *weight == 725));
        for constant in [0, 17, 255] {
            assert_eq!(
                plan.apply(Image::new(1024, 1, vec![constant; 1024])).pixels,
                vec![constant; 725]
            );
        }
        let mut source = vec![0; 1024];
        source[0] = 255;
        let result = plan.apply(Image::new(1024, 1, source));
        assert_eq!(result.pixels[0], 181);
        assert!(result.pixels[1..].iter().all(|value| *value == 0));
    }

    #[test]
    fn formats_share_grayscale_sampling_and_small_images_are_unchanged() {
        for width in [127, 1024] {
            let gray: Vec<u8> = (0..width * 3).map(|index| (index % 251) as u8).collect();
            let plan = SamplingPlan::new(geometry(width, 3), StitchAxis::Vertical);
            let expected = plan.apply(Image::new(width as usize, 3, gray.clone()));
            for format in [PixelFormat::Gray8, PixelFormat::Rgb8, PixelFormat::Rgba8] {
                let pixels = gray
                    .iter()
                    .flat_map(|value| match format {
                        PixelFormat::Gray8 => vec![*value],
                        PixelFormat::Rgb8 => vec![*value; 3],
                        _ => vec![*value, *value, *value, 17],
                    })
                    .collect();
                let frame = Frame::new(width, 3, format, pixels).unwrap();
                assert_eq!(plan.apply(orb::grayscale(&frame)), expected);
            }
            if width <= 512 {
                assert_eq!(expected.pixels, gray);
                assert_eq!(plan.source_coordinates(1.25, 2.5), (1.25, 2.5));
            }
        }
    }
}
