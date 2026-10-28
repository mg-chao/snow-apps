// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Immutable typed icon references and shared, bounded SVG rendering.
//!
//! Built-in artwork is embedded; no asset-source installation is necessary. Custom host assets
//! are loaded through the application's existing [`AssetSource`]. Custom two-tone templates use
//! `currentColor` for primary and `__ADGPUI_SECONDARY__` for secondary. Full-color SVGs preserve
//! source colors. Icons are decorative by default; the containing control supplies its name.

mod generated;

use std::{
    collections::HashMap,
    fmt,
    sync::{Arc, Weak},
};

use gpui::{
    App, AssetSource, BorrowAppContext, Bounds, DevicePixels, Global, IntoElement, Pixels, Radians,
    Refineable, RenderImage, RenderOnce, Rgba, SharedString, Size, StyleRefinement, Styled,
    SvgRenderer, SvgSize, TransformationMatrix, Window, canvas, px, radians, rgba, size,
};

pub use generated::IconName;

/// The coloring contract declared by immutable SVG artwork.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum IconColorModel {
    Monochrome,
    TwoTone,
    FullColor,
}

/// A project-owned static icon. Its key must be namespaced outside `antd/`.
#[derive(Debug)]
pub struct IconDescriptor {
    pub key: &'static str,
    pub color_model: IconColorModel,
    pub svg: &'static [u8],
}

/// Color overrides carried by an icon reference. Full-color assets reject color overrides.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct IconColors {
    pub primary: Option<Rgba>,
    pub secondary: Option<Rgba>,
}

impl IconColors {
    pub fn primary(color: impl Into<Rgba>) -> Self {
        Self {
            primary: Some(color.into()),
            secondary: None,
        }
    }

    pub fn two_tone(primary: impl Into<Rgba>, secondary: impl Into<Rgba>) -> Self {
        Self {
            primary: Some(primary.into()),
            secondary: Some(secondary.into()),
        }
    }
}

#[derive(Clone, Debug)]
enum IconSource {
    Embedded {
        descriptor: &'static IconDescriptor,
        cache_path: SharedString,
    },
    Asset {
        path: SharedString,
        color_model: IconColorModel,
    },
}

/// A cheap immutable reference; cloning never copies SVG data or constructs a runtime icon pack.
#[derive(Clone, Debug)]
pub struct IconRef {
    source: IconSource,
    colors: IconColors,
}

impl IconRef {
    pub const fn builtin(name: IconName) -> Self {
        Self {
            source: IconSource::Embedded {
                descriptor: name.descriptor(),
                cache_path: SharedString::new_static(name.cache_path()),
            },
            colors: IconColors {
                primary: None,
                secondary: None,
            },
        }
    }

    pub fn custom(descriptor: &'static IconDescriptor) -> Result<Self, IconError> {
        if descriptor.key.is_empty() || descriptor.key.starts_with("antd/") {
            return Err(IconError::InvalidCustomKey);
        }
        Ok(Self {
            source: IconSource::Embedded {
                descriptor,
                cache_path: SharedString::from(format!(
                    "__adgpui_custom_{:x}",
                    descriptor as *const IconDescriptor as usize
                )),
            },
            colors: IconColors::default(),
        })
    }

    /// Reference an SVG supplied by the host's existing asset source. No filesystem or URL lookup
    /// is performed by this library. Clear the icon cache after replacing asset contents in place.
    pub fn asset(path: impl Into<SharedString>, color_model: IconColorModel) -> Self {
        Self {
            source: IconSource::Asset {
                path: path.into(),
                color_model,
            },
            colors: IconColors::default(),
        }
    }

    #[must_use]
    pub fn with_colors(mut self, mut colors: IconColors) -> Self {
        if self.color_model() == IconColorModel::TwoTone
            && colors.secondary.is_none()
            && let Some(primary) = colors.primary.and_then(|color| pack_color(color).ok())
        {
            colors.secondary = Some(
                crate::theme::generate_palette(crate::theme::Color::rgba(primary), false)[0]
                    .to_rgba(),
            );
        }
        self.colors = colors;
        self
    }

    pub fn key(&self) -> &str {
        match &self.source {
            IconSource::Embedded { descriptor, .. } => descriptor.key,
            IconSource::Asset { path, .. } => path,
        }
    }

    pub fn color_model(&self) -> IconColorModel {
        match &self.source {
            IconSource::Embedded { descriptor, .. } => descriptor.color_model,
            IconSource::Asset { color_model, .. } => *color_model,
        }
    }

    /// Ant's React icon frame crops catalog paths to 64 64 896 896.
    /// Host artwork keeps the view box declared by its own SVG.
    pub(crate) fn ant_design_viewbox_scale(&self) -> f32 {
        match &self.source {
            IconSource::Embedded { descriptor, .. } if descriptor.key.starts_with("antd/") => {
                8. / 7.
            }
            _ => 1.,
        }
    }
}

impl IconName {
    pub const fn icon(self) -> IconRef {
        IconRef::builtin(self)
    }
}

impl From<IconName> for IconRef {
    fn from(name: IconName) -> Self {
        Self::builtin(name)
    }
}

/// Explicit resolved colors, independent of the application's theme implementation.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct IconPalette {
    pub text: Rgba,
    pub primary: Rgba,
    pub secondary: Rgba,
}

impl Default for IconPalette {
    fn default() -> Self {
        Self {
            text: rgba(0x000000e0),
            primary: rgba(0x1677ffff),
            secondary: rgba(0xe6f4ffff),
        }
    }
}

impl IconPalette {
    /// Resolve icon colors once when the retained theme changes. Ant Design's two-tone secondary
    /// color is the lightest generated tint of its primary, including when the UI uses dark mode.
    pub fn from_theme(tokens: &crate::theme::AliasTokens) -> Self {
        Self {
            text: tokens.color_text.to_rgba(),
            primary: tokens.color_primary.to_rgba(),
            secondary: crate::theme::generate_palette(tokens.color_primary, false)[0].to_rgba(),
        }
    }
}

/// Logical size and effective display scale. Mirroring is opt-in, even in RTL layouts.
#[derive(Clone, Copy, Debug)]
pub struct IconRenderOptions {
    pub size: Size<Pixels>,
    pub scale_factor: f32,
    pub colors: IconPalette,
    pub rotation: Radians,
    pub mirror: bool,
}

impl Default for IconRenderOptions {
    fn default() -> Self {
        Self {
            size: size(px(16.), px(16.)),
            scale_factor: 1.,
            colors: IconPalette::default(),
            rotation: radians(0.),
            mirror: false,
        }
    }
}

/// Limits apply to cache-owned CPU data. Caller-held images and GPUI's GPU atlas are separate.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct IconCacheLimits {
    pub parsed_entries: usize,
    pub raster_entries: usize,
    pub raster_bytes: usize,
    pub max_raster_bytes: usize,
}

impl Default for IconCacheLimits {
    fn default() -> Self {
        Self {
            parsed_entries: 256,
            raster_entries: 512,
            raster_bytes: 16 * 1024 * 1024,
            max_raster_bytes: 1024 * 1024,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct IconCacheStatistics {
    pub parsed_entries: usize,
    pub raster_entries: usize,
    pub raster_bytes: usize,
    pub parse_hits: u64,
    pub parse_misses: u64,
    pub raster_hits: u64,
    pub raster_misses: u64,
    pub evictions: u64,
    /// Live raster resources, including images leased by rendered/cached views outside the LRU.
    pub resident_rasters: usize,
    /// Unique live raster bytes, counted once even when shared by both a view and the CPU cache.
    pub resident_bytes: usize,
}

/// Explicit rendering failures. Built-in descriptors are verified by the generator and tests.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum IconError {
    InvalidCustomKey,
    InvalidSize,
    InvalidColor,
    InvalidViewBox,
    InvalidSvg(String),
    AssetMissing(String),
    AssetLoad(String),
    FullColorOverrides,
}

impl fmt::Display for IconError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidCustomKey => {
                f.write_str("custom icon keys must be outside the antd namespace")
            }
            Self::InvalidSize => {
                f.write_str("icon size and scale must be finite, positive, and bounded")
            }
            Self::InvalidColor => f.write_str("icon colors must have finite channels in [0, 1]"),
            Self::InvalidViewBox => {
                f.write_str("transformed SVG must declare a valid positive viewBox")
            }
            Self::InvalidSvg(error) => write!(f, "invalid SVG: {error}"),
            Self::AssetMissing(path) => write!(f, "icon asset not found: {path}"),
            Self::AssetLoad(error) => write!(f, "icon asset load failed: {error}"),
            Self::FullColorOverrides => {
                f.write_str("full-color icons do not accept color overrides")
            }
        }
    }
}

impl std::error::Error for IconError {}

#[derive(Clone, Debug, Eq, Hash, PartialEq)]
enum SourceKey {
    Embedded(usize),
    Asset(SharedString, usize),
}

#[derive(Clone, Debug, Eq, Hash, PartialEq)]
struct ParsedKey {
    generation: u64,
    source: SourceKey,
    model: IconColorModel,
    primary: u32,
    secondary: u32,
    rotation: u32,
    mirror: bool,
}

#[derive(Clone, Debug, Eq, Hash, PartialEq)]
struct RasterKey {
    parsed: ParsedKey,
    width: i32,
    height: i32,
}

struct ParsedEntry {
    svg: Arc<gpui::ParsedSvg>,
    used: u64,
}

struct RasterEntry {
    resource: Arc<IconImageResource>,
    bytes: usize,
    used: u64,
}

struct RasterizedIcon {
    resource: Arc<IconImageResource>,
    retired: Vec<Arc<RenderImage>>,
}

struct IconImageResource {
    key: RasterKey,
    image: Arc<RenderImage>,
    bytes: usize,
}

// GPUI retains accessed element states when replaying a cached view's scene. An entity lease
// therefore has exactly the lifetime of the scene which can reference this image's atlas tile.
struct IconImageLease {
    resource: Option<Arc<IconImageResource>>,
}

fn lease_image(resource: &Arc<IconImageResource>, window: &mut Window, cx: &mut App) {
    let view = window.current_view();
    window.with_id(("__adgpui_icon_view", view), |window| {
        window.use_keyed_state(("__adgpui_icon_image", resource.image.id.0), cx, |_, cx| {
            cx.on_release(|lease: &mut IconImageLease, cx| {
                if let Some(resource) = lease.resource.take()
                    && Arc::strong_count(&resource) == 1
                {
                    let image = resource.image.clone();
                    let key = resource.key.clone();
                    cx.defer(move |cx| {
                        cx.drop_image(image, None);
                        if cx.has_global::<IconRenderer>() {
                            cx.update_global::<IconRenderer, _>(|renderer, _| {
                                if renderer
                                    .resident
                                    .get(&key)
                                    .is_some_and(|resource| resource.strong_count() == 0)
                                {
                                    renderer.resident.remove(&key);
                                }
                            });
                        }
                    });
                }
            })
            .detach();
            IconImageLease {
                resource: Some(resource.clone()),
            }
        });
    });
}

/// App-owned caches. Color substitution, SVG parsing, and rasterization occur only on misses.
pub struct IconRenderer {
    limits: IconCacheLimits,
    parsed: HashMap<ParsedKey, ParsedEntry>,
    rasters: HashMap<RasterKey, RasterEntry>,
    resident: HashMap<RasterKey, Weak<IconImageResource>>,
    tick: u64,
    generation: u64,
    statistics: IconCacheStatistics,
}

impl Global for IconRenderer {}

impl Default for IconRenderer {
    fn default() -> Self {
        Self::new(IconCacheLimits::default())
    }
}

impl IconRenderer {
    pub fn new(limits: IconCacheLimits) -> Self {
        Self {
            limits,
            parsed: HashMap::new(),
            rasters: HashMap::new(),
            resident: HashMap::new(),
            tick: 0,
            generation: 0,
            statistics: IconCacheStatistics::default(),
        }
    }

    pub fn limits(&self) -> IconCacheLimits {
        self.limits
    }

    pub fn statistics(&self) -> IconCacheStatistics {
        let (resident_rasters, resident_bytes) =
            self.resident
                .values()
                .fold((0, 0), |(count, bytes), resource| {
                    match resource.upgrade() {
                        Some(resource) => (count + 1, bytes + resource.bytes),
                        None => (count, bytes),
                    }
                });
        IconCacheStatistics {
            parsed_entries: self.parsed.len(),
            raster_entries: self.rasters.len(),
            resident_rasters,
            resident_bytes,
            ..self.statistics
        }
    }

    /// Update CPU limits immediately. Scene leases retain visible/cached images separately;
    /// only unleased retired images are returned for atlas disposal through `App::drop_image`.
    pub fn set_limits(&mut self, limits: IconCacheLimits) -> Vec<Arc<RenderImage>> {
        self.limits = limits;
        self.trim_parsed();
        let oversized: Vec<_> = self
            .rasters
            .iter()
            .filter(|(_, entry)| entry.bytes > limits.max_raster_bytes)
            .map(|(key, _)| key.clone())
            .collect();
        let mut retired = Vec::new();
        for key in oversized {
            let entry = self.rasters.remove(&key).expect("selected existing raster");
            self.statistics.raster_bytes -= entry.bytes;
            self.statistics.evictions += 1;
            if Arc::strong_count(&entry.resource) == 1 {
                retired.push(entry.resource.image.clone());
            }
        }
        retired.extend(self.trim(self.limits.raster_bytes));
        retired
    }

    /// Reclaim least-recently-used rasters down to `target_bytes`, respecting the entry limit.
    /// Visible/cached scene resources remain leased outside the LRU budget. Returned unleased
    /// images belong to the caller and no longer count toward cache statistics.
    pub fn trim(&mut self, target_bytes: usize) -> Vec<Arc<RenderImage>> {
        let target = target_bytes.min(self.limits.raster_bytes);
        let mut retired = Vec::new();
        while self.statistics.raster_bytes > target
            || self.rasters.len() > self.limits.raster_entries
        {
            let Some(key) = self
                .rasters
                .iter()
                .min_by_key(|(_, entry)| entry.used)
                .map(|(key, _)| key.clone())
            else {
                break;
            };
            let entry = self.rasters.remove(&key).expect("selected existing raster");
            self.statistics.raster_bytes -= entry.bytes;
            self.statistics.evictions += 1;
            if Arc::strong_count(&entry.resource) == 1 {
                retired.push(entry.resource.image.clone());
            }
        }
        self.resident
            .retain(|_, resource| resource.strong_count() > 0);
        retired
    }

    pub fn clear(&mut self) -> Vec<Arc<RenderImage>> {
        self.parsed.clear();
        self.generation = self.generation.wrapping_add(1);
        self.trim(0)
    }

    /// Headless rendering, also useful for exports. The caller owns any GPU resources it creates
    /// from the returned image; `paint` handles atlas disposal for ordinary controls.
    pub fn rasterize(
        &mut self,
        renderer: &SvgRenderer,
        assets: &dyn AssetSource,
        icon: &IconRef,
        options: &IconRenderOptions,
    ) -> Result<Arc<RenderImage>, IconError> {
        self.rasterize_inner(renderer, assets, icon, options)
            .map(|raster| raster.resource.image.clone())
    }

    /// Paint from an element's paint callback. Monochrome SVGs borrow static bytes and use GPUI's
    /// atlas directly; rotation and mirroring do not reparse or rasterize on each frame.
    pub fn paint(
        &mut self,
        bounds: Bounds<Pixels>,
        icon: &IconRef,
        options: &IconRenderOptions,
        window: &mut Window,
        cx: &mut App,
    ) -> Result<(), IconError> {
        self.paint_with_scale(bounds, icon, options, 1., window, cx)
    }

    /// Scale artwork around its center without changing its raster/atlas identity.
    /// Used by controls whose icon slot animates independently of the artwork.
    pub(crate) fn paint_with_scale(
        &mut self,
        bounds: Bounds<Pixels>,
        icon: &IconRef,
        options: &IconRenderOptions,
        scale: f32,
        window: &mut Window,
        cx: &mut App,
    ) -> Result<(), IconError> {
        if !scale.is_finite() || scale <= 0. {
            return Ok(());
        }
        let mut options = *options;
        options.size = bounds.size;
        options.scale_factor = window.scale_factor();
        validate_size(&options)?;
        if icon.color_model() == IconColorModel::Monochrome {
            let colors = resolve_colors(icon, &options)?;
            let center = bounds.center().scale(window.scale_factor());
            let transform = TransformationMatrix::unit()
                .translate(center)
                .rotate(options.rotation)
                .scale(size(if options.mirror { -scale } else { scale }, scale))
                .translate(center * -1.);
            let (path, data) = match &icon.source {
                IconSource::Embedded {
                    descriptor,
                    cache_path,
                } => (cache_path.clone(), Some(descriptor.svg)),
                IconSource::Asset { path, .. } => (path.clone(), None),
            };
            return window
                .paint_svg(bounds, path, data, transform, rgba(colors.0).into(), cx)
                .map_err(|error| IconError::InvalidSvg(error.to_string()));
        }
        let renderer = cx.svg_renderer();
        let assets = cx.asset_source().clone();
        let raster = self.rasterize_inner(&renderer, &*assets, icon, &options)?;
        lease_image(&raster.resource, window, cx);
        let painted = Bounds::centered_at(
            bounds.center(),
            bounds.size.map(|dimension| dimension * scale),
        );
        let result = window
            .paint_image(
                painted,
                painted,
                gpui::Corners::default(),
                raster.resource.image.clone(),
                0,
                false,
            )
            .map_err(|error| IconError::InvalidSvg(error.to_string()));
        if !raster.retired.is_empty() {
            // Active/cached scene leases protect their images from disposal. Retired resources
            // without a scene lease can be reclaimed once windows are back in the app.
            cx.defer(move |cx| {
                for image in raster.retired {
                    cx.drop_image(image, None);
                }
            });
        }
        result
    }

    fn rasterize_inner(
        &mut self,
        renderer: &SvgRenderer,
        assets: &dyn AssetSource,
        icon: &IconRef,
        options: &IconRenderOptions,
    ) -> Result<RasterizedIcon, IconError> {
        let dimensions = validate_size(options)?;
        let colors = resolve_colors(icon, options)?;
        self.tick = self.tick.wrapping_add(1);
        let source = match &icon.source {
            IconSource::Embedded { descriptor, .. } => {
                SourceKey::Embedded(*descriptor as *const IconDescriptor as usize)
            }
            IconSource::Asset { path, .. } => SourceKey::Asset(
                path.clone(),
                assets as *const dyn AssetSource as *const () as usize,
            ),
        };
        let parsed_key = ParsedKey {
            generation: self.generation,
            source,
            model: icon.color_model(),
            primary: colors.0,
            secondary: colors.1,
            rotation: if options.rotation.0 == 0. {
                0
            } else {
                options.rotation.0.to_bits()
            },
            mirror: options.mirror,
        };
        let key = RasterKey {
            parsed: parsed_key.clone(),
            width: dimensions.width.0,
            height: dimensions.height.0,
        };
        if let Some(entry) = self.rasters.get_mut(&key) {
            entry.used = self.tick;
            self.statistics.raster_hits += 1;
            return Ok(RasterizedIcon {
                resource: entry.resource.clone(),
                retired: Vec::new(),
            });
        }
        if let Some(resource) = self.resident.get(&key).and_then(Weak::upgrade) {
            // The CPU LRU can be smaller than the visible working set. Reuse view-owned images
            // without promoting them into the LRU and evicting another visible icon each frame.
            self.statistics.raster_hits += 1;
            return Ok(RasterizedIcon {
                resource,
                retired: Vec::new(),
            });
        }
        self.statistics.raster_misses += 1;
        let parsed = if let Some(entry) = self.parsed.get_mut(&parsed_key) {
            entry.used = self.tick;
            self.statistics.parse_hits += 1;
            entry.svg.clone()
        } else {
            self.statistics.parse_misses += 1;
            let bytes = match &icon.source {
                IconSource::Embedded { descriptor, .. } => {
                    std::borrow::Cow::Borrowed(descriptor.svg)
                }
                IconSource::Asset { path, .. } => assets
                    .load(path)
                    .map_err(|error| IconError::AssetLoad(error.to_string()))?
                    .ok_or_else(|| IconError::AssetMissing(path.to_string()))?,
            };
            let prepared = prepare_svg(&bytes, icon.color_model(), colors, options)?;
            let parsed = Arc::new(
                renderer
                    .parse_svg(prepared.as_bytes())
                    .map_err(|error| IconError::InvalidSvg(error.to_string()))?,
            );
            if self.limits.parsed_entries > 0 {
                self.parsed.insert(
                    parsed_key,
                    ParsedEntry {
                        svg: parsed.clone(),
                        used: self.tick,
                    },
                );
                self.trim_parsed();
            }
            parsed
        };
        let image = renderer
            .render_parsed(&parsed, SvgSize::ExactSize(dimensions))
            .map_err(|error| IconError::InvalidSvg(error.to_string()))?;
        let bytes = image.as_bytes(0).map_or(0, <[u8]>::len);
        let resource = Arc::new(IconImageResource {
            key: key.clone(),
            image,
            bytes,
        });
        self.resident
            .retain(|_, resource| resource.strong_count() > 0);
        self.resident.insert(key.clone(), Arc::downgrade(&resource));
        if bytes > self.limits.max_raster_bytes
            || bytes > self.limits.raster_bytes
            || self.limits.raster_entries == 0
        {
            return Ok(RasterizedIcon {
                resource,
                retired: Vec::new(),
            });
        }
        self.statistics.raster_bytes += bytes;
        self.rasters.insert(
            key,
            RasterEntry {
                resource: resource.clone(),
                bytes,
                used: self.tick,
            },
        );
        let retired = self.trim(self.limits.raster_bytes);
        Ok(RasterizedIcon { resource, retired })
    }

    fn trim_parsed(&mut self) {
        while self.parsed.len() > self.limits.parsed_entries {
            let Some(key) = self
                .parsed
                .iter()
                .min_by_key(|(_, entry)| entry.used)
                .map(|(key, _)| key.clone())
            else {
                break;
            };
            self.parsed.remove(&key);
        }
    }
}

fn validate_size(options: &IconRenderOptions) -> Result<Size<DevicePixels>, IconError> {
    let width = f32::from(options.size.width) * options.scale_factor;
    let height = f32::from(options.size.height) * options.scale_factor;
    if !options.scale_factor.is_finite()
        || options.scale_factor <= 0.
        || !width.is_finite()
        || !height.is_finite()
        || width <= 0.
        || height <= 0.
        || width > 4096.
        || height > 4096.
        || !options.rotation.0.is_finite()
    {
        return Err(IconError::InvalidSize);
    }
    Ok(size(
        DevicePixels(width.ceil() as i32),
        DevicePixels(height.ceil() as i32),
    ))
}

fn pack_color(color: Rgba) -> Result<u32, IconError> {
    let channels = [color.r, color.g, color.b, color.a];
    if channels
        .iter()
        .any(|channel| !channel.is_finite() || !(0. ..=1.).contains(channel))
    {
        return Err(IconError::InvalidColor);
    }
    let bytes = channels.map(|channel| (channel * 255.).round() as u8);
    Ok(u32::from_be_bytes(bytes))
}

fn resolve_colors(icon: &IconRef, options: &IconRenderOptions) -> Result<(u32, u32), IconError> {
    if icon.color_model() == IconColorModel::FullColor {
        if icon.colors != IconColors::default() {
            return Err(IconError::FullColorOverrides);
        }
        return Ok((0, 0));
    }
    let primary = icon.colors.primary.unwrap_or(match icon.color_model() {
        IconColorModel::Monochrome => options.colors.text,
        _ => options.colors.primary,
    });
    let secondary = if icon.color_model() == IconColorModel::TwoTone {
        pack_color(icon.colors.secondary.unwrap_or(options.colors.secondary))?
    } else {
        0
    };
    Ok((pack_color(primary)?, secondary))
}

fn prepare_svg(
    bytes: &[u8],
    model: IconColorModel,
    colors: (u32, u32),
    options: &IconRenderOptions,
) -> Result<String, IconError> {
    let source =
        std::str::from_utf8(bytes).map_err(|error| IconError::InvalidSvg(error.to_string()))?;
    let mut prepared = if model == IconColorModel::FullColor {
        source.to_owned()
    } else {
        source
            .replace("currentColor", &format!("#{:08x}", colors.0))
            .replace("__ADGPUI_SECONDARY__", &format!("#{:08x}", colors.1))
    };
    if options.mirror || options.rotation.0 != 0. {
        let start = prepared.find("<svg").ok_or(IconError::InvalidViewBox)?;
        let open = prepared[start..]
            .find('>')
            .map(|offset| start + offset + 1)
            .ok_or(IconError::InvalidViewBox)?;
        let close = prepared.rfind("</svg>").ok_or(IconError::InvalidViewBox)?;
        let root = &prepared[start..open];
        let attribute = root.find("viewBox").ok_or(IconError::InvalidViewBox)? + 7;
        let value = root[attribute..].trim_start();
        let value = value
            .strip_prefix('=')
            .ok_or(IconError::InvalidViewBox)?
            .trim_start();
        let quote = value.chars().next().ok_or(IconError::InvalidViewBox)?;
        if quote != '\'' && quote != '"' {
            return Err(IconError::InvalidViewBox);
        }
        let end = value[1..].find(quote).ok_or(IconError::InvalidViewBox)? + 1;
        let values = value[1..end]
            .split(|character: char| character.is_whitespace() || character == ',')
            .filter(|value| !value.is_empty())
            .map(str::parse::<f32>)
            .collect::<Result<Vec<_>, _>>()
            .map_err(|_| IconError::InvalidViewBox)?;
        if values.len() != 4
            || values.iter().any(|value| !value.is_finite())
            || values[2] <= 0.
            || values[3] <= 0.
        {
            return Err(IconError::InvalidViewBox);
        }
        let center = (values[0] + values[2] / 2., values[1] + values[3] / 2.);
        let transform = format!(
            "<g transform=\"translate({} {}) rotate({}) scale({} 1) translate({} {})\">",
            center.0,
            center.1,
            options.rotation.0.to_degrees(),
            if options.mirror { -1 } else { 1 },
            -center.0,
            -center.1,
        );
        prepared.insert_str(close, "</g>");
        prepared.insert_str(open, &transform);
    }
    Ok(prepared)
}

/// A decorative primitive with no focus stop, hitbox, or accessibility node of its own.
#[derive(IntoElement)]
pub struct Icon {
    reference: IconRef,
    options: IconRenderOptions,
    style: StyleRefinement,
    inherit_color: bool,
    frame_scale: f32,
}

pub fn icon(reference: impl Into<IconRef>) -> Icon {
    Icon {
        reference: reference.into(),
        options: IconRenderOptions::default(),
        style: StyleRefinement::default(),
        inherit_color: false,
        frame_scale: 1.,
    }
}

impl Icon {
    /// Match Ant's cropped catalog frame without resizing the layout slot.
    /// Host-provided artwork keeps its own view box.
    pub(crate) fn ant_design_frame(mut self) -> Self {
        self.frame_scale = self.reference.ant_design_viewbox_scale();
        self
    }
    /// Resolve `currentColor` from the native text style at paint time. This
    /// follows ancestor hover and semantic-slot colors without rebuilding SVGs.
    #[must_use]
    pub fn inherit_color(mut self) -> Self {
        self.inherit_color = true;
        self
    }
    #[must_use]
    pub fn render_options(mut self, options: IconRenderOptions) -> Self {
        self.options = options;
        self
    }

    #[must_use]
    pub fn palette(mut self, colors: IconPalette) -> Self {
        self.options.colors = colors;
        self
    }

    /// Static rotation, or a value from the shared motion clock. Loading spinners should use the
    /// monochrome `LoadingOutlined`; transformed full-color SVGs are cached per transform.
    #[must_use]
    pub fn rotation(mut self, rotation: Radians) -> Self {
        self.options.rotation = rotation;
        self
    }

    #[must_use]
    pub fn mirror(mut self, mirror: bool) -> Self {
        self.options.mirror = mirror;
        self
    }
}

impl Styled for Icon {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

impl RenderOnce for Icon {
    fn render(self, _window: &mut Window, _cx: &mut App) -> impl IntoElement {
        let mut element = canvas(
            |_, _, _| (),
            move |bounds, (), window, cx| {
                let mut options = self.options;
                if self.inherit_color {
                    options.colors.text = window.text_style().color.to_rgb();
                }
                if cx.try_global::<IconRenderer>().is_none() {
                    cx.set_global(IconRenderer::default());
                }
                cx.update_global::<IconRenderer, _>(|renderer, cx| {
                    let _ = renderer.paint_with_scale(
                        bounds,
                        &self.reference,
                        &options,
                        self.frame_scale,
                        window,
                        cx,
                    );
                });
            },
        )
        .w(self.options.size.width)
        .h(self.options.size.height)
        .flex_shrink_0();
        element.style().refine(&self.style);
        element
    }
}

/// Reclaim the default app renderer's rasters and matching GPUI atlas entries.
pub fn trim_icon_cache(target_bytes: usize, window: &mut Window, cx: &mut App) {
    if cx.try_global::<IconRenderer>().is_some() {
        cx.update_global::<IconRenderer, _>(|renderer, cx| {
            for image in renderer.trim(target_bytes) {
                cx.drop_image(image, Some(window));
            }
        });
    }
}

/// Clear parsed documents and rasters after replacing host assets or under memory pressure.
pub fn clear_icon_cache(window: &mut Window, cx: &mut App) {
    if cx.try_global::<IconRenderer>().is_some() {
        cx.update_global::<IconRenderer, _>(|renderer, cx| {
            for image in renderer.clear() {
                cx.drop_image(image, Some(window));
            }
        });
        // A cached view must render new content after replacing an asset at an existing path.
        // Scene leases keep its previous image valid until that replacement frame is complete.
        window.refresh();
        cx.refresh_windows();
    }
}

#[cfg(test)]
mod tests;
