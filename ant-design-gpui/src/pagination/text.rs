// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Exact native advances for text totals, with wrapping only after the shaped
//! paragraph exceeds its available width. Ordinary GPUI text can otherwise
//! wrap a kerned line inside the very width measured for that same line.

use std::{cell::RefCell, rc::Rc};

use crate::Direction;
use gpui::{
    App, AvailableSpace, Bounds, Element, ElementId, Font, GlobalElementId, IndentAdjustment,
    InspectorElementId, IntoElement, LayoutId, LineFragment, Pixels, Role, ShapedLine,
    SharedString, Style, TextAlign, TextRun, WhiteSpace, Window, accesskit, px, relative, size,
};

pub(super) struct IntrinsicText {
    text: SharedString,
    direction: Direction,
    semantic: bool,
}

impl IntrinsicText {
    pub(super) fn new(text: SharedString, direction: Direction) -> Self {
        Self {
            text,
            direction,
            semantic: true,
        }
    }

    pub(super) fn decorative(text: SharedString, direction: Direction) -> Self {
        Self {
            text,
            direction,
            semantic: false,
        }
    }
}

impl IntoElement for IntrinsicText {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

pub(super) struct Layout {
    raw: SharedString,
    direction: Direction,
    font: Font,
    font_size: Pixels,
    line_height: Pixels,
    run: TextRun,
    wrap: bool,
    max_lines: Option<usize>,
    natural: Vec<ShapedLine>,
    natural_width: Pixels,
    wrapped: Option<(Pixels, Vec<ShapedLine>)>,
    use_wrapped: bool,
}

impl Layout {
    fn new(text: SharedString, direction: Direction, window: &Window) -> Self {
        let style = window.text_style();
        let font_size = style.font_size.to_pixels(window.rem_size());
        let line_height = window.pixel_snap(
            style
                .line_height
                .to_pixels(font_size.into(), window.rem_size()),
        );
        let mut layout = Self {
            raw: text,
            direction,
            font: style.font(),
            font_size,
            line_height,
            run: style.to_run(0),
            wrap: style.white_space == WhiteSpace::Normal,
            max_lines: style.line_clamp,
            natural: Vec::new(),
            natural_width: px(0.),
            wrapped: None,
            use_wrapped: false,
        };
        layout.natural = layout
            .raw
            .split('\n')
            .map(|paragraph| layout.shape(paragraph, window))
            .collect();
        if let Some(maximum) = layout.max_lines {
            layout.natural.truncate(maximum.max(1));
        }
        layout.natural_width = layout
            .natural
            .iter()
            .map(|line| line.width())
            .fold(px(0.), Pixels::max);
        layout
    }

    fn shape(&self, text: &str, window: &Window) -> ShapedLine {
        super::bidi::shape_paragraph(
            text.to_owned().into(),
            self.direction,
            self.font_size,
            &self.run,
            window,
        )
    }

    fn select(&mut self, width: Option<Pixels>, window: &Window) {
        let tolerance = px(0.5 / window.scale_factor().max(f32::EPSILON) + 0.0001);
        let Some(width) =
            width.filter(|width| self.wrap && *width + tolerance < self.natural_width)
        else {
            self.use_wrapped = false;
            return;
        };
        let width = width.max(px(1.));
        if self
            .wrapped
            .as_ref()
            .is_none_or(|(previous, _)| *previous != width)
        {
            let mut lines = Vec::new();
            for (index, paragraph) in self.raw.split('\n').enumerate() {
                let Some(natural) = self.natural.get(index) else {
                    break;
                };
                if natural.width() <= width + tolerance {
                    lines.push(natural.clone());
                } else {
                    // Resolve direction against the complete paragraph so a
                    // continuation keeps its preceding strong-text context.
                    // Wrapping boundaries remain logical UTF-8 byte offsets.
                    let mut wrapper = window
                        .text_system()
                        .line_wrapper(self.font.clone(), self.font_size);
                    let fragments = [LineFragment::text(paragraph)];
                    let boundaries = wrapper
                        .wrap_line(&fragments, width, IndentAdjustment::NoIndent)
                        .map(|boundary| boundary.ix)
                        .collect::<Vec<_>>();
                    let mut start = 0;
                    for end in boundaries
                        .into_iter()
                        .chain(std::iter::once(paragraph.len()))
                    {
                        if end > start {
                            let line = paragraph[start..end].trim_end_matches([' ', '\t', '\r']);
                            lines.push(super::bidi::shape_wrapped_line(
                                paragraph.to_owned().into(),
                                start..start + line.len(),
                                self.direction,
                                self.font_size,
                                &self.run,
                                window,
                            ));
                        }
                        start = end;
                    }
                }
            }
            if let Some(maximum) = self.max_lines {
                lines.truncate(maximum.max(1));
            }
            self.wrapped = Some((width, lines));
        }
        self.use_wrapped = true;
    }

    fn lines(&self) -> &[ShapedLine] {
        if self.use_wrapped {
            &self.wrapped.as_ref().expect("selected wrapped layout").1
        } else {
            &self.natural
        }
    }
}

impl Element for IntrinsicText {
    type RequestLayoutState = Rc<RefCell<Layout>>;
    type PrepaintState = ();

    fn id(&self) -> Option<ElementId> {
        self.semantic.then(|| "pagination-total-text".into())
    }
    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        None
    }
    fn a11y_role(&self) -> Option<Role> {
        self.semantic.then_some(Role::Label)
    }
    fn write_a11y_info(&self, node: &mut accesskit::Node) {
        if self.semantic {
            node.set_value(self.text.to_string());
        }
    }

    fn request_layout(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, Self::RequestLayoutState) {
        let state = Rc::new(RefCell::new(Layout::new(
            self.text.clone(),
            self.direction,
            window,
        )));
        let natural_width = state.borrow().natural_width;
        let measured = state.clone();
        let base = Style::default();
        let style = Style {
            min_size: gpui::Size {
                width: px(0.).into(),
                ..base.min_size
            },
            max_size: gpui::Size {
                width: relative(1.).into(),
                ..base.max_size
            },
            ..base
        };
        let measured_id =
            window.request_measured_layout(style, move |known, available, window, _| {
                let mut layout = measured.borrow_mut();
                let available = known.width.or(match available.width {
                    AvailableSpace::Definite(width) => Some(width),
                    _ => None,
                });
                layout.select(available, window);
                let width = known.width.unwrap_or_else(|| {
                    available.map_or(layout.natural_width, |width| {
                        width.min(layout.natural_width)
                    })
                });
                size(
                    width,
                    known
                        .height
                        .unwrap_or(layout.line_height * layout.lines().len()),
                )
            });
        // GPUI rounds measured leaf sizes upward to a physical pixel. Keep the
        // authored natural advance on a normal layout node so that this extra
        // rounding cannot accumulate into offsets in the rest of the pager.
        let base = Style::default();
        let wrapper = Style {
            size: gpui::Size {
                width: natural_width.into(),
                ..base.size
            },
            min_size: gpui::Size {
                width: px(0.).into(),
                ..base.min_size
            },
            max_size: gpui::Size {
                width: relative(1.).into(),
                ..base.max_size
            },
            ..base
        };
        let id = window.request_layout(wrapper, [measured_id], cx);
        (id, state)
    }

    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        state: &mut Self::RequestLayoutState,
        window: &mut Window,
        _: &mut App,
    ) {
        state.borrow_mut().select(Some(bounds.size.width), window);
    }

    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        state: &mut Self::RequestLayoutState,
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) {
        let layout = state.borrow();
        let mut origin = bounds.origin;
        let align = if self.direction == Direction::Rtl {
            TextAlign::Right
        } else {
            window.text_style().text_align
        };
        for line in layout.lines() {
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
    use gpui::{
        Context, FontFeatures, Render, Styled, TestAppContext, VisualTestContext, div, prelude::*,
    };

    struct Probe {
        direction: Direction,
        font_size: f32,
        narrow: bool,
    }
    impl Render for Probe {
        fn render(&mut self, window: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            let text: SharedString = "51-60 of 500 items".into();
            let mut font = gpui::font("Segoe UI");
            font.features = FontFeatures(std::sync::Arc::new(vec![("kern".into(), 1)]));
            let natural = super::super::bidi::shape_paragraph(
                text.clone(),
                self.direction,
                px(self.font_size),
                &TextRun {
                    len: text.len(),
                    font: font.clone(),
                    color: gpui::black(),
                    background_color: None,
                    underline: None,
                    strikethrough: None,
                },
                window,
            )
            .width();
            div().flex().items_start().child(
                div()
                    .debug_selector(|| "pagination-total-probe".into())
                    .font(font)
                    .text_size(px(self.font_size))
                    .line_height(px(self.font_size * 1.5))
                    .w(if self.narrow { natural * 0.35 } else { natural })
                    .child(IntrinsicText::new(text, self.direction)),
            )
        }
    }

    #[test]
    fn pagination_total_native_text_uses_exact_advances_and_wraps_only_when_constrained() {
        for scale in [1., 1.5, 2.] {
            for direction in [Direction::Ltr, Direction::Rtl] {
                for font_size in [12., 14., 24.] {
                    let mut cx = TestAppContext::single();
                    let window = cx.add_window(|_, _| Probe {
                        direction,
                        font_size,
                        narrow: false,
                    });
                    cx.simulate_window_scale_factor_change(window.into(), scale);
                    let mut visual = VisualTestContext::from_window(window.into(), &cx);
                    visual.update(|window, cx| window.draw(cx).clear(cx));
                    let wide = visual.debug_bounds("pagination-total-probe").unwrap();
                    assert!(
                        (f32::from(wide.size.height) - font_size * 1.5).abs() <= 1. / scale,
                        "scale={scale}, direction={direction:?}, font={font_size}: {wide:?}"
                    );
                    window
                        .update(&mut visual, |probe, _, cx| {
                            probe.narrow = true;
                            cx.notify();
                        })
                        .unwrap();
                    visual.run_until_parked();
                    visual.update(|window, cx| window.draw(cx).clear(cx));
                    let narrow = visual.debug_bounds("pagination-total-probe").unwrap();
                    assert!(
                        narrow.size.height > wide.size.height,
                        "genuinely constrained totals must wrap"
                    );
                }
            }
        }
    }

    #[test]
    fn pagination_total_preserves_raw_accessible_text_and_decorative_semantics() {
        let raw: SharedString = "500 نتائج\n51-60 of 500 items".into();
        let element = IntrinsicText::new(raw.clone(), Direction::Rtl);
        let mut node = accesskit::Node::new(element.a11y_role().unwrap());
        element.write_a11y_info(&mut node);
        assert_eq!(node.value(), Some(raw.as_ref()));
        let decorative = IntrinsicText::decorative(raw, Direction::Rtl);
        assert_eq!(decorative.id(), None);
        assert_eq!(decorative.a11y_role(), None);
    }
}
