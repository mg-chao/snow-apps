// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Behavioral reference: Ant Design 6.6.7 / @ant-design/fast-color 3.0.1 (MIT).

use std::{fmt, str::FromStr, sync::Arc};

/// Display notation. Changing notation never changes the stored color.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum ColorPickerFormat {
    #[default]
    Hex,
    Rgb,
    Hsb,
}
impl fmt::Display for ColorPickerFormat {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::Hex => "hex",
            Self::Rgb => "rgb",
            Self::Hsb => "hsb",
        })
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum ColorPickerMode {
    #[default]
    Single,
    Gradient,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ColorPickerError {
    InvalidColor,
    NonFiniteChannel,
    ChannelOutOfRange,
    InvalidStopPercent,
}
impl fmt::Display for ColorPickerError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::InvalidColor => "expected a hexadecimal, RGB, HSL, HSB, HSV, or named color",
            Self::NonFiniteChannel => "color channels must be finite",
            Self::ChannelOutOfRange => "color channels are outside their supported range",
            Self::InvalidStopPercent => {
                "gradient stop percentages must be finite and between 0 and 100"
            }
        })
    }
}
impl std::error::Error for ColorPickerError {}

/// A precise, unpremultiplied color. HSB is canonical so editing black or gray
/// retains the chosen hue and saturation. Only display and paint round to bytes.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PickerColor {
    hue: f64,
    saturation: f64,
    brightness: f64,
    alpha: f64,
}
impl Default for PickerColor {
    fn default() -> Self {
        Self::BLACK
    }
}
impl PickerColor {
    pub const BLACK: Self = Self {
        hue: 0.,
        saturation: 0.,
        brightness: 0.,
        alpha: 1.,
    };
    pub const WHITE: Self = Self {
        hue: 0.,
        saturation: 0.,
        brightness: 1.,
        alpha: 1.,
    };
    pub const TRANSPARENT: Self = Self {
        alpha: 0.,
        ..Self::BLACK
    };

    /// Hue is degrees (0..=360); saturation, brightness and alpha are 0..=1.
    pub fn new(
        hue: f64,
        saturation: f64,
        brightness: f64,
        alpha: f64,
    ) -> Result<Self, ColorPickerError> {
        validate_channels(&[hue, saturation, brightness, alpha])?;
        if !(0. ..=360.).contains(&hue)
            || [saturation, brightness, alpha]
                .iter()
                .any(|v| !(0. ..=1.).contains(v))
        {
            return Err(ColorPickerError::ChannelOutOfRange);
        }
        Ok(Self {
            hue,
            saturation,
            brightness,
            alpha,
        })
    }

    pub fn from_hsb(
        hue: f64,
        saturation: f64,
        brightness: f64,
        alpha: f64,
    ) -> Result<Self, ColorPickerError> {
        Self::new(hue, saturation, brightness, alpha)
    }

    /// RGB uses the byte scale (0..=255) and alpha uses 0..=1.
    pub fn from_rgb(red: f64, green: f64, blue: f64, alpha: f64) -> Result<Self, ColorPickerError> {
        validate_channels(&[red, green, blue, alpha])?;
        if [red, green, blue].iter().any(|v| !(0. ..=255.).contains(v))
            || !(0. ..=1.).contains(&alpha)
        {
            return Err(ColorPickerError::ChannelOutOfRange);
        }
        let max = red.max(green).max(blue);
        let min = red.min(green).min(blue);
        let delta = max - min;
        let hue = if delta == 0. {
            0.
        } else if max == red {
            (60. * ((green - blue) / delta)).rem_euclid(360.)
        } else if max == green {
            60. * ((blue - red) / delta + 2.)
        } else {
            60. * ((red - green) / delta + 4.)
        };
        Ok(Self {
            hue,
            saturation: if max == 0. { 0. } else { delta / max },
            brightness: max / 255.,
            alpha,
        })
    }

    pub fn parse(input: &str) -> Result<Self, ColorPickerError> {
        input.parse()
    }

    /// Returns (hue in degrees, saturation, brightness, alpha).
    pub fn to_hsb(self) -> (f64, f64, f64, f64) {
        (self.hue, self.saturation, self.brightness, self.alpha)
    }
    fn rgb_channels(self) -> (f64, f64, f64) {
        let h = self.hue.rem_euclid(360.) / 60.;
        let c = self.brightness * self.saturation;
        let x = c * (1. - (h % 2. - 1.).abs());
        let m = self.brightness - c;
        let (r, g, b) = match h.floor() as u8 {
            0 => (c, x, 0.),
            1 => (x, c, 0.),
            2 => (0., c, x),
            3 => (0., x, c),
            4 => (x, 0., c),
            _ => (c, 0., x),
        };
        ((r + m) * 255., (g + m) * 255., (b + m) * 255.)
    }
    /// Returns rounded (red, green, blue, alpha); RGB channels use bytes.
    pub fn to_rgb(self) -> (u8, u8, u8, f64) {
        let (r, g, b) = self.rgb_channels();
        (
            r.round() as u8,
            g.round() as u8,
            b.round() as u8,
            self.alpha,
        )
    }
    pub fn to_theme_color(self) -> crate::theme::Color {
        let (r, g, b, a) = self.to_rgb();
        crate::theme::Color { r, g, b, a }
    }
    pub fn to_rgba(self) -> gpui::Rgba {
        let (r, g, b) = self.rgb_channels();
        gpui::Rgba {
            r: (r / 255.) as f32,
            g: (g / 255.) as f32,
            b: (b / 255.) as f32,
            a: self.alpha as f32,
        }
    }
    pub fn with_hue(self, hue: f64) -> Self {
        Self {
            hue: edit_channel(hue, self.hue, 360.),
            ..self
        }
    }
    pub fn with_saturation(self, saturation: f64) -> Self {
        Self {
            saturation: edit_channel(saturation, self.saturation, 1.),
            ..self
        }
    }
    pub fn with_brightness(self, brightness: f64) -> Self {
        Self {
            brightness: edit_channel(brightness, self.brightness, 1.),
            ..self
        }
    }
    pub fn with_alpha(self, alpha: f64) -> Self {
        Self {
            alpha: edit_channel(alpha, self.alpha, 1.),
            ..self
        }
    }
    pub fn to_hex(self) -> String {
        let (r, g, b, a) = self.to_rgb();
        if a < 1. {
            format!("{r:02x}{g:02x}{b:02x}{:02x}", (a * 255.).round() as u8)
        } else {
            format!("{r:02x}{g:02x}{b:02x}")
        }
    }
    pub fn to_hex_string(self) -> String {
        format!("#{}", self.to_hex())
    }
    pub fn to_rgb_string(self) -> String {
        let (r, g, b, a) = self.to_rgb();
        if a == 1. {
            format!("rgb({r},{g},{b})")
        } else {
            format!("rgba({r},{g},{b},{})", concise_number(a))
        }
    }
    pub fn to_hsb_string(self) -> String {
        let (h, s, b, a) = self.to_hsb();
        if a == 1. {
            format!(
                "hsb({}, {}%, {}%)",
                h.round(),
                (s * 100.).round(),
                (b * 100.).round()
            )
        } else {
            format!(
                "hsba({}, {}%, {}%, {})",
                h.round(),
                (s * 100.).round(),
                (b * 100.).round(),
                if a == 0. {
                    "0".into()
                } else {
                    format!("{a:.2}")
                }
            )
        }
    }
    pub fn to_css_string(self) -> String {
        self.to_rgb_string()
    }
    /// Contrast rule used by checked preset swatches in the reference.
    pub fn is_bright_on(self, background: crate::theme::Color) -> bool {
        let (r, g, b, a) = self.to_rgb();
        if a <= 0.5 {
            let composite =
                |front: u8, back: u8| (f64::from(front) * a + f64::from(back) * (1. - a)).round();
            composite(r, background.r)
                .max(composite(g, background.g))
                .max(composite(b, background.b))
                > 127.5
        } else {
            f64::from(r) * 0.299 + f64::from(g) * 0.587 + f64::from(b) * 0.114 > 192.
        }
    }

    /// Interpolates unpremultiplied sRGB channels, as the reference gradient editor does.
    pub(crate) fn mix(self, other: Self, amount: f64) -> Self {
        let amount = amount.clamp(0., 1.);
        let (r, g, b) = self.rgb_channels();
        let (rr, gg, bb) = other.rgb_channels();
        Self::from_rgb(
            r + (rr - r) * amount,
            g + (gg - g) * amount,
            b + (bb - b) * amount,
            self.alpha + (other.alpha - self.alpha) * amount,
        )
        .expect("interpolation stays within channel bounds")
    }
}
impl From<crate::theme::Color> for PickerColor {
    fn from(color: crate::theme::Color) -> Self {
        Self::from_rgb(
            f64::from(color.r),
            f64::from(color.g),
            f64::from(color.b),
            color.a.clamp(0., 1.),
        )
        .unwrap_or(Self::BLACK)
    }
}
impl From<PickerColor> for gpui::Rgba {
    fn from(value: PickerColor) -> Self {
        value.to_rgba()
    }
}
impl From<PickerColor> for gpui::Hsla {
    fn from(value: PickerColor) -> Self {
        value.to_rgba().into()
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ColorStop {
    pub color: PickerColor,
    pub percent: f64,
}
impl ColorStop {
    pub fn new(color: PickerColor, percent: f64) -> Result<Self, ColorPickerError> {
        validate_percent(percent)?;
        Ok(Self { color, percent })
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub enum ColorPickerValue {
    #[default]
    Cleared,
    Single(PickerColor),
    Gradient(Arc<[ColorStop]>),
}
impl From<PickerColor> for ColorPickerValue {
    fn from(value: PickerColor) -> Self {
        Self::Single(value)
    }
}
impl From<crate::theme::Color> for ColorPickerValue {
    fn from(value: crate::theme::Color) -> Self {
        Self::Single(value.into())
    }
}
impl FromStr for ColorPickerValue {
    type Err = ColorPickerError;
    fn from_str(input: &str) -> Result<Self, Self::Err> {
        if input.trim().is_empty() {
            Ok(Self::Cleared)
        } else {
            input.parse().map(Self::Single)
        }
    }
}
impl ColorPickerValue {
    pub fn gradient(stops: impl IntoIterator<Item = ColorStop>) -> Result<Self, ColorPickerError> {
        let mut stops: Vec<_> = stops.into_iter().collect();
        for stop in &stops {
            validate_percent(stop.percent)?;
        }
        stops.sort_by(|a, b| a.percent.total_cmp(&b.percent));
        if stops.is_empty() {
            Ok(Self::Cleared)
        } else {
            Ok(Self::Gradient(stops.into()))
        }
    }
    pub fn validate(&self) -> Result<(), ColorPickerError> {
        if let Self::Gradient(stops) = self {
            for stop in stops.iter() {
                validate_percent(stop.percent)?;
            }
        }
        Ok(())
    }
    /// Validate raw enum construction and return the same canonical form as
    /// `gradient`: empty means cleared and stop ordering is stable.
    pub(crate) fn canonicalized(&self) -> Result<Self, ColorPickerError> {
        self.validate()?;
        match self {
            Self::Gradient(stops) if stops.is_empty() => Ok(Self::Cleared),
            Self::Gradient(stops)
                if stops
                    .windows(2)
                    .any(|pair| pair[0].percent > pair[1].percent) =>
            {
                Self::gradient(stops.iter().copied())
            }
            _ => Ok(self.clone()),
        }
    }
    pub fn is_cleared(&self) -> bool {
        matches!(self, Self::Cleared) || matches!(self, Self::Gradient(stops) if stops.is_empty())
    }
    pub fn is_gradient(&self) -> bool {
        matches!(self, Self::Gradient(stops) if !stops.is_empty())
    }
    pub fn active_color(&self, index: usize) -> PickerColor {
        match self {
            Self::Cleared => PickerColor::TRANSPARENT,
            Self::Single(color) => *color,
            Self::Gradient(stops) => stops
                .get(index)
                .or_else(|| stops.first())
                .map_or(PickerColor::TRANSPARENT, |s| s.color),
        }
    }
    pub fn with_opaque_alpha(&self) -> Self {
        match self {
            Self::Cleared => Self::Cleared,
            Self::Single(color) => Self::Single(color.with_alpha(1.)),
            Self::Gradient(stops) if stops.is_empty() => Self::Cleared,
            Self::Gradient(stops) if stops.iter().all(|s| s.color.alpha == 1.) => self.clone(),
            Self::Gradient(stops) => Self::Gradient(
                stops
                    .iter()
                    .map(|s| ColorStop {
                        color: s.color.with_alpha(1.),
                        ..*s
                    })
                    .collect(),
            ),
        }
    }
    pub fn to_css_string(&self) -> String {
        match self {
            Self::Cleared => "rgba(0,0,0,0)".into(),
            Self::Single(color) => color.to_css_string(),
            Self::Gradient(stops) if stops.is_empty() => "rgba(0,0,0,0)".into(),
            Self::Gradient(stops) => format!(
                "linear-gradient(90deg, {})",
                stops
                    .iter()
                    .map(|s| format!("{} {}%", s.color.to_rgb_string(), concise_number(s.percent)))
                    .collect::<Vec<_>>()
                    .join(", ")
            ),
        }
    }
    /// Preset equality compares displayed sRGB, not hidden achromatic hue.
    pub fn visual_eq(&self, other: &Self) -> bool {
        if self.is_cleared() && other.is_cleared() {
            return true;
        }
        match (self, other) {
            (Self::Cleared, Self::Cleared) => true,
            (Self::Single(a), Self::Single(b)) => a.to_rgb() == b.to_rgb(),
            (Self::Gradient(a), Self::Gradient(b)) => {
                a.len() == b.len()
                    && a.iter().zip(b.iter()).all(|(a, b)| {
                        a.percent == b.percent && a.color.to_rgb() == b.color.to_rgb()
                    })
            }
            _ => false,
        }
    }
    /// Equality used by preset swatches; remembered achromatic hue is ignored.
    pub fn preset_eq(&self, other: &Self) -> bool {
        self.visual_eq(other)
    }
}

fn validate_channels(channels: &[f64]) -> Result<(), ColorPickerError> {
    if channels.iter().any(|v| !v.is_finite()) {
        Err(ColorPickerError::NonFiniteChannel)
    } else {
        Ok(())
    }
}
pub(crate) fn validate_percent(percent: f64) -> Result<(), ColorPickerError> {
    if !percent.is_finite() || !(0. ..=100.).contains(&percent) {
        Err(ColorPickerError::InvalidStopPercent)
    } else {
        Ok(())
    }
}
fn edit_channel(value: f64, original: f64, max: f64) -> f64 {
    if value.is_finite() {
        value.clamp(0., max)
    } else {
        original
    }
}
pub(crate) fn concise_number(value: f64) -> String {
    if value == 0. {
        "0".into()
    } else {
        value.to_string()
    }
}

impl FromStr for PickerColor {
    type Err = ColorPickerError;
    fn from_str(input: &str) -> Result<Self, Self::Err> {
        let input = input.trim();
        if input.is_empty() {
            return Err(ColorPickerError::InvalidColor);
        }
        if input.eq_ignore_ascii_case("transparent") {
            return Ok(Self::TRANSPARENT);
        }
        let hex = input.strip_prefix('#').unwrap_or(input);
        if [3, 4, 6, 8].contains(&hex.len()) && hex.bytes().all(|b| b.is_ascii_hexdigit()) {
            let owned;
            let expanded = if hex.len() <= 4 {
                owned = hex.chars().flat_map(|c| [c, c]).collect::<String>();
                owned.as_str()
            } else {
                hex
            };
            let n =
                u32::from_str_radix(expanded, 16).map_err(|_| ColorPickerError::InvalidColor)?;
            let (rgb, a) = if expanded.len() == 8 {
                (n >> 8, f64::from((n & 255) as u8) / 255.)
            } else {
                (n, 1.)
            };
            return Self::from_rgb(
                f64::from(((rgb >> 16) & 255) as u8),
                f64::from(((rgb >> 8) & 255) as u8),
                f64::from((rgb & 255) as u8),
                a,
            );
        }
        if let Some(hex) = named_color(input) {
            return format!("#{hex:06x}").parse();
        }
        parse_function(input)
    }
}

fn number(input: &str) -> Result<f64, ColorPickerError> {
    let value = input
        .trim()
        .parse::<f64>()
        .map_err(|_| ColorPickerError::InvalidColor)?;
    if value.is_finite() {
        Ok(value)
    } else {
        Err(ColorPickerError::NonFiniteChannel)
    }
}
fn fraction(input: &str) -> Result<f64, ColorPickerError> {
    let input = input.trim();
    let value = if let Some(v) = input.strip_suffix('%') {
        number(v)? / 100.
    } else {
        number(input)?
    };
    if (0. ..=1.).contains(&value) {
        Ok(value)
    } else {
        Err(ColorPickerError::ChannelOutOfRange)
    }
}
fn hue(input: &str) -> Result<f64, ColorPickerError> {
    let input = input.trim().to_ascii_lowercase();
    let (v, factor) = if let Some(v) = input.strip_suffix("turn") {
        (v, 360.)
    } else if let Some(v) = input.strip_suffix("grad") {
        (v, 0.9)
    } else if let Some(v) = input.strip_suffix("rad") {
        (v, 180. / std::f64::consts::PI)
    } else {
        (input.strip_suffix("deg").unwrap_or(&input), 1.)
    };
    let value = number(v)? * factor;
    if value.is_finite() {
        Ok(value.rem_euclid(360.))
    } else {
        Err(ColorPickerError::NonFiniteChannel)
    }
}
fn parse_function(input: &str) -> Result<PickerColor, ColorPickerError> {
    let (name, tail) = input
        .split_once('(')
        .ok_or(ColorPickerError::InvalidColor)?;
    let body = tail
        .strip_suffix(')')
        .ok_or(ColorPickerError::InvalidColor)?
        .trim();
    let name = name.trim().to_ascii_lowercase();
    if !["rgb", "rgba", "hsl", "hsla", "hsv", "hsva", "hsb", "hsba"].contains(&name.as_str()) {
        return Err(ColorPickerError::InvalidColor);
    }
    let mut channels: Vec<&str>;
    let alpha;
    if body.contains(',') {
        if body.contains('/') {
            return Err(ColorPickerError::InvalidColor);
        }
        channels = body.split(',').map(str::trim).collect();
        if channels.len() == 4 {
            alpha = fraction(channels.pop().unwrap())?;
        } else if channels.len() == 3 {
            alpha = 1.;
        } else {
            return Err(ColorPickerError::InvalidColor);
        }
    } else {
        let mut pieces = body.split('/');
        channels = pieces
            .next()
            .unwrap_or_default()
            .split_ascii_whitespace()
            .collect();
        alpha = pieces.next().map(fraction).transpose()?.unwrap_or(1.);
        if pieces.next().is_some() || channels.len() != 3 {
            return Err(ColorPickerError::InvalidColor);
        }
    }
    if name.starts_with("rgb") {
        let channel = |s: &str| -> Result<f64, ColorPickerError> {
            if let Some(v) = s.strip_suffix('%') {
                Ok(number(v)? * 255. / 100.)
            } else {
                number(s)
            }
        };
        return PickerColor::from_rgb(
            channel(channels[0])?,
            channel(channels[1])?,
            channel(channels[2])?,
            alpha,
        );
    }
    let h = hue(channels[0])?;
    let percent = |s: &str| -> Result<f64, ColorPickerError> {
        fraction(&format!("{}%", s.strip_suffix('%').unwrap_or(s)))
    };
    let s = percent(channels[1])?;
    let v = percent(channels[2])?;
    if name.starts_with("hsl") {
        let c = (1. - (2. * v - 1.).abs()) * s;
        let b = v + c / 2.;
        let hsv_s = if b == 0. { 0. } else { c / b };
        PickerColor::new(h, hsv_s, b, alpha)
    } else {
        PickerColor::new(h, s, v, alpha)
    }
}

fn named_color(input: &str) -> Option<u32> {
    // CSS named-color table; populated below. Names are ASCII case-insensitive.
    match input.to_ascii_lowercase().as_str() {
        "aliceblue" => Some(0xf0f8ff),
        "antiquewhite" => Some(0xfaebd7),
        "aqua" => Some(0x00ffff),
        "aquamarine" => Some(0x7fffd4),
        "azure" => Some(0xf0ffff),
        "beige" => Some(0xf5f5dc),
        "bisque" => Some(0xffe4c4),
        "black" => Some(0x000000),
        "blanchedalmond" => Some(0xffebcd),
        "blue" => Some(0x0000ff),
        "blueviolet" => Some(0x8a2be2),
        "brown" => Some(0xa52a2a),
        "burlywood" => Some(0xdeb887),
        "cadetblue" => Some(0x5f9ea0),
        "chartreuse" => Some(0x7fff00),
        "chocolate" => Some(0xd2691e),
        "coral" => Some(0xff7f50),
        "cornflowerblue" => Some(0x6495ed),
        "cornsilk" => Some(0xfff8dc),
        "crimson" => Some(0xdc143c),
        "cyan" => Some(0x00ffff),
        "darkblue" => Some(0x00008b),
        "darkcyan" => Some(0x008b8b),
        "darkgoldenrod" => Some(0xb8860b),
        "darkgray" => Some(0xa9a9a9),
        "darkgreen" => Some(0x006400),
        "darkgrey" => Some(0xa9a9a9),
        "darkkhaki" => Some(0xbdb76b),
        "darkmagenta" => Some(0x8b008b),
        "darkolivegreen" => Some(0x556b2f),
        "darkorange" => Some(0xff8c00),
        "darkorchid" => Some(0x9932cc),
        "darkred" => Some(0x8b0000),
        "darksalmon" => Some(0xe9967a),
        "darkseagreen" => Some(0x8fbc8f),
        "darkslateblue" => Some(0x483d8b),
        "darkslategray" => Some(0x2f4f4f),
        "darkslategrey" => Some(0x2f4f4f),
        "darkturquoise" => Some(0x00ced1),
        "darkviolet" => Some(0x9400d3),
        "deeppink" => Some(0xff1493),
        "deepskyblue" => Some(0x00bfff),
        "dimgray" => Some(0x696969),
        "dimgrey" => Some(0x696969),
        "dodgerblue" => Some(0x1e90ff),
        "firebrick" => Some(0xb22222),
        "floralwhite" => Some(0xfffaf0),
        "forestgreen" => Some(0x228b22),
        "fuchsia" => Some(0xff00ff),
        "gainsboro" => Some(0xdcdcdc),
        "ghostwhite" => Some(0xf8f8ff),
        "goldenrod" => Some(0xdaa520),
        "gold" => Some(0xffd700),
        "gray" => Some(0x808080),
        "green" => Some(0x008000),
        "greenyellow" => Some(0xadff2f),
        "grey" => Some(0x808080),
        "honeydew" => Some(0xf0fff0),
        "hotpink" => Some(0xff69b4),
        "indianred" => Some(0xcd5c5c),
        "indigo" => Some(0x4b0082),
        "ivory" => Some(0xfffff0),
        "khaki" => Some(0xf0e68c),
        "lavenderblush" => Some(0xfff0f5),
        "lavender" => Some(0xe6e6fa),
        "lawngreen" => Some(0x7cfc00),
        "lemonchiffon" => Some(0xfffacd),
        "lightblue" => Some(0xadd8e6),
        "lightcoral" => Some(0xf08080),
        "lightcyan" => Some(0xe0ffff),
        "lightgoldenrodyellow" => Some(0xfafad2),
        "lightgray" => Some(0xd3d3d3),
        "lightgreen" => Some(0x90ee90),
        "lightgrey" => Some(0xd3d3d3),
        "lightpink" => Some(0xffb6c1),
        "lightsalmon" => Some(0xffa07a),
        "lightseagreen" => Some(0x20b2aa),
        "lightskyblue" => Some(0x87cefa),
        "lightslategray" => Some(0x778899),
        "lightslategrey" => Some(0x778899),
        "lightsteelblue" => Some(0xb0c4de),
        "lightyellow" => Some(0xffffe0),
        "lime" => Some(0x00ff00),
        "limegreen" => Some(0x32cd32),
        "linen" => Some(0xfaf0e6),
        "magenta" => Some(0xff00ff),
        "maroon" => Some(0x800000),
        "mediumaquamarine" => Some(0x66cdaa),
        "mediumblue" => Some(0x0000cd),
        "mediumorchid" => Some(0xba55d3),
        "mediumpurple" => Some(0x9370db),
        "mediumseagreen" => Some(0x3cb371),
        "mediumslateblue" => Some(0x7b68ee),
        "mediumspringgreen" => Some(0x00fa9a),
        "mediumturquoise" => Some(0x48d1cc),
        "mediumvioletred" => Some(0xc71585),
        "midnightblue" => Some(0x191970),
        "mintcream" => Some(0xf5fffa),
        "mistyrose" => Some(0xffe4e1),
        "moccasin" => Some(0xffe4b5),
        "navajowhite" => Some(0xffdead),
        "navy" => Some(0x000080),
        "oldlace" => Some(0xfdf5e6),
        "olive" => Some(0x808000),
        "olivedrab" => Some(0x6b8e23),
        "orange" => Some(0xffa500),
        "orangered" => Some(0xff4500),
        "orchid" => Some(0xda70d6),
        "palegoldenrod" => Some(0xeee8aa),
        "palegreen" => Some(0x98fb98),
        "paleturquoise" => Some(0xafeeee),
        "palevioletred" => Some(0xdb7093),
        "papayawhip" => Some(0xffefd5),
        "peachpuff" => Some(0xffdab9),
        "peru" => Some(0xcd853f),
        "pink" => Some(0xffc0cb),
        "plum" => Some(0xdda0dd),
        "powderblue" => Some(0xb0e0e6),
        "purple" => Some(0x800080),
        "rebeccapurple" => Some(0x663399),
        "red" => Some(0xff0000),
        "rosybrown" => Some(0xbc8f8f),
        "royalblue" => Some(0x4169e1),
        "saddlebrown" => Some(0x8b4513),
        "salmon" => Some(0xfa8072),
        "sandybrown" => Some(0xf4a460),
        "seagreen" => Some(0x2e8b57),
        "seashell" => Some(0xfff5ee),
        "sienna" => Some(0xa0522d),
        "silver" => Some(0xc0c0c0),
        "skyblue" => Some(0x87ceeb),
        "slateblue" => Some(0x6a5acd),
        "slategray" => Some(0x708090),
        "slategrey" => Some(0x708090),
        "snow" => Some(0xfffafa),
        "springgreen" => Some(0x00ff7f),
        "steelblue" => Some(0x4682b4),
        "tan" => Some(0xd2b48c),
        "teal" => Some(0x008080),
        "thistle" => Some(0xd8bfd8),
        "tomato" => Some(0xff6347),
        "turquoise" => Some(0x40e0d0),
        "violet" => Some(0xee82ee),
        "wheat" => Some(0xf5deb3),
        "white" => Some(0xffffff),
        "whitesmoke" => Some(0xf5f5f5),
        "yellow" => Some(0xffff00),
        "yellowgreen" => Some(0x9acd32),
        _ => None,
    }
}

#[cfg(test)]
#[path = "color_tests.rs"]
mod tests;
