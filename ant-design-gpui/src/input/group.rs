// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Explicit composition of adjacent controls, without an ambient render context.

use std::{
    cell::{Cell, RefCell},
    rc::Rc,
};

use gpui::{
    AnyElement, App, Bounds, Context, Element, ElementId, GlobalElementId, InspectorElementId,
    IntoElement, LayoutId, Pixels, Refineable, RenderOnce, Role, SharedString, Style,
    StyleRefinement, Styled, Subscription, Window, canvas, div, prelude::*, px,
};

use crate::{ConfigScope, Direction, Size, behavior::Accessibility};

/// The axis along which adjacent borders are joined.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum InputGroupOrientation {
    #[default]
    Horizontal,
    Vertical,
}

/// Configuration passed explicitly to a grouped control.
///
/// Library controls implement `InputGroupItem`. Custom controls can use this
/// context's size, scope and frame methods to participate in the same layout.
#[derive(Clone)]
pub struct InputGroupContext {
    pub scope: ConfigScope,
    pub size: Size,
    pub direction: Direction,
    pub orientation: InputGroupOrientation,
    pub first: bool,
    pub last: bool,
    pub disabled: Option<bool>,
    overlap: Pixels,
    priority: Rc<Cell<u8>>,
    paint: PaintQueue,
    opacity: f32,
}

impl InputGroupContext {
    /// Apply logical outer corners and the one-border overlap after local styling.
    pub fn apply_style(&self, style: &mut StyleRefinement) {
        let zero = Some(px(0.).into());
        match self.orientation {
            InputGroupOrientation::Horizontal => {
                let (left, right) = self.direction.horizontal(self.first, self.last);
                if !left {
                    style.corner_radii.top_left = zero;
                    style.corner_radii.bottom_left = zero;
                }
                if !right {
                    style.corner_radii.top_right = zero;
                    style.corner_radii.bottom_right = zero;
                }
                match self.direction {
                    Direction::Ltr => style.margin.right = Some((-self.overlap).into()),
                    Direction::Rtl => style.margin.left = Some((-self.overlap).into()),
                }
            }
            InputGroupOrientation::Vertical => {
                if !self.first {
                    style.corner_radii.top_left = zero;
                    style.corner_radii.top_right = zero;
                }
                if !self.last {
                    style.corner_radii.bottom_left = zero;
                    style.corner_radii.bottom_right = zero;
                }
                style.margin.bottom = Some((-self.overlap).into());
            }
        }
    }

    /// Raise a hovered/focused control above adjacent borders, within this group.
    /// Layout, tab order and accessibility order remain in declaration order.
    pub fn set_interaction(&self, disabled: bool, hovered: bool, focused: bool) {
        self.priority.set(self.priority.get().max(if disabled {
            0
        } else if hovered {
            4
        } else if focused {
            3
        } else {
            2
        }));
    }

    pub(crate) fn part(&self, first: bool, last: bool) -> Self {
        Self {
            first: self.first && (first || self.orientation == InputGroupOrientation::Vertical),
            last: self.last && (last || self.orientation == InputGroupOrientation::Vertical),
            overlap: px(0.),
            ..self.clone()
        }
    }

    pub(crate) fn with_opacity(&self, opacity: f32) -> Self {
        Self {
            opacity: self.opacity * opacity,
            ..self.clone()
        }
    }

    /// Finish a custom control's surface after applying its style and interaction.
    /// Pass a focus ring only when it should be visible. This adds no hitbox or Tab stop.
    pub fn decorate(
        &self,
        root: gpui::Stateful<gpui::Div>,
        focus: Option<crate::behavior::FocusRing>,
        window: &Window,
    ) -> gpui::Stateful<gpui::Div> {
        self.decorate_with_border(root, focus, window, None)
    }

    pub(crate) fn decorate_with_border(
        &self,
        mut root: gpui::Stateful<gpui::Div>,
        focus: Option<crate::behavior::FocusRing>,
        window: &Window,
        custom_border: Option<BorderPainter>,
    ) -> gpui::Stateful<gpui::Div> {
        let mut style = Style::default();
        style.refine(root.style());
        if style.visibility == gpui::Visibility::Hidden {
            return root;
        }
        let borders = style.border_widths.to_pixels(window.rem_size());
        let radii = style.corner_radii.to_pixels(window.rem_size());
        let opacity = self.opacity * style.opacity.unwrap_or(1.);
        let shadows: Rc<[gpui::BoxShadow]> = std::mem::take(&mut style.box_shadow)
            .into_iter()
            .map(|shadow| gpui::BoxShadow {
                color: shadow.color.opacity(opacity),
                ..shadow
            })
            .collect();
        let queue = self.paint.clone();
        let priority = self.priority.clone();
        root.style().border_color = Some(gpui::Hsla::transparent_black());
        root.style().box_shadow = Some(Vec::new());
        root.child(
            div()
                .absolute()
                .top(-borders.top)
                .bottom(-borders.bottom)
                .left(-borders.left)
                .right(-borders.right)
                .child(
                    canvas(
                        |_, _, _| (),
                        move |bounds, (), window, _| {
                            queue.0.borrow_mut().push(Decoration {
                                bounds,
                                radii,
                                borders,
                                color: style.border_color.unwrap_or_default(),
                                border_style: style.border_style,
                                shadows: shadows.clone(),
                                focus,
                                custom_border,
                                opacity,
                                mask: window.content_mask(),
                                priority: priority.get(),
                            });
                        },
                    )
                    .size_full(),
                ),
        )
    }
}

/// A fitted control border can participate in the group's deferred paint order.
/// Opacity includes both the control and nested groups and is applied once.
pub(crate) type BorderPainter = Box<dyn Fn(Bounds<Pixels>, f32, &mut Window, &mut App)>;

#[derive(Clone, Default)]
struct PaintQueue(Rc<RefCell<Vec<Decoration>>>);
struct Decoration {
    bounds: Bounds<Pixels>,
    radii: gpui::Corners<Pixels>,
    borders: gpui::Edges<Pixels>,
    color: gpui::Hsla,
    border_style: gpui::BorderStyle,
    shadows: Rc<[gpui::BoxShadow]>,
    focus: Option<crate::behavior::FocusRing>,
    custom_border: Option<BorderPainter>,
    opacity: f32,
    mask: gpui::ContentMask<Pixels>,
    priority: u8,
}
impl PaintQueue {
    fn paint(&self, window: &mut Window, cx: &mut App) {
        let mut records = self.0.borrow_mut();
        records.sort_by_key(|record| record.priority);
        for record in &*records {
            if record.bounds.is_empty() || record.opacity <= 0. {
                continue;
            }
            let opacity = record.opacity;
            let radii = record.radii.clamp_radii_for_quad_size(record.bounds.size);
            window.with_content_mask(Some(record.mask), |window| {
                window.paint_drop_shadows(record.bounds, radii, &record.shadows);
                window.paint_inset_shadows(record.bounds, radii, &record.shadows);
                if record.color.a > 0. {
                    window.paint_quad(
                        gpui::outline(
                            record.bounds,
                            record.color.opacity(opacity),
                            record.border_style,
                        )
                        .border_widths(record.borders)
                        .corner_radii(radii),
                    );
                }
                if let Some(paint) = &record.custom_border {
                    paint(record.bounds, opacity, window, cx);
                }
                if let Some(focus) = record.focus {
                    let extent = focus.offset + focus.width;
                    window.paint_quad(
                        gpui::outline(
                            record.bounds.dilate(extent),
                            focus.color.opacity(opacity),
                            Default::default(),
                        )
                        .border_widths(focus.width)
                        .corner_radii(radii.map(|r| *r + extent)),
                    );
                }
            });
        }
    }
}

/// A control whose corners and configuration can be composed by `InputGroup`.
pub trait InputGroupItem: Sized + 'static {
    fn into_group_element(self, context: InputGroupContext) -> AnyElement;
}

type Item = Box<dyn FnOnce(InputGroupContext) -> AnyElement>;

/// Joined controls with independent state and normal keyboard traversal.
/// Use `Styled` widths/flex properties on each child to choose its share of space.
#[derive(IntoElement)]
pub struct InputGroup {
    id: ElementId,
    scope: Option<ConfigScope>,
    size: Option<Size>,
    orientation: InputGroupOrientation,
    block: bool,
    disabled: Option<bool>,
    items: Vec<Item>,
    parent: Option<InputGroupContext>,
    accessibility: Accessibility,
    style: StyleRefinement,
}

impl InputGroup {
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            id: id.into(),
            scope: None,
            size: None,
            orientation: InputGroupOrientation::Horizontal,
            block: false,
            disabled: None,
            items: Vec::new(),
            parent: None,
            accessibility: Accessibility::new(Role::Group),
            style: StyleRefinement::default(),
        }
    }
    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }
    pub fn size(mut self, size: Size) -> Self {
        self.size = Some(size);
        self
    }
    pub fn orientation(mut self, orientation: InputGroupOrientation) -> Self {
        self.orientation = orientation;
        self
    }
    pub fn block(mut self, block: bool) -> Self {
        self.block = block;
        self
    }
    /// Inherited by children; a child's explicit setting takes precedence.
    pub fn disabled(mut self, disabled: bool) -> Self {
        self.disabled = Some(disabled);
        self
    }
    pub fn child(mut self, child: impl InputGroupItem) -> Self {
        self.items
            .push(Box::new(move |context| child.into_group_element(context)));
        self
    }
    pub fn children<I: InputGroupItem>(mut self, children: impl IntoIterator<Item = I>) -> Self {
        self.items.extend(
            children
                .into_iter()
                .map(|child| Box::new(move |context| child.into_group_element(context)) as Item),
        );
        self
    }
    pub fn accessibility_label(mut self, label: impl Into<SharedString>) -> Self {
        self.accessibility.label = Some(label.into());
        self
    }
    pub fn accessibility_description(mut self, description: impl Into<SharedString>) -> Self {
        self.accessibility.description = Some(description.into());
        self
    }
    pub fn automation_id(mut self, id: impl Into<SharedString>) -> Self {
        self.accessibility.automation_id = Some(id.into());
        self
    }
}
impl Styled for InputGroup {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}
impl InputGroupItem for InputGroup {
    fn into_group_element(mut self, context: InputGroupContext) -> AnyElement {
        self.scope = self.scope.or_else(|| Some(context.scope.clone()));
        self.size = self.size.or(Some(context.size));
        self.disabled = self.disabled.or(context.disabled);
        self.parent = Some(context);
        self.into_any_element()
    }
}
struct MountedGroup {
    scope: ConfigScope,
    paint: PaintQueue,
    _scope: Subscription,
}
impl RenderOnce for InputGroup {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self.scope.unwrap_or_else(|| ConfigScope::application(cx));
        let mounted = window.use_keyed_state(
            (self.id.clone(), "group"),
            cx,
            |_, cx: &mut Context<MountedGroup>| MountedGroup {
                _scope: scope.observe(cx),
                scope: scope.clone(),
                paint: PaintQueue::default(),
            },
        );
        mounted.update(cx, |state, cx| {
            if state.scope != scope {
                state._scope = scope.observe(cx);
                state.scope = scope.clone();
            }
        });
        let config = scope.snapshot(cx);
        let size = self.size.unwrap_or(config.size);
        let count = self.items.len();
        let paint = self
            .parent
            .as_ref()
            .map(|parent| parent.paint.clone())
            .unwrap_or_else(|| mounted.read(cx).paint.clone());
        let same_axis = self
            .parent
            .as_ref()
            .filter(|parent| parent.orientation == self.orientation);
        let children = self
            .items
            .into_iter()
            .enumerate()
            .map(|(index, item)| {
                let priority = Rc::new(Cell::new(0));
                let element = item(InputGroupContext {
                    scope: scope.clone(),
                    size,
                    direction: config.direction,
                    orientation: self.orientation,
                    first: index == 0 && same_axis.is_none_or(|parent| parent.first),
                    last: index + 1 == count && same_axis.is_none_or(|parent| parent.last),
                    disabled: self.disabled,
                    overlap: px(if index + 1 == count {
                        0.
                    } else {
                        config.theme.tokens.line_width as f32
                    }),
                    priority: priority.clone(),
                    paint: paint.clone(),
                    opacity: self.parent.as_ref().map_or(1., |parent| {
                        parent.opacity * self.style.opacity.unwrap_or(1.)
                    }),
                });
                GroupChild { element }
            })
            .collect();
        let mut row_style = StyleRefinement::default().flex().min_w_0().flex_1();
        row_style = match self.orientation {
            InputGroupOrientation::Horizontal => {
                let style = row_style.items_stretch();
                if config.direction == Direction::Rtl {
                    style.flex_row_reverse()
                } else {
                    style
                }
            }
            InputGroupOrientation::Vertical => row_style.flex_col().items_stretch(),
        };
        let mut row = Style::default();
        row.refine(&row_style);
        let mut root = div()
            .id(self.id)
            .flex()
            .min_w_0()
            .when(self.block, |root| root.w_full());
        root.style().refine(&self.style);
        if let Some(parent) = &self.parent {
            parent.apply_style(root.style());
        }
        self.accessibility.hidden = count == 0;
        self.accessibility.apply(root.child(GroupRow {
            children,
            style: row,
            paint,
            paint_owner: self.parent.is_none(),
        }))
    }
}

struct GroupChild {
    element: AnyElement,
}

/// Controls paint in source order: GPUI registers Tab stops during paint.
/// Only border/focus decoration is raised, inside normal ancestor clipping.
struct GroupRow {
    children: Vec<GroupChild>,
    style: Style,
    paint: PaintQueue,
    paint_owner: bool,
}
impl IntoElement for GroupRow {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for GroupRow {
    type RequestLayoutState = ();
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
        cx: &mut App,
    ) -> (LayoutId, ()) {
        let layouts = self
            .children
            .iter_mut()
            .map(|child| child.element.request_layout(window, cx))
            .collect::<Vec<_>>();
        (window.request_layout(self.style.clone(), layouts, cx), ())
    }
    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) {
        for child in &mut self.children {
            child.element.prepaint(window, cx);
        }
    }
    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) {
        if self.paint_owner {
            self.paint.0.borrow_mut().clear();
        }
        for child in &mut self.children {
            child.element.paint(window, cx);
        }
        if self.paint_owner {
            self.paint.paint(window, cx);
        }
    }
}

#[cfg(test)]
mod tests;
