use crate::{Frame, StitchAxis, StitchError};

pub fn band_size(viewport_extent: u32, shift: u32) -> Result<u32, StitchError> {
    band_height(viewport_extent, shift)
}

pub fn band_height(viewport_height: u32, shift: u32) -> Result<u32, StitchError> {
    let quarter_plus_shift =
        (viewport_height / 4)
            .checked_add(shift)
            .ok_or(StitchError::Arithmetic {
                operation: "calculating repaint band",
            })?;
    let band = (viewport_height / 2).max(quarter_plus_shift);
    if band > viewport_height {
        return Err(StitchError::CheckedCrop {
            x: 0,
            y: 0,
            width: 1,
            height: band,
            image_width: 1,
            image_height: viewport_height,
        });
    }
    Ok(band)
}

fn validate_pair(old: &Frame, incoming: &Frame) -> Result<(), StitchError> {
    if old.width() != incoming.width() || old.pixel_format() != incoming.pixel_format() {
        return Err(StitchError::InvalidFrame {
            message: format!(
                "cannot compose {} with {}",
                old.geometry(),
                incoming.geometry()
            ),
        });
    }
    Ok(())
}

pub fn append_and_repaint(
    old: &Frame,
    incoming: &Frame,
    growth: u32,
) -> Result<Frame, StitchError> {
    validate_pair(old, incoming)?;
    let viewport_height = incoming.height();
    let band = band_height(viewport_height, growth)?;
    let overlap = band.checked_sub(growth).ok_or(StitchError::Arithmetic {
        operation: "calculating append overlap",
    })?;
    let old_end = old
        .height()
        .checked_sub(overlap)
        .ok_or(StitchError::CheckedCrop {
            x: 0,
            y: 0,
            width: old.width(),
            height: overlap,
            image_width: old.width(),
            image_height: old.height(),
        })?;
    let incoming_start = viewport_height
        .checked_sub(band)
        .ok_or(StitchError::Arithmetic {
            operation: "calculating incoming append band",
        })?;
    Frame::from_row_ranges(
        old.width(),
        old.pixel_format(),
        &[
            (old, 0..old_end),
            (incoming, incoming_start..viewport_height),
        ],
    )
}

#[cfg(test)]
pub(crate) fn append_and_repaint_in_place(
    old: &mut Frame,
    incoming: &Frame,
    growth: u32,
) -> Result<(), StitchError> {
    validate_pair(old, incoming)?;
    let viewport_height = incoming.height();
    let band = band_height(viewport_height, growth)?;
    let overlap = band.checked_sub(growth).ok_or(StitchError::Arithmetic {
        operation: "calculating append overlap",
    })?;
    let old_end = old
        .height()
        .checked_sub(overlap)
        .ok_or(StitchError::CheckedCrop {
            x: 0,
            y: 0,
            width: old.width(),
            height: overlap,
            image_width: old.width(),
            image_height: old.height(),
        })?;
    let incoming_start = viewport_height
        .checked_sub(band)
        .ok_or(StitchError::Arithmetic {
            operation: "calculating incoming append band",
        })?;
    let width = old.width();
    let pixel_format = old.pixel_format();
    let row_len = usize::try_from(width)
        .ok()
        .and_then(|value| value.checked_mul(pixel_format.channels() as usize))
        .ok_or(StitchError::Arithmetic {
            operation: "calculating packed row length",
        })?;
    let retained_len = (old_end as usize)
        .checked_mul(row_len)
        .ok_or(StitchError::Arithmetic {
            operation: "calculating retained append length",
        })?;
    let incoming_start =
        (incoming_start as usize)
            .checked_mul(row_len)
            .ok_or(StitchError::Arithmetic {
                operation: "calculating incoming append byte offset",
            })?;
    let final_height = old
        .height()
        .checked_add(growth)
        .ok_or(StitchError::Arithmetic {
            operation: "calculating appended height",
        })?;
    let final_len =
        (final_height as usize)
            .checked_mul(row_len)
            .ok_or(StitchError::Arithmetic {
                operation: "calculating appended buffer length",
            })?;
    let incoming_pixels = incoming.pixels();
    old.pixels_mut().truncate(retained_len);
    old.pixels_mut()
        .extend_from_slice(&incoming_pixels[incoming_start..]);
    debug_assert_eq!(old.pixels().len(), final_len);
    old.set_height(final_height);
    Ok(())
}

pub fn prepend_and_repaint(
    old: &Frame,
    incoming: &Frame,
    growth: u32,
) -> Result<Frame, StitchError> {
    validate_pair(old, incoming)?;
    let band = band_height(incoming.height(), growth)?;
    let old_start = band.checked_sub(growth).ok_or(StitchError::Arithmetic {
        operation: "calculating prepend overlap",
    })?;
    Frame::from_row_ranges(
        old.width(),
        old.pixel_format(),
        &[(incoming, 0..band), (old, old_start..old.height())],
    )
}

#[cfg(test)]
pub(crate) fn prepend_and_repaint_in_place(
    old: &mut Frame,
    incoming: &Frame,
    growth: u32,
) -> Result<(), StitchError> {
    validate_pair(old, incoming)?;
    let band = band_height(incoming.height(), growth)?;
    let old_start = band.checked_sub(growth).ok_or(StitchError::Arithmetic {
        operation: "calculating prepend overlap",
    })?;
    let width = old.width();
    let pixel_format = old.pixel_format();
    let row_len = usize::try_from(width)
        .ok()
        .and_then(|value| value.checked_mul(pixel_format.channels() as usize))
        .ok_or(StitchError::Arithmetic {
            operation: "calculating packed row length",
        })?;
    let incoming_len = (band as usize)
        .checked_mul(row_len)
        .ok_or(StitchError::Arithmetic {
            operation: "calculating incoming prepend length",
        })?;
    let old_start = (old_start as usize)
        .checked_mul(row_len)
        .ok_or(StitchError::Arithmetic {
            operation: "calculating retained prepend byte offset",
        })?;
    let original_len = old.pixels().len();
    let final_height = old
        .height()
        .checked_add(growth)
        .ok_or(StitchError::Arithmetic {
            operation: "calculating prepended height",
        })?;
    let final_len =
        (final_height as usize)
            .checked_mul(row_len)
            .ok_or(StitchError::Arithmetic {
                operation: "calculating prepended buffer length",
            })?;
    old.pixels_mut().resize(final_len, 0);
    old.pixels_mut()
        .copy_within(old_start..original_len, incoming_len);
    old.pixels_mut()[..incoming_len].copy_from_slice(&incoming.pixels()[..incoming_len]);
    old.set_height(final_height);
    Ok(())
}

pub fn synthesize_append(
    reference: &Frame,
    incoming: &Frame,
    shift: u32,
) -> Result<Frame, StitchError> {
    validate_pair(reference, incoming)?;
    if reference.height() != incoming.height() {
        return Err(StitchError::InvalidFrame {
            message: "motion-reference and incoming heights differ".to_owned(),
        });
    }
    let height = reference.height();
    let band = band_height(height, shift)?;
    let keep = height.checked_sub(band).ok_or(StitchError::Arithmetic {
        operation: "calculating retained append reference rows",
    })?;
    let reference_end = shift.checked_add(keep).ok_or(StitchError::Arithmetic {
        operation: "calculating append reference range",
    })?;
    let incoming_start = height.checked_sub(band).ok_or(StitchError::Arithmetic {
        operation: "calculating incoming append reference range",
    })?;
    Frame::from_row_ranges(
        reference.width(),
        reference.pixel_format(),
        &[
            (reference, shift..reference_end),
            (incoming, incoming_start..height),
        ],
    )
}

pub(crate) fn synthesize_append_in_place(
    reference: &mut Frame,
    incoming: &Frame,
    shift: u32,
) -> Result<(), StitchError> {
    validate_pair(reference, incoming)?;
    if reference.height() != incoming.height() {
        return Err(StitchError::InvalidFrame {
            message: "motion-reference and incoming heights differ".to_owned(),
        });
    }
    let height = reference.height();
    let band = band_height(height, shift)?;
    let keep = height.checked_sub(band).ok_or(StitchError::Arithmetic {
        operation: "calculating retained append reference rows",
    })?;
    let reference_end = shift.checked_add(keep).ok_or(StitchError::Arithmetic {
        operation: "calculating append reference range",
    })?;
    let incoming_start = height.checked_sub(band).ok_or(StitchError::Arithmetic {
        operation: "calculating incoming append reference range",
    })?;
    let row_len = usize::try_from(reference.width())
        .ok()
        .and_then(|value| value.checked_mul(reference.pixel_format().channels() as usize))
        .ok_or(StitchError::Arithmetic {
            operation: "calculating packed row length",
        })?;
    let reference_start = (shift as usize)
        .checked_mul(row_len)
        .ok_or(StitchError::Arithmetic {
            operation: "calculating append reference byte offset",
        })?;
    let reference_end =
        (reference_end as usize)
            .checked_mul(row_len)
            .ok_or(StitchError::Arithmetic {
                operation: "calculating append reference length",
            })?;
    let incoming_start =
        (incoming_start as usize)
            .checked_mul(row_len)
            .ok_or(StitchError::Arithmetic {
                operation: "calculating incoming append reference byte offset",
            })?;
    reference
        .pixels_mut()
        .copy_within(reference_start..reference_end, 0);
    reference.pixels_mut()[reference_end - reference_start..]
        .copy_from_slice(&incoming.pixels()[incoming_start..]);
    Ok(())
}

pub fn synthesize_prepend(
    reference: &Frame,
    incoming: &Frame,
    shift: u32,
) -> Result<Frame, StitchError> {
    validate_pair(reference, incoming)?;
    if reference.height() != incoming.height() {
        return Err(StitchError::InvalidFrame {
            message: "motion-reference and incoming heights differ".to_owned(),
        });
    }
    let height = reference.height();
    let band = band_height(height, shift)?;
    let keep = height.checked_sub(band).ok_or(StitchError::Arithmetic {
        operation: "calculating retained prepend reference rows",
    })?;
    let reference_start = band.checked_sub(shift).ok_or(StitchError::Arithmetic {
        operation: "calculating prepend reference start",
    })?;
    let reference_end = reference_start
        .checked_add(keep)
        .ok_or(StitchError::Arithmetic {
            operation: "calculating prepend reference end",
        })?;
    Frame::from_row_ranges(
        reference.width(),
        reference.pixel_format(),
        &[
            (incoming, 0..band),
            (reference, reference_start..reference_end),
        ],
    )
}

pub(crate) fn synthesize_prepend_in_place(
    reference: &mut Frame,
    incoming: &Frame,
    shift: u32,
) -> Result<(), StitchError> {
    validate_pair(reference, incoming)?;
    if reference.height() != incoming.height() {
        return Err(StitchError::InvalidFrame {
            message: "motion-reference and incoming heights differ".to_owned(),
        });
    }
    let height = reference.height();
    let band = band_height(height, shift)?;
    let keep = height.checked_sub(band).ok_or(StitchError::Arithmetic {
        operation: "calculating retained prepend reference rows",
    })?;
    let reference_start = band.checked_sub(shift).ok_or(StitchError::Arithmetic {
        operation: "calculating prepend reference start",
    })?;
    let reference_end = reference_start
        .checked_add(keep)
        .ok_or(StitchError::Arithmetic {
            operation: "calculating prepend reference end",
        })?;
    let row_len = usize::try_from(reference.width())
        .ok()
        .and_then(|value| value.checked_mul(reference.pixel_format().channels() as usize))
        .ok_or(StitchError::Arithmetic {
            operation: "calculating packed row length",
        })?;
    let band_len = (band as usize)
        .checked_mul(row_len)
        .ok_or(StitchError::Arithmetic {
            operation: "calculating incoming prepend reference length",
        })?;
    let reference_start =
        (reference_start as usize)
            .checked_mul(row_len)
            .ok_or(StitchError::Arithmetic {
                operation: "calculating prepend reference byte offset",
            })?;
    let reference_end =
        (reference_end as usize)
            .checked_mul(row_len)
            .ok_or(StitchError::Arithmetic {
                operation: "calculating prepend reference length",
            })?;
    reference
        .pixels_mut()
        .copy_within(reference_start..reference_end, band_len);
    reference.pixels_mut()[..band_len].copy_from_slice(&incoming.pixels()[..band_len]);
    Ok(())
}

pub(crate) fn synthesize_append_axis(
    reference: &Frame,
    incoming: &Frame,
    axis: StitchAxis,
    shift: u32,
) -> Result<Frame, StitchError> {
    if axis == StitchAxis::Vertical {
        return synthesize_append(reference, incoming, shift);
    }
    validate_horizontal_pair(reference, incoming)?;
    let extent = reference.width();
    let band = band_size(extent, shift)?;
    let keep = extent.checked_sub(band).ok_or(StitchError::Arithmetic {
        operation: "calculating retained horizontal append reference columns",
    })?;
    let reference_end = shift.checked_add(keep).ok_or(StitchError::Arithmetic {
        operation: "calculating horizontal append reference range",
    })?;
    let incoming_start = extent.checked_sub(band).ok_or(StitchError::Arithmetic {
        operation: "calculating incoming horizontal append range",
    })?;
    from_column_ranges(
        reference.height(),
        reference.pixel_format(),
        &[
            (reference, shift..reference_end),
            (incoming, incoming_start..extent),
        ],
    )
}

pub(crate) fn synthesize_prepend_axis(
    reference: &Frame,
    incoming: &Frame,
    axis: StitchAxis,
    shift: u32,
) -> Result<Frame, StitchError> {
    if axis == StitchAxis::Vertical {
        return synthesize_prepend(reference, incoming, shift);
    }
    validate_horizontal_pair(reference, incoming)?;
    let extent = reference.width();
    let band = band_size(extent, shift)?;
    let keep = extent.checked_sub(band).ok_or(StitchError::Arithmetic {
        operation: "calculating retained horizontal prepend reference columns",
    })?;
    let reference_start = band.checked_sub(shift).ok_or(StitchError::Arithmetic {
        operation: "calculating horizontal prepend reference start",
    })?;
    let reference_end = reference_start
        .checked_add(keep)
        .ok_or(StitchError::Arithmetic {
            operation: "calculating horizontal prepend reference end",
        })?;
    from_column_ranges(
        reference.height(),
        reference.pixel_format(),
        &[
            (incoming, 0..band),
            (reference, reference_start..reference_end),
        ],
    )
}

fn validate_horizontal_pair(reference: &Frame, incoming: &Frame) -> Result<(), StitchError> {
    if reference.geometry() != incoming.geometry() {
        return Err(StitchError::InvalidFrame {
            message: format!(
                "cannot compose horizontal reference {} with {}",
                reference.geometry(),
                incoming.geometry()
            ),
        });
    }
    Ok(())
}

fn from_column_ranges(
    height: u32,
    pixel_format: crate::PixelFormat,
    ranges: &[(&Frame, std::ops::Range<u32>)],
) -> Result<Frame, StitchError> {
    let width = ranges.iter().try_fold(0_u32, |total, (_, range)| {
        total.checked_add(range.end.checked_sub(range.start)?)
    });
    let width = width.ok_or(StitchError::Arithmetic {
        operation: "calculating composed width",
    })?;
    let channels = pixel_format.channels() as usize;
    let mut pixels = Vec::with_capacity(width as usize * height as usize * channels);
    for y in 0..height {
        for (frame, range) in ranges {
            if frame.height() != height
                || frame.pixel_format() != pixel_format
                || range.start >= range.end
                || range.end > frame.width()
            {
                return Err(StitchError::CheckedCrop {
                    x: range.start,
                    y: 0,
                    width: range.end.saturating_sub(range.start),
                    height,
                    image_width: frame.width(),
                    image_height: frame.height(),
                });
            }
            let row = frame.row(y)?;
            let start = range.start as usize * channels;
            let end = range.end as usize * channels;
            pixels.extend_from_slice(&row[start..end]);
        }
    }
    Frame::new(width, height, pixel_format, pixels)
}

#[cfg(test)]
mod tests {
    use proptest::prelude::*;

    use super::*;
    use crate::PixelFormat;

    fn rows(values: &[u8]) -> Frame {
        Frame::new(1, values.len() as u32, PixelFormat::Gray8, values.to_vec()).unwrap()
    }

    #[test]
    fn append_matches_specification_example() {
        let old = rows(&[10, 11, 12, 13]);
        let incoming = rows(&[100, 101, 102, 103]);
        assert_eq!(
            append_and_repaint(&old, &incoming, 2).unwrap().pixels(),
            &[10, 11, 12, 101, 102, 103]
        );
    }

    #[test]
    fn prepend_matches_specification_example() {
        let old = rows(&[10, 11, 12, 13]);
        let incoming = rows(&[100, 101, 102, 103]);
        assert_eq!(
            prepend_and_repaint(&old, &incoming, 1).unwrap().pixels(),
            &[100, 101, 11, 12, 13]
        );
    }

    #[test]
    fn append_reference_matches_specification_example() {
        let reference = rows(&[10, 11, 12, 13, 14, 15, 16, 17]);
        let incoming = rows(&[100, 101, 102, 103, 104, 105, 106, 107]);
        assert_eq!(
            synthesize_append(&reference, &incoming, 3)
                .unwrap()
                .pixels(),
            &[13, 14, 15, 103, 104, 105, 106, 107]
        );
    }

    #[test]
    fn prepend_reference_matches_specification_example() {
        let reference = rows(&[10, 11, 12, 13, 14, 15, 16, 17]);
        let incoming = rows(&[100, 101, 102, 103, 104, 105, 106, 107]);
        assert_eq!(
            synthesize_prepend(&reference, &incoming, 3)
                .unwrap()
                .pixels(),
            &[100, 101, 102, 103, 104, 12, 13, 14]
        );
    }

    #[test]
    fn in_place_composition_matches_allocating_composition() {
        let old = rows(&[10, 11, 12, 13, 14, 15]);
        let incoming = rows(&[100, 101, 102, 103]);

        let expected = append_and_repaint(&old, &incoming, 2).unwrap();
        let mut actual = old.clone();
        append_and_repaint_in_place(&mut actual, &incoming, 2).unwrap();
        assert_eq!(actual, expected);

        let expected = prepend_and_repaint(&old, &incoming, 2).unwrap();
        let mut actual = old;
        prepend_and_repaint_in_place(&mut actual, &incoming, 2).unwrap();
        assert_eq!(actual, expected);
    }

    #[test]
    fn in_place_synthetic_reference_matches_allocating_composition() {
        let reference = rows(&[10, 11, 12, 13, 14, 15, 16, 17]);
        let incoming = rows(&[100, 101, 102, 103, 104, 105, 106, 107]);

        let expected = synthesize_append(&reference, &incoming, 3).unwrap();
        let mut actual = reference.clone();
        synthesize_append_in_place(&mut actual, &incoming, 3).unwrap();
        assert_eq!(actual, expected);

        let expected = synthesize_prepend(&reference, &incoming, 3).unwrap();
        let mut actual = reference;
        synthesize_prepend_in_place(&mut actual, &incoming, 3).unwrap();
        assert_eq!(actual, expected);
    }

    proptest! {
        #[test]
        fn legal_compositions_preserve_dimensions(
            height in 5_u32..500,
            old_extra in 0_u32..500,
            ratio_numerator in 1_u32..=60,
        ) {
            let shift = ((u64::from(height) * u64::from(ratio_numerator)) / 100) as u32;
            prop_assume!(shift > 0);
            let old_height = height + old_extra;
            let old = rows(&vec![1; old_height as usize]);
            let incoming = rows(&vec![2; height as usize]);
            let appended = append_and_repaint(&old, &incoming, shift).unwrap();
            let prepended = prepend_and_repaint(&old, &incoming, shift).unwrap();
            let append_reference = synthesize_append(&incoming, &incoming, shift).unwrap();
            let prepend_reference = synthesize_prepend(&incoming, &incoming, shift).unwrap();
            prop_assert_eq!(appended.height(), old_height + shift);
            prop_assert_eq!(prepended.height(), old_height + shift);
            prop_assert_eq!(append_reference.height(), height);
            prop_assert_eq!(prepend_reference.height(), height);

            let mut appended_in_place = old.clone();
            append_and_repaint_in_place(&mut appended_in_place, &incoming, shift).unwrap();
            prop_assert_eq!(appended_in_place, appended);

            let mut prepended_in_place = old;
            prepend_and_repaint_in_place(&mut prepended_in_place, &incoming, shift).unwrap();
            prop_assert_eq!(prepended_in_place, prepended);

            let mut append_reference_in_place = incoming.clone();
            synthesize_append_in_place(&mut append_reference_in_place, &incoming, shift).unwrap();
            prop_assert_eq!(append_reference_in_place, append_reference);

            let mut prepend_reference_in_place = incoming.clone();
            synthesize_prepend_in_place(&mut prepend_reference_in_place, &incoming, shift).unwrap();
            prop_assert_eq!(prepend_reference_in_place, prepend_reference);
        }
    }
}
