// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use ant_design_gpui::{
    InputState,
    input::{EditOrigin, TextSelection},
};
use gpui::{App, Entity, EntityInputHandler, Window};

/// Exercise the real platform font backend with several edits/commands in one
/// event batch, deliberately omitting redraws between them.
pub fn verify(states: &[Entity<InputState>], window: &mut Window, cx: &mut App) {
    states[0].update(cx, |state, cx| {
        state.set_value("אבג", window, cx);
        state.select(
            TextSelection {
                anchor: 0,
                caret: 0,
            },
            window,
            cx,
        );
        state.move_horizontal(true, false, false, window, cx);
        assert_eq!(
            state.selection().caret,
            2,
            "logical Right advances by one Hebrew grapheme"
        );
        state.move_horizontal(false, false, false, window, cx);
        assert_eq!(state.selection().caret, 0);
        state.set_value("abc אבג xyz", window, cx);
        state.select(
            TextSelection {
                anchor: 0,
                caret: 0,
            },
            window,
            cx,
        );
        for expected in 1..=11 {
            state.move_horizontal(true, false, false, window, cx);
            assert_eq!(
                state.value()[..state.selection().caret]
                    .encode_utf16()
                    .count(),
                expected
            );
        }
        state.select_all(window, cx);
        state.replace_selection(&"漢".repeat(200), EditOrigin::Typing, window, cx);
        let bounds = state
            .bounds_for_range(200..200, Default::default(), window, cx)
            .unwrap();
        let viewport = state.geometry().unwrap().viewport;
        assert!(
            bounds.left() >= viewport.left() && bounds.right() <= viewport.right() + gpui::px(1.)
        );
    });
    states[12].update(cx, |state, cx| {
        state.set_value("A".repeat(200), window, cx);
        state.select(
            TextSelection {
                anchor: 0,
                caret: 0,
            },
            window,
            cx,
        );
        state.move_boundary(true, false, false, window, cx);
        let row_end = state.selection().caret;
        assert!(row_end > 0 && row_end < 200);
        state.move_vertical(true, false, window, cx);
        assert!(state.selection().caret > row_end);
    });
    println!(
        "Native batched navigation passed: logical Hebrew/mixed-text movement, wrapped End/Down and scrolled IME range bounds."
    );
}
