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

/// Elide a static RTL paragraph at its physical inline end (the left edge).
/// Preserve the complete paragraph's shaping and glyph indices: reshaping a
/// logical suffix would lose mixed-direction order and Arabic joining forms.
/// The original logical text remains available to the semantic text element.
pub(crate) fn elide_rtl_inline_end(
    full: &ShapedLine,
    ellipsis: &ShapedLine,
    width: Pixels,
) -> ShapedLine {
    if full.width() <= width {
        return full.clone();
    }
    let width = width.max(px(0.));
    let offset = full.width() - width;
    let cutoff = offset + ellipsis.width();
    let mut visual = clusters(full, &full.text, 0, px(0.), full.width());
    visual.sort_by(|left, right| f32::from(left.x).total_cmp(&f32::from(right.x)));
    // Removing whole graphemes can leave unused space at the inline end.
    // CSS places the marker immediately beside the first retained cluster,
    // rather than separating the marker from that text with the remainder.
    let marker_offset = visual
        .iter()
        .find(|cluster| cluster.x >= cutoff)
        .map_or(px(0.), |cluster| cluster.x - offset - ellipsis.width());
    let mut runs = Vec::new();
    for run in &ellipsis.runs {
        for glyph in &run.glyphs {
            let mut glyph = glyph.clone();
            // This helper receives a single uniformly decorated paragraph.
            // Associate the marker with that same decoration, without adding
            // invented bytes to its logical source or accessible value.
            glyph.index = 0;
            glyph.position.x += marker_offset;
            append_glyph(&mut runs, run.font_id, glyph);
        }
    }
    for cluster in visual {
        if cluster.x < cutoff {
            continue;
        }
        for (font, mut glyph) in cluster.glyphs {
            glyph.position.x -= offset;
            append_glyph(&mut runs, font, glyph);
        }
    }
    let mut line = full.clone();
    *line = Arc::new(LineLayout {
        font_size: full.font_size,
        width,
        ascent: full.ascent.max(ellipsis.ascent),
        descent: full.descent.max(ellipsis.descent),
        runs,
        len: full.len(),
    });
    line
}

/// Shape one logical paragraph into native glyph runs in visual order. This
/// helper is also path-included by the Windows verification surface.
pub(crate) fn shape_paragraph(
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
pub(crate) fn shape_wrapped_line(
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
