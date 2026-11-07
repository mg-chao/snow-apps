// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Context-scoped defaults. Applications can bind these actions to their own keys.
//! The library never installs editor shortcuts in the application's global context.

use gpui::{App, Context, Entity, Global, InteractiveElement, KeyBinding, Window};

use super::{CommitReason, EditOrigin, InputState};

gpui::actions!(
    ant_design_input,
    [
        SelectAll,
        Copy,
        Cut,
        Paste,
        Undo,
        Redo,
        Left,
        Right,
        SelectLeft,
        SelectRight,
        WordLeft,
        WordRight,
        SelectWordLeft,
        SelectWordRight,
        Up,
        Down,
        SelectUp,
        SelectDown,
        Home,
        End,
        SelectHome,
        SelectEnd,
        DocumentStart,
        DocumentEnd,
        SelectDocumentStart,
        SelectDocumentEnd,
        Backspace,
        Delete,
        DeleteWordLeft,
        DeleteWordRight,
        Enter,
        Submit,
    ]
);

struct Initialized;
impl Global for Initialized {}

pub(crate) fn initialize(cx: &mut App) {
    if cx.has_global::<Initialized>() {
        return;
    }
    cx.set_global(Initialized);
    cx.bind_keys([
        KeyBinding::new("enter", gpui::NoAction, Some("AntDesignInputAction")),
        KeyBinding::new("space", gpui::NoAction, Some("AntDesignInputAction")),
    ]);
    let context = Some("AntDesignInput");
    cx.bind_keys([
        KeyBinding::new("left", Left, context),
        KeyBinding::new("right", Right, context),
        KeyBinding::new("shift-left", SelectLeft, context),
        KeyBinding::new("shift-right", SelectRight, context),
        KeyBinding::new("up", Up, context),
        KeyBinding::new("down", Down, context),
        KeyBinding::new("shift-up", SelectUp, context),
        KeyBinding::new("shift-down", SelectDown, context),
        KeyBinding::new("home", Home, context),
        KeyBinding::new("end", End, context),
        KeyBinding::new("shift-home", SelectHome, context),
        KeyBinding::new("shift-end", SelectEnd, context),
        KeyBinding::new("backspace", Backspace, context),
        KeyBinding::new("delete", Delete, context),
        KeyBinding::new("enter", Enter, context),
    ]);
    macro_rules! primary {
        ($key:literal) => {
            if cfg!(target_os = "macos") {
                concat!("cmd-", $key)
            } else {
                concat!("ctrl-", $key)
            }
        };
    }
    macro_rules! word {
        ($key:literal) => {
            if cfg!(target_os = "macos") {
                concat!("alt-", $key)
            } else {
                concat!("ctrl-", $key)
            }
        };
    }
    cx.bind_keys([
        KeyBinding::new(primary!("a"), SelectAll, context),
        KeyBinding::new(primary!("c"), Copy, context),
        KeyBinding::new(primary!("x"), Cut, context),
        KeyBinding::new(primary!("v"), Paste, context),
        KeyBinding::new(primary!("z"), Undo, context),
        KeyBinding::new(primary!("shift-z"), Redo, context),
        KeyBinding::new(primary!("y"), Redo, context),
        KeyBinding::new(word!("left"), WordLeft, context),
        KeyBinding::new(word!("right"), WordRight, context),
        KeyBinding::new(word!("shift-left"), SelectWordLeft, context),
        KeyBinding::new(word!("shift-right"), SelectWordRight, context),
        KeyBinding::new(word!("backspace"), DeleteWordLeft, context),
        KeyBinding::new(word!("delete"), DeleteWordRight, context),
        KeyBinding::new(primary!("home"), DocumentStart, context),
        KeyBinding::new(primary!("end"), DocumentEnd, context),
        KeyBinding::new(primary!("shift-home"), SelectDocumentStart, context),
        KeyBinding::new(primary!("shift-end"), SelectDocumentEnd, context),
        KeyBinding::new(primary!("enter"), Submit, context),
    ]);
    if cfg!(target_os = "macos") {
        cx.bind_keys([
            KeyBinding::new("cmd-left", Home, context),
            KeyBinding::new("cmd-right", End, context),
            KeyBinding::new("cmd-shift-left", SelectHome, context),
            KeyBinding::new("cmd-shift-right", SelectEnd, context),
            KeyBinding::new("cmd-up", DocumentStart, context),
            KeyBinding::new("cmd-down", DocumentEnd, context),
            KeyBinding::new("cmd-shift-up", SelectDocumentStart, context),
            KeyBinding::new("cmd-shift-down", SelectDocumentEnd, context),
        ]);
    }
}

fn run(
    state: &Entity<InputState>,
    window: &mut Window,
    cx: &mut App,
    edit: impl FnOnce(&mut InputState, &mut Window, &mut Context<InputState>),
) {
    state.update(cx, |state, cx| {
        if state.disabled || !state.focus.is_focused(window) {
            cx.propagate();
            return;
        }
        // The platform IME owns keys while preedit is active. Never let a confirmation
        // key fall through to a form's submit or search action during composition.
        if !state.is_composing() {
            edit(state, window, cx);
        }
        window.prevent_default();
    });
}

pub(super) fn bind<E: InteractiveElement>(
    mut element: E,
    state: &Entity<InputState>,
    focused: bool,
) -> E {
    // Adornments can contain their own focusable controls. Their keyboard events
    // must never run editor commands merely because they share the input's frame.
    if focused {
        element = element.key_context("AntDesignInput");
    }
    macro_rules! handler {
        ($action:ty, |$state:ident, $window:ident, $cx:ident| $body:expr) => {
            let target = state.clone();
            element = element.on_action(move |_: &$action, window, cx| {
                run(&target, window, cx, |$state, $window, $cx| $body)
            });
        };
    }
    handler!(SelectAll, |s, w, c| s.select_all(w, c));
    handler!(Copy, |s, _w, c| s.copy(c));
    handler!(Cut, |s, w, c| s.cut(w, c));
    handler!(Paste, |s, w, c| s.paste_clipboard(w, c));
    handler!(Undo, |s, w, c| s.undo(w, c));
    handler!(Redo, |s, w, c| s.redo(w, c));
    handler!(Left, |s, w, c| s.move_horizontal(false, false, false, w, c));
    handler!(Right, |s, w, c| s.move_horizontal(true, false, false, w, c));
    handler!(SelectLeft, |s, w, c| s
        .move_horizontal(false, true, false, w, c));
    handler!(SelectRight, |s, w, c| s
        .move_horizontal(true, true, false, w, c));
    handler!(WordLeft, |s, w, c| s
        .move_horizontal(false, false, true, w, c));
    handler!(WordRight, |s, w, c| s
        .move_horizontal(true, false, true, w, c));
    handler!(SelectWordLeft, |s, w, c| s
        .move_horizontal(false, true, true, w, c));
    handler!(SelectWordRight, |s, w, c| s
        .move_horizontal(true, true, true, w, c));
    handler!(Up, |s, w, c| vertical(s, false, false, w, c));
    handler!(Down, |s, w, c| vertical(s, true, false, w, c));
    handler!(SelectUp, |s, w, c| vertical(s, false, true, w, c));
    handler!(SelectDown, |s, w, c| vertical(s, true, true, w, c));
    handler!(Home, |s, w, c| s.move_boundary(false, false, false, w, c));
    handler!(End, |s, w, c| s.move_boundary(true, false, false, w, c));
    handler!(SelectHome, |s, w, c| s
        .move_boundary(false, false, true, w, c));
    handler!(SelectEnd, |s, w, c| s
        .move_boundary(true, false, true, w, c));
    handler!(DocumentStart, |s, w, c| s
        .move_boundary(false, true, false, w, c));
    handler!(DocumentEnd, |s, w, c| s
        .move_boundary(true, true, false, w, c));
    handler!(SelectDocumentStart, |s, w, c| s
        .move_boundary(false, true, true, w, c));
    handler!(SelectDocumentEnd, |s, w, c| s
        .move_boundary(true, true, true, w, c));
    handler!(Backspace, |s, w, c| s.delete(false, false, w, c));
    handler!(Delete, |s, w, c| s.delete(true, false, w, c));
    handler!(DeleteWordLeft, |s, w, c| s.delete(false, true, w, c));
    handler!(DeleteWordRight, |s, w, c| s.delete(true, true, w, c));
    handler!(Enter, |s, w, c| if s.is_multiline() {
        s.replace_selection("\n", EditOrigin::Typing, w, c);
    } else {
        s.commit(CommitReason::Enter, w, c);
    });
    handler!(Submit, |s, w, c| s.commit(CommitReason::Enter, w, c));
    element
}

fn vertical(
    state: &mut InputState,
    down: bool,
    extend: bool,
    window: &mut Window,
    cx: &mut Context<InputState>,
) {
    if state.is_multiline() {
        state.move_vertical(down, extend, window, cx);
    } else {
        cx.propagate();
    }
}
