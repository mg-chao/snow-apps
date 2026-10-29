// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use gpui::{
    AnyElement, App, Edges, FontWeight, IntoElement, ParentElement, Pixels, Refineable, RenderOnce,
    Role, StyleRefinement, Styled, Subscription, Window, div, prelude::*, px, relative,
};

use super::{
    Content, DescriptionItem, Descriptions, DescriptionsLayout, DescriptionsStyle,
    DescriptionsStyleContext, DescriptionsStyles, DescriptionsTheme,
    borders::{boundary_parts, cell_borders, horizontal_intrinsic_adjustment, outer_borders},
    element::{Cell, CellPrecision, ColonHandle, Table, TextPart},
    geometry::plan_rows,
    paint::{BorderPaint, BorderQueue},
};
use crate::{ConfigScope, Direction, Size, behavior::Accessibility};

struct ScopeObserver {
    scope: ConfigScope,
    _subscription: Subscription,
}

#[derive(Clone, Copy)]
struct BorderDecoration {
    widths: Edges<Pixels>,
    radii: Option<gpui::Corners<Pixels>>,
    outsets: Option<Edges<Pixels>>,
}

#[derive(Clone, Copy)]
struct CellBorder<'a> {
    insets: Edges<Pixels>,
    decoration: Option<BorderDecoration>,
    queue: Option<&'a BorderQueue>,
}

fn interior_decoration(
    row: usize,
    rows: usize,
    end: usize,
    tracks: usize,
    width: f32,
    scale: f32,
    direction: Direction,
) -> BorderDecoration {
    let mut widths = Edges::default();
    let mut outsets = Edges::default();
    if end < tracks {
        let (before, after) = boundary_parts(end - 1, width, scale);
        let outward = (before - after) / 2.;
        if direction == Direction::Ltr {
            widths.right = px(width);
            outsets.right = outward;
        } else {
            widths.left = px(width);
            outsets.left = outward;
        }
    }
    if row + 1 < rows {
        let (before, after) = boundary_parts(row, width, scale);
        widths.bottom = px(width);
        outsets.bottom = (before - after) / 2.;
    }
    BorderDecoration {
        widths,
        radii: Some(Default::default()),
        outsets: Some(outsets),
    }
}

impl RenderOnce for Descriptions {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self
            .scope
            .take()
            .unwrap_or_else(|| ConfigScope::application(cx));
        let observer = window.use_keyed_state(self.id.clone(), cx, |window, cx| ScopeObserver {
            scope: scope.clone(),
            _subscription: scope.observe_in(window, cx, |_, _, cx| cx.notify()),
        });
        observer.update(cx, |state, cx| {
            if state.scope != scope {
                state._subscription = scope.observe_in(window, cx, |_, _, cx| cx.notify());
                state.scope = scope.clone();
            }
        });
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<DescriptionsTheme>();
        let size = self
            .size
            .or(config.configured_size())
            .unwrap_or(Size::Large);
        let metrics = DescriptionsStyle::from_tokens(&tokens, size, self.bordered);
        // Browser layout viewports use integral CSS pixels. A native resize at
        // fractional DPI can read back, for example, 575.333 logical pixels for
        // a requested 575px window; keep that quantization out of breakpoint gaps.
        let viewport = f64::from(f32::from(window.viewport_size().width).round());
        let columns = self.columns.resolve(viewport, tokens.globals());
        let context = DescriptionsStyleContext {
            layout: self.layout,
            bordered: self.bordered,
            colon: self.colon,
            columns,
            size,
            direction: config.direction,
            item_count: self.items.len(),
            has_title: self.title.is_some(),
            has_extra: self.extra.is_some(),
        };
        let mut styles = config.descriptions.styles.clone();
        if let Some(resolver) = &config.descriptions.styles_fn {
            styles.refine(&resolver.resolve(&context, window, cx));
        }
        styles.refine(&self.styles);
        if let Some(resolver) = &self.styles_fn {
            styles.refine(&resolver(&context, window, cx));
        }
        styles.root.refine(&self.style);
        if self.accessibility.label.is_none()
            && let Some(Content::Text(title)) = &self.title
        {
            self.accessibility.label = Some(title.clone());
        }
        let direction = config.direction;
        let mut root = div()
            .id(self.id)
            .flex()
            .flex_col()
            .min_w(px(0.))
            .font(tokens.font().clone())
            .text_size(px(metrics.font_size as f32))
            .line_height(relative(metrics.line_height as f32))
            .text_color(metrics.content_color)
            .when(direction == Direction::Rtl, |root| root.text_right());
        root.style().refine(&styles.root);
        if self.title.is_some() || self.extra.is_some() {
            let mut header = div()
                .id("header")
                .flex()
                .items_center()
                .min_w(px(0.))
                .mb(px(metrics.title_margin_bottom as f32))
                .when(direction == Direction::Rtl, |header| {
                    header.flex_row_reverse()
                });
            header.style().refine(&styles.header);
            if let Some(title) = self.title {
                let mut slot = div()
                    .id("title")
                    .flex_1()
                    .min_w(px(0.))
                    .truncate()
                    .text_color(metrics.title_color)
                    .text_size(px(metrics.title_font_size as f32))
                    .line_height(relative(metrics.title_line_height as f32))
                    .font_weight(FontWeight(metrics.title_font_weight as f32));
                slot.style().refine(&styles.title);
                header = header.child(slot.child(title.into_element(direction, false)));
            }
            if let Some(extra) = self.extra {
                let mut slot = div()
                    .id("extra")
                    .flex_shrink_0()
                    .text_color(metrics.extra_color)
                    .when(direction == Direction::Ltr, |slot| slot.ml_auto())
                    .when(direction == Direction::Rtl, |slot| slot.mr_auto());
                slot.style().refine(&styles.extra);
                header = header.child(slot.child(extra.into_element(direction, false)));
            }
            root = root.child(header);
        }
        let rows = plan_rows(
            columns,
            self.items
                .iter()
                .map(|item| item.span.resolve(viewport, tokens.globals())),
        );
        let row_count = rows.len();
        let mut items = self.items.into_iter().map(Some).collect::<Vec<_>>();
        let vertical = self.layout == DescriptionsLayout::Vertical;
        let border = metrics.border_width as f32;
        let border_queue = (self.bordered
            && metrics.border_line_type != crate::theme::LineType::Solid)
            .then(BorderQueue::default);
        let physical_rows = row_count * if vertical { 2 } else { 1 };
        let physical_tracks = columns * if self.bordered && !vertical { 2 } else { 1 };
        let collapsed_edges = |row: usize, start: usize, end: usize| {
            if self.bordered {
                cell_borders(
                    row,
                    physical_rows,
                    start,
                    end,
                    physical_tracks,
                    border,
                    window.scale_factor(),
                    direction,
                )
            } else {
                Edges::default()
            }
        };
        let borderless_adjustment = |next: bool| {
            1. - f32::from(window.pixel_snap(px(1.)))
                + if next {
                    metrics.item_padding_end as f32
                        - f32::from(window.pixel_snap(px(metrics.item_padding_end as f32)))
                } else {
                    0.
                }
        };
        let mut cells = Vec::with_capacity(row_count * if vertical { 2 } else { 1 });
        for (row_index, row) in rows.into_iter().enumerate() {
            let last_row = row_index + 1 == row_count;
            let label_spacing = row_spacing(
                row_index * if vertical { 2 } else { 1 },
                !last_row || vertical,
                metrics,
                window,
            );
            let content_spacing = row_spacing(row_index * 2 + 1, !last_row, metrics, window);
            let mut labels = Vec::new();
            let mut contents = Vec::new();
            let count = row.len();
            for (index, placement) in row.into_iter().enumerate() {
                let item = items[placement.item]
                    .take()
                    .expect("one placement per item");
                let last_cell = index + 1 == count;
                if vertical || self.bordered {
                    let accessible_label = item.resolved_accessibility_label();
                    let DescriptionItem {
                        id,
                        label,
                        content,
                        styles: item_styles,
                        style,
                        ..
                    } = item;
                    let horizontal_bordered = !vertical && self.bordered;
                    let start = if horizontal_bordered {
                        placement.start * 2
                    } else {
                        placement.start
                    };
                    let label_span = if horizontal_bordered {
                        1
                    } else {
                        placement.span
                    };
                    let content_span = if horizontal_bordered {
                        placement.span * 2 - 1
                    } else {
                        placement.span
                    };
                    let label_row = row_index * if vertical { 2 } else { 1 };
                    let content_row = label_row + usize::from(vertical);
                    let content_start = start + usize::from(horizontal_bordered);
                    let label_edges = collapsed_edges(label_row, start, start + label_span);
                    let content_edges =
                        collapsed_edges(content_row, content_start, content_start + content_span);
                    let label_decoration = border_queue.as_ref().map(|_| {
                        interior_decoration(
                            label_row,
                            physical_rows,
                            start + label_span,
                            physical_tracks,
                            border,
                            window.scale_factor(),
                            direction,
                        )
                    });
                    let content_decoration = border_queue.as_ref().map(|_| {
                        interior_decoration(
                            content_row,
                            physical_rows,
                            content_start + content_span,
                            physical_tracks,
                            border,
                            window.scale_factor(),
                            direction,
                        )
                    });
                    let (label, label_precision) = slot(
                        id.clone(),
                        label,
                        accessible_label.clone(),
                        true,
                        self.bordered,
                        self.colon,
                        direction,
                        metrics,
                        &style,
                        &styles.label,
                        &item_styles.label,
                        label_spacing,
                        CellBorder {
                            insets: label_edges,
                            decoration: label_decoration,
                            queue: border_queue.as_ref(),
                        },
                        !last_cell || horizontal_bordered,
                        row_index == 0 && index == 0,
                        last_row && !vertical && index == 0,
                        window.rem_size(),
                    );
                    let (content, content_precision) = slot(
                        id,
                        content,
                        accessible_label,
                        false,
                        self.bordered,
                        self.colon,
                        direction,
                        metrics,
                        &style,
                        &styles.content,
                        &item_styles.content,
                        content_spacing,
                        CellBorder {
                            insets: content_edges,
                            decoration: content_decoration,
                            queue: border_queue.as_ref(),
                        },
                        !last_cell,
                        false,
                        vertical && last_row && index == 0,
                        window.rem_size(),
                    );
                    labels.push(Cell {
                        precision: label_precision,
                        start,
                        span: label_span,
                        width_adjustment: if self.bordered {
                            horizontal_intrinsic_adjustment(
                                start,
                                start + label_span,
                                physical_tracks,
                                border,
                                label_edges,
                            )
                        } else {
                            borderless_adjustment(!last_cell)
                        },
                        constrained: if self.bordered {
                            fixed_width(&[&style, &styles.label, &item_styles.label])
                        } else {
                            fixed_width(&[&style])
                        },
                        element: label,
                    });
                    let content_cell = Cell {
                        precision: content_precision,
                        start: content_start,
                        span: content_span,
                        width_adjustment: if self.bordered {
                            horizontal_intrinsic_adjustment(
                                content_start,
                                content_start + content_span,
                                physical_tracks,
                                border,
                                content_edges,
                            )
                        } else {
                            borderless_adjustment(!last_cell)
                        },
                        constrained: if self.bordered {
                            fixed_width(&[&style, &styles.content, &item_styles.content])
                        } else {
                            fixed_width(&[&style])
                        },
                        element: content,
                    };
                    if horizontal_bordered {
                        labels.push(content_cell);
                    } else {
                        contents.push(content_cell);
                    }
                } else {
                    let constrained = fixed_width(&[&item.style]);
                    let (element, precision) = horizontal_item(
                        item,
                        self.colon,
                        direction,
                        metrics,
                        &styles,
                        label_spacing,
                        last_cell,
                    );
                    labels.push(Cell {
                        start: placement.start,
                        span: placement.span,
                        width_adjustment: borderless_adjustment(!last_cell),
                        constrained,
                        element,
                        precision,
                    });
                }
            }
            cells.push(labels);
            if vertical {
                cells.push(contents);
            }
        }
        let table = Table {
            rows: cells,
            tracks: physical_tracks,
            direction,
            border_queue: border_queue.clone(),
        };
        let outside = outer_borders(border, window.scale_factor());
        let view = div()
            .min_w(px(0.))
            .w_full()
            .when(self.bordered, |view| {
                view.border_t(outside.top)
                    .border_r(outside.right)
                    .border_b(outside.bottom)
                    .border_l(outside.left)
                    .border_color(metrics.border_color)
                    .rounded(px(metrics.border_radius as f32))
            })
            .child(table);
        let view = if self.bordered {
            component_border(
                view,
                metrics,
                window.rem_size(),
                Accessibility::default(),
                Some(BorderDecoration {
                    widths: if metrics.border_line_type == crate::theme::LineType::Solid {
                        outside
                    } else {
                        Edges::all(px(border))
                    },
                    radii: None,
                    outsets: None,
                }),
                None,
                true,
            )
        } else {
            view.into_any_element()
        };
        self.accessibility.apply(root.child(view))
    }
}

fn fixed_width(layers: &[&StyleRefinement]) -> bool {
    layers
        .iter()
        .filter_map(|style| style.size.width)
        .next_back()
        .is_some_and(|width| !matches!(width, gpui::Length::Auto))
}

fn row_spacing(
    index: usize,
    has_bottom: bool,
    metrics: DescriptionsStyle,
    window: &Window,
) -> (gpui::Pixels, gpui::Pixels) {
    // Diffuse authored padding rounding over physical rows so half-device-pixel
    // insets do not accumulate into a progressively shorter table at 150% DPI.
    let bottom = metrics.item_padding_bottom as f32;
    let phase = index as f32 * (1. + bottom);
    let start = window.pixel_snap(px(phase));
    let after_top = window.pixel_snap(px(phase + 1.));
    let end = window.pixel_snap(px(phase + 1. + if has_bottom { bottom } else { 0. }));
    (after_top - start, end - after_top)
}

fn component_border<E: IntoElement + Styled>(
    mut surface: E,
    metrics: DescriptionsStyle,
    rem_size: gpui::Pixels,
    semantics: Accessibility,
    decoration: Option<BorderDecoration>,
    queue: Option<&BorderQueue>,
    behind: bool,
) -> AnyElement {
    if metrics.border_line_type == crate::theme::LineType::Solid && !behind {
        return semantics.apply(surface).into_any_element();
    }
    let mut native = gpui::Style::default();
    native.refine(surface.style());
    if native.visibility == gpui::Visibility::Hidden {
        return semantics.apply(surface).into_any_element();
    }
    let mut color = native.border_color.unwrap_or(metrics.border_color);
    color.a *= native.opacity.unwrap_or(1.);
    let paint = BorderPaint {
        // The enclosing view reserves cumulatively snapped edges for layout,
        // but its dotted/dashed outline keeps the uniform authored stroke.
        widths: decoration.map_or_else(
            || native.border_widths.to_pixels(rem_size),
            |value| value.widths,
        ),
        radii: decoration
            .and_then(|value| value.radii)
            .unwrap_or_else(|| native.corner_radii.to_pixels(rem_size)),
        outsets: decoration.and_then(|value| value.outsets),
        color,
        line_type: metrics.border_line_type,
    };
    surface.style().border_color = Some(gpui::Hsla::transparent_black());
    if let Some(queue) = queue {
        semantics
            .apply(super::paint::deferred_bordered(
                surface,
                paint,
                queue.clone(),
            ))
            .into_any_element()
    } else if behind {
        semantics
            .apply(super::paint::behind_bordered(surface, paint))
            .into_any_element()
    } else {
        semantics
            .apply(super::paint::bordered(surface, paint))
            .into_any_element()
    }
}

#[allow(clippy::too_many_arguments)]
fn slot(
    id: gpui::ElementId,
    content: Option<Content>,
    accessible_label: Option<gpui::SharedString>,
    label: bool,
    bordered: bool,
    colon: bool,
    direction: Direction,
    metrics: DescriptionsStyle,
    item_style: &StyleRefinement,
    component_style: &StyleRefinement,
    item_type_style: &StyleRefinement,
    spacing: (gpui::Pixels, gpui::Pixels),
    border: CellBorder<'_>,
    end_padding: bool,
    first_corner: bool,
    last_corner: bool,
    rem_size: gpui::Pixels,
) -> (AnyElement, CellPrecision) {
    let mut precision = CellPrecision::default();
    let slot_id = (id, if label { "label" } else { "content" });
    let mut cell = div()
        .id(slot_id)
        .flex()
        .flex_col()
        .w_full()
        .h_full()
        .min_w(px(0.))
        .text_color(if label {
            metrics.label_color
        } else {
            metrics.content_color
        });
    if bordered {
        cell = cell
            .h_full()
            .justify_center()
            .py(px(metrics.cell_padding_vertical as f32))
            .px(px(metrics.cell_padding_horizontal as f32))
            .border_t(border.insets.top)
            .border_r(border.insets.right)
            .border_b(border.insets.bottom)
            .border_l(border.insets.left)
            .border_color(metrics.border_color);
        if label {
            cell = cell.bg(metrics.label_background);
        }
        let radius = px(metrics.border_radius as f32);
        if first_corner {
            cell = if direction == Direction::Ltr {
                cell.rounded_tl(radius)
            } else {
                cell.rounded_tr(radius)
            };
        }
        if last_corner {
            cell = if direction == Direction::Ltr {
                cell.rounded_bl(radius)
            } else {
                cell.rounded_br(radius)
            };
        }
        cell.style().refine(item_style);
        cell.style().refine(component_style);
        cell.style().refine(item_type_style);
        if let Some(content) = content {
            let (element, part) = cell_content(
                content,
                direction,
                !label,
                false,
                !fixed_width(&[item_style, component_style, item_type_style]),
            );
            if label {
                precision.label = part;
            } else {
                precision.content = part;
            }
            cell = cell.child(element);
        }
    } else {
        // HTML's native table-cell inset survives the reference's borderless
        // selectors. Only bottom and inline-end padding are overridden there.
        cell = cell
            .pt(spacing.0)
            .pb(spacing.1)
            .when(direction == Direction::Ltr, |cell| cell.pl(px(1.)))
            .when(direction == Direction::Rtl, |cell| cell.pr(px(1.)));
        if end_padding {
            cell = if direction == Direction::Ltr {
                cell.pr(px(metrics.item_padding_end as f32))
            } else {
                cell.pl(px(metrics.item_padding_end as f32))
            };
        }
        cell.style().refine(item_style);
        if let Some(content) = content {
            let mut inner = div()
                .flex()
                .items_baseline()
                .min_w(px(0.))
                .when(direction == Direction::Rtl, |inner| {
                    inner.flex_row_reverse()
                });
            if label {
                inner = if direction == Direction::Ltr {
                    inner.self_start()
                } else {
                    inner.self_end()
                };
            }
            inner.style().refine(component_style);
            inner.style().refine(item_type_style);
            let (element, part) = cell_content(
                content,
                direction,
                !label,
                false,
                !fixed_width(&[component_style, item_type_style]),
            );
            if label {
                precision.label = part;
            } else {
                precision.content = part;
            }
            inner = inner.child(element);
            if label {
                if precision.label.is_some() {
                    precision.colon = Some(Default::default());
                }
                inner = inner.child(colon_element(
                    colon,
                    direction,
                    metrics,
                    precision.colon.clone(),
                ));
            }
            cell = cell.child(inner);
        }
    }
    let mut semantics = Accessibility::new(if label { Role::Term } else { Role::Definition });
    semantics.label = accessible_label;
    let cell = if bordered {
        component_border(
            cell,
            metrics,
            rem_size,
            semantics,
            border.decoration,
            border.queue,
            false,
        )
    } else {
        semantics.apply(cell).into_any_element()
    };
    (cell, precision)
}

fn colon_element(
    visible: bool,
    direction: Direction,
    metrics: DescriptionsStyle,
    measurement: Option<ColonHandle>,
) -> impl IntoElement {
    Colon {
        visible,
        direction,
        metrics,
        measurement,
    }
}

#[derive(IntoElement)]
struct Colon {
    visible: bool,
    direction: Direction,
    metrics: DescriptionsStyle,
    measurement: Option<ColonHandle>,
}

impl RenderOnce for Colon {
    fn render(self, window: &mut Window, _: &mut App) -> impl IntoElement {
        let mut colon = div()
            .id("colon")
            .flex_shrink_0()
            .relative()
            .top(px(self.metrics.colon_offset_y as f32));
        colon = if self.direction == Direction::Ltr {
            colon
                .ml(px(self.metrics.colon_margin_left as f32))
                .mr(px(self.metrics.colon_margin_right as f32))
        } else {
            colon
                .mr(px(self.metrics.colon_margin_left as f32))
                .ml(px(self.metrics.colon_margin_right as f32))
        };
        if self.visible {
            let style = window.text_style();
            let font_size = style.font_size.to_pixels(window.rem_size());
            let width = window
                .text_system()
                .shape_line(":".into(), font_size, &[style.to_run(1)], None)
                .width();
            // Explicit glyph advance avoids accumulating two independent text
            // leaf ceil operations for a label followed by its punctuation.
            colon = colon.w(window.pixel_snap(width)).child(":");
            if let Some(measurement) = self.measurement {
                measurement.set(Some((
                    f32::from(width),
                    f32::from(window.pixel_snap(width)),
                )));
            }
        } else if let Some(measurement) = self.measurement {
            measurement.set(Some((0., 0.)));
        }
        Accessibility {
            role: Some(Role::Label),
            hidden: true,
            ..Default::default()
        }
        .apply(colon)
    }
}

fn horizontal_item(
    item: DescriptionItem,
    colon: bool,
    direction: Direction,
    metrics: DescriptionsStyle,
    styles: &DescriptionsStyles,
    spacing: (gpui::Pixels, gpui::Pixels),
    last_cell: bool,
) -> (AnyElement, CellPrecision) {
    let accessible_label = item.resolved_accessibility_label();
    let mut precision = CellPrecision {
        preserve_native_preferred: true,
        ..Default::default()
    };
    let mut outer = div()
        .id((item.id.clone(), "item"))
        .flex()
        .flex_col()
        .w_full()
        .h_full()
        .min_w(px(0.))
        .pt(spacing.0)
        .pb(spacing.1)
        .when(direction == Direction::Ltr, |outer| outer.pl(px(1.)))
        .when(direction == Direction::Rtl, |outer| outer.pr(px(1.)));
    if !last_cell {
        outer = if direction == Direction::Ltr {
            outer.pr(px(metrics.item_padding_end as f32))
        } else {
            outer.pl(px(metrics.item_padding_end as f32))
        };
    }
    outer.style().refine(&item.style);
    // A table cell stretches to the complete row height; its inner flex pair
    // keeps its own content height and remains aligned to the cell's top.
    let mut pair = div()
        .flex()
        .w_full()
        .min_w(px(0.))
        .when(direction == Direction::Rtl, |pair| pair.flex_row_reverse());
    if let Some(label) = item.label {
        let mut semantics = Accessibility::new(Role::Term);
        semantics.label = accessible_label.clone();
        let mut label_slot = div()
            .id((item.id.clone(), "label"))
            .flex()
            .items_baseline()
            .text_color(metrics.label_color)
            .when(direction == Direction::Rtl, |label| {
                label.flex_row_reverse()
            });
        label_slot.style().refine(&styles.label);
        label_slot.style().refine(&item.styles.label);
        let (element, part) = cell_content(
            label,
            direction,
            false,
            false,
            !fixed_width(&[&styles.label, &item.styles.label]),
        );
        precision.label = part;
        if precision.label.is_some() {
            precision.colon = Some(Default::default());
        }
        pair = pair.child(
            semantics.apply(label_slot.child(element).child(colon_element(
                colon,
                direction,
                metrics,
                precision.colon.clone(),
            ))),
        );
    }
    if let Some(content) = item.content {
        let mut content_slot = div()
            .id((item.id, "content"))
            .flex()
            .flex_1()
            .when(direction == Direction::Rtl, |content| {
                content.flex_row_reverse()
            })
            .min_w(px(metrics.font_size as f32))
            .text_color(metrics.content_color);
        content_slot.style().refine(&styles.content);
        content_slot.style().refine(&item.styles.content);
        let (element, part) = cell_content(
            content,
            direction,
            true,
            true,
            !fixed_width(&[&styles.content, &item.styles.content]),
        );
        precision.content = part;
        let mut semantics = Accessibility::new(Role::Definition);
        semantics.label = accessible_label;
        pair = pair.child(semantics.apply(content_slot.child(element)));
    }
    (outer.child(pair).into_any_element(), precision)
}

fn cell_content(
    content: Content,
    direction: Direction,
    anywhere: bool,
    minimum_em: bool,
    measure: bool,
) -> (AnyElement, Option<TextPart>) {
    match content {
        Content::Text(text) if measure => {
            let metrics = Default::default();
            let element = crate::behavior::text::measured_paragraph_text(
                text,
                direction,
                anywhere,
                std::rc::Rc::clone(&metrics),
            );
            (
                element,
                Some(TextPart {
                    metrics,
                    minimum_em,
                }),
            )
        }
        content => (content.into_element(direction, anywhere), None),
    }
}
