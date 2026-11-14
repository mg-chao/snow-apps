// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{fmt, sync::Arc};

use gpui::{App, Window};

use super::view::{
    FormColumn, FormLabelAlign, FormLayout, FormRequiredMark, FormStyleContext, FormStyles,
};

type StylesFn = dyn Fn(&FormStyleContext, &mut Window, &mut App) -> FormStyles + Send + Sync;

#[derive(Clone)]
pub struct FormStyleResolver(Arc<StylesFn>);
impl FormStyleResolver {
    pub fn new(
        resolver: impl Fn(&FormStyleContext, &mut Window, &mut App) -> FormStyles
        + Send
        + Sync
        + 'static,
    ) -> Self {
        Self(Arc::new(resolver))
    }
    pub fn resolve(
        &self,
        context: &FormStyleContext,
        window: &mut Window,
        cx: &mut App,
    ) -> FormStyles {
        (self.0)(context, window, cx)
    }
}
impl PartialEq for FormStyleResolver {
    fn eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }
}
impl Eq for FormStyleResolver {}
impl fmt::Debug for FormStyleResolver {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_tuple("FormStyleResolver")
            .field(&Arc::as_ptr(&self.0))
            .finish()
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct FormConfig {
    pub layout: FormLayout,
    pub label_col: Option<FormColumn>,
    pub wrapper_col: Option<FormColumn>,
    pub label_align: FormLabelAlign,
    pub label_wrap: bool,
    pub colon: bool,
    pub required_mark: FormRequiredMark,
    pub styles: FormStyles,
    pub styles_fn: Option<FormStyleResolver>,
}
impl Default for FormConfig {
    fn default() -> Self {
        Self {
            layout: FormLayout::Horizontal,
            label_col: None,
            wrapper_col: None,
            label_align: FormLabelAlign::Right,
            label_wrap: false,
            colon: true,
            required_mark: FormRequiredMark::Required,
            styles: FormStyles::default(),
            styles_fn: None,
        }
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct FormConfigPatch {
    pub layout: Option<FormLayout>,
    pub label_col: Option<FormColumn>,
    pub wrapper_col: Option<FormColumn>,
    pub label_align: Option<FormLabelAlign>,
    pub label_wrap: Option<bool>,
    pub colon: Option<bool>,
    pub required_mark: Option<FormRequiredMark>,
    pub styles: FormStyles,
    pub styles_fn: Option<FormStyleResolver>,
}
impl FormConfigPatch {
    pub fn resolve(&self, parent: &FormConfig) -> FormConfig {
        let mut styles = parent.styles.clone();
        styles.refine(&self.styles);
        FormConfig {
            layout: self.layout.unwrap_or(parent.layout),
            label_col: self.label_col.clone().or_else(|| parent.label_col.clone()),
            wrapper_col: self
                .wrapper_col
                .clone()
                .or_else(|| parent.wrapper_col.clone()),
            label_align: self.label_align.unwrap_or(parent.label_align),
            label_wrap: self.label_wrap.unwrap_or(parent.label_wrap),
            colon: self.colon.unwrap_or(parent.colon),
            required_mark: self.required_mark.unwrap_or(parent.required_mark),
            styles,
            styles_fn: self.styles_fn.clone().or_else(|| parent.styles_fn.clone()),
        }
    }
}
