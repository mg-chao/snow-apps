// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{cell::RefCell, ops::Range, rc::Rc, time::Duration};

use gpui::{
    App, Bounds, ClipboardItem, Context, EntityInputHandler, EventEmitter, FocusHandle, Focusable,
    Pixels, Point, SharedString, Subscription, TextInputAction, TextInputConfiguration,
    UTF16Selection, Window,
};

use super::{
    editing::{CountStrategy, EditOrigin, EditPolicy, EditingBuffer, HistoryLimit, TextSelection},
    geometry::{CaretAffinity, CaretLocation, InputGeometry},
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CommitReason {
    Enter,
    Blur,
    Programmatic,
}

#[derive(Clone, Debug)]
pub struct InputChange {
    pub value: SharedString,
    pub origin: EditOrigin,
    pub is_composing: bool,
    pub revision: u64,
}

#[derive(Clone, Debug)]
pub struct InputCommit {
    pub value: SharedString,
    pub reason: CommitReason,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SearchSource {
    Enter,
    Action,
    Clear,
    Programmatic,
}

#[derive(Clone, Debug)]
pub struct InputSearch {
    pub value: SharedString,
    pub source: SearchSource,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) enum InputMode {
    #[default]
    Text,
    Password,
    Search,
}

/// Subscribe once with `Context::subscribe` or `subscribe_in`. External `set_value`
/// updates notify entity observers, but do not emit a user change or commit.
#[derive(Clone, Debug)]
pub enum InputEvent {
    Change(InputChange),
    Commit(InputCommit),
    SelectionChanged(TextSelection),
    CompositionChanged(bool),
    Search(InputSearch),
    PasswordVisibilityChanged(bool),
    Focus,
    Blur,
}

/// The retained editing state shared by Input, TextArea, PasswordInput and SearchInput.
/// Create it with `cx.new(|cx| InputState::new(window, cx))` and retain the Entity.
pub struct InputState {
    pub(crate) buffer: EditingBuffer,
    pub(crate) policy: EditPolicy,
    pub(crate) focus: FocusHandle,
    pub(crate) disabled: bool,
    pub(crate) read_only: bool,
    pub(crate) masked: bool,
    pub(crate) requested_disabled: bool,
    pub(crate) availability_revision: u64,
    mode: InputMode,
    password_visible: bool,
    search_loading: bool,
    geometry: Option<Rc<InputGeometry>>,
    pub(super) navigation_layout: Option<super::layout::NavigationLayout>,
    preferred_x: Option<Pixels>,
    caret_affinity: CaretAffinity,
    anchor_affinity: CaretAffinity,
    value_snapshot: RefCell<Option<(u64, SharedString)>>,
    epoch: std::time::Instant,
    input_action: TextInputAction,
    _subscriptions: Vec<Subscription>,
}

impl EventEmitter<InputEvent> for InputState {}

impl Focusable for InputState {
    fn focus_handle(&self, _: &App) -> FocusHandle {
        self.focus.clone()
    }
}

impl InputState {
    pub fn new(window: &mut Window, cx: &mut Context<Self>) -> Self {
        super::actions::initialize(cx);
        let focus = crate::behavior::focus_handle(cx);
        let focused = cx.on_focus(&focus, window, |this, _, cx| {
            this.buffer.break_group();
            cx.emit(InputEvent::Focus);
            cx.notify();
        });
        let blurred = cx.on_blur(&focus, window, |this, window, cx| {
            this.finish_composition(window, cx);
            this.buffer.break_group();
            if !this.disabled {
                this.commit(CommitReason::Blur, window, cx);
            }
            cx.emit(InputEvent::Blur);
            cx.notify();
        });
        Self {
            buffer: EditingBuffer::default(),
            policy: EditPolicy::default(),
            focus,
            disabled: false,
            read_only: false,
            masked: false,
            requested_disabled: false,
            availability_revision: 0,
            mode: InputMode::Text,
            password_visible: false,
            search_loading: false,
            geometry: None,
            navigation_layout: None,
            preferred_x: None,
            caret_affinity: CaretAffinity::Downstream,
            anchor_affinity: CaretAffinity::Downstream,
            value_snapshot: RefCell::default(),
            epoch: cx.background_executor().now(),
            input_action: TextInputAction::Done,
            _subscriptions: vec![focused, blurred],
        }
    }

    pub fn value(&self) -> &str {
        self.buffer.text()
    }
    /// Immutable value shared by rendering, event payloads and consumers. A new
    /// allocation is made only after the text revision changes.
    pub fn value_snapshot(&self) -> SharedString {
        let mut snapshot = self.value_snapshot.borrow_mut();
        if let Some((revision, value)) = snapshot.as_ref()
            && *revision == self.buffer.revision()
        {
            return value.clone();
        }
        let value: SharedString = self.value().to_owned().into();
        *snapshot = Some((self.buffer.revision(), value.clone()));
        value
    }
    pub fn selection(&self) -> TextSelection {
        self.buffer.selection()
    }
    pub fn is_composing(&self) -> bool {
        self.buffer.is_composing()
    }
    pub fn can_undo(&self) -> bool {
        !self.disabled && !self.read_only && self.buffer.can_undo()
    }
    pub fn can_redo(&self) -> bool {
        !self.disabled && !self.read_only && self.buffer.can_redo()
    }
    pub fn is_disabled(&self) -> bool {
        self.disabled
    }
    pub fn is_read_only(&self) -> bool {
        self.read_only
    }
    pub fn is_multiline(&self) -> bool {
        self.policy.multiline
    }
    pub fn is_masked(&self) -> bool {
        self.masked
    }
    /// Password values remain protected from clipboard/accessibility export when revealed.
    pub fn is_protected(&self) -> bool {
        self.mode == InputMode::Password || self.masked
    }
    pub fn password_visible(&self) -> bool {
        self.password_visible
    }
    pub fn character_count(&self) -> usize {
        self.buffer.count(&self.policy.count)
    }
    pub fn max_length(&self) -> Option<usize> {
        self.policy.max_length
    }

    fn now(&self, cx: &App) -> Duration {
        cx.background_executor()
            .now()
            .saturating_duration_since(self.epoch)
    }

    pub fn set_value(
        &mut self,
        value: impl AsRef<str>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.buffer.set_text(value.as_ref(), &self.policy) {
            self.geometry = None;
            self.preferred_x = None;
            self.caret_affinity = CaretAffinity::Downstream;
            self.anchor_affinity = CaretAffinity::Downstream;
            window.notify_text_input_changed(&self.focus);
            cx.notify();
        }
    }

    /// Internal presentation rewrite that preserves the preceding edit as one
    /// undo transaction. Observers redraw, but no second user-change event fires.
    pub(crate) fn format_value(
        &mut self,
        value: &str,
        selection: TextSelection,
        amend: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self
            .buffer
            .rewrite_formatted(value, selection, self.now(cx), &self.policy, amend)
        {
            self.geometry = None;
            self.preferred_x = None;
            self.caret_affinity = CaretAffinity::Downstream;
            self.anchor_affinity = CaretAffinity::Downstream;
            window.notify_text_input_changed(&self.focus);
            window.notify_text_selection_changed(&self.focus);
            cx.notify();
        }
    }

    pub fn set_multiline(&mut self, multiline: bool, window: &mut Window, cx: &mut Context<Self>) {
        if self.policy.multiline == multiline {
            return;
        }
        self.finish_composition(window, cx);
        self.policy.multiline = multiline;
        self.input_action = if multiline {
            TextInputAction::Enter
        } else {
            TextInputAction::Done
        };
        let value = self.value().to_owned();
        self.set_value(value, window, cx);
        self.geometry = None;
        cx.notify();
    }

    pub fn set_max_length(&mut self, maximum: Option<usize>, cx: &mut Context<Self>) {
        self.policy.max_length = maximum;
        cx.notify();
    }

    pub fn set_count_strategy(&mut self, strategy: CountStrategy, cx: &mut Context<Self>) {
        self.policy.count = strategy;
        cx.notify();
    }

    pub fn set_history_limit(&mut self, limit: HistoryLimit) {
        self.buffer.set_history_limit(limit);
    }

    pub fn set_read_only(&mut self, read_only: bool, window: &mut Window, cx: &mut Context<Self>) {
        if self.read_only == read_only {
            return;
        }
        if read_only {
            self.finish_composition(window, cx);
        }
        self.read_only = read_only;
        self.availability_revision = self.availability_revision.wrapping_add(1);
        window.notify_text_input_changed(&self.focus);
        cx.notify();
    }

    pub fn set_disabled(&mut self, disabled: bool, window: &mut Window, cx: &mut Context<Self>) {
        self.requested_disabled = disabled;
        self.apply_disabled(disabled, window, cx);
    }

    pub(crate) fn apply_disabled(
        &mut self,
        disabled: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled == disabled {
            return;
        }
        if disabled {
            self.finish_composition(window, cx);
        }
        self.disabled = disabled;
        self.availability_revision = self.availability_revision.wrapping_add(1);
        self.focus = self.focus.clone().tab_stop(!disabled);
        if disabled && self.focus.contains_focused(window, cx) {
            window.blur(cx);
        }
        window.notify_text_input_changed(&self.focus);
        cx.notify();
    }

    /// Controls clipboard protection as well as rendering. Visibility toggles do
    /// not replace the source value or disturb its selection/history.
    pub fn set_masked(&mut self, masked: bool, window: &mut Window, cx: &mut Context<Self>) {
        if self.mode == InputMode::Password {
            self.password_visible = !masked;
        }
        if self.masked == masked {
            return;
        }
        self.masked = masked;
        self.geometry = None;
        window.notify_text_input_changed(&self.focus);
        cx.notify();
    }

    /// Set visibility without emitting a user action or disturbing the editing transaction.
    pub fn set_password_visible(
        &mut self,
        visible: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.password_visible == visible {
            return;
        }
        self.password_visible = visible;
        if self.mode == InputMode::Password {
            self.set_masked(!visible, window, cx);
        }
        cx.notify();
    }

    pub fn toggle_password_visibility(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.disabled || self.mode != InputMode::Password {
            return;
        }
        self.set_password_visible(!self.password_visible, window, cx);
        cx.emit(InputEvent::PasswordVisibilityChanged(self.password_visible));
    }

    pub(crate) fn configure_mode(
        &mut self,
        mode: InputMode,
        loading: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.mode != mode {
            let was_password = self.mode == InputMode::Password;
            self.mode = mode;
            if mode == InputMode::Password || was_password {
                self.set_masked(
                    mode == InputMode::Password && !self.password_visible,
                    window,
                    cx,
                );
            }
            self.input_action = if mode == InputMode::Search {
                TextInputAction::Search
            } else {
                TextInputAction::Done
            };
            cx.notify();
        }
        self.search_loading = loading;
    }

    pub fn search(&self, source: SearchSource, cx: &mut Context<Self>) {
        if self.disabled
            || self.search_loading
            || self.is_composing()
            || self.mode != InputMode::Search
        {
            return;
        }
        cx.emit(InputEvent::Search(InputSearch {
            value: self.value_snapshot(),
            source,
        }));
    }

    pub fn set_input_action(&mut self, action: TextInputAction, cx: &mut Context<Self>) {
        self.input_action = action;
        cx.notify();
    }

    pub fn focus(&self, window: &mut Window, cx: &mut App) {
        if !self.disabled {
            self.focus.focus(window, cx);
        }
    }

    pub fn blur(&self, window: &mut Window, cx: &mut App) {
        if self.focus.contains_focused(window, cx) {
            window.blur(cx);
        }
    }

    pub fn select(
        &mut self,
        selection: TextSelection,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled {
            return;
        }
        self.finish_composition(window, cx);
        self.buffer.select(selection);
        self.selection_changed(window, cx);
    }

    pub(super) fn select_with_affinities(
        &mut self,
        selection: TextSelection,
        anchor_affinity: CaretAffinity,
        caret_affinity: CaretAffinity,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled {
            return;
        }
        self.select(selection, window, cx);
        self.anchor_affinity = anchor_affinity;
        self.caret_affinity = caret_affinity;
    }

    fn motion_anchor_affinity(&self, extend: bool) -> CaretAffinity {
        if extend && !self.selection().is_empty() {
            self.anchor_affinity
        } else {
            self.caret_affinity
        }
    }

    fn move_caret(
        &mut self,
        caret: CaretLocation,
        extend: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let anchor_affinity = self.motion_anchor_affinity(extend);
        self.buffer.move_to(caret.offset, extend);
        self.selection_changed(window, cx);
        self.caret_affinity = caret.affinity;
        self.anchor_affinity = if extend {
            anchor_affinity
        } else {
            caret.affinity
        };
    }

    pub(super) fn anchor_location(&self) -> CaretLocation {
        CaretLocation {
            offset: self.selection().anchor,
            affinity: self.anchor_affinity,
        }
    }

    pub fn select_all(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.select(
            TextSelection {
                anchor: 0,
                caret: self.value().len(),
            },
            window,
            cx,
        );
    }

    pub fn move_horizontal(
        &mut self,
        forward: bool,
        extend: bool,
        word: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled || self.is_composing() {
            return;
        }
        // Follow logical grapheme/word boundaries, as GPUI Kit does. Horizontal
        // navigation must not depend on platform-specific bidi geometry or a draw.
        let anchor_affinity = self.motion_anchor_affinity(extend);
        self.buffer.move_horizontal(forward, extend, word);
        self.selection_changed(window, cx);
        self.caret_affinity = if forward {
            CaretAffinity::Upstream
        } else {
            CaretAffinity::Downstream
        };
        self.anchor_affinity = if extend {
            anchor_affinity
        } else {
            self.caret_affinity
        };
    }

    pub fn move_boundary(
        &mut self,
        end: bool,
        document: bool,
        extend: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled || self.is_composing() {
            return;
        }
        if !document {
            self.refresh_navigation_geometry(window);
        }
        if !document
            && let Some(caret) = self
                .geometry()
                .and_then(|geometry| geometry.row_boundary(self.caret_location(), end))
        {
            self.move_caret(caret, extend, window, cx);
            return;
        }
        let range = if document {
            0..self.value().len()
        } else {
            self.buffer.line_range(self.selection().caret)
        };
        self.move_caret(
            CaretLocation {
                offset: if end { range.end } else { range.start },
                affinity: CaretAffinity::Downstream,
            },
            extend,
            window,
            cx,
        );
    }

    pub fn move_vertical(
        &mut self,
        down: bool,
        extend: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled || self.is_composing() || !self.policy.multiline {
            return;
        }
        self.refresh_navigation_geometry(window);
        let Some(geometry) = self.geometry() else {
            return;
        };
        let Some(position) = geometry.caret_with_affinity(self.caret_location()) else {
            return;
        };
        let x = self.preferred_x.unwrap_or(position.x);
        let y = position.y + geometry.line_height * if down { 1. } else { -1. };
        if let Some(caret) = geometry.hit_test(gpui::point(x, y)) {
            self.move_caret(caret, extend, window, cx);
            self.preferred_x = Some(x);
        }
    }

    pub fn select_at(
        &mut self,
        position: Point<Pixels>,
        extend: bool,
        clicks: usize,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled {
            return;
        }
        let Some(geometry) = self.geometry() else {
            return;
        };
        let Some(caret) = geometry.hit_test(geometry.local_point(position)) else {
            return;
        };
        let offset = caret.offset;
        self.finish_composition(window, cx);
        match clicks {
            2 => self.buffer.select_word(offset),
            3.. => {
                let range = self.buffer.line_range(offset);
                self.buffer.select(TextSelection {
                    anchor: range.start,
                    caret: range.end,
                });
            }
            _ => {
                self.move_caret(caret, extend, window, cx);
                return;
            }
        }
        self.selection_changed(window, cx);
    }

    pub fn replace_selection(
        &mut self,
        text: &str,
        origin: EditOrigin,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.replace_range(None, text, origin, window, cx);
    }

    pub fn replace_range(
        &mut self,
        range: Option<Range<usize>>,
        text: &str,
        origin: EditOrigin,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled || self.read_only {
            return;
        }
        let composing = self.is_composing();
        let changed = self
            .buffer
            .replace(range, text, origin, self.now(cx), &self.policy);
        self.after_edit(changed, origin, window, cx);
        if composing {
            cx.emit(InputEvent::CompositionChanged(false));
        }
    }

    pub fn delete(
        &mut self,
        forward: bool,
        word: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled || self.read_only || self.is_composing() {
            return;
        }
        let changed = self
            .buffer
            .delete(forward, word, self.now(cx), &self.policy);
        self.after_edit(changed, EditOrigin::Delete, window, cx);
    }

    pub fn clear(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.replace_range(
            Some(0..self.value().len()),
            "",
            EditOrigin::Clear,
            window,
            cx,
        );
    }

    pub fn undo(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.disabled || self.read_only {
            return;
        }
        self.finish_composition(window, cx);
        let changed = self.buffer.undo(self.now(cx), &self.policy);
        self.after_edit(changed, EditOrigin::Undo, window, cx);
    }

    pub fn redo(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.disabled || self.read_only || self.is_composing() {
            return;
        }
        let changed = self.buffer.redo();
        self.after_edit(changed, EditOrigin::Redo, window, cx);
    }

    pub fn copy(&self, cx: &mut App) {
        if self.disabled || self.is_protected() || self.selection().is_empty() {
            return;
        }
        cx.write_to_clipboard(ClipboardItem::new_string(
            self.value()[self.selection().range()].to_owned(),
        ));
    }

    pub fn cut(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.disabled || self.read_only || self.is_protected() || self.is_composing() {
            return;
        }
        self.copy(cx);
        self.replace_selection("", EditOrigin::Cut, window, cx);
    }

    pub fn paste_clipboard(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if let Some(text) = cx.read_from_clipboard().and_then(|item| item.text()) {
            self.replace_selection(&text, EditOrigin::Paste, window, cx);
        }
    }

    pub fn commit(&mut self, reason: CommitReason, _window: &mut Window, cx: &mut Context<Self>) {
        if self.disabled || self.is_composing() {
            return;
        }
        self.buffer.break_group();
        cx.emit(InputEvent::Commit(InputCommit {
            value: self.value_snapshot(),
            reason,
        }));
        if reason == CommitReason::Enter {
            self.search(SearchSource::Enter, cx);
        }
    }

    pub fn finish_composition(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if !self.is_composing() {
            return;
        }
        let changed = self.buffer.finish_composition(self.now(cx), &self.policy);
        self.after_edit(changed, EditOrigin::Composition, window, cx);
        cx.emit(InputEvent::CompositionChanged(false));
    }

    pub fn cancel_composition(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if !self.is_composing() {
            return;
        }
        let changed = self.buffer.cancel_composition();
        self.after_edit(changed, EditOrigin::CompositionCancel, window, cx);
        cx.emit(InputEvent::CompositionChanged(false));
    }

    fn after_edit(
        &mut self,
        changed: bool,
        origin: EditOrigin,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.geometry = None;
        self.preferred_x = None;
        self.caret_affinity = CaretAffinity::Downstream;
        self.anchor_affinity = CaretAffinity::Downstream;
        window.notify_text_input_changed(&self.focus);
        if changed {
            cx.emit(InputEvent::Change(InputChange {
                value: self.value_snapshot(),
                origin,
                is_composing: self.is_composing(),
                revision: self.buffer.revision(),
            }));
            if origin == EditOrigin::Clear {
                self.search(SearchSource::Clear, cx);
            }
        }
        cx.notify();
    }

    fn selection_changed(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.preferred_x = None;
        self.caret_affinity = CaretAffinity::Downstream;
        self.anchor_affinity = CaretAffinity::Downstream;
        window.notify_text_selection_changed(&self.focus);
        cx.emit(InputEvent::SelectionChanged(self.selection()));
        cx.notify();
    }

    pub fn geometry(&self) -> Option<&InputGeometry> {
        self.geometry
            .as_deref()
            .filter(|geometry| geometry.revision == self.buffer.revision())
    }

    fn refresh_navigation_geometry(&mut self, window: &Window) {
        if self.geometry().is_none()
            && let Some(layout) = &self.navigation_layout
            && let Some(geometry) = layout.geometry(
                self.value(),
                self.buffer.revision(),
                self.masked,
                self.policy.multiline,
                self.caret_location(),
                window,
            )
        {
            self.geometry = Some(geometry);
        }
    }

    pub fn caret_location(&self) -> CaretLocation {
        CaretLocation {
            offset: self.selection().caret,
            affinity: self.caret_affinity,
        }
    }

    pub(crate) fn geometry_snapshot(&self) -> Option<Rc<InputGeometry>> {
        self.geometry
            .as_ref()
            .filter(|geometry| geometry.revision == self.buffer.revision())
            .cloned()
    }

    /// Publish geometry during prepaint; it never emits changes or requests a frame.
    pub fn set_geometry(&mut self, geometry: Rc<InputGeometry>) {
        self.geometry = Some(geometry);
    }
}

impl EntityInputHandler for InputState {
    fn text_for_range(
        &mut self,
        range: Range<usize>,
        adjusted: &mut Option<Range<usize>>,
        _: &mut Window,
        _: &mut Context<Self>,
    ) -> Option<String> {
        let range = self.buffer.byte_range(range);
        *adjusted = Some(self.buffer.utf16_range(range.clone()));
        Some(self.value()[range].to_owned())
    }

    fn selected_text_range(
        &mut self,
        ignore_disabled: bool,
        _: &mut Window,
        _: &mut Context<Self>,
    ) -> Option<UTF16Selection> {
        if self.disabled && !ignore_disabled {
            return None;
        }
        let selection = self.selection();
        Some(UTF16Selection {
            range: self.buffer.utf16_range(selection.range()),
            reversed: selection.caret < selection.anchor,
        })
    }

    fn marked_text_range(&self, _: &mut Window, _: &mut Context<Self>) -> Option<Range<usize>> {
        self.buffer
            .marked_range()
            .map(|range| self.buffer.utf16_range(range))
    }

    fn unmark_text(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.finish_composition(window, cx);
    }

    fn paste(&mut self, item: ClipboardItem, window: &mut Window, cx: &mut Context<Self>) {
        if let Some(text) = item.text() {
            self.replace_selection(&text, EditOrigin::Paste, window, cx);
        }
    }

    fn replace_text_in_range(
        &mut self,
        range: Option<Range<usize>>,
        text: &str,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let range = range.map(|range| self.buffer.byte_range(range));
        let origin = if self.is_composing() {
            EditOrigin::Composition
        } else {
            EditOrigin::Typing
        };
        self.replace_range(range, text, origin, window, cx);
    }

    fn replace_and_mark_text_in_range(
        &mut self,
        range: Option<Range<usize>>,
        text: &str,
        selection: Option<Range<usize>>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled || self.read_only {
            return;
        }
        // The pinned Windows bridge reports IME cancellation as an empty
        // GCS_COMPSTR with no range/selection, without forwarding END_COMPOSITION.
        // Keep that platform sentinel out of the general composition model: an
        // explicit empty preedit on other platforms can remain a live session.
        #[cfg(target_os = "windows")]
        if text.is_empty() && range.is_none() && selection.is_none() {
            self.cancel_composition(window, cx);
            return;
        }
        let starting = !self.is_composing();
        let changed = self.buffer.compose(range, text, selection, &self.policy);
        if starting {
            cx.emit(InputEvent::CompositionChanged(true));
        }
        self.after_edit(changed, EditOrigin::Composition, window, cx);
    }

    fn bounds_for_range(
        &mut self,
        range: Range<usize>,
        _: Bounds<Pixels>,
        window: &mut Window,
        _: &mut Context<Self>,
    ) -> Option<Bounds<Pixels>> {
        self.refresh_navigation_geometry(window);
        let range = self.buffer.byte_range(range);
        if range.is_empty() && range.start == self.selection().caret {
            self.geometry()?.caret_bounds(self.caret_location())
        } else {
            self.geometry()?.range_bounds(range)
        }
    }

    fn selection_bounds_for_range(
        &mut self,
        range: Range<usize>,
        _: Bounds<Pixels>,
        window: &mut Window,
        _: &mut Context<Self>,
    ) -> Vec<Bounds<Pixels>> {
        self.refresh_navigation_geometry(window);
        let range = self.buffer.byte_range(range);
        self.geometry().map_or_else(Vec::new, |geometry| {
            if range.is_empty() && range.start == self.selection().caret {
                geometry
                    .caret_bounds(self.caret_location())
                    .into_iter()
                    .collect()
            } else {
                geometry.selection_rects(range)
            }
        })
    }

    fn character_index_for_point(
        &mut self,
        point: Point<Pixels>,
        window: &mut Window,
        _: &mut Context<Self>,
    ) -> Option<usize> {
        self.refresh_navigation_geometry(window);
        let geometry = self.geometry()?;
        let offset = geometry.nearest(geometry.local_point(point))?;
        Some(self.buffer.byte_to_utf16(offset))
    }

    fn set_selected_text_range(
        &mut self,
        range: Range<usize>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let range = self.buffer.byte_range(range);
        self.select(
            TextSelection {
                anchor: range.start,
                caret: range.end,
            },
            window,
            cx,
        );
    }

    fn text_length_utf16(&mut self, _: &mut Window, _: &mut Context<Self>) -> Option<usize> {
        Some(self.buffer.utf16_len())
    }

    fn accepts_text_input(&self, _: &mut Window, _: &mut Context<Self>) -> bool {
        !self.disabled && !self.read_only
    }

    fn text_input_configuration(
        &mut self,
        _: &mut Window,
        _: &mut Context<Self>,
    ) -> TextInputConfiguration {
        TextInputConfiguration {
            input_action: self.input_action,
            ..Default::default()
        }
    }
}
