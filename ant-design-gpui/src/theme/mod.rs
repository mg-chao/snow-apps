// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native Ant Design 6.6.7 tokens, ordered algorithms and retained theme configuration.
//!
//! Lengths use logical pixels, colors use sRGB, font stacks contain ordered
//! native family names, and motion durations use `std::time::Duration`.
//! This module has no platform backend or Node.js runtime requirement.

mod algorithm;
mod component;
mod palette;
mod tokens;
mod values;

pub use algorithm::{AlgorithmChain, BuiltinAlgorithm, MappingAlgorithm};
pub use component::{ComponentTheme, ComponentThemeConfig, ComponentThemes};
pub use palette::{
    Palette, PalettePatch, PresetColor, PresetPalettes, PresetPalettesPatch, generate_palette,
};
pub use tokens::{
    AliasTokens, GlobalTokenPatch, MapTokens, SeedTokenPatch, SeedTokens, SeedTokensPatch,
    TokenPatch,
};
pub use values::{
    Color, CubicBezier, FontStack, LineType, ParseColorError, Shadow, ShadowList, TextDecoration,
};

use std::{
    any::{Any, TypeId},
    collections::HashMap,
    fmt,
    sync::{Arc, Mutex},
};

/// A sparse theme configuration. Merge with the parent before resolving it.
#[derive(Clone, Debug, PartialEq)]
pub struct ThemeConfig {
    pub inherit: bool,
    pub seed: SeedTokenPatch,
    pub tokens: TokenPatch,
    pub algorithms: Option<AlgorithmChain>,
    pub components: ComponentThemes,
}
impl Default for ThemeConfig {
    fn default() -> Self {
        Self {
            inherit: true,
            seed: Default::default(),
            tokens: Default::default(),
            algorithms: None,
            components: Default::default(),
        }
    }
}
impl ThemeConfig {
    pub fn merged(parent: &Self, local: &Self) -> Self {
        let defaults;
        let parent = if local.inherit {
            parent
        } else {
            defaults = Self::default();
            &defaults
        };
        Self {
            inherit: true,
            seed: SeedTokenPatch::merged(
                &SeedTokenPatch::merged(&parent.seed, &parent.tokens.seed_patch()),
                &SeedTokenPatch::merged(&local.seed, &local.tokens.seed_patch()),
            ),
            tokens: TokenPatch::merged(&parent.tokens.without_seed(), &local.tokens.without_seed()),
            algorithms: local
                .algorithms
                .clone()
                .or_else(|| parent.algorithms.clone()),
            components: ComponentThemes::merged(&parent.components, &local.components),
        }
    }

    pub fn validate(&self) -> Result<(), ThemeError> {
        self.seed.validate()?;
        self.tokens.validate()?;
        self.components.validate()?;
        let mut seed = SeedTokens::default();
        self.seed.apply(&mut seed);
        self.tokens.apply_seed(&mut seed);
        validate_seed(&seed)
    }

    /// Resolve once after configuration changes, then share the immutable snapshot.
    ///
    /// # Panics
    /// Panics for invalid token values. Use `try_resolve` for untrusted configuration.
    pub fn resolve(&self) -> ResolvedTheme {
        self.try_resolve().expect("valid theme configuration")
    }

    pub fn try_resolve(&self) -> Result<ResolvedTheme, ThemeError> {
        self.validate()?;
        let mut seed = SeedTokens::default();
        self.seed.apply(&mut seed);
        self.tokens.apply_seed(&mut seed);
        let algorithms = self.algorithms.clone().unwrap_or_default();
        let mut map = algorithms.derive(&seed);
        self.tokens.apply_map(&mut map);
        map.validate()?;
        let mut tokens = algorithm::derive_alias(&map);
        self.tokens.apply_non_seed(&mut tokens);
        tokens.validate()?;
        validate_typography(&tokens)?;
        Ok(ResolvedTheme {
            seed: Arc::new(seed),
            map: Arc::new(map),
            tokens: Arc::new(tokens),
            components: self.components.clone(),
            component_cache: Arc::new(Mutex::new(HashMap::new())),
        })
    }
}

struct CachedComponent {
    source: Arc<AliasTokens>,
    tokens: Arc<dyn Any + Send + Sync>,
}
type ComponentCache = Mutex<HashMap<TypeId, CachedComponent>>;

/// Resolved immutable theme data. The component cache is bounded by used component types.
#[derive(Clone)]
pub struct ResolvedTheme {
    pub seed: Arc<SeedTokens>,
    pub map: Arc<MapTokens>,
    pub tokens: Arc<AliasTokens>,
    components: ComponentThemes,
    component_cache: Arc<ComponentCache>,
}
impl PartialEq for ResolvedTheme {
    fn eq(&self, other: &Self) -> bool {
        self.seed == other.seed
            && self.map == other.map
            && self.tokens == other.tokens
            && self.components == other.components
    }
}
impl fmt::Debug for ResolvedTheme {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ResolvedTheme")
            .field("seed", &self.seed)
            .field("map", &self.map)
            .field("tokens", &self.tokens)
            .field("components", &self.components)
            .finish_non_exhaustive()
    }
}
impl Default for ResolvedTheme {
    fn default() -> Self {
        ThemeConfig::default().resolve()
    }
}
impl ResolvedTheme {
    pub fn component<C: ComponentTheme>(&self) -> Arc<C::Tokens> {
        self.try_component::<C>()
            .expect("valid component theme configuration")
    }
    pub fn try_component<C: ComponentTheme>(&self) -> Result<Arc<C::Tokens>, ThemeError> {
        let key = TypeId::of::<C>();
        let cached = {
            let cache = self.component_cache.lock().expect("component cache lock");
            cache
                .get(&key)
                .filter(|cached| Arc::ptr_eq(&cached.source, &self.tokens))
                .map(|cached| cached.tokens.clone())
        };
        if let Some(cached) = cached {
            return Ok(cached.downcast().expect("component cache type matches key"));
        }
        let mut global = self.tokens.clone();
        let config = self.components.get::<C>();
        if let Some(config) = config {
            if let Some(algorithms) = &config.algorithms {
                let mut seed = self.tokens.as_seed();
                config.tokens.apply_seed(&mut seed);
                validate_seed(&seed)?;
                let mut map = algorithms.derive(&seed);
                config.tokens.apply_map(&mut map);
                map.validate()?;
                let mut aliases = algorithm::derive_alias(&map);
                config.tokens.apply_non_seed(&mut aliases);
                aliases.validate()?;
                validate_typography(&aliases)?;
                global = Arc::new(aliases);
            } else if !config.tokens.is_empty() {
                let aliases = Arc::make_mut(&mut global);
                config.tokens.apply_all(aliases);
                aliases.validate()?;
                validate_typography(aliases)?;
            }
        }
        let mut tokens = C::default_tokens(&global);
        if let Some(config) = config {
            C::apply(&mut tokens, &config.patch);
        }
        let tokens = Arc::new(tokens);
        // Custom token computation runs outside the lock, allowing nested component resolution.
        let mut cache = self.component_cache.lock().expect("component cache lock");
        let value = cache.entry(key).or_insert_with(|| CachedComponent {
            source: self.tokens.clone(),
            tokens: tokens.clone(),
        });
        if !Arc::ptr_eq(&value.source, &self.tokens) {
            *value = CachedComponent {
                source: self.tokens.clone(),
                tokens: tokens.clone(),
            };
        }
        Ok(value
            .tokens
            .clone()
            .downcast()
            .expect("component cache type matches key"))
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ThemeError {
    pub token: &'static str,
    pub reason: &'static str,
}
impl ThemeError {
    pub(crate) fn invalid(token: &'static str, reason: &'static str) -> Self {
        Self { token, reason }
    }
}
impl fmt::Display for ThemeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "invalid theme token {}: {}", self.token, self.reason)
    }
}
impl std::error::Error for ThemeError {}

fn validate_seed(seed: &SeedTokens) -> Result<(), ThemeError> {
    seed.validate()?;
    if seed.font_size < 4.0 {
        return Err(ThemeError::invalid(
            "fontSize",
            "must be at least 4 logical pixels",
        ));
    }
    for (name, value) in [
        ("lineWidth", seed.line_width),
        ("borderRadius", seed.border_radius),
        ("sizeUnit", seed.size_unit),
        ("sizeStep", seed.size_step),
        ("sizePopupArrow", seed.size_popup_arrow),
        ("controlHeight", seed.control_height),
        ("motionUnit", seed.motion_unit),
        ("motionBase", seed.motion_base),
    ] {
        if value < 0.0 {
            return Err(ThemeError::invalid(name, "must be nonnegative"));
        }
    }
    if !(0.0..=1.0).contains(&seed.opacity_image) {
        return Err(ThemeError::invalid(
            "opacityImage",
            "must be between 0 and 1",
        ));
    }
    if std::time::Duration::try_from_secs_f64(seed.motion_base + seed.motion_unit * 3.0).is_err() {
        return Err(ThemeError::invalid(
            "motionUnit",
            "derived duration is out of range",
        ));
    }
    Ok(())
}

fn validate_typography(tokens: &AliasTokens) -> Result<(), ThemeError> {
    for (name, size, height) in [
        ("lineHeight", tokens.font_size, tokens.line_height),
        ("lineHeightSM", tokens.font_size_sm, tokens.line_height_sm),
        ("lineHeightLG", tokens.font_size_lg, tokens.line_height_lg),
        (
            "lineHeightHeading1",
            tokens.font_size_heading1,
            tokens.line_height_heading1,
        ),
        (
            "lineHeightHeading2",
            tokens.font_size_heading2,
            tokens.line_height_heading2,
        ),
        (
            "lineHeightHeading3",
            tokens.font_size_heading3,
            tokens.line_height_heading3,
        ),
        (
            "lineHeightHeading4",
            tokens.font_size_heading4,
            tokens.line_height_heading4,
        ),
        (
            "lineHeightHeading5",
            tokens.font_size_heading5,
            tokens.line_height_heading5,
        ),
    ] {
        let native_height = (size as f32) * (height as f32);
        if !native_height.is_finite() || native_height <= 0.0 {
            return Err(ThemeError::invalid(
                name,
                "computed text height must remain finite and positive in GPUI",
            ));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests;
