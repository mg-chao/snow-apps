// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{cell::RefCell, collections::HashMap, rc::Rc};

use gpui::{
    Bounds, Entity, Pixels, SharedString, StatefulInteractiveElement,
    accesskit::{self, Action, ActionData, Node, NodeId, Role},
    point, px, size,
};
use unicode_bidi::{BidiInfo, Level};
use unicode_segmentation::UnicodeSegmentation;

use super::{
    EditOrigin, InputState, TextSelection,
    geometry::{CaretAffinity, InputGeometry},
};

pub(super) type GeometryCell = Rc<RefCell<Option<Rc<InputGeometry>>>>;
pub(super) type SelectionMapCell = Rc<RefCell<SelectionMap>>;

#[derive(Default)]
pub(super) struct SelectionMap {
    revision: u64,
    runs: Vec<(NodeId, Vec<usize>)>,
}

impl SelectionMap {
    fn position(&self, offset: usize, affinity: CaretAffinity) -> Option<accesskit::TextPosition> {
        let position = |(id, offsets): &(NodeId, Vec<usize>)| {
            if offset < *offsets.first()? || offset > *offsets.last()? {
                return None;
            }
            Some(accesskit::TextPosition {
                node: *id,
                character_index: offsets
                    .partition_point(|byte| *byte <= offset)
                    .saturating_sub(1),
            })
        };
        match affinity {
            CaretAffinity::Upstream => self.runs.iter().find_map(position),
            CaretAffinity::Downstream => self.runs.iter().rev().find_map(position),
        }
    }

    pub(super) fn selection(
        &self,
        revision: u64,
        selection: &accesskit::TextSelection,
    ) -> Option<(TextSelection, CaretAffinity, CaretAffinity)> {
        if revision != self.revision {
            return None;
        }
        let byte = |position: &accesskit::TextPosition| {
            self.runs
                .iter()
                .find(|(id, _)| *id == position.node)?
                .1
                .get(position.character_index)
                .copied()
        };
        let anchor = byte(&selection.anchor)?;
        let caret = byte(&selection.focus)?;
        let affinity = |offset, position: &accesskit::TextPosition| {
            if self.position(offset, CaretAffinity::Downstream).as_ref() == Some(position) {
                CaretAffinity::Downstream
            } else {
                CaretAffinity::Upstream
            }
        };
        Some((
            TextSelection { anchor, caret },
            affinity(anchor, &selection.anchor),
            affinity(caret, &selection.focus),
        ))
    }
}

pub(super) struct TextSemantics {
    pub source: SharedString,
    pub selection: TextSelection,
    pub anchor_affinity: CaretAffinity,
    pub caret_affinity: CaretAffinity,
    pub revision: u64,
    pub protected: bool,
    pub mask_glyph: Option<SharedString>,
    pub disabled: bool,
    pub read_only: bool,
    pub scale: f32,
    pub geometry: GeometryCell,
}

pub(super) fn decorate<E: StatefulInteractiveElement>(
    element: E,
    state: &Entity<InputState>,
    frame: TextSemantics,
    mapping: SelectionMapCell,
) -> E {
    let disabled = frame.disabled;
    let writable = !disabled && !frame.read_only;
    let action_mapping = mapping.clone();
    let mut element = element.a11y_synthetic_children(move |builder| {
        // GPUI calls this only while accessibility is active, after viewport prepaint.
        let geometry = frame.geometry.borrow();
        let Some(geometry) = geometry.as_deref() else {
            return;
        };
        let runs = text_runs(
            &frame.source,
            frame.protected,
            geometry,
            frame.scale,
            frame.mask_glyph.as_deref(),
        );
        let mut map = SelectionMap {
            revision: frame.revision,
            runs: Vec::with_capacity(runs.len()),
        };
        for (index, run) in runs.into_iter().enumerate() {
            let id = builder.synthetic_node_id(("input-text", index));
            map.runs.push((id, run.offsets));
            builder.push_child(id, run.node);
        }
        if let (Some(anchor), Some(focus)) = (
            map.position(frame.selection.anchor, frame.anchor_affinity),
            map.position(frame.selection.caret, frame.caret_affinity),
        ) {
            builder
                .parent_node()
                .set_text_selection(accesskit::TextSelection { anchor, focus });
        }
        *mapping.borrow_mut() = map;
    });
    if !disabled {
        let state = state.clone();
        element = element.on_a11y_action(Action::SetTextSelection, move |data, window, cx| {
            let Some(ActionData::SetTextSelection(selection)) = data else {
                return;
            };
            state.update(cx, |state, cx| {
                if let Some((selection, anchor_affinity, caret_affinity)) = action_mapping
                    .borrow()
                    .selection(state.buffer.revision(), selection)
                {
                    state.focus(window, cx);
                    state.select_with_affinities(
                        selection,
                        anchor_affinity,
                        caret_affinity,
                        window,
                        cx,
                    );
                }
            });
        });
    }
    if writable {
        for action in [Action::SetValue, Action::ReplaceSelectedText] {
            let state = state.clone();
            element = element.on_a11y_action(action, move |data, window, cx| {
                let Some(ActionData::Value(value)) = data else {
                    return;
                };
                state.update(cx, |state, cx| {
                    let range = (action == Action::SetValue).then(|| 0..state.value().len());
                    state.replace_range(range, value, EditOrigin::Accessibility, window, cx);
                });
            });
        }
    }
    element
}

struct AccessibleRun {
    node: Node,
    offsets: Vec<usize>,
}

struct Unit<'a> {
    text: &'a str,
    start: usize,
    end: usize,
    bounds: Bounds<Pixels>,
    rtl: bool,
}

fn text_runs(
    source: &str,
    protected: bool,
    geometry: &InputGeometry,
    scale: f32,
    mask_glyph: Option<&str>,
) -> Vec<AccessibleRun> {
    let base_level = if geometry.right_to_left {
        Level::rtl()
    } else {
        Level::ltr()
    };
    let mask_rtl = mask_glyph.is_some_and(|mask| {
        BidiInfo::new(mask, Some(base_level))
            .levels
            .first()
            .is_some_and(|level| level.is_rtl())
    });
    let bidi = mask_glyph
        .is_none()
        .then(|| BidiInfo::new(source, Some(base_level)));
    let clusters: HashMap<_, _> = geometry
        .clusters
        .iter()
        .map(|cluster| (cluster.range.start, cluster.bounds))
        .collect();
    let mut runs = Vec::new();
    let mut units: Vec<Unit<'_>> = Vec::new();
    for (start, grapheme) in source.grapheme_indices(true) {
        let bounds = clusters.get(&start).copied().unwrap_or_else(|| {
            Bounds::new(
                geometry.caret(start).unwrap_or_default(),
                size(px(1.), geometry.line_height),
            )
        });
        let bounds = Bounds::new(geometry.window_point(bounds.origin), bounds.size);
        let rtl = bidi
            .as_ref()
            .map_or(mask_rtl, |bidi| bidi.levels[start].is_rtl());
        if let Some(last) = units.last()
            && (last.bounds.top() != bounds.top() || last.rtl != rtl || units.len() == 255)
        {
            runs.push(finish_run(&units, scale));
            units.clear();
        }
        if protected {
            units.push(Unit {
                text: "•",
                start,
                end: start + grapheme.len(),
                bounds,
                rtl,
            });
        } else {
            // AccessKit uses u8 UTF-8 lengths. Exceptional >255-byte graphemes are
            // exposed as scalar chunks; editing still snaps requested selections to
            // complete graphemes. No source bytes or combining marks are discarded.
            let mut offset = 0;
            while offset < grapheme.len() {
                let mut end = (offset + 255).min(grapheme.len());
                while !grapheme.is_char_boundary(end) {
                    end -= 1;
                }
                if units.len() == 255 {
                    runs.push(finish_run(&units, scale));
                    units.clear();
                }
                units.push(Unit {
                    text: &grapheme[offset..end],
                    start: start + offset,
                    end: start + end,
                    bounds,
                    rtl,
                });
                offset = end;
            }
        }
        if grapheme == "\n" {
            runs.push(finish_run(&units, scale));
            units.clear();
        }
    }
    if !units.is_empty() {
        runs.push(finish_run(&units, scale));
    }
    // The final empty line needs a selectable endpoint, including an empty input.
    if source.is_empty() || source.ends_with('\n') {
        let origin = geometry.window_point(geometry.caret(source.len()).unwrap_or_default());
        let mut node = Node::new(Role::TextRun);
        node.set_value("");
        node.set_character_lengths([]);
        node.set_bounds(rect(
            Bounds::new(origin, size(px(1.), geometry.line_height)),
            scale,
        ));
        runs.push(AccessibleRun {
            node,
            offsets: vec![source.len()],
        });
    }
    runs
}

fn finish_run(units: &[Unit<'_>], scale: f32) -> AccessibleRun {
    let first = &units[0];
    let mut value = String::new();
    let mut offsets = Vec::with_capacity(units.len() + 1);
    let mut lengths = Vec::with_capacity(units.len());
    let mut bounds = first.bounds;
    for unit in units {
        value.push_str(unit.text);
        lengths.push(unit.text.len() as u8);
        offsets.push(unit.start);
        bounds = Bounds::from_corners(
            point(
                bounds.left().min(unit.bounds.left()),
                bounds.top().min(unit.bounds.top()),
            ),
            point(
                bounds.right().max(unit.bounds.right()),
                bounds.bottom().max(unit.bounds.bottom()),
            ),
        );
    }
    offsets.push(units.last().unwrap().end);
    let mut node = Node::new(Role::TextRun);
    let word_byte_starts: Vec<_> = value.unicode_word_indices().map(|(byte, _)| byte).collect();
    let mut byte = 0;
    let mut words = Vec::new();
    for (index, length) in lengths.iter().enumerate() {
        if word_byte_starts.binary_search(&byte).is_ok() {
            words.push(index as u8);
        }
        byte += *length as usize;
    }
    node.set_value(value);
    node.set_character_lengths(lengths);
    node.set_word_starts(words);
    node.set_bounds(rect(bounds, scale));
    node.set_text_direction(if first.rtl {
        accesskit::TextDirection::RightToLeft
    } else {
        accesskit::TextDirection::LeftToRight
    });
    node.set_character_positions(
        units
            .iter()
            .map(|unit| {
                f32::from(if first.rtl {
                    bounds.right() - unit.bounds.right()
                } else {
                    unit.bounds.left() - bounds.left()
                }) * scale
            })
            .collect::<Vec<_>>(),
    );
    node.set_character_widths(
        units
            .iter()
            .map(|unit| f32::from(unit.bounds.size.width) * scale)
            .collect::<Vec<_>>(),
    );
    AccessibleRun { node, offsets }
}

fn rect(bounds: Bounds<Pixels>, scale: f32) -> accesskit::Rect {
    accesskit::Rect {
        x0: (f32::from(bounds.left()) * scale) as f64,
        y0: (f32::from(bounds.top()) * scale) as f64,
        x1: (f32::from(bounds.right()) * scale) as f64,
        y1: (f32::from(bounds.bottom()) * scale) as f64,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::input::geometry::{CaretStop, TextCluster};

    fn geometry(text: &str) -> InputGeometry {
        let mut geometry = InputGeometry {
            right_to_left: false,
            revision: 5,
            line_height: px(20.),
            viewport: Bounds::new(point(px(10.), px(20.)), size(px(100.), px(60.))),
            scroll: point(px(5.), px(0.)),
            carets: Vec::new(),
            clusters: Vec::new(),
        };
        let mut origin = point(px(0.), px(0.));
        for (start, grapheme) in text.grapheme_indices(true) {
            geometry.carets.push(CaretStop {
                offset: start,
                position: origin,
            });
            geometry.clusters.push(TextCluster {
                range: start..start + grapheme.len(),
                bounds: Bounds::new(origin, size(px(10.), px(20.))),
            });
            if grapheme == "\n" {
                origin = point(px(0.), origin.y + px(20.));
            } else {
                origin.x += px(10.);
            }
        }
        geometry.carets.push(CaretStop {
            offset: text.len(),
            position: origin,
        });
        geometry
    }

    #[test]
    fn screen_reader_runs_preserve_graphemes_lines_and_scaled_scrolled_geometry() {
        let text = "a👨‍👩‍👧‍👦中e\u{301}\n下一行\n";
        let runs = text_runs(text, false, &geometry(text), 1.5, None);
        assert_eq!(runs.len(), 3);
        assert_eq!(
            runs.iter()
                .map(|run| run.node.value().unwrap())
                .collect::<String>(),
            text
        );
        assert_eq!(runs[0].node.character_lengths(), &[1, 25, 3, 3, 1]);
        assert_eq!(runs[0].node.bounds().unwrap().x0, 7.5);
        assert_eq!(runs[0].node.character_widths().unwrap(), &[15.; 5]);
        assert_eq!(runs[2].offsets, vec![text.len()]);
    }

    #[test]
    fn protected_runs_never_export_source_and_keep_source_selection_mapping() {
        let text = "秘密😀e\u{301}";
        let runs = text_runs(text, true, &geometry(text), 1., Some("•"));
        assert_eq!(runs[0].node.value(), Some("••••"));
        assert_eq!(runs[0].node.character_lengths(), &[3; 4]);
        assert_eq!(runs[0].offsets, [0, 3, 6, 10, 13]);
    }

    #[test]
    fn accesskit_u8_limits_do_not_truncate_long_lines_or_combining_sequences() {
        let text = format!("{}a{}", "字".repeat(800), "\u{301}".repeat(200));
        let runs = text_runs(&text, false, &geometry(&text), 1., None);
        assert_eq!(
            runs.iter()
                .map(|run| run.node.value().unwrap())
                .collect::<String>(),
            text
        );
        for run in runs {
            assert!(run.node.character_lengths().len() <= 255);
            assert_eq!(
                run.node
                    .character_lengths()
                    .iter()
                    .map(|n| *n as usize)
                    .sum::<usize>(),
                run.node.value().unwrap().len()
            );
        }
    }

    #[test]
    fn selection_mapping_preserves_direction_and_rejects_stale_or_foreign_nodes() {
        let map = SelectionMap {
            revision: 7,
            runs: vec![(NodeId(1), vec![0, 3, 7]), (NodeId(2), vec![7, 9])],
        };
        let selection = accesskit::TextSelection {
            anchor: map.position(9, CaretAffinity::Downstream).unwrap(),
            focus: map.position(3, CaretAffinity::Downstream).unwrap(),
        };
        assert_eq!(
            map.selection(7, &selection),
            Some((
                TextSelection {
                    anchor: 9,
                    caret: 3
                },
                CaretAffinity::Downstream,
                CaretAffinity::Downstream
            ))
        );
        assert_eq!(
            map.position(7, CaretAffinity::Downstream).unwrap().node,
            NodeId(2)
        );
        assert_eq!(map.selection(8, &selection), None);
        let foreign = accesskit::TextSelection {
            anchor: accesskit::TextPosition {
                node: NodeId(3),
                character_index: 0,
            },
            ..selection
        };
        assert_eq!(map.selection(7, &foreign), None);
    }

    #[test]
    fn accessibility_selection_round_trips_both_sides_of_wraps_and_bidi_joins() {
        let map = SelectionMap {
            revision: 1,
            runs: vec![(NodeId(1), vec![0, 2, 4]), (NodeId(2), vec![4, 6, 8])],
        };
        for affinity in [CaretAffinity::Upstream, CaretAffinity::Downstream] {
            let position = map.position(4, affinity).unwrap();
            assert_eq!(
                position.node,
                NodeId(if affinity == CaretAffinity::Upstream {
                    1
                } else {
                    2
                })
            );
            assert_eq!(
                map.selection(
                    1,
                    &accesskit::TextSelection {
                        anchor: position,
                        focus: position
                    }
                ),
                Some((
                    TextSelection {
                        anchor: 4,
                        caret: 4
                    },
                    affinity,
                    affinity
                ))
            );
        }
    }

    #[test]
    fn explicit_rtl_direction_applies_to_neutral_and_protected_runs() {
        let text = "(  )";
        let mut geometry = geometry(text);
        geometry.right_to_left = true;
        for protected in [false, true] {
            let runs = text_runs(text, protected, &geometry, 1., None);
            assert_eq!(runs.len(), 1);
            assert_eq!(
                runs[0].node.text_direction(),
                Some(accesskit::TextDirection::RightToLeft)
            );
        }
    }

    #[test]
    fn protected_direction_tracks_the_displayed_mask_or_revealed_glyphs() {
        let text = "abc אבג";
        let geometry = geometry(text);
        let revealed = text_runs(text, true, &geometry, 1., None);
        assert_eq!(revealed.len(), 2);
        assert_eq!(
            revealed[1].node.text_direction(),
            Some(accesskit::TextDirection::RightToLeft)
        );
        assert_eq!(
            revealed
                .iter()
                .map(|r| r.node.value().unwrap())
                .collect::<String>(),
            "•••••••"
        );
        for (mask, direction) in [
            ("•", accesskit::TextDirection::LeftToRight),
            ("א", accesskit::TextDirection::RightToLeft),
        ] {
            let runs = text_runs(text, true, &geometry, 1., Some(mask));
            assert_eq!(runs.len(), 1);
            assert_eq!(runs[0].node.text_direction(), Some(direction));
            assert_eq!(runs[0].node.value(), Some("•••••••"));
        }
    }
}
