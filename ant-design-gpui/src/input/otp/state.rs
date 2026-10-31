// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use gpui::{
    App, AppContext, ClipboardItem, Context, Entity, EventEmitter, FocusHandle, Focusable,
    SharedString, Subscription, Window,
};

use super::{OtpError, OtpOptions, model::OtpModel};
use crate::input::{CommitReason, EditOrigin, HistoryLimit, InputEvent, InputState, TextSelection};

#[derive(Clone, Debug)]
pub struct OtpChange {
    pub value: SharedString,
    /// Fixed-length cells preserve interior empty positions in partial codes.
    pub cells: Vec<SharedString>,
    pub complete: bool,
    pub origin: EditOrigin,
}
#[derive(Clone, Debug)]
pub struct OtpCommit {
    pub value: SharedString,
    pub cells: Vec<SharedString>,
    pub complete: bool,
    pub reason: CommitReason,
}
#[derive(Clone, Debug)]
pub enum OtpEvent {
    Change(OtpChange),
    Complete(SharedString),
    Commit(OtpCommit),
    Focus,
    Blur,
    ActiveChanged(usize),
    CompositionChanged { index: usize, active: bool },
}
pub(super) struct Cell {
    pub editor: Entity<InputState>,
    ignore_revision: u64,
    _events: Subscription,
}

/// Owns the aggregate code and history; each retained InputState supplies the
/// platform's normal IME, selection geometry and accessibility text bridge.
pub struct OtpState {
    model: OtpModel,
    pub(super) editors: Vec<Cell>,
    pub(super) requested_disabled: bool,
    disabled: bool,
    read_only: bool,
    focused: bool,
    composing: Option<usize>,
}
impl EventEmitter<OtpEvent> for OtpState {}
impl Focusable for OtpState {
    fn focus_handle(&self, cx: &App) -> FocusHandle {
        self.editors[self.active_index()]
            .editor
            .read(cx)
            .focus_handle(cx)
    }
}
impl OtpState {
    pub fn new(window: &mut Window, cx: &mut Context<Self>) -> Self {
        Self::with_options(OtpOptions::default(), window, cx)
            .expect("default verification code options")
    }
    pub fn with_options(
        options: OtpOptions,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Result<Self, OtpError> {
        let model = OtpModel::new(options)?;
        let mut this = Self {
            model,
            editors: Vec::new(),
            requested_disabled: false,
            disabled: false,
            read_only: false,
            focused: false,
            composing: None,
        };
        this.resize_editors(window, cx);
        this.sync_cells(false, window, cx);
        Ok(this)
    }
    pub fn value(&self) -> String {
        self.model.value()
    }
    pub fn cells(&self) -> &[SharedString] {
        &self.model.cells
    }
    pub fn is_complete(&self) -> bool {
        self.model.complete()
    }
    pub fn active_index(&self) -> usize {
        self.model.selection.active
    }
    pub fn options(&self) -> &OtpOptions {
        &self.model.options
    }
    pub fn is_disabled(&self) -> bool {
        self.disabled
    }
    pub fn is_read_only(&self) -> bool {
        self.read_only
    }
    pub fn is_composing(&self, cx: &App) -> bool {
        self.editors
            .iter()
            .any(|cell| cell.editor.read(cx).is_composing())
    }
    pub fn can_undo(&self) -> bool {
        !self.disabled && !self.read_only && self.model.can_undo()
    }
    pub fn can_redo(&self) -> bool {
        !self.disabled && !self.read_only && self.model.can_redo()
    }
    pub fn set_history_limit(&mut self, limit: HistoryLimit) {
        self.model.set_history_limit(limit);
    }

    pub fn set_value(
        &mut self,
        value: impl AsRef<str>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.model.set_value(value.as_ref()) {
            self.sync_cells(true, window, cx);
            if self.focused {
                self.focus_current(window, cx);
            }
            cx.notify();
        }
    }
    pub fn set_options(
        &mut self,
        options: OtpOptions,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Result<(), OtpError> {
        let resized = self.model.set_options(options)?;
        self.resize_editors(window, cx);
        self.sync_cells(resized, window, cx);
        if resized && self.focused {
            self.focus_current(window, cx);
        }
        cx.notify();
        Ok(())
    }
    pub fn set_disabled(&mut self, disabled: bool, window: &mut Window, cx: &mut Context<Self>) {
        self.requested_disabled = disabled;
        self.apply_disabled(disabled, window, cx);
    }
    pub(super) fn apply_disabled(
        &mut self,
        disabled: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled == disabled {
            return;
        }
        self.disabled = disabled;
        for cell in &self.editors {
            cell.editor
                .update(cx, |editor, cx| editor.apply_disabled(disabled, window, cx));
        }
        self.sync_tab_stops(cx);
        cx.notify();
    }
    pub fn set_read_only(&mut self, read_only: bool, window: &mut Window, cx: &mut Context<Self>) {
        if self.read_only == read_only {
            return;
        }
        self.read_only = read_only;
        for cell in &self.editors {
            cell.editor
                .update(cx, |editor, cx| editor.set_read_only(read_only, window, cx));
        }
        cx.notify();
    }
    pub fn focus(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.focus_cell(0, window, cx);
    }
    pub fn focus_cell(&mut self, index: usize, window: &mut Window, cx: &mut Context<Self>) {
        if self.disabled || self.is_composing(cx) {
            return;
        }
        let previous = self.active_index();
        self.model.select(index);
        self.focus_current(window, cx);
        if self.active_index() != previous {
            cx.emit(OtpEvent::ActiveChanged(self.active_index()));
        }
        cx.notify();
    }
    pub fn blur(&self, window: &mut Window, cx: &mut App) {
        if self
            .editors
            .iter()
            .any(|cell| cell.editor.read(cx).focus_handle(cx).is_focused(window))
        {
            window.blur(cx);
        }
    }
    pub fn select_all(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.disabled || self.is_composing(cx) {
            return;
        }
        self.model.selection.all = true;
        self.focus_current(window, cx);
        cx.notify();
    }
    pub fn copy(&self, cx: &mut App) {
        if self.disabled || self.options().mask.is_some() {
            return;
        }
        if self.model.selection.all {
            cx.write_to_clipboard(ClipboardItem::new_string(self.value()));
        } else {
            self.editors[self.active_index()]
                .editor
                .update(cx, |editor, cx| editor.copy(cx));
        }
    }
    pub fn cut(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if !self.editable(cx) || self.options().mask.is_some() {
            return;
        }
        self.copy(cx);
        if self.model.selection.all {
            self.clear_with_origin(EditOrigin::Cut, window, cx);
        } else {
            self.apply_input(self.active_index(), "", EditOrigin::Cut, true, window, cx);
        }
    }
    pub fn paste(&mut self, text: &str, window: &mut Window, cx: &mut Context<Self>) {
        if self.editable(cx) {
            self.apply_input(
                self.active_index(),
                text,
                EditOrigin::Paste,
                true,
                window,
                cx,
            );
        }
    }
    pub fn paste_clipboard(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if let Some(text) = cx.read_from_clipboard().and_then(|item| item.text()) {
            self.paste(&text, window, cx);
        }
    }
    pub fn clear(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.editable(cx) {
            self.clear_with_origin(EditOrigin::Clear, window, cx);
        }
    }
    pub fn undo(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.editable(cx) && self.model.undo() {
            let focus = self.has_focus(window, cx);
            self.sync_cells(true, window, cx);
            if focus {
                self.focus_current(window, cx);
            }
            self.emit_change(EditOrigin::Undo, cx);
        }
    }
    pub fn redo(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.editable(cx) && self.model.redo() {
            let focus = self.has_focus(window, cx);
            self.sync_cells(true, window, cx);
            if focus {
                self.focus_current(window, cx);
            }
            self.emit_change(EditOrigin::Redo, cx);
        }
    }
    pub fn commit(&self, reason: CommitReason, cx: &mut Context<Self>) {
        if self.disabled || self.is_composing(cx) {
            return;
        }
        cx.emit(OtpEvent::Commit(OtpCommit {
            value: self.value().into(),
            cells: self.model.cells.clone(),
            complete: self.is_complete(),
            reason,
        }));
    }
    pub(super) fn backspace(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if !self.editable(cx) {
            return;
        }
        if self.model.selection.all {
            self.clear_with_origin(EditOrigin::Delete, window, cx);
        } else if self.model.cells[self.active_index()].is_empty() {
            self.focus_cell(self.active_index().saturating_sub(1), window, cx);
        } else {
            self.apply_input(
                self.active_index(),
                "",
                EditOrigin::Delete,
                true,
                window,
                cx,
            );
        }
    }
    pub(super) fn delete(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if !self.editable(cx) {
            return;
        }
        if self.model.selection.all {
            self.clear_with_origin(EditOrigin::Delete, window, cx);
        } else {
            self.apply_input(
                self.active_index(),
                "",
                EditOrigin::Delete,
                true,
                window,
                cx,
            );
        }
    }
    fn editable(&self, cx: &App) -> bool {
        !self.disabled && !self.read_only && !self.is_composing(cx)
    }
    fn has_focus(&self, window: &Window, cx: &App) -> bool {
        self.editors
            .iter()
            .any(|cell| cell.editor.read(cx).focus_handle(cx).is_focused(window))
    }
    fn clear_with_origin(
        &mut self,
        origin: EditOrigin,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.model.clear() {
            let focus = self.has_focus(window, cx);
            self.sync_cells(true, window, cx);
            if focus {
                self.focus_current(window, cx);
            }
            self.emit_change(origin, cx);
        }
    }
    fn resize_editors(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.editors.truncate(self.model.options.length);
        while self.editors.len() < self.model.options.length {
            let index = self.editors.len();
            let editor = cx.new(|cx| {
                let mut editor = InputState::new(window, cx);
                editor.set_history_limit(HistoryLimit {
                    transactions: 0,
                    bytes: 0,
                });
                editor
            });
            let subscription = cx.subscribe_in(
                &editor,
                window,
                move |this, _, event: &InputEvent, window, cx| {
                    this.on_cell(index, event, window, cx)
                },
            );
            self.editors.push(Cell {
                editor,
                ignore_revision: 0,
                _events: subscription,
            });
        }
    }
    fn sync_tab_stops(&self, cx: &mut App) {
        for (index, cell) in self.editors.iter().enumerate() {
            cell.editor.update(cx, |editor, _| {
                editor.focus = editor
                    .focus
                    .clone()
                    .tab_stop(!self.disabled && index == self.active_index())
            });
        }
    }
    fn sync_cells(
        &mut self,
        cancel_composition: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if cancel_composition {
            self.composing = None;
        }
        for (index, cell) in self.editors.iter_mut().enumerate() {
            let value = &self.model.cells[index];
            cell.editor.update(cx, |editor, cx| {
                if cancel_composition && editor.is_composing() {
                    editor.cancel_composition(window, cx);
                }
                if !editor.is_composing() {
                    editor.set_value(value, window, cx);
                }
                editor.set_masked(self.model.options.mask.is_some(), window, cx);
                editor.set_read_only(self.read_only, window, cx);
                editor.apply_disabled(self.disabled, window, cx);
            });
            cell.ignore_revision = cell.editor.read(cx).buffer.revision();
        }
        self.sync_tab_stops(cx);
    }
    fn focus_current(&self, window: &mut Window, cx: &mut App) {
        if self.disabled {
            return;
        }
        self.focus_handle(cx).focus(window, cx);
        for (index, cell) in self.editors.iter().enumerate() {
            cell.editor.update(cx, |editor, cx| {
                if self.model.selection.all || index == self.active_index() {
                    editor.select_all(window, cx);
                } else {
                    editor.select(TextSelection::collapsed(editor.value().len()), window, cx);
                }
            });
        }
        self.sync_tab_stops(cx);
    }
    fn apply_input(
        &mut self,
        index: usize,
        text: &str,
        origin: EditOrigin,
        advance: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let previous = self.active_index();
        let focus = advance && self.has_focus(window, cx);
        let changed = self.model.input(index, text, advance);
        self.sync_cells(false, window, cx);
        if focus {
            self.focus_current(window, cx);
        }
        if changed {
            self.emit_change(origin, cx);
        }
        if previous != self.active_index() {
            cx.emit(OtpEvent::ActiveChanged(self.active_index()));
        }
        cx.notify();
    }
    fn emit_change(&self, origin: EditOrigin, cx: &mut Context<Self>) {
        let value: SharedString = self.value().into();
        cx.emit(OtpEvent::Change(OtpChange {
            value: value.clone(),
            cells: self.model.cells.clone(),
            complete: self.is_complete(),
            origin,
        }));
        if self.is_complete() {
            cx.emit(OtpEvent::Complete(value));
        }
        cx.notify();
    }
    fn on_cell(
        &mut self,
        index: usize,
        event: &InputEvent,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let Some(cell) = self.editors.get(index) else {
            return;
        };
        match event {
            InputEvent::Change(change)
                if change.revision > cell.ignore_revision
                    && change.revision == cell.editor.read(cx).buffer.revision() =>
            {
                if change.is_composing {
                    self.set_composing(index, true, cx);
                } else if change.origin == EditOrigin::CompositionCancel {
                    self.set_composing(index, false, cx);
                    self.sync_cells(false, window, cx);
                } else {
                    let advance =
                        cell.editor.read(cx).focus_handle(cx).is_focused(window) && !self.disabled;
                    self.set_composing(index, false, cx);
                    self.apply_input(index, &change.value, change.origin, advance, window, cx);
                }
            }
            InputEvent::CompositionChanged(active) => {
                if *active {
                    self.set_composing(index, true, cx);
                } else if self.composing == Some(index) {
                    let text = cell.editor.read(cx).value().to_owned();
                    let advance =
                        cell.editor.read(cx).focus_handle(cx).is_focused(window) && !self.disabled;
                    self.set_composing(index, false, cx);
                    self.apply_input(index, &text, EditOrigin::Composition, advance, window, cx);
                }
            }
            InputEvent::Focus => {
                if !cell.editor.read(cx).focus_handle(cx).is_focused(window) {
                    return;
                }
                let target = self.model.focus_index(index);
                if target != index {
                    self.focus_cell(target, window, cx);
                    return;
                }
                let previous = self.active_index();
                self.model.selection.active = index;
                self.focus_current(window, cx);
                if !self.focused {
                    self.focused = true;
                    cx.emit(OtpEvent::Focus);
                }
                if previous != index {
                    cx.emit(OtpEvent::ActiveChanged(index));
                }
                cx.notify();
            }
            InputEvent::Blur => {
                cx.defer_in(window, |this, window, cx| {
                    if this.focused
                        && !this
                            .editors
                            .iter()
                            .any(|cell| cell.editor.read(cx).focus_handle(cx).is_focused(window))
                    {
                        this.focused = false;
                        this.commit(CommitReason::Blur, cx);
                        cx.emit(OtpEvent::Blur);
                        cx.notify();
                    }
                });
            }
            InputEvent::Commit(commit) if commit.reason != CommitReason::Blur => {
                self.commit(commit.reason, cx)
            }
            _ => {}
        }
    }
    fn set_composing(&mut self, index: usize, active: bool, cx: &mut Context<Self>) {
        if active && self.composing != Some(index) {
            self.composing = Some(index);
            cx.emit(OtpEvent::CompositionChanged { index, active });
        } else if !active && self.composing == Some(index) {
            self.composing = None;
            cx.emit(OtpEvent::CompositionChanged { index, active });
        }
    }
}
