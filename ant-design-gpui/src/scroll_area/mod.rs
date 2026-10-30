// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native GPUI scrolling with proportional, themed overlay scrollbars.
//!
//! The retained state owns a single [`gpui::ScrollHandle`]. Content, scrollbar
//! dragging, keyboard navigation, anchors and application updates all use it.

mod external;
mod geometry;
mod state;
mod style;
mod view;

pub(crate) use external::{ExternalScrollbar, ExternalScrollbarGeometry};
pub use state::{ScrollAreaEvent, ScrollAreaState};
pub use style::{ScrollAreaTheme, ScrollAreaTokenPatch, ScrollAreaTokens};
pub use view::ScrollArea;

/// Scrollable physical axes. A vertical area fits its content to the viewport width.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ScrollAxis {
    #[default]
    Vertical,
    Horizontal,
    Both,
}

impl ScrollAxis {
    pub(crate) fn horizontal(self) -> bool {
        self != Self::Vertical
    }

    pub(crate) fn vertical(self) -> bool {
        self != Self::Horizontal
    }
}

/// Visibility never changes viewport size or the ability to scroll.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ScrollbarVisibility {
    /// A slim thumb is always visible when content overflows; hover expands it.
    #[default]
    Always,
    /// Show while the area is hovered, focused, or a thumb is being dragged.
    Hover,
    /// Keep native scrolling and keyboard navigation without painting scrollbars.
    Hidden,
}

#[cfg(test)]
mod tests;
