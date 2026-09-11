/// Reuses the coordinate map for the recorder's fixed output dimensions.
/// Integer mapping preserves the existing top-left nearest-neighbor sampling.
#[derive(Default)]
pub(crate) struct RgbaResizer {
    dimensions: Option<((u32, u32), (u32, u32))>,
    columns: Vec<usize>,
    rows: Vec<usize>,
}

impl RgbaResizer {
    pub(crate) fn resize_into(
        &mut self,
        source: &[u8],
        source_size: (u32, u32),
        output_size: (u32, u32),
        output: &mut Vec<u8>,
    ) {
        assert_eq!(
            source.len(),
            source_size.0 as usize * source_size.1 as usize * 4
        );
        output.resize(output_size.0 as usize * output_size.1 as usize * 4, 0);
        if source_size == output_size {
            output.copy_from_slice(source);
            return;
        }
        if output.is_empty() {
            return;
        }
        assert!(source_size.0 > 0 && source_size.1 > 0);
        if self.dimensions != Some((source_size, output_size)) {
            self.columns = (0..output_size.0)
                .map(|x| {
                    (u64::from(x) * u64::from(source_size.0) / u64::from(output_size.0)) as usize
                })
                .collect();
            self.rows = (0..output_size.1)
                .map(|y| {
                    (u64::from(y) * u64::from(source_size.1) / u64::from(output_size.1)) as usize
                })
                .collect();
            self.dimensions = Some((source_size, output_size));
        }
        let (pixels, _) = source.as_chunks::<4>();
        let (output_pixels, _) = output.as_chunks_mut::<4>();
        for (row, &source_y) in output_pixels
            .chunks_exact_mut(output_size.0 as usize)
            .zip(&self.rows)
        {
            let source_start = source_y * source_size.0 as usize;
            let source_row = &pixels[source_start..source_start + source_size.0 as usize];
            for (pixel, &source_x) in row.iter_mut().zip(&self.columns) {
                *pixel = source_row[source_x];
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cached_resize_preserves_sampling_and_alpha_across_dimension_changes() {
        let mut resizer = RgbaResizer::default();
        let mut output = Vec::new();
        for source_size in [(1, 1), (7, 5), (16, 12)] {
            let source: Vec<u8> = (0..source_size.0 * source_size.1 * 4)
                .map(|value| value as u8)
                .collect();
            for size in [source_size, (1, 1), (3, 2), (19, 13), (3, 2)] {
                for _ in 0..2 {
                    resizer.resize_into(&source, source_size, size, &mut output);
                    assert_eq!(output.len(), size.0 as usize * size.1 as usize * 4);
                    for y in 0..size.1 {
                        for x in 0..size.0 {
                            let from = ((y * source_size.1 / size.1) * source_size.0
                                + x * source_size.0 / size.0)
                                as usize
                                * 4;
                            let to = (y * size.0 + x) as usize * 4;
                            assert_eq!(&output[to..to + 4], &source[from..from + 4]);
                        }
                    }
                }
            }
        }
    }
}
