// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Retained, themed context menus with native GPUI popup ownership.
mod model;
mod state;
mod style;
mod trigger;
mod view;

pub use model::{MenuCheck, MenuEntry, MenuItem, MenuItems, MenuItemsError};
pub use state::{ContextMenuState, MenuActivation, MenuActivationSource, MenuEvent};
pub use style::{ContextMenuTheme, ContextMenuTokenPatch, ContextMenuTokens};
pub use view::ContextMenu;

#[cfg(test)]
mod tests;
