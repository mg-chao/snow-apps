// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Retained native color editing, including solid colors and linear gradients.

mod anchor;
mod color;
mod config;
mod model;
mod motion;
mod paint;
mod state;
mod style;
mod text;
mod view;

pub use color::{
    ColorPickerError, ColorPickerFormat, ColorPickerMode, ColorPickerValue, ColorStop, PickerColor,
};
pub use config::{ColorPickerConfig, ColorPickerConfigPatch, ColorPickerStyleResolver};
pub use state::{
    ColorPickerChange, ColorPickerChangeSource, ColorPickerEvent, ColorPickerGeometry,
    ColorPickerHandleGeometry, ColorPickerState,
};
pub use style::{
    ColorPickerStyleContext, ColorPickerStyles, ColorPickerTheme, ColorPickerTokenPatch,
    ColorPickerTokens, ColorPickerTriggerMetrics,
};
pub use view::{ColorPicker, ColorPickerPanel, ColorPickerPanelParts, ColorPickerPreset};

#[cfg(test)]
mod interaction_tests;
#[cfg(test)]
mod reference_tests;
