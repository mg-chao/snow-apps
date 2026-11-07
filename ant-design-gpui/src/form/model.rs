// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{
    collections::{BTreeMap, BTreeSet, HashMap},
    fmt,
    rc::Rc,
    time::Duration,
};

use gpui::SharedString;

use crate::ValidationStatus;

use super::{
    rules::{FormRule, RuleMode, ValidationTrigger},
    value::{
        FormObject, FormPathError, FormSnapshot, FormTypeError, FormValue, NamePath, NameSegment,
    },
};

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct FieldId(pub u64);

#[derive(Clone, Debug, Default, PartialEq)]
pub struct FieldMeta {
    pub touched: bool,
    pub dirty: bool,
    pub validated: bool,
    pub validating: bool,
    pub errors: Vec<SharedString>,
    pub warnings: Vec<SharedString>,
    pub required: bool,
}
impl FieldMeta {
    pub fn status(&self) -> ValidationStatus {
        if self.validating {
            ValidationStatus::Validating
        } else if !self.errors.is_empty() {
            ValidationStatus::Error
        } else if !self.warnings.is_empty() {
            ValidationStatus::Warning
        } else if self.validated || self.touched {
            ValidationStatus::Success
        } else {
            ValidationStatus::None
        }
    }
}

#[derive(Clone)]
pub struct FieldOptions {
    pub label: SharedString,
    pub initial_value: Option<FormValue>,
    pub rules: Vec<FormRule>,
    pub dependencies: Vec<NamePath>,
    pub validate_trigger: Vec<ValidationTrigger>,
    pub validate_debounce: Duration,
    pub validate_first: RuleMode,
    pub preserve: bool,
    pub hidden: bool,
    pub required: Option<bool>,
    pub normalize: Option<FormNormalize>,
    pub message_variables: BTreeMap<SharedString, SharedString>,
}
pub type FormNormalize = Rc<dyn Fn(FormValue, &FormValue, &FormSnapshot) -> FormValue>;
impl Default for FieldOptions {
    fn default() -> Self {
        Self {
            label: SharedString::default(),
            initial_value: None,
            rules: Vec::new(),
            dependencies: Vec::new(),
            validate_trigger: vec![ValidationTrigger::Change],
            validate_debounce: Duration::ZERO,
            validate_first: RuleMode::ParallelAll,
            preserve: true,
            hidden: false,
            required: None,
            normalize: None,
            message_variables: BTreeMap::new(),
        }
    }
}
impl FieldOptions {
    pub fn label(mut self, label: impl Into<SharedString>) -> Self {
        self.label = label.into();
        self
    }
    pub fn rule(mut self, rule: FormRule) -> Self {
        self.rules.push(rule);
        self
    }
    pub fn rules(mut self, rules: impl IntoIterator<Item = FormRule>) -> Self {
        self.rules.extend(rules);
        self
    }
    pub fn initial_value(mut self, value: impl Into<FormValue>) -> Self {
        self.initial_value = Some(value.into());
        self
    }
    pub fn dependencies(mut self, values: impl IntoIterator<Item = impl Into<NamePath>>) -> Self {
        self.dependencies = values.into_iter().map(Into::into).collect();
        self
    }
    pub fn preserve(mut self, value: bool) -> Self {
        self.preserve = value;
        self
    }
    pub fn hidden(mut self, value: bool) -> Self {
        self.hidden = value;
        self
    }
    pub fn required(mut self, value: bool) -> Self {
        self.required = Some(value);
        self
    }
    pub fn validate_trigger(mut self, value: impl IntoIterator<Item = ValidationTrigger>) -> Self {
        self.validate_trigger = value.into_iter().collect();
        self
    }
    pub fn debounce(mut self, value: Duration) -> Self {
        self.validate_debounce = value;
        self
    }
    pub fn validate_first(mut self, value: RuleMode) -> Self {
        self.validate_first = value;
        self
    }
    pub fn normalize(
        mut self,
        normalize: impl Fn(FormValue, &FormValue, &FormSnapshot) -> FormValue + 'static,
    ) -> Self {
        self.normalize = Some(Rc::new(normalize));
        self
    }
    pub fn message_variable(
        mut self,
        key: impl Into<SharedString>,
        value: impl Into<SharedString>,
    ) -> Self {
        self.message_variables.insert(key.into(), value.into());
        self
    }
    pub fn is_required(&self) -> bool {
        self.required.unwrap_or_else(|| {
            self.rules
                .iter()
                .any(|rule| rule.required && !rule.warning_only)
        })
    }
}
impl fmt::Debug for FieldOptions {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("FieldOptions")
            .field("label", &self.label)
            .field("rules", &self.rules.len())
            .field("dependencies", &self.dependencies)
            .field("preserve", &self.preserve)
            .finish_non_exhaustive()
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum FormError {
    Path(FormPathError),
    Type(FormTypeError),
    UnknownField(FieldId),
    ForeignField,
    DuplicateInitialValue(NamePath),
    InvalidList(NamePath),
    Detached,
    Control(SharedString),
}
impl From<FormPathError> for FormError {
    fn from(value: FormPathError) -> Self {
        Self::Path(value)
    }
}
impl From<FormTypeError> for FormError {
    fn from(value: FormTypeError) -> Self {
        Self::Type(value)
    }
}
impl fmt::Display for FormError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Path(error) => error.fmt(f),
            Self::Type(error) => error.fmt(f),
            Self::UnknownField(id) => write!(f, "unknown form field {}", id.0),
            Self::ForeignField => f.write_str("field belongs to another form"),
            Self::DuplicateInitialValue(name) => write!(f, "multiple initial values for {name}"),
            Self::InvalidList(name) => write!(f, "{name} is not a form array"),
            Self::Detached => f.write_str("form is no longer available"),
            Self::Control(message) => f.write_str(message),
        }
    }
}
impl std::error::Error for FormError {}
pub type FormReadError = FormError;

#[derive(Clone, Debug, Default)]
pub struct FieldErrors {
    pub name: NamePath,
    pub errors: Vec<SharedString>,
    pub warnings: Vec<SharedString>,
}

#[derive(Clone, Debug)]
pub struct FormValidationError {
    pub values: FormSnapshot,
    pub error_fields: Vec<FieldErrors>,
    pub out_of_date: bool,
}

#[derive(Clone, Debug, Default)]
pub struct ValidateOptions {
    pub names: Option<Vec<NamePath>>,
    pub recursive: bool,
    pub dirty: bool,
    pub validate_only: bool,
}

#[derive(Clone, Debug, Default)]
pub struct FieldPatch {
    pub name: NamePath,
    pub value: Option<FormValue>,
    pub touched: Option<bool>,
    pub validating: Option<bool>,
    pub errors: Option<Vec<SharedString>>,
    pub warnings: Option<Vec<SharedString>>,
}

#[derive(Default)]
struct IndexNode {
    exact: BTreeMap<FieldId, usize>,
    descendants: BTreeMap<FieldId, usize>,
    children: HashMap<NameSegment, IndexNode>,
}

/// Prefix queries touch ancestors and matching descendants, not unrelated fields.
#[derive(Default)]
pub(crate) struct PathIndex(IndexNode);
impl PathIndex {
    pub fn insert(&mut self, path: &NamePath, id: FieldId) {
        let mut node = &mut self.0;
        *node.descendants.entry(id).or_default() += 1;
        for segment in path.segments() {
            node = node.children.entry(segment.clone()).or_default();
            *node.descendants.entry(id).or_default() += 1;
        }
        *node.exact.entry(id).or_default() += 1;
    }
    pub fn remove(&mut self, path: &NamePath, id: FieldId) {
        fn decrement(counts: &mut BTreeMap<FieldId, usize>, id: FieldId) {
            if let Some(count) = counts.get_mut(&id) {
                *count -= 1;
                if *count == 0 {
                    counts.remove(&id);
                }
            }
        }
        fn remove(node: &mut IndexNode, path: &[NameSegment], id: FieldId) -> bool {
            if let Some((head, tail)) = path.split_first() {
                if let Some(child) = node.children.get_mut(head) {
                    let removed = remove(child, tail, id);
                    if child.descendants.is_empty() {
                        node.children.remove(head);
                    }
                    if removed {
                        decrement(&mut node.descendants, id);
                    }
                    return removed;
                }
                false
            } else if node.exact.contains_key(&id) {
                decrement(&mut node.exact, id);
                decrement(&mut node.descendants, id);
                true
            } else {
                false
            }
        }
        remove(&mut self.0, path.segments(), id);
    }
    pub fn intersects(&self, path: &NamePath) -> BTreeSet<FieldId> {
        let mut node = &self.0;
        let mut result = node.exact.keys().copied().collect::<BTreeSet<_>>();
        for segment in path.segments() {
            let Some(child) = node.children.get(segment) else {
                return result;
            };
            node = child;
            result.extend(node.exact.keys().copied());
        }
        result.extend(node.descendants.keys().copied());
        result
    }
}

pub(crate) struct FieldRecord {
    pub name: NamePath,
    pub options: Rc<FieldOptions>,
    pub meta: FieldMeta,
    pub generation: u64,
    pub active: bool,
}

pub(crate) struct FormStore {
    pub snapshot: FormSnapshot,
    pub initial: FormValue,
    pub fields: BTreeMap<FieldId, FieldRecord>,
    pub index: PathIndex,
    pub dependencies: PathIndex,
    next_id: u64,
}
impl FormStore {
    pub fn new(form_id: u64) -> Self {
        let mut snapshot = FormSnapshot::empty();
        snapshot.form_id = form_id;
        Self {
            initial: snapshot.root.clone(),
            snapshot,
            fields: BTreeMap::new(),
            index: PathIndex::default(),
            dependencies: PathIndex::default(),
            next_id: 1,
        }
    }
    pub fn value(&self, name: &NamePath) -> FormValue {
        self.snapshot.get(name).cloned().unwrap_or_default()
    }
    pub fn register(
        &mut self,
        name: NamePath,
        options: FieldOptions,
    ) -> Result<FieldId, FormError> {
        if options.initial_value.is_some()
            && self.fields.values().any(|record| {
                record.active && record.name == name && record.options.initial_value.is_some()
            })
        {
            return Err(FormError::DuplicateInitialValue(name));
        }
        if self.snapshot.get(&name).is_none_or(FormValue::is_undefined)
            && let Some(value) = &options.initial_value
        {
            self.snapshot.root = self.snapshot.root.with_path(&name, value.clone())?;
            self.snapshot.revision += 1;
        }
        let id = FieldId(self.next_id);
        self.next_id += 1;
        self.index.insert(&name, id);
        for dependency in &options.dependencies {
            self.dependencies.insert(dependency, id);
        }
        self.snapshot.field_paths.insert(id.0, name.clone());
        let meta = FieldMeta {
            required: options.is_required(),
            dirty: !self.value(&name).is_undefined(),
            ..FieldMeta::default()
        };
        self.fields.insert(
            id,
            FieldRecord {
                name,
                options: Rc::new(options),
                meta,
                generation: 0,
                active: true,
            },
        );
        Ok(id)
    }
    pub fn set(&mut self, name: &NamePath, value: FormValue) -> Result<bool, FormError> {
        if name.is_empty() && value.as_object().is_none() {
            return Err(FormTypeError {
                expected: "object",
                actual: value.type_name(),
            }
            .into());
        }
        if self.value(name) == value {
            return Ok(false);
        }
        self.snapshot.root = self.snapshot.root.with_path(name, value)?;
        self.snapshot.revision += 1;
        Ok(true)
    }
    pub fn replace(&mut self, value: FormValue) -> Result<bool, FormError> {
        if value.as_object().is_none() {
            return Err(FormError::Type(FormTypeError {
                expected: "object",
                actual: value.type_name(),
            }));
        }
        if self.snapshot.root == value {
            return Ok(false);
        }
        self.snapshot.root = value;
        self.snapshot.revision += 1;
        Ok(true)
    }
    pub fn remove_field(&mut self, id: FieldId) -> Result<Option<NamePath>, FormError> {
        let record = self.fields.remove(&id).ok_or(FormError::UnknownField(id))?;
        self.index.remove(&record.name, id);
        for dependency in &record.options.dependencies {
            self.dependencies.remove(dependency, id);
        }
        self.snapshot.field_paths.remove(&id.0);
        if !record.options.preserve
            && !self
                .fields
                .values()
                .any(|field| field.active && field.name == record.name)
        {
            let initial = self.initial.get(&record.name).cloned().unwrap_or_default();
            self.set(&record.name, initial)?;
            return Ok(Some(record.name));
        }
        Ok(None)
    }
    pub fn registered_values(&self) -> FormValue {
        let mut values: FormValue = FormObject::new().into();
        // Parents first avoid attempting to traverse an already materialized scalar.
        let mut names = self
            .fields
            .values()
            .filter(|r| r.active)
            .map(|r| &r.name)
            .collect::<Vec<_>>();
        names.sort_by_key(|name| name.len());
        for name in names {
            if let Some(value) = self.snapshot.get(name)
                && let Ok(next) = values.with_path(name, value.clone())
            {
                values = next;
            }
        }
        values
    }
    pub fn selected(&self, options: &ValidateOptions) -> Vec<FieldId> {
        self.fields
            .iter()
            .filter(|(_, record)| {
                record.active
                    && (!options.dirty
                        || record.meta.dirty
                        || record.meta.touched
                        || record.meta.validated)
                    && options.names.as_ref().is_none_or(|names| {
                        names.iter().any(|name| {
                            record.name == *name
                                || options.recursive && record.name.starts_with(name)
                        })
                    })
            })
            .map(|(id, _)| *id)
            .collect()
    }
}
