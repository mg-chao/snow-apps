// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Explicit control adapters. Each mounted control has one retained subscription
//! bundle; factories rebuild builders without rebuilding the editing controller.

use std::{cell::Cell, rc::Rc};

use gpui::{
    AnyElement, App, Context, ElementId, Entity, EntityId, EventEmitter, FocusHandle, IntoElement,
    RenderOnce, SharedString, Subscription, Window, div, prelude::*,
};

use super::{
    ControlToken, FieldHandle, FormError, FormField, FormItem, FormItemContext, FormPresence,
    FormValue, ValidationTrigger,
};
pub(crate) type NativeActivation = Rc<dyn Fn(crate::behavior::Activation, &mut Window, &mut App)>;

/// A fresh native control builder created by a FormItem's repeatable factory.
pub trait FormControl<T: Clone + PartialEq + 'static>: Sized + 'static {
    fn into_form_element(
        self,
        field: FormField<T>,
        context: FormItemContext,
        window: &mut Window,
        cx: &mut App,
    ) -> AnyElement;
}

#[derive(IntoElement)]
struct BoundControl<T: Clone + PartialEq + 'static, C: FormControl<T>> {
    control: C,
    field: FormField<T>,
    context: FormItemContext,
}
impl<T: Clone + PartialEq + 'static, C: FormControl<T>> RenderOnce for BoundControl<T, C> {
    fn render(self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        self.control
            .into_form_element(self.field, self.context, window, cx)
    }
}

impl FormItem {
    /// Runtime-aware factory for controls whose constructors need GPUI context.
    pub fn control_with_app<T: Clone + PartialEq + 'static, C: FormControl<T>>(
        self,
        field: &FormField<T>,
        factory: impl Fn(FormItemContext, &mut Window, &mut App) -> C + 'static,
    ) -> Self {
        let field = field.clone();
        let factory = Rc::new(factory);
        self.control_with(field.handle(), move |context| {
            BoundFactory {
                factory: factory.clone(),
                field: field.clone(),
                context,
            }
            .into_any_element()
        })
    }

    /// Build only this field's control when its value, metadata or scope changes.
    pub fn control<T: Clone + PartialEq + 'static, C: FormControl<T>>(
        self,
        field: &FormField<T>,
        factory: impl Fn(FormItemContext) -> C + 'static,
    ) -> Self {
        let field = field.clone();
        self.control_with(field.handle(), move |context| {
            BoundControl {
                control: factory(context.clone()),
                field: field.clone(),
                context,
            }
            .into_any_element()
        })
    }
}

type ControlFactory<C> = dyn Fn(FormItemContext, &mut Window, &mut App) -> C;

#[derive(IntoElement)]
struct BoundFactory<T: Clone + PartialEq + 'static, C: FormControl<T>> {
    factory: Rc<ControlFactory<C>>,
    field: FormField<T>,
    context: FormItemContext,
}
impl<T: Clone + PartialEq + 'static, C: FormControl<T>> RenderOnce for BoundFactory<T, C> {
    fn render(self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let control = (self.factory)(self.context.clone(), window, cx);
        control.into_form_element(self.field, self.context, window, cx)
    }
}

/// An adapter event independent of a particular native control's event type.
pub enum FormControlEvent<T> {
    None,
    Change(FormPresence<T>),
    Focus,
    Blur,
    Submit,
}

/// A mounted bridge between a typed field and an existing retained controller.
/// Its subscriptions and focus/reset registration end when the mount is released.
pub struct FormControlBinding {
    field: FieldHandle,
    controller: EntityId,
    token: ControlToken,
    error: Option<FormError>,
    last_value: FormValue,
    _subscriptions: Vec<Subscription>,
    _release: Subscription,
}

impl FormControlBinding {
    /// Custom retained controls can use this bridge without implementing a new
    /// store. `write` must be silent; `reset` also discards transient editing state.
    /// The native controller's entity identity owns its one mounted bridge.
    pub fn retained<S, E, T>(
        field: &FormField<T>,
        controller: &Entity<S>,
        focus: Option<FocusHandle>,
        event: impl Fn(&E) -> FormControlEvent<T> + 'static,
        write: impl Fn(
            &Entity<S>,
            FormPresence<T>,
            bool,
            &mut Window,
            &mut App,
        ) -> Result<(), FormError>
        + 'static,
        window: &mut Window,
        cx: &mut App,
    ) -> Result<ControlToken, FormError>
    where
        S: EventEmitter<E> + 'static,
        E: 'static,
        T: Clone + PartialEq + 'static,
    {
        type Write<S, T> = dyn Fn(
            &Entity<S>,
            FormPresence<T>,
            bool,
            &mut Window,
            &mut App,
        ) -> Result<(), FormError>;
        let id = ("form-control-binding", controller.entity_id().as_u64());
        let field = field.clone();
        let controller = controller.clone();
        let write: Rc<Write<S, T>> = Rc::new(write);
        let bridge = window.use_keyed_state(id, cx, |window, cx| {
            let weak = controller.downgrade();
            let reset_state = field.handle().state().downgrade();
            let reset_codec = field.codec().clone();
            let reset_write = write.clone();
            let skip_blur = Rc::new(Cell::new(false));
            let reset_skip_blur = skip_blur.clone();
            let reset = Rc::new(move |window: &mut Window, cx: &mut App| {
                // A composite may currently focus a panel or a second endpoint.
                // Its next real Focus clears this guard if reset emits no Blur.
                reset_skip_blur.set(true);
                if let Some(controller) = weak.upgrade()
                    && let Some(state) = reset_state.upgrade()
                    && let Ok(value) = reset_codec.decode_presence(Some(state.read(cx).value()))
                {
                    let _ = reset_write(&controller, value, true, window, cx);
                }
            });
            let handle = field.handle();
            let initial_value = handle.value(cx);
            let initial_error = field
                .read(cx)
                .and_then(|value| write(&controller, value, false, window, cx))
                .err();
            let token = handle.register_control(focus, reset, cx);
            let observe_field = field.clone();
            let observe_controller = controller.downgrade();
            let observe_write = write.clone();
            let values = cx.observe_in(
                handle.state(),
                window,
                move |binding: &mut Self, _, window, cx| {
                    let value = observe_field.handle().value(cx);
                    if binding.error.is_none()
                        && (binding.last_value.ptr_eq(&value) || binding.last_value == value)
                    {
                        // Status notifications must not overwrite uncommitted native drafts.
                        return;
                    }
                    if let Some(controller) = observe_controller.upgrade() {
                        match observe_field.read(cx) {
                            Ok(decoded) => {
                                binding.error =
                                    observe_write(&controller, decoded, false, window, cx).err();
                                if binding.error.is_none() {
                                    binding.last_value = value;
                                }
                            }
                            Err(error) => binding.error = Some(error),
                        }
                    }
                },
            );
            let change_field = field.clone();
            let change_controller = controller.downgrade();
            let change_write = write.clone();
            let events = cx.subscribe_in(
                &controller,
                window,
                move |binding: &mut Self, _, value, window, cx| {
                    let result = match event(value) {
                        FormControlEvent::None => return,
                        FormControlEvent::Focus => {
                            skip_blur.set(false);
                            return;
                        }
                        FormControlEvent::Change(value) => {
                            let proposal = change_field.codec().encode_presence(value.clone());
                            let result =
                                change_field.change(value, ValidationTrigger::Change, window, cx);
                            // Normalization can reject an edit without changing the store.
                            // Restore that proposal explicitly instead of echoing values
                            // over every native draft during rendering.
                            if result.is_ok() && proposal != change_field.handle().value(cx) {
                                match (change_controller.upgrade(), change_field.read(cx)) {
                                    (Some(controller), Ok(value)) => {
                                        change_write(&controller, value, false, window, cx)
                                    }
                                    (_, Err(error)) => Err(error),
                                    _ => Err(FormError::Detached),
                                }
                            } else {
                                result
                            }
                        }
                        FormControlEvent::Blur => {
                            if skip_blur.replace(false) {
                                return;
                            }
                            let handle = change_field.handle();
                            if let Some(form) = handle.form() {
                                form.update(cx, |form, cx| {
                                    form.blur_field(handle.id(), window, cx)
                                });
                            }
                            Ok(())
                        }
                        FormControlEvent::Submit => {
                            if let Some(form) = change_field.handle().form() {
                                form.update(cx, |form, cx| {
                                    form.submit(window, cx);
                                });
                            }
                            Ok(())
                        }
                    };
                    binding.error = result.err();
                },
            );
            Self {
                field: handle,
                controller: controller.entity_id(),
                token,
                error: initial_error,
                last_value: initial_value,
                _subscriptions: vec![values, events],
                _release: cx.on_release(|binding: &mut Self, cx| {
                    binding.field.clear_control(binding.token, cx);
                }),
            }
        });
        // Reusing an editing entity for a different field is a host composition
        // error; do not silently redirect the old subscription to a new field.
        let binding = bridge.read(cx);
        if binding.field.state().entity_id() != field.handle().state().entity_id()
            || binding.controller != controller.entity_id()
        {
            return Err(FormError::ForeignField);
        }
        if let Some(error) = &binding.error {
            return Err(error.clone());
        }
        Ok(binding.token)
    }

    /// Resolve focus through the mounted control's current availability.
    pub(crate) fn install_focus<S: 'static>(
        field: &FieldHandle,
        token: ControlToken,
        state: &Entity<S>,
        focus: impl Fn(&mut S, &mut Window, &mut Context<S>) -> bool + 'static,
        cx: &mut App,
    ) {
        Self::install_native_actions(field, token, state, focus, None, cx);
    }
    pub(crate) fn install_native_actions<S: 'static>(
        field: &FieldHandle,
        token: ControlToken,
        state: &Entity<S>,
        focus: impl Fn(&mut S, &mut Window, &mut Context<S>) -> bool + 'static,
        activate: Option<fn(&S) -> Option<NativeActivation>>,
        cx: &mut App,
    ) {
        let weak = state.downgrade();
        let focus_action: super::ControlAction = Rc::new(move |window, cx| {
            weak.update(cx, |state, cx| focus(state, window, cx))
                .unwrap_or(false)
        });
        let label_action = activate.map(|activate| {
            let weak = state.downgrade();
            let focus = focus_action.clone();
            Rc::new(move |window: &mut Window, cx: &mut App| {
                if !focus(window, cx) {
                    return false;
                }
                let action = weak.update(cx, |state, _| activate(state)).ok().flatten();
                if let Some(action) = action {
                    action(crate::behavior::Activation::Pointer, window, cx);
                    true
                } else {
                    false
                }
            }) as super::ControlAction
        });
        field.set_control_actions(token, Some(focus_action), label_action, cx);
    }

    /// Bind a controlled checkbox, switch or option group. Reuse the returned
    /// focus handles on the actual options; the bridge creates no extra Tab stop.
    pub fn controlled<T: Clone + PartialEq + 'static>(
        id: impl Into<ElementId>,
        field: &FormField<T>,
        options: Vec<(ElementId, Option<FocusHandle>)>,
        window: &mut Window,
        cx: &mut App,
    ) -> Result<(FormPresence<T>, Vec<FocusHandle>, ControlToken), FormError> {
        let handle = field.handle();
        let mount = window.use_keyed_state(id, cx, |_, cx| ControlledBinding {
            field: handle.clone(),
            options: Vec::new(),
            token: None,
            skip_blur: Rc::new(Cell::new(false)),
            _subscriptions: Vec::new(),
            _release: cx.on_release(|binding: &mut ControlledBinding, cx| {
                if let Some(token) = binding.token {
                    binding.field.clear_control(token, cx);
                }
            }),
        });
        mount.update(cx, |binding, cx| {
            let changed = binding.token.is_none()
                || binding.field.state().entity_id() != handle.state().entity_id()
                || binding.options.len() != options.len()
                || binding.options.iter().zip(&options).any(
                    |((old_id, old_focus), (id, focus))| {
                        old_id != id || focus.as_ref().is_some_and(|focus| focus != old_focus)
                    },
                );
            if changed {
                if let Some(token) = binding.token.take() {
                    binding.field.clear_control(token, cx);
                }
                binding.field = handle.clone();
                let previous = std::mem::take(&mut binding.options);
                binding.options = options
                    .into_iter()
                    .map(|(id, focus)| {
                        let focus = focus
                            .or_else(|| {
                                previous
                                    .iter()
                                    .find(|(key, _)| key == &id)
                                    .map(|(_, focus)| focus.clone())
                            })
                            .unwrap_or_else(|| crate::behavior::focus_handle(cx));
                        (id, focus)
                    })
                    .collect();
                let focuses: Vec<_> = binding
                    .options
                    .iter()
                    .map(|(_, focus)| focus.clone())
                    .collect();
                binding._subscriptions = focuses
                    .iter()
                    .map(|focus| {
                        let group = focuses.clone();
                        cx.on_blur(focus, window, move |binding, window, cx| {
                            if binding.skip_blur.replace(false) {
                                return;
                            }
                            let field = binding.field.clone();
                            let group = group.clone();
                            window.defer(cx, move |window, cx| {
                                if !group.iter().any(|focus| focus.contains_focused(window, cx)) {
                                    field.blur(window, cx);
                                }
                            });
                        })
                    })
                    .collect();
                for focus in &focuses {
                    binding
                        ._subscriptions
                        .push(cx.on_focus(focus, window, |binding, _, _| {
                            binding.skip_blur.set(false);
                        }));
                }
                let reset_focuses = focuses.clone();
                let skip_blur = binding.skip_blur.clone();
                binding.token = Some(binding.field.register_control(
                    focuses.first().cloned(),
                    Rc::new(move |window, cx| {
                        skip_blur.set(true);
                        if reset_focuses
                            .iter()
                            .any(|focus| focus.contains_focused(window, cx))
                        {
                            window.blur(cx);
                        }
                    }),
                    cx,
                ));
            }
        });
        let focuses = mount
            .read(cx)
            .options
            .iter()
            .map(|(_, focus)| focus.clone())
            .collect();
        let token = mount.read(cx).token.ok_or(FormError::Detached)?;
        Ok((field.read(cx)?, focuses, token))
    }
}

struct ControlledBinding {
    field: FieldHandle,
    options: Vec<(ElementId, FocusHandle)>,
    token: Option<ControlToken>,
    skip_blur: Rc<Cell<bool>>,
    _subscriptions: Vec<Subscription>,
    _release: Subscription,
}

pub(crate) fn binding_error(id: ElementId, error: &FormError) -> AnyElement {
    // A failed raw-value assignment remains visible and diagnosable instead of
    // erasing the value or drawing a misleading empty editable control.
    div()
        .id(id)
        .role(gpui::Role::Alert)
        .child(SharedString::from(error.to_string()))
        .into_any_element()
}
