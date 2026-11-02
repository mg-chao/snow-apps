// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{
    cell::RefCell,
    collections::{BTreeMap, BTreeSet},
    future::Future,
    pin::Pin,
    rc::Rc,
    sync::atomic::{AtomicU64, Ordering},
    task::{Context as TaskContext, Poll, Waker},
};

use gpui::{
    App, AppContext, Bounds, Context, Entity, EventEmitter, FocusHandle, Pixels, SharedString,
    Subscription, Task, WeakEntity, Window,
};

use crate::ConfigScope;

use super::{
    model::{
        FieldErrors, FieldId, FieldMeta, FieldOptions, FieldPatch, FormError, FormReadError,
        FormStore, FormValidationError, PathIndex, ValidateOptions,
    },
    rules::{FormValidationRequest, RuleMode, ValidationTrigger, collect_rule_issues},
    value::{FormCodec, FormObject, FormPresence, FormSnapshot, FormValue, NamePath},
};

#[derive(Clone, Debug)]
pub enum FormEvent {
    ValuesChange {
        changed: FormValue,
        values: FormSnapshot,
    },
    FieldsChange {
        fields: Vec<FieldErrors>,
    },
    Finish(FormSnapshot),
    FinishFailed(FormValidationError),
    Reset,
}

pub type ControlReset = Rc<dyn Fn(&mut Window, &mut App)>;
pub type ControlAction = Rc<dyn Fn(&mut Window, &mut App) -> bool>;
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct ControlToken(pub u64);
struct ControlRegistration {
    focus: Option<FocusHandle>,
    reset: ControlReset,
    focus_action: Option<ControlAction>,
    label_action: Option<ControlAction>,
}

pub struct FormFieldState {
    name: NamePath,
    value: FormValue,
    meta: FieldMeta,
    options: Rc<FieldOptions>,
    revision: u64,
    reset_epoch: u64,
    controls: BTreeMap<ControlToken, ControlRegistration>,
    next_control: u64,
    bounds: Option<Bounds<Pixels>>,
    mounts: usize,
    pending_scroll: bool,
    registered: bool,
    presentation_label: Option<SharedString>,
}
impl FormFieldState {
    pub fn name(&self) -> &NamePath {
        &self.name
    }
    pub fn value(&self) -> &FormValue {
        &self.value
    }
    pub fn meta(&self) -> &FieldMeta {
        &self.meta
    }
    pub fn options(&self) -> &FieldOptions {
        &self.options
    }
    pub fn revision(&self) -> u64 {
        self.revision
    }
    pub fn reset_epoch(&self) -> u64 {
        self.reset_epoch
    }
}

#[derive(Clone)]
pub struct FieldHandle {
    pub(crate) form: WeakEntity<FormState>,
    pub(crate) form_id: u64,
    pub(crate) id: FieldId,
    pub(crate) state: Entity<FormFieldState>,
}
impl FieldHandle {
    pub fn id(&self) -> FieldId {
        self.id
    }
    pub fn form(&self) -> Option<Entity<FormState>> {
        self.form.upgrade()
    }
    pub fn state(&self) -> &Entity<FormFieldState> {
        &self.state
    }
    pub fn name(&self, cx: &App) -> NamePath {
        self.state.read(cx).name.clone()
    }
    pub fn value(&self, cx: &App) -> FormValue {
        self.state.read(cx).value.clone()
    }
    pub fn meta(&self, cx: &App) -> FieldMeta {
        self.state.read(cx).meta.clone()
    }
    pub fn set_presentation_label(&self, label: Option<SharedString>, cx: &mut App) {
        self.state
            .update(cx, |state, _| state.presentation_label = label);
    }
    pub fn set_value(
        &self,
        value: FormValue,
        window: &mut Window,
        cx: &mut App,
    ) -> Result<(), FormError> {
        self.form
            .update(cx, |form, cx| {
                form.set_field_by_id(self.id, value, window, cx)
            })
            .map_err(|_| FormError::Detached)?
    }
    pub fn change(
        &self,
        value: FormValue,
        trigger: ValidationTrigger,
        window: &mut Window,
        cx: &mut App,
    ) -> Result<(), FormError> {
        self.form
            .update(cx, |form, cx| {
                form.change_field(self.id, value, trigger, window, cx)
            })
            .map_err(|_| FormError::Detached)?
    }
    pub fn blur(&self, window: &mut Window, cx: &mut App) {
        let _ = self
            .form
            .update(cx, |form, cx| form.blur_field(self.id, window, cx));
    }
    pub fn register_control(
        &self,
        focus: Option<FocusHandle>,
        reset: ControlReset,
        cx: &mut App,
    ) -> ControlToken {
        self.state.update(cx, |state, _| {
            let token = ControlToken(state.next_control);
            state.next_control += 1;
            state.controls.insert(
                token,
                ControlRegistration {
                    focus,
                    reset,
                    focus_action: None,
                    label_action: None,
                },
            );
            token
        })
    }
    pub fn clear_control(&self, token: ControlToken, cx: &mut App) {
        self.state.update(cx, |state, _| {
            state.controls.remove(&token);
        });
    }
    /// Install live native availability and label behavior for one registration.
    ///
    /// A focus action replaces the raw handle fallback for this control. Return
    /// false when the control cannot accept focus. Label actions may activate the
    /// native control as well as focus it. Capture weak native entities or
    /// [`WeakFormField`] in these callbacks to keep unmount cleanup acyclic.
    pub fn set_control_actions(
        &self,
        token: ControlToken,
        focus_action: Option<ControlAction>,
        label_action: Option<ControlAction>,
        cx: &mut App,
    ) {
        self.state.update(cx, |state, _| {
            if let Some(control) = state.controls.get_mut(&token) {
                control.focus_action = focus_action;
                control.label_action = label_action;
            }
        });
    }
    pub fn set_focus_handle(&self, focus: Option<FocusHandle>, cx: &mut App) {
        self.state.update(cx, |state, _| {
            state.controls.insert(
                ControlToken(0),
                ControlRegistration {
                    focus,
                    reset: Rc::new(|_, _| {}),
                    focus_action: None,
                    label_action: None,
                },
            );
        });
    }
    pub fn focus(&self, window: &mut Window, cx: &mut App) -> bool {
        let controls = self
            .state
            .read(cx)
            .controls
            .values()
            .map(|control| (control.focus.clone(), control.focus_action.clone()))
            .collect::<Vec<_>>();
        for (focus, action) in controls {
            if let Some(action) = action {
                if action(window, cx) {
                    return true;
                }
            } else if let Some(focus) = focus {
                window.focus(&focus, cx);
                return true;
            }
        }
        false
    }
    /// Perform the mounted native control's label action, then fall back to focus.
    pub fn activate_label(&self, window: &mut Window, cx: &mut App) -> bool {
        let actions = self
            .state
            .read(cx)
            .controls
            .values()
            .filter_map(|control| control.label_action.clone())
            .collect::<Vec<_>>();
        for action in actions {
            if action(window, cx) {
                return true;
            }
        }
        self.focus(window, cx)
    }
    pub fn set_bounds(&self, bounds: Bounds<Pixels>, cx: &mut App) {
        self.state
            .update(cx, |state, _| state.bounds = Some(bounds));
    }
    pub fn bounds(&self, cx: &App) -> Option<Bounds<Pixels>> {
        self.state.read(cx).bounds
    }
    pub fn take_scroll_request(&self, cx: &mut App) -> bool {
        self.state
            .update(cx, |state, _| std::mem::take(&mut state.pending_scroll))
    }
    pub fn mount(&self, cx: &mut App) {
        let first = self.state.update(cx, |state, _| {
            state.mounts += 1;
            state.mounts == 1
        });
        if first {
            let _ = self
                .form
                .update(cx, |form, cx| form.activate_field(self.id, cx));
        }
    }
    pub fn unmount(&self, cx: &mut App) {
        let last = self.state.update(cx, |state, _| {
            if state.mounts == 0 {
                return false;
            }
            state.mounts -= 1;
            state.mounts == 0
        });
        if last {
            let _ = self
                .form
                .update(cx, |form, cx| form.deactivate_field(self.id, cx));
        }
    }
}

pub struct FormField<T> {
    handle: FieldHandle,
    codec: FormCodec<T>,
}
impl<T> Clone for FormField<T> {
    fn clone(&self) -> Self {
        Self {
            handle: self.handle.clone(),
            codec: self.codec.clone(),
        }
    }
}
impl<T: 'static> FormField<T> {
    pub fn handle(&self) -> FieldHandle {
        self.handle.clone()
    }
    pub fn codec(&self) -> &FormCodec<T> {
        &self.codec
    }
    pub fn downgrade(&self) -> WeakFormField<T> {
        WeakFormField {
            form: self.handle.form.clone(),
            form_id: self.handle.form_id,
            id: self.handle.id,
            state: self.handle.state.downgrade(),
            codec: self.codec.clone(),
        }
    }
    pub fn encode(&self, value: T) -> FormValue {
        self.codec.encode(value)
    }
    pub fn name(&self, cx: &App) -> NamePath {
        self.handle.name(cx)
    }
    pub fn read(&self, cx: &App) -> Result<FormPresence<T>, FormReadError> {
        if self.handle.form.upgrade().is_none() {
            return Err(FormError::Detached);
        }
        if !self.handle.state.read(cx).registered {
            return Err(FormError::UnknownField(self.handle.id));
        }
        Ok(self
            .codec
            .decode_presence(Some(self.handle.state.read(cx).value()))?)
    }
    pub fn set(
        &self,
        value: FormPresence<T>,
        window: &mut Window,
        cx: &mut App,
    ) -> Result<(), FormError> {
        self.handle
            .set_value(self.codec.encode_presence(value), window, cx)
    }
    pub fn change(
        &self,
        value: FormPresence<T>,
        trigger: ValidationTrigger,
        window: &mut Window,
        cx: &mut App,
    ) -> Result<(), FormError> {
        self.handle
            .change(self.codec.encode_presence(value), trigger, window, cx)
    }
}
/// Retain inside native focus, label or reset callbacks without owning the field.
pub struct WeakFormField<T> {
    form: WeakEntity<FormState>,
    form_id: u64,
    id: FieldId,
    state: WeakEntity<FormFieldState>,
    codec: FormCodec<T>,
}
impl<T> Clone for WeakFormField<T> {
    fn clone(&self) -> Self {
        Self {
            form: self.form.clone(),
            form_id: self.form_id,
            id: self.id,
            state: self.state.clone(),
            codec: self.codec.clone(),
        }
    }
}
impl<T: 'static> WeakFormField<T> {
    pub fn upgrade(&self) -> Option<FormField<T>> {
        self.form.upgrade()?;
        Some(FormField {
            handle: FieldHandle {
                form: self.form.clone(),
                form_id: self.form_id,
                id: self.id,
                state: self.state.upgrade()?,
            },
            codec: self.codec.clone(),
        })
    }
    pub fn read(&self, cx: &App) -> Result<FormPresence<T>, FormReadError> {
        self.upgrade().ok_or(FormError::Detached)?.read(cx)
    }
    pub fn change(
        &self,
        value: FormPresence<T>,
        trigger: ValidationTrigger,
        window: &mut Window,
        cx: &mut App,
    ) -> Result<(), FormError> {
        self.upgrade()
            .ok_or(FormError::Detached)?
            .change(value, trigger, window, cx)
    }
}
impl FormSnapshot {
    pub fn read<T: 'static>(&self, field: &FormField<T>) -> Result<FormPresence<T>, FormReadError> {
        if self.form_id != field.handle.form_id {
            return Err(FormError::ForeignField);
        }
        let path = self
            .field_paths
            .get(&field.handle.id.0)
            .ok_or(FormError::UnknownField(field.handle.id))?;
        Ok(field.codec.decode_presence(self.get(path))?)
    }
}

struct Completion<T> {
    value: RefCell<Option<T>>,
    wakers: RefCell<Vec<Waker>>,
}
impl<T> Default for Completion<T> {
    fn default() -> Self {
        Self {
            value: RefCell::new(None),
            wakers: RefCell::new(Vec::new()),
        }
    }
}
impl<T: Clone> Completion<T> {
    fn finish(&self, value: T) {
        if self.value.borrow().is_some() {
            return;
        }
        *self.value.borrow_mut() = Some(value);
        for waker in self.wakers.take() {
            waker.wake();
        }
    }
    fn poll(&self, cx: &mut TaskContext<'_>) -> Poll<T> {
        if let Some(value) = self.value.borrow().clone() {
            Poll::Ready(value)
        } else {
            let mut wakers = self.wakers.borrow_mut();
            if !wakers.iter().any(|w| w.will_wake(cx.waker())) {
                wakers.push(cx.waker().clone());
            }
            Poll::Pending
        }
    }
    fn is_finished(&self) -> bool {
        self.value.borrow().is_some()
    }
}

/// Awaitable validation completion; cancelling a run resolves it as out-of-date.
#[derive(Clone)]
pub struct ValidationHandle(Rc<Completion<Result<FormSnapshot, FormValidationError>>>);
impl Future for ValidationHandle {
    type Output = Result<FormSnapshot, FormValidationError>;
    fn poll(self: Pin<&mut Self>, cx: &mut TaskContext<'_>) -> Poll<Self::Output> {
        self.0.poll(cx)
    }
}
impl ValidationHandle {
    pub fn is_finished(&self) -> bool {
        self.0.is_finished()
    }
}
#[derive(Clone)]
struct FieldValidationHandle(Rc<Completion<Result<FieldErrors, ()>>>);
impl Future for FieldValidationHandle {
    type Output = Result<FieldErrors, ()>;
    fn poll(self: Pin<&mut Self>, cx: &mut TaskContext<'_>) -> Poll<Self::Output> {
        self.0.poll(cx)
    }
}
struct ValidationTask {
    field: FieldId,
    completion: Rc<Completion<Result<FieldErrors, ()>>>,
    _task: Task<()>,
}

pub struct FormWatch<T> {
    value: T,
    revision: u64,
    _release: Option<Subscription>,
}
impl<T> FormWatch<T> {
    pub fn value(&self) -> &T {
        &self.value
    }
    pub fn revision(&self) -> u64 {
        self.revision
    }
}
type WatchCallback = Rc<dyn Fn(&FormSnapshot, &mut App) -> bool>;
struct WatchRecord {
    paths: Vec<NamePath>,
    callback: WatchCallback,
}

type AggregateValidation = (
    Rc<Completion<Result<FormSnapshot, FormValidationError>>>,
    Task<()>,
);

/// Retained form ownership. Individual fields and watchers own their notifications.
pub struct FormState {
    owner_entity: Option<WeakEntity<FormState>>,
    pub(crate) store: FormStore,
    pub(crate) entities: BTreeMap<FieldId, Entity<FormFieldState>>,
    scope: ConfigScope,
    initialized: bool,
    epoch: u64,
    validation_sequence: u64,
    aggregate_generation: u64,
    validation_tasks: BTreeMap<u64, ValidationTask>,
    aggregate: Option<AggregateValidation>,
    submit_task: Option<Task<()>>,
    watchers: BTreeMap<FieldId, WatchRecord>,
    watch_index: PathIndex,
    next_watch: u64,
    pub(crate) lists: BTreeMap<NamePath, super::list::ListRecord>,
    pub(crate) next_list_key: u64,
    clear_on_destroy: bool,
    pub(crate) provider: Option<(WeakEntity<super::provider::FormProvider>, SharedString)>,
    validate_messages: crate::locale::FormMessagePatch,
    scroll_to_first_error: Option<bool>,
    connections: usize,
    destroyed: bool,
}
impl EventEmitter<FormEvent> for FormState {}

impl FormState {
    pub fn new(_window: &mut Window, cx: &mut Context<Self>) -> Self {
        static NEXT_FORM: AtomicU64 = AtomicU64::new(1);
        Self {
            owner_entity: Some(cx.weak_entity()),
            store: FormStore::new(NEXT_FORM.fetch_add(1, Ordering::Relaxed)),
            entities: BTreeMap::new(),
            scope: ConfigScope::application(cx),
            initialized: false,
            epoch: 0,
            validation_sequence: 0,
            aggregate_generation: 0,
            validation_tasks: BTreeMap::new(),
            aggregate: None,
            submit_task: None,
            watchers: BTreeMap::new(),
            watch_index: PathIndex::default(),
            next_watch: 1,
            lists: BTreeMap::new(),
            next_list_key: 1,
            clear_on_destroy: false,
            provider: None,
            validate_messages: crate::locale::FormMessagePatch::default(),
            scroll_to_first_error: None,
            connections: 0,
            destroyed: false,
        }
    }
    pub fn id(&self) -> u64 {
        self.store.snapshot.form_id
    }
    pub fn snapshot(&self) -> FormSnapshot {
        self.store.snapshot.clone()
    }
    pub fn scope(&self) -> &ConfigScope {
        &self.scope
    }
    pub fn set_scope(&mut self, scope: ConfigScope, _cx: &mut Context<Self>) {
        self.scope = scope;
    }
    pub fn set_validate_messages(
        &mut self,
        messages: crate::locale::FormMessagePatch,
        _cx: &mut Context<Self>,
    ) {
        self.validate_messages = messages;
    }
    pub fn set_scroll_to_first_error(&mut self, enabled: bool, focus: bool) {
        self.scroll_to_first_error = enabled.then_some(focus);
    }
    pub fn connect(&mut self) {
        self.connections += 1;
        self.destroyed = false;
    }
    pub fn scroll_to_field(
        &self,
        name: impl Into<NamePath>,
        focus: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> bool {
        let Some(field) = self.field(&name.into()) else {
            return false;
        };
        field.state.update(cx, |state, cx| {
            state.pending_scroll = true;
            cx.notify();
        });
        if focus {
            window.defer(cx, move |window, cx| {
                field.focus(window, cx);
            });
        }
        true
    }
    pub fn value(&self, name: impl Into<NamePath>) -> FormValue {
        self.store.value(&name.into())
    }
    pub fn get_field_value(&self, name: impl Into<NamePath>) -> FormValue {
        self.value(name)
    }
    pub fn get_fields_value(&self, all: bool) -> FormValue {
        if all {
            self.store.snapshot.root.clone()
        } else {
            self.store.registered_values()
        }
    }
    pub fn field(&self, name: &NamePath) -> Option<FieldHandle> {
        self.store
            .fields
            .iter()
            .find(|(_, record)| record.active && record.name == *name)
            .and_then(|(id, _)| self.handle(*id))
    }
    pub fn fields(&self) -> impl Iterator<Item = FieldHandle> + '_ {
        self.store
            .fields
            .iter()
            .filter(|(_, record)| record.active)
            .filter_map(|(id, _)| self.handle(*id))
    }
    fn handle(&self, id: FieldId) -> Option<FieldHandle> {
        // Each entity stores a weak owner indirectly through handles, never a strong cycle.
        self.entities.get(&id).map(|state| FieldHandle {
            form: self.owner(),
            form_id: self.id(),
            id,
            state: state.clone(),
        })
    }
    fn owner(&self) -> WeakEntity<Self> {
        self.owner_entity
            .as_ref()
            .expect("form ownership initialized")
            .clone()
    }
    pub fn register<T: 'static>(
        &mut self,
        name: impl Into<NamePath>,
        codec: FormCodec<T>,
        mut options: FieldOptions,
        cx: &mut Context<Self>,
    ) -> Result<FormField<T>, FormError> {
        let name = name.into();
        if self
            .lists
            .keys()
            .any(|list| name.starts_with(list) && name.len() > list.len())
        {
            options.initial_value = None;
        }
        // Validate existing/default values before changing registration state.
        let value = self
            .store
            .snapshot
            .get(&name)
            .or(options.initial_value.as_ref());
        codec.decode_presence(value)?;
        let handle = self.register_raw(name, options, cx)?;
        Ok(FormField { handle, codec })
    }
    pub fn register_raw(
        &mut self,
        name: impl Into<NamePath>,
        mut options: FieldOptions,
        cx: &mut Context<Self>,
    ) -> Result<FieldHandle, FormError> {
        self.owner_entity = Some(cx.weak_entity());
        let name = name.into();
        if self
            .lists
            .keys()
            .any(|list| name.starts_with(list) && name.len() > list.len())
        {
            options.initial_value = None;
        }
        let id = self.store.register(name.clone(), options)?;
        let options = self.store.fields[&id].options.clone();
        let value = self.store.value(&name);
        let meta = self.store.fields[&id].meta.clone();
        let state = cx.new(|_| FormFieldState {
            name,
            value,
            meta,
            options,
            revision: 0,
            reset_epoch: 0,
            controls: BTreeMap::new(),
            next_control: 1,
            bounds: None,
            mounts: 0,
            pending_scroll: false,
            registered: true,
            presentation_label: None,
        });
        self.entities.insert(id, state.clone());
        self.sync_fields(&[id], false, cx);
        Ok(FieldHandle {
            form: cx.weak_entity(),
            form_id: self.id(),
            id,
            state,
        })
    }
    fn activate_field(&mut self, id: FieldId, cx: &mut Context<Self>) {
        let Some(record) = self.store.fields.get_mut(&id) else {
            return;
        };
        if record.active {
            return;
        }
        record.active = true;
        self.store.index.insert(&record.name, id);
        for dependency in &record.options.dependencies {
            self.store.dependencies.insert(dependency, id);
        }
        let name = record.name.clone();
        let initial = self
            .store
            .initial
            .get(&name)
            .cloned()
            .or_else(|| record.options.initial_value.clone());
        if self.store.value(&name).is_undefined()
            && let Some(initial) = initial
        {
            let _ = self.store.set(&name, initial);
            self.reconcile_lists(std::slice::from_ref(&name));
            self.notify_watches(std::slice::from_ref(&name), cx);
        }
        if let Some(record) = self.store.fields.get_mut(&id) {
            record.meta.dirty = !self
                .store
                .snapshot
                .get(&name)
                .is_none_or(FormValue::is_undefined);
        }
        self.sync_fields(&[id], false, cx);
    }
    fn deactivate_field(&mut self, id: FieldId, cx: &mut Context<Self>) {
        self.cancel_field(id);
        let Some(record) = self.store.fields.get_mut(&id) else {
            return;
        };
        if !record.active {
            return;
        }
        record.active = false;
        self.store.index.remove(&record.name, id);
        for dependency in &record.options.dependencies {
            self.store.dependencies.remove(dependency, id);
        }
        let name = record.name.clone();
        let preserve = record.options.preserve;
        record.meta = FieldMeta {
            required: record.options.is_required(),
            ..FieldMeta::default()
        };
        if !preserve
            && !(self.destroyed && self.clear_on_destroy)
            && !self
                .store
                .fields
                .values()
                .any(|r| r.active && r.name == name)
        {
            let initial = self.store.initial.get(&name).cloned().unwrap_or_default();
            let _ = self.store.set(&name, initial);
            self.notify_watches(std::slice::from_ref(&name), cx);
        }
        self.sync_fields(&[id], false, cx);
    }
    pub fn unregister(&mut self, id: FieldId, cx: &mut Context<Self>) -> Result<(), FormError> {
        self.cancel_field(id);
        if self.destroyed
            && self.clear_on_destroy
            && let Some(record) = self.store.fields.get_mut(&id)
        {
            Rc::make_mut(&mut record.options).preserve = true;
        }
        let removed = self.store.remove_field(id)?;
        self.retire_field(id, cx);
        if let Some(name) = removed {
            self.changed(&[name], false, false, cx);
        }
        Ok(())
    }
    pub(crate) fn retire_field(&mut self, id: FieldId, cx: &mut Context<Self>) {
        if let Some(entity) = self.entities.remove(&id) {
            entity.update(cx, |state, cx| {
                state.registered = false;
                cx.notify();
            });
        }
    }
    pub fn set_initial_values(
        &mut self,
        values: FormValue,
        cx: &mut Context<Self>,
    ) -> Result<(), FormError> {
        if values.as_object().is_none() {
            return Err(super::value::FormTypeError {
                expected: "object",
                actual: values.type_name(),
            }
            .into());
        }
        self.store.initial = values.clone();
        if !self.initialized {
            self.initialized = true;
            let merged = values.merged(&self.store.snapshot.root);
            self.store.replace(merged)?;
            let item_defaults = self
                .store
                .fields
                .values()
                .filter(|r| !r.meta.touched && r.options.initial_value.is_some())
                .map(|r| r.name.clone())
                .collect::<Vec<_>>();
            for name in item_defaults {
                if let Some(value) = values.get(&name) {
                    self.store.set(&name, value.clone())?;
                }
            }
            self.reconcile_lists(&[NamePath::root()]);
            let ids = self.store.fields.keys().copied().collect::<Vec<_>>();
            for id in &ids {
                if let Some(record) = self.store.fields.get_mut(id) {
                    record.meta.dirty = true;
                }
            }
            self.sync_fields(&ids, false, cx);
            self.notify_watches(&[NamePath::root()], cx);
        }
        Ok(())
    }
    pub fn set_field_value(
        &mut self,
        name: impl Into<NamePath>,
        value: FormValue,
        _window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Result<(), FormError> {
        let name = name.into();
        let value_changed = self.store.set(&name, value)?;
        self.reconcile_lists(std::slice::from_ref(&name));
        let affected = self.store.index.intersects(&name);
        let exact = affected
            .iter()
            .copied()
            .filter(|id| self.store.fields[id].name == name)
            .collect::<BTreeSet<_>>();
        let dependencies = if value_changed {
            self.store.dependencies.intersects(&name)
        } else {
            BTreeSet::new()
        };
        for id in affected
            .iter()
            .copied()
            .filter(|id| value_changed || exact.contains(id))
            .chain(dependencies.iter().copied())
            .collect::<BTreeSet<_>>()
        {
            self.cancel_field(id);
        }
        for id in exact {
            let record = self.store.fields.get_mut(&id).expect("exact field exists");
            record.meta.touched = true;
            record.meta.dirty = true;
            record.meta.validated = false;
            record.meta.errors.clear();
            record.meta.warnings.clear();
        }
        self.sync_fields(
            &affected
                .into_iter()
                .chain(dependencies)
                .collect::<BTreeSet<_>>()
                .into_iter()
                .collect::<Vec<_>>(),
            false,
            cx,
        );
        self.notify_watches(&[name], cx);
        Ok(())
    }
    fn set_field_by_id(
        &mut self,
        id: FieldId,
        value: FormValue,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Result<(), FormError> {
        let name = self
            .store
            .fields
            .get(&id)
            .ok_or(FormError::UnknownField(id))?
            .name
            .clone();
        self.set_field_value(name, value, window, cx)
    }
    pub fn set_fields_value(
        &mut self,
        values: FormValue,
        _window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Result<(), FormError> {
        if values.as_object().is_none() {
            return Err(super::value::FormTypeError {
                expected: "object",
                actual: values.type_name(),
            }
            .into());
        }
        let old = self.snapshot();
        let merged = old.root.merged(&values);
        if !self.store.replace(merged)? {
            return Ok(());
        }
        self.reconcile_lists(&[NamePath::root()]);
        let changed = self
            .store
            .fields
            .values()
            .filter(|record| old.get(&record.name) != self.store.snapshot.get(&record.name))
            .map(|record| record.name.clone())
            .collect::<Vec<_>>();
        self.changed(&changed, true, false, cx);
        self.notify_watches(&[NamePath::root()], cx);
        Ok(())
    }
    pub fn change_field(
        &mut self,
        id: FieldId,
        value: FormValue,
        trigger: ValidationTrigger,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Result<(), FormError> {
        let name = self
            .store
            .fields
            .get(&id)
            .ok_or(FormError::UnknownField(id))?
            .name
            .clone();
        let raw_value = value.clone();
        let value = self.store.fields[&id]
            .options
            .normalize
            .as_ref()
            .map_or_else(
                || value.clone(),
                |normalize| normalize(value.clone(), &self.store.value(&name), &self.snapshot()),
            );
        if !self.store.set(&name, value.clone())? {
            if value != raw_value {
                let ids = self
                    .store
                    .index
                    .intersects(&name)
                    .into_iter()
                    .filter(|id| self.store.fields[id].name == name)
                    .collect::<Vec<_>>();
                for id in ids {
                    if let Some(field) = self.entities.get(&id) {
                        field.update(cx, |field, cx| {
                            field.revision += 1;
                            cx.notify();
                        });
                    }
                }
            }
            return Ok(());
        }
        self.reconcile_lists(std::slice::from_ref(&name));
        let dependencies = self.store.dependencies.intersects(&name);
        let own_fields = self.store.index.intersects(&name);
        self.changed(std::slice::from_ref(&name), true, true, cx);
        let changed = FormValue::from(FormObject::new()).with_path(&name, value)?;
        self.emit(
            FormEvent::ValuesChange {
                changed,
                values: self.snapshot(),
            },
            cx,
        );
        let mut validate = dependencies;
        validate.extend(own_fields.iter().copied());
        for dependent in validate {
            if self.store.fields.get(&dependent).is_some_and(|record| {
                record.active
                    && record.options.validate_trigger.contains(&trigger)
                    && (own_fields.contains(&dependent) || record.meta.dirty)
            }) {
                self.launch_validation(dependent, trigger, false, window, cx);
            }
        }
        Ok(())
    }
    pub fn blur_field(&mut self, id: FieldId, window: &mut Window, cx: &mut Context<Self>) {
        if self.store.fields.get(&id).is_some_and(|record| {
            record
                .options
                .validate_trigger
                .contains(&ValidationTrigger::Blur)
        }) {
            self.launch_validation(id, ValidationTrigger::Blur, false, window, cx);
        }
    }
    pub fn set_fields(
        &mut self,
        patches: impl IntoIterator<Item = FieldPatch>,
        _window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Result<(), FormError> {
        for patch in patches {
            let mut affected = BTreeSet::new();
            if let Some(value) = patch.value {
                self.store.set(&patch.name, value)?;
                affected = self.store.index.intersects(&patch.name);
                self.reconcile_lists(std::slice::from_ref(&patch.name));
            }
            let ids = self
                .store
                .index
                .intersects(&patch.name)
                .into_iter()
                .filter(|id| self.store.fields[id].name == patch.name)
                .collect::<Vec<_>>();
            for id in &ids {
                self.cancel_field(*id);
                let record = self.store.fields.get_mut(id).expect("indexed field exists");
                record.meta.dirty = true;
                if let Some(value) = patch.touched {
                    record.meta.touched = value;
                }
                if let Some(value) = patch.validating {
                    record.meta.validating = value;
                }
                if let Some(value) = &patch.errors {
                    record.meta.errors = value.clone();
                }
                if let Some(value) = &patch.warnings {
                    record.meta.warnings = value.clone();
                }
            }
            affected.extend(ids);
            self.sync_fields(&affected.into_iter().collect::<Vec<_>>(), false, cx);
            self.notify_watches(&[patch.name], cx);
        }
        Ok(())
    }
    pub fn set_field_options(
        &mut self,
        id: FieldId,
        options: FieldOptions,
        cx: &mut Context<Self>,
    ) -> Result<(), FormError> {
        self.cancel_field(id);
        let record = self
            .store
            .fields
            .get_mut(&id)
            .ok_or(FormError::UnknownField(id))?;
        for dependency in &record.options.dependencies {
            self.store.dependencies.remove(dependency, id);
        }
        for dependency in &options.dependencies {
            self.store.dependencies.insert(dependency, id);
        }
        record.meta.required = options.is_required();
        record.options = Rc::new(options);
        self.sync_fields(&[id], false, cx);
        if let Some(field) = self.entities.get(&id) {
            field.update(cx, |field, cx| {
                field.revision += 1;
                cx.notify();
            });
        }
        Ok(())
    }
    pub fn is_fields_touched(&self, names: Option<&[NamePath]>, all: bool) -> bool {
        let fields = || {
            self.store
                .fields
                .values()
                .filter(|field| field.active && !field.name.is_empty())
        };
        if let Some(names) = names {
            // A requested path represents a descendant group. One touched
            // member satisfies that group, including an exact path match.
            let touched = |name: &NamePath| {
                fields().any(|field| field.name.starts_with(name) && field.meta.touched)
            };
            if all {
                names.iter().all(touched)
            } else {
                names.iter().any(touched)
            }
        } else if all {
            fields().all(|field| field.meta.touched || self.lists.contains_key(&field.name))
        } else {
            fields().any(|field| field.meta.touched)
        }
    }
    pub fn is_fields_validating(&self, names: Option<&[NamePath]>) -> bool {
        self.store.fields.values().any(|r| {
            r.active && r.meta.validating && names.is_none_or(|names| names.contains(&r.name))
        })
    }
    pub fn field_errors(&self) -> Vec<FieldErrors> {
        self.store
            .fields
            .values()
            .filter(|r| r.active)
            .map(|r| FieldErrors {
                name: r.name.clone(),
                errors: r.meta.errors.clone(),
                warnings: r.meta.warnings.clone(),
            })
            .collect()
    }
    pub fn reset(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let _ = self.reset_fields(None, window, cx);
    }
    pub fn reset_fields(
        &mut self,
        names: Option<&[NamePath]>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Result<(), FormError> {
        let ids = self
            .store
            .fields
            .iter()
            .filter(|(_, r)| names.is_none_or(|names| names.contains(&r.name)))
            .map(|(id, _)| *id)
            .collect::<Vec<_>>();
        if names.is_none() {
            self.epoch += 1;
            self.cancel_aggregate();
            self.store.replace(self.store.initial.clone())?;
        }
        for id in &ids {
            self.cancel_field(*id);
            let record = &self.store.fields[id];
            let name = record.name.clone();
            let value = self
                .store
                .initial
                .get(&name)
                .cloned()
                .or_else(|| record.options.initial_value.clone())
                .unwrap_or_default();
            self.store.set(&name, value)?;
            let record = self.store.fields.get_mut(id).expect("reset field exists");
            record.meta = FieldMeta {
                required: record.options.is_required(),
                ..FieldMeta::default()
            };
        }
        let paths = names
            .map(<[NamePath]>::to_vec)
            .unwrap_or_else(|| vec![NamePath::root()]);
        self.reconcile_lists(&paths);
        self.sync_fields(&ids, true, cx);
        let callbacks = ids
            .iter()
            .filter_map(|id| self.entities.get(id))
            .flat_map(|state| {
                state
                    .read(cx)
                    .controls
                    .values()
                    .map(|c| (c.focus.clone(), c.reset.clone()))
                    .collect::<Vec<_>>()
            })
            .collect::<Vec<_>>();
        window.defer(cx, move |window, cx| {
            for (focus, callback) in callbacks {
                callback(window, cx);
                if focus
                    .as_ref()
                    .is_some_and(|focus| focus.contains_focused(window, cx))
                {
                    window.blur(cx);
                }
            }
        });
        self.notify_watches(&paths, cx);
        self.emit(FormEvent::Reset, cx);
        Ok(())
    }
    pub fn clear_on_destroy(&mut self, value: bool) {
        self.clear_on_destroy = value;
    }
    pub fn disconnect(&mut self, cx: &mut Context<Self>) {
        if self.connections > 0 {
            self.connections -= 1;
            if self.connections > 0 {
                return;
            }
        }
        self.destroyed = true;
        self.epoch += 1;
        self.cancel_aggregate();
        let ids = self.store.fields.keys().copied().collect::<Vec<_>>();
        for id in &ids {
            self.cancel_field(*id);
        }
        if self.clear_on_destroy {
            let _ = self.store.replace(FormObject::new().into());
            self.reconcile_lists(&[NamePath::root()]);
            self.sync_fields(&ids, false, cx);
            self.notify_watches(&[NamePath::root()], cx);
        }
    }
    pub fn watch<T: Clone + PartialEq + 'static>(
        &mut self,
        name: impl Into<NamePath>,
        codec: FormCodec<T>,
        cx: &mut Context<Self>,
    ) -> Entity<FormWatch<Result<FormPresence<T>, super::value::FormTypeError>>> {
        let name = name.into();
        let selected = name.clone();
        self.watch_selector(
            vec![name],
            move |snapshot| codec.decode_presence(snapshot.get(&selected)),
            cx,
        )
    }
    pub fn watch_selector<T: Clone + PartialEq + 'static>(
        &mut self,
        paths: Vec<NamePath>,
        selector: impl Fn(&FormSnapshot) -> T + 'static,
        cx: &mut Context<Self>,
    ) -> Entity<FormWatch<T>> {
        let id = FieldId(self.next_watch);
        self.next_watch += 1;
        let value = selector(&self.snapshot());
        let owner = cx.weak_entity();
        let entity = cx.new(|cx| FormWatch {
            value,
            revision: 0,
            _release: Some(cx.on_release(move |_, cx| {
                let _ = owner.update(cx, |form, _| form.remove_watch(id));
            })),
        });
        let weak = entity.downgrade();
        let callback: WatchCallback = Rc::new(move |snapshot, cx| {
            let value = selector(snapshot);
            weak.update(cx, |watch, cx| {
                if watch.value != value {
                    watch.value = value;
                    watch.revision += 1;
                    cx.notify();
                }
            })
            .is_ok()
        });
        let paths = if paths.is_empty() {
            vec![NamePath::root()]
        } else {
            paths
        };
        for path in &paths {
            self.watch_index.insert(path, id);
        }
        self.watchers.insert(id, WatchRecord { paths, callback });
        entity
    }
    fn remove_watch(&mut self, id: FieldId) {
        if let Some(watch) = self.watchers.remove(&id) {
            for path in &watch.paths {
                self.watch_index.remove(path, id);
            }
        }
    }
    pub(crate) fn notify_watches(&mut self, paths: &[NamePath], cx: &mut Context<Self>) {
        let ids = paths
            .iter()
            .flat_map(|path| self.watch_index.intersects(path))
            .collect::<BTreeSet<_>>();
        let callbacks = ids
            .into_iter()
            .filter_map(|id| self.watchers.get(&id).map(|w| (id, w.callback.clone())))
            .collect::<Vec<_>>();
        let snapshot = self.snapshot();
        for (id, callback) in callbacks {
            if !callback(&snapshot, cx) {
                self.remove_watch(id);
            }
        }
    }
    pub(crate) fn changed(
        &mut self,
        paths: &[NamePath],
        touched: bool,
        user: bool,
        cx: &mut Context<Self>,
    ) {
        let ids = paths
            .iter()
            .flat_map(|path| self.store.index.intersects(path))
            .collect::<BTreeSet<_>>();
        let dependencies = paths
            .iter()
            .flat_map(|path| self.store.dependencies.intersects(path))
            .collect::<BTreeSet<_>>();
        for id in ids
            .iter()
            .chain(dependencies.iter())
            .copied()
            .collect::<BTreeSet<_>>()
        {
            self.cancel_field(id);
        }
        for id in &ids {
            let record = self.store.fields.get_mut(id).expect("indexed field exists");
            record.meta.touched |= touched;
            record.meta.dirty = true;
            record.meta.validated = false;
            record.meta.errors.clear();
            record.meta.warnings.clear();
        }
        let ids = ids
            .into_iter()
            .chain(dependencies)
            .collect::<BTreeSet<_>>()
            .into_iter()
            .collect::<Vec<_>>();
        self.sync_fields(&ids, false, cx);
        self.notify_watches(paths, cx);
        if user {
            self.emit(
                FormEvent::FieldsChange {
                    fields: self.errors_for(&ids),
                },
                cx,
            );
        }
    }
    pub(crate) fn sync_fields(&mut self, ids: &[FieldId], reset: bool, cx: &mut Context<Self>) {
        let snapshot = self.snapshot();
        for id in ids {
            if let Some(record) = self.store.fields.get_mut(id) {
                record.meta.required = record.options.required.unwrap_or_else(|| {
                    record.options.rules.iter().any(|rule| {
                        if let Some(factory) = &rule.factory {
                            let rule = factory(&snapshot);
                            rule.required && !rule.warning_only
                        } else {
                            rule.required && !rule.warning_only
                        }
                    })
                });
            }
            if let (Some(record), Some(entity)) = (self.store.fields.get(id), self.entities.get(id))
            {
                let value = self.store.value(&record.name);
                entity.update(cx, |field, cx| {
                    let changed = field.value != value
                        || field.meta != record.meta
                        || field.name != record.name
                        || reset;
                    field.name = record.name.clone();
                    field.value = value;
                    field.meta = record.meta.clone();
                    field.options = record.options.clone();
                    if reset {
                        field.reset_epoch += 1;
                    }
                    if changed {
                        field.revision += 1;
                        cx.notify();
                    }
                });
            }
        }
    }
    fn errors_for(&self, ids: &[FieldId]) -> Vec<FieldErrors> {
        ids.iter()
            .filter_map(|id| {
                self.store.fields.get(id).map(|r| FieldErrors {
                    name: r.name.clone(),
                    errors: r.meta.errors.clone(),
                    warnings: r.meta.warnings.clone(),
                })
            })
            .collect()
    }
    pub(crate) fn emit(&mut self, event: FormEvent, cx: &mut Context<Self>) {
        if let Some((provider, name)) = &self.provider {
            let form_id = self.id();
            let provider = provider.clone();
            let name = name.clone();
            let event = event.clone();
            cx.defer(move |cx| {
                let _ = provider.update(cx, |provider, cx| {
                    provider.form_event(name, form_id, event, cx)
                });
            });
        }
        cx.emit(event);
    }
    pub(crate) fn cancel_field(&mut self, id: FieldId) {
        if let Some(record) = self.store.fields.get_mut(&id) {
            record.generation += 1;
            record.meta.validating = false;
        }
        let keys = self
            .validation_tasks
            .iter()
            .filter(|(_, task)| task.field == id)
            .map(|(key, _)| *key)
            .collect::<Vec<_>>();
        for key in keys {
            if let Some(task) = self.validation_tasks.remove(&key) {
                task.completion.finish(Err(()));
            }
        }
    }
    fn cancel_aggregate(&mut self) {
        if let Some((completion, _task)) = self.aggregate.take() {
            completion.finish(Err(FormValidationError {
                values: self.snapshot(),
                error_fields: Vec::new(),
                out_of_date: true,
            }));
        }
    }
    fn launch_validation(
        &mut self,
        id: FieldId,
        trigger: ValidationTrigger,
        only: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> FieldValidationHandle {
        self.validation_tasks
            .retain(|_, task| !task.completion.is_finished());
        let completion = Rc::new(Completion::default());
        let handle = FieldValidationHandle(completion.clone());
        let values = self.snapshot();
        let Some(record) = self.store.fields.get(&id) else {
            completion.finish(Err(()));
            return handle;
        };
        let mut rules = record
            .options
            .rules
            .iter()
            .map(|rule| rule.resolved(&values))
            .filter(|rule| rule.applies(trigger))
            .collect::<Vec<_>>();
        if rules.is_empty() {
            completion.finish(Ok(FieldErrors {
                name: record.name.clone(),
                ..FieldErrors::default()
            }));
            return handle;
        }
        if !only {
            self.cancel_field(id);
        }
        let Some(record) = self.store.fields.get_mut(&id) else {
            completion.finish(Err(()));
            return handle;
        };
        let generation = record.generation;
        let options = record.options.clone();
        let name = record.name.clone();
        if !only {
            record.meta.validating = !options.rules.is_empty();
            record.meta.validated = true;
            record.meta.dirty = true;
            record.meta.errors.clear();
            record.meta.warnings.clear();
        }
        self.sync_fields(&[id], false, cx);
        if !only {
            self.emit(
                FormEvent::FieldsChange {
                    fields: self.errors_for(&[id]),
                },
                cx,
            );
        }
        rules.sort_by_key(|rule| rule.warning_only);
        let request = FormValidationRequest {
            field: id,
            name: name.clone(),
            label: if options.label.is_empty() {
                self.entities
                    .get(&id)
                    .and_then(|field| field.read(cx).presentation_label.clone())
                    .unwrap_or_else(|| name.to_string().into())
            } else {
                options.label.clone()
            },
            value: self.store.value(&name),
            values,
            trigger,
            messages: self
                .scope
                .snapshot(cx)
                .locale
                .form
                .patched(&self.validate_messages),
            variables: options.message_variables.clone(),
        };
        let epoch = self.epoch;
        let mode = options.validate_first;
        let done = completion.clone();
        let task = cx.spawn_in(window, async move |weak, cx| {
            if !matches!(
                trigger,
                ValidationTrigger::Programmatic | ValidationTrigger::Submit
            ) && !options.validate_debounce.is_zero()
            {
                cx.background_executor()
                    .timer(options.validate_debounce)
                    .await;
            }
            let result = if mode == RuleMode::SerialFirst {
                let mut results = Vec::new();
                for rule in rules {
                    let warning = rule.warning_only;
                    let issues =
                        match cx.update(|window, cx| rule.evaluate(request.clone(), window, cx)) {
                            Ok(result) => result.into_issues(request.name.clone(), warning).await,
                            Err(_) => {
                                done.finish(Err(()));
                                return;
                            }
                        };
                    let failed = !issues.is_empty();
                    results.extend(issues);
                    if failed {
                        break;
                    }
                }
                results
            } else {
                let results = match cx.update(|window, cx| {
                    rules
                        .into_iter()
                        .map(|rule| {
                            (
                                rule.warning_only,
                                rule.evaluate(request.clone(), window, cx),
                            )
                        })
                        .collect::<Vec<_>>()
                }) {
                    Ok(results) => results,
                    Err(_) => {
                        done.finish(Err(()));
                        return;
                    }
                };
                collect_rule_issues(
                    results
                        .into_iter()
                        .map(|(warning, result)| (warning, request.name.clone(), result))
                        .collect(),
                    mode,
                )
                .await
            };
            let mut errors = FieldErrors {
                name,
                ..FieldErrors::default()
            };
            for issue in result {
                if issue.warning_only {
                    errors.warnings.push(issue.message);
                } else {
                    errors.errors.push(issue.message);
                }
            }
            let outcome = cx
                .update(|_, cx| {
                    let Some(entity) = weak.upgrade() else {
                        return false;
                    };
                    entity.update(cx, |form, cx| {
                        if form.epoch != epoch
                            || form.store.fields.get(&id).is_none_or(|record| {
                                record.generation != generation || record.name != errors.name
                            })
                        {
                            return false;
                        }
                        if !only {
                            let record = form
                                .store
                                .fields
                                .get_mut(&id)
                                .expect("validated field exists");
                            record.meta.validating = false;
                            record.meta.errors = errors.errors.clone();
                            record.meta.warnings = errors.warnings.clone();
                            form.sync_fields(&[id], false, cx);
                            form.emit(
                                FormEvent::FieldsChange {
                                    fields: vec![errors.clone()],
                                },
                                cx,
                            );
                        }
                        true
                    })
                })
                .unwrap_or(false);
            done.finish(if outcome { Ok(errors) } else { Err(()) });
        });
        self.validation_sequence += 1;
        self.validation_tasks.insert(
            self.validation_sequence,
            ValidationTask {
                field: id,
                completion,
                _task: task,
            },
        );
        handle
    }
    pub fn validate_fields(
        &mut self,
        options: ValidateOptions,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> ValidationHandle {
        self.cancel_aggregate();
        self.aggregate_generation += 1;
        let generation = self.aggregate_generation;
        let epoch = self.epoch;
        let ids = self.store.selected(&options);
        let runs = ids
            .into_iter()
            .map(|id| {
                self.launch_validation(
                    id,
                    ValidationTrigger::Programmatic,
                    options.validate_only,
                    window,
                    cx,
                )
            })
            .collect::<Vec<_>>();
        let snapshot = self.snapshot();
        let completion = Rc::new(Completion::default());
        let done = completion.clone();
        let task = cx.spawn_in(window, async move |weak, cx| {
            let mut error_fields = Vec::new();
            let mut out_of_date = false;
            for run in runs {
                match run.await {
                    Ok(errors) if !errors.errors.is_empty() => error_fields.push(errors),
                    Ok(_) => {}
                    Err(()) => out_of_date = true,
                }
            }
            let current = cx
                .update(|_, cx| {
                    weak.upgrade().is_some_and(|entity| {
                        let form = entity.read(cx);
                        form.epoch == epoch && form.aggregate_generation == generation
                    })
                })
                .unwrap_or(false);
            out_of_date |= !current;
            done.finish(if error_fields.is_empty() && !out_of_date {
                Ok(snapshot.clone())
            } else {
                Err(FormValidationError {
                    values: snapshot,
                    error_fields,
                    out_of_date,
                })
            });
        });
        self.aggregate = Some((completion.clone(), task));
        ValidationHandle(completion)
    }
    pub fn submit(&mut self, window: &mut Window, cx: &mut Context<Self>) -> ValidationHandle {
        let handle = self.validate_fields(ValidateOptions::default(), window, cx);
        let result = handle.clone();
        self.submit_task = Some(cx.spawn_in(window, async move |weak, cx| {
            let result = result.await;
            let _ = cx.update(|window, cx| {
                if let Some(entity) = weak.upgrade() {
                    entity.update(cx, |form, cx| match result {
                        Ok(values) => form.emit(FormEvent::Finish(values), cx),
                        Err(error) => {
                            if !error.out_of_date
                                && let Some(focus) = form.scroll_to_first_error
                                && let Some(field) = error.error_fields.first()
                            {
                                form.scroll_to_field(&field.name, focus, window, cx);
                            }
                            form.emit(FormEvent::FinishFailed(error), cx);
                        }
                    });
                }
            });
        }));
        handle
    }
}
