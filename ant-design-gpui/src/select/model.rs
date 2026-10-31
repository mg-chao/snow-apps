// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{SelectMatches, SelectOption, SelectOptions, SelectRow, SelectSearch};
use gpui::{ElementId, SharedString};
use std::{
    collections::{HashMap, HashSet},
    hash::Hash,
    rc::Rc,
};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum SelectMode {
    #[default]
    Single,
    Multiple,
    Tags,
}
impl SelectMode {
    pub fn multiple(self) -> bool {
        self != Self::Single
    }
}

/// A label travels with a selected value. It survives remote option replacement,
/// and supports domain values that have no Display implementation.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SelectValue<T> {
    pub value: T,
    pub label: SharedString,
}
impl<T> SelectValue<T> {
    pub fn new(value: T, label: impl Into<SharedString>) -> Self {
        Self {
            value,
            label: label.into(),
        }
    }
}
impl<T: Clone> From<&SelectOption<T>> for SelectValue<T> {
    fn from(option: &SelectOption<T>) -> Self {
        Self::new(option.value.clone(), option.label.clone())
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SelectSettings {
    pub mode: SelectMode,
    pub max_count: Option<usize>,
    pub hide_selected: bool,
    pub default_active_first: bool,
    pub token_separators: Vec<char>,
}
impl Default for SelectSettings {
    fn default() -> Self {
        Self {
            mode: SelectMode::Single,
            max_count: None,
            hide_selected: false,
            default_active_first: true,
            token_separators: Vec::new(),
        }
    }
}
/// Convert user-created text to a domain option. Returning None rejects a tag.
/// Stable IDs must be independent of its position in the selected-value list.
pub type SelectTagFactory<T> = Rc<dyn Fn(&str) -> Option<SelectOption<T>>>;
/// An unchanged single token (or an empty list) leaves the query in the editor.
pub type SelectTokenizer = Rc<dyn Fn(&str) -> Vec<SharedString>>;

pub(super) struct SelectionProposal<T> {
    pub values: Vec<SelectValue<T>>,
    pub selected: Vec<SelectOption<T>>,
    pub deselected: Vec<SelectValue<T>>,
}

struct DisplayOrder {
    sources: Vec<usize>,
    inverse: Vec<usize>,
    ordinals: Vec<usize>,
}

pub(super) struct SelectModel<T> {
    pub options: SelectOptions<T>,
    pub settings: SelectSettings,
    pub search: SelectSearch<T>,
    pub values: Rc<[SelectValue<T>]>,
    pub accessible_value: SharedString,
    pub selected: HashSet<T>,
    selection_ids: HashMap<T, u64>,
    next_selection_id: u64,
    pub query: SharedString,
    pub matches: SelectMatches,
    pub suggestion: Option<SelectOption<T>>,
    created: SelectOptions<T>,
    created_matches: SelectMatches,
    display_order: Option<DisplayOrder>,
    next_created_id: usize,
    pub active: Option<usize>,
    pub tag_factory: Option<SelectTagFactory<T>>,
    pub tokenizer: Option<SelectTokenizer>,
    // These counters expose structural work to deterministic performance tests.
    pub filter_revision: u64,
}
impl<T: Clone + Eq + Hash> SelectModel<T> {
    pub fn new(options: SelectOptions<T>) -> Self {
        let matches = options.all();
        let mut this = Self {
            options,
            matches,
            settings: SelectSettings::default(),
            search: SelectSearch::default(),
            values: Rc::from([]),
            accessible_value: "".into(),
            selected: HashSet::new(),
            selection_ids: HashMap::new(),
            next_selection_id: 0,
            query: SharedString::default(),
            suggestion: None,
            created: SelectOptions::default(),
            created_matches: SelectOptions::<T>::default().all(),
            display_order: None,
            next_created_id: 0,
            active: None,
            tag_factory: None,
            tokenizer: None,
            filter_revision: 0,
        };
        this.reset_active();
        this
    }
    pub fn searchable(&self) -> bool {
        self.search.enabled.unwrap_or(self.settings.mode.multiple())
    }
    pub fn set_options(&mut self, options: SelectOptions<T>) -> bool {
        if self.options.same_collection(&options) {
            return false;
        }
        let active = self.active_id().cloned();
        self.options = options;
        // Prefer current labels, while retaining labels for unloaded remote data.
        if self.values.iter().any(|v| {
            self.options
                .option(&v.value)
                .is_some_and(|o| o.label != v.label)
        }) {
            for value in Rc::make_mut(&mut self.values) {
                if let Some(option) = self.options.option(&value.value) {
                    value.label = option.label.clone();
                }
            }
            self.refresh_accessible_value();
        }
        self.refresh_created();
        self.refilter_with(active);
        true
    }
    pub fn set_settings(&mut self, settings: SelectSettings) -> bool {
        if self.settings == settings {
            return false;
        }
        let active = self.active_id().cloned();
        self.settings = settings;
        if !self.settings.mode.multiple() && self.values.len() > 1 {
            self.values = Rc::from(&self.values[..1]);
            self.refresh_accessible_value();
        }
        self.selected = self.values.iter().map(|v| v.value.clone()).collect();
        self.sync_selection_ids();
        self.refresh_created();
        self.refilter_with(active);
        true
    }
    pub fn set_search(&mut self, policy: SelectSearch<T>) -> bool {
        if self.search.equivalent(&policy) {
            return false;
        }
        self.search = policy;
        self.refilter();
        true
    }
    pub fn set_query(&mut self, query: SharedString) -> bool {
        if self.query == query {
            return false;
        }
        self.query = query;
        self.refilter();
        true
    }
    pub fn set_values(&mut self, mut values: Vec<SelectValue<T>>) -> bool {
        let mut seen = HashSet::with_capacity(values.len());
        values.retain(|v| seen.insert(v.value.clone()));
        if !self.settings.mode.multiple() {
            values.truncate(1);
        }
        if self.values.as_ref() == values {
            return false;
        }
        let active = self.active_id().cloned();
        self.values = values.into();
        self.refresh_accessible_value();
        self.selected = self.values.iter().map(|v| v.value.clone()).collect();
        self.sync_selection_ids();
        self.refresh_created();
        if self.settings.hide_selected || self.settings.mode == SelectMode::Tags {
            self.refilter_with(active);
        } else if self.active.is_some_and(|i| !self.enabled(i)) {
            self.reset_active();
        }
        true
    }
    pub fn row_count(&self) -> usize {
        self.matches.rows().len()
            + self.created_matches.rows().len()
            + usize::from(self.suggestion.is_some())
    }
    fn refresh_accessible_value(&mut self) {
        self.accessible_value = self
            .values
            .iter()
            .map(|v| v.label.as_ref())
            .collect::<Vec<_>>()
            .join(", ")
            .into();
    }
    fn sync_selection_ids(&mut self) {
        self.selection_ids
            .retain(|value, _| self.selected.contains(value));
        for value in self.values.iter() {
            self.selection_ids
                .entry(value.value.clone())
                .or_insert_with(|| {
                    self.next_selection_id += 1;
                    self.next_selection_id
                });
        }
    }
    pub fn selection_id(&self, value: &T) -> u64 {
        self.selection_ids[value]
    }
    pub fn row(&self, index: usize) -> Option<&SelectRow<T>> {
        let index = index.checked_sub(usize::from(self.suggestion.is_some()))?;
        let source = match &self.display_order {
            Some(order) => *order.sources.get(index)?,
            None => index,
        };
        self.source_row(source)
    }
    fn source_row(&self, index: usize) -> Option<&SelectRow<T>> {
        if index < self.matches.rows().len() {
            self.matches
                .rows()
                .get(index)
                .map(|i| &self.options.rows()[*i])
        } else {
            self.created_matches
                .rows()
                .get(index - self.matches.rows().len())
                .map(|i| &self.created.rows()[*i])
        }
    }
    pub fn option(&self, index: usize) -> Option<&SelectOption<T>> {
        if index == 0 && self.suggestion.is_some() {
            return self.suggestion.as_ref();
        }
        self.row(index).and_then(SelectRow::option)
    }
    pub fn option_position(&self, index: usize) -> Option<(usize, usize)> {
        let suggestion = usize::from(self.suggestion.is_some());
        let total = suggestion + self.matches.option_count() + self.created_matches.option_count();
        if index == 0 && suggestion != 0 {
            return Some((1, total));
        }
        let index = index.checked_sub(suggestion)?;
        if let Some(order) = &self.display_order {
            let ordinal = *order.ordinals.get(index)?;
            return (ordinal != 0).then_some((suggestion + ordinal, total));
        }
        let position = if index < self.matches.rows().len() {
            suggestion + self.matches.option_position(index)?
        } else {
            suggestion
                + self.matches.option_count()
                + self
                    .created_matches
                    .option_position(index - self.matches.rows().len())?
        };
        Some((position, total))
    }
    pub fn index_of_value(&self, value: &T) -> Option<usize> {
        if self
            .suggestion
            .as_ref()
            .is_some_and(|option| &option.value == value)
        {
            return Some(0);
        }
        let offset = usize::from(self.suggestion.is_some());
        let source = if let Some(row) = self.options.row_for_value(value) {
            self.matches.row_position(row)?
        } else {
            let row = self.created.row_for_value(value)?;
            self.matches.rows().len() + self.created_matches.row_position(row)?
        };
        Some(
            offset
                + self
                    .display_order
                    .as_ref()
                    .map_or(source, |order| order.inverse[source]),
        )
    }
    pub fn enabled(&self, index: usize) -> bool {
        self.option(index)
            .is_some_and(|o| !o.disabled && (self.selected.contains(&o.value) || !self.at_limit()))
    }
    pub fn at_limit(&self) -> bool {
        self.settings.mode.multiple()
            && self
                .settings
                .max_count
                .is_some_and(|n| self.values.len() >= n)
    }
    pub fn active_id(&self) -> Option<&ElementId> {
        self.active.and_then(|i| self.option(i)).map(|o| &o.id)
    }
    pub fn refilter(&mut self) {
        let previous = self.active_id().cloned();
        self.refilter_with(previous);
    }
    fn refilter_with(&mut self, previous: Option<ElementId>) {
        self.matches = if self.settings.hide_selected {
            self.options
                .search_excluding(&self.query, &self.search, &self.selected)
        } else {
            self.options.search(&self.query, &self.search)
        };
        self.created_matches = if self.settings.hide_selected {
            SelectOptions::<T>::default().all()
        } else {
            self.created.search(&self.query, &self.search)
        };
        self.sort_created();
        self.filter_revision += 1;
        self.suggestion = if self.settings.mode == SelectMode::Tags
            && !self.query.is_empty()
            && !(0..self.row_count()).any(|i| {
                self.row(i).and_then(SelectRow::option).is_some_and(|o| {
                    o.value_text.as_ref() == self.query.as_ref()
                        || self.search.fields.iter().any(|field| match field {
                            super::SelectSearchField::Value => o.value_text == self.query,
                            super::SelectSearchField::Label => o.label == self.query,
                            super::SelectSearchField::Field(key) => {
                                o.search_fields.get(key) == Some(&self.query)
                            }
                        })
                })
            }) {
            self.tag_factory
                .as_ref()
                .and_then(|f| f(&self.query))
                .filter(|o| {
                    !self.selected.contains(&o.value)
                        && !self.options.option(&o.value).is_some_and(|o| o.disabled)
                })
        } else {
            None
        };
        self.active = previous.and_then(|id| {
            (0..self.row_count())
                .find(|i| self.enabled(*i) && self.option(*i).is_some_and(|o| o.id == id))
        });
        if self.active.is_none() {
            self.reset_active();
        }
    }
    /// Keep the immutable datasets shared. Only a row permutation is needed to
    /// interleave selected user tags with supplied options under a custom sort.
    fn sort_created(&mut self) {
        self.display_order = None;
        if self.created_matches.is_empty()
            || (self.search.sort.is_none() && self.search.entry_sort.is_none())
        {
            return;
        }
        let count = self.matches.rows().len() + self.created_matches.rows().len();
        let mut blocks = Vec::new();
        let mut start = 0;
        while start < count {
            let mut end = start + 1;
            if matches!(self.source_row(start), Some(SelectRow::Group { .. })) {
                while matches!(
                    self.source_row(end),
                    Some(SelectRow::Option { group: Some(_), .. })
                ) {
                    end += 1;
                }
            }
            blocks.push(start..end);
            start = end;
        }
        blocks.sort_by(|a, b| {
            let a = self.source_row(a.start).unwrap();
            let b = self.source_row(b.start).unwrap();
            if let Some(sort) = &self.search.entry_sort {
                sort(a, b, &self.query)
            } else if let (Some(a), Some(b)) = (a.option(), b.option()) {
                self.search.sort.as_ref().unwrap()(a, b, &self.query)
            } else {
                std::cmp::Ordering::Equal
            }
        });
        let sources: Vec<_> = blocks.into_iter().flatten().collect();
        let mut inverse = vec![0; count];
        let mut ordinal = 0;
        let ordinals = sources
            .iter()
            .enumerate()
            .map(|(position, source)| {
                inverse[*source] = position;
                if self.source_row(*source).unwrap().option().is_some() {
                    ordinal += 1;
                    ordinal
                } else {
                    0
                }
            })
            .collect();
        self.display_order = Some(DisplayOrder {
            sources,
            inverse,
            ordinals,
        });
    }
    fn refresh_created(&mut self) {
        let mut options = Vec::new();
        if self.settings.mode == SelectMode::Tags {
            for value in self.values.iter() {
                if self.options.option(&value.value).is_some() {
                    continue;
                }
                let mut option = if let Some(cached) = self.created.option(&value.value) {
                    cached.clone()
                } else if let Some(option) = self
                    .tag_factory
                    .as_ref()
                    .and_then(|f| f(&value.label))
                    .filter(|o| o.value == value.value)
                {
                    option
                } else {
                    self.next_created_id += 1;
                    SelectOption::with_value_text(
                        ("select-created", self.next_created_id),
                        value.value.clone(),
                        value.label.clone(),
                        value.label.clone(),
                    )
                };
                option.label = value.label.clone();
                options.push(option);
            }
            options.sort_by(|a, b| a.value_text.cmp(&b.value_text));
        }
        // Keep this cache bounded by accepted selected values, never by past queries.
        self.created =
            SelectOptions::new(options).expect("tag factory must produce unique stable IDs");
        self.created_matches = self.created.all();
    }
    pub fn reset_active(&mut self) {
        self.active = self
            .settings
            .default_active_first
            .then(|| (0..self.row_count()).find(|i| self.enabled(*i)))
            .flatten();
    }
    pub fn activate_selected(&mut self) {
        if self.settings.mode == SelectMode::Single {
            self.active = self
                .values
                .first()
                .and_then(|value| self.index_of_value(&value.value))
                .filter(|index| self.enabled(*index))
                .or(self.active);
        }
    }
    pub fn navigate(&mut self, delta: isize) {
        let count = self.row_count();
        if count == 0 {
            self.active = None;
            return;
        }
        let start = self
            .active
            .map(|i| i as isize)
            .unwrap_or(if delta > 0 { -1 } else { 0 });
        for step in 1..=count {
            let index =
                (start + delta.signum() * step as isize).rem_euclid(count as isize) as usize;
            if self.enabled(index) {
                self.active = Some(index);
                return;
            }
        }
        self.active = None;
    }
    pub fn toggle(&self, option: &SelectOption<T>) -> Option<SelectionProposal<T>> {
        if option.disabled {
            return None;
        }
        if self.settings.mode.multiple() && self.selected.contains(&option.value) {
            return self.remove(&option.value);
        }
        if self.at_limit() {
            return None;
        }
        let value = SelectValue::from(option);
        let values = if self.settings.mode.multiple() {
            let mut values = self.values.to_vec();
            values.push(value);
            values
        } else {
            vec![value]
        };
        Some(SelectionProposal {
            values,
            selected: vec![option.clone()],
            deselected: Vec::new(),
        })
    }
    pub fn remove(&self, value: &T) -> Option<SelectionProposal<T>> {
        if self.options.option(value).is_some_and(|o| o.disabled) {
            return None;
        }
        let removed = self.values.iter().find(|v| &v.value == value)?.clone();
        Some(SelectionProposal {
            values: self
                .values
                .iter()
                .filter(|v| &v.value != value)
                .cloned()
                .collect(),
            selected: Vec::new(),
            deselected: vec![removed],
        })
    }
    /// Resolve completed separator-delimited input once. In multiple mode tokens
    /// match labels; tags mode creates missing values through the domain factory.
    pub fn tokenize(
        &self,
        text: &str,
        whole: bool,
    ) -> Option<(SelectionProposal<T>, SharedString)> {
        let custom = if !whole {
            self.tokenizer.as_ref().map(|f| f(text))
        } else {
            None
        };
        if let Some(tokens) = &custom {
            if tokens.is_empty() || (tokens.len() == 1 && tokens[0].as_ref() == text) {
                return None;
            }
        } else if !whole
            && !text
                .chars()
                .any(|c| self.settings.token_separators.contains(&c))
        {
            return None;
        }
        let tokens: Vec<&str> = if whole {
            vec![text]
        } else {
            custom.as_ref().map_or_else(
                || {
                    text.split(|c| self.settings.token_separators.contains(&c))
                        .collect()
                },
                |tokens| tokens.iter().map(|s| s.as_ref()).collect(),
            )
        };
        let mut values = self.values.to_vec();
        let mut seen = self.selected.clone();
        // Split-input callbacks in rc-select report every resulting value, even
        // an already selected value. A single tag submission reports only itself.
        let mut selected = if whole {
            Vec::new()
        } else {
            self.values
                .iter()
                .map(|value| {
                    self.options
                        .option(&value.value)
                        .or_else(|| self.created.option(&value.value))
                        .cloned()
                        .unwrap_or_else(|| {
                            SelectOption::with_value_text(
                                ("select-value", self.selection_id(&value.value)),
                                value.value.clone(),
                                value.label.clone(),
                                value.label.clone(),
                            )
                        })
                })
                .collect()
        };
        for token in tokens.into_iter().filter(|s| !s.is_empty()) {
            if self.settings.max_count.is_some_and(|n| values.len() >= n) {
                break;
            }
            let option = if self.settings.mode == SelectMode::Tags {
                self.options.option_by_value_text(token)
            } else {
                self.options.option_by_label(token)
            }
            .cloned()
            .or_else(|| {
                (self.settings.mode == SelectMode::Tags)
                    .then(|| self.tag_factory.as_ref().and_then(|f| f(token)))
                    .flatten()
            });
            // The original treats label tokenization in multiple mode as a
            // direct value assignment. Tags mode explicitly rejects disabled values.
            if let Some(option) =
                option.filter(|o| self.settings.mode != SelectMode::Tags || !o.disabled)
            {
                if seen.insert(option.value.clone()) {
                    values.push(SelectValue::from(&option));
                    selected.push(option);
                } else if whole && !option.disabled {
                    selected.push(option);
                }
            }
        }
        Some((
            SelectionProposal {
                values,
                selected,
                deselected: Vec::new(),
            },
            "".into(),
        ))
    }
}
