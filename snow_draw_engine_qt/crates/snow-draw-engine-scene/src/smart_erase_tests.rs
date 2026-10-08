use super::*;
use snow_draw_engine_document::{
    CanvasFilterType, ElementMeta, FilterData, PenFilterData, Transaction,
};
use snow_draw_engine_editor::PenFilterPreview;

fn id(index: u32) -> ElementId {
    ElementId {
        index,
        generation: 1,
    }
}

fn filter(kind: CanvasFilterType, opacity: f64) -> FilterData {
    FilterData {
        filter_type: kind,
        center: Point::new(10_000.0, 10_000.0),
        width: 20.0,
        height: 30.0,
        opacity,
        ..Default::default()
    }
}

fn pen(opacity: f64) -> PenFilterData {
    PenFilterData::from_global_points(
        &[
            Point::new(20_000.0, 20_000.0),
            Point::new(20_010.0, 20_020.0),
        ],
        CanvasFilterType::SmartErase,
        0.9,
        10.0,
        opacity,
    )
    .unwrap()
}

fn indices(items: &[SceneDisplayItem]) -> Vec<u32> {
    items
        .iter()
        .map(|item| {
            let SceneDisplayItem::Filter(filter) = item else {
                panic!("Smart Erase exports contain only filters");
            };
            assert_eq!(filter.filter.strength, 0.5);
            filter.id.index
        })
        .collect()
}

#[test]
fn cached_smart_erase_membership_tracks_visibility_type_removal_and_restore() {
    let mut model = DocumentModel::new();
    let mut tx = Transaction::new("sparse Smart Erase membership");
    tx.insert_filter(
        id(0),
        ElementMeta::default(),
        filter(CanvasFilterType::Mosaic, 1.0),
    );
    let mut degenerate = filter(CanvasFilterType::SmartErase, 0.0);
    degenerate.width = 0.0;
    degenerate.height = 0.0;
    tx.insert_filter(id(1), ElementMeta::default(), degenerate);
    tx.insert_pen_filter(id(2), ElementMeta::default(), pen(0.0));
    tx.insert_filter(
        id(3),
        ElementMeta {
            visible: false,
            ..Default::default()
        },
        filter(CanvasFilterType::SmartErase, 1.0),
    );
    model.apply_transaction(tx).unwrap();
    let mut cache = DocumentSceneCache::new();
    cache.sync(&model, None);
    assert!(cache.entry(id(1)).is_none());
    assert!(cache.entry(id(2)).is_none());
    let presentation = EditorPresentationState::default();
    let initial = cached_smart_erase_items(&model, &cache, &presentation);
    assert_eq!(initial, smart_erase_items(&model, &presentation));
    assert_eq!(indices(&initial), vec![1, 2]);
    assert_eq!(cache.smart_erase_candidate_visit_count(), 2);
    assert!(
        initial
            .iter()
            .all(|item| matches!(item, SceneDisplayItem::Filter(f) if f.opacity == 0.0))
    );

    let mut tx = Transaction::new("change sparse Smart Erase members");
    tx.update_filter(id(0), filter(CanvasFilterType::SmartErase, 1.0));
    tx.update_filter(id(1), filter(CanvasFilterType::Mosaic, 1.0));
    tx.update_element_meta(id(3), ElementMeta::default());
    let result = model.apply_transaction(tx).unwrap();
    cache.sync(&model, Some(&result.changes));
    let changed = cached_smart_erase_items(&model, &cache, &presentation);
    assert_eq!(indices(&changed), vec![0, 2, 3]);
    assert_eq!(changed, smart_erase_items(&model, &presentation));
    let mut rebuilt = DocumentSceneCache::new();
    rebuilt.sync(&model, None);
    assert_eq!(
        cached_smart_erase_items(&model, &rebuilt, &presentation),
        changed
    );

    let mut tx = Transaction::new("evict sparse Smart Erase members");
    tx.update_filter(id(0), filter(CanvasFilterType::Mosaic, 1.0));
    tx.remove_element(id(2));
    tx.update_element_meta(
        id(3),
        ElementMeta {
            visible: false,
            ..Default::default()
        },
    );
    let result = model.apply_transaction(tx).unwrap();
    cache.sync(&model, Some(&result.changes));
    assert!(cached_smart_erase_items(&model, &cache, &presentation).is_empty());
    assert_eq!(cache.smart_erase_owner_ids().len(), 0);
    let restored = model.apply_history_transaction(&result.inverse).unwrap();
    cache.sync(&model, Some(&restored.changes));
    assert_eq!(
        cached_smart_erase_items(&model, &cache, &presentation),
        changed
    );
}

#[test]
fn cached_smart_erase_export_preserves_selection_creation_phases_and_uncropped_geometry() {
    let mut model = DocumentModel::new();
    let mut tx = Transaction::new("Smart Erase presentation phases");
    tx.insert_filter(
        id(0),
        ElementMeta::default(),
        filter(CanvasFilterType::SmartErase, 1.0),
    );
    tx.insert_pen_filter(id(1), ElementMeta::default(), pen(1.0));
    model.apply_transaction(tx).unwrap();
    let mut cache = DocumentSceneCache::new();
    cache.sync(&model, None);
    let committed = cached_smart_erase_items(&model, &cache, &EditorPresentationState::default());
    assert!(
        committed
            .iter()
            .all(|item| matches!(item, SceneDisplayItem::Filter(f) if f.filter.render_phase == 0))
    );
    let previews = [(id(0), 30_000.0, 0.4), (id(1), 40_000.0, 0.25)].map(|(owner, x, opacity)| {
        let mut rect = model.element_rect_proxy(owner).unwrap();
        rect.center.x = x;
        rect.opacity = opacity;
        SelectionRectState { id: owner, rect }
    });
    let mut presentation = EditorPresentationState {
        preview_elements: previews.to_vec(),
        creation_preview: Some(ElementCreationPreview::Filter(filter(
            CanvasFilterType::SmartErase,
            0.6,
        ))),
        ..Default::default()
    };
    for creation in [
        presentation.creation_preview.clone().unwrap(),
        ElementCreationPreview::PenFilter(PenFilterPreview {
            global_points: vec![
                Point::new(50_000.0, 50_000.0),
                Point::new(50_010.0, 50_020.0),
            ],
            filter_type: CanvasFilterType::SmartErase,
            strength: 0.9,
            stroke_width: 10.0,
            opacity: 0.6,
        }),
    ] {
        presentation.creation_preview = Some(creation);
        let items = cached_smart_erase_items(&model, &cache, &presentation);
        assert_eq!(items, smart_erase_items(&model, &presentation));
        assert_eq!(indices(&items), vec![0, 1, 2]);
        for (index, item) in items.iter().enumerate() {
            let SceneDisplayItem::Filter(f) = item else {
                unreachable!();
            };
            assert_eq!(f.filter.render_phase, if index < 2 { 2 } else { 1 });
            assert!(f.center_x > 500.0 && f.center_y > 500.0);
            assert_eq!(f.opacity, [0.4, 0.25, 0.6][index]);
        }
    }
}
