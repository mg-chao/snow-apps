// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{PopupArrow, PopupTriggers};
use std::time::Duration;

/// Inheritable common defaults. Each component owns its own configuration.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PopupConfig {
    pub triggers: PopupTriggers,
    pub arrow: PopupArrow,
    pub mouse_enter_delay: Duration,
    pub mouse_leave_delay: Duration,
}

impl Default for PopupConfig {
    fn default() -> Self {
        Self {
            triggers: PopupTriggers::HOVER_FOCUS,
            arrow: PopupArrow::default(),
            mouse_enter_delay: Duration::from_millis(100),
            mouse_leave_delay: Duration::from_millis(100),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PopupConfigPatch {
    pub triggers: Option<PopupTriggers>,
    pub arrow: Option<PopupArrow>,
    pub mouse_enter_delay: Option<Duration>,
    pub mouse_leave_delay: Option<Duration>,
}

impl PopupConfigPatch {
    pub(crate) fn resolve(&self, parent: PopupConfig) -> PopupConfig {
        PopupConfig {
            triggers: self.triggers.unwrap_or(parent.triggers),
            arrow: self.arrow.unwrap_or(parent.arrow),
            mouse_enter_delay: self.mouse_enter_delay.unwrap_or(parent.mouse_enter_delay),
            mouse_leave_delay: self.mouse_leave_delay.unwrap_or(parent.mouse_leave_delay),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TooltipConfig {
    pub popup: PopupConfig,
    pub unique: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TooltipConfigPatch {
    pub popup: PopupConfigPatch,
    pub unique: Option<bool>,
}

impl TooltipConfigPatch {
    pub(crate) fn resolve(&self, parent: TooltipConfig) -> TooltipConfig {
        TooltipConfig {
            popup: self.popup.resolve(parent.popup),
            unique: self.unique.unwrap_or(parent.unique),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{ConfigPatch, ConfigScope};
    use gpui::TestAppContext;

    #[gpui::test]
    fn popup_scopes_inherit_fields_without_leaking_between_components(cx: &mut TestAppContext) {
        cx.update(|cx| {
            let root = ConfigScope::new_window(cx);
            root.set_patch(
                ConfigPatch {
                    tooltip: TooltipConfigPatch {
                        unique: Some(true),
                        popup: PopupConfigPatch {
                            triggers: Some(PopupTriggers::CLICK),
                            mouse_enter_delay: Some(Duration::from_millis(250)),
                            ..Default::default()
                        },
                    },
                    ..Default::default()
                },
                cx,
            )
            .unwrap();
            let child = root
                .child(
                    ConfigPatch {
                        tooltip: TooltipConfigPatch {
                            popup: PopupConfigPatch {
                                arrow: Some(false.into()),
                                ..Default::default()
                            },
                            ..Default::default()
                        },
                        ..Default::default()
                    },
                    cx,
                )
                .unwrap();
            let config = child.snapshot(cx);
            assert_eq!(config.tooltip.popup.triggers, PopupTriggers::CLICK);
            assert_eq!(
                config.tooltip.popup.mouse_enter_delay,
                Duration::from_millis(250)
            );
            assert!(!config.tooltip.popup.arrow.visible);
            assert!(config.tooltip.unique);
            assert_eq!(config.popover, PopupConfig::default());
            assert_eq!(config.popconfirm.triggers, PopupTriggers::CLICK);
            assert!(config.popconfirm.arrow.visible);
            child
                .update_patch(cx, |patch| patch.tooltip.unique = Some(false))
                .unwrap();
            assert!(!child.snapshot(cx).tooltip.unique);
            assert!(root.snapshot(cx).tooltip.unique);
        });
    }

    #[gpui::test]
    fn popup_default_updates_retain_expensive_theme_and_locale_snapshots(cx: &mut TestAppContext) {
        cx.update(|cx| {
            let scope = ConfigScope::new_window(cx);
            let old = scope.snapshot(cx);
            scope
                .update_patch(cx, |patch| {
                    patch.popover.mouse_leave_delay = Some(Duration::ZERO);
                })
                .unwrap();
            let new = scope.snapshot(cx);
            assert_eq!(new.popover.mouse_leave_delay, Duration::ZERO);
            assert!(std::sync::Arc::ptr_eq(&old.theme, &new.theme));
            assert!(std::sync::Arc::ptr_eq(&old.locale, &new.locale));
            assert_ne!(old.popover, new.popover);
        });
    }
}
