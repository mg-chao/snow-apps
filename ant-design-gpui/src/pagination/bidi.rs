// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Visual runs for native labels on backends which flatten bidi glyph runs into
//! logical order. Characters stay logical; native glyph identities, joining,
//! and offsets inside grapheme clusters are retained.

use std::{collections::BTreeMap, ops::Range, sync::Arc};

use crate::Direction;
use gpui::{
    FontId, LineLayout, Pixels, ShapedGlyph, ShapedLine, ShapedRun, SharedString, TextRun, Window,
    px,
};
use unicode_bidi::{BidiInfo, Level};
use unicode_segmentation::UnicodeSegmentation;

struct Cluster {
    x: Pixels,
    width: Pixels,
    glyphs: Vec<(FontId, ShapedGlyph)>,
}

fn native_line(text: SharedString, size: Pixels, run: &TextRun, window: &Window) -> ShapedLine {
    let mut run = run.clone();
    run.len = text.len();
    window.text_system().shape_line(text, size, &[run], None)
}

fn clusters(
    line: &ShapedLine,
    raw: &str,
    prefix: usize,
    prefix_width: Pixels,
    width: Pixels,
) -> Vec<Cluster> {
    let starts = raw
        .grapheme_indices(true)
        .map(|(index, _)| index)
        .collect::<Vec<_>>();
    let mut grouped = BTreeMap::<usize, Vec<(FontId, ShapedGlyph)>>::new();
    for run in &line.runs {
        for glyph in &run.glyphs {
            let Some(index) = glyph
                .index
                .checked_sub(prefix)
                .filter(|index| *index < raw.len())
            else {
                continue;
            };
            let cluster = starts[starts
                .partition_point(|start| *start <= index)
                .saturating_sub(1)];
            let mut glyph = glyph.clone();
            glyph.index = index;
            glyph.position.x -= prefix_width;
            grouped
                .entry(cluster)
                .or_default()
                .push((run.font_id, glyph));
        }
    }
    let mut result = grouped
        .into_values()
        .map(|glyphs| Cluster {
            x: glyphs[0].1.position.x,
            width: px(0.),
            glyphs,
        })
        .collect::<Vec<_>>();
    let mut visual = (0..result.len()).collect::<Vec<_>>();
    visual
        .sort_by(|left, right| f32::from(result[*left].x).total_cmp(&f32::from(result[*right].x)));
    for (position, &index) in visual.iter().enumerate() {
        let end = visual
            .get(position + 1)
            .map_or(width, |next| result[*next].x);
        result[index].width = (end - result[index].x).max(px(0.));
    }
    result
}

fn append_glyph(runs: &mut Vec<ShapedRun>, font: FontId, glyph: ShapedGlyph) {
    if let Some(last) = runs.last_mut().filter(|last| last.font_id == font) {
        last.glyphs.push(glyph);
    } else {
        runs.push(ShapedRun {
            font_id: font,
            glyphs: vec![glyph],
        });
    }
}

/// Shape one logical paragraph into native glyph runs in visual order. This
/// helper is also path-included by the Windows verification surface.
pub(super) fn shape_paragraph(
    text: SharedString,
    direction: Direction,
    font_size: Pixels,
    run: &TextRun,
    window: &Window,
) -> ShapedLine {
    let range = 0..text.len();
    shape_wrapped_line(text, range, direction, font_size, run, window)
}

/// Shape a wrapped line using the complete paragraph's resolved embedding
/// levels. Weak characters on a continuation retain their preceding context.
/// The result owns only the requested line, with line-relative byte indices.
pub(super) fn shape_wrapped_line(
    text: SharedString,
    range: Range<usize>,
    direction: Direction,
    font_size: Pixels,
    run: &TextRun,
    window: &Window,
) -> ShapedLine {
    let line = &text[range.clone()];
    let mut shaped = native_line(line.to_owned().into(), font_size, run, window);
    if direction == Direction::Ltr && text.is_ascii() {
        return shaped;
    }
    let bidi = BidiInfo::new(
        &text,
        Some(if direction == Direction::Rtl {
            Level::rtl()
        } else {
            Level::ltr()
        }),
    );
    let Some(paragraph) = bidi
        .paragraphs
        .iter()
        .find(|paragraph| paragraph.range.start <= range.start && paragraph.range.end >= range.end)
    else {
        return shaped;
    };
    let (levels, visual_runs) = bidi.visual_runs(paragraph, range.clone());
    if visual_runs.len() == 1 && !levels[visual_runs[0].start].is_rtl() {
        return shaped;
    }
    let mut output = Vec::new();
    let mut cursor = px(0.);
    let mut ascent = shaped.ascent;
    let mut descent = shaped.descent;
    for visual_range in visual_runs {
        let raw = &text[visual_range.clone()];
        let rtl = levels[visual_range.start].is_rtl();
        let (native, prefix, prefix_width, width) = if rtl {
            // Embedding still supplies native contextual/mirrored glyph forms;
            // only placement is repaired when the backend discarded bidiLevel.
            let native = native_line(
                format!("\u{202b}{raw}\u{202c}").into(),
                font_size,
                run,
                window,
            );
            let prefix_width = native_line("\u{202b}".into(), font_size, run, window).width();
            let suffix_width = native_line("\u{202c}".into(), font_size, run, window).width();
            let width = (native.width() - prefix_width - suffix_width).max(px(0.));
            (native, '\u{202b}'.len_utf8(), prefix_width, width)
        } else {
            let native = native_line(raw.to_owned().into(), font_size, run, window);
            let width = native.width();
            (native, 0, px(0.), width)
        };
        ascent = ascent.max(native.ascent);
        descent = descent.max(native.descent);
        let mut groups = clusters(&native, raw, prefix, prefix_width, width);
        let already_visual =
            rtl && groups.len() > 1 && groups.first().unwrap().x > groups.last().unwrap().x;
        if rtl {
            groups.reverse();
        }
        let mut visual_cursor = px(0.);
        for cluster in groups {
            let target = if rtl && !already_visual {
                visual_cursor
            } else {
                cluster.x
            };
            for (font, mut glyph) in cluster.glyphs {
                glyph.position.x = cursor + target + (glyph.position.x - cluster.x);
                glyph.index += visual_range.start - range.start;
                append_glyph(&mut output, font, glyph);
            }
            visual_cursor += cluster.width;
        }
        cursor += width;
    }
    *shaped = Arc::new(LineLayout {
        font_size,
        width: cursor,
        ascent,
        descent,
        runs: output,
        len: line.len(),
    });
    shaped
}

#[cfg(test)]
mod tests {
    use super::*;
    use gpui::TestAppContext;

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
