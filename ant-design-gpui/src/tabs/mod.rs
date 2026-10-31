// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Keyed native tabs with retained selection, focus, pane lifetime and overflow.

mod geometry;
mod measure;
mod model;
mod state;
mod style;
mod view;

pub use model::{TabItem, TabPlacement, TabsAnimation, TabsItems, TabsItemsError, TabsType};
pub use state::{TabsChange, TabsEdit, TabsEvent, TabsSource, TabsState};
pub use style::{TabsSpacing, TabsTheme, TabsTokenPatch, TabsTokens};
pub use view::{
    Tabs, TabsIndicator, TabsIndicatorAlign, TabsPopupRender, TabsStyleContext, TabsStyles,
    TabsTabRender,
};

#[cfg(test)]
mod reference_tests;
#[cfg(test)]
mod tests;
