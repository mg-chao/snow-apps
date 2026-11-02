// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native form layout, lossless field values, validation and retained control bindings.

mod bindings;
mod codecs;
mod config;
mod element;
mod list;
mod model;
mod motion;
mod provider;
mod rules;
mod state;
mod style;
mod value;
mod view;

pub(crate) use bindings::NativeActivation;
pub(crate) use bindings::binding_error;
pub use bindings::{FormControl, FormControlBinding, FormControlEvent};
pub use config::{FormConfig, FormConfigPatch, FormStyleResolver};
pub use list::*;
pub use model::*;
pub use provider::*;
pub use rules::*;
pub use state::*;
pub use style::{FormStyle, FormTheme, FormTokenPatch, FormTokens};
pub use value::*;
pub use view::{
    Form, FormColumn, FormColumnSize, FormErrorList, FormFeedback, FormItem, FormItemContext,
    FormItemStyles, FormLabelAlign, FormLayout, FormRequiredMark, FormStyleContext, FormStyles,
};

#[cfg(test)]
mod binding_tests;
#[cfg(test)]
mod scroll_tests;
#[cfg(test)]
mod tests;
#[cfg(test)]
mod view_tests;
