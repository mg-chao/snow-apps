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

pub(super) fn alert_text(text: SharedString, direction: Direction) -> AnyElement {
    AlertText { text, direction }.into_any_element()
}

struct AlertText {
    text: SharedString,
    direction: Direction,
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
        }
    }
}

struct WidthLayout {
    constraint: Option<Pixels>,
    width: Pixels,
    lines: Rc<[ShapedLine]>,
}

impl IntoElement for AlertText {
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
        if self.variants.len() == 4 {
            self.variants.remove(0);
        }
        self.variants.push(WidthLayout {
            constraint: width,
            width: self.width,
            lines: self.lines.clone(),
        });
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

impl Element for AlertText {
    type RequestLayoutState = TextLayout;
    type PrepaintState = ();

    fn id(&self) -> Option<ElementId> {
        Some("alert-text".into())
    }
    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        None
    }
    fn a11y_role(&self) -> Option<Role> {
        Some(Role::Label)
    }
    fn write_a11y_info(&self, node: &mut accesskit::Node) {
        node.set_value(self.text.to_string());
    }

    fn request_layout(
        &mut self,
        global_id: Option<&GlobalElementId>,
        inspector: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, Self::RequestLayoutState) {
        let style = window.text_style();
        if self.direction == Direction::Ltr
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
        let layout = window.with_element_state(
            global_id.expect("Alert text has a stable local ID"),
            |previous, window| {
                let layout = reuse_layout(previous, self.text.clone(), self.direction, window);
                (layout.clone(), layout)
            },
        );
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
            let _ = line.paint(
                origin,
                layout.line_height,
                layout.align,
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
        let text = AlertText {
            text: raw.clone(),
            direction: Direction::Rtl,
        };
        let mut node = accesskit::Node::new(text.a11y_role().unwrap());
        text.write_a11y_info(&mut node);
        assert_eq!(node.value(), Some(raw.as_ref()));
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
                    .child(alert_text(
                        "مرحبا 500 results. Long alert content wraps into several lines.".into(),
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
                        .child(alert_text("Additional description. Long alert content wraps into several lines when constrained.".into(), direction))
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
                    .child(alert_text("مرحبا 500 results. Long alert content wraps into several lines and remains logically accessible.".into(),Direction::Rtl))
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
