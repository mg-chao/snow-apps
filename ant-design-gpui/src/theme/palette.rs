// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Palette algorithm adapted from @ant-design/colors 8.0.1 (MIT).

use super::Color;

/// The 13 preset color families supported by Ant Design 6.6.7.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[repr(u8)]
pub enum PresetColor {
    Blue,
    Purple,
    Cyan,
    Green,
    Magenta,
    Pink,
    Red,
    Orange,
    Yellow,
    Volcano,
    Geekblue,
    Gold,
    Lime,
}
impl PresetColor {
    pub const ALL: [Self; 13] = [
        Self::Blue,
        Self::Purple,
        Self::Cyan,
        Self::Green,
        Self::Magenta,
        Self::Pink,
        Self::Red,
        Self::Orange,
        Self::Yellow,
        Self::Volcano,
        Self::Geekblue,
        Self::Gold,
        Self::Lime,
    ];
    pub const fn name(self) -> &'static str {
        match self {
            Self::Blue => "blue",
            Self::Purple => "purple",
            Self::Cyan => "cyan",
            Self::Green => "green",
            Self::Magenta => "magenta",
            Self::Pink => "pink",
            Self::Red => "red",
            Self::Orange => "orange",
            Self::Yellow => "yellow",
            Self::Volcano => "volcano",
            Self::Geekblue => "geekblue",
            Self::Gold => "gold",
            Self::Lime => "lime",
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Palette {
    pub tones: [Color; 10],
    pub hover: Color,
    pub active: Color,
}
impl Palette {
    /// Access a one-based Ant tone without unchecked indexing.
    pub fn tone(&self, tone: u8) -> Option<Color> {
        tone.checked_sub(1)
            .and_then(|index| self.tones.get(usize::from(index)))
            .copied()
    }
}
#[derive(Clone, Debug, Default, PartialEq)]
pub struct PresetPalettes(pub(crate) [Palette; 13]);
impl PresetPalettes {
    pub fn get(&self, color: PresetColor) -> &Palette {
        &self.0[color as usize]
    }
    pub fn get_mut(&mut self, color: PresetColor) -> &mut Palette {
        &mut self.0[color as usize]
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct PalettePatch {
    pub tones: [Option<Color>; 10],
    pub hover: Option<Color>,
    pub active: Option<Color>,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct PresetPalettesPatch(pub [PalettePatch; 13]);
impl PresetPalettesPatch {
    pub fn get_mut(&mut self, color: PresetColor) -> &mut PalettePatch {
        &mut self.0[color as usize]
    }
    pub fn merged(parent: &Self, local: &Self) -> Self {
        Self(std::array::from_fn(|index| PalettePatch {
            tones: std::array::from_fn(|tone| {
                local.0[index].tones[tone].or(parent.0[index].tones[tone])
            }),
            hover: local.0[index].hover.or(parent.0[index].hover),
            active: local.0[index].active.or(parent.0[index].active),
        }))
    }
    pub fn apply(&self, palettes: &mut PresetPalettes) {
        for (patch, palette) in self.0.iter().zip(&mut palettes.0) {
            for (value, tone) in patch.tones.iter().zip(&mut palette.tones) {
                if let Some(value) = value {
                    *tone = *value;
                }
            }
            if let Some(value) = patch.hover {
                palette.hover = value;
            }
            if let Some(value) = patch.active {
                palette.active = value;
            }
        }
    }
    pub fn is_empty(&self) -> bool {
        self.0.iter().all(|patch| {
            patch.tones.iter().all(Option::is_none)
                && patch.hover.is_none()
                && patch.active.is_none()
        })
    }
}

impl super::values::ValidateToken for PresetPalettes {
    fn validate_token(&self, name: &'static str) -> Result<(), super::ThemeError> {
        for palette in &self.0 {
            for color in palette
                .tones
                .iter()
                .chain([&palette.hover, &palette.active])
            {
                color.validate_token(name)?;
            }
        }
        Ok(())
    }
}
impl super::values::ValidateToken for PresetPalettesPatch {
    fn validate_token(&self, name: &'static str) -> Result<(), super::ThemeError> {
        for palette in &self.0 {
            for color in palette
                .tones
                .iter()
                .chain([&palette.hover, &palette.active])
            {
                color.validate_token(name)?;
            }
        }
        Ok(())
    }
}

/// Generate the exact upstream ten-step HSV palette, optionally mapped for dark surfaces.
pub fn generate_palette(color: Color, dark: bool) -> [Color; 10] {
    let (hue, saturation, value) = color.hsv();
    let mut tones = [Color::BLACK; 10];
    for (index, tone) in tones.iter_mut().enumerate() {
        if index == 5 {
            *tone = color.hex_roundtrip();
            continue;
        }
        let light = index < 5;
        let step = if light { 5 - index } else { index - 5 };
        let direction = if (60.0..=240.0).contains(&hue.round()) {
            -1.0
        } else {
            1.0
        };
        let h = (hue.round() + direction * if light { 1.0 } else { -1.0 } * 2.0 * step as f64)
            .rem_euclid(360.0);
        let s = if hue == 0.0 && saturation == 0.0 {
            0.0
        } else {
            let raw = if light {
                saturation - 0.16 * step as f64
            } else {
                saturation + if step == 4 { 0.16 } else { 0.05 * step as f64 }
            };
            let max = if light && step == 5 { 0.1 } else { 1.0 };
            (raw.clamp(0.06, max) * 100.0).round() / 100.0
        };
        let v = ((value
            + if light {
                0.05 * step as f64
            } else {
                -0.15 * step as f64
            })
        .clamp(0.0, 1.0)
            * 100.0)
            .round()
            / 100.0;
        *tone = Color::from_hsv(h, s, v);
    }
    if dark {
        let background = Color::rgb(0x141414);
        let mapping = [
            (7, 15.0),
            (6, 25.0),
            (5, 30.0),
            (5, 45.0),
            (5, 65.0),
            (5, 85.0),
            (4, 90.0),
            (3, 95.0),
            (2, 97.0),
            (1, 98.0),
        ];
        mapping.map(|(index, percent)| {
            background
                .mix(tones[index], percent / 100.0)
                .hex_roundtrip()
        })
    } else {
        tones
    }
}
