// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;

fn edit(buffer: &mut EditingBuffer, text: &str) {
    buffer.replace(
        None,
        text,
        EditOrigin::Typing,
        Duration::ZERO,
        &EditPolicy::default(),
    );
}

#[test]
fn formatter_rewrites_are_one_undo_transaction_and_preserve_following_offsets() {
    let policy = EditPolicy::default();
    let mut buffer = EditingBuffer::new("123");
    edit(&mut buffer, "4");
    buffer.rewrite_formatted(
        "1,234",
        TextSelection::collapsed(5),
        Duration::ZERO,
        &policy,
        true,
    );
    assert!(buffer.undo(Duration::ZERO, &policy));
    assert_eq!(buffer.text(), "123");
    assert!(buffer.redo());
    assert_eq!(buffer.text(), "1,234");
    buffer.break_group();
    buffer.move_to(3, false);
    edit(&mut buffer, "0");
    assert_eq!(buffer.text(), "1,2034");
    buffer.rewrite_formatted(
        "12,034",
        TextSelection::collapsed(4),
        Duration::ZERO,
        &policy,
        true,
    );
    assert!(buffer.undo(Duration::ZERO, &policy));
    assert_eq!(buffer.text(), "1,234");
    assert!(buffer.undo(Duration::ZERO, &policy));
    assert_eq!(buffer.text(), "123");
    assert!(buffer.redo());
    assert!(buffer.redo());
    assert_eq!(buffer.text(), "12,034");
}

#[test]
fn formatter_rewrite_rejects_preedit_and_normalization_can_cancel_a_noop_edit() {
    let policy = EditPolicy::default();
    let mut buffer = EditingBuffer::new("1");
    edit(&mut buffer, ".");
    buffer.rewrite_formatted(
        "1",
        TextSelection::collapsed(1),
        Duration::ZERO,
        &policy,
        true,
    );
    assert!(!buffer.can_undo());
    assert_eq!(buffer.history_bytes(), 0);
    buffer.compose(None, "ni", None, &policy);
    assert!(!buffer.rewrite_formatted(
        "unexpected",
        TextSelection::collapsed(0),
        Duration::ZERO,
        &policy,
        true
    ));
    assert_eq!(buffer.text(), "1ni");
    assert!(buffer.is_composing());
}

#[test]
fn grapheme_movement_and_deletion_preserve_combining_emoji_and_cjk() {
    let parts = ["A", "e\u{301}", "👩🏽‍💻", "🇨🇳", "中"];
    let text = parts.concat();
    let mut buffer = EditingBuffer::new(&text);
    assert_eq!(buffer.count(&CountStrategy::Graphemes), 5);
    let mut end = text.len();
    for part in parts.iter().rev() {
        end -= part.len();
        buffer.move_horizontal(false, false, false);
        assert_eq!(buffer.selection().caret, end);
    }
    buffer.move_to(text.len(), false);
    for part in parts.iter().rev() {
        let before = buffer.text().to_owned();
        assert!(buffer.delete(false, false, Duration::ZERO, &EditPolicy::default()));
        assert_eq!(buffer.text(), &before[..before.len() - part.len()]);
    }
    assert!(!buffer.delete(false, false, Duration::ZERO, &EditPolicy::default()));
    assert!(buffer.undo(Duration::ZERO, &EditPolicy::default()));
    assert_eq!(buffer.text(), text);
}

#[test]
fn native_utf16_ranges_round_surrogates_without_slicing_utf8() {
    let buffer = EditingBuffer::new("a😀中e\u{301}");
    assert_eq!(buffer.utf16_len(), 6);
    assert_eq!(buffer.byte_range(2..2), 1..5);
    assert_eq!(
        buffer.byte_range(7..999),
        buffer.text().len()..buffer.text().len()
    );
    for (byte, _) in buffer
        .text()
        .char_indices()
        .chain([(buffer.text().len(), '\0')])
    {
        let utf16 = buffer.byte_to_utf16(byte);
        assert_eq!(buffer.utf16_to_byte(utf16, false), byte);
        assert_eq!(buffer.utf16_to_byte(utf16, true), byte);
    }
}

#[test]
fn directional_selection_word_navigation_and_line_boundaries() {
    let mut buffer = EditingBuffer::new("hello world\n中文\n");
    buffer.move_to(0, false);
    buffer.move_horizontal(true, true, true);
    assert_eq!(&buffer.text()[buffer.selection().range()], "hello");
    buffer.move_horizontal(true, false, false);
    assert_eq!(buffer.selection(), TextSelection::collapsed(5));
    buffer.move_horizontal(true, false, true);
    assert_eq!(buffer.selection().caret, 11);
    buffer.select_word(7);
    assert_eq!(&buffer.text()[buffer.selection().range()], "world");
    assert_eq!(buffer.line_range(12), 12..18);
    assert_eq!(buffer.line_range(19), 19..19);
    buffer.select(TextSelection {
        anchor: 11,
        caret: 6,
    });
    assert_eq!(buffer.selection().range(), 6..11);
}

#[test]
fn typing_paste_and_selection_make_separate_undo_transactions() {
    let mut buffer = EditingBuffer::default();
    edit(&mut buffer, "h");
    edit(&mut buffer, "i");
    buffer.replace(
        None,
        "世界",
        EditOrigin::Paste,
        Duration::ZERO,
        &EditPolicy::default(),
    );
    assert_eq!(buffer.text(), "hi世界");
    buffer.undo(Duration::ZERO, &EditPolicy::default());
    assert_eq!(buffer.text(), "hi");
    buffer.undo(Duration::ZERO, &EditPolicy::default());
    assert_eq!(buffer.text(), "");
    buffer.redo();
    buffer.redo();
    assert_eq!(buffer.text(), "hi世界");
    buffer.select(TextSelection {
        anchor: 2,
        caret: 8,
    });
    edit(&mut buffer, "!");
    buffer.undo(Duration::ZERO, &EditPolicy::default());
    assert_eq!(buffer.text(), "hi世界");
    assert_eq!(
        buffer.selection(),
        TextSelection {
            anchor: 2,
            caret: 8
        }
    );
    edit(&mut buffer, "?");
    assert!(!buffer.can_redo());
}

#[test]
fn typing_time_boundary_and_navigation_break_coalescing() {
    let mut buffer = EditingBuffer::default();
    edit(&mut buffer, "a");
    buffer.replace(
        None,
        "b",
        EditOrigin::Typing,
        Duration::from_secs(2),
        &EditPolicy::default(),
    );
    buffer.undo(Duration::from_secs(3), &EditPolicy::default());
    assert_eq!(buffer.text(), "a");
    buffer.move_to(1, false);
    edit(&mut buffer, "c");
    buffer.undo(Duration::from_secs(3), &EditPolicy::default());
    assert_eq!(buffer.text(), "a");
}

#[test]
fn continued_typing_over_selection_is_one_undo_transaction() {
    let mut buffer = EditingBuffer::new("prefix 中文 e\u{301} 👩🏽‍💻 suffix");
    let original = buffer.text().to_owned();
    let selected = TextSelection {
        anchor: original.len() - 7,
        caret: 7,
    };
    buffer.select(selected);
    for character in ["1", "2", "3"] {
        edit(&mut buffer, character);
    }
    assert_eq!(buffer.text(), "prefix 123 suffix");
    assert!(buffer.undo(Duration::ZERO, &EditPolicy::default()));
    assert_eq!(buffer.text(), original);
    assert_eq!(buffer.selection(), selected);
    assert!(buffer.redo());
    assert_eq!(buffer.text(), "prefix 123 suffix");
}

#[test]
fn ime_preedit_is_unlimited_and_commit_is_one_limited_transaction() {
    let policy = EditPolicy {
        max_length: Some(2),
        ..Default::default()
    };
    let mut buffer = EditingBuffer::new("A");
    buffer.compose(None, "zhongwen", Some(8..8), &policy);
    assert_eq!(buffer.text(), "Azhongwen");
    assert_eq!(buffer.marked_range(), Some(1..9));
    assert!(!buffer.can_undo());
    buffer.compose(None, "中文", Some(2..2), &policy);
    buffer.finish_composition(Duration::ZERO, &policy);
    assert_eq!(buffer.text(), "A中");
    assert!(!buffer.is_composing());
    buffer.undo(Duration::ZERO, &policy);
    assert_eq!(buffer.text(), "A");
    buffer.redo();
    assert_eq!(buffer.text(), "A中");
}

#[test]
fn ime_selection_offsets_are_relative_utf16_and_cancellation_restores_selection() {
    let mut buffer = EditingBuffer::new("a😀z");
    let selection = TextSelection {
        anchor: 1,
        caret: 5,
    };
    buffer.select(selection);
    buffer.compose(None, "你好😀", Some(2..4), &EditPolicy::default());
    assert_eq!(buffer.text(), "a你好😀z");
    assert_eq!(&buffer.text()[buffer.selection().range()], "😀");
    assert!(buffer.cancel_composition());
    assert_eq!(buffer.text(), "a😀z");
    assert_eq!(buffer.selection(), selection);
    assert!(!buffer.can_undo());
}

#[test]
fn final_ime_replacement_and_empty_preedit_do_not_split_history() {
    let policy = EditPolicy::default();
    let mut buffer = EditingBuffer::new("x");
    buffer.compose(None, "ni", None, &policy);
    buffer.replace(None, "你", EditOrigin::Composition, Duration::ZERO, &policy);
    assert_eq!(buffer.text(), "x你");
    buffer.undo(Duration::ZERO, &policy);
    assert_eq!(buffer.text(), "x");
    buffer.compose(None, "a", None, &policy);
    buffer.compose(None, "", None, &policy);
    buffer.finish_composition(Duration::ZERO, &policy);
    assert_eq!(buffer.text(), "x");
    assert!(!buffer.can_undo());
}

#[test]
fn hard_limits_preserve_suffix_and_whole_graphemes() {
    let policy = EditPolicy {
        max_length: Some(3),
        ..Default::default()
    };
    let mut buffer = EditingBuffer::new("ab");
    buffer.move_to(1, false);
    buffer.replace(None, "👩🏽‍💻中文", EditOrigin::Paste, Duration::ZERO, &policy);
    assert_eq!(buffer.text(), "a👩🏽‍💻b");
    buffer.select(TextSelection {
        anchor: 0,
        caret: 1,
    });
    buffer.replace(None, "XYZ", EditOrigin::Paste, Duration::ZERO, &policy);
    assert_eq!(buffer.text(), "X👩🏽‍💻b");
    let mut buffer = EditingBuffer::new("e");
    let policy = EditPolicy {
        max_length: Some(1),
        ..Default::default()
    };
    buffer.replace(None, "\u{301}", EditOrigin::Typing, Duration::ZERO, &policy);
    assert_eq!(buffer.text(), "e\u{301}");
}

#[test]
fn utf16_limit_never_cuts_emoji_and_external_values_may_exceed_limit() {
    let policy = EditPolicy {
        max_length: Some(2),
        count: CountStrategy::Utf16,
        ..Default::default()
    };
    let mut buffer = EditingBuffer::default();
    buffer.replace(None, "😀a", EditOrigin::Paste, Duration::ZERO, &policy);
    assert_eq!(buffer.text(), "😀");
    buffer.set_text("long", &policy);
    buffer.move_to(4, false);
    buffer.replace(None, "x", EditOrigin::Typing, Duration::ZERO, &policy);
    assert_eq!(buffer.text(), "long");
    buffer.delete(false, false, Duration::ZERO, &policy);
    assert_eq!(buffer.text(), "lon");
}

#[test]
fn equal_external_value_preserves_composition_and_replacement_resets_history() {
    let policy = EditPolicy::default();
    let mut buffer = EditingBuffer::default();
    edit(&mut buffer, "x");
    buffer.compose(None, "ni", None, &policy);
    assert!(!buffer.set_text("xni", &policy));
    assert!(buffer.is_composing());
    assert!(buffer.can_undo());
    assert!(buffer.set_text("你好", &policy));
    assert!(!buffer.is_composing());
    assert!(!buffer.can_undo());
    assert!(!buffer.can_redo());
}

#[test]
fn history_is_bounded_by_bytes_and_transaction_count() {
    let mut buffer = EditingBuffer::default();
    buffer.set_history_limit(HistoryLimit {
        transactions: 3,
        bytes: 5,
    });
    for value in ["ab", "cd", "ef", "gh"] {
        buffer.replace(
            None,
            value,
            EditOrigin::Paste,
            Duration::ZERO,
            &EditPolicy::default(),
        );
    }
    assert_eq!(buffer.history_bytes(), 4);
    buffer.undo(Duration::ZERO, &EditPolicy::default());
    buffer.undo(Duration::ZERO, &EditPolicy::default());
    assert_eq!(buffer.text(), "abcd");
    assert!(!buffer.undo(Duration::ZERO, &EditPolicy::default()));
    buffer.set_history_limit(HistoryLimit {
        transactions: 0,
        bytes: 0,
    });
    assert!(!buffer.can_redo());
    assert_eq!(buffer.history_bytes(), 0);
}

#[test]
fn line_endings_are_normalized_without_a_canvas_text_size_limit() {
    let mut buffer = EditingBuffer::default();
    let text = "中\r\n文\r".repeat(20_000);
    buffer.replace(
        None,
        &text,
        EditOrigin::Paste,
        Duration::ZERO,
        &EditPolicy::default(),
    );
    assert_eq!(buffer.text(), "中文".repeat(20_000));
    let policy = EditPolicy {
        multiline: true,
        ..Default::default()
    };
    buffer.set_text("a\r\nb\rc\n", &policy);
    assert_eq!(buffer.text(), "a\nb\nc\n");
}

#[test]
fn deterministic_edit_undo_roundtrips_cover_mixed_unicode_and_selections() {
    let policy = EditPolicy {
        multiline: true,
        ..Default::default()
    };
    let samples = ["a", "👩🏽‍💻", "中", "e\u{301}", "\n", "אב", ""];
    let mut buffer = EditingBuffer::new("start");
    let mut seed = 42u64;
    for step in 0..250 {
        seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1);
        let len = buffer.grapheme_boundaries().len();
        let start = buffer.grapheme_boundaries()[seed as usize % len];
        let end = buffer.grapheme_boundaries()[(seed >> 32) as usize % len];
        buffer.select(TextSelection {
            anchor: start,
            caret: end,
        });
        let before = buffer.text().to_owned();
        let selection = buffer.selection();
        let changed = buffer.replace(
            None,
            samples[step % samples.len()],
            EditOrigin::Paste,
            Duration::from_secs(step as u64),
            &policy,
        );
        let after = buffer.text().to_owned();
        if changed {
            assert!(buffer.undo(Duration::from_secs(step as u64), &policy));
            assert_eq!(buffer.text(), before);
            assert_eq!(buffer.selection(), selection);
            assert!(buffer.redo());
            assert_eq!(buffer.text(), after);
        }
    }
}
