// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Shared native popup placement, lifecycle and window coordination.

pub(crate) mod component;
mod config;
mod element;
mod geometry;
mod host;
mod lifecycle;
mod motion;
mod runtime;
mod shadow;
mod shape;
mod trigger;
pub use shadow::{PopupShadowStatistics, popup_shadow_statistics};
pub use trigger::{PopupTrigger, PopupTriggerElement};

pub use component::PopupColor;
pub use config::{PopupConfig, PopupConfigPatch, TooltipConfig, TooltipConfigPatch};
pub use geometry::{PopupGeometry, PopupLayout, PopupPlacement};
pub use host::PopupHost;
pub use runtime::{Popup, PopupHandle, PopupSession};

/// Size the popup from its anchor in the same layout pass. No measurement
/// notification or second frame is required when the trigger resizes.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub enum PopupWidth {
    #[default]
    Content,
    Trigger,
    AtLeastTrigger,
    /// An explicit width, clamped to at least the trigger width in the same pass.
    TriggerMinimum(gpui::Pixels),
    Fixed(gpui::Pixels),
}

#[cfg(test)]
mod component_tests;
#[cfg(test)]
mod exit_tests;
#[cfg(test)]
mod placement_tests;
#[cfg(test)]
mod reference_tests;
#[cfg(test)]
mod style_tests;
#[cfg(test)]
mod tests;
#[cfg(test)]
mod unique_tests;

/// Arrow visibility and alignment. Edge-aligned arrows normally retain their
/// token-defined inset; `point_at_center` aligns that inset to the trigger center.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PopupArrow {
    pub visible: bool,
    pub point_at_center: bool,
}

impl Default for PopupArrow {
    fn default() -> Self {
        Self {
            visible: true,
            point_at_center: false,
        }
    }
}

impl From<bool> for PopupArrow {
    fn from(visible: bool) -> Self {
        Self {
            visible,
            ..Self::default()
        }
    }
}

/// Independent trigger modes. Combine modes with `|`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct PopupTriggers(u8);

impl PopupTriggers {
    pub const NONE: Self = Self(0);
    pub const HOVER: Self = Self(1);
    pub const FOCUS: Self = Self(2);
    pub const CLICK: Self = Self(4);
    pub const CONTEXT_MENU: Self = Self(8);
    pub const HOVER_FOCUS: Self = Self(3);

    pub fn contains(self, trigger: Self) -> bool {
        self.0 & trigger.0 == trigger.0
    }
}

impl std::ops::BitOr for PopupTriggers {
    type Output = Self;
    fn bitor(self, other: Self) -> Self {
        Self(self.0 | other.0)
    }
}

/// Why an interaction requested a visibility change. Outside dismissal is not
/// a Cancel-button activation and never calls a confirmation's cancel callback.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PopupOpenReason {
    Hover,
    Focus,
    Click,
    ContextMenu,
    Escape,
    OutsidePress,
    Confirm,
    Cancel,
    ParentClosed,
    UniqueReplaced,
    Programmatic,
    Accessibility,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PopupOpenChange {
    pub open: bool,
    pub reason: PopupOpenReason,
}
