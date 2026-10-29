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
    cell::{Cell, RefCell},
    collections::{HashMap, HashSet},
    future::Future,
    rc::{Rc, Weak},
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
};

const MAX_IDLE_ENTRIES: usize = 128;
const MAX_IDLE_BYTES: usize = 64 * 1024 * 1024;

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
    Pending(HashSet<EntityId>),
    Ready(Arc<T>),
}

/// A cancellable background preparation. Results stay attached to the original
/// cache key, and completion only invalidates views which requested that key.
pub(crate) struct Preparation<T> {
    state: Rc<RefCell<PreparationState<T>>>,
    completion: Rc<RefCell<Option<Completion>>>,
    _task: Task<()>,
}
type Completion = Box<dyn FnOnce(&mut App)>;

impl<T: Send + Sync + 'static> Preparation<T> {
    pub(crate) fn new(future: impl Future<Output = T> + Send + 'static, cx: &App) -> Self {
        let state = Rc::new(RefCell::new(PreparationState::Pending(HashSet::new())));
        let weak = Rc::downgrade(&state);
        let completion = Rc::new(RefCell::new(None::<Completion>));
        let completed = completion.clone();
        let task = cx.background_executor().spawn(future);
        let task = cx.spawn(async move |cx| {
            let result = Arc::new(task.await);
            let Some(state) = weak.upgrade() else { return };
            let old = state.replace(PreparationState::Ready(result));
            if let PreparationState::Pending(views) = old {
                cx.update(|cx| {
                    for view in views {
                        cx.notify(view);
                    }
                    if let Some(completion) = completed.borrow_mut().take() {
                        completion(cx);
                    }
                });
            }
        });
        Self {
            state,
            completion,
            _task: task,
        }
    }

    fn on_complete(&self, callback: impl FnOnce(&mut App) + 'static) {
        *self.completion.borrow_mut() = Some(Box::new(callback));
    }

    pub(crate) fn get(&self) -> Option<Arc<T>> {
        match &*self.state.borrow() {
            PreparationState::Pending(_) => None,
            PreparationState::Ready(value) => Some(value.clone()),
        }
    }

    pub(crate) fn use_value(&self, window: &Window) -> Option<Arc<T>> {
        match &mut *self.state.borrow_mut() {
            PreparationState::Pending(views) => {
                views.insert(window.current_view());
                None
            }
            PreparationState::Ready(value) => Some(value.clone()),
        }
    }
}

type Decoded = Result<Arc<RenderImage>, ImageCacheError>;
struct Record {
    id: u64,
    load: Preparation<Decoded>,
    bytes: Cell<Option<usize>>,
}
impl Record {
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
    fn image(&self) -> Option<Arc<RenderImage>> {
        self.load
            .get()
            .and_then(|value| value.as_ref().as_ref().ok().cloned())
    }
}
struct Entry {
    record: Rc<Record>,
    used: u64,
    bytes: usize,
}

/// Decoded images retained by active views are leased; only idle retention is
/// limited. This prevents a warm image from restarting its load under pressure.
#[derive(Default)]
pub(super) struct DecodedImageCache {
    owner: Option<WeakEntity<Self>>,
    entries: HashMap<SourceKey, Entry>,
    resident: HashMap<SourceKey, Weak<Record>>,
    tick: u64,
    bytes: usize,
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
    let entity = cx.new(|cx| DecodedImageCache {
        owner: Some(cx.weak_entity()),
        ..Default::default()
    });
    cx.observe_release(&entity, |cache, cx| {
        for (_, entry) in cache.entries.drain() {
            retire(&entry.record, cx);
        }
    })
    .detach();
    entity
}

fn retire(record: &Rc<Record>, cx: &mut App) {
    if Rc::strong_count(record) == 1
        && let Some(image) = record.image()
    {
        cx.defer(move |cx| cx.drop_image(image, None));
    }
}

struct Lease(Rc<Record>);
fn lease(record: &Rc<Record>, window: &mut Window, cx: &mut App) {
    window.with_id(
        ("ant-image-decoded-view", window.current_view()),
        |window| {
            window.use_keyed_state(("ant-image-decoded", record.id), cx, |_, cx| {
                cx.on_release(|lease: &mut Lease, cx| retire(&lease.0, cx))
                    .detach();
                Lease(record.clone())
            });
        },
    );
}

impl DecodedImageCache {
    fn get(
        &mut self,
        key: SourceKey,
        create: impl FnOnce(&mut App) -> Preparation<Decoded>,
        window: &mut Window,
        cx: &mut App,
    ) -> Option<Decoded> {
        self.tick = self.tick.wrapping_add(1);
        let record = if let Some(entry) = self.entries.get_mut(&key) {
            entry.used = self.tick;
            entry.record.clone()
        } else {
            static NEXT: AtomicU64 = AtomicU64::new(1);
            let record = self
                .resident
                .get(&key)
                .and_then(Weak::upgrade)
                .unwrap_or_else(|| {
                    let load = create(cx);
                    if let Some(owner) = self.owner.clone() {
                        load.on_complete(move |cx| {
                            // Retiring the just-completed entry may drop its
                            // task; do so after that task has returned.
                            cx.defer(move |cx| {
                                let _ = owner.update(cx, |cache, cx| {
                                    cache.bytes = 0;
                                    for entry in cache.entries.values_mut() {
                                        entry.bytes = entry.record.bytes();
                                        cache.bytes = cache.bytes.saturating_add(entry.bytes);
                                    }
                                    cache.trim(cx);
                                });
                            });
                        });
                    }
                    Rc::new(Record {
                        id: NEXT.fetch_add(1, Ordering::Relaxed),
                        load,
                        bytes: Cell::new(None),
                    })
                });
            self.resident.retain(|_, value| value.strong_count() > 0);
            self.resident.insert(key.clone(), Rc::downgrade(&record));
            self.entries.insert(
                key.clone(),
                Entry {
                    record: record.clone(),
                    used: self.tick,
                    bytes: 0,
                },
            );
            record
        };
        lease(&record, window, cx);
        let result = record
            .load
            .use_value(window)
            .map(|value| value.as_ref().clone());
        if let Some(entry) = self.entries.get_mut(&key) {
            let bytes = record.bytes();
            self.bytes = self.bytes.saturating_add(bytes.saturating_sub(entry.bytes));
            entry.bytes = bytes;
        }
        self.trim(cx);
        result
    }

    fn trim(&mut self, cx: &mut App) {
        while self.entries.len() > MAX_IDLE_ENTRIES || self.bytes > MAX_IDLE_BYTES {
            let Some(key) = self
                .entries
                .iter()
                .min_by_key(|(_, entry)| entry.used)
                .map(|(key, _)| key.clone())
            else {
                break;
            };
            if let Some(entry) = self.entries.remove(&key) {
                self.bytes = self.bytes.saturating_sub(entry.bytes);
                retire(&entry.record, cx);
            }
        }
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
            started: bool,
        }
        impl gpui::Render for Consumer {
            fn render(
                &mut self,
                window: &mut Window,
                cx: &mut gpui::Context<Self>,
            ) -> impl gpui::IntoElement {
                if !self.started {
                    self.started = true;
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
                started: false,
            }
        });
        cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
            .unwrap();
        cx.run_until_parked();
        cache.read_with(&cx, |cache, _| {
            assert_eq!(cache.bytes, 4);
            assert_eq!(cache.entries[&SourceKey::Encoded(123)].bytes, 4);
        });
    }
}
