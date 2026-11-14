// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Segmented verification codes with native IME cells and aggregate history.

mod model;
mod state;
#[cfg(test)]
mod tests;
mod view;
pub use model::{MAX_OTP_LENGTH, OtpError, OtpFilter, OtpFormatter, OtpOptions};
pub use state::{OtpChange, OtpCommit, OtpEvent, OtpState};
pub use view::{OtpInput, OtpTheme, OtpTokenPatch, OtpTokens};
