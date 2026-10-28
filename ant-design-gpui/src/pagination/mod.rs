// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native pagination with a retained controller and bounded page generation.

mod bidi;
mod geometry;
mod model;
mod state;
mod style;
mod text;
mod view;

pub use model::{
    PaginationItem, PaginationItemKind, PaginationMode, PaginationSettings, QuickJumper,
};
pub use state::{
    PaginationChange, PaginationChangeSource, PaginationEvent, PaginationSizeChange,
    PaginationState,
};
pub use style::{PaginationMetrics, PaginationTheme, PaginationTokenPatch, PaginationTokens};
pub use view::{
    Pagination, PaginationAlign, PaginationItemRender, PaginationSizeChangerRender,
    PaginationStyleContext, PaginationStyles,
};

#[cfg(test)]
mod interaction_tests;
#[cfg(test)]
mod tests;
