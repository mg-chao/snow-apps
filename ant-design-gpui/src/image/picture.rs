// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! A retained source/load state and a small native image layout/paint element.

mod loader;
mod renderer;

use super::PreviewTransform;
use gpui::{
    App, AvailableSpace, Bounds, Context, Element, ElementId, Entity, EventEmitter,
    GlobalElementId, Hitbox, ImageCacheError, ImageSource, InspectorElementId, Interactivity,
    IntoElement, LayoutId, Length, ObjectFit, Pixels, Point, RenderImage, Size, StyleRefinement,
    Styled, Task, Window, px, size,
};
pub(super) use loader::Preparation;
use std::{sync::Arc, time::Duration};

/// The load state of the source currently selected by a picture.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PictureStatus {
    Empty,
    Loading,
    Ready,
    Error,
}

/// Load notifications are emitted once per source attempt, including a separate
/// notification when a configured fallback fails.
#[derive(Clone, Debug)]
pub enum PictureEvent {
    Loaded {
        size: Size<Pixels>,
        fallback: bool,
    },
    Error {
        error: ImageCacheError,
        fallback: bool,
    },
}

/// Retain one state per displayed image. Shared decoded requests are leased by
/// painted scenes, so changing a source cannot publish a stale completion.
pub struct PictureState {
    source: Option<ImageSource>,
    fallback: Option<ImageSource>,
    status: PictureStatus,
    image: Option<Arc<RenderImage>>,
    natural: Option<Size<Pixels>>,
    measured_image: Option<usize>,
    error: Option<ImageCacheError>,
    source_failed: bool,
    fallback_failed: bool,
    announced_image: Option<usize>,
    frame: usize,
    wake: Option<Task<()>>,
}
impl EventEmitter<PictureEvent> for PictureState {}

impl PictureState {
    pub fn new(source: Option<ImageSource>, _: &mut Context<Self>) -> Self {
        let image = source.as_ref().and_then(|source| match source {
            ImageSource::Render(image) if valid_image(image) => Some(image.clone()),
            _ => None,
        });
        let status = if image.is_some() {
            PictureStatus::Ready
        } else if source.is_some() {
            PictureStatus::Loading
        } else {
            PictureStatus::Empty
        };
        Self {
            source,
            fallback: None,
            status,
            image,
            natural: None,
            measured_image: None,
            error: None,
            source_failed: false,
            fallback_failed: false,
            announced_image: None,
            frame: 0,
            wake: None,
        }
    }

    pub fn source(&self) -> Option<&ImageSource> {
        self.source.as_ref()
    }
    pub fn fallback(&self) -> Option<&ImageSource> {
        self.fallback.as_ref()
    }
    pub fn status(&self) -> PictureStatus {
        self.status
    }
    pub fn image(&self) -> Option<Arc<RenderImage>> {
        self.image.clone()
    }
    pub fn error(&self) -> Option<&ImageCacheError> {
        self.error.as_ref()
    }
    pub fn natural_size(&self) -> Option<Size<Pixels>> {
        self.natural.or_else(|| {
            self.image
                .as_ref()
                .map(|image| image.size(0).map(|v| px(v.0 as f32)))
        })
    }

    pub fn set_source(&mut self, source: Option<ImageSource>, cx: &mut Context<Self>) {
        if same_source(self.source.as_ref(), source.as_ref()) {
            return;
        }
        self.source = source;
        self.image = None;
        self.natural = None;
        self.measured_image = None;
        self.error = None;
        self.source_failed = false;
        self.fallback_failed = false;
        self.announced_image = None;
        self.frame = 0;
        self.wake = None;
        self.status = if self.source.is_some() {
            PictureStatus::Loading
        } else {
            PictureStatus::Empty
        };
        if let Some(ImageSource::Render(image)) = &self.source
            && valid_image(image)
        {
            self.image = Some(image.clone());
            self.status = PictureStatus::Ready;
        }
        cx.notify();
    }

    pub fn set_fallback(&mut self, source: Option<ImageSource>, cx: &mut Context<Self>) {
        if same_source(self.fallback.as_ref(), source.as_ref()) {
            return;
        }
        self.fallback = source;
        self.fallback_failed = false;
        if self.source_failed {
            self.image = None;
            self.natural = None;
            self.measured_image = None;
            self.announced_image = None;
            self.frame = 0;
            self.wake = None;
            self.status = if self.fallback.is_some() {
                PictureStatus::Loading
            } else {
                PictureStatus::Error
            };
        }
        cx.notify();
    }

    fn resolve(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let Some(source) = self.source.clone() else {
            return;
        };
        if !self.source_failed {
            let result = loader::resolve(&source, None, window, cx);
            self.measure(&result, window, cx);
            self.apply(result, false, cx);
        }
        if self.source_failed
            && !self.fallback_failed
            && let Some(fallback) = self.fallback.clone()
        {
            let result = loader::resolve(&fallback, None, window, cx);
            self.measure(&result, window, cx);
            self.apply(result, true, cx);
        }
    }

    fn measure(&mut self, result: &loader::LoadResult, window: &mut Window, cx: &mut App) {
        let loader::LoadResult::Ready(image) = result else {
            return;
        };
        if self.measured_image == Some(image.id.0) {
            return;
        }
        // RenderImage's raster scale is private to GPUI. A native intrinsic
        // measurement preserves supersampled SVGs' logical size without copying
        // pixels or reaching into the dependency's internal representation.
        let mut probe = gpui::img(image.clone()).into_any_element();
        self.natural = Some(probe.layout_as_root(
            size(AvailableSpace::MaxContent, AvailableSpace::MaxContent),
            window,
            cx,
        ));
        self.measured_image = Some(image.id.0);
    }

    fn apply(&mut self, result: loader::LoadResult, fallback: bool, cx: &mut Context<Self>) {
        match result {
            loader::LoadResult::Pending => {
                if self.status != PictureStatus::Loading {
                    self.status = PictureStatus::Loading;
                    cx.notify();
                }
            }
            loader::LoadResult::Ready(image) => {
                let id = image.id.0;
                self.status = PictureStatus::Ready;
                self.image = Some(image);
                if self.announced_image != Some(id) {
                    self.announced_image = Some(id);
                    self.frame = 0;
                    self.wake = None;
                    cx.emit(PictureEvent::Loaded {
                        size: self.natural_size().unwrap(),
                        fallback,
                    });
                    cx.notify();
                }
            }
            loader::LoadResult::Failed(error) => {
                self.image = None;
                self.error = Some(error.clone());
                if fallback {
                    self.fallback_failed = true;
                } else {
                    self.source_failed = true;
                }
                self.status = if !fallback && self.fallback.is_some() {
                    PictureStatus::Loading
                } else {
                    PictureStatus::Error
                };
                cx.emit(PictureEvent::Error { error, fallback });
                cx.notify();
            }
        }
    }

    fn schedule(&mut self, visible: bool, window: &Window, cx: &mut Context<Self>) {
        if !visible || !window.is_window_active() || cx.reduce_motion() {
            self.wake = None;
            return;
        }
        let Some(image) = &self.image else { return };
        if image.frame_count() <= 1 || self.wake.is_some() {
            return;
        }
        // Honor source timing without requesting a 60 Hz redraw for a 10 Hz GIF.
        let wait = Duration::from(image.delay(self.frame)).max(Duration::from_millis(10));
        let timer = cx.background_executor().timer(wait);
        let image_id = image.id;
        self.wake = Some(cx.spawn_in(window, async move |this, cx| {
            timer.await;
            let _ = this.update_in(cx, |state, window, cx| {
                state.wake = None;
                if window.is_visible()
                    && window.is_window_active()
                    && !cx.reduce_motion()
                    && let Some(image) = &state.image
                    && image.id == image_id
                {
                    state.frame = (state.frame + 1) % image.frame_count();
                    cx.notify();
                }
            });
        }));
    }
}

fn valid_image(image: &RenderImage) -> bool {
    image.frame_count() > 0 && image.size(0).width.0 > 0 && image.size(0).height.0 > 0
}
fn same_source(a: Option<&ImageSource>, b: Option<&ImageSource>) -> bool {
    match (a, b) {
        (None, None) => true,
        (Some(a), Some(b)) => loader::source_key(a) == loader::source_key(b),
        _ => false,
    }
}

/// A native layout participant; resizing/zooming changes geometry, never the
/// decoded image identity or texture pixels.
pub(crate) struct Picture {
    interactivity: Interactivity,
    state: Entity<PictureState>,
    transform: PreviewTransform,
    affine: bool,
    matrix: Option<[f32; 6]>,
    grayscale: f32,
    object_fit: ObjectFit,
}
impl Picture {
    pub fn new(id: impl Into<ElementId>, state: &Entity<PictureState>) -> Self {
        let mut interactivity = Interactivity::default();
        interactivity.element_id = Some(id.into());
        Self {
            interactivity,
            state: state.clone(),
            transform: PreviewTransform::default(),
            affine: false,
            matrix: None,
            grayscale: 0.,
            object_fit: ObjectFit::Contain,
        }
    }
    pub fn transform(mut self, transform: PreviewTransform) -> Self {
        self.transform = transform;
        self.affine = true;
        self.matrix = None;
        self
    }
    pub fn affine(self, matrix: [f32; 6]) -> Self {
        let mut picture = self.transform(PreviewTransform::default());
        picture.matrix = Some(matrix);
        picture
    }
    pub fn grayscale(mut self, grayscale: f32) -> Self {
        self.grayscale = grayscale;
        self
    }
    pub fn object_fit(mut self, object_fit: ObjectFit) -> Self {
        self.object_fit = object_fit;
        self
    }
}
impl Styled for Picture {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.interactivity.base_style
    }
}
impl IntoElement for Picture {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for Picture {
    type RequestLayoutState = ();
    type PrepaintState = Option<Hitbox>;
    fn id(&self) -> Option<ElementId> {
        self.interactivity.element_id.clone()
    }
    fn source_location(&self) -> Option<&'static core::panic::Location<'static>> {
        None
    }
    fn request_layout(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, ()) {
        self.state.update(cx, |state, cx| state.resolve(window, cx));
        let natural = self.state.read(cx).natural_size();
        let layout = self.interactivity.request_layout(
            id,
            inspector,
            window,
            cx,
            |mut style, window, cx| {
                if let Some(natural) = natural {
                    style
                        .aspect_ratio
                        .get_or_insert(natural.width / natural.height);
                    if matches!(style.size.width, Length::Auto)
                        && matches!(style.size.height, Length::Auto)
                    {
                        style.size.width = natural.width.into();
                    }
                }
                window.request_layout(style, [], cx)
            },
        );
        (layout, ())
    }
    fn prepaint(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) -> Self::PrepaintState {
        self.interactivity.prepaint(
            id,
            inspector,
            bounds,
            bounds.size,
            window,
            cx,
            |_, _, hitbox, _, _| hitbox,
        )
    }
    fn paint(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        hitbox: &mut Self::PrepaintState,
        window: &mut Window,
        cx: &mut App,
    ) {
        let state = self.state.clone();
        let transform = self.transform;
        let affine = self.affine;
        let matrix = self.matrix;
        let grayscale = self.grayscale;
        let fit = &self.object_fit;
        self.interactivity.paint(
            id,
            inspector,
            bounds,
            hitbox.as_ref(),
            window,
            cx,
            |style, window, cx| {
                let picture = state.read(cx);
                let Some(image) = picture.image() else { return };
                let frame = picture.frame.min(image.frame_count().saturating_sub(1));
                let fitted = fit.get_bounds(bounds, image.size(frame));
                let paintable = !fitted.intersect(&window.content_mask().bounds).is_empty();
                let painted = if paintable {
                    if !affine {
                        let corners = style.corner_radii.to_pixels(window.rem_size());
                        renderer::paint_plain(
                            image,
                            frame,
                            renderer::PlainGeometry {
                                bounds,
                                fitted,
                                corners,
                            },
                            grayscale,
                            window,
                            cx,
                        )
                    } else if let Some(matrix) = matrix {
                        renderer::paint_affine(image, frame, fitted, matrix, grayscale, window, cx)
                    } else {
                        renderer::paint(image, frame, fitted, transform, grayscale, window, cx)
                    }
                } else {
                    false
                };
                state.update(cx, |state, cx| {
                    state.schedule(window.is_visible() && painted, window, cx)
                });
            },
        );
    }
}

fn affine(transform: PreviewTransform, center: Point<Pixels>) -> [f32; 6] {
    let (sin, cos) = transform.rotate.to_radians().sin_cos();
    let sx = transform.scale * if transform.flip_x { -1. } else { 1. };
    let sy = transform.scale * if transform.flip_y { -1. } else { 1. };
    let (a, b, c, d) = (sx * cos, sy * sin, -sx * sin, sy * cos);
    let (x, y) = (f32::from(center.x), f32::from(center.y));
    [
        a,
        b,
        c,
        d,
        x + transform.x - a * x - c * y,
        y + transform.y - b * x - d * y,
    ]
}

#[cfg(test)]
mod tests {
    use super::*;
    use gpui::{
        AppContext, Render, Subscription, TestAppContext, WindowHandle, div, point, prelude::*,
        size,
    };
    use std::{cell::RefCell, rc::Rc};

    struct Consumer {
        pictures: Vec<Entity<PictureState>>,
        shown: bool,
        width: f32,
        _events: Vec<Subscription>,
    }
    impl Render for Consumer {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            div()
                .w(px(self.width))
                .flex()
                .flex_col()
                .items_start()
                .when(self.shown, |root| {
                    root.children(self.pictures.iter().enumerate().map(|(index, picture)| {
                        Picture::new(("native-picture", index), picture).w_full()
                    }))
                })
        }
    }
    struct Fixture {
        cx: TestAppContext,
        window: WindowHandle<Consumer>,
        view: Entity<Consumer>,
        events: Rc<RefCell<Vec<PictureEvent>>>,
    }
    impl Fixture {
        fn new(sources: Vec<ImageSource>) -> Self {
            let mut cx = TestAppContext::single();
            let mut view = None;
            let events = Rc::new(RefCell::new(Vec::new()));
            let recorded = events.clone();
            let window = cx.open_window(size(px(600.), px(600.)), |window, cx| {
                window.activate_window();
                let pictures = sources
                    .into_iter()
                    .map(|source| cx.new(|cx| PictureState::new(Some(source), cx)))
                    .collect::<Vec<_>>();
                let subscriptions = pictures
                    .iter()
                    .map(|picture| {
                        let recorded = recorded.clone();
                        cx.subscribe(picture, move |_, _, event, _| {
                            recorded.borrow_mut().push(event.clone())
                        })
                    })
                    .collect();
                view = Some(cx.entity());
                Consumer {
                    pictures,
                    shown: true,
                    width: 200.,
                    _events: subscriptions,
                }
            });
            Self {
                cx,
                window,
                view: view.unwrap(),
                events,
            }
        }
        fn draw(&mut self) {
            self.cx.run_until_parked();
            self.cx
                .update_window(self.window.into(), |_, window, cx| {
                    window.draw(cx).clear(cx)
                })
                .unwrap();
            self.cx.run_until_parked();
        }
        fn picture(&self, index: usize) -> Entity<PictureState> {
            self.view
                .read_with(&self.cx, |view, _| view.pictures[index].clone())
        }
        fn frame(&self) -> usize {
            self.picture(0).read_with(&self.cx, |p, _| p.frame)
        }
    }
    fn image() -> Arc<RenderImage> {
        Arc::new(RenderImage::new(vec![::image::Frame::new(
            ::image::RgbaImage::from_pixel(2, 3, ::image::Rgba([10, 20, 30, 255])),
        )]))
    }
    #[gpui::test]
    fn source_changes_clear_failure_and_keep_retained_source_identity(
        cx: &mut gpui::TestAppContext,
    ) {
        let source = ImageSource::Render(image());
        let state = cx.new(|cx| PictureState::new(Some(source.clone()), cx));
        state.update(cx, |state, cx| {
            assert_eq!(state.status(), PictureStatus::Ready);
            assert_eq!(state.natural_size(), Some(size(px(2.), px(3.))));
            state.set_source(Some(source), cx);
            assert_eq!(state.status(), PictureStatus::Ready);
            state.apply(
                loader::LoadResult::Failed(ImageCacheError::Asset("broken".into())),
                false,
                cx,
            );
            assert_eq!(state.status(), PictureStatus::Error);
            state.set_source(None, cx);
            assert_eq!(state.status(), PictureStatus::Empty);
            assert!(state.error().is_none());
        });
    }
    #[gpui::test]
    fn fallback_success_preserves_primary_error_and_fallback_changes_retry(
        cx: &mut gpui::TestAppContext,
    ) {
        let state = cx.new(|cx| PictureState::new(Some("broken".into()), cx));
        state.update(cx, |state, cx| {
            state.set_fallback(Some(ImageSource::Render(image())), cx);
            state.apply(
                loader::LoadResult::Failed(ImageCacheError::Asset("primary failed".into())),
                false,
                cx,
            );
            assert_eq!(state.status(), PictureStatus::Loading);
            state.apply(loader::LoadResult::Ready(image()), true, cx);
            assert_eq!(state.status(), PictureStatus::Ready);
            assert!(state.error().is_some());
            state.set_fallback(None, cx);
            assert_eq!(state.status(), PictureStatus::Error);
        });
    }
    #[test]
    fn transform_has_screen_axis_flip_and_keeps_center_fixed() {
        let center = point(px(100.), px(50.));
        let matrix = affine(
            PreviewTransform {
                rotate: 90.,
                flip_x: true,
                x: 7.,
                y: -3.,
                ..Default::default()
            },
            center,
        );
        let [a, b, c, d, x, y] = matrix;
        assert!((a * 100. + c * 50. + x - 107.).abs() < 0.001);
        assert!((b * 100. + d * 50. + y - 47.).abs() < 0.001);
        assert!((c - 1.).abs() < 0.001);
        assert!((b - 1.).abs() < 0.001);
    }

    #[test]
    fn percentage_width_uses_intrinsic_aspect_ratio_and_static_images_do_not_schedule() {
        let mut fixture = Fixture::new(vec![ImageSource::Render(image())]);
        fixture.draw();
        fixture.draw();
        fixture
            .cx
            .update_window(fixture.window.into(), |_, window, cx| {
                let sprites = &window.rendered_scene().polychrome_sprites;
                assert_eq!(sprites.len(), 1);
                let scale = window.scale_factor();
                assert_eq!(sprites[0].bounds.size.width.0 / scale, 200.);
                assert_eq!(sprites[0].bounds.size.height.0 / scale, 300.);
                assert_eq!(window.simulate_next_frame(cx), 0);
            })
            .unwrap();
        assert!(
            fixture
                .picture(0)
                .read_with(&fixture.cx, |p, _| p.wake.is_none())
        );
        assert_eq!(fixture.events.borrow().len(), 1);
    }

    #[test]
    fn encoded_sources_share_decoding_and_emit_one_ready_event_per_consumer() {
        let source = ImageSource::Image(Arc::new(gpui::Image::from_bytes(
            gpui::ImageFormat::Pnm,
            b"P6\n1 1\n255\n\x10\x20\x30".to_vec(),
        )));
        let mut fixture = Fixture::new(vec![source.clone(), source]);
        fixture.draw();
        fixture.draw();
        let first = fixture
            .picture(0)
            .read_with(&fixture.cx, |p, _| p.image().unwrap());
        let second = fixture
            .picture(1)
            .read_with(&fixture.cx, |p, _| p.image().unwrap());
        assert!(Arc::ptr_eq(&first, &second));
        assert_eq!(fixture.events.borrow().len(), 2);
        fixture.draw();
        assert_eq!(fixture.events.borrow().len(), 2);
    }

    #[test]
    fn svg_sources_keep_logical_intrinsic_size_when_the_decoder_supersamples() {
        let source = "data:image/svg+xml,%3Csvg%20xmlns='http://www.w3.org/2000/svg'%20width='4'%20height='3'%3E%3Crect%20width='4'%20height='3'%20fill='red'/%3E%3C/svg%3E";
        let mut fixture = Fixture::new(vec![source.into()]);
        fixture.draw();
        fixture.draw();
        let state = fixture.picture(0);
        assert_eq!(
            state.read_with(&fixture.cx, |p, _| p.natural_size()),
            Some(size(px(4.), px(3.)))
        );
        assert!(
            matches!(&fixture.events.borrow()[..],[PictureEvent::Loaded{size:loaded,..}] if *loaded==size(px(4.),px(3.)))
        );
    }

    #[test]
    fn broken_custom_source_uses_fallback_once_and_keeps_error_events_typed() {
        let source = ImageSource::Custom(Arc::new(|_, _| {
            Some(Err(ImageCacheError::Asset("broken source".into())))
        }));
        let mut fixture = Fixture::new(vec![source]);
        fixture.picture(0).update(&mut fixture.cx, |p, cx| {
            p.set_fallback(Some(ImageSource::Render(image())), cx)
        });
        fixture.draw();
        fixture.draw();
        assert_eq!(
            fixture.picture(0).read_with(&fixture.cx, |p, _| p.status()),
            PictureStatus::Ready
        );
        assert!(matches!(
            &fixture.events.borrow()[..],
            [
                PictureEvent::Error {
                    fallback: false,
                    ..
                },
                PictureEvent::Loaded { fallback: true, .. }
            ]
        ));
        fixture.draw();
        assert_eq!(fixture.events.borrow().len(), 2);
    }

    #[test]
    fn animation_honors_frame_delays_and_stops_for_reduced_motion_and_unmount() {
        let frames = (0..2)
            .map(|index| {
                ::image::Frame::from_parts(
                    ::image::RgbaImage::from_pixel(1, 1, ::image::Rgba([index, 0, 0, 255])),
                    0,
                    0,
                    ::image::Delay::from_numer_denom_ms(100, 1),
                )
            })
            .collect::<Vec<_>>();
        let mut fixture = Fixture::new(vec![ImageSource::Render(Arc::new(RenderImage::new(
            frames,
        )))]);
        fixture.draw();
        assert_eq!(fixture.frame(), 0);
        fixture
            .cx
            .executor()
            .advance_clock(Duration::from_millis(99));
        fixture.cx.run_until_parked();
        assert_eq!(fixture.frame(), 0);
        fixture
            .cx
            .executor()
            .advance_clock(Duration::from_millis(1));
        fixture.cx.run_until_parked();
        assert_eq!(fixture.frame(), 1);
        fixture.draw();
        fixture.cx.update(|cx| cx.set_reduce_motion(true));
        fixture.draw();
        fixture
            .cx
            .executor()
            .advance_clock(Duration::from_millis(500));
        fixture.cx.run_until_parked();
        assert_eq!(fixture.frame(), 1);
        assert!(
            fixture
                .picture(0)
                .read_with(&fixture.cx, |p, _| p.wake.is_none())
        );
        fixture.cx.update(|cx| cx.set_reduce_motion(false));
        fixture.draw();
        fixture.view.update(&mut fixture.cx, |view, cx| {
            view.shown = false;
            cx.notify();
        });
        fixture.draw();
        fixture
            .cx
            .executor()
            .advance_clock(Duration::from_millis(100));
        fixture.cx.run_until_parked();
        fixture.draw();
        assert!(
            fixture
                .picture(0)
                .read_with(&fixture.cx, |p, _| p.wake.is_none())
        );
    }
}
