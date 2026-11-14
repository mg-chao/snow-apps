// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use gpui::{ElementId, SharedString};
use std::{
    cmp::Ordering,
    collections::{BTreeMap, HashMap, HashSet},
    fmt,
    hash::Hash,
    rc::Rc,
};

/// An option's stable native identity is independent of its value, label, or position.
#[derive(Clone, Debug)]
pub struct SelectOption<T> {
    pub id: ElementId,
    pub value: T,
    pub label: SharedString,
    pub disabled: bool,
    pub title: Option<SharedString>,
    /// Additional application fields available to multi-field search.
    pub search_fields: BTreeMap<SharedString, SharedString>,
    /// A text representation of the value, used by upstream's default filter.
    pub value_text: SharedString,
}
impl<T> SelectOption<T> {
    pub fn new(id: impl Into<ElementId>, value: T, label: impl Into<SharedString>) -> Self
    where
        T: fmt::Display,
    {
        let value_text = value.to_string().into();
        Self {
            id: id.into(),
            value,
            label: label.into(),
            disabled: false,
            title: None,
            search_fields: BTreeMap::new(),
            value_text,
        }
    }
    /// Use a domain value that has no Display implementation with explicit search text.
    pub fn with_value_text(
        id: impl Into<ElementId>,
        value: T,
        value_text: impl Into<SharedString>,
        label: impl Into<SharedString>,
    ) -> Self {
        Self {
            id: id.into(),
            value,
            label: label.into(),
            disabled: false,
            title: None,
            search_fields: BTreeMap::new(),
            value_text: value_text.into(),
        }
    }
    pub fn disabled(mut self, disabled: bool) -> Self {
        self.disabled = disabled;
        self
    }
    pub fn title(mut self, title: impl Into<SharedString>) -> Self {
        self.title = Some(title.into());
        self
    }
    pub fn search_field(
        mut self,
        key: impl Into<SharedString>,
        value: impl Into<SharedString>,
    ) -> Self {
        self.search_fields.insert(key.into(), value.into());
        self
    }
}

#[derive(Clone, Debug)]
pub struct SelectGroup<T> {
    pub id: ElementId,
    pub label: SharedString,
    pub options: Vec<SelectOption<T>>,
}
impl<T> SelectGroup<T> {
    pub fn new(
        id: impl Into<ElementId>,
        label: impl Into<SharedString>,
        options: impl IntoIterator<Item = SelectOption<T>>,
    ) -> Self {
        Self {
            id: id.into(),
            label: label.into(),
            options: options.into_iter().collect(),
        }
    }
}
#[derive(Clone, Debug)]
pub enum SelectEntry<T> {
    Option(SelectOption<T>),
    Group(SelectGroup<T>),
}
impl<T> From<SelectOption<T>> for SelectEntry<T> {
    fn from(option: SelectOption<T>) -> Self {
        Self::Option(option)
    }
}
impl<T> From<SelectGroup<T>> for SelectEntry<T> {
    fn from(group: SelectGroup<T>) -> Self {
        Self::Group(group)
    }
}

#[derive(Clone, Debug)]
pub enum SelectRow<T> {
    Group {
        id: ElementId,
        label: SharedString,
    },
    Option {
        option: SelectOption<T>,
        group: Option<usize>,
    },
}
impl<T> SelectRow<T> {
    pub fn id(&self) -> &ElementId {
        match self {
            Self::Group { id, .. } => id,
            Self::Option { option, .. } => &option.id,
        }
    }
    pub fn option(&self) -> Option<&SelectOption<T>> {
        match self {
            Self::Option { option, .. } => Some(option),
            _ => None,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SelectOptionsError {
    DuplicateId(ElementId),
    DuplicateValue,
}
impl fmt::Display for SelectOptionsError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::DuplicateId(id) => write!(f, "Select option/group ID must be unique: {id:?}"),
            Self::DuplicateValue => f.write_str("Select option values must be unique"),
        }
    }
}
impl std::error::Error for SelectOptionsError {}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SelectSearchField {
    Value,
    Label,
    Field(SharedString),
}
type Filter<T> = Rc<dyn Fn(&str, &SelectOption<T>) -> bool>;
type Sort<T> = Rc<dyn Fn(&SelectOption<T>, &SelectOption<T>, &str) -> Ordering>;
type EntryFilter<T> = Rc<dyn Fn(&str, &SelectRow<T>) -> bool>;
type EntrySort<T> = Rc<dyn Fn(&SelectRow<T>, &SelectRow<T>, &str) -> Ordering>;

/// Search policy is retained alongside options. Reuse it between renders so a
/// Select can reuse a filtered result when neither input nor policy changed.
#[derive(Clone)]
pub struct SelectSearch<T> {
    pub enabled: Option<bool>,
    pub filter_enabled: bool,
    pub fields: Vec<SelectSearchField>,
    pub auto_clear: bool,
    pub filter: Option<Filter<T>>,
    pub sort: Option<Sort<T>>,
    pub entry_filter: Option<EntryFilter<T>>,
    pub entry_sort: Option<EntrySort<T>>,
}
impl<T> Default for SelectSearch<T> {
    fn default() -> Self {
        Self {
            enabled: None,
            filter_enabled: true,
            fields: Vec::new(),
            auto_clear: true,
            filter: None,
            sort: None,
            entry_filter: None,
            entry_sort: None,
        }
    }
}
impl<T> SelectSearch<T> {
    pub fn label() -> Self {
        Self {
            fields: vec![SelectSearchField::Label],
            ..Self::default()
        }
    }
    pub fn fields(fields: impl IntoIterator<Item = SelectSearchField>) -> Self {
        Self {
            fields: fields.into_iter().collect(),
            ..Self::default()
        }
    }
    pub fn filter(mut self, filter: impl Fn(&str, &SelectOption<T>) -> bool + 'static) -> Self {
        self.filter = Some(Rc::new(filter));
        self
    }
    pub fn sort(
        mut self,
        sort: impl Fn(&SelectOption<T>, &SelectOption<T>, &str) -> Ordering + 'static,
    ) -> Self {
        self.sort = Some(Rc::new(sort));
        self
    }
    pub fn equivalent(&self, other: &Self) -> bool {
        self.enabled == other.enabled
            && self.filter_enabled == other.filter_enabled
            && self.fields == other.fields
            && self.auto_clear == other.auto_clear
            && match (&self.filter, &other.filter) {
                (None, None) => true,
                (Some(a), Some(b)) => Rc::ptr_eq(a, b),
                _ => false,
            }
            && match (&self.sort, &other.sort) {
                (None, None) => true,
                (Some(a), Some(b)) => Rc::ptr_eq(a, b),
                _ => false,
            }
            && match (&self.entry_filter, &other.entry_filter) {
                (None, None) => true,
                (Some(a), Some(b)) => Rc::ptr_eq(a, b),
                _ => false,
            }
            && match (&self.entry_sort, &other.entry_sort) {
                (None, None) => true,
                (Some(a), Some(b)) => Rc::ptr_eq(a, b),
                _ => false,
            }
    }

    /// Filter group headers as well as options. A matching group includes all
    /// its children; otherwise each child is checked. Takes precedence over `filter`.
    pub fn filter_entries(
        mut self,
        filter: impl Fn(&str, &SelectRow<T>) -> bool + 'static,
    ) -> Self {
        self.entry_filter = Some(Rc::new(filter));
        self
    }

    /// Sort top-level groups/options and the options within each group without
    /// detaching children from their group. Takes precedence over `sort`.
    pub fn sort_entries(
        mut self,
        sort: impl Fn(&SelectRow<T>, &SelectRow<T>, &str) -> Ordering + 'static,
    ) -> Self {
        self.entry_sort = Some(Rc::new(sort));
        self
    }
}

struct SearchText {
    value: String,
    label: String,
    fields: BTreeMap<SharedString, String>,
}
struct Data<T> {
    rows: Vec<SelectRow<T>>,
    lookup: HashMap<T, usize>,
    ids: HashMap<ElementId, usize>,
    labels: HashMap<SharedString, usize>,
    value_texts: HashMap<SharedString, usize>,
    search: Vec<SearchText>,
    all: Rc<[usize]>,
    all_positions: Rc<[usize]>,
}

/// Immutable, indexed options. Cloning a collection shares its rows and lookup
/// tables; it never copies the dataset. Replace the collection to publish changes.
pub struct SelectOptions<T>(Rc<Data<T>>);
impl<T> Clone for SelectOptions<T> {
    fn clone(&self) -> Self {
        Self(self.0.clone())
    }
}
impl<T> Default for SelectOptions<T> {
    fn default() -> Self {
        Self(Rc::new(Data {
            rows: Vec::new(),
            lookup: HashMap::new(),
            ids: HashMap::new(),
            labels: HashMap::new(),
            value_texts: HashMap::new(),
            search: Vec::new(),
            all: Rc::from([]),
            all_positions: Rc::from([]),
        }))
    }
}
impl<T> fmt::Debug for SelectOptions<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("SelectOptions")
            .field("rows", &self.0.rows.len())
            .finish_non_exhaustive()
    }
}

#[derive(Clone, Debug)]
pub struct SelectMatches {
    rows: Rc<[usize]>,
    // One-based option ordinals; zero denotes a group heading. Cached with the
    // filtered order so accessibility never scans preceding rows during a draw.
    positions: Rc<[usize]>,
    sorted_positions: Option<Rc<HashMap<usize, usize>>>,
    option_count: usize,
}
impl SelectMatches {
    pub fn rows(&self) -> &[usize] {
        &self.rows
    }
    pub fn option_count(&self) -> usize {
        self.option_count
    }
    pub fn option_position(&self, row: usize) -> Option<usize> {
        self.positions
            .get(row)
            .copied()
            .filter(|position| *position != 0)
    }
    /// Locate a source row without scanning a filtered or custom-sorted list.
    pub fn row_position(&self, source_row: usize) -> Option<usize> {
        match &self.sorted_positions {
            Some(positions) => positions.get(&source_row).copied(),
            None => self.rows.binary_search(&source_row).ok(),
        }
    }
    pub fn is_empty(&self) -> bool {
        self.option_count == 0
    }
    pub fn same_rows(&self, other: &Self) -> bool {
        Rc::ptr_eq(&self.rows, &other.rows)
    }
}

impl<T: Clone + Eq + Hash> SelectOptions<T> {
    pub fn new(
        options: impl IntoIterator<Item = SelectOption<T>>,
    ) -> Result<Self, SelectOptionsError> {
        Self::from_entries(options.into_iter().map(SelectEntry::Option))
    }
    pub fn from_entries(
        entries: impl IntoIterator<Item = SelectEntry<T>>,
    ) -> Result<Self, SelectOptionsError> {
        let mut rows = Vec::new();
        for entry in entries {
            match entry {
                SelectEntry::Option(option) => rows.push(SelectRow::Option {
                    option,
                    group: None,
                }),
                SelectEntry::Group(group) => {
                    let index = rows.len();
                    rows.push(SelectRow::Group {
                        id: group.id,
                        label: group.label,
                    });
                    rows.extend(group.options.into_iter().map(|option| SelectRow::Option {
                        option,
                        group: Some(index),
                    }));
                }
            }
        }
        let mut lookup = HashMap::with_capacity(rows.len());
        let mut ids = HashMap::with_capacity(rows.len());
        let mut labels = HashMap::with_capacity(rows.len());
        let mut value_texts = HashMap::with_capacity(rows.len());
        let mut search = Vec::with_capacity(rows.len());
        for (index, row) in rows.iter().enumerate() {
            if ids.insert(row.id().clone(), index).is_some() {
                return Err(SelectOptionsError::DuplicateId(row.id().clone()));
            }
            match row {
                SelectRow::Option { option, .. } => {
                    labels.insert(option.label.clone(), index);
                    value_texts.insert(option.value_text.clone(), index);
                    if lookup.insert(option.value.clone(), index).is_some() {
                        return Err(SelectOptionsError::DuplicateValue);
                    }
                    search.push(SearchText {
                        value: option.value_text.to_uppercase(),
                        label: option.label.to_uppercase(),
                        fields: option
                            .search_fields
                            .iter()
                            .map(|(key, value)| (key.clone(), value.to_uppercase()))
                            .collect(),
                    });
                }
                SelectRow::Group { label, .. } => search.push(SearchText {
                    value: String::new(),
                    label: label.to_uppercase(),
                    fields: BTreeMap::new(),
                }),
            }
        }
        let all = (0..rows.len()).collect::<Vec<_>>().into();
        let mut ordinal = 0;
        let all_positions = rows
            .iter()
            .map(|row| {
                if row.option().is_some() {
                    ordinal += 1;
                    ordinal
                } else {
                    0
                }
            })
            .collect::<Vec<_>>()
            .into();
        Ok(Self(Rc::new(Data {
            rows,
            lookup,
            ids,
            labels,
            value_texts,
            search,
            all,
            all_positions,
        })))
    }
    pub fn same_collection(&self, other: &Self) -> bool {
        Rc::ptr_eq(&self.0, &other.0)
    }
    pub fn rows(&self) -> &[SelectRow<T>] {
        &self.0.rows
    }
    pub fn option_count(&self) -> usize {
        self.0.lookup.len()
    }
    pub fn option(&self, value: &T) -> Option<&SelectOption<T>> {
        self.0
            .lookup
            .get(value)
            .and_then(|i| self.0.rows[*i].option())
    }
    pub fn row_for_value(&self, value: &T) -> Option<usize> {
        self.0.lookup.get(value).copied()
    }
    pub fn option_by_label(&self, label: &str) -> Option<&SelectOption<T>> {
        self.0
            .labels
            .get(label)
            .and_then(|i| self.0.rows[*i].option())
    }
    pub fn option_by_value_text(&self, value: &str) -> Option<&SelectOption<T>> {
        self.0
            .value_texts
            .get(value)
            .and_then(|i| self.0.rows[*i].option())
    }
    pub fn row_for_id(&self, id: &ElementId) -> Option<usize> {
        self.0.ids.get(id).copied()
    }
    pub fn all(&self) -> SelectMatches {
        SelectMatches {
            rows: self.0.all.clone(),
            positions: self.0.all_positions.clone(),
            sorted_positions: None,
            option_count: self.option_count(),
        }
    }
    pub fn search(&self, query: &str, policy: &SelectSearch<T>) -> SelectMatches {
        self.search_excluding(query, policy, &HashSet::new())
    }
    pub fn search_excluding(
        &self,
        query: &str,
        policy: &SelectSearch<T>,
        excluded: &HashSet<T>,
    ) -> SelectMatches {
        let filtering = policy.filter_enabled && !query.is_empty();
        if !filtering && policy.sort.is_none() && policy.entry_sort.is_none() && excluded.is_empty()
        {
            return self.all();
        }
        let upper = query.to_uppercase();
        let matches_text = |index: usize, group: bool| {
            let text = &self.0.search[index];
            if policy.fields.is_empty() {
                return if group {
                    text.label.contains(&upper)
                } else {
                    text.value.contains(&upper)
                };
            }
            policy.fields.iter().any(|field| match field {
                SelectSearchField::Value => !group && text.value.contains(&upper),
                SelectSearchField::Label => text.label.contains(&upper),
                SelectSearchField::Field(key) => text
                    .fields
                    .get(key)
                    .is_some_and(|text| text.contains(&upper)),
            })
        };
        let mut rows = Vec::new();
        let mut option_count = 0;
        let mut group = None;
        let mut group_matches = false;
        let mut children = Vec::new();
        let mut blocks = Vec::new();
        let mut flush = |group: Option<usize>, children: &mut Vec<usize>, rows: &mut Vec<usize>| {
            if children.is_empty() {
                return;
            }
            if let Some(sort) = &policy.entry_sort {
                children.sort_by(|a, b| sort(&self.0.rows[*a], &self.0.rows[*b], query));
            } else if let Some(sort) = &policy.sort {
                children.sort_by(|a, b| {
                    sort(
                        self.0.rows[*a].option().unwrap(),
                        self.0.rows[*b].option().unwrap(),
                        query,
                    )
                });
            }
            let start = rows.len();
            if let Some(group) = group {
                rows.push(group);
            }
            rows.append(children);
            if policy.entry_sort.is_some() {
                if group.is_some() {
                    blocks.push(start..rows.len());
                } else {
                    blocks.extend((start..rows.len()).map(|index| index..index + 1));
                }
            }
        };
        for (index, row) in self.0.rows.iter().enumerate() {
            match row {
                SelectRow::Group { .. } => {
                    flush(group, &mut children, &mut rows);
                    group = Some(index);
                    group_matches = filtering
                        && policy.entry_filter.as_ref().map_or_else(
                            || policy.filter.is_none() && matches_text(index, true),
                            |filter| filter(query, row),
                        );
                }
                SelectRow::Option {
                    option,
                    group: own_group,
                } => {
                    if *own_group != group {
                        flush(group, &mut children, &mut rows);
                        group = *own_group;
                        group_matches = false;
                    }
                    if excluded.contains(&option.value) {
                        continue;
                    }
                    if !filtering
                        || group_matches
                        || policy.entry_filter.as_ref().map_or_else(
                            || {
                                policy.filter.as_ref().map_or_else(
                                    || matches_text(index, false),
                                    |f| f(query, option),
                                )
                            },
                            |filter| filter(query, row),
                        )
                    {
                        children.push(index);
                        option_count += 1;
                    }
                }
            }
        }
        flush(group, &mut children, &mut rows);
        if let Some(sort) = &policy.entry_sort {
            blocks.sort_by(|a, b| {
                sort(
                    &self.0.rows[rows[a.start]],
                    &self.0.rows[rows[b.start]],
                    query,
                )
            });
            rows = blocks
                .into_iter()
                .flat_map(|block| rows[block].iter().copied())
                .collect();
        }
        let mut ordinal = 0;
        let positions = rows
            .iter()
            .map(|index| {
                if self.0.rows[*index].option().is_some() {
                    ordinal += 1;
                    ordinal
                } else {
                    0
                }
            })
            .collect::<Vec<_>>()
            .into();
        let sorted_positions = (policy.sort.is_some() || policy.entry_sort.is_some()).then(|| {
            Rc::new(
                rows.iter()
                    .copied()
                    .enumerate()
                    .map(|(position, row)| (row, position))
                    .collect(),
            )
        });
        SelectMatches {
            rows: rows.into(),
            positions,
            sorted_positions,
            option_count,
        }
    }
}
