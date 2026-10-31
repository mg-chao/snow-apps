// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Color conversion behavior adapted from @ant-design/fast-color 3.0.1 (MIT).

use std::{fmt, str::FromStr, sync::Arc};

/// An sRGB color with byte RGB channels and an unpremultiplied alpha channel.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Color {
    pub r: u8,
    pub g: u8,
    pub b: u8,
    pub a: f64,
}

impl Color {
    pub const BLACK: Self = Self::rgb(0x000000);
    pub const WHITE: Self = Self::rgb(0xffffff);
    pub const TRANSPARENT: Self = Self {
        a: 0.0,
        ..Self::BLACK
    };

    pub const fn rgb(hex: u32) -> Self {
        Self {
            r: (hex >> 16) as u8,
            g: (hex >> 8) as u8,
            b: hex as u8,
            a: 1.0,
        }
    }

    pub const fn rgba(hex: u32) -> Self {
        Self {
            r: (hex >> 24) as u8,
            g: (hex >> 16) as u8,
            b: (hex >> 8) as u8,
            a: (hex as u8) as f64 / 255.0,
        }
    }

    pub fn with_alpha(self, alpha: f64) -> Self {
        Self {
            a: alpha.clamp(0.0, 1.0),
            ..self
        }
    }

    pub fn to_rgba(self) -> gpui::Rgba {
        gpui::Rgba {
            r: f32::from(self.r) / 255.0,
            g: f32::from(self.g) / 255.0,
            b: f32::from(self.b) / 255.0,
            a: self.a as f32,
        }
    }

    pub fn to_hsla(self) -> gpui::Hsla {
        self.to_rgba().into()
    }

    pub(crate) fn mix(self, other: Self, amount: f64) -> Self {
        let channel =
            |a: u8, b: u8| (f64::from(a) + (f64::from(b) - f64::from(a)) * amount).round() as u8;
        Self {
            r: channel(self.r, other.r),
            g: channel(self.g, other.g),
            b: channel(self.b, other.b),
            a: ((self.a + (other.a - self.a) * amount) * 100.0).round() / 100.0,
        }
    }

    pub(crate) fn hex_roundtrip(self) -> Self {
        Self {
            a: (self.a * 255.0).round() / 255.0,
            ..self
        }
    }

    pub(crate) fn hsv(self) -> (f64, f64, f64) {
        let r = f64::from(self.r);
        let g = f64::from(self.g);
        let b = f64::from(self.b);
        let max = r.max(g).max(b);
        let min = r.min(g).min(b);
        let delta = max - min;
        let hue = if delta == 0.0 {
            0.0
        } else if max == r {
            ((g - b) / delta + if g < b { 6.0 } else { 0.0 }) * 60.0
        } else if max == g {
            ((b - r) / delta + 2.0) * 60.0
        } else {
            ((r - g) / delta + 4.0) * 60.0
        };
        (
            hue.round(),
            if max == 0.0 { 0.0 } else { delta / max },
            max / 255.0,
        )
    }

    pub(crate) fn from_hsv(hue: f64, saturation: f64, value: f64) -> Self {
        let h = hue.rem_euclid(360.0) / 60.0;
        let i = h.floor() as u32;
        let f = h - h.floor();
        let p = value * (1.0 - saturation);
        let q = value * (1.0 - f * saturation);
        let t = value * (1.0 - (1.0 - f) * saturation);
        let (r, g, b) = match i {
            0 => (value, t, p),
            1 => (q, value, p),
            2 => (p, value, t),
            3 => (p, q, value),
            4 => (t, p, value),
            _ => (value, p, q),
        };
        Self {
            r: (r * 255.0).round() as u8,
            g: (g * 255.0).round() as u8,
            b: (b * 255.0).round() as u8,
            a: 1.0,
        }
    }

    pub(crate) fn adjust_lightness(self, amount: f64) -> Self {
        let (h, saturation, _) = self.hsv();
        let max = f64::from(self.r.max(self.g).max(self.b)) / 255.0;
        let min = f64::from(self.r.min(self.g).min(self.b)) / 255.0;
        let lightness = (max + min) / 2.0;
        // fast-color 3.0.1 intentionally uses its legacy HSV saturation getter
        // in lighten/darken before constructing an HSL color. Preserve parity.
        let l = (lightness + amount).clamp(0.0, 1.0);
        let chroma = (1.0 - (2.0 * l - 1.0).abs()) * saturation;
        let x = chroma * (1.0 - ((h / 60.0) % 2.0 - 1.0).abs());
        let m = l - chroma / 2.0;
        let (r, g, b) = match (h / 60.0).floor() as u32 {
            0 => (chroma, x, 0.0),
            1 => (x, chroma, 0.0),
            2 => (0.0, chroma, x),
            3 => (0.0, x, chroma),
            4 => (x, 0.0, chroma),
            _ => (chroma, 0.0, x),
        };
        Self {
            r: ((r + m) * 255.0).round() as u8,
            g: ((g + m) * 255.0).round() as u8,
            b: ((b + m) * 255.0).round() as u8,
            a: self.a,
        }
        .hex_roundtrip()
    }

    /// Recover a translucent foreground whose composite matches this solid color.
    pub(crate) fn alpha_over(self, background: Self) -> Self {
        if self.a < 1.0 {
            return self;
        }
        let mut alpha = 0.01;
        while alpha <= 1.0 {
            let channel = |front: u8, back: u8| {
                (((f64::from(front) - f64::from(back) * (1.0 - alpha)) / alpha) + 0.5).floor()
            };
            let r = channel(self.r, background.r);
            let g = channel(self.g, background.g);
            let b = channel(self.b, background.b);
            if [r, g, b].iter().all(|value| (0.0..=255.0).contains(value)) {
                return Self {
                    r: r as u8,
                    g: g as u8,
                    b: b as u8,
                    a: (alpha * 100.0).round() / 100.0,
                };
            }
            alpha += 0.01;
        }
        self
    }
}

impl Default for Color {
    fn default() -> Self {
        Self::BLACK
    }
}
impl From<Color> for gpui::Hsla {
    fn from(value: Color) -> Self {
        value.to_hsla()
    }
}
impl From<Color> for gpui::Rgba {
    fn from(value: Color) -> Self {
        value.to_rgba()
    }
}
impl From<Color> for gpui::Fill {
    fn from(value: Color) -> Self {
        value.to_hsla().into()
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ParseColorError;
impl fmt::Display for ParseColorError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("expected #RGB, #RGBA, #RRGGBB, or #RRGGBBAA")
    }
}
impl std::error::Error for ParseColorError {}
impl FromStr for Color {
    type Err = ParseColorError;
    fn from_str(input: &str) -> Result<Self, Self::Err> {
        let hex = input.strip_prefix('#').ok_or(ParseColorError)?;
        if !hex.is_ascii() {
            return Err(ParseColorError);
        }
        let value = u32::from_str_radix(hex, 16).map_err(|_| ParseColorError)?;
        let nibble = |n: u32| ((value >> n) & 15) as u8 * 17;
        match hex.len() {
            3 => Ok(Self {
                r: nibble(8),
                g: nibble(4),
                b: nibble(0),
                a: 1.0,
            }),
            4 => Ok(Self {
                r: nibble(12),
                g: nibble(8),
                b: nibble(4),
                a: f64::from(nibble(0)) / 255.0,
            }),
            6 => Ok(Self::rgb(value)),
            8 => Ok(Self::rgba(value)),
            _ => Err(ParseColorError),
        }
    }
}

/// Native ordered font families; generic CSS names are retained as fallback hints.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct FontStack(pub Arc<[Arc<str>]>);
impl FontStack {
    pub fn new(families: impl IntoIterator<Item = impl Into<Arc<str>>>) -> Self {
        Self(families.into_iter().map(Into::into).collect())
    }
    pub fn families(&self) -> &[Arc<str>] {
        &self.0
    }

    /// Resolve platform-supported CSS aliases without probing the font database.
    /// Cache this descriptor with the resolved configuration rather than per render.
    pub fn to_gpui_font(&self) -> gpui::Font {
        let native_family = |family: &str| {
            let family = match family {
                "-apple-system" | "BlinkMacSystemFont" => {
                    // These CSS names are Apple-specific. Mapping them to another
                    // platform's system font masks later supported families,
                    // including Ant's Segoe UI choice on Windows.
                    cfg!(any(target_os = "macos", target_os = "ios")).then_some(".SystemUIFont")
                }
                "system-ui" | "sans-serif" => Some(".SystemUIFont"),
                "monospace" if cfg!(target_os = "windows") => Some("Consolas"),
                "monospace" if cfg!(target_os = "macos") => Some("SFMono-Regular"),
                other => Some(other),
            };
            family.map(str::to_owned)
        };
        let mut families = Vec::new();
        for family in self.families() {
            if let Some(family) = native_family(family)
                && !family.is_empty()
                && !families.contains(&family)
            {
                families.push(family);
            }
        }
        let mut font = gpui::font(
            families
                .first()
                .cloned()
                .unwrap_or_else(|| ".SystemUIFont".into()),
        );
        if families.len() > 1 {
            font.fallbacks = Some(gpui::FontFallbacks::from_fonts(families[1..].to_vec()));
        }
        font
    }
}

/// Cubic Bézier coordinates. X coordinates are within [0,1]; Y may overshoot.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CubicBezier {
    pub x1: f64,
    pub y1: f64,
    pub x2: f64,
    pub y2: f64,
}
impl CubicBezier {
    pub const fn new(x1: f64, y1: f64, x2: f64, y2: f64) -> Self {
        Self { x1, y1, x2, y2 }
    }
    pub fn is_valid(self) -> bool {
        [self.x1, self.y1, self.x2, self.y2]
            .iter()
            .all(|value| value.is_finite())
            && (0.0..=1.0).contains(&self.x1)
            && (0.0..=1.0).contains(&self.x2)
    }
}
impl Default for CubicBezier {
    fn default() -> Self {
        Self::new(0.0, 0.0, 1.0, 1.0)
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum LineType {
    #[default]
    Solid,
    Dashed,
    Dotted,
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum TextDecoration {
    #[default]
    None,
    Underline,
    LineThrough,
}

/// Shadow dimensions use logical pixels; inset shadows remain explicit.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Shadow {
    pub x: f64,
    pub y: f64,
    pub blur: f64,
    pub spread: f64,
    pub color: Color,
    pub inset: bool,
}
pub type ShadowList = Arc<[Shadow]>;

pub(crate) trait SeedValue<T> {
    fn resolved(&self) -> T;
}
impl<T: Clone> SeedValue<T> for T {
    fn resolved(&self) -> T {
        self.clone()
    }
}
impl SeedValue<Color> for Option<Color> {
    fn resolved(&self) -> Color {
        self.unwrap_or_default()
    }
}

pub(crate) fn clone_token<T: Clone>(value: &T) -> T {
    value.clone()
}
pub(crate) fn to_seed<T: Clone + Into<U>, U>(value: &T) -> U {
    value.clone().into()
}

pub(crate) trait ValidateToken {
    fn validate_token(&self, name: &'static str) -> Result<(), super::ThemeError>;
}
impl ValidateToken for f64 {
    fn validate_token(&self, name: &'static str) -> Result<(), super::ThemeError> {
        if !self.is_finite() || !(*self as f32).is_finite() {
            return Err(super::ThemeError::invalid(
                name,
                "must have a finite GPUI representation",
            ));
        }
        if (name.starts_with("fontSize") || name.starts_with("lineHeight")) && (*self as f32) <= 0.0
        {
            return Err(super::ThemeError::invalid(
                name,
                "must remain positive in GPUI",
            ));
        }
        Ok(())
    }
}
impl ValidateToken for Color {
    fn validate_token(&self, name: &'static str) -> Result<(), super::ThemeError> {
        if self.a.is_finite() && (0.0..=1.0).contains(&self.a) {
            Ok(())
        } else {
            Err(super::ThemeError::invalid(
                name,
                "alpha must be between 0 and 1",
            ))
        }
    }
}
impl ValidateToken for CubicBezier {
    fn validate_token(&self, name: &'static str) -> Result<(), super::ThemeError> {
        if self.is_valid()
            && [self.x1, self.y1, self.x2, self.y2]
                .iter()
                .all(|value| (*value as f32).is_finite())
        {
            Ok(())
        } else {
            Err(super::ThemeError::invalid(
                name,
                "requires finite coordinates and X within [0,1]",
            ))
        }
    }
}
impl<T: ValidateToken> ValidateToken for Option<T> {
    fn validate_token(&self, name: &'static str) -> Result<(), super::ThemeError> {
        match self {
            Some(value) => value.validate_token(name),
            None => Ok(()),
        }
    }
}
impl ValidateToken for ShadowList {
    fn validate_token(&self, name: &'static str) -> Result<(), super::ThemeError> {
        for shadow in self.iter() {
            for value in [shadow.x, shadow.y, shadow.blur, shadow.spread] {
                value.validate_token(name)?;
            }
            if shadow.blur < 0.0 {
                return Err(super::ThemeError::invalid(
                    name,
                    "shadow blur must be nonnegative",
                ));
            }
            shadow.color.validate_token(name)?;
        }
        Ok(())
    }
}
macro_rules! always_valid {
    ($($ty:ty),* $(,)?) => { $(impl ValidateToken for $ty { fn validate_token(&self, _: &'static str) -> Result<(), super::ThemeError> { Ok(()) } })* };
}
always_valid!(
    bool,
    FontStack,
    LineType,
    TextDecoration,
    std::time::Duration
);
