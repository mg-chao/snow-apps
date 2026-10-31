// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Private pagination facade for the shared native paragraph compositor.

pub(super) use crate::behavior::paragraph::{shape_paragraph, shape_wrapped_line};

#[cfg(test)]
mod tests {
    use super::*;
    use crate::Direction;
    use gpui::{Pixels, ShapedLine, SharedString, TestAppContext, TextRun, Window, px};

    fn native_line(text: SharedString, size: Pixels, run: &TextRun, window: &Window) -> ShapedLine {
        let mut run = run.clone();
        run.len = text.len();
        window.text_system().shape_line(text, size, &[run], None)
    }

    #[test]
    fn pagination_bidi_keeps_latin_and_cjk_runs_logical_with_rtl_run_order() {
        let mut cx = TestAppContext::single();
        let window = cx.add_window(|_, _| gpui::Empty);
        window
            .update(&mut cx, |_, window, _| {
                for (text, left) in [("10 / page", 5), ("10 条/页", 3)] {
                    let run = window.text_style().to_run(text.len());
                    let snapshot = |line: &ShapedLine| {
                        line.runs
                            .iter()
                            .flat_map(|run| {
                                run.glyphs.iter().map(move |glyph| {
                                    (
                                        run.font_id,
                                        glyph.id,
                                        glyph.index,
                                        glyph.position,
                                        glyph.is_emoji,
                                    )
                                })
                            })
                            .collect::<Vec<_>>()
                    };
                    let cached = native_line(text.into(), px(14.), &run, window);
                    let before = snapshot(&cached);
                    let line = shape_paragraph(text.into(), Direction::Rtl, px(14.), &run, window);
                    let position = |index| {
                        line.runs
                            .iter()
                            .flat_map(|run| &run.glyphs)
                            .find(|glyph| glyph.index == index)
                            .unwrap()
                            .position
                            .x
                    };
                    assert!(position(left) < position(0), "{text}");
                    assert!(position(0) < position(1), "digits preserve logical order");
                    assert_eq!(line.text.as_ref(), text);
                    assert_eq!(line.len(), text.len());
                    assert_eq!(
                        snapshot(&cached),
                        before,
                        "retained native line stays unchanged"
                    );
                    assert_eq!(
                        snapshot(&native_line(text.into(), px(14.), &run, window)),
                        before,
                        "native shape cache stays unchanged"
                    );
                }
            })
            .unwrap();
    }

    #[test]
    fn pagination_bidi_preserves_unicode_boundaries_and_combining_clusters() {
        let mut cx = TestAppContext::single();
        let window = cx.add_window(|_, _| gpui::Empty);
        window
            .update(&mut cx, |_, window, _| {
                for text in [
                    "مجموع 500 نتيجة",
                    "א\u{05b0}ב",
                    "10 保存",
                    "A\u{0301} 500 نتائج",
                ] {
                    let line = shape_paragraph(
                        text.into(),
                        Direction::Rtl,
                        px(14.),
                        &window.text_style().to_run(text.len()),
                        window,
                    );
                    assert!(line.width() >= px(0.));
                    assert!(
                        line.runs
                            .iter()
                            .flat_map(|run| &run.glyphs)
                            .all(|glyph| glyph.index < text.len()
                                && text.is_char_boundary(glyph.index))
                    );
                    assert_eq!(line.text.as_ref(), text);
                }
                let text = "א\u{05b0}ב";
                let run = window.text_style().to_run(text.len());
                let original = native_line(text.into(), px(14.), &run, window);
                let visual = shape_paragraph(text.into(), Direction::Rtl, px(14.), &run, window);
                let delta = |line: &ShapedLine| {
                    let glyphs = line
                        .runs
                        .iter()
                        .flat_map(|run| &run.glyphs)
                        .collect::<Vec<_>>();
                    Some(
                        glyphs.iter().find(|glyph| glyph.index == 2)?.position.x
                            - glyphs.iter().find(|glyph| glyph.index == 0)?.position.x,
                    )
                };
                if let (Some(before), Some(after)) = (delta(&original), delta(&visual)) {
                    assert_eq!(before, after, "combining glyph offset is preserved");
                }
            })
            .unwrap();
    }

    #[test]
    fn pagination_wrapped_rtl_line_retains_preceding_strong_paragraph_context() {
        let mut cx = TestAppContext::single();
        let window = cx.add_window(|_, _| gpui::Empty);
        window
            .update(&mut cx, |_, window, _| {
                let text: SharedString = "51-60 of 500 items".into();
                let run = window.text_style().to_run(text.len());
                let line = shape_wrapped_line(
                    text.clone(),
                    9..text.len(),
                    Direction::Rtl,
                    px(14.),
                    &run,
                    window,
                );
                let position = |index| {
                    line.runs
                        .iter()
                        .flat_map(|run| &run.glyphs)
                        .find(|glyph| glyph.index == index)
                        .unwrap()
                        .position
                        .x
                };
                assert!(
                    position(0) < position(4),
                    "500 stays left of items after wrapping"
                );
                assert!(
                    position(0) < position(1),
                    "digits retain their logical order"
                );
                assert_eq!(line.text.as_ref(), "500 items");
                assert_eq!(line.len(), "500 items".len());
                assert!(
                    line.runs
                        .iter()
                        .flat_map(|run| &run.glyphs)
                        .all(|glyph| glyph.index < line.len())
                );
            })
            .unwrap();
    }
}
