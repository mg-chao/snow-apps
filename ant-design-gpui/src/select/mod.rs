// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Typed selection data and native select controls.

mod accessibility;
mod empty;
mod model;
mod options;
mod overflow;
mod state;
mod style;
mod view;
pub use model::{SelectMode, SelectSettings, SelectTagFactory, SelectTokenizer, SelectValue};
pub use options::{
    SelectEntry, SelectGroup, SelectMatches, SelectOption, SelectOptions, SelectOptionsError,
    SelectRow, SelectSearch, SelectSearchField,
};
pub use state::{SelectChange, SelectChangeSource, SelectEvent, SelectState};
pub use style::{SelectTheme, SelectTokenPatch, SelectTokens};
pub use view::{
    Select, SelectListLayout, SelectOptionRender, SelectStyleContext, SelectStyles, SelectTagRender,
};

#[cfg(test)]
mod interaction_tests;
#[cfg(test)]
mod tests;
