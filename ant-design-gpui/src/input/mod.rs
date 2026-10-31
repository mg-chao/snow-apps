// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native input controls and retained editing state.

mod accessibility;
mod action_view;
pub mod actions;
mod controls;
pub mod editing;
pub mod geometry;
pub mod group;
mod layout;
pub(crate) mod motion;
pub mod otp;
mod state;
mod style;
mod view;

pub use controls::{CountConfig, CountInfo, PasswordInput, SearchInput, TextArea};
pub use editing::{CountStrategy, EditOrigin, HistoryLimit, TextSelection};
pub use group::{InputGroup, InputGroupContext, InputGroupItem, InputGroupOrientation};
pub use otp::{OtpChange, OtpCommit, OtpEvent, OtpInput, OtpOptions, OtpState};
pub use state::{
    CommitReason, InputChange, InputCommit, InputEvent, InputSearch, InputState, SearchSource,
};
pub use style::{InputInteraction, InputStyle, InputTheme, InputTokenPatch, InputTokens};
pub use view::Input;
pub(crate) use view::InputFrame;

#[cfg(test)]
mod tests;
