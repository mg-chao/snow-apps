// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
use super::{ModalButtonOptions, ModalStyles};

#[derive(Clone, Debug, Default, PartialEq)]
pub struct ModalConfig {
    pub centered: bool,
    pub mask: Option<bool>,
    pub mask_closable: Option<bool>,
    pub focus_trap: Option<bool>,
    pub focus_return: Option<bool>,
    pub ok_button: ModalButtonOptions,
    pub cancel_button: ModalButtonOptions,
    pub styles: ModalStyles,
}
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ModalConfigPatch {
    pub centered: Option<bool>,
    pub mask: Option<bool>,
    pub mask_closable: Option<bool>,
    pub focus_trap: Option<bool>,
    pub focus_return: Option<bool>,
    pub ok_button: Option<ModalButtonOptions>,
    pub cancel_button: Option<ModalButtonOptions>,
    pub styles: ModalStyles,
}
impl ModalConfigPatch {
    pub fn resolve(&self, parent: &ModalConfig) -> ModalConfig {
        let mut styles = parent.styles.clone();
        styles.refine(&self.styles);
        ModalConfig {
            centered: self.centered.unwrap_or(parent.centered),
            mask: self.mask.or(parent.mask),
            mask_closable: self.mask_closable.or(parent.mask_closable),
            focus_trap: self.focus_trap.or(parent.focus_trap),
            focus_return: self.focus_return.or(parent.focus_return),
            ok_button: self.ok_button.as_ref().map_or_else(
                || parent.ok_button.clone(),
                |p| p.resolve(&parent.ok_button),
            ),
            cancel_button: self.cancel_button.as_ref().map_or_else(
                || parent.cancel_button.clone(),
                |p| p.resolve(&parent.cancel_button),
            ),
            styles,
        }
    }
}
