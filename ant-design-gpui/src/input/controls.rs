// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::rc::Rc;

use gpui::{
    App, ElementId, Entity, IntoElement, RenderOnce, SharedString, StyleRefinement, Styled, Window,
};

use crate::{ConfigScope, Size, ValidationStatus, Variant};

use super::{Input, InputState, state::InputMode};

type CountFormatter = Rc<dyn Fn(CountInfo<'_>) -> SharedString>;

/// Counts use the state's strategy. This maximum is advisory; `InputState::set_max_length`
/// is the hard editing limit. Supplying an external over-limit value never truncates it.
#[derive(Clone, Default)]
pub struct CountConfig {
    pub maximum: Option<usize>,
    pub(crate) formatter: Option<CountFormatter>,
}

#[derive(Clone, Copy, Debug)]
pub struct CountInfo<'a> {
    pub value: &'a str,
    pub count: usize,
    pub maximum: Option<usize>,
}

impl CountConfig {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn maximum(mut self, maximum: usize) -> Self {
        self.maximum = Some(maximum);
        self
    }

    pub fn formatter(
        mut self,
        formatter: impl Fn(CountInfo<'_>) -> SharedString + 'static,
    ) -> Self {
        self.formatter = Some(Rc::new(formatter));
        self
    }

    pub(crate) fn format(&self, info: CountInfo<'_>) -> SharedString {
        if let Some(formatter) = &self.formatter {
            return formatter(info);
        }
        match info.maximum {
            Some(maximum) => format!("{} / {maximum}", info.count).into(),
            None => info.count.to_string().into(),
        }
    }
}

#[derive(Clone, Debug, Default)]
pub(crate) enum SearchButton {
    #[default]
    Icon,
    PrimaryIcon,
    Label(SharedString),
}

macro_rules! input_control {
    ($name:ident, $documentation:literal) => {
        #[doc = $documentation]
        #[derive(IntoElement)]
        pub struct $name {
            input: Input,
        }

        impl $name {
            /// Configure any shared input property without introducing another state owner.
            pub fn configure(mut self, configure: impl FnOnce(Input) -> Input) -> Self {
                self.input = configure(self.input);
                self
            }
            pub fn scope(self, value: ConfigScope) -> Self {
                self.configure(|input| input.scope(value))
            }
            pub fn size(self, value: Size) -> Self {
                self.configure(|input| input.size(value))
            }
            pub fn variant(self, value: Variant) -> Self {
                self.configure(|input| input.variant(value))
            }
            pub fn status(self, value: ValidationStatus) -> Self {
                self.configure(|input| input.status(value))
            }
            pub fn disabled(self, value: bool) -> Self {
                self.configure(|input| input.disabled(value))
            }
            pub fn read_only(self, value: bool) -> Self {
                self.configure(|input| input.read_only(value))
            }
            pub fn placeholder(self, value: impl Into<SharedString>) -> Self {
                self.configure(|input| input.placeholder(value))
            }
            pub fn allow_clear(self, value: bool) -> Self {
                self.configure(|input| input.allow_clear(value))
            }
            pub fn show_count(self, value: bool) -> Self {
                self.configure(|input| input.show_count(value))
            }
            pub fn count(self, value: CountConfig) -> Self {
                self.configure(|input| input.count(value))
            }
            pub fn prefix(self, value: impl IntoElement) -> Self {
                self.configure(|input| input.prefix(value))
            }
            pub fn suffix(self, value: impl IntoElement) -> Self {
                self.configure(|input| input.suffix(value))
            }
            pub fn accessibility_label(self, value: impl Into<SharedString>) -> Self {
                self.configure(|input| input.accessibility_label(value))
            }
            pub fn accessibility_description(self, value: impl Into<SharedString>) -> Self {
                self.configure(|input| input.accessibility_description(value))
            }
            pub fn automation_id(self, value: impl Into<SharedString>) -> Self {
                self.configure(|input| input.automation_id(value))
            }
        }
        impl Styled for $name {
            fn style(&mut self) -> &mut StyleRefinement {
                self.input.style()
            }
        }
        impl super::InputGroupItem for $name {
            fn into_group_element(mut self, context: super::InputGroupContext) -> gpui::AnyElement {
                self.input = self.input.in_group(context);
                self.into_any_element()
            }
        }
        impl RenderOnce for $name {
            fn render(self, _: &mut Window, _: &mut App) -> impl IntoElement {
                self.input
            }
        }
    };
}

input_control!(
    TextArea,
    "A multiline native editor with optional content-driven height."
);
input_control!(
    PasswordInput,
    "A password editor with retained visibility, protected export, and a keyboard-accessible reveal action."
);
input_control!(
    SearchInput,
    "A native search editor. Subscribe to InputEvent::Search on the retained InputState."
);

impl TextArea {
    pub fn new(id: impl Into<ElementId>, state: &Entity<InputState>) -> Self {
        Self {
            input: Input::new(id, state).multiline(true).rows(3),
        }
    }
    pub fn rows(self, rows: usize) -> Self {
        self.configure(|input| input.rows(rows))
    }
    pub fn auto_size(self, min_rows: usize, max_rows: usize) -> Self {
        self.configure(|input| input.auto_size(min_rows, max_rows))
    }
}

impl PasswordInput {
    pub fn new(id: impl Into<ElementId>, state: &Entity<InputState>) -> Self {
        let mut input = Input::new(id, state);
        input.mode = InputMode::Password;
        Self { input }
    }
    pub fn visibility_toggle(mut self, enabled: bool) -> Self {
        self.input.visibility_toggle = enabled;
        self
    }
}

impl SearchInput {
    pub fn new(id: impl Into<ElementId>, state: &Entity<InputState>) -> Self {
        let mut input = Input::new(id, state);
        input.mode = InputMode::Search;
        Self { input }
    }
    pub fn loading(mut self, loading: bool) -> Self {
        self.input.search_loading = loading;
        self
    }
    pub fn enter_button(mut self, label: impl Into<SharedString>) -> Self {
        self.input.search_button = SearchButton::Label(label.into());
        self
    }
    pub fn primary_action(mut self, primary: bool) -> Self {
        self.input.search_button = if primary {
            SearchButton::PrimaryIcon
        } else {
            SearchButton::Icon
        };
        self
    }
}
