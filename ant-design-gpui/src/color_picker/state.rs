// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    ColorPickerError, ColorPickerFormat, ColorPickerMode, ColorPickerValue, PickerColor,
    model::PickerModel,
};
use crate::{
    ConfigScope, InputEvent, InputNumberEvent, InputNumberState, InputState, NumericValue,
    SelectEvent, SelectOption, SelectOptions, SelectState, behavior::focus_handle,
    input_number::NumberOptions, popup::PopupHandle,
};
use gpui::{
    App, AppContext, Bounds, Context, Entity, EventEmitter, FocusHandle, Focusable, Pixels, Point,
    SharedString, Subscription, Window,
};
use std::{collections::HashMap, rc::Rc};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ColorPickerChangeSource {
    Palette,
    Hue,
    Alpha,
    Input,
    Preset,
    Gradient,
    Mode,
    Clear,
    Keyboard,
    Accessibility,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ColorPickerChange {
    pub value: ColorPickerValue,
    pub css: SharedString,
    pub source: ColorPickerChangeSource,
}

#[derive(Clone, Debug, PartialEq)]
pub enum ColorPickerEvent {
    Change(ColorPickerChange),
    ChangeComplete(ColorPickerChange),
    FormatChange(ColorPickerFormat),
    OpenChange(bool),
    Clear,
}

pub(super) type ChangeHandler = Rc<dyn Fn(&ColorPickerChange, &mut Window, &mut App)>;
pub(super) type FormatHandler = Rc<dyn Fn(ColorPickerFormat, &mut Window, &mut App)>;
pub(super) type OpenHandler = Rc<dyn Fn(bool, &mut Window, &mut App)>;
pub(super) type ClearHandler = Rc<dyn Fn(&mut Window, &mut App)>;

/// Logical bounds of the surfaces actually painted in the current window.
#[derive(Clone, Debug, Default)]
pub struct ColorPickerGeometry {
    pub trigger: Option<Bounds<Pixels>>,
    pub trigger_swatch: Option<Bounds<Pixels>>,
    pub panel: Option<Bounds<Pixels>>,
    pub palette: Option<Bounds<Pixels>>,
    pub hue: Option<Bounds<Pixels>>,
    pub alpha: Option<Bounds<Pixels>>,
    pub gradient: Option<Bounds<Pixels>>,
    pub preview: Option<Bounds<Pixels>>,
    pub clear: Option<Bounds<Pixels>>,
    pub editors: Vec<Bounds<Pixels>>,
    pub presets: Vec<Bounds<Pixels>>,
    pub handles: Vec<ColorPickerHandleGeometry>,
}

#[derive(Clone, Debug)]
pub struct ColorPickerHandleGeometry {
    pub kind: SharedString,
    pub index: usize,
    pub bounds: Bounds<Pixels>,
}

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub(super) enum Control {
    Palette,
    Saturation,
    Brightness,
    Hue,
    Alpha,
    Stop(u64),
    Clear,
    Preset(SharedString, usize),
    Group(SharedString),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Surface {
    Palette,
    Hue,
    Alpha,
    Gradient,
}

pub(super) struct Drag {
    pub surface: Surface,
    pub stop: Option<u64>,
    pub working: PickerModel,
    origin: PickerModel,
    pub changed: bool,
}

struct DiscreteOrigin {
    model: PickerModel,
    focused: Option<Control>,
}

enum EditCompletion {
    Continuous,
    Discrete(Option<DiscreteOrigin>),
}

/// Construct once per mounted picker. Editors, focus handles and gesture identity
/// survive rebuilding the lightweight ColorPicker/ColorPickerPanel builders.
pub struct ColorPickerState {
    pub(super) model: PickerModel,
    pub(super) accepted: ColorPickerValue,
    pub(super) controlled: bool,
    pub(super) initialized: bool,
    pub(super) format: ColorPickerFormat,
    pub(super) format_controlled: bool,
    pub(super) format_initialized: bool,
    pub(super) open: bool,
    pub(super) open_controlled: bool,
    pub(super) open_initialized: bool,
    pub(super) disabled: bool,
    pub(super) disabled_alpha: bool,
    pub(super) disabled_format: bool,
    pub(super) modes: Vec<ColorPickerMode>,
    pub(super) scope: Option<ConfigScope>,
    pub(super) trigger_focus: FocusHandle,
    pub(super) focuses: HashMap<Control, FocusHandle>,
    pub(super) groups: HashMap<SharedString, bool>,
    pub(super) hex: Entity<InputState>,
    pub(super) channels: [Entity<InputNumberState>; 4],
    pub(super) format_select: Entity<SelectState<ColorPickerFormat>>,
    pub(super) popup: PopupHandle,
    pub(super) drag: Option<Drag>,
    pub(super) key_active: bool,
    pub(super) key_changed: bool,
    key_origin: Option<PickerModel>,
    completion_proposal: Option<PickerModel>,
    pub(super) hovered: bool,
    pub(super) bounds: ColorPickerGeometry,
    pub(super) on_change: Option<ChangeHandler>,
    pub(super) on_complete: Option<ChangeHandler>,
    pub(super) on_format: Option<FormatHandler>,
    pub(super) on_open: Option<OpenHandler>,
    pub(super) on_clear: Option<ClearHandler>,
    pub(super) editor_format: Option<ColorPickerFormat>,
    _subscriptions: Vec<Subscription>,
    _focus_subscriptions: HashMap<Control, Vec<Subscription>>,
    _scope: Option<Subscription>,
}

impl EventEmitter<ColorPickerEvent> for ColorPickerState {}
impl Focusable for ColorPickerState {
    fn focus_handle(&self, _: &App) -> FocusHandle {
        self.trigger_focus.clone()
    }
}

impl ColorPickerState {
    pub fn new(window: &mut Window, cx: &mut Context<Self>) -> Self {
        let hex = cx.new(|cx| InputState::new(window, cx));
        let channels = std::array::from_fn(|_| cx.new(|cx| InputNumberState::new(window, cx)));
        let format_select = cx.new(|cx| {
            SelectState::new(
                SelectOptions::new([
                    SelectOption::new("hex", ColorPickerFormat::Hex, "HEX"),
                    SelectOption::new("rgb", ColorPickerFormat::Rgb, "RGB"),
                    SelectOption::new("hsb", ColorPickerFormat::Hsb, "HSB"),
                ])
                .expect("unique picker formats"),
                window,
                cx,
            )
        });
        let mut subscriptions =
            vec![cx.subscribe_in(&hex, window, |this, _, event, window, cx| {
                if let InputEvent::Change(change) = event
                    && !change.is_composing
                {
                    this.hex_change(&change.value, window, cx);
                }
            })];
        for (index, channel) in channels.iter().enumerate() {
            subscriptions.push(cx.subscribe_in(
                channel,
                window,
                move |this, _, event, window, cx| {
                    if let InputNumberEvent::Change(change) = event
                        && let Some(value) = change
                            .value
                            .as_ref()
                            .map_or(Some(0.), |v| v.to_number().ok())
                    {
                        this.channel_change(index, value, window, cx);
                    }
                },
            ));
        }
        subscriptions.push(cx.subscribe_in(
            &format_select,
            window,
            |this, _, event, window, cx| {
                if let SelectEvent::Change(change) = event
                    && let Some(value) = change.values.first()
                {
                    this.change_format(value.value, window, cx);
                }
            },
        ));
        subscriptions.push(cx.observe_window_activation(window, |this, window, cx| {
            if !window.is_window_active() {
                this.cancel(window, cx);
            }
        }));
        subscriptions.push(
            cx.observe_window_visibility(window, |this, _, window, cx| this.cancel(window, cx)),
        );
        let mut this = Self {
            model: PickerModel::new(ColorPickerValue::Cleared),
            accepted: ColorPickerValue::Cleared,
            controlled: false,
            initialized: false,
            format: ColorPickerFormat::Hex,
            format_controlled: false,
            format_initialized: false,
            open: false,
            open_controlled: false,
            open_initialized: false,
            disabled: false,
            disabled_alpha: false,
            disabled_format: false,
            modes: vec![ColorPickerMode::Single],
            scope: None,
            trigger_focus: focus_handle(cx),
            focuses: HashMap::new(),
            groups: HashMap::new(),
            hex,
            channels,
            format_select,
            popup: PopupHandle::new(),
            drag: None,
            key_active: false,
            key_changed: false,
            key_origin: None,
            completion_proposal: None,
            hovered: false,
            bounds: ColorPickerGeometry::default(),
            on_change: None,
            on_complete: None,
            on_format: None,
            on_open: None,
            on_clear: None,
            editor_format: None,
            _subscriptions: subscriptions,
            _focus_subscriptions: HashMap::new(),
            _scope: None,
        };
        this.sync_editors(None, window, cx);
        this
    }

    pub fn value(&self) -> &ColorPickerValue {
        &self.accepted
    }
    pub fn preview_value(&self) -> &ColorPickerValue {
        self.model.value()
    }
    pub fn format(&self) -> ColorPickerFormat {
        self.format
    }
    pub fn mode(&self) -> ColorPickerMode {
        self.model.mode()
    }
    pub fn is_open(&self) -> bool {
        self.open && !self.disabled
    }
    pub fn geometry(&self) -> ColorPickerGeometry {
        self.bounds.clone()
    }
    #[cfg(any(test, feature = "test-support"))]
    pub fn is_trigger_hovered(&self) -> bool {
        self.hovered
    }
    #[cfg(any(test, feature = "test-support"))]
    pub fn retained_editor_ids(&self) -> [gpui::EntityId; 6] {
        [
            self.hex.entity_id(),
            self.channels[0].entity_id(),
            self.channels[1].entity_id(),
            self.channels[2].entity_id(),
            self.channels[3].entity_id(),
            self.format_select.entity_id(),
        ]
    }
    #[cfg(any(test, feature = "test-support"))]
    pub fn focus_gradient_stop(
        &mut self,
        index: usize,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled {
            return;
        }
        if let Some(id) = self.model.stops().get(index).map(|stop| stop.id) {
            self.control_focus(Control::Stop(id), window, cx)
                .focus(window, cx);
        }
    }
    pub fn focus_hue(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if !self.disabled {
            self.control_focus(Control::Hue, window, cx)
                .focus(window, cx);
        }
    }
    pub fn set_value(
        &mut self,
        value: ColorPickerValue,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Result<(), ColorPickerError> {
        let value = value.canonicalized()?;
        self.restore_completion_proposal(Some(&value));
        self.initialized = true;
        if self.accepted == value && self.model.value() == &value {
            return Ok(());
        }
        if self.model.value() == &value {
            // An owner echo accepts the current precise proposal without
            // replacing stop identities or interrupting an active gesture.
            self.accepted = value;
            cx.notify();
            return Ok(());
        }
        self.accepted = value.clone();
        self.model.set_value(value);
        self.initialized = true;
        self.drag = None;
        self.key_active = false;
        self.key_origin = None;
        self.sync_editors(None, window, cx);
        cx.notify();
        Ok(())
    }
    pub fn set_format(
        &mut self,
        format: ColorPickerFormat,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.format_initialized = true;
        if self.format == format {
            return;
        }
        self.format = format;
        self.format_initialized = true;
        self.sync_editors(None, window, cx);
        cx.notify();
    }
    pub fn set_open(&mut self, open: bool, window: &mut Window, cx: &mut Context<Self>) {
        self.open_initialized = true;
        let open = open && !self.disabled;
        if self.open == open {
            return;
        }
        self.open = open && !self.disabled;
        self.open_initialized = true;
        if !self.open {
            self.cancel(window, cx);
            self.clear_panel_bounds();
        }
        cx.notify();
    }
    pub fn focus(&self, window: &mut Window, cx: &mut App) {
        if !self.disabled {
            self.trigger_focus.focus(window, cx);
        }
    }
    pub fn blur(&self, window: &mut Window, cx: &mut App) {
        if self.trigger_focus.contains_focused(window, cx)
            || self.focuses.values().any(|focus| focus.is_focused(window))
            || self.hex.read(cx).focus_handle(cx).is_focused(window)
            || self
                .channels
                .iter()
                .any(|channel| channel.read(cx).focus_handle(cx).is_focused(window))
            || self
                .format_select
                .read(cx)
                .focus_handle(cx)
                .is_focused(window)
        {
            window.blur(cx);
        }
    }
    /// Focus a channel editor. Index zero selects HEX in hex format, and indices
    /// zero through two select RGB/HSB channels. Index three selects alpha.
    pub fn focus_input(&self, index: usize, window: &mut Window, cx: &mut App) {
        if self.disabled || (index == 3 && self.disabled_alpha) {
            return;
        }
        if self.format == ColorPickerFormat::Hex && index != 0 && index != 3 {
            return;
        }
        if self.format == ColorPickerFormat::Hex && index == 0 {
            self.hex.read(cx).focus_handle(cx).focus(window, cx);
        } else if let Some(channel) = self.channels.get(index) {
            channel.read(cx).focus_handle(cx).focus(window, cx);
        }
    }

    pub(super) fn configure_scope(
        &mut self,
        scope: ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.scope.as_ref() != Some(&scope) {
            self._scope = Some(scope.observe_in(window, cx, |this, window, cx| {
                this.sync_editors(None, window, cx);
                cx.notify();
            }));
            self.scope = Some(scope);
        }
    }
    pub(super) fn control_focus(
        &mut self,
        control: Control,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> FocusHandle {
        if let Some(focus) = self.focuses.get(&control) {
            return focus.clone();
        }
        let focus = focus_handle(cx);
        let active = control.clone();
        let focused = cx.on_focus(&focus, window, move |this, window, cx| {
            if let Control::Stop(id) = active
                && let Some(index) = this.model.index_for_id(id)
            {
                this.model.set_active_index(index);
                this.sync_editors(None, window, cx);
            }
            cx.notify();
        });
        let blurred = cx.on_blur(&focus, window, |this, window, cx| {
            if this.key_active {
                this.cancel(window, cx);
            }
            cx.notify();
        });
        self._focus_subscriptions
            .insert(control.clone(), vec![focused, blurred]);
        focus
            .clone()
            .tab_stop(!self.disabled && !(control == Control::Alpha && self.disabled_alpha));
        self.focuses.insert(control, focus.clone());
        focus
    }

    /// Removed stops and preset rows release their focus listeners with their
    /// native identity; rebuilding a dynamic palette does not retain old closures.
    pub(super) fn retain_controls(
        &mut self,
        presets: &[(SharedString, usize)],
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled_alpha {
            self.bounds.alpha = None;
        }
        if self.model.mode() != ColorPickerMode::Gradient {
            self.bounds.gradient = None;
        }
        let available = |control: &Control| match control {
            Control::Stop(id) => {
                self.model.mode() == ColorPickerMode::Gradient
                    && self.model.index_for_id(*id).is_some()
            }
            Control::Preset(key, index) => {
                presets.iter().any(|(k, count)| k == key && index < count)
            }
            Control::Group(key) => presets.iter().any(|(k, _)| k == key),
            _ => true,
        };
        let removed_focus = self
            .focuses
            .iter()
            .any(|(control, focus)| !available(control) && focus.is_focused(window));
        self.focuses.retain(|control, _| available(control));
        self._focus_subscriptions
            .retain(|control, _| available(control));
        self.groups
            .retain(|key, _| presets.iter().any(|(k, _)| k == key));
        for (control, focus) in &self.focuses {
            let enabled = !self.disabled && !(control == &Control::Alpha && self.disabled_alpha);
            focus.clone().tab_stop(enabled);
            if !enabled && focus.is_focused(window) {
                window.blur(cx);
            }
        }
        self.trigger_focus.clone().tab_stop(!self.disabled);
        if removed_focus && !self.disabled && self.bounds.panel.is_some() {
            let control = if self.model.mode() == ColorPickerMode::Gradient {
                self.model
                    .stops()
                    .get(self.model.active_index())
                    .map_or(Control::Palette, |stop| Control::Stop(stop.id))
            } else {
                Control::Palette
            };
            self.control_focus(control, window, cx).focus(window, cx);
        }
    }

    fn emit(&self, event: ColorPickerEvent, window: &mut Window, cx: &mut Context<Self>) {
        cx.emit(event.clone());
        let change = self.on_change.clone();
        let complete = self.on_complete.clone();
        let format = self.on_format.clone();
        let open = self.on_open.clone();
        let clear = self.on_clear.clone();
        // A consumer may synchronously update this controller from its callback.
        // Leave the entity borrow before calling consumer code.
        window.defer(cx, move |window, cx| match event {
            ColorPickerEvent::Change(value) => {
                if let Some(f) = change {
                    f(&value, window, cx);
                }
            }
            ColorPickerEvent::ChangeComplete(value) => {
                if let Some(f) = complete {
                    f(&value, window, cx);
                }
            }
            ColorPickerEvent::FormatChange(value) => {
                if let Some(f) = format {
                    f(value, window, cx);
                }
            }
            ColorPickerEvent::OpenChange(value) => {
                if let Some(f) = open {
                    f(value, window, cx);
                }
            }
            ColorPickerEvent::Clear => {
                if let Some(f) = clear {
                    f(window, cx);
                }
            }
        });
    }
    fn change(&self, source: ColorPickerChangeSource) -> ColorPickerChange {
        let value = self.model.value().clone();
        ColorPickerChange {
            css: value.to_css_string().into(),
            value,
            source,
        }
    }
    fn discrete_completion(&mut self, window: &Window) -> EditCompletion {
        self.completion_proposal = None;
        EditCompletion::Discrete(self.controlled.then(|| {
            DiscreteOrigin {
                model: self.model.clone(),
                focused: self
                    .focuses
                    .iter()
                    .find_map(|(control, focus)| focus.is_focused(window).then(|| control.clone())),
            }
        }))
    }
    fn publish(
        &mut self,
        source: ColorPickerChangeSource,
        completion: EditCompletion,
        skip: Option<usize>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled {
            return;
        }
        if self.disabled_alpha {
            self.model.force_opaque_alpha();
        }
        if !self.controlled {
            self.accepted = self.model.value().clone();
        }
        let change = self.change(source);
        self.emit(ColorPickerEvent::Change(change.clone()), window, cx);
        if matches!(completion, EditCompletion::Discrete(_)) {
            self.emit(ColorPickerEvent::ChangeComplete(change), window, cx);
        }
        self.sync_editors(skip, window, cx);
        cx.notify();
        if let EditCompletion::Discrete(Some(origin)) = completion {
            // Owner echoes are processed before reconciliation; a rejected proposal
            // returns to its accepted color on the next owner render.
            let entity = cx.entity();
            window.defer(cx, move |window, cx| {
                entity.update(cx, |this, cx| {
                    if this.drag.is_none()
                        && !this.key_active
                        && this.model.value() != &this.accepted
                    {
                        let restore_focus = origin.model.value() == &this.accepted
                            && this.focuses.values().any(|focus| focus.is_focused(window));
                        this.reconcile_completion(Some(&origin.model));
                        this.sync_editors(None, window, cx);
                        if restore_focus
                            && !this.disabled
                            && let Some(control) = origin.focused
                        {
                            this.control_focus(control, window, cx).focus(window, cx);
                        }
                        cx.notify();
                    }
                });
            });
        }
    }
    pub(super) fn request_open(&mut self, open: bool, window: &mut Window, cx: &mut Context<Self>) {
        if self.disabled && open {
            return;
        }
        if !self.open_controlled {
            self.open = open;
        }
        if !open {
            self.cancel(window, cx);
            self.clear_panel_bounds();
        }
        self.emit(ColorPickerEvent::OpenChange(open), window, cx);
        cx.notify();
    }
    pub(super) fn restore_trigger_focus(&self, window: &mut Window, cx: &mut App) {
        let owns_focus = self.focuses.values().any(|focus| focus.is_focused(window))
            || self.hex.read(cx).focus_handle(cx).is_focused(window)
            || self
                .channels
                .iter()
                .any(|channel| channel.read(cx).focus_handle(cx).is_focused(window))
            || self
                .format_select
                .read(cx)
                .focus_handle(cx)
                .is_focused(window);
        if owns_focus {
            let focus = self
                .popup
                .trigger_focus_handle(cx)
                .unwrap_or_else(|| self.trigger_focus.clone());
            focus.focus(window, cx);
        }
    }
    pub(super) fn change_format(
        &mut self,
        format: ColorPickerFormat,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled || self.disabled_format || self.format == format {
            return;
        }
        if !self.format_controlled {
            self.format = format;
        }
        self.sync_editors(None, window, cx);
        self.emit(ColorPickerEvent::FormatChange(format), window, cx);
        cx.notify();
    }
    pub(super) fn choose_mode(
        &mut self,
        mode: ColorPickerMode,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if !self.disabled && self.modes.contains(&mode) {
            let completion = self.discrete_completion(window);
            if self.model.set_mode(mode) {
                self.publish(ColorPickerChangeSource::Mode, completion, None, window, cx);
            }
        }
    }
    pub(super) fn clear(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if !self.disabled {
            let completion = self.discrete_completion(window);
            if self.model.clear() {
                self.publish(ColorPickerChangeSource::Clear, completion, None, window, cx);
                self.emit(ColorPickerEvent::Clear, window, cx);
            }
        }
    }
    pub(super) fn choose_preset(
        &mut self,
        value: ColorPickerValue,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled {
            return;
        }
        let completion = self.discrete_completion(window);
        self.model.set_value(value);
        self.publish(
            ColorPickerChangeSource::Preset,
            completion,
            None,
            window,
            cx,
        );
    }
    pub(super) fn cancel(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.completion_proposal = None;
        let drag = self.drag.take();
        let active = drag.is_some() || self.key_active;
        let key_origin = self.key_origin.take();
        let origin = drag.map(|drag| drag.origin).or(key_origin);
        self.key_active = false;
        self.key_changed = false;
        if active && self.controlled {
            self.reconcile_model(origin.as_ref());
        }
        if active {
            self.sync_editors(None, window, cx);
            cx.notify();
        }
    }
    fn reconcile_model(&mut self, origin: Option<&PickerModel>) -> bool {
        if self.model.value() == &self.accepted {
            return false;
        }
        if let Some(origin) = origin.filter(|model| model.value() == &self.accepted) {
            self.model = origin.clone();
        } else {
            self.model.set_value(self.accepted.clone());
        }
        true
    }
    fn reconcile_completion(&mut self, origin: Option<&PickerModel>) {
        // A callback can notify its owner without immediately rebuilding the
        // controlled builder. Keep the precise proposal through rollback so that
        // the next render's owner echo can accept it without reassigning stop IDs.
        let proposal = self.model.clone();
        if self.reconcile_model(origin) {
            self.completion_proposal = Some(proposal);
        }
    }
    pub(super) fn restore_completion_proposal(&mut self, value: Option<&ColorPickerValue>) -> bool {
        let Some(proposal) = self.completion_proposal.take() else {
            return false;
        };
        if Some(proposal.value()) == value && Some(self.model.value()) != value {
            self.model = proposal;
            true
        } else {
            false
        }
    }
    pub(super) fn release_mount(&mut self) {
        self.completion_proposal = None;
        let key_origin = self.key_origin.take();
        let origin = self.drag.take().map(|drag| drag.origin).or(key_origin);
        if self.controlled {
            self.reconcile_model(origin.as_ref());
        }
        self.key_active = false;
        self.key_changed = false;
        self.hovered = false;
        self.clear_panel_bounds();
        self.bounds.trigger = None;
        self.bounds.trigger_swatch = None;
    }
    pub(super) fn clear_panel_bounds(&mut self) {
        self.bounds.panel = None;
        self.bounds.palette = None;
        self.bounds.hue = None;
        self.bounds.alpha = None;
        self.bounds.gradient = None;
        self.bounds.preview = None;
        self.bounds.clear = None;
        self.bounds.editors.clear();
        self.bounds.presets.clear();
        self.bounds.handles.clear();
    }

    pub(super) fn sync_editors(
        &mut self,
        skip: Option<usize>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let color = self.model.active_color();
        let (h, s, b, a) = color.to_hsb();
        let (r, g, blue, _) = color.to_rgb();
        let values = if self.format == ColorPickerFormat::Hsb {
            [
                h.round(),
                (s * 100.).round(),
                (b * 100.).round(),
                (a * 100.).round(),
            ]
        } else {
            [
                f64::from(r),
                f64::from(g),
                f64::from(blue),
                (a * 100.).round(),
            ]
        };
        if self.editor_format != Some(self.format) {
            for (index, channel) in self.channels.iter().enumerate() {
                let max = if index == 3 {
                    100
                } else if self.format == ColorPickerFormat::Hsb {
                    if index == 0 { 360 } else { 100 }
                } else {
                    255
                };
                let percent = index == 3 || (self.format == ColorPickerFormat::Hsb && index > 0);
                let options = NumberOptions {
                    minimum: Some(0.into()),
                    maximum: Some(max.into()),
                    precision: Some(0),
                    formatter: percent.then(|| {
                        Rc::new(|context: crate::input_number::NumberFormatContext<'_>| {
                            if context.user_typing {
                                context.input.to_string()
                            } else {
                                format!("{}%", context.canonical)
                            }
                        }) as crate::input_number::NumberFormatter
                    }),
                    parser: percent.then(|| {
                        Rc::new(|text: &str| text.replace('%', ""))
                            as crate::input_number::NumberParser
                    }),
                    ..Default::default()
                };
                channel.update(cx, |s, cx| {
                    s.set_options(options, window, cx)
                        .expect("bounded picker channel")
                });
            }
            self.editor_format = Some(self.format);
        }
        for (index, channel) in self.channels.iter().enumerate() {
            if skip != Some(index) {
                channel.update(cx, |s, cx| {
                    s.set_value(Some(NumericValue::Number(values[index])), window, cx)
                        .expect("finite picker channel")
                });
            }
        }
        let hex = color
            .to_hex_string()
            .trim_start_matches('#')
            .chars()
            .take(6)
            .collect::<String>()
            .to_ascii_uppercase();
        if skip != Some(4) {
            self.hex.update(cx, |s, cx| s.set_value(hex, window, cx));
        }
        self.format_select
            .update(cx, |s, cx| s.set_values([self.format], cx));
    }
    fn hex_change(&mut self, text: &str, window: &mut Window, cx: &mut Context<Self>) {
        if self.disabled {
            return;
        }
        let filtered: String = text
            .chars()
            .filter(char::is_ascii_hexdigit)
            .take(8)
            .collect();
        let display = filtered
            .chars()
            .take(6)
            .collect::<String>()
            .to_ascii_uppercase();
        self.hex.update(cx, |s, cx| {
            let selection = s.selection();
            let len = display.len();
            s.format_value(
                &display,
                crate::input::TextSelection {
                    anchor: selection.anchor.min(len),
                    caret: selection.caret.min(len),
                },
                true,
                window,
                cx,
            );
        });
        if matches!(filtered.len(), 6 | 8)
            && let Ok(color) = PickerColor::parse(&filtered)
        {
            let completion = self.discrete_completion(window);
            self.model.set_active_color(color);
            self.publish(
                ColorPickerChangeSource::Input,
                completion,
                Some(4),
                window,
                cx,
            );
        }
    }
    fn channel_change(
        &mut self,
        index: usize,
        value: f64,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled || !value.is_finite() {
            return;
        }
        let completion = self.discrete_completion(window);
        let mut color = self.model.active_color();
        if self.model.value().is_cleared() {
            color = color.with_alpha(1.);
        }
        color = if index == 3 {
            color.with_alpha(value / 100.)
        } else if self.format == ColorPickerFormat::Hsb {
            match index {
                0 => color.with_hue(value),
                1 => color.with_saturation(value / 100.),
                _ => color.with_brightness(value / 100.),
            }
        } else {
            let (r, g, b, a) = color.to_rgb();
            let mut rgb = [f64::from(r), f64::from(g), f64::from(b)];
            rgb[index] = value.clamp(0., 255.);
            PickerColor::from_rgb(rgb[0], rgb[1], rgb[2], a).expect("bounded RGB")
        };
        self.model.set_active_color(color);
        self.publish(
            ColorPickerChangeSource::Input,
            completion,
            Some(index),
            window,
            cx,
        );
    }

    pub(super) fn surface_bounds(&self, surface: Surface) -> Option<Bounds<Pixels>> {
        match surface {
            Surface::Palette => self.bounds.palette,
            Surface::Hue => self.bounds.hue,
            Surface::Alpha => self.bounds.alpha,
            Surface::Gradient => self.bounds.gradient,
        }
    }
    fn editing_color(&self, control: &Control) -> PickerColor {
        let color = self.model.active_color();
        if !self.model.value().is_cleared() {
            return color;
        }
        match control {
            Control::Hue => PickerColor::new(0., 1., 1., 1.).expect("valid first hue color"),
            Control::Alpha => PickerColor::new(0., 1., 1., 0.).expect("valid first alpha color"),
            _ => color.with_alpha(1.),
        }
    }
    pub(super) fn pointer_start(
        &mut self,
        surface: Surface,
        stop: Option<u64>,
        position: Point<Pixels>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled
            || (surface == Surface::Alpha && self.disabled_alpha)
            || self.surface_bounds(surface).is_none()
        {
            return;
        }
        let bounds = self
            .surface_bounds(surface)
            .expect("checked surface bounds");
        if bounds.size.width <= gpui::px(0.)
            || bounds.size.height <= gpui::px(0.)
            || stop.is_some_and(|id| self.model.index_for_id(id).is_none())
        {
            return;
        }
        self.key_active = false;
        self.key_origin = None;
        self.completion_proposal = None;
        let mut origin = self.model.clone();
        let previous = self.model.value().clone();
        let mut stop = stop;
        if surface == Surface::Gradient && stop.is_none() {
            let bounds = self.surface_bounds(surface).unwrap();
            let percent =
                f64::from(((position.x - bounds.left()) / bounds.size.width).clamp(0., 1.)) * 100.;
            stop = self.model.add_stop(percent);
        }
        if let Some(id) = stop
            && let Some(index) = self.model.index_for_id(id)
        {
            self.model.set_active_index(index);
            if let Some(index) = origin.index_for_id(id) {
                origin.set_active_index(index);
            }
        }
        let control = match surface {
            Surface::Palette => Control::Palette,
            Surface::Hue => Control::Hue,
            Surface::Alpha => Control::Alpha,
            Surface::Gradient => stop.map_or(Control::Palette, Control::Stop),
        };
        self.control_focus(control, window, cx).focus(window, cx);
        self.drag = Some(Drag {
            surface,
            stop,
            working: self.model.clone(),
            origin,
            changed: false,
        });
        if previous != *self.model.value() {
            self.publish(
                ColorPickerChangeSource::Gradient,
                EditCompletion::Continuous,
                None,
                window,
                cx,
            );
        }
        self.pointer_move(position, window, cx);
    }
    pub(super) fn pointer_move(
        &mut self,
        position: Point<Pixels>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let Some(mut drag) = self.drag.take() else {
            return;
        };
        if self.disabled {
            self.drag = Some(drag);
            self.cancel(window, cx);
            return;
        }
        let Some(bounds) = self.surface_bounds(drag.surface) else {
            self.drag = Some(drag);
            self.cancel(window, cx);
            return;
        };
        if bounds.size.width <= gpui::px(0.) || bounds.size.height <= gpui::px(0.) {
            self.drag = Some(drag);
            self.cancel(window, cx);
            return;
        }
        let x = f64::from(((position.x - bounds.left()) / bounds.size.width).clamp(0., 1.));
        let y = f64::from(((position.y - bounds.top()) / bounds.size.height).clamp(0., 1.));
        let old = self.model.value().clone();
        if drag.surface == Surface::Gradient {
            self.model = drag.working.clone();
            if let Some(id) = drag.stop {
                self.model.move_stop(id, x * 100.);
                drag.working = self.model.clone();
                if (position.y - bounds.center().y).abs() > gpui::px(130.) {
                    self.model.remove_stop(id);
                }
            }
        } else {
            let control = match drag.surface {
                Surface::Palette => Control::Palette,
                Surface::Hue => Control::Hue,
                Surface::Alpha => Control::Alpha,
                Surface::Gradient => unreachable!(),
            };
            let color = self.editing_color(&control);
            let color = match drag.surface {
                Surface::Palette => color.with_saturation(x).with_brightness(1. - y),
                Surface::Hue => color.with_hue(x * 360.),
                Surface::Alpha => color.with_alpha(x),
                Surface::Gradient => unreachable!(),
            };
            self.model.set_active_color(color);
        }
        let changed = old != *self.model.value();
        drag.changed |= changed;
        let source = match drag.surface {
            Surface::Palette => ColorPickerChangeSource::Palette,
            Surface::Hue => ColorPickerChangeSource::Hue,
            Surface::Alpha => ColorPickerChangeSource::Alpha,
            Surface::Gradient => ColorPickerChangeSource::Gradient,
        };
        self.drag = Some(drag);
        if changed {
            self.publish(source, EditCompletion::Continuous, None, window, cx);
        } else {
            cx.notify();
        }
    }
    pub(super) fn pointer_end(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let Some(drag) = self.drag.take() else {
            return;
        };
        if !self.disabled {
            let source = match drag.surface {
                Surface::Palette => ColorPickerChangeSource::Palette,
                Surface::Hue => ColorPickerChangeSource::Hue,
                Surface::Alpha => ColorPickerChangeSource::Alpha,
                Surface::Gradient => ColorPickerChangeSource::Gradient,
            };
            self.emit(
                ColorPickerEvent::ChangeComplete(self.change(source)),
                window,
                cx,
            );
            if self.controlled {
                // Run after the completion callback has had a chance to accept.
                let entity = cx.entity();
                let origin = drag.origin;
                window.defer(cx, move |window, cx| {
                    entity.update(cx, |this, cx| {
                        if this.drag.is_none()
                            && !this.key_active
                            && this.model.value() != &this.accepted
                        {
                            this.reconcile_completion(Some(&origin));
                            this.sync_editors(None, window, cx);
                            cx.notify();
                        }
                    });
                });
            }
        }
        cx.notify();
    }

    pub(super) fn key(
        &mut self,
        control: Control,
        key: &str,
        shift: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> bool {
        if self.disabled {
            return false;
        }
        if matches!(control, Control::Stop(_)) && matches!(key, "delete" | "backspace") {
            let completion = self.discrete_completion(window);
            if let Control::Stop(id) = control
                && self.model.remove_stop(id)
            {
                if let Some(next) = self
                    .model
                    .stops()
                    .get(self.model.active_index())
                    .map(|stop| stop.id)
                {
                    self.control_focus(Control::Stop(next), window, cx)
                        .focus(window, cx);
                }
                self.publish(
                    ColorPickerChangeSource::Keyboard,
                    completion,
                    None,
                    window,
                    cx,
                );
            }
            return true;
        }
        if !matches!(
            key,
            "left" | "right" | "up" | "down" | "home" | "end" | "pageup" | "pagedown"
        ) {
            return false;
        }
        let units = if shift {
            10.
        } else if matches!(key, "pageup" | "pagedown") {
            2.
        } else {
            1.
        };
        let sign = if matches!(key, "left" | "down" | "pagedown") {
            -1.
        } else {
            1.
        };
        let target = |current: f64, max: f64| match key {
            "home" => 0.,
            "end" => max,
            _ => (current + sign * units).clamp(0., max),
        };
        let previous = self.model.value().clone();
        if !self.key_active {
            self.completion_proposal = None;
            self.key_origin = Some(self.model.clone());
        }
        let color = self.editing_color(&control);
        let (h, s, b, a) = color.to_hsb();
        let next = match control {
            Control::Palette => {
                if matches!(key, "up" | "down") {
                    color.with_brightness(target(b * 100., 100.) / 100.)
                } else {
                    color.with_saturation(target(s * 100., 100.) / 100.)
                }
            }
            Control::Saturation => color.with_saturation(target(s * 100., 100.) / 100.),
            Control::Brightness => color.with_brightness(target(b * 100., 100.) / 100.),
            Control::Hue => color.with_hue(target(h, 360.)),
            Control::Alpha if !self.disabled_alpha => {
                color.with_alpha(target(a * 100., 100.) / 100.)
            }
            Control::Stop(id) => {
                if let Some(stop) = self.model.stops().iter().find(|s| s.id == id) {
                    let value = target(stop.percent, 100.);
                    self.model.move_stop(id, value);
                }
                color
            }
            _ => return false,
        };
        if !matches!(control, Control::Stop(_)) {
            self.model.set_active_color(next);
        }
        self.key_active = true;
        if previous != *self.model.value() {
            self.key_changed = true;
            self.publish(
                ColorPickerChangeSource::Keyboard,
                EditCompletion::Continuous,
                None,
                window,
                cx,
            );
        }
        true
    }
    pub(super) fn key_end(&mut self, key: &str, window: &mut Window, cx: &mut Context<Self>) {
        if self.key_active
            && matches!(
                key,
                "left" | "right" | "up" | "down" | "home" | "end" | "pageup" | "pagedown"
            )
        {
            self.key_active = false;
            if self.key_changed {
                self.emit(
                    ColorPickerEvent::ChangeComplete(
                        self.change(ColorPickerChangeSource::Keyboard),
                    ),
                    window,
                    cx,
                );
            }
            self.key_changed = false;
            let origin = self.key_origin.take();
            if self.controlled {
                let entity = cx.entity();
                window.defer(cx, move |window, cx| {
                    entity.update(cx, |this, cx| {
                        if this.drag.is_none()
                            && !this.key_active
                            && this.model.value() != &this.accepted
                        {
                            this.reconcile_completion(origin.as_ref());
                            this.sync_editors(None, window, cx);
                            cx.notify();
                        }
                    })
                });
            }
            cx.notify();
        }
    }
    pub(super) fn accessible_value(
        &mut self,
        control: Control,
        value: f64,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled || !value.is_finite() {
            return;
        }
        let completion = self.discrete_completion(window);
        let color = self.editing_color(&control);
        let next = match control {
            Control::Hue => color.with_hue(value),
            Control::Alpha if !self.disabled_alpha => color.with_alpha(value / 100.),
            Control::Saturation => color.with_saturation(value / 100.),
            Control::Brightness => color.with_brightness(value / 100.),
            Control::Stop(id) => {
                self.model.move_stop(id, value);
                color
            }
            _ => return,
        };
        if !matches!(control, Control::Stop(_)) {
            self.model.set_active_color(next);
        }
        self.publish(
            ColorPickerChangeSource::Accessibility,
            completion,
            None,
            window,
            cx,
        );
    }
}
