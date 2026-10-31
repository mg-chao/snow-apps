// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Allocation-free layout coefficients shared by native layout and visual fixtures.

use super::SwitchStyle;
use crate::Direction;

/// A physical horizontal coordinate: `fraction * track_width + pixels`.
///
/// Keeping the two terms separate lets GPUI resolve percentage layout in the
/// same pass as the parent, without measuring a control or requesting a redraw.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SwitchOffset {
    pub fraction: f64,
    pub pixels: f64,
}

impl SwitchOffset {
    pub fn resolve(self, width: f64) -> f64 {
        self.fraction * width + self.pixels
    }
}

/// Native layout coefficients at one point in the checked and press transitions.
/// All horizontal coordinates use physical left, including RTL layouts.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SwitchLayout {
    /// Left of the square indicator before its painted surface stretches.
    pub handle_left: SwitchOffset,
    /// Translation of each full-width content slot, before its padding.
    pub checked_slot_left: SwitchOffset,
    pub unchecked_slot_left: SwitchOffset,
    /// Left of each content area after the slot's fixed padding.
    pub checked_content_left: SwitchOffset,
    pub unchecked_content_left: SwitchOffset,
    pub checked_padding_left: f64,
    pub checked_padding_right: f64,
    pub unchecked_padding_left: f64,
    pub unchecked_padding_right: f64,
    pub handle_extension_left: f64,
    pub handle_extension_right: f64,
    /// Space reserved by the handle and outer content margin together.
    pub content_inset: f64,
}

/// Concrete geometry in logical pixels, relative to the track's top-left corner.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SwitchGeometry {
    /// Square indicator bounds, used for the centered loading icon.
    pub indicator_left: f64,
    pub indicator_top: f64,
    pub indicator_size: f64,
    /// Painted handle surface, including its extension while pressed.
    pub handle_left: f64,
    pub handle_top: f64,
    pub handle_width: f64,
    pub handle_height: f64,
    pub checked_content_left: f64,
    pub unchecked_content_left: f64,
    pub content_width: f64,
}

impl SwitchStyle {
    /// Resolve affine offsets without knowing the eventual parent layout width.
    ///
    /// `position` is 0 for unchecked and 1 for checked. `press` is 0 at rest and
    /// 1 while held. Intermediate values follow the same geometry continuously.
    pub fn layout(&self, position: f64, press: f64, direction: Direction) -> SwitchLayout {
        let position = progress(position);
        let press = progress(press);
        let sign = f64::from(direction.forward_sign());
        let handle_travel_inset = self.handle_size + self.track_padding * 2.;
        let checked_fraction = position - 1.;
        let unchecked_fraction = position;
        let pressed_shift = self.active_content_offset * press;
        let checked_slide = -handle_travel_inset * checked_fraction - pressed_shift * position;
        let unchecked_slide =
            -handle_travel_inset * unchecked_fraction + pressed_shift * (1. - position);
        let checked_slot_left = SwitchOffset {
            fraction: checked_fraction * sign,
            pixels: checked_slide * sign,
        };
        let unchecked_slot_left = SwitchOffset {
            fraction: unchecked_fraction * sign,
            pixels: unchecked_slide * sign,
        };
        let (checked_padding_left, checked_padding_right) =
            direction.horizontal(self.inner_min_margin, self.inner_max_margin);
        let (unchecked_padding_left, unchecked_padding_right) =
            direction.horizontal(self.inner_max_margin, self.inner_min_margin);
        let handle_left = match direction {
            Direction::Ltr => SwitchOffset {
                fraction: position,
                pixels: self.track_padding - handle_travel_inset * position,
            },
            Direction::Rtl => SwitchOffset {
                fraction: 1. - position,
                pixels: -self.handle_size - self.track_padding + handle_travel_inset * position,
            },
        };
        let extension = self.handle_size * 0.3 * press;
        let (handle_extension_left, handle_extension_right) =
            direction.horizontal(extension * position, extension * (1. - position));
        SwitchLayout {
            handle_left,
            checked_slot_left,
            unchecked_slot_left,
            checked_content_left: SwitchOffset {
                pixels: checked_slot_left.pixels + checked_padding_left,
                ..checked_slot_left
            },
            unchecked_content_left: SwitchOffset {
                pixels: unchecked_slot_left.pixels + unchecked_padding_left,
                ..unchecked_slot_left
            },
            checked_padding_left,
            checked_padding_right,
            unchecked_padding_left,
            unchecked_padding_right,
            handle_extension_left,
            handle_extension_right,
            content_inset: self.inner_min_margin + self.inner_max_margin,
        }
    }

    /// Resolve the exact painted geometry after the track width is known.
    pub fn geometry(
        &self,
        width: f64,
        position: f64,
        press: f64,
        direction: Direction,
    ) -> SwitchGeometry {
        let width = if width.is_finite() { width.max(0.) } else { 0. };
        let layout = self.layout(position, press, direction);
        let indicator_left = layout.handle_left.resolve(width);
        SwitchGeometry {
            indicator_left,
            indicator_top: self.track_padding,
            indicator_size: self.handle_size,
            handle_left: indicator_left - layout.handle_extension_left,
            handle_top: self.track_padding,
            handle_width: self.handle_size
                + layout.handle_extension_left
                + layout.handle_extension_right,
            handle_height: self.handle_size,
            checked_content_left: layout.checked_content_left.resolve(width),
            unchecked_content_left: layout.unchecked_content_left.resolve(width),
            content_width: (width - layout.content_inset).max(0.),
        }
    }
}

fn progress(value: f64) -> f64 {
    if value.is_finite() {
        value.clamp(0., 1.)
    } else {
        0.
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{Size, theme::ResolvedTheme};

    #[test]
    fn switch_geometry_mirrors_continuously_including_a_pressed_handle() {
        let style =
            SwitchStyle::resolve(&ResolvedTheme::default(), Size::Medium, false, false, false);
        for width in [44., 71.25, 256.] {
            for position in [0., 0.125, 0.5, 0.8, 1.] {
                for press in [0., 0.4, 1.] {
                    let ltr = style.geometry(width, position, press, Direction::Ltr);
                    let rtl = style.geometry(width, position, press, Direction::Rtl);
                    let near = |a: f64, b: f64| assert!((a - b).abs() < 1e-10, "{a} != {b}");
                    near(ltr.handle_left + rtl.handle_left + ltr.handle_width, width);
                    near(
                        ltr.indicator_left + rtl.indicator_left + ltr.indicator_size,
                        width,
                    );
                    near(
                        ltr.checked_content_left + rtl.checked_content_left + ltr.content_width,
                        width,
                    );
                    near(
                        ltr.unchecked_content_left + rtl.unchecked_content_left + ltr.content_width,
                        width,
                    );
                    near(ltr.handle_width, style.handle_size * (1. + 0.3 * press));
                }
            }
        }
    }

    #[test]
    fn switch_geometry_keeps_negative_padding_and_clamps_invalid_progress() {
        let mut style =
            SwitchStyle::resolve(&ResolvedTheme::default(), Size::Small, false, false, false);
        style.track_padding = -3.;
        style.handle_size = 20.;
        let start = style.geometry(32., 0., 0., Direction::Ltr);
        assert_eq!(start.indicator_left, -3.);
        assert_eq!(start.handle_top, -3.);
        let end = style.geometry(32., 1., 0., Direction::Ltr);
        assert_eq!(end.indicator_left, 15.);
        assert_eq!(
            style.layout(f64::NAN, f64::INFINITY, Direction::Ltr),
            style.layout(0., 0., Direction::Ltr)
        );
        assert_eq!(
            style.layout(2., -1., Direction::Rtl),
            style.layout(1., 0., Direction::Rtl)
        );
    }
}
