// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    ImageContent, ImageCoverPlacement, ImagePlaceholder, ImageProgress, ImageStyleContext,
    ImageStyleResolver, ImageStyles, ImageTheme, PreviewGroup, PreviewItem, PreviewOptions,
    PreviewState,
    picture::{Picture, PictureEvent, PictureState, PictureStatus},
};
use crate::{
    ConfigScope,
    behavior::{Accessibility, ActivationGuard, Availability, ScalarTransition},
    theme::CubicBezier,
};
use gpui::{
    App, AppContext, Bounds, Context, ElementId, Entity, EventEmitter, FocusHandle, Focusable,
    ImageSource, IntoElement, Length, Pixels, Refineable, RenderOnce, Role, SharedString,
    StyleRefinement, Styled, Subscription, Window, canvas, div, prelude::*, px,
};
use std::time::{Duration, Instant};

pub type ImageEvent = PictureEvent;

pub(super) fn progress_semantics(
    layer: gpui::Stateful<gpui::Div>,
    percent: Option<u8>,
    loading: SharedString,
) -> gpui::Stateful<gpui::Div> {
    let layer = layer
        .role(Role::ProgressIndicator)
        .aria_label(percent.map_or_else(|| loading.to_string(), |p| format!("{p}%")));
    if let Some(percent) = percent {
        layer
            .aria_numeric_value(f64::from(percent))
            .aria_min_numeric_value(0.)
            .aria_max_numeric_value(100.)
    } else {
        layer
    }
}

/// Retain one controller per mounted image. Rebuilding an Image does not restart its load.
pub struct ImageState {
    picture: Entity<PictureState>,
    preview: Entity<PreviewState>,
    source: Option<ImageSource>,
    preview_source: Option<ImageSource>,
    fallback: Option<ImageSource>,
    alt: SharedString,
    scope: ConfigScope,
    focus: FocusHandle,
    activation: ActivationGuard,
    hovered: bool,
    cover: ScalarTransition,
    progress_width: Option<(ScalarTransition, Duration)>,
    bounds: Bounds<Pixels>,
    epoch: Instant,
    frame_pending: bool,
    animate: bool,
    mounted_group: Option<gpui::WeakEntity<PreviewState>>,
    mounted_id: Option<SharedString>,
    mount_generation: u64,
    _picture_subscription: Subscription,
    _scope_subscription: Subscription,
    _blur_subscription: Subscription,
}
impl EventEmitter<ImageEvent> for ImageState {}
impl Focusable for ImageState {
    fn focus_handle(&self, _: &App) -> FocusHandle {
        self.focus.clone()
    }
}
impl ImageState {
    pub fn new(
        source: impl Into<ImageSource>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        Self::create(Some(source.into()), window, cx)
    }
    pub fn empty(window: &mut Window, cx: &mut Context<Self>) -> Self {
        Self::create(None, window, cx)
    }
    fn create(source: Option<ImageSource>, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let scope = ConfigScope::application(cx);
        let picture = cx.new(|cx| PictureState::new(source.clone(), cx));
        let preview = cx.new(|cx| PreviewState::new(Vec::new(), window, cx));
        let focus = crate::behavior::focus_handle(cx);
        let activation = ActivationGuard::default();
        let blur = activation.observe_blur(&focus, window, cx);
        let picture_subscription = cx.subscribe(&picture, |_, _, event, cx| {
            cx.emit(event.clone());
            cx.notify();
        });
        Self {
            picture,
            preview,
            source,
            preview_source: None,
            fallback: None,
            alt: SharedString::default(),
            _scope_subscription: scope.observe(cx),
            scope,
            focus,
            activation,
            hovered: false,
            cover: ScalarTransition::new(0., CubicBezier::new(0.25, 0.1, 0.25, 1.)),
            progress_width: None,
            bounds: Bounds::default(),
            epoch: cx.background_executor().now(),
            frame_pending: false,
            animate: false,
            mounted_group: None,
            mounted_id: None,
            mount_generation: 0,
            _picture_subscription: picture_subscription,
            _blur_subscription: blur,
        }
    }
    pub fn picture(&self) -> Entity<PictureState> {
        self.picture.clone()
    }
    pub fn bounds(&self) -> Bounds<Pixels> {
        self.bounds
    }
    pub fn preview(&self) -> Entity<PreviewState> {
        self.preview.clone()
    }
    pub fn status(&self, cx: &App) -> PictureStatus {
        self.picture.read(cx).status()
    }
    pub fn source(&self) -> Option<&ImageSource> {
        self.source.as_ref()
    }
    pub fn set_source(&mut self, source: Option<ImageSource>, cx: &mut Context<Self>) {
        self.source = source.clone();
        self.picture.update(cx, |p, cx| p.set_source(source, cx));
        cx.notify();
    }
    pub fn set_preview_source(&mut self, source: Option<ImageSource>, cx: &mut Context<Self>) {
        self.preview_source = source;
        cx.notify();
    }
    pub fn set_fallback(&mut self, source: Option<ImageSource>, cx: &mut Context<Self>) {
        self.fallback = source.clone();
        self.picture.update(cx, |p, cx| p.set_fallback(source, cx));
        cx.notify();
    }
    pub fn set_alt(&mut self, alt: impl Into<SharedString>, cx: &mut Context<Self>) {
        self.alt = alt.into();
        cx.notify();
    }
    pub fn set_scope(&mut self, scope: ConfigScope, cx: &mut Context<Self>) {
        if self.scope != scope {
            self._scope_subscription = scope.observe(cx);
            self.scope = scope;
            cx.notify();
        }
    }
    fn frame(&mut self, w: &mut Window, cx: &mut Context<Self>) {
        if self.frame_pending {
            return;
        }
        self.frame_pending = true;
        let state = cx.weak_entity();
        w.on_next_frame(move |_, cx| {
            if let Some(state) = state.upgrade() {
                state.update(cx, |s, cx| {
                    s.frame_pending = false;
                    cx.notify();
                });
            }
        });
        w.request_animation_frame();
    }
}

#[derive(IntoElement)]
pub struct Image {
    id: ElementId,
    state: Entity<ImageState>,
    scope: Option<ConfigScope>,
    group: Option<Entity<PreviewState>>,
    preview: Option<PreviewOptions>,
    enabled: Option<bool>,
    placeholder: Option<ImagePlaceholder>,
    cover: Option<ImageContent>,
    placement: ImageCoverPlacement,
    styles: ImageStyles,
    styles_fn: Option<ImageStyleResolver>,
    style: StyleRefinement,
    grayscale: f32,
    alt: Option<SharedString>,
    automation_id: Option<SharedString>,
    fit: gpui::ObjectFit,
}
impl Image {
    pub fn new(id: impl Into<ElementId>, state: &Entity<ImageState>, _: &App) -> Self {
        Self {
            id: id.into(),
            state: state.clone(),
            scope: None,
            group: None,
            preview: None,
            enabled: None,
            placeholder: None,
            cover: None,
            placement: ImageCoverPlacement::Center,
            styles: ImageStyles::default(),
            styles_fn: None,
            style: StyleRefinement::default(),
            grayscale: 0.,
            alt: None,
            automation_id: None,
            fit: gpui::ObjectFit::Fill,
        }
    }
    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }
    pub fn preview_group(mut self, group: &Entity<PreviewState>) -> Self {
        self.group = Some(group.clone());
        self
    }
    pub fn preview(mut self, enabled: bool) -> Self {
        self.enabled = Some(enabled);
        self
    }
    pub fn preview_options(mut self, options: PreviewOptions) -> Self {
        self.preview = Some(options);
        self
    }
    pub fn placeholder(mut self, placeholder: ImagePlaceholder) -> Self {
        self.placeholder = Some(placeholder);
        self
    }
    pub fn progress(self, progress: ImageProgress) -> Self {
        self.placeholder(ImagePlaceholder::Progress(progress))
    }
    pub fn cover(mut self, content: ImageContent) -> Self {
        self.cover = Some(content);
        self
    }
    pub fn cover_placement(mut self, placement: ImageCoverPlacement) -> Self {
        self.placement = placement;
        self
    }
    pub fn styles(mut self, styles: ImageStyles) -> Self {
        self.styles = styles;
        self
    }
    pub fn styles_fn(mut self, styles: ImageStyleResolver) -> Self {
        self.styles_fn = Some(styles);
        self
    }
    pub fn grayscale(mut self, value: f32) -> Self {
        self.grayscale = if value.is_finite() {
            value.clamp(0., 1.)
        } else {
            0.
        };
        self
    }
    pub fn object_fit(mut self, fit: gpui::ObjectFit) -> Self {
        self.fit = fit;
        self
    }
    pub fn alt(mut self, alt: impl Into<SharedString>) -> Self {
        self.alt = Some(alt.into());
        self
    }
    pub fn automation_id(mut self, id: impl Into<SharedString>) -> Self {
        self.automation_id = Some(id.into());
        self
    }
}
impl Styled for Image {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

struct ImageMount {
    _observe: Subscription,
    _release: Subscription,
}

impl RenderOnce for Image {
    fn render(self, w: &mut Window, cx: &mut App) -> impl IntoElement {
        let state = self.state.clone();
        let observe = state.clone();
        let _mount = w.use_keyed_state((self.id.clone(), "image-observe"), cx, |_, cx| {
            let generation = observe.update(cx, |s, _| {
                s.mount_generation = s.mount_generation.wrapping_add(1);
                s.progress_width = None;
                s.mount_generation
            });
            let weak = observe.downgrade();
            ImageMount {
                _observe: cx.observe(&observe, |_, _, cx| cx.notify()),
                _release: cx.on_release(move |_: &mut ImageMount, cx| {
                    cx.defer(move |cx| {
                        let Some(state) = weak.upgrade() else {
                            return;
                        };
                        let registration = state.update(cx, |s, _| {
                            if s.mount_generation != generation {
                                return None;
                            }
                            s.mounted_group.take().zip(s.mounted_id.take())
                        });
                        if let Some((group, id)) = registration {
                            let _ = group.update(cx, |s, cx| s.unregister_item(&id, cx));
                        }
                    })
                }),
            }
        });
        let scope = self
            .scope
            .clone()
            .unwrap_or_else(|| state.read(cx).scope.clone());
        state.update(cx, |s, cx| s.set_scope(scope.clone(), cx));
        let config = scope.snapshot(cx);
        let token = config.theme.component::<ImageTheme>();
        let globals = token.globals();
        let progress = matches!(self.placeholder, Some(ImagePlaceholder::Progress(_)));
        let finite_progress = match &self.placeholder {
            Some(ImagePlaceholder::Progress(progress)) => progress
                .percent
                .filter(|p| p.is_finite())
                .map(|p| p.round().clamp(0., 100.) as u8),
            _ => None,
        };
        let source = state.read(cx).source.clone();
        let placeholder_only = source.is_none() && self.placeholder.is_some();
        let preview = self
            .group
            .clone()
            .unwrap_or_else(|| state.read(cx).preview.clone());
        let enabled = self.enabled.unwrap_or(config.image.preview_enabled)
            && self.preview.as_ref().is_none_or(|p| p.enabled)
            && (source.is_some() || state.read(cx).preview_source.is_some())
            && !progress
            && !placeholder_only;
        let status = state.read(cx).status(cx);
        let context = ImageStyleContext {
            preview: enabled,
            loading: status == PictureStatus::Loading || progress || placeholder_only,
            failed: status == PictureStatus::Error,
            direction: config.direction,
        };
        let mut styles = config.image.styles.clone();
        if let Some(resolver) = &config.image.styles_fn {
            styles.refine(&resolver.resolve(&context, w, cx));
        }
        styles.refine(&self.styles);
        if let Some(resolver) = &self.styles_fn {
            styles.refine(&resolver.resolve(&context, w, cx));
        }
        styles.root.refine(&self.style);
        let (picture, focus, guard, alt) = {
            let s = state.read(cx);
            (
                s.picture.clone(),
                s.focus.clone(),
                s.activation.clone(),
                self.alt.clone().unwrap_or_else(|| s.alt.clone()),
            )
        };
        if self.group.is_none() {
            preview.update(cx, |s, cx| {
                s.set_scope(scope.clone(), cx);
                let mut options = self.preview.clone().unwrap_or_else(|| PreviewOptions {
                    enabled,
                    mask: config.image.mask,
                    mask_closable: config.image.mask_closable,
                    focus_trap: config.image.focus_trap,
                    wheel: config.image.wheel,
                    movable: config.image.movable,
                    close_icon: config.image.close_icon.clone(),
                    ..Default::default()
                });
                options.show_count = Some(options.show_count.unwrap_or(false));
                options.styles.refine(&styles);
                s.set_options(options, cx);
            });
        }
        let item_id = SharedString::from(format!("image:{:?}", state.entity_id()));
        let item_source = state
            .read(cx)
            .preview_source
            .clone()
            .or(source.clone())
            .filter(|_| enabled);
        let old = state.update(cx, |s, _| {
            let changed = s
                .mounted_group
                .as_ref()
                .and_then(|g| g.upgrade())
                .is_some_and(|g| g.entity_id() != preview.entity_id())
                || item_source.is_none();
            let old = changed
                .then(|| s.mounted_group.take().zip(s.mounted_id.take()))
                .flatten();
            if item_source.is_some() {
                s.mounted_group = Some(preview.downgrade());
                s.mounted_id = Some(item_id.clone());
            }
            old
        });
        if let Some((group, id)) = old {
            let _ = group.update(cx, |s, cx| s.unregister_item(&id, cx));
        }
        if let Some(source) = item_source {
            let item = PreviewItem {
                id: item_id.clone(),
                source,
                alt: alt.clone(),
                fallback: state.read(cx).fallback.clone(),
            };
            preview.update(cx, |s, cx| s.register_item(item, cx));
        }
        guard.set_availability(Availability {
            disabled: !enabled,
            busy: false,
        });
        let policy =
            crate::behavior::MotionPolicy::new(config.motion && globals.motion, cx.reduce_motion());
        let (cover_opacity, elapsed, progress_track) = state.update(cx, |s, cx| {
            let elapsed = cx.background_executor().now().duration_since(s.epoch);
            let show =
                enabled && (s.hovered || (s.focus.is_focused(w) && w.last_input_was_keyboard()));
            s.cover.retarget(
                if show { 1. } else { 0. },
                elapsed,
                globals.motion_duration_slow,
                policy,
            );
            let sample = s.cover.sample(elapsed);
            let progress_track = finite_progress.map(|percent| {
                let (transition, started_at) = s.progress_width.get_or_insert_with(|| {
                    (
                        ScalarTransition::new(
                            f64::from(percent),
                            CubicBezier::new(0.25, 0.1, 0.25, 1.),
                        ),
                        elapsed,
                    )
                });
                transition.retarget(
                    f64::from(percent),
                    elapsed,
                    globals.motion_duration_mid,
                    policy,
                );
                (
                    transition.sample(elapsed).value as f32,
                    elapsed.saturating_sub(*started_at),
                )
            });
            if finite_progress.is_none() {
                s.progress_width = None;
            }
            s.animate = ((enabled && !sample.finished) || progress) && policy.allows_motion();
            (sample.value as f32, elapsed, progress_track)
        });
        let mut root = div()
            .id(self.id.clone())
            .relative()
            .flex_none()
            .when(enabled, |d| d.track_focus(&focus).cursor_pointer());
        if let Some(id) = self.automation_id.clone() {
            root = root.debug_selector(move || id.to_string());
        }
        if progress || placeholder_only {
            root = root.w(px(200.)).h(px(200.));
        }
        root.style().refine(&styles.root);
        if !progress && !placeholder_only {
            if status == PictureStatus::Error {
                // Failed images have no intrinsic bitmap dimensions. Keep the
                // error presentation in normal flow so automatic height still
                // exposes the icon and alt text. A new loading source mounts
                // Picture again and resumes the shared loader naturally.
                let mut content = div()
                    .id(("image-error", state.entity_id()))
                    .debug_selector(|| "image-error-content".into())
                    .flex()
                    .items_start()
                    .whitespace_nowrap()
                    .overflow_hidden()
                    .gap(px(4.))
                    .text_color(globals.color_text_secondary)
                    .child(
                        crate::icons::icon(crate::icons::IconName::PictureOutlined).size(px(16.)),
                    )
                    .when(!alt.is_empty(), |content| {
                        content.child(div().min_w_0().overflow_hidden().child(alt.clone()))
                    });
                if matches!(styles.root.size.width, Some(Length::Definite(_))) {
                    content = content.w_full();
                }
                if matches!(styles.root.size.height, Some(Length::Definite(_))) {
                    content = content.h_full();
                }
                content.style().refine(&styles.image);
                root = root.child(content);
            } else {
                let mut content = Picture::new(("image-picture", state.entity_id()), &picture)
                    .object_fit(self.fit)
                    .grayscale(self.grayscale);
                if matches!(styles.root.size.width, Some(Length::Definite(_))) {
                    content = content.w_full();
                }
                if matches!(styles.root.size.height, Some(Length::Definite(_))) {
                    content = content.h_full();
                }
                content.style().refine(&styles.image);
                root = root.child(content);
            }
        }
        if let Some(placeholder) = &self.placeholder {
            match placeholder {
                ImagePlaceholder::Default
                    if status == PictureStatus::Loading || placeholder_only =>
                {
                    let mut overlay = div()
                        .absolute()
                        .inset_0()
                        .flex()
                        .items_center()
                        .justify_center()
                        .bg(globals.color_bg_container_disabled)
                        .child(
                            crate::icons::icon(crate::icons::IconName::PictureFilled).size(px(60.)),
                        );
                    overlay.style().refine(&styles.placeholder);
                    root = root.child(
                        Accessibility {
                            hidden: true,
                            ..Default::default()
                        }
                        .apply(overlay),
                    );
                }
                ImagePlaceholder::Content(factory)
                    if status == PictureStatus::Loading || placeholder_only =>
                {
                    let mut overlay = div().absolute().inset_0().size_full().child(factory(w, cx));
                    overlay.style().refine(&styles.placeholder);
                    root = root.child(
                        Accessibility {
                            hidden: true,
                            ..Default::default()
                        }
                        .apply(overlay),
                    );
                }
                ImagePlaceholder::Progress(p) => {
                    let bounds = state.read(cx).bounds;
                    let width = if bounds.size.width > px(0.) {
                        f32::from(bounds.size.width)
                    } else {
                        200.
                    };
                    let height = if bounds.size.height > px(0.) {
                        f32::from(bounds.size.height)
                    } else {
                        200.
                    };
                    let mut layer = div()
                        .id("image-progress")
                        .relative()
                        .size_full()
                        .overflow_hidden()
                        .bg(globals.color_bg_base);
                    layer.style().refine(&styles.progress_root);
                    layer = layer.child(super::progress::clouds(
                        width,
                        height,
                        elapsed,
                        token.progress_animation_duration,
                        policy.allows_motion(),
                        w,
                        cx,
                    ));
                    let finite = finite_progress;
                    layer = progress_semantics(layer, finite, config.locale.image.loading.clone());
                    let rail = if let Some(percent) = finite {
                        let (width, rail_elapsed) =
                            progress_track.unwrap_or((f32::from(percent), Duration::ZERO));
                        let mut rail = div()
                            .w_full()
                            .h(px(6.))
                            .mt(px(globals.margin_sm as f32))
                            .rounded(px(globals.border_radius_xs as f32))
                            .bg(gpui::rgba(0xffffff80))
                            .overflow_hidden();
                        rail.style().refine(&styles.progress_rail);
                        rail.child(super::progress::rail_fill(
                            width,
                            globals.border_radius_xs as f32,
                            rail_elapsed,
                            token.progress_animation_duration,
                            policy.allows_motion(),
                        ))
                        .into_any_element()
                    } else {
                        div().into_any_element()
                    };
                    let content = if let Some(factory) = &p.render {
                        factory(rail, finite.unwrap_or(0), w, cx)
                    } else if let Some(percent) = finite {
                        let mut text = div()
                            .mt(px(globals.margin_xs as f32))
                            .child(format!("{percent}%"));
                        text.style().refine(&styles.progress_indicator);
                        div().w_full().child(rail).child(text).into_any_element()
                    } else {
                        rail
                    };
                    let mut text = div()
                        .absolute()
                        .inset_0()
                        .flex()
                        .items_center()
                        .justify_center()
                        .px(px(globals.padding_lg as f32))
                        .text_color(globals.color_text_secondary)
                        .child(div().w_full().text_center().child(content));
                    text.style().refine(&styles.progress_content);
                    root = root.child(layer.child(text));
                }
                _ => {}
            }
        }
        if enabled {
            let mut cover = div()
                .absolute()
                .inset_0()
                .flex()
                .items_center()
                .justify_center()
                .bg(gpui::rgba(0x0000004d))
                .text_color(gpui::rgb(0xffffff))
                .opacity(cover_opacity);
            if self.placement != ImageCoverPlacement::Center {
                cover = cover.h_auto();
                cover = if self.placement == ImageCoverPlacement::Top {
                    cover.bottom_auto()
                } else {
                    cover.top_auto()
                };
            }
            if let Some(factory) = &self.cover {
                cover = cover.child(factory(w, cx));
            }
            cover.style().refine(&styles.cover);
            root = root.child(cover);
        }
        let hover = state.clone();
        root = root.on_hover(move |hovered, _, cx| {
            hover.update(cx, |s, cx| {
                s.hovered = *hovered;
                cx.notify();
            })
        });
        let measure = state.clone();
        let animate = state.clone();
        let owner = crate::modal::host::owner(w, cx);
        root = root.child(
            canvas(
                move |bounds, _, cx| {
                    measure.update(cx, |s, cx| {
                        if s.bounds != bounds {
                            s.bounds = bounds;
                            cx.notify();
                        }
                    });
                },
                move |bounds, _, w, cx| {
                    if w.is_visible()
                        && w.is_window_active()
                        && !bounds.intersect(&w.content_mask().bounds).is_empty()
                        && crate::modal::host::popup_allowed(owner, w, cx)
                    {
                        animate.update(cx, |s, cx| {
                            if s.animate {
                                s.frame(w, cx);
                            }
                        });
                    }
                },
            )
            .absolute()
            .inset_0(),
        );
        let target = preview.clone();
        let item_id = SharedString::from(format!("image:{:?}", state.entity_id()));
        let origin = state.clone();
        let root = if enabled {
            guard.install(root, move |_, w, cx| {
                let bounds = origin.read(cx).bounds;
                target.update(cx, |s, cx| {
                    s.set_open_origin(bounds, cx);
                    s.request_item(&item_id, w, cx);
                });
            })
        } else {
            root
        };
        let mut semantics = Accessibility::new(if progress || placeholder_only {
            Role::GenericContainer
        } else if enabled {
            Role::Button
        } else {
            Role::Image
        });
        semantics.label = (!alt.is_empty()).then_some(alt);
        semantics.busy = context.loading && finite_progress.is_none();
        semantics.automation_id = self.automation_id;
        let root = config.focus_ring().apply(semantics.apply(root), &focus);
        if self.group.is_none() {
            div()
                .relative()
                .flex_none()
                .child(root)
                .child(PreviewGroup::new(
                    ("image-preview", state.entity_id()),
                    &preview,
                ))
                .into_any_element()
        } else {
            root.into_any_element()
        }
    }
}
