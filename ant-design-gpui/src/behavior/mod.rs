// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Reusable interaction, semantics, focus and motion primitives for native controls.

mod accessibility;
mod activation;
mod focus;
mod motion;
pub(crate) mod paragraph;
pub(crate) mod text;
mod wave;

pub use accessibility::{Accessibility, SemanticElement};
pub(crate) use activation::WeakActivationGuard;
pub use activation::{Activation, ActivationGuard, Availability};
pub use focus::{
    FocusNext, FocusPrevious, FocusRing, FocusRingElement, focus_handle, initialize, traversal,
};
pub use motion::{MotionPolicy, ScalarTransition, TransitionSample, evaluate_cubic_bezier};
pub use text::paragraph_text;
pub(crate) use wave::{WaveMotion, WaveSample, WaveState, wave_color};

#[cfg(test)]
mod tests;
