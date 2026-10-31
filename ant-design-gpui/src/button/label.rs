// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native label layout for Ant's two-character Chinese spacing treatment.

use std::sync::Arc;

use gpui::{
    App, Bounds, Div, Element, ElementId, GlobalElementId, InspectorElementId, IntoElement,
    LayoutId, LineLayout, Pixels, ShapedLine, SharedString, Styled, TextAlign, Window, div,
    prelude::*, px, size,
};

/// Keep the inserted space during loading, when Ant removes only its additional
/// tracking. Shape the complete text before adjusting glyph advances, so native
/// pixel snapping applies once to the whole label, including its inserted space.
pub(super) fn button_label(
    label: SharedString,
    insert: bool,
    loading: bool,
    font_size: f32,
) -> Div {
    let container = div().flex_shrink_0().whitespace_nowrap();
    if insert && let Some([first, second]) = two_chinese_characters(&label) {
        container.child(TrackedLabel {
            text: format!("{first} {second}").into(),
            tracking: px(if loading { 0. } else { font_size * 0.34 }),
        })
    } else {
        container.child(label)
    }
}

struct TrackedLabel {
    text: SharedString,
    tracking: Pixels,
}

impl IntoElement for TrackedLabel {
    type Element = Self;

    fn into_element(self) -> Self {
        self
    }
}

impl Element for TrackedLabel {
    type RequestLayoutState = (ShapedLine, Pixels);
    type PrepaintState = ();

    fn id(&self) -> Option<ElementId> {
        None
    }

    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        None
    }

    fn request_layout(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        window: &mut Window,
        _: &mut App,
    ) -> (LayoutId, Self::RequestLayoutState) {
        let style = window.text_style();
        let font_size = style.font_size.to_pixels(window.rem_size());
        let line_height = window.pixel_snap(
            style
                .line_height
                .to_pixels(font_size.into(), window.rem_size()),
        );
        let mut line = window.text_system().shape_line(
            self.text.clone(),
            font_size,
            &[style.to_run(self.text.len())],
            None,
        );
        apply_tracking(&mut line, self.tracking);
        // Match GPUI's ordinary TextLayout: round the full line width up once
        // in logical pixels before the layout engine snaps to the display.
        let measured = size(line.width().ceil(), line_height);
        let layout = window.request_measured_layout(Default::default(), move |_, _, _, _| measured);
        (layout, (line, line_height))
    }

    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut Self::RequestLayoutState,
        _: &mut Window,
        _: &mut App,
    ) {
    }

    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        (line, line_height): &mut Self::RequestLayoutState,
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) {
        let _ = line.paint(
            bounds.origin,
            *line_height,
            TextAlign::Left,
            None,
            window,
            cx,
        );
    }
}

fn apply_tracking(line: &mut ShapedLine, tracking: Pixels) {
    if tracking <= px(0.) {
        return;
    }
    // The input is exactly one three-byte Chinese character, one ASCII space,
    // and another Chinese character. Clone runs before adding advances so the
    // text system's cached layout remains immutable and reusable.
    let mut runs = line.runs.clone();
    for glyph in runs.iter_mut().flat_map(|run| &mut run.glyphs) {
        glyph.position.x += tracking
            * if glyph.index >= 4 {
                2.
            } else if glyph.index >= 3 {
                1.
            } else {
                0.
            };
    }
    **line = Arc::new(LineLayout {
        font_size: line.font_size,
        width: line.width() + tracking * 2.,
        ascent: line.ascent,
        descent: line.descent,
        runs,
        len: line.len(),
    });
}

fn two_chinese_characters(label: &str) -> Option<[char; 2]> {
    let mut characters = label.chars();
    let first = characters.next()?;
    let second = characters.next()?;
    (characters.next().is_none()
        && ('\u{4e00}'..='\u{9fa5}').contains(&first)
        && ('\u{4e00}'..='\u{9fa5}').contains(&second))
    .then_some([first, second])
}

#[cfg(test)]
mod tests {
    use super::*;
    use gpui::{Context, IntoElement, Render, TestAppContext, Window, rgb};

    #[test]
    fn spacing_uses_the_reference_character_range_and_exactly_two_characters() {
        assert_eq!(two_chinese_characters("保存"), Some(['保', '存']));
        assert_eq!(two_chinese_characters("一龥"), Some(['一', '龥']));
        for label in ["", "保", "保存中", "Save", "保A", "保 存", "保龦", "保㐀"] {
            assert_eq!(two_chinese_characters(label), None, "{label}");
        }
    }

    struct LabelFixture {
        label: &'static str,
        insert: bool,
        loading: bool,
        font_size: f32,
    }

    impl Render for LabelFixture {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            div().flex().child(
                button_label(self.label.into(), self.insert, self.loading, self.font_size)
                    .text_size(px(self.font_size))
                    .bg(rgb(0xffffff)),
            )
        }
    }

    fn widths(
        label: &'static str,
        insert: bool,
        loading: bool,
        font_size: f32,
    ) -> (f32, f32, f32, f32) {
        let mut cx = TestAppContext::single();
        let window = cx.add_window(|_, _| LabelFixture {
            label,
            insert,
            loading,
            font_size,
        });
        cx.update_window(window.into(), |_, window, cx| {
            window.draw(cx).clear(cx);
            let measured =
                window.rendered_scene().quads[0].bounds.size.width.0 / window.scale_factor();
            let text: SharedString =
                if insert && let Some([first, second]) = two_chinese_characters(label) {
                    format!("{first} {second}").into()
                } else {
                    label.into()
                };
            let style = window.text_style();
            let runs = [style.to_run(text.len())];
            let mut shaped =
                window
                    .text_system()
                    .shape_line(text.clone(), px(font_size), &runs, None);
            let natural = f32::from(shaped.width());
            if insert && !loading && two_chinese_characters(label).is_some() {
                apply_tracking(&mut shaped, px(font_size * 0.34));
            }
            assert_eq!(
                f32::from(
                    window
                        .text_system()
                        .shape_line(text, px(font_size), &runs, None)
                        .width()
                ),
                natural,
                "tracking must not mutate the cached native shape"
            );
            (
                measured,
                natural,
                f32::from(shaped.width()),
                window.scale_factor(),
            )
        })
        .unwrap()
    }

    fn width(label: &'static str, insert: bool, loading: bool, font_size: f32) -> f32 {
        widths(label, insert, loading, font_size).0
    }

    #[test]
    fn native_spacing_adds_exactly_point_68_em_and_removes_tracking_during_loading() {
        for font_size in [12., 14., 16., 20.] {
            let (rest, natural, tracked, scale) = widths("保存", true, false, font_size);
            let loading = width("保存", true, true, font_size);
            let literal_space = width("保 存", false, false, font_size);
            assert!(
                (tracked - natural - font_size * 0.68).abs() < 0.0001,
                "font {font_size}: natural={natural}, tracked={tracked}"
            );
            // GPUI's text layout rounds the whole line to logical pixels,
            // then the layout engine rounds measured leaves to device pixels.
            assert!(
                (rest - (tracked.ceil() * scale).ceil() / scale).abs() < 0.02,
                "font {font_size}: rest={rest}, tracked={tracked}, scale={scale}"
            );
            assert!(
                (loading - literal_space).abs() < 0.02,
                "font {font_size}: loading={loading}, literal_space={literal_space}"
            );
        }
    }

    #[test]
    fn ordinary_labels_and_disabled_auto_spacing_keep_native_text_width() {
        for label in ["Save", "保存中", "保A", "保 存"] {
            assert_eq!(
                width(label, true, false, 14.),
                width(label, false, false, 14.)
            );
            assert_eq!(
                width(label, true, true, 14.),
                width(label, false, false, 14.)
            );
        }
        assert_eq!(
            width("保存", false, true, 14.),
            width("保存", false, false, 14.)
        );
    }
}
