use snow_draw_engine::{
    ActiveTextDraftPresentation, ActiveTextDraftTarget, Point, TextCommitTarget, TextData,
    TextDraftCommit, TextStyle,
};

use crate::abi::convert::snow_element_id_to_rust;
use crate::abi::types::{SnowActiveTextDraftPresentation, SnowError, SnowTextCommitDraft};

pub(crate) fn utf8_prefix_length_at_char_boundary(text: &str, capacity: usize) -> usize {
    let bytes = text.as_bytes();
    let mut len = bytes.len().min(capacity);
    while !text.is_char_boundary(len) {
        len = len.saturating_sub(1);
    }
    len
}

pub(crate) fn copy_str_to_c_char_field<const N: usize>(
    out: &mut [std::ffi::c_char; N],
    len_out: &mut u32,
    truncated_out: &mut u8,
    text: &str,
) {
    out.fill(0);
    let len = utf8_prefix_length_at_char_boundary(text, N);
    *len_out = len as u32;
    *truncated_out = u8::from(len < text.len());
    for (target, source) in out.iter_mut().zip(text.as_bytes().iter().take(len)) {
        *target = *source as std::ffi::c_char;
    }
}

pub(crate) fn copy_optional_str_to_c_char_field<const N: usize>(
    out: &mut [std::ffi::c_char; N],
    len_out: &mut u32,
    truncated_out: &mut u8,
    text: Option<&str>,
) {
    match text {
        Some(text) => copy_str_to_c_char_field(out, len_out, truncated_out, text),
        None => {
            out.fill(0);
            *len_out = 0;
            *truncated_out = 0;
        }
    }
}

pub(crate) fn string_from_c_char_field<const N: usize>(
    bytes: &[std::ffi::c_char; N],
    len: u32,
) -> Option<String> {
    let len = (len as usize).min(N);
    let bytes = bytes
        .iter()
        .take(len)
        .map(|value| *value as u8)
        .collect::<Vec<_>>();
    std::str::from_utf8(&bytes)
        .ok()
        .map(|value| value.to_owned())
}

pub(crate) fn text_string_from_raw(
    text_utf8: *const std::ffi::c_char,
    text_utf8_len: u32,
) -> Result<String, SnowError> {
    if text_utf8.is_null() && text_utf8_len != 0 {
        return Err(SnowError::InvalidArgument);
    }
    let bytes = if text_utf8_len == 0 {
        &[][..]
    } else {
        unsafe { std::slice::from_raw_parts(text_utf8.cast::<u8>(), text_utf8_len as usize) }
    };
    std::str::from_utf8(bytes)
        .map(|text| text.to_owned())
        .map_err(|_| SnowError::InvalidArgument)
}

pub(crate) fn text_draft_commit_from_c(
    draft: &SnowTextCommitDraft,
) -> Result<TextDraftCommit, SnowError> {
    let text = text_string_from_raw(draft.text_utf8, draft.text_utf8_len)?;
    let target = if draft.has_existing_element != 0 {
        TextCommitTarget::Existing(snow_element_id_to_rust(draft.element_id))
    } else if draft.arrow_id.generation != 0 {
        TextCommitTarget::NewArrow(snow_element_id_to_rust(draft.arrow_id))
    } else {
        TextCommitTarget::New
    };
    Ok(TextDraftCommit::new(
        target,
        Point::new(draft.center_x, draft.center_y),
        text,
        draft.measured_layout.into(),
        draft.style.into(),
        draft.auto_resize != 0,
        draft.update_default_style != 0,
    ))
}

pub(crate) fn active_text_draft_from_c(
    draft: &SnowActiveTextDraftPresentation,
) -> Result<ActiveTextDraftPresentation, SnowError> {
    let text = text_string_from_raw(draft.text_utf8, draft.text_utf8_len)?;
    let style: TextStyle = draft.style.into();
    let target = if draft.has_existing_element != 0 {
        ActiveTextDraftTarget::Existing(snow_element_id_to_rust(draft.element_id))
    } else if draft.arrow_id.generation != 0 {
        ActiveTextDraftTarget::NewArrow(snow_element_id_to_rust(draft.arrow_id))
    } else {
        ActiveTextDraftTarget::New
    };
    Ok(ActiveTextDraftPresentation {
        target,
        revision: 0,
        text: TextData {
            center: Point::new(draft.center_x, draft.center_y),
            width: draft.width,
            height: draft.height,
            rotation: draft.rotation,
            text,
            color: style.color,
            font_size: style.font_size,
            font_family: style.font_family,
            fill: style.fill,
            fill_style: style.fill_style,
            stroke: style.stroke,
            stroke_width: style.stroke_width,
            corner_radii: style.corner_radii,
            horizontal_align: style.horizontal_align,
            vertical_align: style.vertical_align,
            auto_resize: draft.auto_resize != 0,
            opacity: style.opacity,
        },
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::abi::types::{
        SnowActiveTextDraftPresentation, SnowElementId, SnowTextCommitDraft, SnowTextLayoutSize,
        SnowTextStyle,
    };
    use snow_draw_engine::{ActiveTextDraftTarget, ElementId, TextCommitTarget};

    #[test]
    fn text_string_from_raw_accepts_empty_null_text() {
        assert_eq!(text_string_from_raw(std::ptr::null(), 0), Ok(String::new()));
    }

    #[test]
    fn text_string_from_raw_rejects_nonzero_length_null_text() {
        assert_eq!(
            text_string_from_raw(std::ptr::null(), 1),
            Err(SnowError::InvalidArgument)
        );
    }

    #[test]
    fn text_string_from_raw_rejects_invalid_utf8() {
        let bytes = [0xffu8];
        assert_eq!(
            text_string_from_raw(bytes.as_ptr().cast(), bytes.len() as u32),
            Err(SnowError::InvalidArgument)
        );
    }

    #[test]
    fn copy_str_to_c_char_field_truncates_at_utf8_boundary() {
        let text = format!("{}{}", "a".repeat(4), "😀");
        let mut field = [0 as std::ffi::c_char; 7];
        let mut len = 0;
        let mut truncated = 0;

        copy_str_to_c_char_field(&mut field, &mut len, &mut truncated, &text);

        assert_eq!(len, 4);
        assert_eq!(truncated, 1);
        assert_eq!(
            string_from_c_char_field(&field, len),
            Some("aaaa".to_owned())
        );
    }

    #[test]
    fn copy_optional_str_to_c_char_field_clears_empty_value() {
        let mut field = [1 as std::ffi::c_char; 8];
        let mut len = 99;
        let mut truncated = 1;

        copy_optional_str_to_c_char_field(&mut field, &mut len, &mut truncated, None);

        assert_eq!(field, [0 as std::ffi::c_char; 8]);
        assert_eq!(len, 0);
        assert_eq!(truncated, 0);
    }

    #[test]
    fn string_from_c_char_field_bounds_reported_length() {
        let mut field = [0 as std::ffi::c_char; 4];
        field.copy_from_slice(&[
            b't' as std::ffi::c_char,
            b'e' as std::ffi::c_char,
            b'x' as std::ffi::c_char,
            b't' as std::ffi::c_char,
        ]);

        assert_eq!(
            string_from_c_char_field(&field, 99),
            Some("text".to_owned())
        );
    }

    #[test]
    fn text_draft_commit_from_c_converts_typed_payload() {
        let text = "draft text";
        let draft = SnowTextCommitDraft {
            element_id: SnowElementId {
                index: 42,
                generation: 7,
            },
            has_existing_element: 1,
            auto_resize: 1,
            update_default_style: 1,
            center_x: 12.0,
            center_y: 34.0,
            text_utf8: text.as_ptr().cast(),
            text_utf8_len: text.len() as u32,
            measured_layout: SnowTextLayoutSize {
                width: 120.0,
                height: 44.0,
            },
            style: SnowTextStyle {
                font_size: 27.0,
                ..SnowTextStyle::default()
            },
            ..SnowTextCommitDraft::default()
        };

        let commit = text_draft_commit_from_c(&draft).expect("draft should convert");

        assert_eq!(
            commit.target,
            TextCommitTarget::Existing(ElementId {
                index: 42,
                generation: 7
            })
        );
        assert_eq!(commit.center.x, 12.0);
        assert_eq!(commit.center.y, 34.0);
        assert_eq!(commit.text, "draft text");
        assert_eq!(commit.measured_layout.width, 120.0);
        assert_eq!(commit.measured_layout.height, 44.0);
        assert_eq!(commit.style.font_size, 27.0);
        assert!(commit.auto_resize);
        assert!(commit.update_default_style);
    }

    #[test]
    fn active_text_draft_from_c_converts_typed_payload() {
        let text = "active draft";
        let draft = SnowActiveTextDraftPresentation {
            element_id: SnowElementId {
                index: 9,
                generation: 4,
            },
            has_existing_element: 1,
            auto_resize: 1,
            center_x: 14.0,
            center_y: 28.0,
            width: 120.0,
            height: 44.0,
            rotation: 0.25,
            text_utf8: text.as_ptr().cast(),
            text_utf8_len: text.len() as u32,
            style: SnowTextStyle {
                font_size: 31.0,
                opacity: 0.6,
                ..SnowTextStyle::default()
            },
            ..SnowActiveTextDraftPresentation::default()
        };

        let active = active_text_draft_from_c(&draft).expect("active draft should convert");

        assert_eq!(
            active.target,
            ActiveTextDraftTarget::Existing(ElementId {
                index: 9,
                generation: 4,
            })
        );
        assert_eq!(active.revision, 0);
        assert_eq!(active.text.center.x, 14.0);
        assert_eq!(active.text.center.y, 28.0);
        assert_eq!(active.text.width, 120.0);
        assert_eq!(active.text.height, 44.0);
        assert_eq!(active.text.rotation, 0.25);
        assert_eq!(active.text.text, "active draft");
        assert_eq!(active.text.font_size, 31.0);
        assert_eq!(active.text.opacity, 0.6);
        assert!(active.text.auto_resize);
    }
}
