// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use crate::icons::IconRef;
use gpui::{AnyElement, App, IntoElement, Pixels, Point, SharedString, StyleRefinement, Window};
use std::{collections::HashMap, fmt, rc::Rc, time::Duration};

pub type MenuKey = SharedString;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum MenuMode {
    #[default]
    Vertical,
    Horizontal,
    Inline,
}

/// Menu appearance is independent of the provider's light/dark token algorithm.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum MenuColorTheme {
    #[default]
    Light,
    Dark,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum MenuTrigger {
    #[default]
    Hover,
    Click,
}

#[derive(Clone, Debug, PartialEq)]
pub struct MenuOptions {
    pub mode: MenuMode,
    pub color_theme: MenuColorTheme,
    pub inline_collapsed: bool,
    pub inline_indent: f64,
    pub multiple: bool,
    pub selectable: bool,
    pub trigger: MenuTrigger,
    pub open_delay: Duration,
    pub close_delay: Duration,
    pub force_submenu_render: bool,
    pub tooltip: bool,
}
impl Default for MenuOptions {
    fn default() -> Self {
        Self {
            mode: MenuMode::Vertical,
            color_theme: MenuColorTheme::Light,
            inline_collapsed: false,
            inline_indent: 24.,
            multiple: false,
            selectable: true,
            trigger: MenuTrigger::Hover,
            open_delay: Duration::from_millis(100),
            close_delay: Duration::from_millis(100),
            force_submenu_render: false,
            tooltip: true,
        }
    }
}
impl MenuOptions {
    pub fn is_inline(&self) -> bool {
        self.mode == MenuMode::Inline && !self.inline_collapsed
    }
    pub fn collapsed(&self) -> bool {
        self.inline_collapsed && self.mode != MenuMode::Horizontal
    }
}

#[derive(Clone, Debug)]
pub struct MenuRenderContext {
    pub key: MenuKey,
    pub label: SharedString,
    pub depth: usize,
    pub selected: bool,
    pub active: bool,
    pub expanded: bool,
    pub collapsed: bool,
    pub disabled: bool,
    pub color_theme: MenuColorTheme,
    pub mode: MenuMode,
}
#[derive(Clone, Debug)]
pub struct MenuPopupContext {
    pub key: MenuKey,
    /// Innermost submenu first, followed by its ancestors.
    pub key_path: Vec<MenuKey>,
    pub color_theme: MenuColorTheme,
}
pub type MenuContentRenderer = Rc<dyn Fn(&MenuRenderContext, &mut Window, &mut App) -> AnyElement>;
pub type MenuPopupRenderer =
    Rc<dyn Fn(AnyElement, &MenuPopupContext, &mut Window, &mut App) -> AnyElement>;

/// A leaf's accessible text is retained even when its visual content is replaced.
#[derive(Clone)]
pub struct MenuItem {
    pub key: MenuKey,
    pub label: SharedString,
    pub icon: Option<IconRef>,
    pub extra: Option<SharedString>,
    pub disabled: bool,
    pub danger: bool,
    pub title: Option<SharedString>,
    pub render: Option<MenuContentRenderer>,
    pub extra_render: Option<MenuContentRenderer>,
    pub style: StyleRefinement,
}
impl fmt::Debug for MenuItem {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("MenuItem")
            .field("key", &self.key)
            .field("label", &self.label)
            .field("disabled", &self.disabled)
            .field("danger", &self.danger)
            .finish_non_exhaustive()
    }
}
impl MenuItem {
    pub fn new(key: impl Into<MenuKey>, label: impl Into<SharedString>) -> Self {
        Self {
            key: key.into(),
            label: label.into(),
            icon: None,
            extra: None,
            disabled: false,
            danger: false,
            title: None,
            render: None,
            extra_render: None,
            style: StyleRefinement::default(),
        }
    }
    pub fn icon(mut self, icon: impl Into<IconRef>) -> Self {
        self.icon = Some(icon.into());
        self
    }
    pub fn extra(mut self, extra: impl Into<SharedString>) -> Self {
        self.extra = Some(extra.into());
        self
    }
    pub fn disabled(mut self, disabled: bool) -> Self {
        self.disabled = disabled;
        self
    }
    pub fn danger(mut self, danger: bool) -> Self {
        self.danger = danger;
        self
    }
    /// An empty title suppresses the collapsed tooltip for this row.
    pub fn title(mut self, title: impl Into<SharedString>) -> Self {
        self.title = Some(title.into());
        self
    }
    pub fn style(mut self, style: StyleRefinement) -> Self {
        self.style = style;
        self
    }
    pub fn content<E: IntoElement>(
        mut self,
        render: impl Fn(&MenuRenderContext, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.render = Some(Rc::new(move |info, window, cx| {
            render(info, window, cx).into_any_element()
        }));
        self
    }
    pub fn extra_content<E: IntoElement>(
        mut self,
        render: impl Fn(&MenuRenderContext, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.extra_render = Some(Rc::new(move |info, window, cx| {
            render(info, window, cx).into_any_element()
        }));
        self
    }
}

#[derive(Clone)]
pub struct SubMenu {
    pub item: MenuItem,
    pub children: MenuItems,
    pub color_theme: Option<MenuColorTheme>,
    pub popup_offset: Point<Pixels>,
    pub popup_render: Option<MenuPopupRenderer>,
}
impl fmt::Debug for SubMenu {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("SubMenu")
            .field("item", &self.item)
            .field("children", &self.children)
            .finish_non_exhaustive()
    }
}
impl SubMenu {
    pub fn new(
        key: impl Into<MenuKey>,
        label: impl Into<SharedString>,
        children: MenuItems,
    ) -> Self {
        Self {
            item: MenuItem::new(key, label),
            children,
            color_theme: None,
            popup_offset: Point::default(),
            popup_render: None,
        }
    }
    pub fn icon(mut self, icon: impl Into<IconRef>) -> Self {
        self.item.icon = Some(icon.into());
        self
    }
    pub fn disabled(mut self, disabled: bool) -> Self {
        self.item.disabled = disabled;
        self
    }
    pub fn theme(mut self, theme: MenuColorTheme) -> Self {
        self.color_theme = Some(theme);
        self
    }
    pub fn color_theme(self, theme: MenuColorTheme) -> Self {
        self.theme(theme)
    }
    pub fn style(mut self, style: StyleRefinement) -> Self {
        self.item.style = style;
        self
    }
    pub fn popup_offset(mut self, offset: Point<Pixels>) -> Self {
        self.popup_offset = offset;
        self
    }
    pub fn content<E: IntoElement>(
        mut self,
        render: impl Fn(&MenuRenderContext, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.item = self.item.content(render);
        self
    }
    pub fn popup_render<E: IntoElement>(
        mut self,
        render: impl Fn(AnyElement, &MenuPopupContext, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.popup_render = Some(Rc::new(move |body, info, window, cx| {
            render(body, info, window, cx).into_any_element()
        }));
        self
    }
}

#[derive(Clone, Debug)]
pub struct MenuGroup {
    pub key: MenuKey,
    pub label: SharedString,
    pub children: MenuItems,
}
impl MenuGroup {
    pub fn new(
        key: impl Into<MenuKey>,
        label: impl Into<SharedString>,
        children: MenuItems,
    ) -> Self {
        Self {
            key: key.into(),
            label: label.into(),
            children,
        }
    }
}
#[derive(Clone, Debug)]
pub struct MenuDivider {
    pub key: MenuKey,
    pub dashed: bool,
}
impl MenuDivider {
    pub fn new(key: impl Into<MenuKey>) -> Self {
        Self {
            key: key.into(),
            dashed: false,
        }
    }
    pub fn dashed(mut self, dashed: bool) -> Self {
        self.dashed = dashed;
        self
    }
}
#[derive(Clone, Debug)]
pub enum MenuEntry {
    Item(MenuItem),
    SubMenu(SubMenu),
    Group(MenuGroup),
    Divider(MenuDivider),
}
impl MenuEntry {
    pub fn key(&self) -> &MenuKey {
        match self {
            Self::Item(i) => &i.key,
            Self::SubMenu(s) => &s.item.key,
            Self::Group(g) => &g.key,
            Self::Divider(d) => &d.key,
        }
    }
    pub fn item(&self) -> Option<&MenuItem> {
        match self {
            Self::Item(i) => Some(i),
            Self::SubMenu(s) => Some(&s.item),
            _ => None,
        }
    }
    pub fn children(&self) -> Option<&MenuItems> {
        match self {
            Self::SubMenu(s) => Some(&s.children),
            Self::Group(g) => Some(&g.children),
            _ => None,
        }
    }
}
impl From<MenuItem> for MenuEntry {
    fn from(value: MenuItem) -> Self {
        Self::Item(value)
    }
}
impl From<SubMenu> for MenuEntry {
    fn from(value: SubMenu) -> Self {
        Self::SubMenu(value)
    }
}
impl From<MenuGroup> for MenuEntry {
    fn from(value: MenuGroup) -> Self {
        Self::Group(value)
    }
}
impl From<MenuDivider> for MenuEntry {
    fn from(value: MenuDivider) -> Self {
        Self::Divider(value)
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MenuItemsError {
    DuplicateKey(MenuKey),
}
impl fmt::Display for MenuItemsError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::DuplicateKey(key) => write!(f, "duplicate navigation menu key: {key}"),
        }
    }
}
impl std::error::Error for MenuItemsError {}

#[derive(Debug)]
struct Record {
    entry: MenuEntry,
    parent: Option<MenuKey>,
    path: Vec<MenuKey>,
    depth: usize,
    enabled: bool,
    root_position: usize,
}
#[derive(Clone, Debug, Default)]
pub struct MenuItems {
    entries: Rc<[MenuEntry]>,
    index: Rc<HashMap<String, Record>>,
}
impl MenuItems {
    pub fn new(entries: impl IntoIterator<Item = MenuEntry>) -> Result<Self, MenuItemsError> {
        let entries: Rc<[MenuEntry]> = entries.into_iter().collect();
        let mut index = HashMap::new();
        Self::index_entries(&entries, None, &[], true, None, &mut index)?;
        Ok(Self {
            entries,
            index: Rc::new(index),
        })
    }
    fn index_entries(
        entries: &[MenuEntry],
        parent: Option<&MenuKey>,
        ancestors: &[MenuKey],
        enabled: bool,
        root_position: Option<usize>,
        index: &mut HashMap<String, Record>,
    ) -> Result<(), MenuItemsError> {
        for (position, entry) in entries.iter().enumerate() {
            let root_position = root_position.unwrap_or(position);
            let key = entry.key();
            if index.contains_key(key.as_ref()) {
                return Err(MenuItemsError::DuplicateKey(key.clone()));
            }
            let mut path = ancestors.to_vec();
            if entry.item().is_some() {
                path.push(key.clone());
            }
            let allowed = enabled && entry.item().is_none_or(|item| !item.disabled);
            index.insert(
                key.to_string(),
                Record {
                    entry: entry.clone(),
                    parent: parent.cloned(),
                    path: path.iter().rev().cloned().collect(),
                    depth: ancestors.len(),
                    enabled: allowed && entry.item().is_some(),
                    root_position,
                },
            );
            if let Some(children) = entry.children() {
                let next_parent = if matches!(entry, MenuEntry::SubMenu(_)) {
                    Some(key)
                } else {
                    parent
                };
                Self::index_entries(
                    children.entries(),
                    next_parent,
                    &path,
                    allowed,
                    Some(root_position),
                    index,
                )?;
            }
        }
        Ok(())
    }
    pub fn entries(&self) -> &[MenuEntry] {
        &self.entries
    }
    pub fn len(&self) -> usize {
        self.entries.len()
    }
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }
    pub fn same_collection(&self, other: &Self) -> bool {
        Rc::ptr_eq(&self.entries, &other.entries)
    }
    pub fn find(&self, key: &str) -> Option<&MenuEntry> {
        self.index.get(key).map(|r| &r.entry)
    }
    pub fn item(&self, key: &str) -> Option<&MenuItem> {
        self.find(key)?.item()
    }
    pub fn parent(&self, key: &str) -> Option<&MenuKey> {
        self.index.get(key)?.parent.as_ref()
    }
    pub fn path(&self, key: &str) -> Vec<MenuKey> {
        self.index
            .get(key)
            .map_or_else(Vec::new, |r| r.path.clone())
    }
    pub fn depth(&self, key: &str) -> usize {
        self.index.get(key).map_or(0, |r| r.depth)
    }
    pub fn root_position(&self, key: &str) -> Option<usize> {
        self.index.get(key).map(|record| record.root_position)
    }
    pub fn enabled(&self, key: &str) -> bool {
        self.index.get(key).is_some_and(|r| r.enabled)
    }
    pub fn contains(&self, key: &str) -> bool {
        self.index.contains_key(key)
    }
    pub fn is_submenu(&self, key: &str) -> bool {
        matches!(self.find(key), Some(MenuEntry::SubMenu(_)))
    }
    pub fn descendant_of(&self, key: &str, parent: &str) -> bool {
        self.index
            .get(key)
            .is_some_and(|r| r.path.iter().skip(1).any(|k| k.as_ref() == parent))
    }
    pub fn visible_keys(
        &self,
        open: &[MenuKey],
        inline: bool,
        parent: Option<&str>,
    ) -> Vec<MenuKey> {
        fn collect(items: &MenuItems, open: &[MenuKey], inline: bool, result: &mut Vec<MenuKey>) {
            for entry in items.entries() {
                match entry {
                    MenuEntry::Item(i) => result.push(i.key.clone()),
                    MenuEntry::SubMenu(s) => {
                        result.push(s.item.key.clone());
                        if inline && open.contains(&s.item.key) {
                            collect(&s.children, open, inline, result);
                        }
                    }
                    MenuEntry::Group(g) => collect(&g.children, open, inline, result),
                    MenuEntry::Divider(_) => {}
                }
            }
        }
        let mut result = Vec::new();
        let items = parent
            .and_then(|key| self.find(key))
            .and_then(MenuEntry::children)
            .unwrap_or(self);
        collect(items, open, inline, &mut result);
        result.retain(|key| self.enabled(key));
        result
    }
}
