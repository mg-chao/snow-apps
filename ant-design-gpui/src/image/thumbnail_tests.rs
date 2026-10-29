// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{ConfigScope, UiRoot};
use gpui::{
    AppContext, Context, Element, Entity, ImageCacheError, IntoElement, Render, RenderImage,
    Subscription, TestAppContext, VisualTestContext, WindowHandle, div, prelude::*, px, size,
};
use std::{
    cell::RefCell,
    rc::Rc,
    sync::{Arc, Mutex},
};

struct Page {
    image: Entity<ImageState>,
    scope: ConfigScope,
    placeholder: Option<ImagePlaceholder>,
    mounted: bool,
    styles: ImageStyles,
    resolver: ImageStyleResolver,
    _events: Subscription,
}
impl Render for Page {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        div().size_full().when(self.mounted, |root| {
            root.child(
                Image::new("loading-thumbnail", &self.image, cx)
                    .scope(self.scope.clone())
                    .automation_id("image.loading.test")
                    .styles_fn(self.resolver.clone())
                    .styles(self.styles.clone())
                    .when_some(self.placeholder.clone(), |image, placeholder| {
                        image.placeholder(placeholder)
                    }),
            )
        })
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    page: Entity<Page>,
    image: Entity<ImageState>,
    scope: ConfigScope,
    events: Rc<RefCell<Vec<ImageEvent>>>,
    contexts: Arc<Mutex<Vec<ImageStyleContext>>>,
}
impl Fixture {
    fn new(source: Option<gpui::ImageSource>, placeholder: Option<ImagePlaceholder>) -> Self {
        let mut cx = TestAppContext::single();
        let events = Rc::new(RefCell::new(Vec::new()));
        let output = events.clone();
        let contexts = Arc::new(Mutex::new(Vec::new()));
        let observed = contexts.clone();
        let mut created = None;
        let window = cx.open_window(size(px(600.), px(400.)), |window, cx| {
            window.activate_window();
            let scope = ConfigScope::new_window(cx);
            scope
                .update_patch(cx, |patch| patch.motion = Some(false))
                .unwrap();
            let image = cx.new(|cx| {
                let mut state = match source {
                    Some(source) => ImageState::new(source, window, cx),
                    None => ImageState::empty(window, cx),
                };
                state.set_alt("Photograph of mountains", cx);
                state
            });
            let page = cx.new(|cx| Page {
                image: image.clone(),
                scope: scope.clone(),
                placeholder,
                mounted: true,
                styles: ImageStyles {
                    root: gpui::StyleRefinement::default().w(px(200.)).h(px(120.)),
                    ..Default::default()
                },
                resolver: ImageStyleResolver::new(move |context, _, _| {
                    observed.lock().unwrap().push(*context);
                    ImageStyles::default()
                }),
                _events: cx.subscribe(&image, move |_, _, event, _| {
                    output.borrow_mut().push(event.clone())
                }),
            });
            created = Some((page.clone(), image, scope.clone()));
            UiRoot::new(scope, page, window, cx)
        });
        let (page, image, scope) = created.unwrap();
        let mut result = Self {
            cx,
            window,
            page,
            image,
            scope,
            events,
            contexts,
        };
        result.draw();
        result
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
    fn rerender(&mut self) {
        self.page.update(&mut self.cx, |_, cx| cx.notify());
        self.draw();
    }
    fn placeholder_visible(&mut self) -> bool {
        VisualTestContext::from_window(self.window.into(), &self.cx)
            .debug_bounds("image-custom-placeholder")
            .is_some()
    }
    fn status(&self) -> PictureStatus {
        self.image.read_with(&self.cx, |image, cx| image.status(cx))
    }
    fn context(&self) -> ImageStyleContext {
        *self.contexts.lock().unwrap().last().unwrap()
    }
    fn set_styles(&mut self, styles: ImageStyles) {
        self.page.update(&mut self.cx, |page, cx| {
            page.styles = styles;
            cx.notify();
        });
        self.draw();
    }
    fn bounds(&self, selector: &'static str) -> gpui::Bounds<gpui::Pixels> {
        VisualTestContext::from_window(self.window.into(), &self.cx)
            .debug_bounds(selector)
            .unwrap()
    }
    fn error_visible(&self) -> bool {
        VisualTestContext::from_window(self.window.into(), &self.cx)
            .debug_bounds("image-error-content")
            .is_some()
    }
}
fn image() -> Arc<RenderImage> {
    Arc::new(RenderImage::new(vec![::image::Frame::new(
        ::image::RgbaImage::from_pixel(2, 3, ::image::Rgba([10, 20, 30, 255])),
    )]))
}
fn custom_placeholder() -> ImagePlaceholder {
    ImagePlaceholder::Content(Rc::new(|_, _| {
        div()
            .id("custom-loading-content")
            .debug_selector(|| "image-custom-placeholder".into())
            .size_full()
            .child("Loading photograph")
            .into_any_element()
    }))
}
type PendingResult = Rc<RefCell<Option<Result<Arc<RenderImage>, ImageCacheError>>>>;
fn pending_source() -> (gpui::ImageSource, PendingResult) {
    let pending = Rc::new(RefCell::new(None));
    let result = pending.clone();
    (
        gpui::ImageSource::from(move |_: &mut Window, _: &mut App| result.borrow().clone()),
        pending,
    )
}

#[test]
fn image_loading_placeholder_releases_on_ready_and_notifies_cached_owner_once() {
    let (source, pending) = pending_source();
    let mut fixture = Fixture::new(Some(source), Some(custom_placeholder()));
    assert_eq!(fixture.status(), PictureStatus::Loading);
    assert!(fixture.context().loading);
    assert!(fixture.placeholder_visible());
    assert!(fixture.events.borrow().is_empty());
    *pending.borrow_mut() = Some(Ok(image()));
    fixture.rerender();
    fixture.draw();
    assert_eq!(fixture.status(), PictureStatus::Ready);
    assert!(!fixture.context().loading);
    assert!(!fixture.placeholder_visible());
    assert!(matches!(
        &fixture.events.borrow()[..],
        [ImageEvent::Loaded {
            fallback: false,
            ..
        }]
    ));
    fixture.rerender();
    fixture.rerender();
    assert_eq!(
        fixture.events.borrow().len(),
        1,
        "owner renders must retain the existing event subscription"
    );
}

#[test]
fn image_failure_removes_placeholder_and_fallback_success_restores_pixels() {
    let (source, pending) = pending_source();
    let mut fixture = Fixture::new(Some(source), Some(custom_placeholder()));
    *pending.borrow_mut() = Some(Err(ImageCacheError::Asset("image request failed".into())));
    fixture.rerender();
    fixture.draw();
    assert_eq!(fixture.status(), PictureStatus::Error);
    assert!(fixture.context().failed);
    assert!(!fixture.placeholder_visible());
    fixture.image.update(&mut fixture.cx, |state, cx| {
        state.set_fallback(Some(gpui::ImageSource::Render(image())), cx)
    });
    fixture.draw();
    fixture.draw();
    assert_eq!(fixture.status(), PictureStatus::Ready);
    assert!(!fixture.context().failed);
    assert!(matches!(
        &fixture.events.borrow()[..],
        [
            ImageEvent::Error {
                fallback: false,
                ..
            },
            ImageEvent::Loaded { fallback: true, .. }
        ]
    ));
}

#[test]
fn terminal_error_content_has_intrinsic_height_and_preserves_root_and_image_dimensions() {
    let (source, pending) = pending_source();
    let mut f = Fixture::new(Some(source), None);
    *pending.borrow_mut() = Some(Err(ImageCacheError::Asset("missing image".into())));
    f.rerender();
    f.draw();
    assert_eq!(f.status(), PictureStatus::Error);
    assert_eq!(
        f.bounds("image.loading.test").size,
        size(px(200.), px(120.))
    );
    assert_eq!(
        f.bounds("image-error-content").size,
        size(px(200.), px(120.))
    );

    // An explicit auto height and an absent height must both retain the
    // terminal presentation's intrinsic size; the failed bitmap has none.
    f.set_styles(ImageStyles {
        root: gpui::StyleRefinement::default().w(px(200.)).h_auto(),
        ..Default::default()
    });
    assert_eq!(f.bounds("image.loading.test").size.width, px(200.));
    assert!(f.bounds("image.loading.test").size.height >= px(16.));
    let line_height = f.bounds("image.loading.test").size.height;
    f.image.update(&mut f.cx, |state, cx| state.set_alt("", cx));
    f.draw();
    f.set_styles(ImageStyles::default());
    assert!(f.bounds("image.loading.test").size.width >= px(16.));
    assert_eq!(f.bounds("image.loading.test").size.height, px(16.));
    assert!(
        f.cx.update_window(f.window.into(), |_, w, _| {
            !w.rendered_scene().monochrome_sprites.is_empty()
        })
        .unwrap(),
        "an empty alt still paints the native error icon"
    );

    f.image.update(&mut f.cx, |state, cx| {
        state.set_alt("This unavailable photograph has descriptive alternative text that exceeds the available width.", cx)
    });
    f.set_styles(ImageStyles {
        root: gpui::StyleRefinement::default().w(px(100.)),
        ..Default::default()
    });
    let root = f.bounds("image.loading.test");
    let content = f.bounds("image-error-content");
    assert_eq!(root.size.width, px(100.));
    assert_eq!(
        root.size.height, line_height,
        "long alt remains one clipped line"
    );
    assert!(content.bottom() <= root.bottom());

    f.set_styles(ImageStyles {
        root: gpui::StyleRefinement::default().w(px(200.)).h(px(120.)),
        image: gpui::StyleRefinement::default().w(px(96.)).h(px(64.)),
        ..Default::default()
    });
    assert_eq!(
        f.bounds("image.loading.test").size,
        size(px(200.), px(120.))
    );
    assert_eq!(f.bounds("image-error-content").size, size(px(96.), px(64.)));
    f.set_styles(ImageStyles {
        image: gpui::StyleRefinement::default().w(px(96.)).h(px(64.)),
        ..Default::default()
    });
    assert_eq!(f.bounds("image.loading.test").size.height, px(64.));
    f.set_styles(ImageStyles {
        root: gpui::StyleRefinement::default().w(px(8.)).h(px(8.)),
        ..Default::default()
    });
    assert_eq!(f.bounds("image.loading.test").size, size(px(8.), px(8.)));
    assert_eq!(f.bounds("image-error-content").size, size(px(8.), px(8.)));
    assert!(
        f.cx.update_window(f.window.into(), |_, w, _| {
            let scale = w.scale_factor();
            w.rendered_scene().monochrome_sprites.iter().all(|sprite| {
                sprite.content_mask.bounds.size.width.0 / scale <= 8.
                    && sprite.content_mask.bounds.size.height.0 / scale <= 8.
            })
        })
        .unwrap(),
        "small explicit slots clip their icon and alt content"
    );
}

#[test]
fn terminal_error_content_recovers_intrinsic_image_layout_after_late_fallback_or_source() {
    let (source, pending) = pending_source();
    let mut f = Fixture::new(Some(source), None);
    f.set_styles(ImageStyles {
        root: gpui::StyleRefinement::default().w(px(200.)).h_auto(),
        ..Default::default()
    });
    *pending.borrow_mut() = Some(Err(ImageCacheError::Asset("missing image".into())));
    f.rerender();
    f.draw();
    assert_eq!(f.status(), PictureStatus::Error);
    assert!(f.error_visible());
    assert!(f.bounds("image.loading.test").size.height >= px(16.));
    let (fallback, pending_fallback) = pending_source();
    f.image.update(&mut f.cx, |state, cx| {
        state.set_fallback(Some(fallback), cx)
    });
    assert_eq!(f.status(), PictureStatus::Loading);
    f.draw();
    assert!(!f.error_visible());
    *pending_fallback.borrow_mut() = Some(Ok(image()));
    f.rerender();
    f.draw();
    assert_eq!(f.status(), PictureStatus::Ready);
    assert!(!f.error_visible());
    assert_eq!(
        f.bounds("image.loading.test").size,
        size(px(200.), px(300.))
    );
    f.image
        .update(&mut f.cx, |state, cx| state.set_fallback(None, cx));
    f.draw();
    f.draw();
    assert_eq!(f.status(), PictureStatus::Error);
    assert!(f.error_visible());
    f.image.update(&mut f.cx, |state, cx| {
        state.set_source(Some(gpui::ImageSource::Render(image())), cx)
    });
    f.draw();
    f.draw();
    assert_eq!(f.status(), PictureStatus::Ready);
    assert!(!f.error_visible());
    assert_eq!(
        f.bounds("image.loading.test").size,
        size(px(200.), px(300.))
    );
}

#[test]
fn replacing_a_pending_source_does_not_publish_its_late_result() {
    let (source, pending) = pending_source();
    let mut fixture = Fixture::new(Some(source), Some(custom_placeholder()));
    let replacement = image();
    fixture.image.update(&mut fixture.cx, |state, cx| {
        state.set_source(Some(gpui::ImageSource::Render(replacement.clone())), cx)
    });
    fixture.draw();
    fixture.draw();
    *pending.borrow_mut() = Some(Err(ImageCacheError::Asset("obsolete failure".into())));
    fixture.rerender();
    assert_eq!(fixture.status(), PictureStatus::Ready);
    let picture = fixture
        .image
        .read_with(&fixture.cx, |state, _| state.picture());
    assert!(Arc::ptr_eq(
        &replacement,
        &picture.read_with(&fixture.cx, |state, _| state.image().unwrap())
    ));
    assert!(
        fixture
            .events
            .borrow()
            .iter()
            .all(|event| matches!(event, ImageEvent::Loaded { .. }))
    );
}

#[test]
fn source_less_placeholder_remains_visible_without_loading_or_preview_registration() {
    let mut fixture = Fixture::new(None, Some(custom_placeholder()));
    fixture.draw();
    assert!(fixture.placeholder_visible());
    assert_eq!(fixture.status(), PictureStatus::Empty);
    assert!(!fixture.context().preview);
    assert!(fixture.events.borrow().is_empty());
    let preview = fixture
        .image
        .read_with(&fixture.cx, |state, _| state.preview());
    assert!(preview.read_with(&fixture.cx, |state, _| state.items().is_empty()));
}

#[test]
fn explicit_progress_suppresses_source_loading_and_thumbnail_activation() {
    let calls = Rc::new(std::cell::Cell::new(0));
    let requested = calls.clone();
    let source = gpui::ImageSource::from(move |_: &mut Window, _: &mut App| {
        requested.set(requested.get() + 1);
        Some(Err(ImageCacheError::Asset(
            "should not be requested".into(),
        )))
    });
    let mut fixture = Fixture::new(
        Some(source),
        Some(ImagePlaceholder::Progress(ImageProgress {
            percent: Some(75.),
            render: None,
        })),
    );
    fixture.rerender();
    assert!(fixture.context().loading);
    assert!(!fixture.context().preview);
    assert_eq!(calls.get(), 0);
    assert!(fixture.events.borrow().is_empty());
}

#[test]
fn progress_semantics_expose_determinate_range_and_localized_indeterminate_label() {
    let finite = super::thumbnail::progress_semantics(
        div().id("progress"),
        Some(75),
        "Loading picture".into(),
    );
    assert_eq!(finite.a11y_role(), Some(gpui::Role::ProgressIndicator));
    let mut node = gpui::accesskit::Node::new(gpui::Role::ProgressIndicator);
    finite.write_a11y_info(&mut node);
    assert_eq!(node.label(), Some("75%"));
    assert_eq!(node.numeric_value(), Some(75.));
    assert_eq!(node.min_numeric_value(), Some(0.));
    assert_eq!(node.max_numeric_value(), Some(100.));
    let unknown =
        super::thumbnail::progress_semantics(div().id("progress"), None, "Loading picture".into());
    let mut node = gpui::accesskit::Node::new(gpui::Role::ProgressIndicator);
    unknown.write_a11y_info(&mut node);
    assert_eq!(node.label(), Some("Loading picture"));
    assert_eq!(node.numeric_value(), None);
}

#[test]
fn progress_rail_interpolates_updates_continuously_and_paints_rounded_ends() {
    use std::time::Duration;

    fn set_motion(f: &mut Fixture, enabled: bool) {
        f.cx.update(|cx| {
            f.scope
                .update_patch(cx, |patch| patch.motion = Some(enabled))
        })
        .unwrap();
        f.draw();
    }

    fn set_percent(f: &mut Fixture, percent: Option<f32>) {
        f.page.update(&mut f.cx, |page, cx| {
            page.placeholder = Some(ImagePlaceholder::Progress(ImageProgress {
                percent,
                ..Default::default()
            }));
            cx.notify();
        });
        f.draw();
    }
    fn rail(f: &mut Fixture) -> (f32, f32, f32) {
        f.cx.update_window(f.window.into(), |_, w, _| {
            let scale = w.scale_factor();
            let quads: Vec<_> = w
                .rendered_scene()
                .quads
                .iter()
                .filter(|quad| {
                    quad.background.as_solid().is_none()
                        && (quad.bounds.size.height.0 / scale - 6.).abs() < 0.01
                })
                .collect();
            let first = quads
                .iter()
                .min_by(|a, b| a.bounds.origin.x.0.total_cmp(&b.bounds.origin.x.0))
                .unwrap();
            let last = quads
                .iter()
                .max_by(|a, b| a.bounds.right().0.total_cmp(&b.bounds.right().0))
                .unwrap();
            (
                (last.bounds.right().0 - first.bounds.origin.x.0) / scale,
                first.corner_radii.top_left.0 / scale,
                last.corner_radii.top_right.0 / scale,
            )
        })
        .unwrap()
    }
    let mut f = Fixture::new(
        None,
        Some(ImagePlaceholder::Progress(ImageProgress {
            percent: Some(25.),
            ..Default::default()
        })),
    );
    let initial = rail(&mut f);
    assert_eq!(initial, (38., 2., 1.));
    set_motion(&mut f, true);
    set_percent(&mut f, Some(75.));
    assert_eq!(
        rail(&mut f).0,
        initial.0,
        "updates start at the painted width"
    );
    f.cx.executor().advance_clock(Duration::from_millis(100));
    f.rerender();
    let halfway = rail(&mut f).0;
    // CSS ease at 50% time is 0.802403; the default transition lasts 200 ms.
    assert!((halfway - (38. + 76. * 0.802403)).abs() <= 0.51);
    set_percent(&mut f, Some(50.));
    assert!(
        (rail(&mut f).0 - halfway).abs() <= 0.01,
        "retargeting must not jump"
    );
    set_motion(&mut f, false);
    assert_eq!(rail(&mut f).0, 76., "reduced motion snaps to the target");
    set_percent(&mut f, Some(100.));
    assert_eq!(rail(&mut f), (152., 2., 2.));
    set_percent(&mut f, None);
    set_motion(&mut f, true);
    set_percent(&mut f, Some(50.));
    assert_eq!(
        rail(&mut f).0,
        76.,
        "a newly mounted rail uses its initial width"
    );
    set_percent(&mut f, Some(75.));
    f.page.update(&mut f.cx, |page, cx| {
        page.mounted = false;
        cx.notify();
    });
    f.draw();
    set_percent(&mut f, Some(25.));
    f.page.update(&mut f.cx, |page, cx| {
        page.mounted = true;
        cx.notify();
    });
    f.draw();
    assert_eq!(rail(&mut f).0, 38., "remounting starts a new rail lifetime");
}
