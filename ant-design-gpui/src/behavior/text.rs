// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native text wrapping with paragraph-level bidi glyph placement. Logical text
//! remains unchanged for accessibility and native shaping caches.

use std::{cell::RefCell, rc::Rc};

use gpui::{
    AnyElement, App, AvailableSpace, Bounds, Element, ElementId, FontId, GlobalElementId,
    InspectorElementId, IntoElement, LayoutId, Pixels, Role, ShapedLine, SharedString, TextAlign,
    TextOverflow, TextRun, TextStyle, TruncateFrom, WhiteSpace, Window, accesskit, px, size,
};

use crate::{Direction, behavior::paragraph::shape_wrapped_line};
use unicode_linebreak::{BreakOpportunity, linebreaks};

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
        }
    }
}

struct WidthLayout {
    constraint: Option<Pixels>,
    width: Pixels,
    lines: Rc<[ShapedLine]>,
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
    #[cfg(test)]
    compositions: usize,
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
            #[cfg(test)]
            compositions: 0,
        }
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
        let trimmed_end =
            |start: usize, end: usize| start + display[start..end].trim_end_matches(' ').len();
        let mut ranges = Vec::new();
        let mut start = 0;
        let mut previous = 0;
        for (end, opportunity) in linebreaks(&display) {
            if previous > start && shape(start..trimmed_end(start, end)).width() > width {
                ranges.push(start..trimmed_end(start, previous));
                start = previous;
            }
            previous = end;
            if opportunity == BreakOpportunity::Mandatory {
                ranges.push(start..trimmed_end(start, end));
                start = end;
            }
        }
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
    let mut key = LayoutKey::new(text.clone(), direction, window);
    key.css_wrap = true;
    previous
        .filter(|previous| previous.borrow().key == key)
        .unwrap_or_else(|| {
            let mut layout = Layout::new(text, direction, window);
            layout.key.css_wrap = true;
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
                let layout = if self.logical_text.is_some() {
                    reuse_css_layout(previous, self.text.clone(), self.direction, window)
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
        let measured = layout.clone();
        let id = window.request_measured_layout(
            Default::default(),
            move |known, available, window, _| {
                let width = known.width.or(match available.width {
                    AvailableSpace::Definite(width) => Some(width),
                    AvailableSpace::MinContent => Some(px(0.)),
                    _ => None,
                });
                let mut layout = measured.borrow_mut();
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
            TextLayout::Paragraph(layout) => {
                layout.borrow_mut().measure(Some(bounds.size.width), window)
            }
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

    #[test]
    fn alert_native_text_preserves_logical_accessible_content() {
        let raw: SharedString = "500 نتائج\nא\u{05b0}ב A\u{0301} 🙂".into();
        let text = ParagraphText {
            id: "text".into(),
            text: raw.clone(),
            direction: Direction::Rtl,
            logical_text: None,
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
