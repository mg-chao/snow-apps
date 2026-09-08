use snow_draw_engine_core::{ErrorCode, Point};
use snow_draw_engine_document::{
    ElementId, TextData, TextLayoutSize, normalize_font_family, validate_text_layout_size,
};

use super::TextStyle;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TextCommitTarget {
    New,
    NewArrow(ElementId),
    Existing(ElementId),
}

#[derive(Clone, Debug, PartialEq)]
pub struct TextDraftCommit {
    pub target: TextCommitTarget,
    pub center: Point<f64>,
    pub text: String,
    pub measured_layout: TextLayoutSize,
    pub style: TextStyle,
    pub auto_resize: bool,
    pub update_default_style: bool,
}

impl TextDraftCommit {
    pub fn new(
        target: TextCommitTarget,
        center: Point<f64>,
        text: impl Into<String>,
        measured_layout: TextLayoutSize,
        style: TextStyle,
        auto_resize: bool,
        update_default_style: bool,
    ) -> Self {
        Self {
            target,
            center,
            text: text.into(),
            measured_layout,
            style,
            auto_resize,
            update_default_style,
        }
    }

    pub fn existing_id(&self) -> Option<ElementId> {
        match self.target {
            TextCommitTarget::New | TextCommitTarget::NewArrow(_) => None,
            TextCommitTarget::Existing(id) => Some(id),
        }
    }

    pub fn text_is_empty(&self) -> bool {
        self.text.trim().is_empty()
    }
}

pub(crate) fn text_with_style_attributes(text: &TextData, style: &TextStyle) -> TextData {
    let mut updated = text.clone();
    updated.color = style.color;
    updated.font_size = style.font_size;
    updated.font_family = normalize_font_family(style.font_family.clone());
    updated.fill = style.fill;
    updated.fill_style = style.fill_style;
    updated.stroke = style.stroke;
    updated.stroke_width = style.stroke_width;
    updated.corner_radii = style.corner_radii;
    updated.horizontal_align = style.horizontal_align;
    updated.vertical_align = style.vertical_align;
    updated.opacity = style.opacity;
    updated
}

pub(crate) fn text_with_committed_draft(
    base: &TextData,
    draft: &TextDraftCommit,
) -> Result<TextData, ErrorCode> {
    let layout = validate_text_layout_size(draft.measured_layout)?;
    let mut updated = text_with_style_attributes(base, &draft.style);
    updated.center = draft.center;
    updated.width = layout.width;
    updated.height = layout.height;
    updated.text = draft.text.clone();
    updated.auto_resize = draft.auto_resize;
    Ok(updated)
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::{ColorRgba8, CornerRadii};
    use snow_draw_engine_document::{FillStyle, TextHorizontalAlign, TextVerticalAlign};

    fn style() -> TextStyle {
        TextStyle {
            color: ColorRgba8 {
                r: 1,
                g: 2,
                b: 3,
                a: 255,
            },
            font_size: 21.0,
            font_family: Some("Inter".to_owned()),
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            corner_radii: CornerRadii::default(),
            horizontal_align: TextHorizontalAlign::Left,
            vertical_align: TextVerticalAlign::Center,
            opacity: 1.0,
        }
    }

    #[test]
    fn draft_commit_identifies_existing_empty_text() {
        let id = ElementId {
            index: 42,
            generation: 7,
        };
        let draft = TextDraftCommit::new(
            TextCommitTarget::Existing(id),
            Point::new(10.0, 20.0),
            " \n ",
            TextLayoutSize {
                width: 120.0,
                height: 40.0,
            },
            style(),
            false,
            true,
        );

        assert_eq!(draft.existing_id(), Some(id));
        assert!(draft.text_is_empty());
        assert_eq!(draft.center, Point::new(10.0, 20.0));
        assert_eq!(draft.measured_layout.width, 120.0);
        assert!(!draft.auto_resize);
        assert!(draft.update_default_style);
    }

    #[test]
    fn committed_draft_applies_style_layout_text_and_resize_flag() {
        let draft = TextDraftCommit::new(
            TextCommitTarget::New,
            Point::new(10.0, 20.0),
            "committed",
            TextLayoutSize {
                width: 120.0,
                height: 40.0,
            },
            TextStyle {
                font_family: Some("  Inter  ".to_owned()),
                ..style()
            },
            false,
            true,
        );

        let updated = text_with_committed_draft(
            &TextData {
                text: "base".to_owned(),
                auto_resize: true,
                ..TextData::default()
            },
            &draft,
        )
        .unwrap();

        assert_eq!(updated.text, "committed");
        assert_eq!(updated.center, Point::new(10.0, 20.0));
        assert_eq!(updated.width, 120.0);
        assert_eq!(updated.height, 40.0);
        assert_eq!(updated.font_size, draft.style.font_size);
        assert_eq!(updated.font_family, Some("Inter".to_owned()));
        assert_eq!(updated.color, draft.style.color);
        assert_eq!(updated.fill_style, draft.style.fill_style);
        assert_eq!(updated.horizontal_align, draft.style.horizontal_align);
        assert!(!updated.auto_resize);
    }
}
