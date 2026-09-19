use super::*;
use snow_draw_engine_core::{Camera, SurfaceSize};
use snow_draw_engine_document::{CanvasFilterType, ElementMeta, FilterData, Transaction};

fn id(index: u32) -> ElementId {
    ElementId {
        index,
        generation: 1,
    }
}
fn filter(kind: CanvasFilterType, x: f64) -> FilterData {
    FilterData {
        center: Point::new(x, 0.0),
        width: 40.0,
        height: 40.0,
        filter_type: kind,
        ..Default::default()
    }
}
fn rectangle(x: f64) -> RectangleData {
    RectangleData {
        rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
        highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
        center: Point::new(x, 0.0),
        width: 8.0,
        height: 8.0,
        rotation: 0.0,
        fill: ColorRgba8::default(),
        fill_style: snow_draw_engine_document::FillStyle::Solid,
        stroke: ColorRgba8::default(),
        stroke_width: 0.0,
        stroke_style: StrokeStyle::Solid,
        corner_radii: CornerRadii::default(),
        opacity: 1.0,
    }
}
fn frame(width: u32) -> FrameView {
    FrameView {
        surface: SurfaceSize { width, height: 64 },
        camera: Camera {
            center: Point::default(),
            zoom: 1.0,
        },
        clear_color: ColorRgba8::default(),
    }
}
fn refresh(
    composer: &mut ViewportComposer,
    cache: &DocumentSceneCache,
    model: &DocumentModel,
    width: u32,
) {
    composer.refresh_with_presentation(
        cache,
        model,
        frame(width),
        &EditorPresentationState::default(),
        SnapConfig::default(),
    );
}
fn fixture(separator_is_filter: bool) -> (DocumentModel, DocumentSceneCache) {
    let mut model = DocumentModel::new();
    let mut tx = Transaction::new("source boundaries");
    tx.insert_filter(
        id(0),
        ElementMeta::default(),
        filter(CanvasFilterType::Inversion, 0.0),
    );
    if separator_is_filter {
        tx.insert_filter(
            id(1),
            ElementMeta::default(),
            filter(CanvasFilterType::Grayscale, 100.0),
        );
    } else {
        tx.insert_rectangle(id(1), ElementMeta::default(), rectangle(100.0));
    }
    tx.insert_filter(
        id(2),
        ElementMeta::default(),
        filter(CanvasFilterType::Inversion, 0.0),
    );
    model.apply_transaction(tx).unwrap();
    let mut cache = DocumentSceneCache::new();
    cache.sync(&model, None);
    (model, cache)
}

#[test]
fn culling_preserves_source_boundaries_and_effect_order() {
    for separator_is_filter in [false, true] {
        let (model, cache) = fixture(separator_is_filter);
        let mut composer = ViewportComposer::new();
        refresh(&mut composer, &cache, &model, 256);
        let wide = composer
            .acquire_patch(None)
            .scene_render_plan
            .clone()
            .unwrap();
        refresh(&mut composer, &cache, &model, 64);
        let narrow = composer
            .acquire_patch(None)
            .scene_render_plan
            .clone()
            .unwrap();
        assert_eq!(narrow.len(), 2);
        assert_eq!(narrow[0].source_pass, wide[0].source_pass);
        assert_eq!(narrow[1].source_pass, wide.last().unwrap().source_pass);
        assert_ne!(narrow[0].effect_run, narrow[1].effect_run);
        assert_eq!(
            narrow[0].source_pass == narrow[1].source_pass,
            separator_is_filter
        );
    }
}

#[test]
fn offscreen_boundary_visibility_changes_only_plan_and_dirties_filter_outputs() {
    let (mut model, mut cache) = fixture(false);
    let mut composer = ViewportComposer::new();
    refresh(&mut composer, &cache, &model, 64);
    let cursor = composer.current_cursor();
    let mut tx = Transaction::new("hide offscreen boundary");
    tx.update_element_meta(
        id(1),
        ElementMeta {
            visible: false,
            ..Default::default()
        },
    );
    let result = model.apply_transaction(tx).unwrap();
    cache.sync(&model, Some(&result.changes));
    refresh(&mut composer, &cache, &model, 64);
    let patch = composer.acquire_patch(Some(cursor));
    assert!(patch.scene.ops.is_empty());
    assert!(patch.scene.revision > cursor.scene_revision.0);
    assert_eq!(patch.scene_render_plan.as_ref().unwrap().len(), 1);
    assert!(
        patch
            .scene
            .dirty_regions
            .iter()
            .any(|r| r.min_x <= 12.0 && r.max_x >= 52.0)
    );
    let noop = composer.acquire_patch(Some(composer.current_cursor()));
    assert!(noop.scene_render_plan.is_none());
    assert!(composer.acquire_patch(None).scene_render_plan.is_some());
}

#[test]
fn geometry_and_view_changes_reuse_ordering_and_relationship_indices() {
    let (mut model, mut cache) = fixture(false);
    let builds = cache.order_plan_build_count();
    let relations = model.relation_index_build_count();
    let mut tx = Transaction::new("move separator");
    tx.update_rectangle(id(1), rectangle(200.0));
    let result = model.apply_transaction(tx).unwrap();
    cache.sync(&model, Some(&result.changes));
    let mut composer = ViewportComposer::new();
    for width in [64, 256, 64] {
        refresh(&mut composer, &cache, &model, width);
    }
    assert_eq!(cache.order_plan_build_count(), builds);
    assert_eq!(model.relation_index_build_count(), relations);
}

#[test]
fn duplicate_preview_reuses_model_and_updates_geometry_without_rebuilding_order() {
    let cache = DocumentSceneCache::new();
    let mut tx = Transaction::new("duplicate");
    tx.insert_filter(
        id(10),
        ElementMeta::default(),
        filter(CanvasFilterType::Inversion, 0.0),
    );
    let mut presentation = EditorPresentationState {
        duplicate_preview: Some(tx),
        ..Default::default()
    };
    let first = duplicate_preview_scene(&cache, &presentation).unwrap();
    let second = duplicate_preview_scene(&cache, &presentation).unwrap();
    assert!(std::rc::Rc::ptr_eq(&first, &second));
    let builds = first.cache.order_plan_build_count();
    drop(first);
    drop(second);
    let mut moved = Transaction::new("duplicate");
    moved.insert_filter(
        id(10),
        ElementMeta::default(),
        filter(CanvasFilterType::Inversion, 10.0),
    );
    presentation.duplicate_preview = Some(moved);
    let updated = duplicate_preview_scene(&cache, &presentation).unwrap();
    assert_eq!(updated.model.filter(id(0)).unwrap().center.x, 10.0);
    assert_eq!(updated.cache.order_plan_build_count(), builds);
    let model = DocumentModel::new();
    let items = compose_scene_items(&cache, &model, &presentation, frame(64));
    let plan = compose_scene_render_plan(&cache, &model, &presentation, &items);
    assert_eq!(plan[0].source_pass, display_item_id(id(10)));
}

#[test]
fn removing_last_filter_replaces_plan_with_empty_array() {
    let (mut model, mut cache) = fixture(false);
    let mut composer = ViewportComposer::new();
    refresh(&mut composer, &cache, &model, 64);
    let cursor = composer.current_cursor();
    let mut tx = Transaction::new("remove filters");
    tx.remove_element(id(0));
    tx.remove_element(id(2));
    let result = model.apply_transaction(tx).unwrap();
    cache.sync(&model, Some(&result.changes));
    refresh(&mut composer, &cache, &model, 64);
    assert_eq!(
        composer.acquire_patch(Some(cursor)).scene_render_plan,
        Some(Vec::new())
    );
}

#[test]
fn arrow_labels_leave_their_original_filter_boundary() {
    use snow_draw_engine_document::{ArrowData, TextData, TextLayoutSize};
    let mut model = DocumentModel::new();
    let mut tx = Transaction::new("label part order");
    let mut arrow = ArrowData::from_global_points(
        &[Point::new(-10.0, 0.0), Point::new(10.0, 0.0)],
        ColorRgba8::default(),
        2.0,
        StrokeStyle::Solid,
        snow_draw_engine_core::arrow::ArrowType::Straight,
        None,
        None,
    )
    .unwrap();
    arrow.text_element_id = Some(id(2));
    tx.insert_arrow(id(0), ElementMeta::default(), arrow);
    tx.insert_filter(
        id(1),
        ElementMeta::default(),
        filter(CanvasFilterType::Inversion, 0.0),
    );
    tx.insert_text(
        id(2),
        ElementMeta::default(),
        TextData {
            text: "label".into(),
            layout: TextLayoutSize::new(20.0, 10.0),
            ..Default::default()
        },
    );
    tx.insert_filter(
        id(3),
        ElementMeta::default(),
        filter(CanvasFilterType::Inversion, 0.0),
    );
    model.apply_transaction(tx).unwrap();
    let mut cache = DocumentSceneCache::new();
    cache.sync(&model, None);
    let mut composer = ViewportComposer::new();
    refresh(&mut composer, &cache, &model, 64);
    let plan = composer
        .acquire_patch(None)
        .scene_render_plan
        .clone()
        .unwrap();
    assert_eq!(plan.len(), 1);
    assert_eq!(plan[0].count, 2);
    let mut tx = Transaction::new("unbind label");
    let mut arrow = model.arrow(id(0)).unwrap().clone();
    arrow.text_element_id = None;
    tx.update_arrow(id(0), arrow);
    tx.reorder_elements(vec![id(2)], 2);
    let result = model.apply_transaction(tx).unwrap();
    cache.sync(&model, Some(&result.changes));
    refresh(&mut composer, &cache, &model, 64);
    assert_eq!(
        composer
            .acquire_patch(None)
            .scene_render_plan
            .clone()
            .unwrap()
            .len(),
        2
    );
}

#[test]
fn serial_relationships_refresh_on_rebind_order_and_generation_changes() {
    use snow_draw_engine_document::{SerialNumberData, TextData, TextLayoutSize};
    let mut model = DocumentModel::new();
    let mut tx = Transaction::new("shared text");
    for index in 0..2 {
        tx.insert_serial_number(
            id(index),
            ElementMeta::default(),
            SerialNumberData {
                text_element_id: Some(id(2)),
                ..Default::default()
            },
        );
    }
    for index in 2..4 {
        tx.insert_text(
            id(index),
            ElementMeta::default(),
            TextData {
                text: "text".into(),
                layout: TextLayoutSize::new(20.0, 10.0),
                ..Default::default()
            },
        );
    }
    model.apply_transaction(tx).unwrap();
    assert_eq!(model.serials_for_text(id(2)), &[id(0), id(1)]);
    let mut tx = Transaction::new("reorder serials");
    tx.reorder_elements(vec![id(1)], 0);
    model.apply_transaction(tx).unwrap();
    assert_eq!(model.serials_for_text(id(2)), &[id(1), id(0)]);
    let mut serial = model.serial_number(id(0)).unwrap().clone();
    serial.text_element_id = Some(id(3));
    let mut tx = Transaction::new("rebind serial");
    tx.update_serial_number(id(0), serial);
    let result = model.apply_transaction(tx).unwrap();
    assert!(result.changes.relations_changed);
    assert_eq!(model.serials_for_text(id(2)), &[id(1)]);
    assert_eq!(model.serials_for_text(id(3)), &[id(0)]);
    assert!(
        model
            .serials_for_text(ElementId {
                index: 2,
                generation: 2
            })
            .is_empty()
    );
    let mut tx = Transaction::new("delete text");
    tx.remove_element(id(2));
    model.apply_transaction(tx).unwrap();
    assert!(model.serials_for_text(id(2)).is_empty());
    assert_eq!(model.bound_text_id_for_serial_number(id(1)), None);
}

#[test]
fn reorder_and_history_restore_source_passes() {
    let (mut model, mut cache) = fixture(false);
    let mut composer = ViewportComposer::new();
    refresh(&mut composer, &cache, &model, 64);
    let original = composer.acquire_patch(None).scene_render_plan.clone();
    let mut tx = Transaction::new("move boundary below filters");
    tx.reorder_elements(vec![id(1)], 0);
    let moved = model.apply_transaction(tx).unwrap();
    cache.sync(&model, Some(&moved.changes));
    refresh(&mut composer, &cache, &model, 64);
    assert_eq!(
        composer
            .acquire_patch(None)
            .scene_render_plan
            .as_ref()
            .unwrap()
            .len(),
        1
    );
    let undone = model.apply_history_transaction(&moved.inverse).unwrap();
    cache.sync(&model, Some(&undone.changes));
    refresh(&mut composer, &cache, &model, 64);
    assert_eq!(composer.acquire_patch(None).scene_render_plan, original);
    let redone = model.apply_history_transaction(&undone.inverse).unwrap();
    cache.sync(&model, Some(&redone.changes));
    refresh(&mut composer, &cache, &model, 64);
    assert_eq!(
        composer
            .acquire_patch(None)
            .scene_render_plan
            .as_ref()
            .unwrap()
            .len(),
        1
    );
}

#[test]
fn effect_changes_preserve_source_identity_and_rebuild_runs() {
    let (mut model, mut cache) = fixture(true);
    let mut composer = ViewportComposer::new();
    refresh(&mut composer, &cache, &model, 64);
    let source = composer
        .acquire_patch(None)
        .scene_render_plan
        .as_ref()
        .unwrap()[0]
        .source_pass;
    let cursor = composer.current_cursor();
    let mut tx = Transaction::new("change offscreen effect");
    tx.update_filter(id(1), filter(CanvasFilterType::Inversion, 100.0));
    let result = model.apply_transaction(tx).unwrap();
    cache.sync(&model, Some(&result.changes));
    refresh(&mut composer, &cache, &model, 64);
    let patch = composer.acquire_patch(Some(cursor));
    let plan = patch.scene_render_plan.as_ref().unwrap();
    assert_eq!(plan.len(), 1);
    assert_eq!(plan[0].source_pass, source);
    assert!(!patch.scene.dirty_regions.is_empty());
}

#[test]
fn culled_first_filter_retains_complete_generation_aware_identity() {
    let mut model = DocumentModel::new();
    let first = ElementId {
        index: 0,
        generation: 17,
    };
    let mut tx = Transaction::new("offscreen pass owner");
    tx.insert_filter(
        first,
        ElementMeta::default(),
        filter(CanvasFilterType::Inversion, 100.0),
    );
    tx.insert_filter(
        id(1),
        ElementMeta::default(),
        filter(CanvasFilterType::Inversion, 0.0),
    );
    model.apply_transaction(tx).unwrap();
    let mut cache = DocumentSceneCache::new();
    cache.sync(&model, None);
    let mut composer = ViewportComposer::new();
    refresh(&mut composer, &cache, &model, 64);
    let patch = composer.acquire_patch(None);
    let run = patch.scene_render_plan.as_ref().unwrap()[0];
    assert_eq!(run.count, 1);
    assert_eq!(run.source_pass, display_item_id(first));
    assert_eq!(run.effect_run, display_item_id(first));
    let cursor = composer.current_cursor();
    let presentation = EditorPresentationState {
        hovered_rect: Some(rectangle(0.0)),
        ..Default::default()
    };
    composer.refresh_with_presentation(
        &cache,
        &model,
        frame(64),
        &presentation,
        SnapConfig::default(),
    );
    assert!(
        composer
            .acquire_patch(Some(cursor))
            .scene_render_plan
            .is_none()
    );
}

#[test]
fn new_arrow_label_draft_is_a_boundary_even_when_owner_emits_no_geometry() {
    use snow_draw_engine_document::{TextData, TextLayoutSize};
    use snow_draw_engine_editor::{ActiveTextDraftPresentation, ActiveTextDraftTarget};
    let mut model = DocumentModel::new();
    let mut arrow = ArrowData::from_global_points(
        &[Point::new(-10.0, 0.0), Point::new(10.0, 0.0)],
        ColorRgba8::default(),
        2.0,
        StrokeStyle::Solid,
        snow_draw_engine_core::arrow::ArrowType::Straight,
        None,
        None,
    )
    .unwrap();
    arrow.points = vec![[0.0, 0.0], [0.0, 0.0]];
    let mut tx = Transaction::new("label without arrow geometry");
    tx.insert_filter(
        id(0),
        ElementMeta::default(),
        filter(CanvasFilterType::Inversion, 0.0),
    );
    tx.insert_arrow(id(1), ElementMeta::default(), arrow);
    tx.insert_filter(
        id(2),
        ElementMeta::default(),
        filter(CanvasFilterType::Inversion, 0.0),
    );
    model.apply_transaction(tx).unwrap();
    let mut cache = DocumentSceneCache::new();
    cache.sync(&model, None);
    let presentation = EditorPresentationState {
        active_text_draft: Some(ActiveTextDraftPresentation {
            target: ActiveTextDraftTarget::NewArrow(id(1)),
            revision: 1,
            text: TextData {
                text: "label".into(),
                layout: TextLayoutSize::new(20.0, 10.0),
                ..Default::default()
            },
        }),
        ..Default::default()
    };
    let items = compose_scene_items(&cache, &model, &presentation, frame(64));
    assert!(matches!(
        items.as_slice(),
        [
            SceneDisplayItem::Filter(_),
            SceneDisplayItem::Text(_),
            SceneDisplayItem::Filter(_)
        ]
    ));
    let runs = compose_scene_render_plan(&cache, &model, &presentation, &items);
    assert_ne!(runs[0].source_pass, runs[1].source_pass);
}
