// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::PopupHandle;
use crate::theme::{AliasTokens, Color, PresetColor};
use gpui::{AnyElement, App, IntoElement, SharedString, Window};
use std::rc::Rc;

/// A palette color or an arbitrary sRGB surface color.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum PopupColor {
    Preset(PresetColor),
    Custom(Color),
}
impl From<PresetColor> for PopupColor {
    fn from(value: PresetColor) -> Self {
        Self::Preset(value)
    }
}
impl From<Color> for PopupColor {
    fn from(value: Color) -> Self {
        Self::Custom(value)
    }
}
impl PopupColor {
    pub(crate) fn resolve(self, tokens: &AliasTokens) -> Color {
        match self {
            Self::Preset(color) => tokens.palettes.get(color).tones[5],
            Self::Custom(color) => color,
        }
    }
}

type ContentFactory = Rc<dyn Fn(&PopupHandle, &mut Window, &mut App) -> AnyElement>;

#[derive(Clone)]
pub(crate) struct Slot {
    pub text: Option<SharedString>,
    factory: ContentFactory,
}
impl Slot {
    pub fn text(text: impl Into<SharedString>) -> Option<Self> {
        let text = text.into();
        if text.is_empty() {
            return None;
        }
        let label = text.clone();
        Some(Self {
            text: Some(text),
            factory: Rc::new(move |_, _, _| label.clone().into_any_element()),
        })
    }
    pub fn content<E: IntoElement>(
        factory: impl Fn(&PopupHandle, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        Self {
            text: None,
            factory: Rc::new(move |handle, window, cx| {
                factory(handle, window, cx).into_any_element()
            }),
        }
    }
    pub fn build(&self, handle: &PopupHandle, window: &mut Window, cx: &mut App) -> AnyElement {
        (self.factory)(handle, window, cx)
    }
}

// Keep all three convenience controls on the same interaction and layout API.
// Each method is ordinary public Rust code after expansion; no boxed builder chain.
macro_rules! popup_methods {
    () => {
        pub fn scope(mut self, scope: crate::ConfigScope) -> Self {
            self.popup = self.popup.scope(scope);
            self
        }
        pub fn child(mut self, child: impl gpui::IntoElement) -> Self {
            self.popup = self.popup.child(child);
            self
        }
        pub fn open(mut self, open: bool) -> Self {
            self.popup = self.popup.open(open);
            self
        }
        pub fn default_open(mut self, open: bool) -> Self {
            self.popup = self.popup.default_open(open);
            self
        }
        pub fn fresh(mut self, fresh: bool) -> Self {
            self.popup = self.popup.fresh(fresh);
            self
        }
        pub fn destroy_on_hidden(mut self, destroy: bool) -> Self {
            self.popup = self.popup.destroy_on_hidden(destroy);
            self
        }
        pub fn handle(mut self, handle: &crate::popup::PopupHandle) -> Self {
            self.popup = self.popup.handle(handle);
            self
        }
        pub fn on_open_change(
            mut self,
            callback: impl Fn(&crate::popup::PopupOpenChange, &mut gpui::Window, &mut gpui::App)
            + 'static,
        ) -> Self {
            self.popup = self.popup.on_open_change(callback);
            self
        }
        pub fn placement(mut self, placement: crate::popup::PopupPlacement) -> Self {
            self.popup = self.popup.placement(placement);
            self
        }
        pub fn on_popup_click(
            mut self,
            callback: impl Fn(&gpui::ClickEvent, &mut gpui::Window, &mut gpui::App) + 'static,
        ) -> Self {
            self.popup = self.popup.on_popup_click(callback);
            self
        }
        pub fn triggers(mut self, triggers: crate::popup::PopupTriggers) -> Self {
            self.popup = self.popup.triggers(triggers);
            self
        }
        pub fn arrow(mut self, arrow: impl Into<crate::popup::PopupArrow>) -> Self {
            self.popup = self.popup.arrow(arrow);
            self
        }
        pub fn auto_adjust_overflow(mut self, adjust: bool) -> Self {
            self.popup = self.popup.auto_adjust_overflow(adjust);
            self
        }
        pub fn gap(mut self, gap: gpui::Pixels) -> Self {
            self.popup = self.popup.gap(gap);
            self
        }
        pub fn offset(mut self, offset: gpui::Point<gpui::Pixels>) -> Self {
            self.popup = self.popup.offset(offset);
            self
        }
        pub fn z_index(mut self, z_index: usize) -> Self {
            self.popup = self.popup.z_index(z_index);
            self
        }
        pub fn mouse_enter_delay(mut self, delay: std::time::Duration) -> Self {
            self.popup = self.popup.mouse_enter_delay(delay);
            self
        }
        pub fn mouse_leave_delay(mut self, delay: std::time::Duration) -> Self {
            self.popup = self.popup.mouse_leave_delay(delay);
            self
        }
        pub fn disabled(mut self, disabled: bool) -> Self {
            self.popup = self.popup.disabled(disabled);
            self
        }
        /// Refine the padded surface. Title/content slots have separate refinements.
        pub fn surface_style(mut self, style: gpui::StyleRefinement) -> Self {
            self.popup = self.popup.surface_style(style);
            self
        }
        pub fn accessibility_label(mut self, label: impl Into<gpui::SharedString>) -> Self {
            self.popup = self.popup.accessibility_label(label);
            self
        }
        pub fn automation_id(mut self, id: impl Into<gpui::SharedString>) -> Self {
            self.popup = self.popup.automation_id(id);
            self
        }
        pub fn accessibility_description(
            mut self,
            description: impl Into<gpui::SharedString>,
        ) -> Self {
            self.popup = self.popup.accessibility_description(description);
            self
        }
    };
}
pub(crate) use popup_methods;
