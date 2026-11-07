// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native numeric selection on a continuous rail with discrete values.

mod model;
mod state;
mod style;
mod view;
pub use model::{
    SliderDisabled, SliderError, SliderOptions, SliderOrientation, SliderRange, SliderScale,
    SliderValue,
};
pub use state::{SliderChange, SliderChangeSource, SliderEvent, SliderState};
pub use style::{SliderTheme, SliderTokenPatch, SliderTokens};
pub use view::{Slider, SliderMark, SliderStyleContext, SliderStyles, SliderTooltip};

#[cfg(test)]
mod interaction_tests;
#[cfg(test)]
mod tests;
