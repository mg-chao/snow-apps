// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Shared, bounded decoded assets. A mounted consumer leases its load independently
//! of the idle LRU. Background results belong to their source key, so an old
//! completion cannot replace a subsequently selected image.

use gpui::{
    AnyImageCache, App, AppContext, Asset, Entity, EntityId, Global, ImageCache, ImageCacheError,
    ImageSource, ImgResourceLoader, RenderImage, Resource, Task, WeakEntity, Window,
};
use std::{
    cell::RefCell,
    collections::HashMap,
    future::Future,
    rc::{Rc, Weak},
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
};

#[cfg(test)]
use std::cell::Cell;

const MAX_IDLE_ENTRIES: usize = 128;

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub(super) enum SourceKey {
    Resource(Resource),
    Render(usize),
    Encoded(u64),
    Custom(usize),
}

pub(super) fn source_key(source: &ImageSource) -> SourceKey {
    match source {
        ImageSource::Resource(resource) => SourceKey::Resource(resource.clone()),
        ImageSource::Render(image) => SourceKey::Render(image.id.0),
        ImageSource::Image(image) => SourceKey::Encoded(image.id()),
        ImageSource::Custom(loader) => SourceKey::Custom(Arc::as_ptr(loader) as *const () as usize),
    }
}

#[derive(Clone, Debug)]
pub(super) enum LoadResult {
    Pending,
    Ready(Arc<RenderImage>),
    Failed(ImageCacheError),
}

fn classify(value: Option<Result<Arc<RenderImage>, ImageCacheError>>) -> LoadResult {
    match value {
        None => LoadResult::Pending,
        Some(Ok(image))
            if image.frame_count() > 0
                && image.size(0).width.0 > 0
                && image.size(0).height.0 > 0 =>
        {
            LoadResult::Ready(image)
        }
        Some(Ok(_)) => {
            LoadResult::Failed(ImageCacheError::Asset("Image has no decoded pixels".into()))
        }
        Some(Err(error)) => LoadResult::Failed(error),
    }
}

pub(super) fn resolve(
    source: &ImageSource,
    cache: Option<&AnyImageCache>,
    window: &mut Window,
    cx: &mut App,
) -> LoadResult {
    let result = match source {
        ImageSource::Render(image) => Some(Ok(image.clone())),
        ImageSource::Custom(loader) => loader(window, cx),
        ImageSource::Resource(resource) => {
            if let Some(cache) = cache {
                cache.load(resource, window, cx)
            } else {
                let cache = shared_cache(cx);
                cache.update(cx, |cache, cx| cache.load(resource, window, cx))
            }
        }
        ImageSource::Image(image) => {
            let cache = shared_cache(cx);
            cache.update(cx, |cache, cx| cache.load_encoded(image, window, cx))
        }
    };
    classify(result)
}

enum PreparationState<T> {
    Pending(HashMap<EntityId, usize>),
    Ready(Arc<T>),
}

/// A cancellable background preparation. Results stay attached to the original
/// cache key, and completion only invalidates views which requested that key.
pub(crate) struct Preparation<T> {
    id: u64,
    state: Rc<RefCell<PreparationState<T>>>,
    _task: Task<()>,
}

impl<T: Send + Sync + 'static> Preparation<T> {
    pub(crate) fn new(future: impl Future<Output = T> + Send + 'static, cx: &App) -> Self {
        let state = Rc::new(RefCell::new(PreparationState::Pending(HashMap::new())));
        let weak = Rc::downgrade(&state);
        let task = cx.background_executor().spawn(future);
        let task = cx.spawn(async move |cx| {
            let result = Arc::new(task.await);
            let Some(state) = weak.upgrade() else { return };
            let old = state.replace(PreparationState::Ready(result));
            if let PreparationState::Pending(views) = old {
                cx.update(|cx| {
                    for view in views.into_keys() {
                        cx.notify(view);
                    }
                });
            }
        });
        Self {
            id: crate::behavior::resources::next_resource_id(),
            state,
            _task: task,
        }
    }

    #[cfg(test)]
    pub(crate) fn get(&self) -> Option<Arc<T>> {
        match &*self.state.borrow() {
            PreparationState::Pending(_) => None,
            PreparationState::Ready(value) => Some(value.clone()),
        }
    }

    pub(crate) fn use_value(&self, window: &mut Window, cx: &mut App) -> Option<Arc<T>> {
        if let PreparationState::Ready(value) = &*self.state.borrow() {
            return Some(value.clone());
        }
        let view = window.current_view();
        window.use_keyed_state(("ant-image-preparation-waiter", self.id), cx, |_, _| {
            if let PreparationState::Pending(views) = &mut *self.state.borrow_mut() {
                *views.entry(view).or_default() += 1;
            }
            PreparationWaiter {
                state: Rc::downgrade(&self.state),
                view,
            }
        });
        None
    }
}

/// Waiters follow scene ownership independently of the shared preparation. A
/// hidden/departed consumer must not accumulate subscriptions while another
/// scene still waits for the same source.
struct PreparationWaiter<T> {
    state: Weak<RefCell<PreparationState<T>>>,
    view: EntityId,
}
impl<T> Drop for PreparationWaiter<T> {
    fn drop(&mut self) {
        if let Some(state) = self.state.upgrade()
            && let PreparationState::Pending(views) = &mut *state.borrow_mut()
            && let Some(count) = views.get_mut(&self.view)
        {
            *count -= 1;
            if *count == 0 {
                views.remove(&self.view);
                if views.is_empty() {
                    *views = HashMap::new();
                } else if views.capacity() > views.len().max(16) * 4 {
                    views.shrink_to(views.len().max(16));
                }
            }
        }
    }
}

type Decoded = Result<Arc<RenderImage>, ImageCacheError>;
struct Record {
    id: u64,
    load: Preparation<Decoded>,
    #[cfg(test)]
    bytes: Cell<Option<usize>>,
    key: SourceKey,
    owner: WeakEntity<DecodedImageCache>,
}
impl Record {
    #[cfg(test)]
    fn bytes(&self) -> usize {
        if let Some(bytes) = self.bytes.get() {
            return bytes;
        }
        let Some(value) = self.load.get() else {
            return 0;
        };
        let bytes = value.as_ref().as_ref().map_or(0, |image| {
            (0..image.frame_count()).fold(0usize, |bytes, frame| {
                bytes.saturating_add(image.as_bytes(frame).map_or(0, <[u8]>::len))
            })
        });
        self.bytes.set(Some(bytes));
        bytes
    }
}
struct Entry {
    record: Weak<Record>,
    used: u64,
}

/// Weak, bounded lookup metadata. Pending work and decoded pixels belong only
/// to scenes using the source; there is no idle decoded-image payload cache.
#[derive(Default)]
pub(super) struct DecodedImageCache {
    owner: Option<WeakEntity<Self>>,
    entries: HashMap<u64, Entry>,
    tick: u64,
}
struct SharedDecodedCache(Entity<DecodedImageCache>);
impl Global for SharedDecodedCache {}

pub(super) fn shared_cache(cx: &mut App) -> Entity<DecodedImageCache> {
    if !cx.has_global::<SharedDecodedCache>() {
        let cache = new_cache(cx);
        cx.set_global(SharedDecodedCache(cache));
    }
    cx.global::<SharedDecodedCache>().0.clone()
}

pub(super) fn new_cache(cx: &mut App) -> Entity<DecodedImageCache> {
    cx.new(|cx| DecodedImageCache {
        owner: Some(cx.weak_entity()),
        ..Default::default()
    })
}

fn fingerprint(key: &SourceKey) -> u64 {
    use std::hash::{Hash, Hasher};
    let mut hash = std::collections::hash_map::DefaultHasher::new();
    key.hash(&mut hash);
    hash.finish()
}

fn retire(record: &Rc<Record>, cx: &mut App) {
    let hash = fingerprint(&record.key);
    let id = record.id;
    let _ = record.owner.update(cx, |cache, _| {
        if cache
            .entries
            .get(&hash)
            .is_some_and(|entry| entry.record.upgrade().is_none_or(|record| record.id == id))
        {
            cache.entries.remove(&hash);
            if cache.entries.is_empty() {
                cache.entries = HashMap::new();
            }
        }
    });
}

impl DecodedImageCache {
    fn get(
        &mut self,
        key: SourceKey,
        create: impl FnOnce(&mut App) -> Preparation<Decoded>,
        window: &mut Window,
        cx: &mut App,
    ) -> Option<Decoded> {
        let owner = self
            .owner
            .as_ref()
            .expect("decoded registry owner")
            .entity_id();
        let record = window.with_id(("ant-image-decoded-owner", owner), |window| {
            crate::behavior::resources::scene_resource(
                ("ant-image-decoded-source", fingerprint(&key)),
                |record: &Rc<Record>| record.key == key,
                |_, cx| self.record(key.clone(), create, cx),
                |record| Rc::strong_count(record) == 1,
                retire,
                window,
                cx,
            )
        });
        record
            .load
            .use_value(window, cx)
            .map(|value| value.as_ref().clone())
    }

    fn record(
        &mut self,
        key: SourceKey,
        create: impl FnOnce(&mut App) -> Preparation<Decoded>,
        cx: &mut App,
    ) -> Rc<Record> {
        self.tick = self.tick.wrapping_add(1);
        let hash = fingerprint(&key);
        let record = self
            .entries
            .get(&hash)
            .and_then(|entry| entry.record.upgrade())
            .filter(|record| record.key == key)
            .unwrap_or_else(|| {
                static NEXT: AtomicU64 = AtomicU64::new(1);
                Rc::new(Record {
                    id: NEXT.fetch_add(1, Ordering::Relaxed),
                    load: create(cx),
                    #[cfg(test)]
                    bytes: Cell::new(None),
                    key,
                    owner: self.owner.clone().expect("decoded registry owner"),
                })
            });
        self.entries
            .retain(|_, entry| entry.record.strong_count() > 0);
        self.entries.insert(
            hash,
            Entry {
                record: Rc::downgrade(&record),
                used: self.tick,
            },
        );
        while self.entries.len() > MAX_IDLE_ENTRIES {
            let key = *self
                .entries
                .iter()
                .min_by_key(|(_, entry)| entry.used)
                .unwrap()
                .0;
            self.entries.remove(&key);
        }
        record
    }

    fn load_encoded(
        &mut self,
        image: &Arc<gpui::Image>,
        window: &mut Window,
        cx: &mut App,
    ) -> Option<Decoded> {
        let image = image.clone();
        self.get(
            SourceKey::Encoded(image.id()),
            move |cx| {
                let renderer = cx.svg_renderer();
                Preparation::new(async move { decode_encoded(&image, renderer) }, cx)
            },
            window,
            cx,
        )
    }
}

fn decode_encoded(image: &gpui::Image, renderer: gpui::SvgRenderer) -> Decoded {
    // GPUI's resource decoder supports animated WebP, but its encoded-image
    // helper only returns the first WebP frame. Preserve source-independent
    // playback while continuing to share all other decoding with GPUI.
    if image.format() == gpui::ImageFormat::Webp {
        use ::image::AnimationDecoder;
        let mut decoder =
            ::image::codecs::webp::WebPDecoder::new(std::io::Cursor::new(image.bytes()))?;
        if decoder.has_animation() {
            let _ = decoder.set_background_color(::image::Rgba([0, 0, 0, 0]));
            let mut frames = Vec::new();
            for mut frame in decoder.into_frames().flatten() {
                for pixel in frame.buffer_mut().as_chunks_mut::<4>().0 {
                    pixel.swap(0, 2);
                }
                frames.push(frame);
            }
            if frames.is_empty() {
                return Err(ImageCacheError::Asset(
                    "WebP has no decodable frames".into(),
                ));
            }
            return Ok(Arc::new(RenderImage::new(frames)));
        }
    }
    image.to_image_data(renderer).map_err(ImageCacheError::from)
}

impl ImageCache for DecodedImageCache {
    fn load(&mut self, resource: &Resource, window: &mut Window, cx: &mut App) -> Option<Decoded> {
        let resource = resource.clone();
        self.get(
            SourceKey::Resource(resource.clone()),
            move |cx| {
                if let Resource::Uri(uri) = &resource
                    && uri
                        .as_ref()
                        .get(..5)
                        .is_some_and(|prefix| prefix.eq_ignore_ascii_case("data:"))
                {
                    let uri = uri.clone();
                    let renderer = cx.svg_renderer();
                    return Preparation::new(async move { decode_data_uri(&uri, renderer) }, cx);
                }
                let future = ImgResourceLoader::load(resource, cx);
                Preparation::new(future, cx)
            },
            window,
            cx,
        )
    }
}

fn decode_data_uri(uri: &str, renderer: gpui::SvgRenderer) -> Decoded {
    let uri = data_url::DataUrl::process(uri)
        .map_err(|error| ImageCacheError::Asset(error.to_string().into()))?;
    let (bytes, _) = uri
        .decode_to_vec()
        .map_err(|error| ImageCacheError::Asset(error.to_string().into()))?;
    match ::image::guess_format(&bytes) {
        Ok(::image::ImageFormat::Gif) => decode_encoded(
            &gpui::Image::from_bytes(gpui::ImageFormat::Gif, bytes),
            renderer,
        ),
        Ok(::image::ImageFormat::WebP) => decode_encoded(
            &gpui::Image::from_bytes(gpui::ImageFormat::Webp, bytes),
            renderer,
        ),
        Ok(format) => {
            use ::image::ImageDecoder;
            let mut decoder =
                ::image::ImageReader::with_format(std::io::Cursor::new(bytes), format)
                    .into_decoder()?;
            let orientation = decoder.orientation()?;
            let mut image = ::image::DynamicImage::from_decoder(decoder)?;
            image.apply_orientation(orientation);
            let mut pixels = image.into_rgba8();
            for pixel in pixels.as_chunks_mut::<4>().0 {
                pixel.swap(0, 2);
            }
            Ok(Arc::new(RenderImage::new(vec![::image::Frame::new(
                pixels,
            )])))
        }
        Err(_) => renderer.render_single_frame(&bytes, 1.).map_err(Into::into),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicUsize;

    #[test]
    fn source_identity_distinguishes_sources_and_preserves_clones() {
        let encoded = Arc::new(gpui::Image::from_bytes(
            gpui::ImageFormat::Png,
            vec![1, 2, 3],
        ));
        let source = ImageSource::Image(encoded);
        assert_eq!(source_key(&source), source_key(&source.clone()));
        assert_ne!(
            source_key(&ImageSource::from("one.png")),
            source_key(&ImageSource::from("two.png"))
        );
        let callback = ImageSource::from(|_: &mut Window, _: &mut App| None);
        assert_eq!(source_key(&callback), source_key(&callback.clone()));
        let another = ImageSource::from(|_: &mut Window, _: &mut App| None);
        assert_ne!(source_key(&callback), source_key(&another));
    }

    #[test]
    fn invalid_decoded_images_fail_instead_of_waiting_forever() {
        let image = Arc::new(RenderImage::new(Vec::<::image::Frame>::new()));
        assert!(matches!(classify(Some(Ok(image))), LoadResult::Failed(_)));
        assert!(matches!(classify(None), LoadResult::Pending));
    }

    #[gpui::test]
    fn completed_preparations_share_the_result_without_restarting(cx: &mut gpui::TestAppContext) {
        let calls = Arc::new(AtomicUsize::new(0));
        let counter = calls.clone();
        let preparation = cx.update(|cx| {
            Preparation::new(
                async move {
                    counter.fetch_add(1, Ordering::SeqCst);
                    vec![1u8, 2, 3]
                },
                cx,
            )
        });
        cx.run_until_parked();
        let first = preparation.get().unwrap();
        let second = preparation.get().unwrap();
        assert!(Arc::ptr_eq(&first, &second));
        assert_eq!(calls.load(Ordering::SeqCst), 1);
    }

    #[gpui::test]
    fn dropping_a_pending_preparation_cancels_its_worker(cx: &mut gpui::TestAppContext) {
        struct Guard(Arc<AtomicUsize>);
        impl Drop for Guard {
            fn drop(&mut self) {
                self.0.fetch_add(1, Ordering::SeqCst);
            }
        }
        let drops = Arc::new(AtomicUsize::new(0));
        let guard = Guard(drops.clone());
        let preparation = cx.update(|cx| {
            Preparation::new(
                async move {
                    let _guard = guard;
                    std::future::pending::<()>().await;
                },
                cx,
            )
        });
        cx.run_until_parked();
        assert!(preparation.get().is_none());
        drop(preparation);
        cx.run_until_parked();
        assert_eq!(drops.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn encoded_image_decode_reports_errors_and_preserves_bgra() {
        let renderer = gpui::SvgRenderer::new(Arc::new(()));
        let image = gpui::Image::from_bytes(
            gpui::ImageFormat::Pnm,
            b"P6\n1 1\n255\n\x10\x20\x30".to_vec(),
        );
        let decoded = decode_encoded(&image, renderer.clone()).unwrap();
        assert_eq!(decoded.as_bytes(0).unwrap(), [0x30, 0x20, 0x10, 255]);
        let corrupt = gpui::Image::from_bytes(gpui::ImageFormat::Png, vec![1, 2, 3]);
        assert!(decode_encoded(&corrupt, renderer).is_err());
    }

    #[test]
    fn data_sources_decode_base64_percent_encoded_raster_and_utf8_svg() {
        let renderer = gpui::SvgRenderer::new(Arc::new(()));
        for source in [
            "data:image/x-portable-anymap;base64,UDYKMSAxCjI1NQoQIDA=",
            "data:image/x-portable-anymap,P6%0A1%201%0A255%0A%10%20%30",
        ] {
            let image = decode_data_uri(source, renderer.clone()).unwrap();
            assert_eq!(image.as_bytes(0).unwrap(), [0x30, 0x20, 0x10, 255]);
        }
        let svg = "data:image/svg+xml,%3Csvg%20xmlns='http://www.w3.org/2000/svg'%20width='4'%20height='3'%3E%3Crect%20width='4'%20height='3'%20fill='red'/%3E%3C/svg%3E";
        let image = decode_data_uri(svg, renderer.clone()).unwrap();
        assert_eq!(
            image.size(0),
            gpui::size(gpui::DevicePixels(8), gpui::DevicePixels(6))
        );
        assert!(decode_data_uri("data:image/png;base64,!!invalid!!", renderer.clone()).is_err());
        assert!(decode_data_uri("data:image/png", renderer).is_err());
    }

    #[test]
    fn completed_decodes_account_for_bytes_without_reading_the_source_again() {
        struct Consumer {
            cache: Entity<DecodedImageCache>,
            shown: bool,
        }
        impl gpui::Render for Consumer {
            fn render(
                &mut self,
                window: &mut Window,
                cx: &mut gpui::Context<Self>,
            ) -> impl gpui::IntoElement {
                if self.shown {
                    self.cache.update(cx, |cache, cx| {
                        let _ = cache.get(
                            SourceKey::Encoded(123),
                            |cx| {
                                Preparation::new(
                                    async {
                                        Ok(Arc::new(RenderImage::new(vec![::image::Frame::new(
                                            ::image::RgbaImage::from_pixel(
                                                1,
                                                1,
                                                ::image::Rgba([1, 2, 3, 255]),
                                            ),
                                        )])))
                                    },
                                    cx,
                                )
                            },
                            window,
                            cx,
                        );
                    });
                }
                gpui::div()
            }
        }
        let mut cx = gpui::TestAppContext::single();
        let cache = cx.update(new_cache);
        let retained = cache.clone();
        let window = cx.open_window(gpui::size(gpui::px(100.), gpui::px(100.)), |_, _| {
            Consumer {
                cache: retained,
                shown: true,
            }
        });
        cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
            .unwrap();
        cx.run_until_parked();
        cache.read_with(&cx, |cache, _| {
            let record = cache.entries[&fingerprint(&SourceKey::Encoded(123))]
                .record
                .upgrade()
                .unwrap();
            assert_eq!(record.bytes(), 4);
            assert_eq!(cache.entries.len(), 1);
        });
        let weak = cache.read_with(&cx, |cache, _| {
            cache.entries[&fingerprint(&SourceKey::Encoded(123))]
                .record
                .clone()
        });
        window
            .update(&mut cx, |consumer, _, cx| {
                consumer.shown = false;
                cx.notify();
            })
            .unwrap();
        cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
            .unwrap();
        cx.run_until_parked();
        assert!(
            weak.upgrade().is_none(),
            "last scene releases decoded payload"
        );
        cache.read_with(&cx, |cache, _| assert!(cache.entries.is_empty()));
    }
    #[test]
    fn shared_pending_decodes_unregister_departed_scenes_and_cancel_after_the_last_user() {
        struct Guard(Arc<AtomicUsize>);
        impl Drop for Guard {
            fn drop(&mut self) {
                self.0.fetch_add(1, Ordering::SeqCst);
            }
        }
        struct Consumer {
            cache: Entity<DecodedImageCache>,
            shown: bool,
            drops: Arc<AtomicUsize>,
            starts: Arc<AtomicUsize>,
        }
        impl gpui::Render for Consumer {
            fn render(
                &mut self,
                window: &mut Window,
                cx: &mut gpui::Context<Self>,
            ) -> impl gpui::IntoElement {
                if self.shown {
                    let drops = self.drops.clone();
                    let starts = self.starts.clone();
                    self.cache.update(cx, |cache, cx| {
                        cache.get(
                            SourceKey::Encoded(456),
                            move |cx| {
                                starts.fetch_add(1, Ordering::SeqCst);
                                let guard = Guard(drops);
                                Preparation::new(
                                    async move {
                                        let _guard = guard;
                                        std::future::pending::<Decoded>().await
                                    },
                                    cx,
                                )
                            },
                            window,
                            cx,
                        )
                    });
                }
                gpui::div()
            }
        }
        let mut cx = gpui::TestAppContext::single();
        let cache = cx.update(new_cache);
        let starts = Arc::new(AtomicUsize::new(0));
        let drops = Arc::new(AtomicUsize::new(0));
        let windows: Vec<_> = (0..2)
            .map(|_| {
                cx.add_window(|_, _| Consumer {
                    cache: cache.clone(),
                    shown: true,
                    drops: drops.clone(),
                    starts: starts.clone(),
                })
            })
            .collect();
        let draw = |cx: &mut gpui::TestAppContext, index: usize| {
            cx.update_window(windows[index].into(), |_, window, cx| {
                window.draw(cx).clear(cx)
            })
            .unwrap();
            cx.run_until_parked();
        };
        draw(&mut cx, 0);
        draw(&mut cx, 1);
        let weak = cache.read_with(&cx, |cache, _| {
            cache.entries[&fingerprint(&SourceKey::Encoded(456))]
                .record
                .clone()
        });
        let waiters = || {
            let record = weak.upgrade().unwrap();
            let state = record.load.state.borrow();
            match &*state {
                PreparationState::Pending(views) => views.len(),
                PreparationState::Ready(_) => panic!("fixture remains pending"),
            }
        };
        assert_eq!(starts.load(Ordering::SeqCst), 1);
        assert_eq!(waiters(), 2);
        windows[0]
            .update(&mut cx, |consumer, _, cx| {
                consumer.shown = false;
                cx.notify();
            })
            .unwrap();
        draw(&mut cx, 0);
        assert_eq!(waiters(), 1);
        assert_eq!(
            drops.load(Ordering::SeqCst),
            0,
            "the other scene still owns the task"
        );
        windows[1]
            .update(&mut cx, |consumer, _, cx| {
                consumer.shown = false;
                cx.notify();
            })
            .unwrap();
        draw(&mut cx, 1);
        assert!(weak.upgrade().is_none());
        assert_eq!(drops.load(Ordering::SeqCst), 1);
        cache.read_with(&cx, |cache, _| assert!(cache.entries.is_empty()));
    }

    #[test]
    fn active_decodes_survive_lookup_eviction_without_restarting_or_changing_identity() {
        const SOURCES: usize = MAX_IDLE_ENTRIES + 1;
        struct Consumer {
            cache: Entity<DecodedImageCache>,
            shown: bool,
            starts: Rc<Cell<usize>>,
            probes: Vec<Option<Weak<Record>>>,
            identities: Vec<Option<usize>>,
        }
        impl gpui::Render for Consumer {
            fn render(
                &mut self,
                window: &mut Window,
                cx: &mut gpui::Context<Self>,
            ) -> impl gpui::IntoElement {
                if self.shown {
                    for index in 0..SOURCES {
                        window.with_id(("decode-pressure", index), |window| {
                            self.cache.update(cx, |cache, cx| {
                                let key = SourceKey::Encoded(index as u64);
                                let value = cache.get(
                                    key.clone(),
                                    |cx| {
                                        self.starts.set(self.starts.get() + 1);
                                        Preparation::new(
                                            async move {
                                                Ok(Arc::new(RenderImage::new(vec![
                                                    ::image::Frame::new(
                                                        ::image::RgbaImage::from_pixel(
                                                            1,
                                                            1,
                                                            ::image::Rgba([index as u8, 0, 0, 255]),
                                                        ),
                                                    ),
                                                ])))
                                            },
                                            cx,
                                        )
                                    },
                                    window,
                                    cx,
                                );
                                if self.probes[index].is_none() {
                                    self.probes[index] =
                                        Some(cache.entries[&fingerprint(&key)].record.clone());
                                }
                                self.identities[index] = value.map(|image| image.unwrap().id.0);
                            });
                        });
                    }
                }
                gpui::div()
            }
        }
        let mut cx = gpui::TestAppContext::single();
        let cache = cx.update(new_cache);
        let starts = Rc::new(Cell::new(0));
        let window = cx.add_window(|_, _| Consumer {
            cache: cache.clone(),
            shown: true,
            starts: starts.clone(),
            probes: vec![None; SOURCES],
            identities: vec![None; SOURCES],
        });
        let draw = |cx: &mut gpui::TestAppContext| {
            cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
                .unwrap();
            cx.run_until_parked();
        };
        draw(&mut cx);
        draw(&mut cx);
        let identities = window
            .read_with(&cx, |consumer, _| consumer.identities.clone())
            .unwrap();
        assert!(identities.iter().all(Option::is_some));
        for _ in 0..4 {
            window.update(&mut cx, |_, _, cx| cx.notify()).unwrap();
            draw(&mut cx);
            window
                .read_with(&cx, |consumer, _| {
                    assert_eq!(consumer.identities, identities);
                    assert!(
                        consumer
                            .probes
                            .iter()
                            .all(|probe| { probe.as_ref().unwrap().upgrade().is_some() })
                    );
                })
                .unwrap();
        }
        assert_eq!(starts.get(), SOURCES);
        cache.read_with(&cx, |cache, _| {
            assert!(cache.entries.len() <= MAX_IDLE_ENTRIES)
        });
        window
            .update(&mut cx, |consumer, _, cx| {
                consumer.shown = false;
                cx.notify();
            })
            .unwrap();
        draw(&mut cx);
        window
            .read_with(&cx, |consumer, _| {
                assert!(
                    consumer
                        .probes
                        .iter()
                        .all(|probe| { probe.as_ref().unwrap().upgrade().is_none() })
                );
            })
            .unwrap();
        cache.read_with(&cx, |cache, _| assert!(cache.entries.is_empty()));
    }
}
