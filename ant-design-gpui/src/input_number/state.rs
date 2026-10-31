// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::time::Duration;

use gpui::{
    App, AppContext, Context, Entity, EventEmitter, FocusHandle, Focusable, SharedString,
    Subscription, Task, Window,
};

use super::{
    ExactDecimal, NumberError, NumberOptions, NumericDraft, NumericValue,
    model::{NumberModel, NumberUpdate},
};
use crate::input::{CommitReason, EditOrigin, InputEvent, InputState, TextSelection};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StepDirection {
    Up,
    Down,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StepSource {
    Button,
    Keyboard,
    Wheel,
    Accessibility,
    Programmatic,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NumericChangeSource {
    Input(EditOrigin),
    Commit(CommitReason),
    Step(StepSource),
}

#[derive(Clone, Debug)]
pub struct NumericChange {
    pub value: Option<NumericValue>,
    pub source: NumericChangeSource,
}
#[derive(Clone, Debug)]
pub struct NumericCommit {
    pub value: Option<NumericValue>,
    pub reason: CommitReason,
}
#[derive(Clone, Debug)]
pub struct NumericInput {
    pub text: SharedString,
    pub status: NumericDraft,
    pub origin: EditOrigin,
}
#[derive(Clone, Debug)]
pub struct NumericStep {
    pub value: Option<NumericValue>,
    pub offset: ExactDecimal,
    pub direction: StepDirection,
    pub source: StepSource,
}

#[derive(Clone, Debug)]
pub enum InputNumberEvent {
    Input(NumericInput),
    Change(NumericChange),
    Commit(NumericCommit),
    Step(NumericStep),
    Focus,
    Blur,
    Error(NumberError),
}

/// Retained numeric controller. The shared InputState owns selection, IME and
/// undo; this state owns accepted numeric values and validation. Use this state's
/// `set_value` for external updates and subscribe to `InputNumberEvent`.
pub struct InputNumberState {
    pub(crate) editor: Entity<InputState>,
    model: NumberModel,
    draft_status: NumericDraft,
    ignore_through_revision: u64,
    repeat_task: Option<Task<()>>,
    repeat_generation: u64,
    _subscriptions: Vec<Subscription>,
}

impl EventEmitter<InputNumberEvent> for InputNumberState {}
impl Focusable for InputNumberState {
    fn focus_handle(&self, cx: &App) -> FocusHandle {
        self.editor.read(cx).focus_handle(cx)
    }
}

impl InputNumberState {
    pub fn new(window: &mut Window, cx: &mut Context<Self>) -> Self {
        Self::with_options(NumberOptions::default(), window, cx).expect("default numeric options")
    }
    pub fn with_options(
        options: NumberOptions,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Result<Self, NumberError> {
        let model = NumberModel::new(options)?;
        let editor = cx.new(|cx| InputState::new(window, cx));
        let input = cx.subscribe_in(
            &editor,
            window,
            |this, _, event: &InputEvent, window, cx| this.on_input_event(event, window, cx),
        );
        let observe = cx.observe_in(&editor, window, |this, _, _, cx| {
            if this.editor.read(cx).is_disabled() || this.editor.read(cx).is_read_only() {
                this.cancel_repeat();
            }
            cx.notify();
        });
        let active = cx.observe_window_activation(window, |this, window, _| {
            if !window.is_window_active() {
                this.cancel_repeat();
            }
        });
        let visible = cx.observe_window_visibility(window, |this, _, _, _| this.cancel_repeat());
        Ok(Self {
            editor,
            model,
            draft_status: NumericDraft::Empty,
            ignore_through_revision: 0,
            repeat_task: None,
            repeat_generation: 0,
            _subscriptions: vec![input, observe, active, visible],
        })
    }

    pub fn value(&self) -> Option<&NumericValue> {
        self.model.value.as_ref()
    }
    pub fn text<'a>(&self, cx: &'a App) -> &'a str {
        self.editor.read(cx).value()
    }
    pub fn draft_status(&self) -> &NumericDraft {
        &self.draft_status
    }
    pub fn options(&self) -> &NumberOptions {
        &self.model.options
    }
    pub fn selection(&self, cx: &App) -> TextSelection {
        self.editor.read(cx).selection()
    }
    pub fn is_composing(&self, cx: &App) -> bool {
        self.editor.read(cx).is_composing()
    }
    pub fn is_disabled(&self, cx: &App) -> bool {
        self.editor.read(cx).is_disabled()
    }
    pub fn is_read_only(&self, cx: &App) -> bool {
        self.editor.read(cx).is_read_only()
    }

    pub fn set_value(
        &mut self,
        value: Option<NumericValue>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Result<(), NumberError> {
        // Equal owner echoes preserve drafts such as "1." and active IME sessions.
        let normalized = value.as_ref().map(NumericValue::decimal).transpose()?;
        if normalized
            == self
                .model
                .value
                .as_ref()
                .map(NumericValue::decimal)
                .transpose()?
        {
            return Ok(());
        }
        let display = self.model.set_value(value)?;
        self.cancel_repeat();
        self.editor
            .update(cx, |editor, cx| editor.set_value(&display, window, cx));
        self.ignore_through_revision = self.editor.read(cx).buffer.revision();
        self.draft_status = self.model.options.parse(&display);
        cx.notify();
        Ok(())
    }

    pub fn set_options(
        &mut self,
        options: NumberOptions,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Result<(), NumberError> {
        let mut model = NumberModel::new(options)?;
        let display = model.set_value(self.model.value.clone())?;
        self.model = model;
        self.cancel_repeat();
        if !self.editor.read(cx).focus_handle(cx).is_focused(window) && !self.is_composing(cx) {
            self.editor
                .update(cx, |editor, cx| editor.set_value(&display, window, cx));
            self.ignore_through_revision = self.editor.read(cx).buffer.revision();
        }
        self.draft_status = self.model.options.parse(self.text(cx));
        cx.notify();
        Ok(())
    }

    pub fn set_disabled(&mut self, disabled: bool, window: &mut Window, cx: &mut Context<Self>) {
        self.cancel_repeat();
        self.editor
            .update(cx, |editor, cx| editor.set_disabled(disabled, window, cx));
    }
    pub fn set_read_only(&mut self, read_only: bool, window: &mut Window, cx: &mut Context<Self>) {
        self.cancel_repeat();
        self.editor
            .update(cx, |editor, cx| editor.set_read_only(read_only, window, cx));
    }
    pub fn focus(&self, window: &mut Window, cx: &mut App) {
        if !self.is_disabled(cx) {
            self.focus_handle(cx).focus(window, cx);
        }
    }
    pub fn blur(&self, window: &mut Window, cx: &mut App) {
        if self.focus_handle(cx).contains_focused(window, cx) {
            window.blur(cx);
        }
    }
    pub fn select(&self, selection: TextSelection, window: &mut Window, cx: &mut App) {
        self.editor
            .update(cx, |editor, cx| editor.select(selection, window, cx));
    }
    pub fn select_all(&self, window: &mut Window, cx: &mut App) {
        self.editor
            .update(cx, |editor, cx| editor.select_all(window, cx));
    }
    pub fn clear(&self, window: &mut Window, cx: &mut App) {
        self.editor
            .update(cx, |editor, cx| editor.clear(window, cx));
    }
    pub fn undo(&self, window: &mut Window, cx: &mut App) {
        self.editor.update(cx, |editor, cx| editor.undo(window, cx));
    }
    pub fn redo(&self, window: &mut Window, cx: &mut App) {
        self.editor.update(cx, |editor, cx| editor.redo(window, cx));
    }

    pub fn can_step(&self, direction: StepDirection, cx: &App) -> bool {
        !self.is_disabled(cx)
            && !self.is_read_only(cx)
            && !self.is_composing(cx)
            && self.model.can_step(direction == StepDirection::Up)
    }

    pub fn step(
        &mut self,
        direction: StepDirection,
        source: StepSource,
        accelerated: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if !self.can_step(direction, cx) {
            return;
        }
        match self.model.step(direction == StepDirection::Up, accelerated) {
            Ok(Some(update)) => {
                self.draft_status = update.draft;
                self.editor.update(cx, |editor, cx| {
                    editor.replace_range(
                        Some(0..editor.value().len()),
                        &update.display,
                        EditOrigin::Step,
                        window,
                        cx,
                    )
                });
                self.ignore_through_revision = self.editor.read(cx).buffer.revision();
                if update.changed {
                    self.emit_change(NumericChangeSource::Step(source), cx);
                }
                let offset = self
                    .model
                    .options
                    .step
                    .checked_multiply_integer(if accelerated { 10 } else { 1 })
                    .expect("step was checked before applying");
                cx.emit(InputNumberEvent::Step(NumericStep {
                    value: self.model.value.clone(),
                    offset,
                    direction,
                    source,
                }));
                if matches!(source, StepSource::Button | StepSource::Accessibility) {
                    self.focus(window, cx);
                }
                cx.notify();
            }
            Err(error) => cx.emit(InputNumberEvent::Error(error)),
            Ok(None) => {}
        }
    }

    pub fn commit(&mut self, reason: CommitReason, window: &mut Window, cx: &mut Context<Self>) {
        if self.is_disabled(cx) || self.is_composing(cx) {
            return;
        }
        if !self.is_read_only(cx)
            && (reason != CommitReason::Blur || self.model.options.change_on_blur)
        {
            let raw = self.text(cx).to_owned();
            match self.model.commit(&raw) {
                Ok(update) => {
                    self.apply_format(&raw, &update.display, true, window, cx);
                    self.draft_status = self.model.options.parse(&update.display);
                    if update.changed {
                        self.emit_change(NumericChangeSource::Commit(reason), cx);
                    }
                }
                Err(error) => {
                    cx.emit(InputNumberEvent::Error(error));
                    return;
                }
            }
        }
        cx.emit(InputNumberEvent::Commit(NumericCommit {
            value: self.model.value.clone(),
            reason,
        }));
        cx.notify();
    }

    pub(crate) fn start_repeat(
        &mut self,
        direction: StepDirection,
        accelerated: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.cancel_repeat();
        self.step(direction, StepSource::Button, accelerated, window, cx);
        if !self.can_step(direction, cx) {
            return;
        }
        let generation = self.repeat_generation;
        self.repeat_task = Some(cx.spawn_in(window, async move |state, cx| {
            cx.background_executor()
                .timer(Duration::from_millis(600))
                .await;
            loop {
                let repeat = state
                    .update_in(cx, |this, window, cx| {
                        if this.repeat_generation != generation
                            || !window.is_window_active()
                            || !this.can_step(direction, cx)
                        {
                            return false;
                        }
                        this.step(direction, StepSource::Button, accelerated, window, cx);
                        this.can_step(direction, cx)
                    })
                    .unwrap_or(false);
                if !repeat {
                    break;
                }
                cx.background_executor()
                    .timer(Duration::from_millis(200))
                    .await;
            }
            let _ = state.update(cx, |this, _| {
                if this.repeat_generation == generation {
                    this.repeat_task = None;
                }
            });
        }));
    }

    pub(crate) fn cancel_repeat(&mut self) {
        self.repeat_generation = self.repeat_generation.wrapping_add(1);
        self.repeat_task = None;
    }

    fn on_input_event(&mut self, event: &InputEvent, window: &mut Window, cx: &mut Context<Self>) {
        match event {
            InputEvent::Change(change) if change.revision > self.ignore_through_revision => {
                self.collect(
                    &change.value,
                    change.origin,
                    change.is_composing,
                    window,
                    cx,
                );
            }
            InputEvent::CompositionChanged(false) => {
                // Final native text may equal the final preedit and emit no Change.
                if self.draft_status == NumericDraft::Composing {
                    let raw = self.text(cx).to_owned();
                    self.collect(&raw, EditOrigin::Composition, false, window, cx);
                }
            }
            InputEvent::Commit(commit) => self.commit(commit.reason, window, cx),
            InputEvent::Focus => cx.emit(InputNumberEvent::Focus),
            InputEvent::Blur => {
                self.cancel_repeat();
                cx.emit(InputNumberEvent::Blur);
            }
            _ => {}
        }
    }

    fn collect(
        &mut self,
        raw: &str,
        origin: EditOrigin,
        composing: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        match self.model.input(raw, composing) {
            Ok(NumberUpdate {
                changed,
                display,
                draft,
            }) => {
                self.draft_status = draft.clone();
                // Earlier batched input events must not rewrite a later draft.
                if !composing
                    && self.text(cx) == raw
                    && !matches!(
                        origin,
                        EditOrigin::Undo | EditOrigin::Redo | EditOrigin::CompositionCancel
                    )
                {
                    self.apply_format(raw, &display, true, window, cx);
                }
                if changed {
                    self.emit_change(NumericChangeSource::Input(origin), cx);
                }
                cx.emit(InputNumberEvent::Input(NumericInput {
                    text: raw.to_owned().into(),
                    status: draft,
                    origin,
                }));
                cx.notify();
            }
            Err(error) => cx.emit(InputNumberEvent::Error(error)),
        }
    }

    fn apply_format(
        &mut self,
        old: &str,
        new: &str,
        amend: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if old == new {
            return;
        }
        let selection = self.editor.read(cx).selection();
        let selection = TextSelection {
            anchor: formatted_offset(old, new, selection.anchor),
            caret: formatted_offset(old, new, selection.caret),
        };
        self.editor.update(cx, |editor, cx| {
            editor.format_value(new, selection, amend, window, cx)
        });
    }

    fn emit_change(&self, source: NumericChangeSource, cx: &mut Context<Self>) {
        cx.emit(InputNumberEvent::Change(NumericChange {
            value: self.model.value.clone(),
            source,
        }));
    }
}

fn formatted_offset(old: &str, new: &str, offset: usize) -> usize {
    let offset = offset.min(old.len());
    let before = &old[..offset];
    let after = &old[offset..];
    if !after.is_empty() && new.ends_with(after) {
        return new.len() - after.len();
    }
    if after.is_empty() {
        return new.len();
    }
    if new.starts_with(before) {
        return before.len();
    }
    // Currency/grouping formatters insert presentation characters around digits.
    // Keep the same numeric position when neither unchanged side survives intact.
    let count = before.chars().filter(char::is_ascii_digit).count();
    if count == 0 {
        return new
            .char_indices()
            .find(|(_, c)| c.is_ascii_digit())
            .map_or(0, |(byte, _)| byte);
    }
    new.char_indices()
        .filter(|(_, c)| c.is_ascii_digit())
        .nth(count - 1)
        .map_or(new.len(), |(byte, c)| byte + c.len_utf8())
}
