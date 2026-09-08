use super::*;
use snow_draw_engine_core::ColorRgba8;
use snow_draw_engine_core::arrow::{ArrowType, StrokeStyle};
use snow_draw_engine_document::TextLayoutSize;
use snow_draw_engine_document::{
    ArrowData, ElementData, ElementMeta, LinearElementKind, TextHorizontalAlign, Transaction,
    arrow_segment_midpoints, arrow_text_anchor, arrow_text_max_width,
};
use snow_draw_engine_editor::{ApplyTransactionCommand, TextCommitTarget, TextDraftCommit};

fn arrow(points: &[[f64; 2]], kind: ArrowType) -> ArrowData {
    ArrowData::from_global_points(
        &points
            .iter()
            .map(|p| Point::new(p[0], p[1]))
            .collect::<Vec<_>>(),
        ColorRgba8 {
            r: 32,
            g: 40,
            b: 50,
            a: 255,
        },
        2.0,
        StrokeStyle::Solid,
        kind,
        None,
        Some(snow_draw_engine_core::arrow::Arrowhead::Arrow),
    )
    .unwrap()
}

fn setup() -> (Engine, ViewportId, ElementId) {
    let mut engine = Engine::default();
    let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine
        .set_viewport_surface_size(viewport, 800, 600)
        .unwrap();
    let id = engine.model.peek_next_element_id();
    let mut tx = Transaction::new("create arrow");
    tx.insert_arrow(
        id,
        ElementMeta::default(),
        arrow(&[[-200.0, 0.0], [200.0, 0.0]], ArrowType::Straight),
    );
    engine
        .commit_transaction(
            viewport,
            ApplyTransactionCommand {
                transaction: tx,
                history_undo_snapshot: None,
            },
        )
        .unwrap();
    engine.editor.select_element(&engine.model, id).unwrap();
    (engine, viewport, id)
}

fn label(engine: &mut Engine, viewport: ViewportId, owner: ElementId, text: &str) -> ElementId {
    let style = engine.arrow_text_target(viewport, None).unwrap().unwrap().1;
    engine
        .commit_text_draft_with_viewport_changes(
            viewport,
            TextDraftCommit::new(
                TextCommitTarget::NewArrow(owner),
                Point::new(999.0, 999.0),
                text,
                TextLayoutSize {
                    width: 120.0,
                    height: 24.0,
                },
                style,
                true,
                false,
            ),
        )
        .unwrap();
    engine.model.bound_text_id_for_arrow(owner).unwrap()
}

#[test]
fn arrow_text_anchor_matches_middle_vertex_segment_and_vertical_minimum() {
    for kind in [ArrowType::Straight, ArrowType::Curve, ArrowType::Elbow] {
        let a = arrow(&[[0.0, 0.0], [0.0, 100.0]], kind);
        assert_eq!(arrow_text_anchor(&a), Point::new(0.0, 50.0));
        assert_eq!(arrow_text_max_width(&a, 20.0), 220.0);
        let a = arrow(&[[0.0, 0.0], [30.0, 150.0], [200.0, 0.0]], kind);
        assert_eq!(arrow_text_anchor(&a), Point::new(30.0, 150.0));
    }
    let a = arrow(
        &[[0.0, 0.0], [20.0, 0.0], [120.0, 0.0], [200.0, 0.0]],
        ArrowType::Straight,
    );
    assert_eq!(arrow_text_anchor(&a), Point::new(70.0, 0.0));
    let a = arrow(&[[0.0, 0.0], [0.0, 0.0]], ArrowType::Curve);
    assert_eq!(arrow_text_anchor(&a), Point::new(0.0, 0.0));
}

#[test]
fn arrow_text_curve_uses_arc_length_without_changing_handles() {
    let a = arrow(
        &[[-300.0, -400.0], [0.0, 0.0], [100.0, 0.0], [110.0, 40.0]],
        ArrowType::Curve,
    );
    let anchor = arrow_text_anchor(&a);
    let parameter_midpoint = arrow_segment_midpoints(&a)[1].1;
    assert!((anchor.x - parameter_midpoint.x).hypot(anchor.y - parameter_midpoint.y) > 0.5);
    assert!(anchor.x.is_finite() && anchor.y.is_finite());
}

#[test]
fn arrow_text_creation_move_delete_and_history_are_atomic() {
    let (mut engine, viewport, owner) = setup();
    let text_id = label(&mut engine, viewport, owner, "连接 → result\nsecond line");
    let text = engine.model.text(text_id).unwrap();
    assert_eq!(text.center, Point::new(0.0, 0.0));
    assert_eq!(text.horizontal_align, TextHorizontalAlign::Center);
    assert_eq!(engine.model.arrow_id_for_text(text_id), Some(owner));
    assert!(
        engine
            .model
            .bindable_element_states_with_overrides(&[])
            .iter()
            .all(|state| state.id() != text_id)
    );
    assert_eq!(engine.model.paint_order(), &[owner, text_id]);
    assert!(engine.undo().unwrap());
    assert!(engine.model.text(text_id).is_err());
    assert_eq!(engine.model.bound_text_id_for_arrow(owner), None);
    assert!(engine.redo().unwrap());
    let mut moved = engine.model.arrow(owner).unwrap().clone();
    moved.x += 75.0;
    moved.y += 45.0;
    moved.rotation = 0.75;
    let mut tx = Transaction::new("move arrow");
    tx.update_arrow(owner, moved);
    engine
        .commit_transaction(
            viewport,
            ApplyTransactionCommand {
                transaction: tx,
                history_undo_snapshot: None,
            },
        )
        .unwrap();
    assert_eq!(
        engine.model.text(text_id).unwrap().center,
        Point::new(75.0, 45.0)
    );
    assert_eq!(engine.model.text(text_id).unwrap().rotation, 0.0);
    engine.undo().unwrap();
    assert_eq!(
        engine.model.text(text_id).unwrap().center,
        Point::new(0.0, 0.0)
    );
    engine.editor.select_element(&engine.model, owner).unwrap();
    engine
        .delete_selected_with_viewport_changes(viewport)
        .unwrap();
    assert!(engine.model.element(owner).is_err() && engine.model.element(text_id).is_err());
    engine.undo().unwrap();
    assert_eq!(engine.model.bound_text_id_for_arrow(owner), Some(text_id));
}

#[test]
fn arrow_text_duplicate_remaps_label_and_session_round_trips_history() {
    let (mut engine, viewport, owner) = setup();
    let original = label(&mut engine, viewport, owner, "duplicate");
    engine.editor.select_element(&engine.model, owner).unwrap();
    engine
        .duplicate_selected_with_viewport_changes(viewport, Point::new(50.0, 30.0))
        .unwrap();
    let duplicate = engine.editor.selected_ids()[0];
    let copied = engine.model.bound_text_id_for_arrow(duplicate).unwrap();
    assert_ne!(original, copied);
    assert_eq!(engine.model.text(copied).unwrap().text, "duplicate");
    assert_eq!(
        engine.model.text(copied).unwrap().center,
        Point::new(50.0, 30.0)
    );
    let bytes = engine.serialize_document_session().unwrap();
    let mut restored =
        Engine::from_serialized_document_session_with_config(&bytes, EngineConfig::default())
            .unwrap();
    assert_eq!(
        restored.model.bound_text_id_for_arrow(duplicate),
        Some(copied)
    );
    restored.undo().unwrap();
    assert!(restored.model.element(duplicate).is_err());
    restored.redo().unwrap();
    assert_eq!(
        restored.model.bound_text_id_for_arrow(duplicate),
        Some(copied)
    );
}

#[test]
fn arrow_text_reordering_crosses_entire_pairs_in_both_directions() {
    let (mut engine, viewport, owner) = setup();
    let original = label(&mut engine, viewport, owner, "first");
    engine
        .duplicate_selected_with_viewport_changes(viewport, Point::new(0.0, 100.0))
        .unwrap();
    let duplicate = engine.editor.selected_ids()[0];
    let copied = engine.model.bound_text_id_for_arrow(duplicate).unwrap();
    for action in [1, 0] {
        engine
            .reorder_selected_with_viewport_changes(viewport, action)
            .unwrap();
        assert_eq!(
            engine.model.paint_order(),
            &[duplicate, copied, owner, original]
        );
        engine.undo().unwrap();
        assert_eq!(
            engine.model.paint_order(),
            &[owner, original, duplicate, copied]
        );
        engine.redo().unwrap();
        engine
            .reorder_selected_with_viewport_changes(viewport, if action == 1 { 2 } else { 3 })
            .unwrap();
        assert_eq!(
            engine.model.paint_order(),
            &[owner, original, duplicate, copied]
        );
    }
}

#[test]
fn arrow_text_invalid_ownership_rolls_back_the_entire_transaction() {
    let (mut engine, viewport, owner) = setup();
    let text = label(&mut engine, viewport, owner, "one owner");
    let before = engine.model.document().clone();
    let second_id = engine.model.peek_next_element_id();
    let mut other = arrow(&[[10.0, 50.0], [50.0, 50.0]], ArrowType::Straight);
    other.text_element_id = Some(text);
    let mut tx = Transaction::new("invalid shared label");
    tx.insert_arrow(second_id, ElementMeta::default(), other);
    assert_eq!(
        engine.model.apply_transaction(tx).unwrap_err(),
        ErrorCode::InvalidArgument
    );
    assert!(engine.model.document().has_same_session_content(&before));
    engine.model.validate_session().unwrap();
    let mut invalid = engine.model.arrow(owner).unwrap().clone();
    invalid.linear_kind = LinearElementKind::Line;
    let mut tx = Transaction::new("line cannot own text");
    tx.update_arrow(owner, invalid);
    assert!(engine.model.apply_transaction(tx).is_err());
    assert!(matches!(
        engine.model.element(text).unwrap().data,
        ElementData::Text(_)
    ));
}

#[test]
fn arrow_text_measurements_are_keyed_and_join_the_next_geometry_transaction() {
    let (mut engine, viewport, owner) = setup();
    let text_id = label(&mut engine, viewport, owner, "wrapping layout");
    let request = engine
        .arrow_text_layout_requests(viewport)
        .unwrap()
        .remove(0);
    engine
        .apply_arrow_text_measurements(
            viewport,
            &[(
                text_id,
                request.key + 1,
                TextLayoutSize {
                    width: 80.0,
                    height: 90.0,
                },
            )],
        )
        .unwrap();
    assert_eq!(
        engine.arrow_text_layout_requests(viewport).unwrap().len(),
        1
    );
    engine
        .apply_arrow_text_measurements(
            viewport,
            &[(
                text_id,
                request.key,
                TextLayoutSize {
                    width: 80.0,
                    height: 90.0,
                },
            )],
        )
        .unwrap();
    assert!(
        engine
            .arrow_text_layout_requests(viewport)
            .unwrap()
            .is_empty()
    );
    let mut moved = engine.model.arrow(owner).unwrap().clone();
    moved.y += 20.0;
    let mut tx = Transaction::new("move with measured label");
    tx.update_arrow(owner, moved);
    engine
        .commit_transaction(
            viewport,
            ApplyTransactionCommand {
                transaction: tx,
                history_undo_snapshot: None,
            },
        )
        .unwrap();
    assert_eq!(engine.model.text(text_id).unwrap().height, 90.0);
    engine.undo().unwrap();
    assert_eq!(engine.model.text(text_id).unwrap().height, 24.0);
}

#[test]
fn arrow_text_owner_metadata_and_invalid_commit_preserve_draft() {
    use snow_draw_engine_editor::{ActiveTextDraftPresentation, ActiveTextDraftTarget};
    let (mut engine, viewport, owner) = setup();
    let text_id = label(&mut engine, viewport, owner, "protected label");
    let draft = ActiveTextDraftPresentation {
        target: ActiveTextDraftTarget::Existing(text_id),
        revision: 1,
        text: engine.model.text(text_id).unwrap().clone(),
    };
    engine
        .set_active_text_draft_presentation_with_viewport_changes(viewport, draft.clone())
        .unwrap();
    let style = engine.arrow_text_target(viewport, None).unwrap().unwrap().1;
    let invalid = TextDraftCommit::new(
        TextCommitTarget::NewArrow(owner),
        Point::new(0.0, 0.0),
        "second label",
        TextLayoutSize {
            width: 100.0,
            height: 24.0,
        },
        style,
        true,
        false,
    );
    assert!(
        engine
            .commit_text_draft_with_viewport_changes(viewport, invalid)
            .is_err()
    );
    assert_eq!(engine.editor.active_text_draft_presentation(), Some(draft));
    engine
        .clear_active_text_draft_presentation_with_viewport_changes(viewport)
        .unwrap();
    for meta in [
        ElementMeta {
            locked: true,
            ..ElementMeta::default()
        },
        ElementMeta {
            visible: false,
            ..ElementMeta::default()
        },
    ] {
        let mut tx = Transaction::new("owner metadata");
        tx.update_element_meta(owner, meta);
        engine
            .commit_transaction(
                viewport,
                ApplyTransactionCommand {
                    transaction: tx,
                    history_undo_snapshot: None,
                },
            )
            .unwrap();
        assert_eq!(engine.model.element(text_id).unwrap().meta, meta);
        assert!(
            engine
                .arrow_text_target(viewport, Some(Point::new(0.0, 0.0)))
                .unwrap()
                .is_none()
        );
        engine.undo().unwrap();
        assert_eq!(
            engine.model.element(text_id).unwrap().meta,
            ElementMeta::default()
        );
    }
}

#[test]
fn arrow_text_version_one_sessions_without_ownership_remain_readable() {
    fn remove_ownership(value: &mut serde_json::Value) {
        match value {
            serde_json::Value::Object(object) => {
                object.remove("text_element_id");
                for value in object.values_mut() {
                    remove_ownership(value);
                }
            }
            serde_json::Value::Array(values) => {
                for value in values {
                    remove_ownership(value);
                }
            }
            _ => {}
        }
    }
    let (engine, _, owner) = setup();
    for history_only in [false, true] {
        let bytes = if history_only {
            engine.serialize_document_history()
        } else {
            engine.serialize_document_session()
        }
        .unwrap();
        let mut legacy: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
        legacy["schemaVersion"] = serde_json::json!(1);
        remove_ownership(&mut legacy);
        let bytes = serde_json::to_vec(&legacy).unwrap();
        let mut restored = if history_only {
            Engine::from_serialized_document_history_with_config(&bytes, EngineConfig::default())
        } else {
            Engine::from_serialized_document_session_with_config(&bytes, EngineConfig::default())
        }
        .unwrap();
        assert_eq!(restored.model.arrow(owner).unwrap().text_element_id, None);
        let viewport = restored.create_viewport(ViewportConfig::default()).unwrap();
        restored
            .set_viewport_surface_size(viewport, 800, 600)
            .unwrap();
        assert!(restored.undo().unwrap());
        assert!(restored.redo().unwrap());
        assert!(restored.model.arrow(owner).is_ok());
    }
}
