// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Retained native carousels with keyed content, bounded slide rendering and cancellable motion.

mod config;
mod controls;
mod element;
mod model;
mod state;
mod style;
mod view;

pub use config::{CarouselConfig, CarouselConfigPatch, CarouselStyleResolver};
pub use model::{
    CarouselEffect, CarouselItems, CarouselItemsError, CarouselOptions, CarouselSlide, DotPlacement,
};
pub use state::{CarouselChange, CarouselEvent, CarouselSource, CarouselState};
pub use style::{
    CarouselStyleContext, CarouselStyles, CarouselTheme, CarouselTokenPatch, CarouselTokens,
};
pub use view::{Carousel, CarouselArrowContext, CarouselDotContext};

#[cfg(test)]
mod integration_tests;
#[cfg(test)]
mod tests;
