// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Array operations preserve row identities independently from numeric names.

use super::{
    model::{FieldOptions, FormError},
    state::{FieldHandle, FormEvent, FormState},
    value::{FormArray, FormValue, NamePath, NameSegment},
};
use gpui::{App, Context, Entity, WeakEntity, Window};
use std::{
    cell::RefCell,
    collections::{BTreeMap, BTreeSet},
    rc::Rc,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct FormListKey(pub u64);

#[derive(Clone, Debug)]
pub struct FormListItem {
    pub key: FormListKey,
    pub index: usize,
    pub name: NamePath,
}
impl FormListItem {
    pub fn field(&self, name: impl Into<NameSegment>) -> NamePath {
        self.name.child(name)
    }
}

pub(crate) struct ListRecord {
    path: Rc<RefCell<NamePath>>,
    keys: Vec<FormListKey>,
    field: FieldHandle,
}

#[derive(Clone)]
pub struct FormList {
    form: WeakEntity<FormState>,
    path: Rc<RefCell<NamePath>>,
    field: FieldHandle,
}
impl FormList {
    pub fn new(
        form: &Entity<FormState>,
        name: impl Into<NamePath>,
        initial: Option<FormArray>,
        cx: &mut App,
    ) -> Result<Self, FormError> {
        form.update(cx, |form, cx| form.register_list(name, initial, cx))
    }
    pub fn name(&self) -> NamePath {
        self.path.borrow().clone()
    }
    pub fn field(&self) -> FieldHandle {
        self.field.clone()
    }
    pub fn items(&self, cx: &App) -> Vec<FormListItem> {
        let name = self.name();
        self.form
            .upgrade()
            .and_then(|form| {
                form.read(cx).lists.get(&name).map(|record| {
                    record
                        .keys
                        .iter()
                        .enumerate()
                        .map(|(index, key)| FormListItem {
                            key: *key,
                            index,
                            name: name.index(index),
                        })
                        .collect()
                })
            })
            .unwrap_or_default()
    }
    pub fn add(
        &self,
        value: FormValue,
        index: Option<usize>,
        window: &mut Window,
        cx: &mut App,
    ) -> Result<FormListKey, FormError> {
        self.form
            .update(cx, |form, cx| {
                let name = self.name();
                form.reconcile_list(&name);
                let mut values = form
                    .store
                    .value(&name)
                    .as_array()
                    .cloned()
                    .unwrap_or_default();
                let index = index
                    .filter(|index| *index <= values.len())
                    .unwrap_or(values.len());
                let key = FormListKey(form.next_list_key);
                form.next_list_key += 1;
                let old_len = values.len();
                values.insert(index, value);
                let record = form
                    .lists
                    .get_mut(&name)
                    .ok_or_else(|| FormError::InvalidList(name.clone()))?;
                record.keys.insert(index, key);
                let mapping = (0..old_len)
                    .map(|old| (old, Some(if old >= index { old + 1 } else { old })))
                    .collect();
                form.update_list(&name, values, mapping, window, cx)?;
                Ok(key)
            })
            .map_err(|_| FormError::Detached)?
    }
    pub fn remove(
        &self,
        indices: impl IntoIterator<Item = usize>,
        window: &mut Window,
        cx: &mut App,
    ) -> Result<(), FormError> {
        let indices = indices.into_iter().collect::<BTreeSet<_>>();
        self.form
            .update(cx, |form, cx| {
                let name = self.name();
                form.reconcile_list(&name);
                let values = form
                    .store
                    .value(&name)
                    .as_array()
                    .cloned()
                    .unwrap_or_default();
                let indices = indices
                    .into_iter()
                    .filter(|index| *index < values.len())
                    .collect::<BTreeSet<_>>();
                if indices.is_empty() {
                    return Ok(());
                }
                let mut mapping = BTreeMap::new();
                let mut next = 0;
                for old in 0..values.len() {
                    if indices.contains(&old) {
                        mapping.insert(old, None);
                    } else {
                        mapping.insert(old, Some(next));
                        next += 1;
                    }
                }
                let values = values
                    .iter()
                    .enumerate()
                    .filter(|(i, _)| !indices.contains(i))
                    .map(|(_, v)| v.clone())
                    .collect::<FormArray>();
                let record = form
                    .lists
                    .get_mut(&name)
                    .ok_or_else(|| FormError::InvalidList(name.clone()))?;
                record.keys = record
                    .keys
                    .iter()
                    .enumerate()
                    .filter(|(i, _)| !indices.contains(i))
                    .map(|(_, key)| *key)
                    .collect();
                form.update_list(&name, values, mapping, window, cx)
            })
            .map_err(|_| FormError::Detached)?
    }
    pub fn move_item(
        &self,
        from: usize,
        to: usize,
        window: &mut Window,
        cx: &mut App,
    ) -> Result<(), FormError> {
        self.form
            .update(cx, |form, cx| {
                let name = self.name();
                form.reconcile_list(&name);
                let mut values = form
                    .store
                    .value(&name)
                    .as_array()
                    .cloned()
                    .unwrap_or_default();
                if from == to || from >= values.len() || to >= values.len() {
                    return Ok(());
                }
                let value = values.remove(from).expect("valid row index");
                values.insert(to, value);
                let record = form
                    .lists
                    .get_mut(&name)
                    .ok_or_else(|| FormError::InvalidList(name.clone()))?;
                let key = record.keys.remove(from);
                record.keys.insert(to, key);
                let mut order = (0..values.len()).collect::<Vec<_>>();
                let moved = order.remove(from);
                order.insert(to, moved);
                let mapping = order
                    .into_iter()
                    .enumerate()
                    .map(|(new, old)| (old, Some(new)))
                    .collect();
                form.update_list(&name, values, mapping, window, cx)
            })
            .map_err(|_| FormError::Detached)?
    }
}

impl FormState {
    pub fn register_list(
        &mut self,
        name: impl Into<NamePath>,
        initial: Option<FormArray>,
        cx: &mut Context<Self>,
    ) -> Result<FormList, FormError> {
        let name = name.into();
        if let Some(record) = self.lists.get(&name) {
            return Ok(FormList {
                form: cx.weak_entity(),
                path: record.path.clone(),
                field: record.field.clone(),
            });
        }
        let value = initial.unwrap_or_default();
        if self.store.initial.get(&name).is_none() {
            self.store.initial = self.store.initial.with_path(&name, value.clone().into())?;
        }
        let field = self.register_raw(
            name.clone(),
            FieldOptions::default().initial_value(value),
            cx,
        )?;
        let path = Rc::new(RefCell::new(name.clone()));
        self.lists.insert(
            name.clone(),
            ListRecord {
                path: path.clone(),
                keys: Vec::new(),
                field: field.clone(),
            },
        );
        self.reconcile_list(&name);
        Ok(FormList {
            form: cx.weak_entity(),
            path,
            field,
        })
    }
    pub(crate) fn reconcile_list(&mut self, name: &NamePath) {
        let len = self.store.value(name).as_array().map_or(0, FormArray::len);
        if let Some(record) = self.lists.get_mut(name) {
            record.keys.truncate(len);
            while record.keys.len() < len {
                record.keys.push(FormListKey(self.next_list_key));
                self.next_list_key += 1;
            }
        }
    }
    pub(crate) fn reconcile_lists(&mut self, paths: &[NamePath]) {
        let names = self
            .lists
            .keys()
            .filter(|name| paths.iter().any(|path| path.intersects(name)))
            .cloned()
            .collect::<Vec<_>>();
        for name in names {
            self.reconcile_list(&name);
        }
    }
    fn update_list(
        &mut self,
        name: &NamePath,
        values: FormArray,
        mapping: BTreeMap<usize, Option<usize>>,
        _window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Result<(), FormError> {
        self.store.set(name, values.into())?;
        fn remap(
            path: &NamePath,
            base: &NamePath,
            mapping: &BTreeMap<usize, Option<usize>>,
        ) -> Option<NamePath> {
            if !path.starts_with(base) || path.len() <= base.len() {
                return Some(path.clone());
            }
            let NameSegment::Index(index) = path.segments()[base.len()] else {
                return Some(path.clone());
            };
            let new = mapping.get(&index).copied().flatten()?;
            Some(path.replace_prefix(&base.index(index), &base.index(new)))
        }
        let ids = self.store.fields.keys().copied().collect::<Vec<_>>();
        let mut removed = Vec::new();
        for id in &ids {
            let record = &self.store.fields[id];
            let old = record.name.clone();
            let next = remap(&old, name, &mapping);
            let active = record.active;
            if old.starts_with(name) {
                self.cancel_field(*id);
            }
            let Some(next) = next else {
                removed.push(*id);
                continue;
            };
            if old != next {
                if active {
                    self.store.index.remove(&old, *id);
                    self.store.index.insert(&next, *id);
                }
                self.store.snapshot.field_paths.insert(id.0, next.clone());
                self.store.fields.get_mut(id).expect("field exists").name = next;
            }
            let dependencies = self.store.fields[id].options.dependencies.clone();
            for dependency in &dependencies {
                if active {
                    self.store.dependencies.remove(dependency, *id);
                }
            }
            let dependencies = dependencies
                .iter()
                .filter_map(|path| remap(path, name, &mapping))
                .collect::<Vec<_>>();
            for dependency in &dependencies {
                if active {
                    self.store.dependencies.insert(dependency, *id);
                }
            }
            if self.store.fields[id].options.dependencies != dependencies {
                Rc::make_mut(&mut self.store.fields.get_mut(id).expect("field exists").options)
                    .dependencies = dependencies;
            }
        }
        for id in removed {
            // List removal owns the array; field non-preservation must not restore a row.
            if let Some(record) = self.store.fields.get_mut(&id) {
                Rc::make_mut(&mut record.options).preserve = true;
            }
            self.store.remove_field(id)?;
            self.retire_field(id, cx);
        }
        let lists = std::mem::take(&mut self.lists);
        for (old, record) in lists {
            if let Some(next) = remap(&old, name, &mapping) {
                *record.path.borrow_mut() = next.clone();
                self.lists.insert(next, record);
            }
        }
        let list_id = self.lists.get(name).map(|record| record.field.id());
        if let Some(id) = list_id
            && let Some(record) = self.store.fields.get_mut(&id)
        {
            record.meta.touched = true;
            record.meta.dirty = true;
            record.meta.validated = false;
            record.meta.errors.clear();
            record.meta.warnings.clear();
        }
        let ids = self.store.fields.keys().copied().collect::<Vec<_>>();
        self.sync_fields(&ids, false, cx);
        self.notify_watches(std::slice::from_ref(name), cx);
        let changed = FormValue::from(super::value::FormObject::new())
            .with_path(name, self.store.value(name))?;
        self.emit(
            FormEvent::ValuesChange {
                changed,
                values: self.snapshot(),
            },
            cx,
        );
        Ok(())
    }
}
