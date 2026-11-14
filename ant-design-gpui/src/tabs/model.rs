// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use gpui::{
    AnyElement, AnyView, App, AppContext, Context, IntoElement, ParentElement, Render,
    SharedString, Window,
};
use std::{collections::HashMap, fmt, rc::Rc};

use crate::{Direction, icons::IconRef};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum TabsType {
    #[default]
    Line,
    Card,
    EditableCard,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum TabPlacement {
    #[default]
    Top,
    Bottom,
    Start,
    End,
}
impl TabPlacement {
    pub fn vertical(self) -> bool {
        matches!(self, Self::Start | Self::End)
    }
    pub(crate) fn trailing(self, direction: Direction) -> bool {
        match self {
            Self::Top => false,
            Self::Bottom => true,
            Self::Start => direction == Direction::Rtl,
            Self::End => direction == Direction::Ltr,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TabsAnimation {
    pub indicator: bool,
    pub panes: bool,
}
impl Default for TabsAnimation {
    fn default() -> Self {
        Self {
            indicator: true,
            panes: false,
        }
    }
}
impl From<bool> for TabsAnimation {
    fn from(value: bool) -> Self {
        Self {
            indicator: value,
            panes: value,
        }
    }
}

pub(super) type ContentRenderer = Rc<dyn Fn(&mut Window, &mut App) -> AnyElement>;
pub(super) type PaneFactory = Rc<dyn Fn(&mut Window, &mut App) -> AnyView>;

struct ContentView(ContentRenderer);
impl Render for ContentView {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        (self.0)(window, cx)
    }
}

/// Retain and clone items when changing their order or metadata. Keys are stable
/// application identities; labels may be duplicated or localized independently.
#[derive(Clone)]
pub struct TabItem {
    pub key: SharedString,
    pub label: SharedString,
    pub disabled: bool,
    pub closable: bool,
    pub force_render: bool,
    pub destroy_on_hidden: Option<bool>,
    pub icon: Option<IconRef>,
    pub(super) label_render: Option<ContentRenderer>,
    pub(super) close_render: Option<ContentRenderer>,
    pub(super) pane: Option<PaneFactory>,
}
impl TabItem {
    pub fn new(key: impl Into<SharedString>, label: impl Into<SharedString>) -> Self {
        Self {
            key: key.into(),
            label: label.into(),
            disabled: false,
            closable: true,
            force_render: false,
            destroy_on_hidden: None,
            icon: None,
            label_render: None,
            close_render: None,
            pane: None,
        }
    }
    pub fn disabled(mut self, value: bool) -> Self {
        self.disabled = value;
        self
    }
    pub fn closable(mut self, value: bool) -> Self {
        self.closable = value;
        self
    }
    pub fn force_render(mut self, value: bool) -> Self {
        self.force_render = value;
        self
    }
    pub fn destroy_on_hidden(mut self, value: bool) -> Self {
        self.destroy_on_hidden = Some(value);
        self
    }
    pub fn icon(mut self, icon: impl Into<IconRef>) -> Self {
        self.icon = Some(icon.into());
        self
    }
    /// Native label content; `label` remains the accessible name and search text.
    pub fn label_render(
        mut self,
        render: impl Fn(&mut Window, &mut App) -> AnyElement + 'static,
    ) -> Self {
        self.label_render = Some(Rc::new(render));
        self
    }
    pub fn close_icon(
        mut self,
        render: impl Fn(&mut Window, &mut App) -> AnyElement + 'static,
    ) -> Self {
        self.close_render = Some(Rc::new(render));
        self
    }
    /// A retained view is created only on first activation (or `force_render`).
    /// Destroying a hidden pane drops this component's handle; external handles
    /// remain owned by the application. Reopening calls this factory again.
    pub fn view(mut self, factory: impl Fn(&mut Window, &mut App) -> AnyView + 'static) -> Self {
        self.pane = Some(Rc::new(factory));
        self
    }
    /// Convenience for native element content. Keep application state in entities
    /// captured by the closure, as with other GPUI render builders.
    pub fn content(self, render: impl Fn(&mut Window, &mut App) -> AnyElement + 'static) -> Self {
        let render: ContentRenderer = Rc::new(render);
        self.view(move |_, cx| cx.new(|_| ContentView(render.clone())).into())
    }
    pub fn text(self, text: impl Into<SharedString>) -> Self {
        let text = text.into();
        self.content(move |_, _| gpui::div().child(text.clone()).into_any_element())
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum TabsItemsError {
    DuplicateKey(SharedString),
}
impl fmt::Display for TabsItemsError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::DuplicateKey(key) => write!(f, "duplicate tab key: {key}"),
        }
    }
}
impl std::error::Error for TabsItemsError {}

#[derive(Default)]
struct Items {
    entries: Vec<TabItem>,
    positions: HashMap<SharedString, usize>,
}

/// Immutable, cheaply cloned collection with constant-time key lookup.
#[derive(Clone, Default)]
pub struct TabsItems(Rc<Items>);
impl TabsItems {
    pub fn new(items: impl IntoIterator<Item = TabItem>) -> Result<Self, TabsItemsError> {
        let entries: Vec<_> = items.into_iter().collect();
        let mut positions = HashMap::with_capacity(entries.len());
        for (index, item) in entries.iter().enumerate() {
            if positions.insert(item.key.clone(), index).is_some() {
                return Err(TabsItemsError::DuplicateKey(item.key.clone()));
            }
        }
        Ok(Self(Rc::new(Items { entries, positions })))
    }
    pub fn entries(&self) -> &[TabItem] {
        &self.0.entries
    }
    pub fn len(&self) -> usize {
        self.0.entries.len()
    }
    pub fn is_empty(&self) -> bool {
        self.0.entries.is_empty()
    }
    pub fn get(&self, key: &str) -> Option<&TabItem> {
        self.index(key).map(|i| &self.0.entries[i])
    }
    pub fn index(&self, key: &str) -> Option<usize> {
        self.0.positions.get(key).copied()
    }
    pub(super) fn same(&self, other: &Self) -> bool {
        Rc::ptr_eq(&self.0, &other.0)
    }
}

pub(super) struct TabsModel {
    pub items: TabsItems,
    pub active: Option<SharedString>,
    pub controlled: bool,
    pub initialized: bool,
}
impl TabsModel {
    pub fn new(items: TabsItems) -> Self {
        let active = items.entries().first().map(|item| item.key.clone());
        Self {
            items,
            active,
            controlled: false,
            initialized: false,
        }
    }
    pub fn set_items(&mut self, items: TabsItems) -> bool {
        if self.items.same(&items) {
            return false;
        }
        let old_index = self
            .active
            .as_deref()
            .and_then(|key| self.items.index(key))
            .unwrap_or(0);
        if !self.controlled
            && self
                .active
                .as_deref()
                .is_none_or(|key| items.get(key).is_none())
        {
            self.active = items
                .entries()
                .get(old_index.min(items.len().saturating_sub(1)))
                .map(|item| item.key.clone());
        }
        self.items = items;
        true
    }
    pub fn next_enabled(&self, key: Option<&str>, offset: isize) -> Option<SharedString> {
        let len = self.items.len();
        if len == 0 {
            return None;
        }
        let start = key
            .and_then(|key| self.items.index(key))
            .map(|i| i as isize)
            .unwrap_or(if offset > 0 { -1 } else { 0 });
        (1..=len).find_map(|step| {
            let index = (start + offset * step as isize).rem_euclid(len as isize) as usize;
            let item = &self.items.entries()[index];
            (!item.disabled).then(|| item.key.clone())
        })
    }
}
