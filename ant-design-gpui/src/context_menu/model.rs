// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use crate::icons::IconName;
use gpui::{App, ElementId, SharedString, Window};
use std::{collections::HashSet, fmt, rc::Rc, sync::Arc};

type SubmenuFactory = Rc<dyn Fn(&mut Window, &mut App) -> MenuItems>;

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub enum MenuCheck {
    #[default]
    None,
    Checkbox(bool),
    Radio {
        group: SharedString,
        checked: bool,
    },
}
impl MenuCheck {
    pub fn checked(&self) -> Option<bool> {
        match self {
            Self::None => None,
            Self::Checkbox(checked) | Self::Radio { checked, .. } => Some(*checked),
        }
    }
}

/// Stable identity and presentation are separate. Shortcuts are display metadata;
/// applications own their command keymaps.
#[derive(Clone)]
pub struct MenuItem {
    pub id: ElementId,
    pub label: SharedString,
    pub icon: Option<IconName>,
    pub shortcut: Option<SharedString>,
    pub badge: Option<SharedString>,
    pub disabled: bool,
    pub danger: bool,
    pub check: MenuCheck,
    pub submenu: Option<MenuItems>,
    pub(super) lazy_submenu: Option<SubmenuFactory>,
}
impl fmt::Debug for MenuItem {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("MenuItem")
            .field("id", &self.id)
            .field("label", &self.label)
            .field("disabled", &self.disabled)
            .field("check", &self.check)
            .field("submenu", &self.submenu)
            .field("lazy", &self.lazy_submenu.is_some())
            .finish_non_exhaustive()
    }
}
impl MenuItem {
    pub fn new(id: impl Into<ElementId>, label: impl Into<SharedString>) -> Self {
        Self {
            id: id.into(),
            label: label.into(),
            icon: None,
            shortcut: None,
            badge: None,
            disabled: false,
            danger: false,
            check: MenuCheck::None,
            submenu: None,
            lazy_submenu: None,
        }
    }
    pub fn icon(mut self, icon: IconName) -> Self {
        self.icon = Some(icon);
        self
    }
    pub fn shortcut(mut self, shortcut: impl Into<SharedString>) -> Self {
        self.shortcut = Some(shortcut.into());
        self
    }
    pub fn badge(mut self, badge: impl Into<SharedString>) -> Self {
        self.badge = Some(badge.into());
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
    pub fn checked(mut self, checked: bool) -> Self {
        self.check = MenuCheck::Checkbox(checked);
        self
    }
    pub fn radio(mut self, group: impl Into<SharedString>, checked: bool) -> Self {
        self.check = MenuCheck::Radio {
            group: group.into(),
            checked,
        };
        self
    }
    pub fn submenu(mut self, items: MenuItems) -> Self {
        self.submenu = Some(items);
        self.lazy_submenu = None;
        self
    }
    /// Populate on opening only. Contents are released when this branch closes.
    pub fn lazy_submenu(
        mut self,
        factory: impl Fn(&mut Window, &mut App) -> MenuItems + 'static,
    ) -> Self {
        self.lazy_submenu = Some(Rc::new(factory));
        self.submenu = None;
        self
    }
    pub fn has_submenu(&self) -> bool {
        self.submenu.is_some() || self.lazy_submenu.is_some()
    }
}

#[derive(Clone, Debug)]
pub enum MenuEntry {
    Item(MenuItem),
    Separator,
    Group(SharedString),
}
impl MenuEntry {
    pub fn group(label: impl Into<SharedString>) -> Self {
        Self::Group(label.into())
    }
    pub fn item(&self) -> Option<&MenuItem> {
        if let Self::Item(item) = self {
            Some(item)
        } else {
            None
        }
    }
}
impl From<MenuItem> for MenuEntry {
    fn from(item: MenuItem) -> Self {
        Self::Item(item)
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MenuItemsError {
    DuplicateId(ElementId),
    TooDeep,
}
impl fmt::Display for MenuItemsError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::DuplicateId(id) => write!(f, "duplicate menu item identity: {id:?}"),
            Self::TooDeep => f.write_str("menu nesting exceeds 32 levels"),
        }
    }
}
impl std::error::Error for MenuItemsError {}

/// Cheap immutable snapshots. IDs are unique within a menu level, matching GPUI's
/// hierarchical element identity. A single item can therefore appear in two branches.
#[derive(Clone, Debug, Default)]
pub struct MenuItems(pub(super) Arc<[MenuEntry]>, Arc<[usize]>, usize);
impl MenuItems {
    pub fn new(items: impl IntoIterator<Item = MenuEntry>) -> Result<Self, MenuItemsError> {
        let entries: Arc<[MenuEntry]> = items.into_iter().collect();
        let mut count = 0;
        let positions = entries
            .iter()
            .map(|entry| {
                if entry.item().is_some() {
                    count += 1;
                    count
                } else {
                    0
                }
            })
            .collect();
        let items = Self(entries, positions, count);
        items.validate(0)?;
        Ok(items)
    }
    fn validate(&self, depth: usize) -> Result<(), MenuItemsError> {
        if depth >= 32 {
            return Err(MenuItemsError::TooDeep);
        }
        let mut ids = HashSet::new();
        for item in self.0.iter().filter_map(MenuEntry::item) {
            if !ids.insert(&item.id) {
                return Err(MenuItemsError::DuplicateId(item.id.clone()));
            }
            if let Some(children) = &item.submenu {
                children.validate(depth + 1)?;
            }
        }
        Ok(())
    }
    pub fn entries(&self) -> &[MenuEntry] {
        &self.0
    }
    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }
    pub fn len(&self) -> usize {
        self.0.len()
    }
    pub(super) fn item_position(&self, index: usize) -> (usize, usize) {
        (self.1[index], self.2)
    }
    pub(super) fn item(&self, index: usize) -> Option<&MenuItem> {
        self.0.get(index)?.item()
    }
    pub(super) fn enabled(&self, index: usize) -> bool {
        self.item(index).is_some_and(|i| !i.disabled)
    }
    pub(super) fn next(&self, active: Option<usize>, delta: isize) -> Option<usize> {
        let count = self.len();
        if count == 0 {
            return None;
        }
        let start =
            active
                .map(|i| i as isize)
                .unwrap_or(if delta > 0 { -1 } else { count as isize });
        (1..=count)
            .map(|step| {
                (start + step as isize * delta.signum()).rem_euclid(count as isize) as usize
            })
            .find(|&i| self.enabled(i))
    }
    pub(super) fn toggle(&mut self, index: usize) -> Option<bool> {
        let check = self.item(index)?.check.clone();
        let items = Arc::make_mut(&mut self.0);
        match check {
            MenuCheck::None => None,
            MenuCheck::Checkbox(value) => {
                if let MenuEntry::Item(item) = &mut items[index] {
                    item.check = MenuCheck::Checkbox(!value)
                }
                Some(!value)
            }
            MenuCheck::Radio { group, .. } => {
                for (i, entry) in items.iter_mut().enumerate() {
                    if let MenuEntry::Item(item) = entry
                        && let MenuCheck::Radio {
                            group: item_group,
                            checked,
                        } = &mut item.check
                        && *item_group == group
                    {
                        *checked = i == index;
                    }
                }
                Some(true)
            }
        }
    }
}
