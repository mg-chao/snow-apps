// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Explicit coordination between independently retained forms.

use super::{
    model::FieldErrors,
    state::{FormEvent, FormState},
    value::FormSnapshot,
};
use gpui::{App, Context, Entity, EventEmitter, SharedString, WeakEntity};
use std::collections::BTreeMap;

#[derive(Clone, Debug)]
pub enum FormProviderEvent {
    Change {
        name: SharedString,
        changed_fields: Vec<FieldErrors>,
        forms: BTreeMap<SharedString, FormSnapshot>,
    },
    Finish {
        name: SharedString,
        values: FormSnapshot,
        forms: BTreeMap<SharedString, FormSnapshot>,
    },
}

/// Form names are stable identifiers; provider labels remain application-owned.
#[derive(Default)]
pub struct FormProvider {
    forms: BTreeMap<SharedString, WeakEntity<FormState>>,
}
impl EventEmitter<FormProviderEvent> for FormProvider {}
impl FormProvider {
    pub fn new(_cx: &mut Context<Self>) -> Self {
        Self::default()
    }
    pub fn register(
        &mut self,
        name: impl Into<SharedString>,
        form: &Entity<FormState>,
        cx: &mut Context<Self>,
    ) {
        let name = name.into();
        let owner = cx.weak_entity();
        let previous_owner = form.read(cx).provider.clone();
        if let Some((old_owner, old_name)) = previous_owner {
            if old_owner == owner {
                if old_name != name {
                    self.forms.remove(&old_name);
                }
            } else {
                let _ = old_owner.update(cx, |provider, _| {
                    if provider.forms.get(&old_name).is_some_and(|old| old == form) {
                        provider.forms.remove(&old_name);
                    }
                });
            }
        }
        if let Some(previous) = self
            .forms
            .insert(name.clone(), form.downgrade())
            .and_then(|weak| weak.upgrade())
            && previous != *form
        {
            previous.update(cx, |form, _| {
                if form
                    .provider
                    .as_ref()
                    .is_some_and(|(provider, old_name)| *provider == owner && *old_name == name)
                {
                    form.provider = None;
                }
            });
        }
        form.update(cx, |form, _| form.provider = Some((owner, name)));
    }
    pub fn unregister(&mut self, name: &str, cx: &mut Context<Self>) {
        let owner = cx.weak_entity();
        if let Some(form) = self.forms.remove(name).and_then(|form| form.upgrade()) {
            form.update(cx, |form, _| {
                if form.provider.as_ref().is_some_and(|(provider, old_name)| {
                    *provider == owner && old_name.as_ref() == name
                }) {
                    form.provider = None;
                }
            });
        }
    }
    pub fn form(&self, name: &str) -> Option<Entity<FormState>> {
        self.forms.get(name).and_then(WeakEntity::upgrade)
    }
    pub fn forms(&self) -> BTreeMap<SharedString, Entity<FormState>> {
        self.forms
            .iter()
            .filter_map(|(name, form)| form.upgrade().map(|form| (name.clone(), form)))
            .collect()
    }
    pub fn snapshots(&self, cx: &App) -> BTreeMap<SharedString, FormSnapshot> {
        self.forms()
            .into_iter()
            .map(|(name, form)| (name, form.read(cx).snapshot()))
            .collect()
    }
    pub(crate) fn form_event(
        &mut self,
        name: SharedString,
        form_id: u64,
        event: FormEvent,
        cx: &mut Context<Self>,
    ) {
        self.forms.retain(|_, form| form.upgrade().is_some());
        if self
            .form(&name)
            .is_none_or(|form| form.read(cx).id() != form_id)
        {
            return;
        }
        match event {
            FormEvent::FieldsChange { fields } => cx.emit(FormProviderEvent::Change {
                name,
                changed_fields: fields,
                forms: self.snapshots(cx),
            }),
            FormEvent::Finish(values) => cx.emit(FormProviderEvent::Finish {
                name,
                values,
                forms: self.snapshots(cx),
            }),
            _ => {}
        }
    }
}
