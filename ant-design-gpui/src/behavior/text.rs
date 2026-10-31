// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native text wrapping with paragraph-level bidi glyph placement. Logical text
//! remains unchanged for accessibility and native shaping caches.

use std::{
    cell::{Cell, RefCell},
    rc::Rc,
};

use gpui::{
    AnyElement, App, AvailableSpace, Bounds, Element, ElementId, FontId, GlobalElementId,
    InspectorElementId, IntoElement, LayoutId, Pixels, Role, ShapedLine, SharedString, TextAlign,
    TextOverflow, TextRun, TextStyle, TruncateFrom, WhiteSpace, Window, accesskit, px, size,
};

use crate::{
    Direction,
    behavior::paragraph::{elide_rtl_inline_end, shape_wrapped_line},
};
use unicode_bidi::{BidiInfo, Level};
use unicode_linebreak::{BreakOpportunity, linebreaks};
use unicode_segmentation::UnicodeSegmentation;

/// Unrounded text advances for intrinsic table sizing. Native layout still
/// applies its usual device-pixel snapping to the actual paragraph element.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct TextIntrinsicMetrics {
    pub minimum: Pixels,
    pub preferred: Pixels,
    pub font_size: Pixels,
    pub scale: f32,
}

pub(crate) type TextIntrinsicHandle = Rc<Cell<Option<TextIntrinsicMetrics>>>;

#[cfg(test)]
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct CssLayoutStatistics {
    pub created: usize,
    pub reused: usize,
}

#[cfg(test)]
impl gpui::Global for CssLayoutStatistics {}

#[cfg(test)]
pub(crate) fn css_layout_statistics(cx: &App) -> CssLayoutStatistics {
    cx.try_global::<CssLayoutStatistics>()
        .copied()
        .unwrap_or_default()
}

/// Native paragraph text with explicit base direction and bounded layout caching.
/// Use a stable, distinct ID for each paragraph in the same parent.
pub fn paragraph_text(
    id: impl Into<ElementId>,
    text: impl Into<SharedString>,
    direction: Direction,
) -> AnyElement {
    ParagraphText {
        id: id.into(),
        text: text.into(),
        direction,
        logical_text: None,
        css_wrap: false,
        preserve_css_minimum: false,
        intrinsics: None,
    }
    .into_any_element()
}

/// CSS normal/nowrap text collapses ASCII whitespace before shaping. Keep the
/// original logical value on the same semantic element for accessibility.
pub(crate) fn normal_paragraph_text(text: SharedString, direction: Direction) -> AnyElement {
    ParagraphText {
        id: "paragraph-text".into(),
        text: collapse_css_whitespace(&text),
        direction,
        logical_text: Some(text),
        css_wrap: true,
        preserve_css_minimum: true,
        intrinsics: None,
    }
    .into_any_element()
}

/// Normal CSS whitespace with emergency wrapping for fixed-width notification cards.
pub(crate) fn break_word_paragraph_text(text: SharedString, direction: Direction) -> AnyElement {
    ParagraphText {
        id: "paragraph-text".into(),
        text: collapse_css_whitespace(&text),
        direction,
        logical_text: Some(text),
        css_wrap: false,
        preserve_css_minimum: true,
        intrinsics: None,
    }
    .into_any_element()
}

/// CSS `word-break: break-word` behaves as `overflow-wrap: anywhere`,
/// including emergency opportunities when calculating min-content widths.
pub(crate) fn anywhere_paragraph_text(text: SharedString, direction: Direction) -> AnyElement {
    ParagraphText {
        id: "paragraph-text".into(),
        text: collapse_css_whitespace(&text),
        direction,
        logical_text: Some(text),
        css_wrap: false,
        preserve_css_minimum: false,
        intrinsics: None,
    }
    .into_any_element()
}

/// A native paragraph that also publishes its raw CSS intrinsic advances for
/// Descriptions. The handle is filled during request_layout, after inheriting
/// the actual label/content font and style refinements.
pub(crate) fn measured_paragraph_text(
    text: SharedString,
    direction: Direction,
    anywhere: bool,
    handle: TextIntrinsicHandle,
) -> AnyElement {
    ParagraphText {
        id: "paragraph-text".into(),
        text: collapse_css_whitespace(&text),
        direction,
        logical_text: Some(text),
        css_wrap: !anywhere,
        preserve_css_minimum: !anywhere,
        intrinsics: Some(handle),
    }
    .into_any_element()
}

fn collapse_css_whitespace(text: &SharedString) -> SharedString {
    let whitespace = |byte| matches!(byte, b' ' | b'\t' | b'\n' | b'\r' | 0x0c);
    let mut previous_space = false;
    let needs_collapse = text.bytes().enumerate().any(|(index, byte)| {
        let space = whitespace(byte);
        let changed =
            space && (byte != b' ' || previous_space || index == 0 || index + 1 == text.len());
        previous_space = space;
        changed
    });
    if !needs_collapse {
        return text.clone();
    }
    let mut collapsed = String::with_capacity(text.len());
    let mut pending_space = false;
    for character in text.chars() {
        if matches!(character, ' ' | '\t' | '\n' | '\r' | '\u{000c}') {
            pending_space = !collapsed.is_empty();
        } else {
            if pending_space {
                collapsed.push(' ');
                pending_space = false;
            }
            collapsed.push(character);
        }
    }
    collapsed.into()
}

struct ParagraphText {
    id: ElementId,
    text: SharedString,
    direction: Direction,
    logical_text: Option<SharedString>,
    css_wrap: bool,
    preserve_css_minimum: bool,
    intrinsics: Option<TextIntrinsicHandle>,
}

enum TextLayout {
    Native(gpui::TextLayout),
    Paragraph(Rc<RefCell<Layout>>),
}

#[derive(PartialEq)]
struct LayoutKey {
    text: SharedString,
    direction: Direction,
    style: TextStyle,
    rem: Pixels,
    scale: f32,
    font: FontId,
    css_wrap: bool,
    emergency_wrap: bool,
}

impl LayoutKey {
    fn new(text: SharedString, direction: Direction, window: &Window) -> Self {
        let style = window.text_style();
        let font = window.text_system().resolve_font(&style.font());
        Self {
            text,
            direction,
            style,
            rem: window.rem_size(),
            scale: window.scale_factor(),
            font,
            css_wrap: false,
            emergency_wrap: false,
        }
    }
}

struct WidthLayout {
    constraint: Option<Pixels>,
    width: Pixels,
    lines: Rc<[ShapedLine]>,
}

fn css_line_ranges(
    text: &str,
    width: Pixels,
    emergency: bool,
    shape: impl Fn(std::ops::Range<usize>) -> ShapedLine,
) -> Vec<std::ops::Range<usize>> {
    let trimmed_end =
        |start: usize, end: usize| start + text[start..end].trim_end_matches(' ').len();
    let mut ranges = Vec::new();
    let mut start = 0;
    let mut previous = 0;
    for (end, opportunity) in linebreaks(text) {
        if previous > start && shape(start..trimmed_end(start, end)).width() > width {
            ranges.push(start..trimmed_end(start, previous));
            start = previous;
        }
        let segment_end = trimmed_end(start, end);
        if emergency && width > px(0.) && shape(start..segment_end).width() > width {
            let boundaries = text[start..segment_end]
                .grapheme_indices(true)
                .map(|(index, _)| start + index)
                .chain(std::iter::once(segment_end))
                .collect::<Vec<_>>();
            let mut first = 0;
            while first + 1 < boundaries.len()
                && shape(boundaries[first]..segment_end).width() > width
            {
                let mut low = first + 1;
                let mut high = boundaries.len() - 1;
                while low < high {
                    let middle = low + (high - low).div_ceil(2);
                    if shape(boundaries[first]..boundaries[middle]).width() <= width {
                        low = middle;
                    } else {
                        high = middle - 1;
                    }
                }
                if boundaries[low] == segment_end {
                    break;
                }
                ranges.push(boundaries[first]..boundaries[low]);
                first = low;
                start = boundaries[first];
            }
            // Keep the last fragment open: a following ordinary word may fit
            // beside it. Only an overlong fresh-line segment breaks at graphemes.
        }
        previous = end;
        if opportunity == BreakOpportunity::Mandatory {
            ranges.push(start..trimmed_end(start, end));
            start = end;
        }
    }
    ranges
}

impl IntoElement for ParagraphText {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

struct Layout {
    key: LayoutKey,
    text: SharedString,
    direction: Direction,
    font_size: Pixels,
    line_height: Pixels,
    run: TextRun,
    wrap: bool,
    max_lines: Option<usize>,
    overflow: Option<TextOverflow>,
    align: TextAlign,
    variants: Vec<WidthLayout>,
    lines: Rc<[ShapedLine]>,
    width: Pixels,
    css_minimum: Option<gpui::Size<Pixels>>,
    anywhere_minimum: Option<gpui::Size<Pixels>>,
    css_preferred: Option<Pixels>,
    last_request_width: Option<Pixels>,
    #[cfg(test)]
    compositions: usize,
    #[cfg(test)]
    minimum_compositions: usize,
    #[cfg(test)]
    anywhere_minimum_compositions: usize,
    #[cfg(test)]
    anywhere_full_bidi_analyses: usize,
    #[cfg(test)]
    anywhere_bidi_fallbacks: usize,
}

impl Layout {
    fn new(text: SharedString, direction: Direction, window: &Window) -> Self {
        let style = window.text_style();
        let font_size = style.font_size.to_pixels(window.rem_size());
        Self {
            key: LayoutKey::new(text.clone(), direction, window),
            run: style.to_run(text.len()),
            text,
            direction,
            font_size,
            // Follow stock GPUI TextLayout's device-pixel leading policy. A
            // logical-pixel round accumulates a different height per RTL line.
            line_height: window.pixel_snap(
                style
                    .line_height
                    .to_pixels(font_size.into(), window.rem_size()),
            ),
            wrap: style.white_space == WhiteSpace::Normal,
            max_lines: style.line_clamp,
            overflow: style.text_overflow,
            align: style.text_align,
            variants: Vec::with_capacity(4),
            lines: Rc::from([]),
            width: px(0.),
            css_minimum: None,
            anywhere_minimum: None,
            css_preferred: None,
            last_request_width: None,
            #[cfg(test)]
            compositions: 0,
            #[cfg(test)]
            minimum_compositions: 0,
            #[cfg(test)]
            anywhere_minimum_compositions: 0,
            #[cfg(test)]
            anywhere_full_bidi_analyses: 0,
            #[cfg(test)]
            anywhere_bidi_fallbacks: 0,
        }
    }

    /// `overflow-wrap: break-word` changes constrained wrapping without
    /// reducing the CSS min-content width to one glyph. Preserve that normal
    /// intrinsic minimum independently of the emergency-wrap width variants.
    fn css_minimum(&mut self, window: &Window) -> gpui::Size<Pixels> {
        if let Some(minimum) = self.css_minimum {
            return minimum;
        }
        #[cfg(test)]
        {
            self.minimum_compositions += 1;
        }
        let width = self.width;
        let lines = self.lines.clone();
        self.measure_css(Some(px(0.)), window);
        let minimum = size(self.width, self.line_height * self.lines.len());
        self.width = width;
        self.lines = lines;
        self.css_minimum = Some(minimum);
        minimum
    }

    fn css_preferred(&mut self, window: &Window) -> Pixels {
        if let Some(preferred) = self.css_preferred {
            return preferred;
        }
        let width = self.width;
        let lines = self.lines.clone();
        self.measure_css(None, window);
        let preferred = self.width;
        self.width = width;
        self.lines = lines;
        self.css_preferred = Some(preferred);
        preferred
    }

    fn intrinsic_grapheme_width(
        &self,
        start: usize,
        grapheme: &str,
        bidi: Option<&BidiInfo<'_>>,
        window: &Window,
    ) -> (Pixels, bool) {
        let end = start + grapheme.len();
        let direction = if let Some(bidi) = bidi {
            let level = bidi.levels[start];
            let uniform = bidi.levels[start..end].iter().all(|value| *value == level);
            // Line-end whitespace resets and explicit embedding/isolate
            // controls require the original line algorithm. These rare
            // graphemes retain its exact behavior; ordinary text uses only
            // its already-resolved level and a small isolated shape.
            let line_sensitive = grapheme.chars().any(|character| {
                character.is_whitespace()
                    || matches!(character, '\u{061c}' | '\u{200e}' | '\u{200f}' | '\u{202a}'..='\u{202e}' | '\u{2066}'..='\u{2069}')
            });
            if !uniform || line_sensitive {
                return (
                    shape_wrapped_line(
                        self.text.clone(),
                        start..end,
                        self.direction,
                        self.font_size,
                        &self.run,
                        window,
                    )
                    .width(),
                    true,
                );
            }
            if level.is_rtl() {
                Direction::Rtl
            } else {
                Direction::Ltr
            }
        } else {
            Direction::Ltr
        };
        let local: SharedString = grapheme.to_owned().into();
        (
            shape_wrapped_line(
                local,
                0..grapheme.len(),
                direction,
                self.font_size,
                &self.run,
                window,
            )
            .width(),
            false,
        )
    }

    fn anywhere_minimum(&mut self, window: &Window) -> gpui::Size<Pixels> {
        if let Some(minimum) = self.anywhere_minimum {
            return minimum;
        }
        #[cfg(test)]
        {
            self.anywhere_minimum_compositions += 1;
        }
        let minimum = if self.text.is_empty() {
            size(px(0.), px(0.))
        } else if !self.wrap {
            let full = shape_wrapped_line(
                self.text.clone(),
                0..self.text.len(),
                self.direction,
                self.font_size,
                &self.run,
                window,
            );
            size(full.width(), self.line_height)
        } else {
            // Resolve the complete paragraph once, rather than reparsing all
            // its bidi levels for every grapheme during cold intrinsic sizing.
            // Final line shaping and paint still use the full paragraph path.
            let bidi = if self.direction == Direction::Ltr && self.text.is_ascii() {
                None
            } else {
                Some(BidiInfo::new(
                    &self.text,
                    Some(if self.direction == Direction::Rtl {
                        Level::rtl()
                    } else {
                        Level::ltr()
                    }),
                ))
            };
            #[cfg(test)]
            let fallback_count = Cell::new(0usize);
            let width = self
                .text
                .grapheme_indices(true)
                .filter(|(_, grapheme)| *grapheme != " ")
                .map(|(start, grapheme)| {
                    let measurement =
                        self.intrinsic_grapheme_width(start, grapheme, bidi.as_ref(), window);
                    #[cfg(test)]
                    fallback_count.set(fallback_count.get() + usize::from(measurement.1));
                    measurement.0
                })
                .max_by(|left, right| f32::from(*left).total_cmp(&f32::from(*right)))
                .unwrap_or(px(0.));
            #[cfg(test)]
            {
                self.anywhere_bidi_fallbacks += fallback_count.get();
                self.anywhere_full_bidi_analyses +=
                    usize::from(bidi.is_some()) + fallback_count.get();
            }
            let lines = css_line_ranges(&self.text, width, true, |range| {
                shape_wrapped_line(
                    self.text.clone(),
                    range,
                    self.direction,
                    self.font_size,
                    &self.run,
                    window,
                )
            })
            .len();
            size(
                width,
                self.line_height
                    * self
                        .max_lines
                        .map_or(lines, |maximum| lines.min(maximum.max(1))),
            )
        };
        self.anywhere_minimum = Some(minimum);
        minimum
    }

    fn measure(&mut self, width: Option<Pixels>, window: &Window) {
        if let Some(cached) = self
            .variants
            .iter()
            .find(|cached| cached.constraint == width)
        {
            self.width = cached.width;
            self.lines = cached.lines.clone();
            return;
        }
        #[cfg(test)]
        {
            self.compositions += 1;
        }
        if self.key.css_wrap {
            self.measure_css(width, window);
            self.cache_width(width);
            return;
        }
        let mut lines = Vec::new();
        self.width = px(0.);
        let mut run = self.run.clone();
        let mut display = self.text.clone();
        if let Some(width) = width {
            let (affix, from) = match &self.overflow {
                Some(TextOverflow::Truncate(affix)) => (affix.as_ref(), TruncateFrom::End),
                Some(TextOverflow::TruncateStart(affix)) => (affix.as_ref(), TruncateFrom::Start),
                Some(TextOverflow::TruncateMiddle(affix)) => (affix.as_ref(), TruncateFrom::Middle),
                None => ("", TruncateFrom::End),
            };
            let mut wrapper = window
                .text_system()
                .line_wrapper(run.font.clone(), self.font_size);
            if let Some(maximum) = self.max_lines {
                // Truncate logical text before shaping with a bounded line
                // budget. GPUI's wrapped-line cache omits max_lines from its
                // key; shaping this final display with max_lines=None avoids
                // poisoning/reusing an unlimited layout for a clamped label.
                display = wrapper
                    .truncate_wrapped_line(
                        display,
                        width,
                        maximum.max(1),
                        affix,
                        std::slice::from_ref(&run),
                        from,
                    )
                    .0;
            } else if self.overflow.is_some() {
                let fits = window
                    .text_system()
                    .shape_line(
                        display.clone(),
                        self.font_size,
                        std::slice::from_ref(&run),
                        None,
                    )
                    .width()
                    <= width;
                if !fits {
                    display = wrapper
                        .truncate_line(display, width, affix, std::slice::from_ref(&run), from)
                        .0;
                }
            }
            run.len = display.len();
        }
        let Ok(paragraphs) = window.text_system().shape_text(
            display,
            self.font_size,
            std::slice::from_ref(&run),
            width.filter(|_| self.wrap),
            None,
        ) else {
            self.lines = Rc::from([]);
            return;
        };
        for paragraph in paragraphs {
            // Keep GPUI's measured wrap width, even when the longest visual
            // fragment is shorter. Otherwise intrinsic sizing can shrink the
            // box and introduce extra lines only during prepaint.
            self.width = self.width.max(paragraph.width()).ceil();
            let ends = paragraph
                .wrap_boundaries
                .iter()
                .map(|boundary| {
                    paragraph.unwrapped_layout.runs[boundary.run_ix].glyphs[boundary.glyph_ix].index
                })
                .chain(std::iter::once(paragraph.text.len()));
            let mut start = 0;
            for end in ends {
                if end < start || !paragraph.text.is_char_boundary(end) {
                    continue;
                }
                let raw = &paragraph.text[start..end];
                let end = start + raw.trim_end_matches([' ', '\t', '\r']).len();
                let line = shape_wrapped_line(
                    paragraph.text.clone(),
                    start..end,
                    self.direction,
                    self.font_size,
                    &run,
                    window,
                );
                lines.push(line);
                // The next line retains the full paragraph embedding context,
                // including whitespace skipped only for visual line painting.
                start += raw.len();
            }
        }
        if let Some(maximum) = self.max_lines {
            lines.truncate(maximum.max(1));
        }
        self.lines = Rc::from(lines);
        self.cache_width(width);
    }

    fn measure_for_bounds(&mut self, width: Pixels, window: &Window) {
        // Layout's final absolute edges snap independently to device pixels.
        // Keep the exact finite width that produced this CSS paragraph's row
        // height when the bounds merely reflect that native snap. This is not
        // extra fitting space: glyph decisions use the original constraint.
        let constraint = self
            .last_request_width
            .filter(|requested| {
                self.key.css_wrap
                    && (f32::from(*requested - width)).abs() <= 1. / window.scale_factor()
            })
            .unwrap_or(width);
        self.measure(Some(constraint), window);
    }

    fn cache_width(&mut self, width: Option<Pixels>) {
        if self.variants.len() == 4 {
            self.variants.remove(0);
        }
        self.variants.push(WidthLayout {
            constraint: width,
            width: self.width,
            lines: self.lines.clone(),
        });
    }

    /// CSS normal wrapping permits overflow inside an unbreakable segment.
    /// Native glyph shaping and bidi placement remain shared with other labels;
    /// UAX #14 supplies legal opportunities instead of emergency character breaks.
    fn measure_css(&mut self, width: Option<Pixels>, window: &Window) {
        if self.text.is_empty() {
            self.width = px(0.);
            self.lines = Rc::from([]);
            return;
        }
        if self.direction == Direction::Rtl
            && !self.wrap
            && self.max_lines.is_none()
            && let Some(TextOverflow::Truncate(affix)) = &self.overflow
            && let Some(width) = width
        {
            let full = shape_wrapped_line(
                self.text.clone(),
                0..self.text.len(),
                self.direction,
                self.font_size,
                &self.run,
                window,
            );
            let line = if full.width() > width {
                let marker = shape_wrapped_line(
                    affix.clone(),
                    0..affix.len(),
                    self.direction,
                    self.font_size,
                    &self.run,
                    window,
                );
                elide_rtl_inline_end(&full, &marker, width)
            } else {
                full
            };
            self.width = line.width();
            self.lines = Rc::from([line]);
            return;
        }
        let mut display = self.text.clone();
        let mut run = self.run.clone();
        let (affix, from) = match &self.overflow {
            Some(TextOverflow::Truncate(affix)) => (affix.as_ref(), TruncateFrom::End),
            Some(TextOverflow::TruncateStart(affix)) => (affix.as_ref(), TruncateFrom::Start),
            Some(TextOverflow::TruncateMiddle(affix)) => (affix.as_ref(), TruncateFrom::Middle),
            None => ("", TruncateFrom::End),
        };
        if self.max_lines.is_none()
            && self.overflow.is_some()
            && let Some(width) = width
        {
            let mut wrapper = window
                .text_system()
                .line_wrapper(run.font.clone(), self.font_size);
            display = wrapper
                .truncate_line(display, width, affix, std::slice::from_ref(&run), from)
                .0;
            run.len = display.len();
        }
        let shape = |range: std::ops::Range<usize>| {
            shape_wrapped_line(
                display.clone(),
                range,
                self.direction,
                self.font_size,
                &run,
                window,
            )
        };
        let full = shape(0..display.len());
        if !self.wrap || width.is_none_or(|width| full.width() <= width) {
            self.width = full.width();
            self.lines = Rc::from([full]);
            return;
        }
        let width = width.expect("constrained wrapping");
        let ranges = css_line_ranges(&display, width, self.key.emergency_wrap, shape);
        let mut lines = Vec::with_capacity(ranges.len());
        let maximum = self.max_lines.map(|maximum| maximum.max(1));
        for (index, range) in ranges.iter().enumerate() {
            if maximum.is_some_and(|maximum| index + 1 == maximum && ranges.len() > maximum) {
                let tail: SharedString = display[range.start..].to_owned().into();
                let mut tail_run = run.clone();
                tail_run.len = tail.len();
                let mut wrapper = window
                    .text_system()
                    .line_wrapper(run.font.clone(), self.font_size);
                let truncated = wrapper
                    .truncate_line(tail, width, affix, std::slice::from_ref(&tail_run), from)
                    .0;
                tail_run.len = truncated.len();
                lines.push(shape_wrapped_line(
                    truncated.clone(),
                    0..truncated.len(),
                    self.direction,
                    self.font_size,
                    &tail_run,
                    window,
                ));
                break;
            }
            lines.push(shape(range.clone()));
        }
        self.width = if width > px(0.) {
            width
        } else {
            lines
                .iter()
                .map(|line| line.width())
                .max_by(|a, b| f32::from(*a).total_cmp(&f32::from(*b)))
                .unwrap_or(px(0.))
        };
        self.lines = Rc::from(lines);
    }
}

fn reuse_layout(
    previous: Option<Rc<RefCell<Layout>>>,
    text: SharedString,
    direction: Direction,
    window: &Window,
) -> Rc<RefCell<Layout>> {
    let key = LayoutKey::new(text.clone(), direction, window);
    previous
        .filter(|previous| previous.borrow().key == key)
        .unwrap_or_else(|| Rc::new(RefCell::new(Layout::new(text, direction, window))))
}

fn reuse_css_layout(
    previous: Option<Rc<RefCell<Layout>>>,
    text: SharedString,
    direction: Direction,
    window: &Window,
) -> Rc<RefCell<Layout>> {
    reuse_css_layout_with_emergency(previous, text, direction, false, window)
}

fn reuse_css_layout_with_emergency(
    previous: Option<Rc<RefCell<Layout>>>,
    text: SharedString,
    direction: Direction,
    emergency_wrap: bool,
    window: &Window,
) -> Rc<RefCell<Layout>> {
    let mut key = LayoutKey::new(text.clone(), direction, window);
    key.css_wrap = true;
    key.emergency_wrap = emergency_wrap;
    previous
        .filter(|previous| previous.borrow().key == key)
        .unwrap_or_else(|| {
            let mut layout = Layout::new(text, direction, window);
            layout.key.css_wrap = true;
            layout.key.emergency_wrap = emergency_wrap;
            Rc::new(RefCell::new(layout))
        })
}

impl Element for ParagraphText {
    type RequestLayoutState = TextLayout;
    type PrepaintState = ();

    fn id(&self) -> Option<ElementId> {
        Some(self.id.clone())
    }
    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        None
    }
    fn a11y_role(&self) -> Option<Role> {
        Some(Role::Label)
    }
    fn write_a11y_info(&self, node: &mut accesskit::Node) {
        node.set_value(self.logical_text.as_ref().unwrap_or(&self.text).to_string());
    }

    fn request_layout(
        &mut self,
        global_id: Option<&GlobalElementId>,
        inspector: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, Self::RequestLayoutState) {
        let style = window.text_style();
        if self.logical_text.is_none()
            && self.direction == Direction::Ltr
            && self.text.is_ascii()
            && style.text_overflow.is_none()
            && style.line_clamp.is_none()
        {
            let (id, state) = <SharedString as Element>::request_layout(
                &mut self.text,
                global_id,
                inspector,
                window,
                cx,
            );
            return (id, TextLayout::Native(state));
        }
        #[cfg(test)]
        let created = std::cell::Cell::new(false);
        let layout = window.with_element_state(
            global_id.expect("Paragraph text has a stable local ID"),
            |previous, window| {
                #[cfg(test)]
                let prior = previous.clone();
                let layout = if self.css_wrap {
                    reuse_css_layout(previous, self.text.clone(), self.direction, window)
                } else if self.logical_text.is_some() {
                    reuse_css_layout_with_emergency(
                        previous,
                        self.text.clone(),
                        self.direction,
                        true,
                        window,
                    )
                } else {
                    reuse_layout(previous, self.text.clone(), self.direction, window)
                };
                #[cfg(test)]
                created.set(
                    prior
                        .as_ref()
                        .is_none_or(|prior| !Rc::ptr_eq(prior, &layout)),
                );
                (layout.clone(), layout)
            },
        );
        layout.borrow_mut().last_request_width = None;
        #[cfg(test)]
        if self.logical_text.is_some() {
            if cx.try_global::<CssLayoutStatistics>().is_none() {
                cx.set_global(CssLayoutStatistics::default());
            }
            let stats = cx.global_mut::<CssLayoutStatistics>();
            if created.get() {
                stats.created += 1;
            } else {
                stats.reused += 1;
            }
        }
        if let Some(handle) = &self.intrinsics {
            let mut layout = layout.borrow_mut();
            let minimum = if self.preserve_css_minimum {
                layout.css_minimum(window).width
            } else {
                layout.anywhere_minimum(window).width
            };
            let preferred = layout.css_preferred(window);
            handle.set(Some(TextIntrinsicMetrics {
                minimum,
                preferred,
                font_size: layout.font_size,
                scale: window.scale_factor(),
            }));
        }
        let measured = layout.clone();
        let preserve_css_minimum = self.preserve_css_minimum && !self.css_wrap;
        let preserve_anywhere_minimum =
            self.logical_text.is_some() && !self.preserve_css_minimum && !self.css_wrap;
        let id = window.request_measured_layout(
            Default::default(),
            move |known, available, window, _| {
                let mut layout = measured.borrow_mut();
                let finite_width = known.width.or(match available.width {
                    AvailableSpace::Definite(width) => Some(width),
                    _ => None,
                });
                if let Some(width) =
                    finite_width.filter(|width| f32::from(*width).is_finite() && *width >= px(0.))
                {
                    layout.last_request_width = Some(width);
                }
                if preserve_css_minimum
                    && known.width.is_none()
                    && matches!(available.width, AvailableSpace::MinContent)
                {
                    let minimum = layout.css_minimum(window);
                    return size(minimum.width, known.height.unwrap_or(minimum.height));
                }
                if preserve_anywhere_minimum
                    && known.width.is_none()
                    && matches!(available.width, AvailableSpace::MinContent)
                {
                    let minimum = layout.anywhere_minimum(window);
                    return size(minimum.width, known.height.unwrap_or(minimum.height));
                }
                let width = known.width.or(match available.width {
                    AvailableSpace::Definite(width) => Some(width),
                    AvailableSpace::MinContent => Some(px(0.)),
                    _ => None,
                });
                layout.measure(width, window);
                size(
                    known.width.unwrap_or(layout.width),
                    known
                        .height
                        .unwrap_or(layout.line_height * layout.lines.len()),
                )
            },
        );
        (id, TextLayout::Paragraph(layout))
    }

    fn prepaint(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        layout: &mut Self::RequestLayoutState,
        window: &mut Window,
        cx: &mut App,
    ) {
        match layout {
            TextLayout::Native(layout) => <SharedString as Element>::prepaint(
                &mut self.text,
                id,
                inspector,
                bounds,
                layout,
                window,
                cx,
            ),
            TextLayout::Paragraph(layout) => layout
                .borrow_mut()
                .measure_for_bounds(bounds.size.width, window),
        }
    }

    fn paint(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        layout: &mut Self::RequestLayoutState,
        prepaint: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) {
        let layout = match layout {
            TextLayout::Native(layout) => {
                <SharedString as Element>::paint(
                    &mut self.text,
                    id,
                    inspector,
                    bounds,
                    layout,
                    prepaint,
                    window,
                    cx,
                );
                return;
            }
            TextLayout::Paragraph(layout) => layout,
        };
        let layout = layout.borrow();
        let mut origin = bounds.origin;
        for line in layout.lines.iter() {
            // CSS expands an unbreakable line toward the paragraph's inline
            // end. Its requested centering applies only while the line fits.
            let align = if layout.key.css_wrap && line.width() > bounds.size.width {
                match layout.direction {
                    Direction::Ltr => TextAlign::Left,
                    Direction::Rtl => TextAlign::Right,
                }
            } else {
                layout.align
            };
            let _ = line.paint(
                origin,
                layout.line_height,
                align,
                Some(bounds.size.width),
                window,
                cx,
            );
            origin.y += layout.line_height;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use gpui::{Context, Render, Styled, TestAppContext, VisualTestContext, div, prelude::*};

    type ProbeCallback = Rc<dyn Fn(&mut Window, &mut App)>;

    struct MeasurementProbe {
        run: ProbeCallback,
        ran: Rc<std::cell::Cell<bool>>,
    }

    impl Render for MeasurementProbe {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            MeasurementProbe {
                run: self.run.clone(),
                ran: self.ran.clone(),
            }
        }
    }

    impl IntoElement for MeasurementProbe {
        type Element = Self;
        fn into_element(self) -> Self {
            self
        }
    }

    impl Element for MeasurementProbe {
        type RequestLayoutState = ();
        type PrepaintState = ();
        fn id(&self) -> Option<ElementId> {
            Some("descriptions-measurements".into())
        }
        fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
            None
        }
        fn request_layout(
            &mut self,
            _: Option<&GlobalElementId>,
            _: Option<&InspectorElementId>,
            window: &mut Window,
            cx: &mut App,
        ) -> (LayoutId, ()) {
            if !self.ran.replace(true) {
                (self.run)(window, cx);
            }
            (window.request_layout(gpui::Style::default(), [], cx), ())
        }
        fn prepaint(
            &mut self,
            _: Option<&GlobalElementId>,
            _: Option<&InspectorElementId>,
            _: Bounds<Pixels>,
            _: &mut (),
            _: &mut Window,
            _: &mut App,
        ) {
        }
        fn paint(
            &mut self,
            _: Option<&GlobalElementId>,
            _: Option<&InspectorElementId>,
            _: Bounds<Pixels>,
            _: &mut (),
            _: &mut (),
            _: &mut Window,
            _: &mut App,
        ) {
        }
    }

    fn during_layout(run: impl Fn(&mut Window, &mut App) + 'static) {
        let mut cx = TestAppContext::single();
        let ran = Rc::new(std::cell::Cell::new(false));
        let observed = ran.clone();
        let window = cx.add_window(move |_, _| MeasurementProbe {
            run: Rc::new(run),
            ran,
        });
        let mut visual = VisualTestContext::from_window(window.into(), &cx);
        visual.update(|window, cx| window.draw(cx).clear(cx));
        assert!(
            observed.get(),
            "measurement probe ran in real request_layout"
        );
    }

    #[test]
    fn descriptions_break_word_keeps_css_minimum_and_wraps_at_definite_width() {
        during_layout(|window, cx| {
            window.with_text_style(
                Some(gpui::TextStyleRefinement {
                    font_size: Some(px(14.).into()),
                    line_height: Some(px(22.).into()),
                    ..Default::default()
                }),
                |window| {
                    let word: SharedString = "LongUnbreakableIdentifier".into();
                    let mut expected = Layout::new(word, Direction::Ltr, window);
                    expected.measure(None, window);
                    let mut element = break_word_paragraph_text(
                        "Short LongUnbreakableIdentifier end".into(),
                        Direction::Ltr,
                    );
                    let minimum = element.layout_as_root(
                        size(AvailableSpace::MinContent, AvailableSpace::MaxContent),
                        window,
                        cx,
                    );
                    assert!((f32::from(minimum.width - expected.width)).abs() < 0.01);
                    assert!(
                        minimum.width > px(100.),
                        "min-content must retain the whole unbreakable word"
                    );
                    let wide = element.layout_as_root(
                        size(AvailableSpace::MaxContent, AvailableSpace::MaxContent),
                        window,
                        cx,
                    );
                    let narrow = element.layout_as_root(
                        size(
                            AvailableSpace::Definite(px(40.)),
                            AvailableSpace::MaxContent,
                        ),
                        window,
                        cx,
                    );
                    assert!(wide.width > minimum.width);
                    assert!(narrow.height > wide.height);
                    assert!(narrow.width <= px(40.));
                    let mut anywhere = anywhere_paragraph_text(
                        "Short LongUnbreakableIdentifier end".into(),
                        Direction::Ltr,
                    );
                    anywhere.downcast_mut::<ParagraphText>().unwrap().id =
                        "anywhere-minimum".into();
                    let anywhere_minimum = anywhere.layout_as_root(
                        size(AvailableSpace::MinContent, AvailableSpace::MaxContent),
                        window,
                        cx,
                    );
                    assert!(
                        anywhere_minimum.width > px(0.) && anywhere_minimum.width < px(40.),
                        "word-break: break-word includes emergency opportunities in min-content"
                    );
                    assert!(anywhere_minimum.width < minimum.width);
                },
            );
        });
    }

    #[test]
    fn descriptions_css_minimum_cache_preserves_emergency_wrap_variants() {
        during_layout(|window, _| {
            let text: SharedString = "Short LongUnbreakableIdentifier end".into();
            let layout = reuse_layout(None, text.clone(), Direction::Ltr, window);
            layout.borrow_mut().measure(Some(px(40.)), window);
            let previous_lines = layout.borrow().lines.clone();
            let previous_width = layout.borrow().width;
            let minimum = layout.borrow_mut().css_minimum(window);
            assert!(minimum.width > px(40.));
            assert_eq!(layout.borrow().width, previous_width);
            assert!(Rc::ptr_eq(&layout.borrow().lines, &previous_lines));
            assert_eq!(layout.borrow().variants.len(), 1);
            let warm = reuse_layout(Some(layout.clone()), text.clone(), Direction::Ltr, window);
            assert!(Rc::ptr_eq(&warm, &layout));
            assert_eq!(warm.borrow_mut().css_minimum(window), minimum);
            warm.borrow_mut().measure(Some(px(40.)), window);
            assert_eq!(warm.borrow().minimum_compositions, 1);
            assert_eq!(warm.borrow().compositions, 1);
            assert!(!Rc::ptr_eq(
                &warm,
                &reuse_layout(Some(warm.clone()), text, Direction::Rtl, window)
            ));
            assert!(!Rc::ptr_eq(
                &warm,
                &reuse_layout(Some(warm.clone()), "Changed".into(), Direction::Ltr, window)
            ));
        });
    }

    #[test]
    fn descriptions_anywhere_minimum_uses_widest_grapheme_and_retains_warm_layout() {
        during_layout(|window, _| {
            let text: SharedString = "LongUnbreakableIdentifier".into();
            let layout = reuse_layout(None, text.clone(), Direction::Ltr, window);
            layout.borrow_mut().measure(Some(px(40.)), window);
            let previous_lines = layout.borrow().lines.clone();
            let previous_width = layout.borrow().width;
            let expected = text
                .grapheme_indices(true)
                .map(|(start, grapheme)| {
                    let layout = layout.borrow();
                    shape_wrapped_line(
                        text.clone(),
                        start..start + grapheme.len(),
                        Direction::Ltr,
                        layout.font_size,
                        &layout.run,
                        window,
                    )
                    .width()
                })
                .max_by(|left, right| f32::from(*left).total_cmp(&f32::from(*right)))
                .unwrap();
            let minimum = layout.borrow_mut().anywhere_minimum(window);
            assert_eq!(minimum.width, expected);
            assert!(minimum.width > px(0.) && minimum.width < px(40.));
            assert!(minimum.height >= layout.borrow().line_height);
            assert_eq!(layout.borrow().width, previous_width);
            assert!(Rc::ptr_eq(&previous_lines, &layout.borrow().lines));
            assert_eq!(layout.borrow().variants.len(), 1);
            assert_eq!(layout.borrow_mut().anywhere_minimum(window), minimum);
            layout.borrow_mut().measure(Some(px(40.)), window);
            assert_eq!(layout.borrow().anywhere_minimum_compositions, 1);
            assert_eq!(layout.borrow().compositions, 1);

            // A joined emoji remains one typographic grapheme in both base
            // directions; intrinsic sizing must never split it into codepoints.
            for direction in [Direction::Ltr, Direction::Rtl] {
                let family: SharedString = "👩‍👩‍👧‍👦".into();
                let mut layout = Layout::new(family.clone(), direction, window);
                let expected = shape_wrapped_line(
                    family.clone(),
                    0..family.len(),
                    direction,
                    layout.font_size,
                    &layout.run,
                    window,
                )
                .width();
                assert_eq!(layout.anywhere_minimum(window).width, expected);
                assert_eq!(layout.anywhere_minimum_compositions, 1);
            }
            window.with_text_style(
                Some(gpui::TextStyleRefinement {
                    white_space: Some(WhiteSpace::Nowrap),
                    ..Default::default()
                }),
                |window| {
                    let mut layout = Layout::new(text.clone(), Direction::Ltr, window);
                    let full = shape_wrapped_line(
                        text.clone(),
                        0..text.len(),
                        Direction::Ltr,
                        layout.font_size,
                        &layout.run,
                        window,
                    )
                    .width();
                    assert_eq!(layout.anywhere_minimum(window).width, full);
                },
            );
            let mut empty = Layout::new("".into(), Direction::Ltr, window);
            assert_eq!(empty.anywhere_minimum(window), size(px(0.), px(0.)));
        });
    }

    #[test]
    fn descriptions_intrinsic_bidi_analysis_is_linear_and_matches_contextual_baseline() {
        during_layout(|window, _| {
            for text in [
                "Account مرحبا بالعالم / אבג 123 世界 (ready) 👩‍💻",
                "אבג (abc) [١٢٣] {مرحبا} — 中文 👨‍👩‍👧‍👦",
                "A\u{0301} ב\u{05b0} שָׁלוֹם؟ «مرحبا»，漢字🙂",
            ] {
                for direction in [Direction::Ltr, Direction::Rtl] {
                    let mut layout = Layout::new(text.into(), direction, window);
                    let bidi = BidiInfo::new(
                        text,
                        Some(if direction == Direction::Rtl {
                            Level::rtl()
                        } else {
                            Level::ltr()
                        }),
                    );
                    let mut expected = px(0.);
                    for (start, grapheme) in text
                        .grapheme_indices(true)
                        .filter(|(_, grapheme)| *grapheme != " ")
                    {
                        let baseline = shape_wrapped_line(
                            layout.text.clone(),
                            start..start + grapheme.len(),
                            direction,
                            layout.font_size,
                            &layout.run,
                            window,
                        )
                        .width();
                        let optimized =
                            layout.intrinsic_grapheme_width(start, grapheme, Some(&bidi), window);
                        assert_eq!(
                            optimized.0, baseline,
                            "{direction:?}: {grapheme:?} in {text:?}"
                        );
                        assert!(
                            !optimized.1,
                            "ordinary mixed text must use its resolved level directly"
                        );
                        expected = expected.max(baseline);
                    }
                    assert_eq!(layout.anywhere_minimum(window).width, expected);
                    assert_eq!(layout.anywhere_full_bidi_analyses, 1);
                    assert_eq!(layout.anywhere_bidi_fallbacks, 0);
                    layout.anywhere_minimum(window);
                    assert_eq!(
                        layout.anywhere_full_bidi_analyses, 1,
                        "warm minima perform no additional analysis"
                    );
                }
            }
            // Explicit directional controls and unusual line-end whitespace
            // retain the contextual baseline, including its rare extra parse.
            let text: SharedString = "\u{2067}אבג\u{2069} A\u{00a0}} \u{202b}مرحبا\u{202c}".into();
            let mut layout = Layout::new(text.clone(), Direction::Rtl, window);
            let expected = text
                .grapheme_indices(true)
                .filter(|(_, grapheme)| *grapheme != " ")
                .map(|(start, grapheme)| {
                    shape_wrapped_line(
                        text.clone(),
                        start..start + grapheme.len(),
                        Direction::Rtl,
                        layout.font_size,
                        &layout.run,
                        window,
                    )
                    .width()
                })
                .max_by(|left, right| f32::from(*left).total_cmp(&f32::from(*right)))
                .unwrap();
            assert_eq!(layout.anywhere_minimum(window).width, expected);
            assert!(layout.anywhere_bidi_fallbacks > 0);
            assert_eq!(
                layout.anywhere_full_bidi_analyses,
                1 + layout.anywhere_bidi_fallbacks
            );
            let analyses = layout.anywhere_full_bidi_analyses;
            layout.anywhere_minimum(window);
            assert_eq!(layout.anywhere_full_bidi_analyses, analyses);
        });
    }

    #[test]
    fn descriptions_intrinsic_handles_publish_raw_advances_before_native_leaf_snapping() {
        during_layout(|window, cx| {
            window.with_text_style(Some(gpui::TextStyleRefinement {
                font_size: Some(px(13.7).into()), line_height: Some(px(22.).into()),
                ..Default::default()
            }), |window| {
                for anywhere in [false, true] {
                    let text: SharedString = "Label with text".into();
                    let handle: TextIntrinsicHandle = Rc::default();
                    let mut paragraph = measured_paragraph_text(text.clone(), Direction::Ltr, anywhere, handle.clone());
                    paragraph.downcast_mut::<ParagraphText>().unwrap().id = ("raw-intrinsic", usize::from(anywhere)).into();
                    assert!(handle.get().is_none());
                    let leaf_minimum = paragraph.layout_as_root(
                        size(AvailableSpace::MinContent, AvailableSpace::MaxContent), window, cx,
                    );
                    let metrics = handle.get().expect("actual request_layout publishes intrinsic metrics");
                    let mut expected = Layout::new(text.clone(), Direction::Ltr, window);
                    let minimum = if anywhere { expected.anywhere_minimum(window).width } else { expected.css_minimum(window).width };
                    let preferred = shape_wrapped_line(text.clone(), 0..text.len(), Direction::Ltr,
                        expected.font_size, &expected.run, window).width();
                    assert_eq!(metrics.minimum, minimum);
                    assert_eq!(metrics.preferred, preferred);
                    assert_eq!(metrics.font_size, px(13.7));
                    assert_eq!(metrics.scale, window.scale_factor());
                    let snapped_minimum = px((f32::from(minimum) * metrics.scale).ceil() / metrics.scale);
                    assert_eq!(leaf_minimum.width, snapped_minimum);
                    assert!(metrics.minimum < leaf_minimum.width,
                        "raw advances must retain the fractional amount lost by measured-leaf snapping");
                    paragraph.layout_as_root(size(AvailableSpace::Definite(px(40.)), AvailableSpace::MaxContent), window, cx);
                    assert_eq!(handle.get(), Some(metrics));

                    expected.measure(Some(px(40.)), window);
                    let lines = expected.lines.clone();
                    let width = expected.width;
                    let variants = expected.variants.len();
                    assert_eq!(expected.css_preferred(window), preferred);
                    assert_eq!(expected.css_preferred(window), preferred);
                    assert_eq!(expected.width, width);
                    assert!(Rc::ptr_eq(&expected.lines, &lines));
                    assert_eq!(expected.variants.len(), variants);
                }
            });
        });
    }

    fn shaped_glyphs(line: &ShapedLine) -> Vec<(FontId, gpui::ShapedGlyph)> {
        line.runs
            .iter()
            .flat_map(|run| {
                run.glyphs
                    .iter()
                    .cloned()
                    .map(move |glyph| (run.font_id, glyph))
            })
            .collect()
    }

    fn assert_rtl_elision_geometry(
        full: &ShapedLine,
        marker: &ShapedLine,
        elided: &ShapedLine,
        width: Pixels,
    ) {
        assert_eq!(
            elided.text, full.text,
            "elision does not rewrite the logical source"
        );
        assert_eq!(elided.width(), width);
        let glyphs = shaped_glyphs(elided);
        let marker_glyphs = shaped_glyphs(marker);
        let original = shaped_glyphs(full);
        assert!(glyphs.len() >= marker_glyphs.len());
        let retained = &glyphs[marker_glyphs.len()..];
        assert!(!retained.is_empty());
        let retained_left = retained
            .iter()
            .map(|(_, glyph)| glyph.position.x)
            .min_by(|left, right| f32::from(*left).total_cmp(&f32::from(*right)))
            .unwrap();
        let marker_offset = retained_left - marker.width();
        assert!(
            marker_offset >= px(0.),
            "the marker stays inside the available box"
        );
        for ((font, glyph), (expected_font, expected)) in glyphs.iter().zip(&marker_glyphs) {
            assert_eq!(font, expected_font);
            assert_eq!(glyph.id, expected.id);
            assert_eq!(
                glyph.position,
                gpui::point(expected.position.x + marker_offset, expected.position.y),
                "the marker touches the retained visual text without a grapheme-sized gap"
            );
        }
        let offset = full.width() - width;
        for (font, glyph) in retained {
            assert!(
                original.iter().any(|(original_font, source)| {
                    original_font == font
                        && source.id == glyph.id
                        && source.index == glyph.index
                        && (f32::from(source.position.x - offset - glyph.position.x)).abs() < 0.001
                        && source.position.y == glyph.position.y
                }),
                "retained glyphs preserve full-paragraph shaping and only translate horizontally"
            );
        }
        for (start, grapheme) in full.text.grapheme_indices(true) {
            let in_grapheme = |glyph: &gpui::ShapedGlyph| {
                glyph.index >= start && glyph.index < start + grapheme.len()
            };
            let total = original
                .iter()
                .filter(|(_, glyph)| in_grapheme(glyph))
                .count();
            let kept = retained
                .iter()
                .filter(|(_, glyph)| in_grapheme(glyph))
                .count();
            assert!(
                kept == 0 || kept == total,
                "never retain only part of a grapheme"
            );
        }
        let rightmost = original
            .iter()
            .max_by(|left, right| {
                f32::from(left.1.position.x).total_cmp(&f32::from(right.1.position.x))
            })
            .unwrap();
        assert!(
            retained.iter().any(|(font, glyph)| *font == rightmost.0
                && glyph.id == rightmost.1.id
                && glyph.index == rightmost.1.index),
            "the visual right edge is retained"
        );
    }

    #[test]
    fn descriptions_rtl_ellipsis_keeps_visual_suffix_and_original_shaping_for_mixed_scripts() {
        during_layout(|window, _| {
            window.with_text_style(
                Some(gpui::TextStyleRefinement {
                    white_space: Some(WhiteSpace::Nowrap),
                    text_overflow: Some(TextOverflow::Truncate("…".into())),
                    ..Default::default()
                }),
                |window| {
                    for text in [
                        "Beginning English account title with its distinctive suffix",
                        "مرحبا بالعالم هذا عنوان طويل للحساب الجديد",
                        "Account مرحبا بالعالم / אבג 123 — Ready 👩‍💻",
                        "A\u{0301} long heading אבג with 👨‍👩‍👧‍👦 and a suffix",
                    ] {
                        let layout = reuse_css_layout(None, text.into(), Direction::Rtl, window);
                        let source = layout.borrow();
                        let full = shape_wrapped_line(
                            text.into(),
                            0..text.len(),
                            Direction::Rtl,
                            source.font_size,
                            &source.run,
                            window,
                        );
                        let marker = shape_wrapped_line(
                            "…".into(),
                            0.."…".len(),
                            Direction::Rtl,
                            source.font_size,
                            &source.run,
                            window,
                        );
                        drop(source);
                        let width = full.width() * 0.65;
                        layout.borrow_mut().measure(Some(width), window);
                        let elided = layout.borrow().lines[0].clone();
                        assert_rtl_elision_geometry(&full, &marker, &elided, width);
                        if text.is_ascii() {
                            assert!(shaped_glyphs(&elided)[0].1.position.x > px(0.),
                                "a nonintegral number of retained glyphs leaves space before the adjacent marker");
                        }
                        let retained = shaped_glyphs(&elided)
                            .into_iter()
                            .skip(shaped_glyphs(&marker).len())
                            .collect::<Vec<_>>();
                        if text.is_ascii() {
                            assert!(
                                retained.iter().all(|(_, glyph)| glyph.index > 0),
                                "English retains its suffix, not prefix"
                            );
                            assert!(
                                retained
                                    .iter()
                                    .any(|(_, glyph)| glyph.index + 1 == text.len())
                            );
                        } else if text.starts_with('م') {
                            assert!(
                                retained.iter().any(|(_, glyph)| glyph.index == 0),
                                "Arabic retains its visual rightmost logical prefix"
                            );
                        }
                        let compositions = layout.borrow().compositions;
                        layout.borrow_mut().measure(Some(width), window);
                        assert_eq!(
                            layout.borrow().compositions,
                            compositions,
                            "warm elision remains cached"
                        );
                        layout
                            .borrow_mut()
                            .measure(Some(full.width() + px(10.)), window);
                        assert_eq!(
                            layout.borrow().lines[0].width(),
                            full.width(),
                            "fitting titles do not gain a marker"
                        );
                    }
                },
            );
        });
    }

    #[test]
    fn descriptions_css_emergency_wrap_uses_fractional_advances_and_preserves_word_priority() {
        during_layout(|window, _| {
            window.with_text_style(Some(gpui::TextStyleRefinement {
                font_size: Some(px(13.7).into()), line_height: Some(px(22.).into()),
                ..Default::default()
            }), |window| {
                for direction in [Direction::Ltr, Direction::Rtl] {
                    let word: SharedString = "ooooooooooooooo".into();
                    let css = reuse_css_layout_with_emergency(None, word.clone(), direction, true, window);
                    let full = css.borrow_mut().css_preferred(window);
                    assert!(full <= px(123.8));
                    css.borrow_mut().measure(Some(px(123.8)), window);
                    assert_eq!(css.borrow().lines.len(), 1, "fractional space must not be floored before fitting 15 glyphs");
                    assert_eq!(css.borrow().lines[0].text, word);
                    css.borrow_mut().measure(Some(full - px(0.01)), window);
                    assert!(css.borrow().lines.len() > 1, "the comparison uses the exact width without fitting slack");
                    let normal = reuse_css_layout(None, word, direction, window);
                    normal.borrow_mut().measure(Some(full - px(0.01)), window);
                    assert_eq!(normal.borrow().lines.len(), 1, "normal CSS permits overflow inside an unbreakable segment");

                    let text: SharedString = "aa bbbbbbbbb cc".into();
                    let mut style = Layout::new(text.clone(), direction, window);
                    let width = shape_wrapped_line("bbb cc".into(), 0..6, direction, style.font_size, &style.run, window).width();
                    style.key.css_wrap = true;
                    style.key.emergency_wrap = true;
                    style.measure(Some(width), window);
                    assert_eq!(style.lines.iter().map(|line| line.text.as_ref()).collect::<Vec<_>>(),
                        ["aa", "bbbbbb", "bbb cc"], "ordinary words move to a fresh line before emergency splitting, and the final fragment remains open");
                    for source in ["مرحبا1234567890 بالعالم 45", "אבג1234567890 ואז 67", "前缀1234567890 后缀 89"] {
                        let mixed = reuse_css_layout_with_emergency(None, source.into(), direction, true, window);
                        mixed.borrow_mut().measure(Some(px(63.75)), window);
                        assert!(mixed.borrow().lines.len() > 1);
                        assert_eq!(mixed.borrow().lines.iter().map(|line| line.text.as_ref()).collect::<String>().replace(' ', ""), source.replace(' ', ""));
                        assert!(mixed.borrow().lines.iter().all(|line| line.width() <= px(63.75)));
                    }
                }
            });
        });
    }

    #[test]
    fn descriptions_css_minimum_collapses_whitespace_and_keeps_bidi_logical_text() {
        let raw: SharedString = "  Status\n\tمرحبابالعالم   A\u{0301}🙂  ".into();
        let collapsed = collapse_css_whitespace(&raw);
        assert_eq!(collapsed.as_ref(), "Status مرحبابالعالم A\u{0301}🙂");
        during_layout(move |window, cx| {
            for direction in [Direction::Ltr, Direction::Rtl] {
                let paragraph = ParagraphText {
                    id: (
                        "descriptions-bidi-minimum",
                        usize::from(direction == Direction::Rtl),
                    )
                        .into(),
                    text: collapsed.clone(),
                    logical_text: Some(raw.clone()),
                    direction,
                    css_wrap: false,
                    preserve_css_minimum: true,
                    intrinsics: None,
                };
                let mut node = accesskit::Node::new(paragraph.a11y_role().unwrap());
                paragraph.write_a11y_info(&mut node);
                assert_eq!(node.value(), Some(raw.as_ref()));
                let mut expected = Layout::new(collapsed.clone(), direction, window);
                expected.measure_css(Some(px(0.)), window);
                let mut element = paragraph.into_any_element();
                let minimum = element.layout_as_root(
                    size(AvailableSpace::MinContent, AvailableSpace::MaxContent),
                    window,
                    cx,
                );
                // GPUI rounds a custom measured leaf up in device-pixel space
                // before exposing layout bounds (taffy.rs measured-size snap).
                let snapped = px((f32::from(expected.width) * window.scale_factor()).ceil()
                    / window.scale_factor());
                assert_eq!(minimum.width, snapped);
                let narrow = element.layout_as_root(
                    size(
                        AvailableSpace::Definite(px(25.)),
                        AvailableSpace::MaxContent,
                    ),
                    window,
                    cx,
                );
                assert!(narrow.height > px(0.));
                assert!(narrow.width <= px(25.));
            }
        });
    }

    #[test]
    fn alert_native_text_preserves_logical_accessible_content() {
        let raw: SharedString = "500 نتائج\nא\u{05b0}ב A\u{0301} 🙂".into();
        let text = ParagraphText {
            id: "text".into(),
            text: raw.clone(),
            direction: Direction::Rtl,
            logical_text: None,
            css_wrap: false,
            preserve_css_minimum: false,
            intrinsics: None,
        };
        let mut node = accesskit::Node::new(text.a11y_role().unwrap());
        text.write_a11y_info(&mut node);
        assert_eq!(node.value(), Some(raw.as_ref()));
    }

    #[test]
    fn result_css_whitespace_preserves_unicode_spaces_and_original_accessible_text() {
        let raw: SharedString = " \tTask\n\r  complete\u{000c}مرحبا\u{00a0}世界 \t".into();
        let display = collapse_css_whitespace(&raw);
        assert_eq!(display.as_ref(), "Task complete مرحبا\u{00a0}世界");
        let element = ParagraphText {
            id: "text".into(),
            text: display,
            direction: Direction::Rtl,
            logical_text: Some(raw.clone()),
            css_wrap: true,
            preserve_css_minimum: true,
            intrinsics: None,
        };
        let mut node = accesskit::Node::new(element.a11y_role().unwrap());
        element.write_a11y_info(&mut node);
        assert_eq!(node.value(), Some(raw.as_ref()));
        assert_eq!(collapse_css_whitespace(&" \r\n\t".into()).as_ref(), "");
        assert_eq!(
            collapse_css_whitespace(&"\u{00a0}\u{000b}".into()).as_ref(),
            "\u{00a0}\u{000b}"
        );
        assert_eq!(
            collapse_css_whitespace(&"A normal sentence".into()).as_ref(),
            "A normal sentence"
        );
    }

    #[test]
    fn notification_break_word_preserves_css_lines_and_only_splits_overlong_graphemes() {
        during_layout(|window, _| {
            window.with_text_style(
                Some(gpui::TextStyleRefinement {
                    font_family: Some("Segoe UI".into()),
                    font_size: Some(px(14.).into()),
                    line_height: Some(px(22.).into()),
                    ..Default::default()
                }),
                |window| {
                    let sentence: SharedString = "This is the content of the notification. This is the content of the notification. This is the content of the notification.".into();
                    for direction in [Direction::Ltr, Direction::Rtl] {
                        let css = reuse_css_layout(None, sentence.clone(), direction, window);
                        let emergency = reuse_css_layout_with_emergency(None, sentence.clone(), direction, true, window);
                        css.borrow_mut().measure(Some(px(300.)), window);
                        emergency.borrow_mut().measure(Some(px(300.)), window);
                        let lines = |layout: &Rc<RefCell<Layout>>| layout.borrow().lines.iter().map(|line| line.text.to_string()).collect::<Vec<_>>();
                        assert_eq!(lines(&css), lines(&emergency));
                        let composed = emergency.borrow().compositions;
                        emergency.borrow_mut().measure(Some(px(300.)), window);
                        assert_eq!(emergency.borrow().compositions, composed);
                        assert!(!Rc::ptr_eq(&css, &reuse_css_layout_with_emergency(Some(css.clone()), sentence.clone(), direction, true, window)));
                        let family = "👩‍👩‍👧‍👦";
                        let word: SharedString = format!("{}{}{}", "LongIdentifier".repeat(20), family, family).into();
                        let split = reuse_css_layout_with_emergency(None, word.clone(), direction, true, window);
                        split.borrow_mut().measure(Some(px(80.)), window);
                        assert!(split.borrow().lines.len() > 2);
                        let lines = lines(&split);
                        assert_eq!(lines.concat(), word.as_ref());
                        let mut boundary = 0;
                        let valid = word.grapheme_indices(true).map(|(i, _)| i).chain(std::iter::once(word.len())).collect::<Vec<_>>();
                        for line in lines {
                            boundary += line.len();
                            assert!(valid.contains(&boundary));
                        }
                    }
                },
            );
        });
    }

    #[test]
    fn result_css_wrap_preserves_unbreakable_segments_and_unicode_line_opportunities() {
        let mut cx = TestAppContext::single();
        let window = cx.add_window(|_, _| gpui::Empty);
        window
            .update(&mut cx, |_, window, _| {
                for direction in [Direction::Ltr, Direction::Rtl] {
                    let token = "LongUnbrokenResultTitleThatCannotBreakAtAnOrdinaryWordBoundary";
                    let text: SharedString = token.into();
                    let css = reuse_css_layout(None, text.clone(), direction, window);
                    css.borrow_mut().measure(Some(px(80.)), window);
                    assert_eq!(css.borrow().lines.len(), 1);
                    assert_eq!(css.borrow().lines[0].text.as_ref(), token);
                    assert!(css.borrow().lines[0].width() > px(80.));
                    let mixed: SharedString = format!("short words {token} after done").into();
                    let css = reuse_css_layout(None, mixed, direction, window);
                    css.borrow_mut().measure(Some(px(80.)), window);
                    assert!(css.borrow().lines.len() >= 3);
                    assert!(
                        css.borrow()
                            .lines
                            .iter()
                            .any(|line| line.text.as_ref() == token)
                    );
                    let cjk: SharedString = "第一行第二行第三行第四行".into();
                    let css = reuse_css_layout(None, cjk.clone(), direction, window);
                    css.borrow_mut().measure(Some(px(30.)), window);
                    assert!(css.borrow().lines.len() > 1);
                    assert_eq!(
                        css.borrow()
                            .lines
                            .iter()
                            .map(|line| line.text.as_ref())
                            .collect::<String>(),
                        cjk.as_ref()
                    );
                    let glued: SharedString = "Long\u{00a0}nonbreaking\u{00a0}phrase".into();
                    let css = reuse_css_layout(None, glued.clone(), direction, window);
                    css.borrow_mut().measure(Some(px(30.)), window);
                    assert_eq!(css.borrow().lines.len(), 1);
                    assert_eq!(css.borrow().lines[0].text, glued);
                    let family = "👩‍👩‍👧‍👦";
                    let css = reuse_css_layout(None, family.repeat(2).into(), direction, window);
                    css.borrow_mut().measure(Some(px(10.)), window);
                    assert_eq!(css.borrow().lines.len(), 2);
                    assert!(
                        css.borrow()
                            .lines
                            .iter()
                            .all(|line| line.text.as_ref() == family)
                    );
                }
            })
            .unwrap();
    }

    #[test]
    fn result_css_mode_retains_native_overflow_clamping_and_bounded_warm_cache() {
        let mut cx = TestAppContext::single();
        let ran = Rc::new(std::cell::Cell::new(false));
        let probe_ran = ran.clone();
        let window = cx.add_window(|_, _| CacheProbe(Rc::new(move |window| {
            if probe_ran.replace(true) { return; }
            let raw: SharedString = "Short words followed by LongUnbreakableIdentifier and additional words for wrapping.".into();
            let css = reuse_css_layout(None, raw.clone(), Direction::Ltr, window);
            for width in [None, Some(px(80.)), Some(px(120.))] {
                css.borrow_mut().measure(width, window);
            }
            let count = css.borrow().compositions;
            let warm = reuse_css_layout(Some(css.clone()), raw.clone(), Direction::Ltr, window);
            assert!(Rc::ptr_eq(&css, &warm));
            for width in [None, Some(px(80.)), Some(px(120.))] {
                warm.borrow_mut().measure(width, window);
            }
            assert_eq!(warm.borrow().compositions, count);
            assert!(!Rc::ptr_eq(&css, &reuse_css_layout(Some(css.clone()), "Changed text".into(), Direction::Ltr, window)));
            assert!(!Rc::ptr_eq(&css, &reuse_css_layout(Some(css.clone()), raw.clone(), Direction::Rtl, window)));
            window.with_text_style(Some(gpui::TextStyleRefinement { font_size: Some(px(24.).into()), ..Default::default() }), |window| {
                assert!(!Rc::ptr_eq(&css, &reuse_css_layout(Some(css.clone()), raw.clone(), Direction::Ltr, window)));
            });
            let old_scale = window.scale_factor();
            window.set_scale_factor(if old_scale == 1.5 { 2. } else { 1.5 });
            assert!(!Rc::ptr_eq(&css, &reuse_css_layout(Some(css.clone()), raw.clone(), Direction::Ltr, window)));
            window.set_scale_factor(old_scale);
            let native = reuse_layout(Some(css.clone()), raw.clone(), Direction::Ltr, window);
            assert!(!Rc::ptr_eq(&css, &native), "native emergency wrapping has a different cache mode");
            for width in 40..50 { css.borrow_mut().measure(Some(px(width as f32)), window); }
            assert_eq!(css.borrow().variants.len(), 4);
            window.with_text_style(Some(gpui::TextStyleRefinement { line_clamp: Some(2), ..Default::default() }), |window| {
                let clamped = reuse_css_layout(None, raw.clone(), Direction::Ltr, window);
                clamped.borrow_mut().measure(Some(px(80.)), window);
                assert_eq!(clamped.borrow().lines.len(), 2);
            });
            window.with_text_style(Some(gpui::TextStyleRefinement {
                white_space: Some(WhiteSpace::Nowrap),
                text_overflow: Some(TextOverflow::Truncate("…".into())),
                ..Default::default()
            }), |window| {
                let ellipsis = reuse_css_layout(None, raw, Direction::Ltr, window);
                ellipsis.borrow_mut().measure(Some(px(80.)), window);
                assert_eq!(ellipsis.borrow().lines.len(), 1);
                assert!(ellipsis.borrow().lines[0].text.ends_with('…'));
                assert!(ellipsis.borrow().lines[0].width() <= px(80.));
            });
        })));
        let mut visual = VisualTestContext::from_window(window.into(), &cx);
        visual.update(|window, cx| window.draw(cx).clear(cx));
        assert!(
            ran.get(),
            "cache and native style probes run during real prepaint"
        );
    }

    /// Emit deterministic glyph textures so overflow tests observe actual paint
    /// coordinates instead of the default no-op text system's empty rasters.
    struct PaintTextSystem;
    impl gpui::PlatformTextSystem for PaintTextSystem {
        fn add_fonts(&self, fonts: Vec<std::borrow::Cow<'static, [u8]>>) -> gpui::Result<()> {
            gpui::NoopTextSystem.add_fonts(fonts)
        }
        fn all_font_names(&self) -> Vec<String> {
            gpui::NoopTextSystem.all_font_names()
        }
        fn font_id(&self, font: &gpui::Font) -> gpui::Result<gpui::FontId> {
            gpui::NoopTextSystem.font_id(font)
        }
        fn font_metrics(&self, font: gpui::FontId) -> gpui::FontMetrics {
            gpui::NoopTextSystem.font_metrics(font)
        }
        fn typographic_bounds(
            &self,
            font: gpui::FontId,
            glyph: gpui::GlyphId,
        ) -> gpui::Result<Bounds<f32>> {
            gpui::NoopTextSystem.typographic_bounds(font, glyph)
        }
        fn advance(
            &self,
            font: gpui::FontId,
            glyph: gpui::GlyphId,
        ) -> gpui::Result<gpui::Size<f32>> {
            gpui::NoopTextSystem.advance(font, glyph)
        }
        fn glyph_for_char(&self, font: gpui::FontId, ch: char) -> Option<gpui::GlyphId> {
            gpui::NoopTextSystem.glyph_for_char(font, ch)
        }
        fn glyph_raster_bounds(
            &self,
            _: &gpui::RenderGlyphParams,
        ) -> gpui::Result<Bounds<gpui::DevicePixels>> {
            Ok(Bounds {
                origin: gpui::point(gpui::DevicePixels(0), gpui::DevicePixels(-8)),
                size: gpui::size(gpui::DevicePixels(3), gpui::DevicePixels(8)),
            })
        }
        fn rasterize_glyph(
            &self,
            _: &gpui::RenderGlyphParams,
            bounds: Bounds<gpui::DevicePixels>,
        ) -> gpui::Result<(gpui::Size<gpui::DevicePixels>, Vec<u8>)> {
            Ok((
                bounds.size,
                vec![255; (bounds.size.width.0 * bounds.size.height.0) as usize],
            ))
        }
        fn layout_line(
            &self,
            text: &str,
            font_size: Pixels,
            runs: &[gpui::FontRun],
        ) -> gpui::LineLayout {
            gpui::NoopTextSystem.layout_line(text, font_size, runs)
        }
        fn recommended_rendering_mode(
            &self,
            font: gpui::FontId,
            size: Pixels,
        ) -> gpui::TextRenderingMode {
            gpui::NoopTextSystem.recommended_rendering_mode(font, size)
        }
    }

    struct CssOverflowProbe {
        direction: Direction,
        text: SharedString,
    }

    struct RtlElisionSnapshot {
        full: ShapedLine,
        marker: ShapedLine,
        visible: ShapedLine,
        bounds: Bounds<Pixels>,
        accessible_value: String,
    }

    #[derive(Clone, Copy)]
    struct FractionalSnapshot {
        requested: Option<Pixels>,
        bounds: Bounds<Pixels>,
        full_width: Pixels,
        before: usize,
        after: usize,
    }

    struct CapturedFractionalParagraph {
        paragraph: ParagraphText,
        records: Rc<RefCell<Vec<Option<FractionalSnapshot>>>>,
        index: usize,
    }

    impl IntoElement for CapturedFractionalParagraph {
        type Element = Self;
        fn into_element(self) -> Self {
            self
        }
    }

    impl Element for CapturedFractionalParagraph {
        type RequestLayoutState = TextLayout;
        type PrepaintState = ();
        fn id(&self) -> Option<ElementId> {
            self.paragraph.id()
        }
        fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
            None
        }
        fn request_layout(
            &mut self,
            id: Option<&GlobalElementId>,
            inspector: Option<&InspectorElementId>,
            window: &mut Window,
            cx: &mut App,
        ) -> (LayoutId, TextLayout) {
            self.paragraph.request_layout(id, inspector, window, cx)
        }
        fn prepaint(
            &mut self,
            id: Option<&GlobalElementId>,
            inspector: Option<&InspectorElementId>,
            bounds: Bounds<Pixels>,
            state: &mut TextLayout,
            window: &mut Window,
            cx: &mut App,
        ) {
            let (requested, before) = match state {
                TextLayout::Paragraph(layout) => {
                    let layout = layout.borrow();
                    (layout.last_request_width, layout.lines.len())
                }
                _ => panic!("canonical CSS emergency layout"),
            };
            self.paragraph
                .prepaint(id, inspector, bounds, state, window, cx);
            let TextLayout::Paragraph(layout) = state else {
                unreachable!()
            };
            let mut layout = layout.borrow_mut();
            self.records.borrow_mut()[self.index] = Some(FractionalSnapshot {
                requested,
                bounds,
                full_width: layout.css_preferred(window),
                before,
                after: layout.lines.len(),
            });
        }
        fn paint(
            &mut self,
            id: Option<&GlobalElementId>,
            inspector: Option<&InspectorElementId>,
            bounds: Bounds<Pixels>,
            state: &mut TextLayout,
            prepaint: &mut (),
            window: &mut Window,
            cx: &mut App,
        ) {
            self.paragraph
                .paint(id, inspector, bounds, state, prepaint, window, cx);
        }
    }

    struct FractionalProbe(Rc<RefCell<Vec<Option<FractionalSnapshot>>>>);
    impl Render for FractionalProbe {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            div()
                .flex()
                .flex_col()
                .text_size(px(13.7))
                .line_height(px(22.))
                .children((0..2).map(|mode| {
                    div().flex().w(px(370.)).children((0..3).map(|column| {
                        let index = mode * 3 + column;
                        div()
                            .flex_1()
                            .min_w(px(0.))
                            .child(CapturedFractionalParagraph {
                                paragraph: ParagraphText {
                                    id: ("fractional-paragraph", index).into(),
                                    text: "ooooooooooooooo".into(),
                                    logical_text: Some("ooooooooooooooo".into()),
                                    direction: Direction::Ltr,
                                    css_wrap: false,
                                    preserve_css_minimum: mode == 0,
                                    intrinsics: None,
                                },
                                records: self.0.clone(),
                                index,
                            })
                    }))
                }))
        }
    }

    #[test]
    fn descriptions_css_prepaint_keeps_fractional_request_width_when_flex_edges_snap() {
        let mut cx = TestAppContext::single();
        let records = Rc::new(RefCell::new(vec![None; 6]));
        let observation = records.clone();
        let window = cx.add_window(move |_, _| FractionalProbe(records));
        cx.simulate_window_scale_factor_change(window.into(), 2.);
        let mut visual = VisualTestContext::from_window(window.into(), &cx);
        visual.update(|window, cx| window.draw(cx).clear(cx));
        let records = observation.borrow();
        let samples = records
            .iter()
            .map(|sample| sample.expect("actual CSS prepaint"))
            .collect::<Vec<_>>();
        for sample in &samples {
            assert!(sample.requested.is_some());
            assert_eq!(sample.before, 1);
            assert_eq!(
                sample.after, 1,
                "prepaint must not add a row because native edges snapped"
            );
            assert_eq!(sample.bounds.size.height, px(22.));
        }
        assert!(
            samples.iter().any(|sample| {
                let requested = sample.requested.unwrap();
                requested > sample.bounds.size.width
                    && sample.full_width > sample.bounds.size.width
                    && sample.full_width <= requested
            }),
            "exercise a fractional fit whose snapped bounds alone would rewrap"
        );
    }

    struct CapturedRtlParagraph {
        paragraph: ParagraphText,
        record: Rc<RefCell<Option<RtlElisionSnapshot>>>,
    }

    impl IntoElement for CapturedRtlParagraph {
        type Element = Self;
        fn into_element(self) -> Self {
            self
        }
    }

    impl Element for CapturedRtlParagraph {
        type RequestLayoutState = TextLayout;
        type PrepaintState = ();
        fn id(&self) -> Option<ElementId> {
            self.paragraph.id()
        }
        fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
            None
        }
        fn a11y_role(&self) -> Option<Role> {
            self.paragraph.a11y_role()
        }
        fn write_a11y_info(&self, node: &mut accesskit::Node) {
            self.paragraph.write_a11y_info(node);
        }
        fn request_layout(
            &mut self,
            id: Option<&GlobalElementId>,
            inspector: Option<&InspectorElementId>,
            window: &mut Window,
            cx: &mut App,
        ) -> (LayoutId, TextLayout) {
            self.paragraph.request_layout(id, inspector, window, cx)
        }
        fn prepaint(
            &mut self,
            id: Option<&GlobalElementId>,
            inspector: Option<&InspectorElementId>,
            bounds: Bounds<Pixels>,
            state: &mut TextLayout,
            window: &mut Window,
            cx: &mut App,
        ) {
            self.paragraph
                .prepaint(id, inspector, bounds, state, window, cx);
            let TextLayout::Paragraph(layout) = state else {
                panic!("CSS paragraph must retain its canonical layout");
            };
            let layout = layout.borrow();
            assert_eq!(layout.lines.len(), 1);
            let mut node = accesskit::Node::new(Role::Label);
            self.paragraph.write_a11y_info(&mut node);
            *self.record.borrow_mut() = Some(RtlElisionSnapshot {
                full: shape_wrapped_line(
                    layout.text.clone(),
                    0..layout.text.len(),
                    Direction::Rtl,
                    layout.font_size,
                    &layout.run,
                    window,
                ),
                marker: shape_wrapped_line(
                    "…".into(),
                    0.."…".len(),
                    Direction::Rtl,
                    layout.font_size,
                    &layout.run,
                    window,
                ),
                visible: layout.lines[0].clone(),
                bounds,
                accessible_value: node.value().unwrap().to_owned(),
            });
        }
        fn paint(
            &mut self,
            id: Option<&GlobalElementId>,
            inspector: Option<&InspectorElementId>,
            bounds: Bounds<Pixels>,
            state: &mut TextLayout,
            prepaint: &mut (),
            window: &mut Window,
            cx: &mut App,
        ) {
            self.paragraph
                .paint(id, inspector, bounds, state, prepaint, window, cx);
        }
    }

    struct RtlElisionProbe {
        text: SharedString,
        record: Rc<RefCell<Option<RtlElisionSnapshot>>>,
    }

    impl Render for RtlElisionProbe {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            div()
                .w(px(160.))
                .ml(px(200.))
                .truncate()
                .text_right()
                .text_size(px(14.))
                .line_height(px(22.))
                .child(CapturedRtlParagraph {
                    paragraph: ParagraphText {
                        id: "captured-rtl-title".into(),
                        text: collapse_css_whitespace(&self.text),
                        logical_text: Some(self.text.clone()),
                        direction: Direction::Rtl,
                        css_wrap: true,
                        preserve_css_minimum: true,
                        intrinsics: None,
                    },
                    record: self.record.clone(),
                })
        }
    }

    #[test]
    fn descriptions_rtl_ellipsis_actual_layout_paints_left_marker_and_preserves_accessible_source()
    {
        for raw in [
            " \tBeginning English account title with its distinctive suffix  ",
            "مرحبا بالعالم هذا عنوان طويل للحساب الجديد",
            "Account مرحبا بالعالم / אבג 123 — Ready 👩‍💻",
        ] {
            let mut cx = TestAppContext::build_with_text_system(
                gpui::TestDispatcher::new(0),
                None,
                std::sync::Arc::new(PaintTextSystem),
            );
            let record = Rc::new(RefCell::new(None));
            let observation = record.clone();
            let window = cx.add_window(move |_, _| RtlElisionProbe {
                text: raw.into(),
                record,
            });
            let mut visual = VisualTestContext::from_window(window.into(), &cx);
            visual.update(|window, cx| window.draw(cx).clear(cx));
            let snapshot = observation.borrow();
            let snapshot = snapshot.as_ref().expect("capture actual prepaint layout");
            assert_eq!(snapshot.accessible_value, raw);
            assert_eq!(snapshot.bounds.size.width, px(160.));
            assert_rtl_elision_geometry(
                &snapshot.full,
                &snapshot.marker,
                &snapshot.visible,
                snapshot.bounds.size.width,
            );
            visual.update(|window, _| {
                let scene = window.rendered_scene();
                let left = scene
                    .monochrome_sprites
                    .iter()
                    .map(|sprite| sprite.bounds.left().0)
                    .chain(
                        scene
                            .subpixel_sprites
                            .iter()
                            .map(|sprite| sprite.bounds.left().0),
                    )
                    .min_by(|left, right| left.total_cmp(right))
                    .expect("actual glyph paint");
                let marker_origin = f32::from(snapshot.bounds.left()
                    + shaped_glyphs(&snapshot.visible)[0].1.position.x)
                    * window.scale_factor();
                // Window::paint_glyph first quantizes to a subpixel variant,
                // then stores its integer raster origin in the scene sprite.
                // This fixture's raster_bounds has zero horizontal bearing.
                let variants = f32::from(gpui::SUBPIXEL_VARIANTS_X);
                let expected = (((marker_origin * variants - 0.5).ceil()) / variants).trunc();
                assert_eq!(left, expected,
                    "actual marker paint follows its adjacency-adjusted shape through native raster quantization");
            });
        }
    }
    impl Render for CssOverflowProbe {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            div()
                .w(px(80.))
                .ml(px(200.))
                .text_size(px(14.))
                .line_height(px(22.))
                .text_center()
                .debug_selector(|| "result-css-overflow".into())
                .child(normal_paragraph_text(self.text.clone(), self.direction))
        }
    }

    #[test]
    fn result_css_overlong_lines_follow_browser_inline_start_and_fitting_lines_center() {
        for (direction, text) in [
            (Direction::Ltr, "UnbrokenResultIdentifier"),
            (Direction::Rtl, "UnbrokenResultIdentifier"),
            (Direction::Ltr, "Fit"),
        ] {
            let mut cx = TestAppContext::build_with_text_system(
                gpui::TestDispatcher::new(0),
                None,
                std::sync::Arc::new(PaintTextSystem),
            );
            let window = cx.add_window(|_, _| CssOverflowProbe {
                direction,
                text: text.into(),
            });
            let mut visual = VisualTestContext::from_window(window.into(), &cx);
            visual.update(|window, cx| window.draw(cx).clear(cx));
            let bounds = visual.debug_bounds("result-css-overflow").unwrap();
            visual.update(|window, _| {
                let scene = window.rendered_scene();
                let sprites: Vec<_> = scene
                    .monochrome_sprites
                    .iter()
                    .map(|sprite| sprite.bounds)
                    .chain(scene.subpixel_sprites.iter().map(|sprite| sprite.bounds))
                    .collect();
                assert!(!sprites.is_empty(), "observe actual native glyph paint");
                let scale = window.scale_factor();
                let left = sprites
                    .iter()
                    .map(|bounds| bounds.origin.x.0 / scale)
                    .fold(f32::INFINITY, f32::min);
                let right = sprites
                    .iter()
                    .map(|bounds| (bounds.origin.x.0 + bounds.size.width.0) / scale)
                    .fold(f32::NEG_INFINITY, f32::max);
                if text == "Fit" {
                    assert!(
                        left > f32::from(bounds.origin.x) && right < f32::from(bounds.right()),
                        "fitting text remains centered"
                    );
                } else if direction == Direction::Ltr {
                    assert!(
                        (left - f32::from(bounds.origin.x)).abs() < 1.,
                        "LTR begins at the browser's inline start: left={left}, bounds={bounds:?}"
                    );
                    assert!(right > f32::from(bounds.right()));
                } else {
                    assert!(left < f32::from(bounds.origin.x));
                    assert!(
                        right <= f32::from(bounds.right()) + 1.
                            && right > f32::from(bounds.origin.x)
                    );
                }
            });
        }
    }

    #[test]
    fn alert_rtl_punctuation_uses_visual_runs_after_native_wrapping() {
        let mut cx = TestAppContext::single();
        let window = cx.add_window(|_, _| gpui::Empty);
        window
            .update(&mut cx, |_, window, _| {
                let raw: SharedString =
                    "Additional description and information about this alert.".into();
                let mut layout = Layout::new(raw.clone(), Direction::Rtl, window);
                layout.measure(Some(px(100.)), window);
                assert!(layout.lines.len() > 1);
                let last = layout.lines.last().unwrap();
                assert!(last.text.ends_with('.'));
                let period = last.text.len() - 1;
                let position = |index| {
                    last.runs
                        .iter()
                        .flat_map(|run| &run.glyphs)
                        .find(|glyph| glyph.index == index)
                        .unwrap()
                        .position
                        .x
                };
                assert!(
                    position(period) < position(0),
                    "terminal punctuation is visually leading in RTL"
                );
                assert_eq!(layout.text, raw, "logical source text stays unchanged");
                assert!(layout.lines.iter().all(|line| {
                    line.runs
                        .iter()
                        .flat_map(|run| &run.glyphs)
                        .all(|glyph| line.text.is_char_boundary(glyph.index))
                }));
            })
            .unwrap();
    }

    struct Probe {
        width: f32,
    }
    impl Render for Probe {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            div().flex().items_start().child(
                div()
                    .w(px(self.width))
                    .text_size(px(14.))
                    .line_height(px(22.))
                    .text_right()
                    .debug_selector(|| "alert-bidi-wrap".into())
                    .child(paragraph_text(
                        "text",
                        "مرحبا 500 results. Long alert content wraps into several lines.",
                        Direction::Rtl,
                    )),
            )
        }
    }

    #[test]
    fn alert_native_bidi_text_measures_multiple_lines_at_each_scale() {
        for scale in [1., 1.5, 2.] {
            let mut cx = TestAppContext::single();
            let window = cx.add_window(|_, _| Probe { width: 700. });
            cx.simulate_window_scale_factor_change(window.into(), scale);
            let mut visual = VisualTestContext::from_window(window.into(), &cx);
            visual.update(|window, cx| window.draw(cx).clear(cx));
            let wide = visual.debug_bounds("alert-bidi-wrap").unwrap();
            window
                .update(&mut visual, |probe, _, cx| {
                    probe.width = 160.;
                    cx.notify();
                })
                .unwrap();
            visual.run_until_parked();
            visual.update(|window, cx| window.draw(cx).clear(cx));
            let narrow = visual.debug_bounds("alert-bidi-wrap").unwrap();
            assert!(
                narrow.size.height > wide.size.height,
                "constrained RTL text must wrap: scale={scale}, wide={wide:?}, narrow={narrow:?}"
            );
        }
    }

    struct LeadingProbe {
        font_size: f32,
        width: f32,
    }
    impl Render for LeadingProbe {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            div()
                .flex()
                .flex_col()
                .children(
                    [Direction::Ltr, Direction::Rtl]
                        .into_iter()
                        .map(|direction| {
                            div()
                        .w(px(self.width))
                                .text_size(px(self.font_size))
                                .line_height(gpui::relative(22. / 14.))
                                .debug_selector(move || {
                                    format!("alert-leading-{direction:?}")
                                })
                        .child(paragraph_text("text", "Additional description. Long alert content wraps into several lines when constrained.", direction))
                        }),
                )
        }
    }

    #[test]
    fn alert_plain_text_leading_matches_between_ltr_and_rtl_at_each_scale() {
        for scale in [1., 1.5, 2.] {
            for font_size in [14., 16.] {
                for width in [160., 500.] {
                    let mut cx = TestAppContext::single();
                    let window = cx.add_window(|_, _| LeadingProbe { font_size, width });
                    cx.simulate_window_scale_factor_change(window.into(), scale);
                    let mut visual = VisualTestContext::from_window(window.into(), &cx);
                    visual.update(|window, cx| window.draw(cx).clear(cx));
                    let left = visual.debug_bounds("alert-leading-Ltr").unwrap();
                    let right = visual.debug_bounds("alert-leading-Rtl").unwrap();
                    assert_eq!(
                        left.size.height, right.size.height,
                        "scale={scale}, font={font_size}, width={width}, LTR={left:?}, RTL={right:?}"
                    );
                }
            }
        }
    }

    struct OverflowProbe;
    impl Render for OverflowProbe {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            div().flex().flex_col().items_start().children((0..3).map(|mode| {
                div().w(px(160.)).text_size(px(14.)).line_height(px(22.)).text_right()
                    .when(mode==1,|d|d.line_clamp(2)).when(mode==2,|d|d.truncate())
                    .debug_selector(move||format!("alert-overflow-{mode}"))
                    .child(paragraph_text("text", "مرحبا 500 results. Long alert content wraps into several lines and remains logically accessible.",Direction::Rtl))
            }))
        }
    }

    #[test]
    fn alert_styled_overflow_keeps_native_clamping_and_ellipsis_limits() {
        let mut cx = TestAppContext::single();
        let window = cx.add_window(|_, _| OverflowProbe);
        let mut visual = VisualTestContext::from_window(window.into(), &cx);
        visual.update(|window, cx| window.draw(cx).clear(cx));
        let unlimited = visual.debug_bounds("alert-overflow-0").unwrap();
        let clamped = visual.debug_bounds("alert-overflow-1").unwrap();
        let ellipsis = visual.debug_bounds("alert-overflow-2").unwrap();
        assert!(
            unlimited.size.height > clamped.size.height,
            "unlimited={unlimited:?}, clamped={clamped:?}"
        );
        assert_eq!(clamped.size.height, px(44.));
        assert_eq!(ellipsis.size.height, px(22.));
    }

    struct CacheProbe(Rc<dyn Fn(&mut Window)>);
    impl Render for CacheProbe {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            let run = self.0.clone();
            gpui::canvas(move |_, window, _| run(window), |_, _, _, _| {})
        }
    }

    #[test]
    fn alert_paragraph_cache_reuses_warm_widths_and_invalidates_content_style_and_scale() {
        let mut cx = TestAppContext::single();
        let ran = Rc::new(std::cell::Cell::new(false));
        let probe_ran = ran.clone();
        let window = cx.add_window(|_, _| {
            CacheProbe(Rc::new(move |window| {
                if probe_ran.replace(true) {
                    return;
                }
                let raw: SharedString =
                    "Additional description and information about this alert.".into();
                let layout = reuse_layout(None, raw.clone(), Direction::Rtl, window);
                for width in [None, Some(px(0.)), Some(px(160.))] {
                    layout.borrow_mut().measure(width, window);
                }
                let count = layout.borrow().compositions;
                let warm = reuse_layout(Some(layout.clone()), raw.clone(), Direction::Rtl, window);
                assert!(Rc::ptr_eq(&layout, &warm));
                for _ in 0..4 {
                    for width in [None, Some(px(0.)), Some(px(160.))] {
                        warm.borrow_mut().measure(width, window);
                    }
                }
                assert_eq!(
                    warm.borrow().compositions,
                    count,
                    "warm layout probes must not recompose glyph runs"
                );
                warm.borrow_mut().measure(Some(px(70.)), window);
                let narrow = warm.borrow().lines.len();
                warm.borrow_mut().measure(Some(px(500.)), window);
                assert!(
                    narrow > warm.borrow().lines.len(),
                    "width invalidation changes wrapping"
                );
                for width in 1..10 {
                    warm.borrow_mut()
                        .measure(Some(px(60. + width as f32)), window);
                }
                assert_eq!(
                    warm.borrow().variants.len(),
                    4,
                    "width cache remains bounded"
                );
                let changed = reuse_layout(
                    Some(warm.clone()),
                    "Changed alert content.".into(),
                    Direction::Rtl,
                    window,
                );
                assert!(!Rc::ptr_eq(&warm, &changed));
                let direction =
                    reuse_layout(Some(warm.clone()), raw.clone(), Direction::Ltr, window);
                assert!(!Rc::ptr_eq(&warm, &direction));
                window.with_text_style(
                    Some(gpui::TextStyleRefinement {
                        font_size: Some(px(24.).into()),
                        color: Some(gpui::red()),
                        ..Default::default()
                    }),
                    |window| {
                        let styled =
                            reuse_layout(Some(warm.clone()), raw.clone(), Direction::Rtl, window);
                        assert!(!Rc::ptr_eq(&warm, &styled));
                        assert_eq!(styled.borrow().font_size, px(24.));
                        assert_eq!(styled.borrow().run.color, gpui::red());
                    },
                );
                window.set_scale_factor(1.5);
                let scaled = reuse_layout(Some(warm.clone()), raw, Direction::Rtl, window);
                assert!(!Rc::ptr_eq(&warm, &scaled));
            }))
        });
        let mut visual = VisualTestContext::from_window(window.into(), &cx);
        visual.update(|window, cx| window.draw(cx).clear(cx));
        assert!(ran.get(), "cache probe ran in the real prepaint phase");
    }
}
