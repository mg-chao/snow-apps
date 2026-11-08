use super::*;
use snow_draw_engine_core::{Camera, SurfaceSize};
use snow_draw_engine_document::{
    ElementMeta, HighlightShape, RectangleElementKind, TextLayoutSize, Transaction,
};
use snow_draw_engine_editor::{
    ActiveTextDraftPresentation, ActiveTextDraftTarget, ShapeBoundTextPreview,
};

fn id(index: u32) -> ElementId {
    ElementId {
        index,
        generation: 1,
    }
}

fn shape(text_element_id: Option<ElementId>) -> RectangleData {
    RectangleData {
        text_element_id,
        rectangle_kind: RectangleElementKind::Rectangle,
        highlight_shape: HighlightShape::Rectangle,
        center: Point::new(-70.0, -30.0),
        width: 80.0,
        height: 60.0,
        rotation: 0.4,
        fill: ColorRgba8::default(),
        fill_style: FillStyle::Solid,
        stroke: ColorRgba8 {
            r: 20,
            g: 40,
            b: 60,
            a: 255,
        },
        stroke_width: 3.0,
        stroke_style: StrokeStyle::Solid,
        stroke_rendering_style: Default::default(),
        stroke_rendering_seed: 0,
        corner_radii: CornerRadii::splat(8.0),
        opacity: 0.7,
    }
}

fn label() -> TextData {
    TextData {
        text: "label".to_owned(),
        center: Point::new(130.0, 90.0),
        layout: TextLayoutSize::with_content(100.0, 30.0, 80.0, 24.0),
        fill: ColorRgba8 {
            r: 255,
            g: 255,
            b: 255,
            a: 255,
        },
        ..Default::default()
    }
}

fn frame() -> FrameView {
    FrameView {
        surface: SurfaceSize {
            width: 800,
            height: 600,
        },
        camera: Camera {
            center: Point::default(),
            zoom: 1.0,
        },
        clear_color: ColorRgba8::default(),
    }
}

fn fixture(shape: RectangleData, text: Option<TextData>) -> (DocumentModel, DocumentSceneCache) {
    let mut model = DocumentModel::new();
    let mut tx = Transaction::new("shape label fixture");
    tx.insert_rectangle(id(0), ElementMeta::default(), shape);
    if let Some(text) = text {
        tx.insert_text(id(1), ElementMeta::default(), text);
    }
    model.apply_transaction(tx).unwrap();
    let mut cache = DocumentSceneCache::new();
    cache.sync(&model, None);
    (model, cache)
}

fn assert_connector(items: &[SceneDisplayItem], shape: &RectangleData, text_id: ElementId) {
    let index = items
        .iter()
        .position(|item| {
            matches!(item,
        SceneDisplayItem::Text(text) if text.id == display_item_id(text_id))
        })
        .unwrap();
    let SceneDisplayItem::Text(text) = &items[index] else {
        unreachable!()
    };
    let SceneDisplayItem::SerialNumberConnector(connector) = &items[index + 1] else {
        panic!("shape connector must paint immediately above its label");
    };
    let expected = snow_draw_engine_document::resolve_shape_paint_text_connection(
        shape,
        &text_paint_geometry_from_display_item(text),
    )
    .unwrap();
    assert_eq!(connector.id, display_item_id(id(0)));
    assert_eq!(connector.start_x, expected.start.x);
    assert_eq!(connector.start_y, expected.start.y);
    assert_eq!(connector.end_x, expected.end.x);
    assert_eq!(connector.end_y, expected.end.y);
    assert_eq!(connector.stroke, shape.stroke);
    assert_eq!(connector.stroke_width, shape.stroke_width);
    assert_eq!(connector.opacity, shape.opacity);
}

#[test]
fn shape_text_connector_uses_each_shape_contour_and_paints_above_filled_label() {
    for kind in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        let mut shape = shape(Some(id(1)));
        shape.highlight_shape = kind;
        let (model, cache) = fixture(shape, Some(label()));
        let items =
            compose_scene_items(&cache, &model, &EditorPresentationState::default(), frame());
        assert_connector(&items, &shape, id(1));
        let SceneDisplayItem::Rectangle(owner) = &items[0] else {
            panic!("owner shape")
        };
        assert_eq!(owner.bound_text_id, Some(display_item_id(id(1))));
    }
}

#[test]
fn shape_text_drag_preview_binds_synthetic_text_and_replaces_existing_text() {
    for existing in [false, true] {
        let shape = shape(existing.then_some(id(1)));
        let (model, cache) = fixture(shape, existing.then(label));
        let text_id = if existing { id(1) } else { id(u32::MAX - 2) };
        let mut text = label();
        text.center = Point::new(220.0, 120.0);
        let presentation = EditorPresentationState {
            shape_bound_text_preview: Some(ShapeBoundTextPreview {
                owner_id: id(0),
                text_id,
                text,
            }),
            ..Default::default()
        };
        let items = compose_scene_items(&cache, &model, &presentation, frame());
        assert_eq!(
            items
                .iter()
                .filter(|item| matches!(item, SceneDisplayItem::Text(_)))
                .count(),
            1
        );
        assert_connector(&items, &shape, text_id);
        let SceneDisplayItem::Rectangle(owner) = &items[0] else {
            panic!("owner shape")
        };
        assert_eq!(owner.bound_text_id, Some(display_item_id(text_id)));
    }
}

#[test]
fn shape_text_connector_tracks_displayed_owner_and_active_text_draft() {
    let shape = shape(Some(id(1)));
    let (model, cache) = fixture(shape, Some(label()));
    let mut preview_shape = shape;
    preview_shape.center.x += 50.0;
    preview_shape.width += 20.0;
    preview_shape.rotation += 0.3;
    let mut text = label();
    text.center = Point::new(240.0, 70.0);
    let presentation = EditorPresentationState {
        preview_elements: vec![SelectionRectState {
            id: id(0),
            rect: preview_shape,
        }],
        active_text_draft: Some(ActiveTextDraftPresentation {
            target: ActiveTextDraftTarget::Existing(id(1)),
            revision: 1,
            text,
        }),
        ..Default::default()
    };
    let items = compose_scene_items(&cache, &model, &presentation, frame());
    assert_connector(&items, &preview_shape, id(1));
}

#[test]
fn shape_text_connector_remains_visible_when_owner_is_culled() {
    let mut shape = shape(Some(id(1)));
    shape.center.x = -900.0;
    let (model, cache) = fixture(shape, Some(label()));
    let items = compose_scene_items(&cache, &model, &EditorPresentationState::default(), frame());
    assert!(
        !items
            .iter()
            .any(|item| matches!(item, SceneDisplayItem::Rectangle(_)))
    );
    assert_connector(&items, &shape, id(1));
}

#[test]
fn shape_text_connector_survives_culled_labels_and_preserves_paint_order() {
    for owner_x in [-200.0, -900.0] {
        let mut shape = shape(Some(id(1)));
        shape.center = Point::new(owner_x, 0.0);
        let mut text = label();
        text.center = Point::new(900.0, 0.0);
        let (mut model, mut cache) = fixture(shape, Some(text));
        let mut tx = Transaction::new("drawable above label");
        let mut above = shape;
        above.text_element_id = None;
        above.center = Point::new(0.0, 150.0);
        tx.insert_rectangle(id(2), ElementMeta::default(), above);
        let result = model.apply_transaction(tx).unwrap();
        cache.sync(&model, Some(&result.changes));
        let presentation = EditorPresentationState::default();
        let mut wide = frame();
        wide.surface.width = 2400;
        let full = compose_scene_items(&cache, &model, &presentation, wide);
        let cropped = compose_scene_items(&cache, &model, &presentation, frame());
        assert!(
            !cropped
                .iter()
                .any(|item| matches!(item, SceneDisplayItem::Text(_)))
        );
        let index = cropped
            .iter()
            .position(|item| {
                matches!(item,
            SceneDisplayItem::SerialNumberConnector(connector)
            if connector.id == display_item_id(id(0)))
            })
            .expect("visible connector to culled label");
        assert_eq!(
            cropped[index],
            *full
                .iter()
                .find(|item| matches!(item, SceneDisplayItem::SerialNumberConnector(_)))
                .unwrap()
        );
        assert!(
            matches!(&cropped[index + 1], SceneDisplayItem::Rectangle(rect)
            if rect.id == display_item_id(id(2)))
        );
    }
}

#[test]
fn shape_text_culled_preview_connectors_use_presented_geometry() {
    for mode in 0..4 {
        let mut shape = shape((mode != 3).then_some(id(1)));
        shape.center = Point::new(-200.0, 0.0);
        let (model, cache) = fixture(shape, (mode != 3).then(label));
        let mut text = label();
        text.center = Point::new(900.0, 0.0);
        let text_id = if mode == 3 { id(u32::MAX - 2) } else { id(1) };
        let mut presentation = EditorPresentationState::default();
        match mode {
            0 => {
                presentation.active_text_draft = Some(ActiveTextDraftPresentation {
                    target: ActiveTextDraftTarget::Existing(text_id),
                    revision: 1,
                    text: text.clone(),
                })
            }
            1 => {
                let mut rect = model.element_rect_proxy(text_id).unwrap();
                rect.center = text.center;
                presentation
                    .preview_elements
                    .push(SelectionRectState { id: text_id, rect });
            }
            _ => {
                presentation.shape_bound_text_preview = Some(ShapeBoundTextPreview {
                    owner_id: id(0),
                    text_id,
                    text: text.clone(),
                })
            }
        }
        let items = compose_scene_items(&cache, &model, &presentation, frame());
        assert!(
            !items
                .iter()
                .any(|item| matches!(item, SceneDisplayItem::Text(_)))
        );
        let connector = items
            .iter()
            .find_map(|item| match item {
                SceneDisplayItem::SerialNumberConnector(connector) => Some(connector),
                _ => None,
            })
            .expect("connector from offscreen presented text");
        let expected =
            snow_draw_engine_document::resolve_shape_text_connection(&shape, &text).unwrap();
        assert_eq!(connector.start_x, expected.start.x);
        assert_eq!(connector.end_x, expected.end.x);
    }
}

#[test]
fn serial_text_connector_survives_culled_label() {
    let mut model = DocumentModel::new();
    let mut text = label();
    text.center = Point::new(900.0, 0.0);
    let serial = SerialNumberData {
        center: Point::new(-200.0, 0.0),
        text_element_id: Some(id(1)),
        ..Default::default()
    };
    let mut tx = Transaction::new("serial with offscreen label");
    tx.insert_serial_number(id(0), ElementMeta::default(), serial.clone());
    tx.insert_text(id(1), ElementMeta::default(), text.clone());
    model.apply_transaction(tx).unwrap();
    let mut cache = DocumentSceneCache::new();
    cache.sync(&model, None);
    let items = compose_scene_items(&cache, &model, &EditorPresentationState::default(), frame());
    assert!(
        !items
            .iter()
            .any(|item| matches!(item, SceneDisplayItem::Text(_)))
    );
    let expected =
        snow_draw_engine_document::resolve_serial_number_text_connection(&serial, &text).unwrap();
    assert!(items.iter().any(|item| matches!(item,
        SceneDisplayItem::SerialNumberConnector(connector)
        if connector.start_x == expected.start.x && connector.end_x == expected.end.x)));
}

#[test]
fn shape_text_connectors_respect_hidden_labels_and_offscreen_geometry() {
    for hidden in [false, true] {
        let mut shape = shape(Some(id(1)));
        shape.center = if hidden {
            Point::new(-200.0, 0.0)
        } else {
            Point::new(-900.0, -900.0)
        };
        let mut text = label();
        text.center = if hidden {
            Point::new(900.0, 0.0)
        } else {
            Point::new(-700.0, -900.0)
        };
        let (mut model, mut cache) = fixture(shape, Some(text));
        if hidden {
            let mut tx = Transaction::new("hide bound label");
            tx.update_element_meta(
                id(1),
                ElementMeta {
                    visible: false,
                    locked: false,
                },
            );
            let result = model.apply_transaction(tx).unwrap();
            cache.sync(&model, Some(&result.changes));
        }
        let items =
            compose_scene_items(&cache, &model, &EditorPresentationState::default(), frame());
        assert!(!items.iter().any(|item| matches!(
            item,
            SceneDisplayItem::Text(_) | SceneDisplayItem::SerialNumberConnector(_)
        )));
    }
}

#[test]
fn shape_and_speech_bubble_bottom_handles_match_angle_controls_at_each_zoom() {
    let presentation = EditorPresentationState {
        shape_text_handle: Some(Point::new(20.0, 80.0)),
        speech_bubble_handles: vec![(0, Point::new(20.0, 80.0))],
        arrow_handles: [ArrowHandleKind::Endpoint, ArrowHandleKind::AngleArc]
            .into_iter()
            .map(|kind| ArrowHandleState {
                kind,
                center: Point::new(-20.0, -40.0),
                anchor: None,
                fixed_segment: false,
                hovered: false,
            })
            .collect(),
        ..Default::default()
    };
    for zoom in [0.5, 1.0, 2.0] {
        let mut view = frame();
        view.camera.zoom = zoom;
        let items = compose_overlay_items(SnapConfig::default(), &presentation, view);
        let control = |kind| {
            items
                .iter()
                .find_map(|item| match item {
                    OverlayDisplayItem::Rectangle(rect) if rect.kind == kind => Some(rect),
                    _ => None,
                })
                .unwrap()
        };
        let handle = control(UiShapeKind::ShapeTextHandle);
        let bubble = control(UiShapeKind::SpeechBubbleAddTailHandle);
        assert_eq!((handle.center_x, handle.center_y), (20.0, 80.0));
        assert_eq!((bubble.center_x, bubble.center_y), (20.0, 80.0));
        assert_eq!(handle.stroke, bubble.stroke);
        assert_eq!(handle.fill, bubble.fill);

        let angles: Vec<_> = items
            .iter()
            .filter_map(|item| match item {
                OverlayDisplayItem::Rectangle(rect)
                    if rect.kind == UiShapeKind::ArrowEndpointHandle =>
                {
                    Some(rect)
                }
                _ => None,
            })
            .collect();
        assert_eq!(angles.len(), 2);
        for angle in angles {
            assert_eq!((angle.width * zoom, angle.height * zoom), (10.0, 10.0));
            for bottom in [handle, bubble] {
                assert_eq!((bottom.width, bottom.height), (angle.width, angle.height));
                assert_eq!(bottom.corner_radii, CornerRadii::splat(angle.width / 2.0));
                assert_eq!(bottom.stroke_width, angle.stroke_width);
            }
        }
    }
}

#[test]
fn shape_text_duplicate_preview_remaps_owner_and_label_together() {
    let (model, cache) = fixture(shape(Some(id(1))), Some(label()));
    let mut copy = Transaction::new("duplicate shape label preview");
    let mut copied_shape = shape(Some(id(3)));
    copied_shape.center.y += 60.0;
    let mut copied_text = label();
    copied_text.center.y += 60.0;
    copy.insert_rectangle(id(2), ElementMeta::default(), copied_shape);
    copy.insert_text(id(3), ElementMeta::default(), copied_text);
    let presentation = EditorPresentationState {
        duplicate_preview: Some(copy),
        ..Default::default()
    };
    let items = compose_scene_items(&cache, &model, &presentation, frame());
    let owner = items
        .iter()
        .find_map(|item| match item {
            SceneDisplayItem::Rectangle(owner) if owner.id == display_item_id(id(2)) => Some(owner),
            _ => None,
        })
        .unwrap();
    assert_eq!(owner.bound_text_id, Some(display_item_id(id(3))));
    let text_index = items
        .iter()
        .position(|item| {
            matches!(item,
        SceneDisplayItem::Text(text) if text.id == display_item_id(id(3)))
        })
        .unwrap();
    assert!(
        matches!(&items[text_index + 1], SceneDisplayItem::SerialNumberConnector(connector)
        if connector.id == display_item_id(id(2)))
    );
}

#[test]
fn shape_text_connector_move_dirties_previous_and_current_geometry() {
    let (mut model, mut cache) = fixture(shape(Some(id(1))), Some(label()));
    let presentation = EditorPresentationState::default();
    let mut composer = ViewportComposer::new();
    composer.refresh_with_presentation(
        &cache,
        &model,
        frame(),
        &presentation,
        SnapConfig::default(),
    );
    let old_items = compose_scene_items(&cache, &model, &presentation, frame());
    let cursor = composer.current_cursor();
    let mut text = label();
    text.center = Point::new(250.0, 150.0);
    let mut tx = Transaction::new("move shape label");
    tx.update_text(id(1), text);
    let result = model.apply_transaction(tx).unwrap();
    cache.sync(&model, Some(&result.changes));
    let new_items = compose_scene_items(&cache, &model, &presentation, frame());
    composer.refresh_with_presentation(
        &cache,
        &model,
        frame(),
        &presentation,
        SnapConfig::default(),
    );
    let patch = composer.acquire_patch(Some(cursor));
    for items in [old_items, new_items] {
        let connector = items
            .iter()
            .find(|item| matches!(item, SceneDisplayItem::SerialNumberConnector(_)))
            .unwrap();
        let bounds = scene_display_item_bounds(connector, frame()).unwrap();
        assert!(patch.scene.dirty_regions.iter().any(|region| {
            region.min_x <= bounds.min_x
                && region.min_y <= bounds.min_y
                && region.max_x >= bounds.max_x
                && region.max_y >= bounds.max_y
        }));
    }
}
