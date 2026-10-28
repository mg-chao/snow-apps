// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Persistent navigation menus with native GPUI layout, focus and popup ownership.
//!
//! Retain one [`MenuState`] per mounted menu and build a fresh [`Menu`] during
//! render. Item keys are independent of labels and unique across the tree.
//! Command menus remain available separately in [`crate::context_menu`].

mod model;
mod overflow;
mod state;
mod style;
mod view;

pub use model::{
    MenuColorTheme, MenuContentRenderer, MenuDivider, MenuEntry, MenuGroup, MenuItem, MenuItems,
    MenuItemsError, MenuKey, MenuMode, MenuOptions, MenuPopupContext, MenuPopupRenderer,
    MenuRenderContext, MenuTrigger, SubMenu,
};
pub use state::{MenuActivation, MenuEvent, MenuSelection, MenuSource, MenuState};
pub use style::{MenuRowStyle, MenuStyle, MenuTheme, MenuTokenPatch, MenuTokens, MenuVisualState};
pub use view::{Menu, MenuStyleContext, MenuStyles};

#[cfg(test)]
mod interaction_tests;
#[cfg(test)]
mod tests;
