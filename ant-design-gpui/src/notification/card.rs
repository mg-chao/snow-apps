// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    Content, NotificationCloseReason, NotificationConfig, NotificationState,
    NotificationStyleContext, NotificationStyles, NotificationTokens, NotificationType,
    state::RenderNotice,
};
use crate::{
    ConfigScope, Direction,
    behavior::{
        Accessibility, ActivationGuard, Availability, FocusRing, MotionPolicy, ScalarTransition,
        notice::{ContentProbe, FocusBoundary, LiveSurface},
    },
    icons::{IconName, IconPalette, IconRenderOptions, icon},
};
use gpui::{
    AnyElement, App, AppContext, AvailableSpace, Context, Entity, FocusHandle, IntoElement,
    LayoutId, Pixels, Refineable, Render, Role, Size, StyleRefinement, Subscription, Window, div,
    prelude::*, px, relative, size,
};
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
    sync::Arc,
    time::Instant,
};

pub(super) struct NoticeVisual {
    pub boundary: FocusBoundary,
    pub guard: ActivationGuard,
    pub presented: bool,
    pub dirty: bool,
    focus: FocusHandle,
    hovered: Rc<Cell<bool>>,
    hover: ScalarTransition,
    body: Option<Entity<Body>>,
    body_layout: Rc<Cell<Option<LayoutId>>>,
    body_size: Option<Size<Pixels>>,
    width: Pixels,
    scale: f32,
    context: Option<NotificationStyleContext>,
    _blur: Subscription,
    _focus_in: Subscription,
    _focus_out: Subscription,
}
impl NoticeVisual {
    pub fn new(
        state: &Entity<NotificationState>,
        window: &mut Window,
        cx: &mut App,
        tokens: &NotificationTokens,
    ) -> Self {
        let boundary = FocusBoundary::new(window, cx);
        let focus = crate::behavior::focus_handle(cx);
        let guard = ActivationGuard::default();
        let blur = guard.observe_blur(&focus, window, cx);
        let weak = state.downgrade();
        let focus_in = window.on_focus_in(&boundary.0.root, cx, move |window, cx| {
            let _ = weak.update(cx, |state, cx| state.sync_focus(window, cx));
        });
        let weak = state.downgrade();
        let focus_out = window.on_focus_out(&boundary.0.root, cx, move |_, window, cx| {
            let _ = weak.update(cx, |state, cx| state.sync_focus(window, cx));
        });
        Self {
            boundary,
            focus,
            guard,
            presented: false,
            dirty: true,
            hovered: Default::default(),
            hover: ScalarTransition::new(0., tokens.globals().motion_ease_in_out),
            body: None,
            body_layout: Default::default(),
            body_size: None,
            width: px(0.),
            scale: 0.,
            context: None,
            _blur: blur,
            _focus_in: focus_in,
            _focus_out: focus_out,
        }
    }
}

struct Body {
    notice: RenderNotice,
    styles: NotificationStyles,
    tokens: Arc<NotificationTokens>,
    direction: Direction,
    closable: bool,
}
fn content(
    content: &Content,
    notice: &RenderNotice,
    direction: Direction,
    window: &mut Window,
    cx: &mut App,
) -> AnyElement {
    match content {
        Content::Text(text) => {
            crate::behavior::text::break_word_paragraph_text(text.clone(), direction)
        }
        Content::Factory(factory) => factory(&notice.handle, window, cx),
    }
}
impl Render for Body {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let g = self.tokens.globals();
        let options = &self.notice.options;
        let title_content = options
            .title
            .as_ref()
            .filter(|value| !matches!(value, Content::Text(text) if text.is_empty()));
        let has_title = title_content.is_some();
        let has_description = options.description.is_some();
        let reserve_close = |d: gpui::Stateful<gpui::Div>, reserve| {
            d.when(reserve, |d| {
                if self.direction == Direction::Rtl {
                    d.pl(px(g.padding_lg as f32))
                } else {
                    d.pr(px(g.padding_lg as f32))
                }
            })
        };
        let title = title_content.map(|value| {
            let mut d = reserve_close(
                div()
                    .id("title")
                    .min_w(px(0.))
                    .flex_shrink(1.)
                    .text_color(g.color_text_heading.to_hsla())
                    .text_size(px(g.font_size_lg as f32))
                    .line_height(relative(g.line_height_lg as f32)),
                self.closable,
            );
            d.style().refine(&self.styles.title);
            d.child(content(value, &self.notice, self.direction, window, cx))
                .into_any_element()
        });
        let description = options.description.as_ref().map(|value| {
            let mut d = reserve_close(
                div()
                    .id("description")
                    .min_w(px(0.))
                    .flex_shrink(1.)
                    .text_color(g.color_text.to_hsla())
                    .text_size(px(g.font_size as f32)),
                self.closable && !has_title,
            );
            d.style().refine(&self.styles.description);
            d.child(content(value, &self.notice, self.direction, window, cx))
                .into_any_element()
        });
        let text = if has_title && has_description {
            let mut section = div()
                .id("section")
                .flex()
                .flex_col()
                .flex_1()
                .min_w(px(0.))
                .gap(px(g.margin_xs as f32));
            section.style().refine(&self.styles.section);
            section
                .children(title)
                .children(description)
                .into_any_element()
        } else {
            title
                .or(description)
                .unwrap_or_else(|| gpui::Empty.into_any_element())
        };
        let icon_ref = options.icon.clone().or_else(|| {
            options.kind.map(|kind| match kind {
                NotificationType::Info => IconName::InfoCircleFilled.icon(),
                NotificationType::Success => IconName::CheckCircleFilled.icon(),
                NotificationType::Warning => IconName::ExclamationCircleFilled.icon(),
                NotificationType::Error => IconName::CloseCircleFilled.icon(),
            })
        });
        let text = if options.show_icon && (icon_ref.is_some() || options.icon_content.is_some()) {
            let color = match options.kind {
                Some(NotificationType::Info) => g.color_info,
                Some(NotificationType::Success) => g.color_success,
                Some(NotificationType::Warning) => g.color_warning,
                Some(NotificationType::Error) => g.color_error,
                None => g.color_text,
            };
            let mut slot = div()
                .id("icon")
                .flex()
                .flex_none()
                .text_size(px((g.font_size_lg * g.line_height_lg) as f32))
                .line_height(px(0.))
                .text_color(color.to_hsla());
            slot.style().refine(&self.styles.icon);
            let icon_size = slot
                .style()
                .text
                .font_size
                .map_or(px((g.font_size_lg * g.line_height_lg) as f32), |s| {
                    s.to_pixels(window.rem_size())
                });
            let element = options
                .icon_content
                .as_ref()
                .map(|c| content(c, &self.notice, self.direction, window, cx))
                .unwrap_or_else(|| {
                    icon(icon_ref.unwrap())
                        .ant_design_frame()
                        .inherit_color()
                        .render_options(IconRenderOptions {
                            size: size(icon_size, icon_size),
                            colors: IconPalette::from_theme(g),
                            ..Default::default()
                        })
                        .into_any_element()
                });
            let mut wrapper = div()
                .id("wrapper")
                .flex()
                .items_start()
                .gap(px(g.margin_sm as f32))
                .min_w(px(0.))
                .when(self.direction == Direction::Rtl, |d| d.flex_row_reverse());
            wrapper.style().refine(&self.styles.wrapper);
            wrapper
                .child(slot.child(element))
                .child(text)
                .into_any_element()
        } else {
            text
        };
        let mut body = div()
            .id("body")
            .flex()
            .flex_col()
            .w_full()
            .min_w(px(0.))
            .child(text);
        if let Some(actions) = &options.actions {
            let mut slot = div()
                .id("actions")
                .flex()
                .mt(px(g.margin_sm as f32))
                // The upstream footer floats physically right in both directions.
                .justify_end();
            slot.style().refine(&self.styles.actions);
            body =
                body.child(slot.child(content(actions, &self.notice, self.direction, window, cx)));
        }
        body
    }
}

pub(super) struct PreparedCard {
    pub element: AnyElement,
    pub measured: Size<Pixels>,
    pub animating: bool,
    pub paintable: bool,
    pub surface_opacity: f32,
    pub surface: StyleRefinement,
}

pub(super) struct CardContext<'a> {
    pub state: &'a Entity<NotificationState>,
    pub visual: &'a Rc<RefCell<super::view::VisualState>>,
    pub config: &'a NotificationConfig,
    pub tokens: &'a Arc<NotificationTokens>,
    pub scope: &'a ConfigScope,
    pub direction: Direction,
    pub motion: MotionPolicy,
    pub now: Instant,
    pub elapsed: std::time::Duration,
}
impl CardContext<'_> {
    pub fn prepare(
        &self,
        notice: &RenderNotice,
        width: Pixels,
        opacity: f32,
        stacked: bool,
        window: &mut Window,
        cx: &mut App,
    ) -> PreparedCard {
        let g = self.tokens.globals();
        let options = &notice.options;
        let closable = options.closable.unwrap_or(self.config.closable);
        let progress =
            options.show_progress.unwrap_or(self.config.show_progress) && notice.budget.timed();
        let context = NotificationStyleContext {
            kind: options.kind,
            placement: notice.placement,
            direction: self.direction,
            motion: self.motion.allows_motion(),
            closing: notice.closing.is_some(),
            stacked,
            closable,
            show_progress: progress,
        };
        let mut styles = self.config.styles.clone();
        if let Some(resolver) = &self.config.styles_fn {
            styles.refine(&resolver.resolve(&context, window, cx));
        }
        styles.refine_card(&options.styles);
        if let Some(resolver) = &options.styles_fn {
            styles.refine_card(&resolver.resolve(&context, window, cx));
        }
        if styles.root.display == Some(gpui::Display::None) {
            return PreparedCard {
                element: gpui::Empty.into_any_element(),
                measured: size(px(0.), px(0.)),
                animating: false,
                paintable: false,
                surface_opacity: 0.,
                surface: styles.root,
            };
        }
        // Create native interaction state once per logical lifetime.
        if !self.visual.borrow().cards.contains_key(&notice.id) {
            let surface = NoticeVisual::new(self.state, window, cx, self.tokens);
            self.visual.borrow_mut().cards.insert(notice.id, surface);
        }
        let (body, cached_height, layout, boundary, guard, focus, hovered, hover) = {
            let mut visual = self.visual.borrow_mut();
            let surface = visual.cards.get_mut(&notice.id).unwrap();
            let changed = surface.dirty
                || surface.width != width
                || surface.scale != window.scale_factor()
                || surface.context != Some(context)
                || surface
                    .body
                    .as_ref()
                    .is_some_and(|b| b.read(cx).styles != styles);
            match &surface.body {
                None => {
                    surface.body = Some(cx.new(|_| Body {
                        notice: notice.clone(),
                        styles: styles.clone(),
                        tokens: self.tokens.clone(),
                        direction: self.direction,
                        closable,
                    }))
                }
                Some(body) if changed => body.update(cx, |body, cx| {
                    *body = Body {
                        notice: notice.clone(),
                        styles: styles.clone(),
                        tokens: self.tokens.clone(),
                        direction: self.direction,
                        closable,
                    };
                    cx.notify();
                }),
                Some(_) => {}
            }
            if changed {
                surface.body_size = None;
            }
            surface.width = width;
            surface.scale = window.scale_factor();
            surface.context = Some(context);
            surface.dirty = false;
            surface.hover.retarget(
                if surface.hovered.get() && !options.close_disabled {
                    1.
                } else {
                    0.
                },
                self.elapsed,
                g.motion_duration_mid,
                self.motion,
            );
            let hover = surface.hover.sample(self.elapsed);
            (
                surface.body.as_ref().unwrap().clone(),
                surface.body_size.map(|s| s.height),
                surface.body_layout.clone(),
                surface.boundary.clone(),
                surface.guard.clone(),
                surface.focus.clone(),
                surface.hovered.clone(),
                hover,
            )
        };
        let body = if let Some(height) = cached_height {
            body.cached(StyleRefinement {
                size: gpui::SizeRefinement {
                    width: Some(relative(1.).into()),
                    height: Some(height.into()),
                },
                ..Default::default()
            })
            .into_any_element()
        } else {
            body.into_any_element()
        };
        let mut root = div()
            .id(("notification", notice.id))
            .relative()
            .flex()
            .flex_col()
            .flex_none()
            .w(width)
            .max_w((window.viewport_size().width - px((2. * g.margin_lg) as f32)).max(px(0.)))
            .font(self.tokens.font.clone())
            .text_size(px(g.font_size as f32))
            .line_height(relative(g.line_height as f32))
            .text_color(g.color_text.to_hsla())
            .bg(self.tokens.background(options.kind))
            .rounded(px(g.border_radius_lg as f32))
            .px(px(g.padding_content_horizontal_lg as f32))
            .py(px(g.padding_md as f32))
            .shadow(self.tokens.shadows.to_vec())
            .track_focus(&boundary.0.root)
            .when(self.direction == Direction::Rtl, |d| d.text_right())
            .child(ContentProbe {
                child: body,
                layout: layout.clone(),
                offset: Default::default(),
            });
        root.style().refine(&styles.root);
        let caller_opacity = root.style().opacity.unwrap_or(1.);
        root = root.opacity(caller_opacity * opacity);
        let weak = self.state.downgrade();
        let id = notice.id;
        root = root.on_hover(move |hovered, window, cx| {
            let _ = weak.update(cx, |state, cx| state.hover(id, *hovered, window, cx));
        });
        if let Some(callback) = options.on_click.clone() {
            let handle = notice.handle.clone();
            root = root.on_click(move |event, window, cx| {
                if !handle.is_dismissed() {
                    callback(event, window, cx);
                }
            });
        }
        if closable {
            let close_size = px((g.control_height_lg * 0.55) as f32);
            let mix = |a: crate::theme::Color, b: crate::theme::Color| {
                a.to_hsla().blend(b.to_hsla().opacity(hover.value as f32))
            };
            let mut close = div()
                .id("close")
                .absolute()
                .top(px(g.padding_md as f32))
                .size(close_size)
                .when(self.direction == Direction::Ltr, |d| {
                    d.right(px(g.padding_lg as f32))
                })
                .when(self.direction == Direction::Rtl, |d| {
                    d.left(px(g.padding_lg as f32))
                })
                .flex()
                .items_center()
                .justify_center()
                .rounded(px(g.border_radius_sm as f32))
                .text_color(mix(g.color_icon, g.color_icon_hover))
                .text_size(px(g.font_size as f32))
                .bg(g.color_bg_text_hover.to_hsla().opacity(hover.value as f32))
                .when(!options.close_disabled, |d| {
                    d.cursor_pointer().track_focus(&focus)
                })
                .when(options.close_disabled, |d| {
                    d.cursor_not_allowed()
                        .on_click(|_, _, cx| cx.stop_propagation())
                })
                .when(!options.close_disabled, |d| {
                    d.active(|d| d.bg(g.color_bg_text_active.to_hsla()))
                });
            close.style().refine(&styles.close);
            let icon_size = close
                .style()
                .text
                .font_size
                .map_or(px(g.font_size as f32), |s| s.to_pixels(window.rem_size()));
            let close_ref = options
                .close_icon
                .clone()
                .or_else(|| {
                    self.config
                        .close_icon
                        .as_ref()
                        .map(|icon| icon.reference().clone())
                })
                .unwrap_or_else(|| IconName::CloseOutlined.icon());
            close = close.child(
                icon(close_ref)
                    .ant_design_frame()
                    .inherit_color()
                    .render_options(IconRenderOptions {
                        size: size(icon_size, icon_size),
                        colors: IconPalette::from_theme(g),
                        ..Default::default()
                    }),
            );
            let weak = self.state.downgrade();
            close = close.on_hover(move |over, _, cx| {
                if hovered.replace(*over) != *over {
                    let _ = weak.update(cx, |_, cx| cx.notify());
                }
            });
            guard.set_availability(Availability {
                disabled: options.close_disabled || notice.closing.is_some(),
                busy: false,
            });
            let handle = notice.handle.clone();
            let close = guard.install(close, move |_, window, cx| {
                cx.stop_propagation();
                handle.close_with(NotificationCloseReason::CloseButton, window, cx);
            });
            let close = Accessibility {
                role: Some(Role::Button),
                label: Some(
                    options
                        .close_label
                        .clone()
                        .unwrap_or_else(|| self.scope.snapshot(cx).locale.common.close.clone()),
                ),
                disabled: options.close_disabled || notice.closing.is_some(),
                ..Default::default()
            }
            .apply(close);
            root = root.child(
                FocusRing::new(
                    g.color_primary_border.to_hsla(),
                    px(g.line_width_focus as f32),
                )
                .radius(px(g.border_radius_sm as f32))
                .apply(close, &focus),
            );
        }
        if progress {
            let fraction = notice.budget.fraction(self.now);
            let mut bar = div()
                .id("progress")
                .absolute()
                .bottom(px(0.))
                .left(px(g.border_radius_lg as f32))
                .right(px(g.border_radius_lg as f32))
                .h(px(2.))
                .rounded(px(g.border_radius_lg as f32))
                .bg(gpui::rgba(0x0000000a))
                .overflow_hidden();
            bar.style().refine(&styles.progress);
            bar = bar.child(
                div()
                    .h_full()
                    .w(relative(fraction))
                    .bg(self.tokens.progress_bg)
                    .rounded(px(g.border_radius_lg as f32)),
            );
            root = root.child(
                Accessibility::new(Role::ProgressIndicator).apply(
                    bar.aria_numeric_value(f64::from(fraction) * 100.)
                        .aria_min_numeric_value(0.)
                        .aria_max_numeric_value(100.),
                ),
            );
        }
        let mut accessibility = options.accessibility.clone();
        if accessibility.label.is_none()
            && let Some(Content::Text(text)) = &options.title
        {
            accessibility.label = Some(text.clone());
        }
        if accessibility.description.is_none()
            && let Some(Content::Text(text)) = &options.description
        {
            accessibility.description = Some(text.clone());
        }
        let surface = root.style().clone();
        let mut element = LiveSurface {
            element: accessibility.apply(root).into_element(),
            closing: notice.closing.is_some(),
            boundary: Some(boundary),
            geometry: None,
        }
        .into_any_element();
        let measured = element.layout_as_root(
            size(AvailableSpace::Definite(width), AvailableSpace::MaxContent),
            window,
            cx,
        );
        if !self.visual.borrow().cards.contains_key(&notice.id) {
            // A native content factory can synchronously dismiss its own lifetime.
            // Its snapshot no longer owns geometry or interaction after layout.
            return PreparedCard {
                element: gpui::Empty.into_any_element(),
                measured: size(px(0.), px(0.)),
                animating: false,
                paintable: false,
                surface_opacity: 0.,
                surface,
            };
        }
        if let Some(id) = layout.get() {
            self.visual
                .borrow_mut()
                .cards
                .get_mut(&notice.id)
                .unwrap()
                .body_size = Some(window.layout_bounds(id).size);
        }
        PreparedCard {
            element,
            measured,
            animating: !hover.finished
                || progress
                    && notice.budget.running()
                    && notice.closing.is_none()
                    && styles.progress.display != Some(gpui::Display::None)
                    && styles.progress.opacity.unwrap_or(1.) > 0.,
            paintable: caller_opacity > 0.
                && styles
                    .root
                    .visibility
                    .is_none_or(|v| v == gpui::Visibility::Visible),
            surface_opacity: caller_opacity,
            surface,
        }
    }
}
