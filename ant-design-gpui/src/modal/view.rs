// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
use super::{
    ContentSource, ModalAutoFocus, ModalCancelReason, ModalContent, ModalEvent, ModalFooter,
    ModalFooterActions, ModalHandle, ModalKind, ModalOptions, ModalPhase, ModalState,
    ModalStyleContext, ModalTheme, host, state,
};
use crate::{
    Button, ConfigScope, Direction, ScrollArea,
    behavior::{Accessibility, FocusNext, FocusPrevious},
    button::ButtonType,
    icons::{IconName, icon},
};
use gpui::{
    AnyElement, App, AvailableSpace, Bounds, Element, ElementId, Entity, FontWeight,
    GlobalElementId, InspectorElementId, IntoElement, LayoutId, MouseButton, Pixels, Refineable,
    RenderOnce, Role, SharedString, Styled, Subscription, WeakEntity, Window, canvas, div, point,
    prelude::*, px, relative, size,
};
use std::rc::Rc;

/// Fresh builders share the retained controller, content and focus handles.
#[derive(IntoElement)]
pub struct Modal {
    id: ElementId,
    state: Entity<ModalState>,
    options: ModalOptions,
    scope: Option<ConfigScope>,
}
impl Modal {
    pub fn new(id: impl Into<ElementId>, state: &Entity<ModalState>, cx: &App) -> Self {
        Self {
            id: id.into(),
            state: state.clone(),
            options: state.read(cx).options.clone(),
            scope: None,
        }
    }
    pub fn options(mut self, options: ModalOptions) -> Self {
        self.options = options;
        self
    }
    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }
    pub fn title(mut self, text: impl Into<SharedString>) -> Self {
        self.options.title = Some(ModalContent::text(text));
        self
    }
    pub fn content(mut self, content: ModalContent) -> Self {
        self.options.content = Some(content);
        self
    }
    pub fn footer(mut self, footer: ModalFooter) -> Self {
        self.options.footer = footer;
        self
    }
    pub fn width(mut self, width: impl Into<super::ModalWidth>) -> Self {
        self.options.width = Some(width.into());
        self
    }
    pub fn centered(mut self, centered: bool) -> Self {
        self.options.centered = Some(centered);
        self
    }
    pub fn on_event(
        mut self,
        handler: impl Fn(&ModalEvent, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.options.on_event = Some(Rc::new(handler));
        self
    }
}
struct Mount {
    state: WeakEntity<ModalState>,
    generation: u64,
    window: gpui::AnyWindowHandle,
    _observe: Subscription,
    _release: Subscription,
}
impl RenderOnce for Modal {
    fn render(self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let observed = self.state.clone();
        let _mount = window.with_element_namespace(self.id.clone(), |w| {
            w.use_keyed_state(("modal-mount", observed.entity_id()), cx, move |w, cx| {
                let generation = observed.update(cx, |s, _| {
                    s.mounted = true;
                    s.mount_generation = s.mount_generation.wrapping_add(1);
                    s.mount_generation
                });
                Mount {
                    state: observed.downgrade(),
                    generation,
                    window: w.window_handle(),
                    _observe: cx.observe(&observed, |_, _, cx| cx.notify()),
                    _release: cx.on_release(|mount: &mut Mount, cx| {
                        let target = mount.window;
                        let state = mount.state.clone();
                        let generation = mount.generation;
                        cx.defer(move |cx| {
                            let _ = target.update(cx, |_, w, cx| {
                                let Some(entity) = state.upgrade() else {
                                    return;
                                };
                                if entity.read(cx).mount_generation != generation {
                                    return;
                                }
                                let focus = entity.update(cx, |s, cx| {
                                    let restore = s
                                        .options
                                        .focus_return
                                        .or(s.scope.snapshot(cx).modal.focus_return)
                                        .unwrap_or(true);
                                    let focus = s.return_focus.take().filter(|_| restore);
                                    s.unmount(cx);
                                    focus
                                });
                                crate::popup::host::dismiss_modal(Some(entity.entity_id()), w, cx);
                                host::close_descendants(entity.entity_id(), w, cx);
                                host::retire(entity.entity_id(), w, cx);
                                if let Some(focus) = focus {
                                    w.focus(&focus, cx);
                                }
                                host::restore_focus(w, cx);
                            });
                        });
                    }),
                }
            })
        });
        self.state.update(cx, |s, cx| {
            if let Some(scope) = self.scope {
                s.set_scope(scope, cx);
            }
            // Copy configuration without recursively invalidating the builder's observer.
            s.replace_options(self.options);
        });
        Marker {
            id: self.id,
            state: self.state,
            child: div().size_0().into_any_element(),
        }
    }
}
struct Marker {
    id: ElementId,
    state: Entity<ModalState>,
    child: AnyElement,
}
impl IntoElement for Marker {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for Marker {
    type RequestLayoutState = ();
    type PrepaintState = ();
    fn id(&self) -> Option<ElementId> {
        Some(self.id.clone())
    }
    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        None
    }
    fn request_layout(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        w: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, ()) {
        if self.state.read(cx).open {
            host::activate(&self.state, self.state.read(cx).options.z_index, w, cx);
        }
        initialize_content(&self.state, w, cx);
        (self.child.request_layout(w, cx), ())
    }
    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        w: &mut Window,
        cx: &mut App,
    ) {
        self.child.prepaint(w, cx);
    }
    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        _: &mut (),
        w: &mut Window,
        cx: &mut App,
    ) {
        self.child.paint(w, cx);
    }
}

fn initialize_content(state: &Entity<ModalState>, window: &mut Window, cx: &mut App) {
    for slot in 0..3 {
        let factory = {
            let s = state.read(cx);
            if !s.open && !s.options.force_render {
                return;
            }
            let (view, content) = match slot {
                0 => (&s.title_view, s.options.title.as_ref()),
                1 => (&s.body_view, s.options.content.as_ref()),
                _ => (
                    &s.footer_view,
                    match &s.options.footer {
                        ModalFooter::Content(c) => Some(c),
                        _ => None,
                    },
                ),
            };
            if view.is_some() || (slot != 0 && s.options.loading) {
                continue;
            }
            content.and_then(|c| match &c.0 {
                ContentSource::View(f) => Some(f.clone()),
                _ => None,
            })
        };
        if let Some(factory) = factory {
            let view = factory(window, cx);
            state.update(cx, |s, _| match slot {
                0 => s.title_view = Some(view),
                1 => s.body_view = Some(view),
                _ => s.footer_view = Some(view),
            });
        }
    }
}
fn slot(
    content: &ModalContent,
    handle: &ModalHandle,
    window: &mut Window,
    cx: &mut App,
) -> AnyElement {
    match &content.0 {
        ContentSource::Text(text) => {
            let direction = handle.state().map_or(crate::Direction::Ltr, |s| {
                s.read(cx).scope.snapshot(cx).direction
            });
            crate::behavior::paragraph_text("modal-text", text.clone(), direction)
        }
        ContentSource::Render(render) => render(handle, window, cx),
        ContentSource::View(factory) => factory(window, cx).into_any_element(),
    }
}

pub(super) fn draw(state: &Entity<ModalState>, window: &mut Window, cx: &mut App) {
    initialize_content(state, window, cx);
    if state.read(cx).phase == ModalPhase::Closed {
        return;
    }
    let opacity = state.update(cx, |s, cx| s.sample(window, cx));
    if state.read(cx).phase == ModalPhase::Closed {
        return;
    }
    let id = state.entity_id();
    let viewport = window.viewport_size();
    let mut surface = host::with_owner(id, window, cx, |w, cx| {
        w.with_element_namespace(("modal-surface", id), |w| {
            render_surface(state, opacity, w, cx)
        })
    });
    surface = OwnedSurface {
        state: state.clone(),
        child: surface,
    }
    .into_any_element();
    host::with_owner(id, window, cx, |w, cx| {
        surface.layout_as_root(
            size(
                AvailableSpace::Definite(viewport.width),
                AvailableSpace::Definite(viewport.height),
            ),
            w,
            cx,
        )
    });
    let priority = host::priority(id, window, cx);
    window.defer_draw(surface, point(px(0.), px(0.)), priority, None);
}

struct OwnedSurface {
    state: Entity<ModalState>,
    child: AnyElement,
}
impl IntoElement for OwnedSurface {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for OwnedSurface {
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
        w: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, ()) {
        (self.child.request_layout(w, cx), ())
    }
    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        w: &mut Window,
        cx: &mut App,
    ) {
        host::with_owner(self.state.entity_id(), w, cx, |w, cx| {
            self.child.prepaint(w, cx)
        });
        let focus = self.state.update(cx, |s, cx| {
            if !std::mem::take(&mut s.needs_focus) {
                return None;
            }
            let config = s.scope.snapshot(cx);
            let ok = s.options.ok_button.resolve(&config.modal.ok_button);
            let cancel = s.options.cancel_button.resolve(&config.modal.cancel_button);
            let has_actions = matches!(
                s.options.footer,
                ModalFooter::Default | ModalFooter::Render(_)
            ) && !s.options.loading;
            let has_cancel = s.options.show_cancel.unwrap_or(matches!(
                s.options.kind,
                ModalKind::Standard | ModalKind::Confirm
            ));
            s.options
                .initial_focus
                .clone()
                .or_else(|| match s.options.auto_focus {
                    ModalAutoFocus::Dialog => Some(s.focus.clone()),
                    ModalAutoFocus::Ok
                        if has_actions
                            && !ok.disabled.unwrap_or(false)
                            && !ok.loading.unwrap_or(false)
                            && !s.confirm_loading =>
                    {
                        Some(s.ok_focus.clone())
                    }
                    ModalAutoFocus::Cancel
                        if has_actions
                            && has_cancel
                            && !cancel.disabled.unwrap_or(false)
                            && !cancel.loading.unwrap_or(false) =>
                    {
                        Some(s.cancel_focus.clone())
                    }
                    ModalAutoFocus::None => None,
                    _ => Some(s.focus.clone()),
                })
        });
        if let Some(focus) = focus {
            let id = self.state.entity_id();
            w.defer(cx, move |w, cx| {
                if host::is_top(id, w, cx) {
                    w.focus(&focus, cx);
                    host::restore_focus(w, cx);
                }
            });
        }
    }
    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        _: &mut (),
        w: &mut Window,
        cx: &mut App,
    ) {
        self.child.paint(w, cx);
    }
}

fn render_surface(
    state: &Entity<ModalState>,
    opacity: f32,
    window: &mut Window,
    cx: &mut App,
) -> AnyElement {
    let (options, scope, focus, start, end, close_focus, scroll, body_view, busy, open) = {
        let s = state.read(cx);
        (
            s.options.clone(),
            s.scope.clone(),
            s.focus.clone(),
            s.start_focus.clone(),
            s.end_focus.clone(),
            s.close_focus.clone(),
            s.scroll.clone(),
            s.body_view.clone(),
            s.confirm_loading,
            s.open,
        )
    };
    let config = scope.snapshot(cx);
    let g = &config.theme.tokens;
    let t = config.theme.component::<ModalTheme>();
    let kind = options.kind;
    let standard = kind == ModalKind::Standard;
    let rtl = config.direction == Direction::Rtl;
    let handle = ModalHandle::from_entity(state, cx);
    let id = state.entity_id();
    let viewport = window.viewport_size();
    let viewport_surface = state.read(cx).viewport_surface;
    let width = if viewport_surface {
        f32::from(viewport.width)
    } else {
        super::style::width(options.width.as_ref(), kind, f32::from(viewport.width), g)
    };
    let mut styles = if viewport_surface {
        super::ModalStyles::default()
    } else {
        config.modal.styles.clone()
    };
    styles.refine(&options.styles);
    if let Some(resolve) = &options.styles_fn {
        styles.refine(&resolve(
            &ModalStyleContext {
                kind,
                loading: options.loading,
                confirm_loading: busy,
                direction: config.direction,
            },
            window,
            cx,
        ));
    }
    let title_view = state.read(cx).title_view.clone();
    let title = options.title.as_ref().map(|c| {
        title_view.clone().map_or_else(
            || slot(c, &handle, window, cx),
            IntoElement::into_any_element,
        )
    });
    let has_title = title.is_some();
    let body = if options.loading {
        super::skeleton::render(state, window, cx)
    } else if let Some(view) = body_view {
        view.into_any_element()
    } else if let Some(content) = &options.content {
        slot(content, &handle, window, cx)
    } else {
        div().into_any_element()
    };
    let mut body_slot = div()
        .id("body")
        .min_w_0()
        .text_size(px(g.font_size as f32))
        .line_height(relative(g.line_height as f32))
        .when(standard, |d| d.p(px(t.body_padding as f32)))
        .child(body);
    body_slot.style().refine(&styles.body);
    let mut container = div()
        .id("container")
        .relative()
        .flex()
        .flex_col()
        .w_full()
        .bg(t.content_bg)
        .rounded(px(g.border_radius_lg as f32))
        .px(px(t.content_padding.horizontal as f32))
        .py(px(t.content_padding.vertical as f32))
        .shadow(
            g.box_shadow
                .iter()
                .map(|s| gpui::BoxShadow {
                    inset: s.inset,
                    color: s.color.into(),
                    offset: point(px(s.x as f32), px(s.y as f32)),
                    blur_radius: px(s.blur as f32),
                    spread_radius: px(s.spread as f32),
                })
                .collect::<Vec<_>>(),
        );
    if standard {
        if let Some(title) = title {
            let mut heading = div()
                .id("title")
                .text_size(px(t.title_font_size as f32))
                .line_height(relative(t.title_line_height as f32))
                .font_weight(FontWeight(g.font_weight_strong as f32))
                .text_color(t.title_color)
                .child(title);
            heading.style().refine(&styles.title);
            let mut header = div()
                .id("header")
                .bg(t.header_bg)
                .px(px(t.header_padding.horizontal as f32))
                .py(px(t.header_padding.vertical as f32))
                .mb(px(t.header_margin_bottom as f32))
                .border_b(px(t.header_border_width as f32))
                .border_color(g.color_split)
                .child(heading);
            header.style().refine(&styles.header);
            container = container.child(header);
        }
        container = container.child(body_slot);
    } else {
        let default_icon = match kind {
            ModalKind::Info => IconName::InfoCircleFilled,
            ModalKind::Success => IconName::CheckCircleFilled,
            ModalKind::Error => IconName::CloseCircleFilled,
            _ => IconName::ExclamationCircleFilled,
        };
        let color = match kind {
            ModalKind::Info => g.color_info,
            ModalKind::Success => g.color_success,
            ModalKind::Error => g.color_error,
            _ => g.color_warning,
        };
        let mut paragraph = div()
            .flex()
            .flex_col()
            .flex_1()
            .min_w_0()
            .gap(px(g.margin_xs as f32));
        if let Some(title) = title {
            let mut heading = div()
                .id("title")
                .text_size(px(t.title_font_size as f32))
                .line_height(relative(t.title_line_height as f32))
                .font_weight(FontWeight(g.font_weight_strong as f32))
                .text_color(t.title_color)
                .child(title);
            heading.style().refine(&styles.title);
            paragraph = paragraph.child(heading);
        }
        paragraph = paragraph.child(body_slot);
        let mut row = div()
            .flex()
            .items_start()
            .when(rtl, |d| d.flex_row_reverse())
            .gap(px(t.confirm_icon_margin as f32));
        if options.show_icon {
            let extent = g.font_height as f32;
            let margin = if has_title {
                (t.title_font_size * t.title_line_height - g.font_height) / 2.
            } else {
                0.
            };
            row = row.child(
                div()
                    .flex_none()
                    .mt(px(margin as f32))
                    .text_color(color)
                    .child(
                        icon(options.icon.clone().unwrap_or_else(|| default_icon.into()))
                            .palette(crate::icons::IconPalette::from_theme(g))
                            .inherit_color()
                            .ant_design_frame()
                            .size(px(extent)),
                    ),
            );
        }
        container = container.child(
            row.child(paragraph)
                .px(px(t.confirm_body_padding.horizontal as f32))
                .pt(px(t.confirm_body_padding.vertical as f32)),
        );
    }
    if !options.loading && !matches!(options.footer, ModalFooter::None) {
        let footer = footer(state, &options, window, cx);
        let mut footer = div()
            .id("footer")
            .bg(t.footer_bg)
            .mt(px(if standard {
                t.footer_margin_top
            } else {
                t.confirm_buttons_margin_top
            } as f32))
            .px(px(if standard {
                t.footer_padding.horizontal
            } else {
                t.confirm_body_padding.horizontal
            } as f32))
            .pt(px(if standard {
                t.footer_padding.vertical
            } else {
                0.
            } as f32))
            .pb(px(if standard {
                t.footer_padding.vertical
            } else {
                t.confirm_body_padding_bottom
            } as f32))
            .border_t(px(if standard { t.footer_border_width } else { 0. } as f32))
            .border_color(g.color_split)
            .child(footer);
        footer.style().refine(&styles.footer);
        container = container.child(footer);
    }
    if options.closable.unwrap_or(standard) {
        let inset = ((g.line_height_heading5 * g.font_size_heading5 + 2. * g.padding)
            - g.control_height)
            / 2.;
        let target = state.clone();
        let close_scope = state.update(cx, |s, cx| s.close_scope(cx));
        let mut close = Button::new("close")
            .scope(close_scope)
            .button_type(ButtonType::Text)
            .icon(options.close_icon.clone())
            .accessibility_label(config.locale.common.close.clone())
            .automation_id("modal.close")
            .focus_handle(&close_focus)
            .disabled(options.close_disabled || !open)
            .wave(false)
            .size(crate::Size::Medium)
            .icon_style(gpui::StyleRefinement::default().text_size(px(g.font_size_lg as f32)))
            .w(px(g.control_height as f32))
            .h(px(g.control_height as f32))
            .p_0()
            .rounded(px(g.border_radius_sm as f32))
            .on_click(move |_, w, cx| {
                state::activate(&target, Some(ModalCancelReason::Close), w, cx)
            });
        close.style().refine(&styles.close);
        container = container.child(
            div()
                .absolute()
                .top(px(inset as f32))
                .when(rtl, |d| d.left(px(inset as f32)))
                .when(!rtl, |d| d.right(px(inset as f32)))
                .child(close),
        );
    }
    container.style().refine(&styles.container);
    let target = state.clone();
    let measured = canvas(
        move |bounds, _, cx| {
            target.update(cx, |s, _| s.bounds = bounds);
        },
        |_, _, _, _| {},
    )
    .absolute()
    .inset_0()
    .size_full();
    let mut panel = div()
        .id("dialog")
        .relative()
        .flex_none()
        .w(px(width))
        .track_focus(&focus)
        .tab_group()
        .key_context("AntDesign")
        .on_action(|_: &FocusNext, w, cx| {
            host::traverse(false, w, cx);
            cx.stop_propagation();
        })
        .on_action(|_: &FocusPrevious, w, cx| {
            host::traverse(true, w, cx);
            cx.stop_propagation();
        })
        .child(div().id("focus-start").size_0().track_focus(&start))
        .child(if let Some(render) = &options.surface_render {
            render(container.into_any_element(), &handle, window, cx)
        } else {
            container.into_any_element()
        })
        .child(div().id("focus-end").size_0().track_focus(&end))
        .child(measured);
    panel.style().refine(&styles.root);
    let mut semantics = Accessibility::new(if standard {
        Role::Dialog
    } else {
        Role::AlertDialog
    });
    semantics.label = options
        .accessibility_label
        .clone()
        .or_else(|| options.title.as_ref().and_then(ModalContent::label));
    semantics.description = options.accessibility_description.clone();
    semantics.modal = state.read(cx).trap(cx);
    semantics.busy = busy || options.loading || state.read(cx).is_pending();
    semantics.hidden = !open || !host::is_top(id, window, cx);
    semantics.automation_id = options
        .automation_id
        .clone()
        .or_else(|| Some("modal.dialog".into()));
    let centered = options.centered.unwrap_or(config.modal.centered);
    let narrow = f64::from(f32::from(viewport.width)) <= g.screen_sm_max;
    let mut wrapper = div()
        .flex()
        .flex_col()
        .items_center()
        .min_h(viewport.height)
        .w_full()
        .when(centered, |d| {
            d.justify_center()
                .py(px(if narrow { g.margin_xs as f32 } else { 0. }))
        })
        .when(!centered, |d| {
            d.pt(px(
                options.top.max(0.) + if narrow { g.margin_xs as f32 } else { 0. }
            ))
            .pb(px(g.padding_lg as f32))
        })
        .child(semantics.apply(panel));
    wrapper.style().refine(&styles.wrapper);
    let mask = options.mask.or(config.modal.mask).unwrap_or(true);
    let mut mask_element = div()
        .absolute()
        .size_full()
        .when(mask, |d| d.bg(g.color_bg_mask));
    mask_element.style().refine(&styles.mask);
    let target = state.clone();
    let release = state.clone();
    let closable_mask = options
        .mask_closable
        .or(config.modal.mask_closable)
        .unwrap_or(standard);
    let layer = div()
        .id(("modal-layer", id))
        .relative()
        .w(viewport.width)
        .h(viewport.height)
        .when(options.scroll_lock || !open, |d| d.occlude())
        .opacity(opacity)
        .font(config.font.clone())
        .text_color(g.color_text)
        .text_size(px(g.font_size as f32))
        .line_height(relative(g.line_height as f32))
        .when(rtl, |d| d.text_right())
        .on_mouse_down(MouseButton::Left, move |e, w, cx| {
            let outside = host::is_top(id, w, cx)
                && !target.read(cx).bounds.contains(&e.position)
                && !crate::popup::host::modal_contains_position(id, e.position, w, cx);
            target.update(cx, |s, _| s.mask_down = outside);
            cx.stop_propagation();
        })
        .on_mouse_down(MouseButton::Right, |_, _, cx| cx.stop_propagation())
        .on_mouse_up(MouseButton::Right, |_, _, cx| cx.stop_propagation())
        .on_mouse_down(MouseButton::Middle, |_, _, cx| cx.stop_propagation())
        .on_mouse_up(MouseButton::Middle, |_, _, cx| cx.stop_propagation())
        .on_mouse_up(MouseButton::Left, move |e, w, cx| {
            let pressed = release.update(cx, |s, _| std::mem::take(&mut s.mask_down));
            if pressed
                && closable_mask
                && host::is_top(id, w, cx)
                && !release.read(cx).bounds.contains(&e.position)
                && !crate::popup::host::modal_contains_position(id, e.position, w, cx)
            {
                state::activate(&release, Some(ModalCancelReason::Mask), w, cx);
            }
            cx.stop_propagation();
        })
        .on_scroll(move |_, _, cx| {
            if options.scroll_lock {
                cx.stop_propagation();
            }
        })
        .child(mask_element)
        .child(
            ScrollArea::new("modal-scroll", &scroll)
                .scope(scope)
                .embedded()
                .w(viewport.width)
                .h(viewport.height)
                .child(wrapper),
        )
        .when(!open, |layer| {
            layer.child(
                div()
                    .absolute()
                    .inset_0()
                    .occlude()
                    .on_mouse_down(MouseButton::Left, |_, _, cx| cx.stop_propagation())
                    .on_mouse_up(MouseButton::Left, |_, _, cx| cx.stop_propagation())
                    .on_mouse_down(MouseButton::Right, |_, _, cx| cx.stop_propagation())
                    .on_scroll(|_, _, cx| cx.stop_propagation()),
            )
        });
    layer.into_any_element()
}

fn footer(
    state: &Entity<ModalState>,
    options: &ModalOptions,
    window: &mut Window,
    cx: &mut App,
) -> AnyElement {
    let s = state.read(cx);
    let config = s.scope.snapshot(cx);
    let cancel = options.show_cancel.unwrap_or(matches!(
        options.kind,
        ModalKind::Standard | ModalKind::Confirm
    ));
    let ok_text = options.ok_text.clone().unwrap_or_else(|| {
        if cancel {
            config.locale.modal.ok_text.clone()
        } else {
            config.locale.modal.just_ok_text.clone()
        }
    });
    let cancel_text = options
        .cancel_text
        .clone()
        .unwrap_or_else(|| config.locale.modal.cancel_text.clone());
    let ok_options = options.ok_button.resolve(&config.modal.ok_button);
    let cancel_options = options.cancel_button.resolve(&config.modal.cancel_button);
    let target = state.clone();
    let cancel_target = state.clone();
    let mut ok = Button::new("ok")
        .scope(s.scope.clone())
        .label(ok_text)
        .button_type(ok_options.button_type.unwrap_or(ButtonType::Primary))
        .danger(ok_options.danger.unwrap_or(false))
        .disabled(ok_options.disabled.unwrap_or(false) || !s.open)
        .loading(s.confirm_loading || s.pending_ok || ok_options.loading.unwrap_or(false))
        .focus_handle(&s.ok_focus)
        .automation_id("modal.ok")
        .on_click(move |_, w, cx| state::activate(&target, None, w, cx));
    ok.style().refine(&ok_options.style);
    let cancel = cancel.then(|| {
        let mut button = Button::new("cancel")
            .scope(s.scope.clone())
            .label(cancel_text)
            .button_type(cancel_options.button_type.unwrap_or(ButtonType::Default))
            .danger(cancel_options.danger.unwrap_or(false))
            .disabled(cancel_options.disabled.unwrap_or(false) || !s.open)
            .loading(s.pending_cancel || cancel_options.loading.unwrap_or(false))
            .focus_handle(&s.cancel_focus)
            .automation_id("modal.cancel")
            .on_click(move |_, w, cx| {
                state::activate(&cancel_target, Some(ModalCancelReason::Button), w, cx)
            });
        button.style().refine(&cancel_options.style);
        button.into_any_element()
    });
    let handle = ModalHandle::from_entity(state, cx);
    let ok = ok.into_any_element();
    match &options.footer {
        ModalFooter::Content(content) => {
            if let Some(view) = state.read(cx).footer_view.clone() {
                view.into_any_element()
            } else {
                slot(content, &handle, window, cx)
            }
        }
        ModalFooter::Render(render) => {
            render(ModalFooterActions { ok, cancel }, &handle, window, cx)
        }
        _ => div()
            .flex()
            .justify_end()
            .items_center()
            .gap(px(config.theme.tokens.margin_xs as f32))
            .when(config.direction == Direction::Rtl, |d| {
                d.flex_row_reverse().justify_start()
            })
            .children(cancel)
            .child(ok)
            .into_any_element(),
    }
}
