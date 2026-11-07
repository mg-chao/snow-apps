// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use gpui::{
    App, Bounds, Element, ElementId, GlobalElementId, InspectorElementId, IntoElement, LayoutId,
    Pixels, Role, ShapedLine, SharedString, Style, TextAlign, Window, accesskit, size,
};
use std::{cell::RefCell, sync::Arc};

thread_local! {
    static FONTS: RefCell<Vec<(gpui::Font, gpui::Font)>> = const { RefCell::new(Vec::new()) };
}

/// CSS's normal text uses pair kerning. DirectWrite needs the feature requested
/// explicitly; retain a caller's choice and reuse the derived feature allocation.
pub(super) fn picker_font(base: &gpui::Font) -> gpui::Font {
    if base
        .features
        .tag_value_list()
        .iter()
        .any(|(tag, _)| tag == "kern")
    {
        return base.clone();
    }
    FONTS.with(|fonts| {
        let mut fonts = fonts.borrow_mut();
        if let Some((_, font)) = fonts.iter().find(|(key, _)| key == base) {
            return font.clone();
        }
        let mut font = base.clone();
        let mut features = base.features.tag_value_list().to_vec();
        features.push(("kern".into(), 1));
        font.features = gpui::FontFeatures(Arc::new(features));
        if fonts.len() == 16 {
            fonts.remove(0);
        }
        fonts.push((base.clone(), font.clone()));
        font
    })
}

/// An unwrapped native label whose layout keeps the actual shaped advance.
/// Measured text leaves are rounded up by GPUI, which otherwise accumulates
/// spacing errors in rows of compact triggers at fractional display scales.
pub(super) struct IntrinsicText(SharedString);
impl IntrinsicText {
    pub fn new(text: SharedString) -> Self {
        Self(text)
    }
}
pub(super) struct TextLayout {
    line: ShapedLine,
    height: Pixels,
    align: TextAlign,
}
impl IntoElement for IntrinsicText {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for IntrinsicText {
    type RequestLayoutState = TextLayout;
    type PrepaintState = ();
    fn id(&self) -> Option<ElementId> {
        None
    }
    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        None
    }
    fn a11y_role(&self) -> Option<Role> {
        Some(Role::Label)
    }
    fn write_a11y_info(&self, node: &mut accesskit::Node) {
        node.set_value(self.0.to_string());
    }
    fn request_layout(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, TextLayout) {
        let text = window.text_style();
        let font_size = text.font_size.to_pixels(window.rem_size());
        let height = text
            .line_height
            .to_pixels(font_size.into(), window.rem_size());
        let line = window.text_system().shape_line(
            self.0.clone(),
            font_size,
            &[text.to_run(self.0.len())],
            None,
        );
        let style = Style {
            size: size(line.width.into(), height.into()),
            flex_shrink: 0.,
            ..Default::default()
        };
        let id = window.request_layout(style, [], cx);
        (
            id,
            TextLayout {
                line,
                height,
                align: text.text_align,
            },
        )
    }
    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut TextLayout,
        _: &mut Window,
        _: &mut App,
    ) {
    }
    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        state: &mut TextLayout,
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) {
        let _ = state.line.paint(
            bounds.origin,
            state.height,
            state.align,
            Some(bounds.size.width),
            window,
            cx,
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn picker_kerning_reuses_features_and_preserves_explicit_font_choices() {
        let mut base = gpui::font("Arial");
        base.features = gpui::FontFeatures(Arc::new(vec![("liga".into(), 0)]));
        let first = picker_font(&base);
        let second = picker_font(&base);
        assert!(Arc::ptr_eq(&first.features.0, &second.features.0));
        assert_eq!(
            first.features.tag_value_list(),
            &[("liga".into(), 0), ("kern".into(), 1)]
        );
        assert_eq!(base.features.tag_value_list(), &[("liga".into(), 0)]);
        base.features = gpui::FontFeatures(Arc::new(vec![("kern".into(), 0)]));
        assert_eq!(picker_font(&base), base);
    }
}
