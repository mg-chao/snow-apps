// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{
    cell::RefCell,
    collections::HashMap,
    rc::{Rc, Weak},
};

use gpui::{
    Bounds, Font, Hsla, Pixels, Point, SharedString, TextRun, Window, WrappedLine, point, px, size,
};
use unicode_segmentation::UnicodeSegmentation;

use super::geometry::{CaretLocation, CaretStop, InputGeometry, TextCluster};

#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub(super) enum TextAlignment {
    #[default]
    Left,
    Center,
    Right,
}
impl TextAlignment {
    fn offset(self, available: Pixels, width: Pixels) -> Pixels {
        let remaining = (available - width).max(px(0.));
        match self {
            Self::Left => px(0.),
            Self::Center => remaining / 2.,
            Self::Right => remaining,
        }
    }
}

#[derive(Clone, PartialEq)]
pub(super) struct LayoutStyle {
    pub font: Font,
    pub font_size: Pixels,
    pub line_height: Pixels,
    pub wrap_width: Option<Pixels>,
    pub masked: bool,
    pub mask_glyph: SharedString,
    pub right_to_left: bool,
}

pub(super) struct Paragraph {
    pub source: SharedString,
    shaped: WrappedLine,
    carets: Vec<CaretStop>,
    clusters: Vec<TextCluster>,
    row_widths: Vec<Pixels>,
}

pub(super) struct PositionedParagraph {
    pub paragraph: Rc<Paragraph>,
    pub start: usize,
    pub top: Pixels,
}

pub(super) struct TextLayout {
    pub paragraphs: Vec<PositionedParagraph>,
    pub width: Pixels,
    pub height: Pixels,
    right_to_left: bool,
}

#[derive(Default)]
pub(super) struct LayoutCache {
    style: Option<LayoutStyle>,
    paragraphs: HashMap<SharedString, Rc<Paragraph>>,
    layout: Option<(SharedString, Rc<TextLayout>)>,
}

/// A mounted editor can answer a geometry-dependent command before the next
/// frame. The weak cache avoids retaining the view after it is unmounted, and
/// the next draw reuses any paragraph shaped by an input/IME query.
pub(super) struct NavigationLayout {
    pub cache: Weak<RefCell<LayoutCache>>,
    pub style: LayoutStyle,
    pub alignment: TextAlignment,
    pub viewport: Bounds<Pixels>,
    pub scroll: Point<Pixels>,
}

impl NavigationLayout {
    pub fn geometry(
        &self,
        text: &str,
        revision: u64,
        masked: bool,
        multiline: bool,
        caret: CaretLocation,
        window: &Window,
    ) -> Option<Rc<InputGeometry>> {
        let cache = self.cache.upgrade()?;
        let style = LayoutStyle {
            masked,
            wrap_width: multiline.then_some(self.viewport.size.width.max(px(1.))),
            ..self.style.clone()
        };
        let layout = cache.borrow_mut().shape(text, &style, window);
        let mut geometry = layout.geometry(
            revision,
            self.viewport,
            self.scroll,
            self.style.line_height,
            self.alignment,
        );
        geometry.scroll = geometry.scroll_to_caret(caret, self.scroll);
        geometry.scroll.x = geometry
            .scroll
            .x
            .min((layout.width - self.viewport.size.width).max(px(0.)));
        geometry.scroll.y = geometry
            .scroll
            .y
            .min((layout.height - self.viewport.size.height).max(px(0.)));
        Some(Rc::new(geometry))
    }
}

impl LayoutCache {
    pub fn shape(&mut self, text: &str, style: &LayoutStyle, window: &Window) -> Rc<TextLayout> {
        if self.style.as_ref() != Some(style) {
            self.paragraphs.clear();
            self.layout = None;
            self.style = Some(style.clone());
        }
        if let Some((source, layout)) = &self.layout
            && source.as_ref() == text
        {
            return layout.clone();
        }
        let mut retained: HashMap<SharedString, Rc<Paragraph>> = HashMap::new();
        let mut paragraphs = Vec::new();
        let mut top = px(0.);
        let mut width = px(0.);
        let mut start = 0;
        for source in text.split('\n') {
            let paragraph = self
                .paragraphs
                .get(source)
                .or_else(|| retained.get(source))
                .cloned()
                .unwrap_or_else(|| {
                    let mut boundaries = Vec::new();
                    let display = if style.masked {
                        let mut display = String::new();
                        for (offset, _) in source.grapheme_indices(true) {
                            boundaries.push((offset, display.len()));
                            display.push_str(&style.mask_glyph);
                        }
                        display
                    } else {
                        boundaries.extend(
                            source
                                .grapheme_indices(true)
                                .map(|(offset, _)| (offset, offset)),
                        );
                        source.to_owned()
                    };
                    boundaries.push((source.len(), display.len()));
                    let run = TextRun {
                        len: display.len(),
                        font: style.font.clone(),
                        color: gpui::black(),
                        background_color: None,
                        underline: None,
                        strikethrough: None,
                    };
                    let shaped = window
                        .text_system()
                        .shape_text(
                            display.into(),
                            style.font_size,
                            &[run],
                            style.wrap_width,
                            None,
                        )
                        .expect("valid input text layout")
                        .into_iter()
                        .next()
                        .unwrap_or_default();
                    let (carets, clusters) =
                        paragraph_geometry(&shaped, &boundaries, style.line_height);
                    let mut row_starts = vec![px(0.)];
                    row_starts.extend(shaped.wrap_boundaries().iter().map(|boundary| {
                        shaped.runs()[boundary.run_ix].glyphs[boundary.glyph_ix]
                            .position
                            .x
                    }));
                    row_starts.push(shaped.unwrapped_layout.width);
                    let row_widths = row_starts
                        .windows(2)
                        .map(|pair| pair[1] - pair[0])
                        .collect();
                    Rc::new(Paragraph {
                        source: source.to_owned().into(),
                        shaped,
                        carets,
                        clusters,
                        row_widths,
                    })
                });
            let height = paragraph.height(style.line_height);
            width = width.max(paragraph.width());
            retained.insert(paragraph.source.clone(), paragraph.clone());
            paragraphs.push(PositionedParagraph {
                paragraph,
                start,
                top,
            });
            top += height;
            start += source.len() + 1;
        }
        self.paragraphs = retained;
        let layout = Rc::new(TextLayout {
            paragraphs,
            width,
            height: top,
            right_to_left: style.right_to_left,
        });
        self.layout = Some((text.to_owned().into(), layout.clone()));
        layout
    }
}

impl Paragraph {
    pub fn height(&self, line_height: Pixels) -> Pixels {
        self.shaped.size(line_height).height.max(line_height)
    }
    pub fn width(&self) -> Pixels {
        self.shaped.width()
    }
    /// Inputs have one foreground color and no per-run text decorations. Paint
    /// through GPUI's native glyph atlas with a current color so animation never
    /// invalidates shaping, wrapping, hit testing or accessibility geometry.
    pub fn paint(
        &self,
        origin: Point<Pixels>,
        line_height: Pixels,
        color: Hsla,
        alignment: TextAlignment,
        available_width: Pixels,
        window: &mut Window,
    ) {
        let shaped = &self.shaped;
        let layout = &shaped.unwrapped_layout;
        let bounds = Bounds::new(
            origin,
            size(
                available_width.max(layout.width),
                shaped.size(line_height).height,
            ),
        );
        window.paint_layer(bounds, |window| {
            let baseline = (line_height - layout.ascent - layout.descent) / 2. + layout.ascent;
            let mut wraps = shaped.wrap_boundaries().iter().peekable();
            let mut row_x = px(0.);
            let mut row_y = px(0.);
            let mut row = 0;
            for (run_ix, run) in layout.runs.iter().enumerate() {
                let max_size = window
                    .text_system()
                    .bounding_box(run.font_id, layout.font_size)
                    .size;
                for (glyph_ix, glyph) in run.glyphs.iter().enumerate() {
                    if wraps
                        .peek()
                        .is_some_and(|w| w.run_ix == run_ix && w.glyph_ix == glyph_ix)
                    {
                        wraps.next();
                        row_x = glyph.position.x;
                        row_y += line_height;
                        row += 1;
                    }
                    let horizontal = alignment.offset(available_width, self.row_widths[row]);
                    let top = origin + point(glyph.position.x - row_x + horizontal, row_y);
                    if !Bounds::new(top, max_size).intersects(&window.content_mask().bounds) {
                        continue;
                    }
                    let position = top + point(px(0.), baseline + glyph.position.y);
                    if glyph.is_emoji {
                        let _ =
                            window.paint_emoji(position, run.font_id, glyph.id, layout.font_size);
                    } else {
                        let _ = window.paint_glyph(
                            position,
                            run.font_id,
                            glyph.id,
                            layout.font_size,
                            color,
                        );
                    }
                }
            }
        });
    }
}

impl TextLayout {
    pub fn geometry(
        &self,
        revision: u64,
        viewport: Bounds<Pixels>,
        scroll: Point<Pixels>,
        line_height: Pixels,
        alignment: TextAlignment,
    ) -> InputGeometry {
        let mut carets = Vec::with_capacity(
            self.paragraphs
                .iter()
                .map(|p| p.paragraph.carets.len())
                .sum(),
        );
        let mut clusters = Vec::with_capacity(
            self.paragraphs
                .iter()
                .map(|p| p.paragraph.clusters.len() + 1)
                .sum(),
        );
        for positioned in &self.paragraphs {
            let paragraph = &positioned.paragraph;
            let translation = point(px(0.), positioned.top);
            let row_offset = |y: Pixels| {
                let row = (y / line_height).round() as usize;
                alignment.offset(
                    viewport.size.width.max(self.width),
                    paragraph.row_widths[row],
                )
            };
            for caret in &paragraph.carets {
                carets.push(CaretStop {
                    offset: positioned.start + caret.offset,
                    position: translation
                        + caret.position
                        + point(row_offset(caret.position.y), px(0.)),
                });
            }
            for cluster in &paragraph.clusters {
                clusters.push(TextCluster {
                    range: positioned.start + cluster.range.start
                        ..positioned.start + cluster.range.end,
                    bounds: Bounds::new(
                        translation
                            + cluster.bounds.origin
                            + point(row_offset(cluster.bounds.top()), px(0.)),
                        cluster.bounds.size,
                    ),
                });
            }
            if positioned.start + paragraph.source.len()
                < self.paragraphs.last().map_or(0, |p| p.start)
            {
                clusters.push(TextCluster {
                    range: positioned.start + paragraph.source.len()
                        ..positioned.start + paragraph.source.len() + 1,
                    bounds: Bounds::new(
                        translation
                            + paragraph.carets.last().map_or_default(|caret| {
                                caret.position + point(row_offset(caret.position.y), px(0.))
                            }),
                        size(px(3.), line_height),
                    ),
                });
            }
        }
        InputGeometry {
            right_to_left: self.right_to_left,
            revision,
            line_height,
            viewport,
            scroll,
            carets,
            clusters,
        }
    }
}

/// Calculate local geometry once with the shaped paragraph. The GPUI convenience
/// index lookup chooses only one side of a wrap and scans the whole line on every
/// call; retaining both row endpoints also avoids quadratic work on long notes.
fn paragraph_geometry(
    shaped: &WrappedLine,
    boundaries: &[(usize, usize)],
    line_height: Pixels,
) -> (Vec<CaretStop>, Vec<TextCluster>) {
    let mut glyphs: Vec<_> = shaped
        .runs()
        .iter()
        .flat_map(|run| {
            run.glyphs
                .iter()
                .map(|glyph| (glyph.index, glyph.position.x))
        })
        .collect();
    // Native shapers may return runs in visual order. Resolve the source mapping
    // independently of that order and retain each cluster's first glyph origin.
    glyphs.sort_by_key(|glyph| glyph.0);
    glyphs.dedup_by_key(|glyph| glyph.0);
    glyphs.push((shaped.len(), shaped.unwrapped_layout.width));
    let mut positions = vec![px(0.); boundaries.len()];
    let mut boundary_index = 0;
    for pair in glyphs.windows(2) {
        let start = boundary_index;
        while boundary_index < boundaries.len() && boundaries[boundary_index].1 < pair[1].0 {
            boundary_index += 1;
        }
        let count = boundary_index - start;
        for (step, position) in positions[start..boundary_index].iter_mut().enumerate() {
            *position = pair[0].1 + (pair[1].1 - pair[0].1) * (step as f32 / count as f32);
        }
    }
    if let Some(position) = positions.last_mut() {
        *position = shaped.unwrapped_layout.width;
    }
    let mut rows = vec![(0, px(0.))];
    rows.extend(shaped.wrap_boundaries().iter().map(|boundary| {
        let glyph = &shaped.runs()[boundary.run_ix].glyphs[boundary.glyph_ix];
        (glyph.index, glyph.position.x)
    }));
    rows.push((shaped.len(), shaped.unwrapped_layout.width));
    let mut carets = Vec::with_capacity(boundaries.len() + rows.len() - 2);
    let mut clusters = Vec::with_capacity(boundaries.len().saturating_sub(1));
    for (row, pair) in rows.windows(2).enumerate() {
        let start = boundaries.partition_point(|(_, display)| *display < pair[0].0);
        let end = boundaries.partition_point(|(_, display)| *display <= pair[1].0);
        let y = line_height * row;
        for index in start..end {
            let position = point(positions[index] - pair[0].1, y);
            carets.push(CaretStop {
                offset: boundaries[index].0,
                position,
            });
            if index + 1 < end {
                let next_x = positions[index + 1] - pair[0].1;
                clusters.push(TextCluster {
                    range: boundaries[index].0..boundaries[index + 1].0,
                    bounds: Bounds::new(
                        point(position.x.min(next_x), y),
                        size((next_x - position.x).abs(), line_height),
                    ),
                });
            }
        }
    }
    (carets, clusters)
}
