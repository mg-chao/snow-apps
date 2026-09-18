use std::collections::HashMap;

use super::*;

pub(crate) fn compose_scene_items(
    cache: &DocumentSceneCache,
    model: &DocumentModel,
    presentation: &EditorPresentationState,
    frame_view: FrameView,
) -> Vec<SceneDisplayItem> {
    let viewport = canvas_viewport(frame_view.camera, frame_view.surface);
    let mut ordered_ids = model.visible_element_ids(ViewportQuery {
        camera: frame_view.camera,
        surface: frame_view.surface,
    });
    let preview_rects: HashMap<_, _> = presentation
        .preview_elements
        .iter()
        .copied()
        .map(|preview| (preview.id, preview.rect))
        .collect();
    let preview_text_font_sizes: HashMap<_, _> = presentation
        .preview_text_font_sizes
        .iter()
        .map(|preview| (preview.id, preview.font_size))
        .collect();
    let active_existing_text = presentation
        .active_text_draft
        .as_ref()
        .and_then(|draft| draft.existing_id().map(|id| (id, draft.text.clone())));
    let preview_texts = preview_text_items(model, &preview_rects, active_existing_text.as_ref());
    let preview_serials = preview_serial_items(model, &preview_rects);
    let preview_arrows: HashMap<_, _> = presentation
        .preview_arrows
        .iter()
        .cloned()
        .map(|preview| (preview.id, preview.arrow))
        .collect();
    let mut emitted_preview_ids = HashMap::<ElementId, bool>::new();
    let mut items = Vec::new();
    let serial_connectors = SerialConnectorEmission {
        model,
        preview_texts: &preview_texts,
        preview_serials: &preview_serials,
        viewport,
    };

    for id in preview_rects
        .keys()
        .chain(preview_arrows.keys())
        .chain(active_existing_text.iter().map(|(id, _)| id))
    {
        if model.paint_rank(*id).is_some() && !ordered_ids.contains(id) {
            ordered_ids.push(*id);
        }
    }
    ordered_ids.sort_unstable_by_key(|id| model.paint_rank(*id).unwrap_or(u32::MAX));

    for id in &ordered_ids {
        if let Some((active_id, active_text)) = active_existing_text.as_ref()
            && id == active_id
        {
            emitted_preview_ids.insert(*id, true);
            if bounds_visible(text_bounds(active_text), viewport) {
                serial_connectors.push_item(
                    &mut items,
                    *id,
                    scene_item_from_text(*id, active_text.clone()),
                );
            }
            continue;
        }
        if let Some(preview_rect) = preview_rects.get(id) {
            emitted_preview_ids.insert(*id, true);
            if let Some((item, bounds)) = scene_item_from_selection_preview(
                model,
                *id,
                *preview_rect,
                preview_text_font_sizes.get(id).copied(),
            ) && bounds_visible(bounds, viewport)
            {
                serial_connectors.push_item(&mut items, *id, item);
            }
            continue;
        }
        if let Some(preview_arrow) = preview_arrows.get(id) {
            emitted_preview_ids.insert(*id, true);
            if !arrow_is_degenerate(preview_arrow)
                && bounds_visible(arrow_bounds(preview_arrow), viewport)
            {
                items.push(scene_item_from_arrow(*id, preview_arrow.clone()));
            }
            continue;
        }

        let Some(bounds) = cache.bounds(*id) else {
            continue;
        };
        if !bounds_visible(bounds, viewport) {
            continue;
        }
        if let Some(item) = cache.entry(*id) {
            serial_connectors.push_item(
                &mut items,
                *id,
                scene_item_with_serial_bound_text(
                    item.clone(),
                    model.bound_text_id_for_serial_number(*id),
                ),
            );
        }
    }

    for preview in &presentation.preview_elements {
        if emitted_preview_ids.contains_key(&preview.id) {
            continue;
        }
        if let Some((item, bounds)) = scene_item_from_selection_preview(
            model,
            preview.id,
            preview.rect,
            preview_text_font_sizes.get(&preview.id).copied(),
        ) && bounds_visible(bounds, viewport)
        {
            serial_connectors.push_item(&mut items, preview.id, item);
        }
    }
    for preview in &presentation.preview_arrows {
        if emitted_preview_ids.contains_key(&preview.id) {
            continue;
        }
        if !arrow_is_degenerate(&preview.arrow)
            && bounds_visible(arrow_bounds(&preview.arrow), viewport)
        {
            items.push(scene_item_from_arrow(preview.id, preview.arrow.clone()));
        }
    }
    if let Some(preview) = presentation.creation_preview.as_ref() {
        let id = model.peek_next_element_id();
        match preview {
            ElementCreationPreview::Rectangle(rect)
                if !rect.is_spotlight() && bounds_visible(rect_bounds(*rect), viewport) =>
            {
                items.push(scene_item_from_rect(id, *rect));
            }
            ElementCreationPreview::Filter(filter)
                if bounds_visible(filter_bounds(filter), viewport) =>
            {
                items.push(scene_item_from_filter(id, *filter));
            }
            ElementCreationPreview::PenFilter(preview) => {
                if let Some((item, bounds)) = scene_item_from_pen_filter_preview(id, preview)
                    && bounds_visible(bounds, viewport)
                {
                    items.push(item);
                }
            }
            ElementCreationPreview::Arrow(arrow)
                if !arrow_is_degenerate(arrow) && bounds_visible(arrow_bounds(arrow), viewport) =>
            {
                items.push(scene_item_from_arrow(id, arrow.clone()));
            }
            ElementCreationPreview::FreeDraw(preview)
                if bounds_visible(free_draw_preview_bounds(preview), viewport) =>
            {
                items.push(scene_item_from_free_draw_preview(id, preview));
            }
            ElementCreationPreview::SerialNumber(serial)
                if bounds_visible(serial_number_bounds(serial), viewport) =>
            {
                items.push(scene_item_from_serial_number(id, serial.clone(), None));
            }
            _ => {}
        }
    }
    if let Some(active_draft) = presentation.active_text_draft.as_ref()
        && active_draft.existing_id().is_none()
        && bounds_visible(text_bounds(&active_draft.text), viewport)
    {
        items.push(scene_item_from_text(
            active_draft.display_id(),
            active_draft.text.clone(),
        ));
    }

    for item in &mut items {
        if let SceneDisplayItem::Filter(filter) = item {
            let id = ElementId {
                index: filter.id.index,
                generation: filter.id.generation,
            };
            if presentation.creation_preview.is_some() && id == model.peek_next_element_id() {
                filter.filter.render_phase = 1;
            } else if preview_rects.contains_key(&id) {
                filter.filter.render_phase = 2;
            }
        }
    }
    // Creation previews are appended above; Smart Erase keeps its fixed bottom layer even then.
    items.sort_by_key(|item| match item {
        SceneDisplayItem::Filter(f)
            if f.filter.filter_type == snow_draw_engine_display::DisplayFilterType::SmartErase =>
        {
            (0, f.id.index)
        }
        _ => (1, 0),
    });
    compose_arrow_text(&mut items, model, presentation, &preview_arrows, viewport);
    if let Some((copy_model, copy_cache, ids)) = duplicate_preview_scene(presentation) {
        let mut copies = compose_scene_items(
            &copy_cache,
            &copy_model,
            &EditorPresentationState::default(),
            frame_view,
        );
        for item in &mut copies {
            remap_copy_display_ids(item, &ids);
        }
        items.extend(copies);
        // Smart Erase always occupies the bottom layer, including copy previews.
        items.sort_by_key(|item| match item {
            SceneDisplayItem::Filter(f)
                if f.filter.filter_type
                    == snow_draw_engine_display::DisplayFilterType::SmartErase =>
            {
                (0, f.id.index)
            }
            _ => (1, 0),
        });
    }
    // Emission puts connectors after bound text; later passes can separate them.
    // Restack is the composition output contract, including copy previews whose
    // ids are absent from `model` and are recovered from serial display items.
    restack_serial_connectors_above_bound_text(&mut items, model);
    debug_assert!(
        serial_connectors_follow_bound_text(&items, model),
        "serial connectors must paint immediately above their bound text"
    );
    items
}

// Build only the copied subset, never clone the full document. This keeps links,
// arrow labels and serial connectors on the normal scene composition path.
pub(crate) fn duplicate_preview_scene(
    presentation: &EditorPresentationState,
) -> Option<(DocumentModel, DocumentSceneCache, Vec<ElementId>)> {
    use snow_draw_engine_document::{Operation, Transaction};
    let transaction = presentation.duplicate_preview.as_ref()?;
    let ids: Vec<_> = transaction
        .operations()
        .iter()
        .filter_map(|op| match op {
            Operation::InsertElement { id, .. } => Some(*id),
            _ => None,
        })
        .collect();
    let local_ids: HashMap<_, _> = ids
        .iter()
        .enumerate()
        .map(|(index, id)| {
            (
                *id,
                ElementId {
                    index: index as u32,
                    generation: 1,
                },
            )
        })
        .collect();
    let local_id = |id| local_ids.get(&id).copied();
    let mut compact = Transaction::new("copy preview");
    for op in transaction.operations() {
        if let Operation::InsertElement { id, meta, data } = op {
            let mut data = data.clone();
            match &mut data {
                ElementData::Arrow(arrow) => {
                    arrow.text_element_id = arrow.text_element_id.and_then(local_id);
                }
                ElementData::SerialNumber(serial) => {
                    serial.text_element_id = serial.text_element_id.and_then(local_id);
                }
                _ => {}
            }
            compact.push(Operation::InsertElement {
                id: local_id(*id)?,
                meta: *meta,
                data,
            });
        }
    }
    let mut model = DocumentModel::new();
    model.apply_transaction(compact).ok()?;
    let mut cache = DocumentSceneCache::new();
    cache.sync(&model, None);
    Some((model, cache, ids))
}

fn remap_copy_display_ids(item: &mut SceneDisplayItem, ids: &[ElementId]) {
    let remap = |id: &mut DisplayItemId| {
        *id = display_item_id(ids[id.index as usize]);
    };
    match item {
        SceneDisplayItem::Rectangle(item) => remap(&mut item.id),
        SceneDisplayItem::Filter(item) => remap(&mut item.id),
        SceneDisplayItem::Text(item) => remap(&mut item.id),
        SceneDisplayItem::SerialNumberConnector(item) => remap(&mut item.id),
        SceneDisplayItem::Arrow(item) => {
            remap(&mut item.id);
            if let Some(id) = &mut item.bound_text_id {
                remap(id);
            }
        }
        SceneDisplayItem::SerialNumber(item) => {
            remap(&mut item.id);
            if let Some(id) = &mut item.bound_text_id {
                remap(id);
            }
        }
        SceneDisplayItem::Stroke | SceneDisplayItem::Image => {}
    }
}

fn compose_arrow_text(
    items: &mut Vec<SceneDisplayItem>,
    model: &DocumentModel,
    presentation: &EditorPresentationState,
    arrows: &HashMap<ElementId, ArrowData>,
    viewport: (f64, f64, f64, f64),
) {
    let mut bindings = model.arrow_text_bindings();
    let new_draft = presentation.active_text_draft.as_ref().and_then(|draft| {
        if let snow_draw_engine_editor::ActiveTextDraftTarget::NewArrow(id) = draft.target {
            Some((id, draft.display_id()))
        } else {
            None
        }
    });
    if let Some(binding) = new_draft {
        bindings.push(binding);
    }
    for (arrow_id, text_id) in bindings {
        let Some(arrow) = arrows.get(&arrow_id).or_else(|| model.arrow(arrow_id).ok()) else {
            continue;
        };
        let Ok(owner) = model.element(arrow_id) else {
            continue;
        };
        let draft = presentation
            .active_text_draft
            .as_ref()
            .filter(|draft| draft.display_id() == text_id);
        let Some(mut text) = draft
            .map(|d| d.text.clone())
            .or_else(|| {
                presentation
                    .arrow_text_previews
                    .iter()
                    .find(|(id, _)| *id == text_id)
                    .map(|(_, text)| text.clone())
            })
            .or_else(|| model.text(text_id).ok().cloned())
        else {
            continue;
        };
        text.center = snow_draw_engine_document::arrow_text_anchor(arrow);
        text.rotation = 0.0;
        if let Ok(committed) = model.arrow(arrow_id)
            && committed.opacity > 0.0
            && arrow.opacity != committed.opacity
        {
            text.opacity = (text.opacity * arrow.opacity / committed.opacity).clamp(0.0, 1.0);
        }
        let text_display_id = display_item_id(text_id);
        items.retain(
            |item| !matches!(item, SceneDisplayItem::Text(item) if item.id == text_display_id),
        );
        if !owner.meta.visible {
            continue;
        }
        for item in items.iter_mut() {
            if let SceneDisplayItem::Arrow(item) = item
                && item.id == display_item_id(arrow_id)
                && !text.text.trim().is_empty()
            {
                item.bound_text_id = Some(text_display_id);
                item.label_bounds = Some(text_bounds(&text));
            }
        }
        if !bounds_visible(text_bounds(&text), viewport) {
            continue;
        }
        let item = scene_item_from_text(text_id, text);
        if let Some(index) = items.iter().position(|item| matches!(item, SceneDisplayItem::Arrow(item) if item.id == display_item_id(arrow_id))) {
            items.insert(index + 1, item);
        } else {
            // The arrow itself may be culled while its label is in the viewport.
            let rank = model.paint_rank(arrow_id).unwrap_or(u32::MAX);
            let index = items.iter().position(|item| {
                scene_element_id(item).and_then(|id| model.paint_rank(id)).is_some_and(|r| r > rank)
            }).unwrap_or(items.len());
            items.insert(index, item);
        }
    }
}

fn scene_element_id(item: &SceneDisplayItem) -> Option<ElementId> {
    let id = match item {
        SceneDisplayItem::Arrow(item) => item.id,
        SceneDisplayItem::Text(item) => item.id,
        SceneDisplayItem::Rectangle(item) => item.id,
        SceneDisplayItem::Filter(item) => item.id,
        SceneDisplayItem::Stroke | SceneDisplayItem::Image => return None,
        SceneDisplayItem::SerialNumber(item) => item.id,
        SceneDisplayItem::SerialNumberConnector(item) => item.id,
    };
    Some(ElementId {
        index: id.index,
        generation: id.generation,
    })
}

fn preview_text_items(
    model: &DocumentModel,
    preview_rects: &HashMap<ElementId, RectangleData>,
    active_existing_text: Option<&(ElementId, TextData)>,
) -> HashMap<ElementId, TextData> {
    let mut items: HashMap<ElementId, TextData> = preview_rects
        .iter()
        .filter_map(|(id, rect)| {
            let mut text = model.text(*id).ok()?.clone();
            text.center = rect.center;
            text.width = rect.width;
            text.height = rect.height;
            text.rotation = rect.rotation;
            text.corner_radii = rect.corner_radii;
            text.opacity = rect.opacity;
            Some((*id, text))
        })
        .collect();
    if let Some((id, text)) = active_existing_text {
        items.insert(*id, text.clone());
    }
    items
}

fn preview_serial_items(
    model: &DocumentModel,
    preview_rects: &HashMap<ElementId, RectangleData>,
) -> HashMap<ElementId, SerialNumberData> {
    preview_rects
        .iter()
        .filter_map(|(id, rect)| {
            let serial = model.serial_number(*id).ok()?;
            let mut preview = serial_number_with_selection_rect(serial, *rect);
            preview.opacity = rect.opacity;
            Some((*id, preview))
        })
        .collect()
}

// Serial connectors are decorations of bound text. The renderer paints scene
// items in list order, and the underline centerline sits on the text's painted
// bottom edge, so a text background fill occludes any connector that is emitted
// earlier. This helper is the only emission path: push the scene item first,
// then append connectors when `id` is that bound text.
struct SerialConnectorEmission<'a> {
    model: &'a DocumentModel,
    preview_texts: &'a HashMap<ElementId, TextData>,
    preview_serials: &'a HashMap<ElementId, SerialNumberData>,
    viewport: (f64, f64, f64, f64),
}

impl SerialConnectorEmission<'_> {
    fn push_item(&self, items: &mut Vec<SceneDisplayItem>, id: ElementId, item: SceneDisplayItem) {
        items.push(item);
        self.append_connectors_for_text(items, id);
    }

    fn append_connectors_for_text(&self, items: &mut Vec<SceneDisplayItem>, text_id: ElementId) {
        let Some(text) = self
            .preview_texts
            .get(&text_id)
            .cloned()
            .or_else(|| self.model.text(text_id).ok().cloned())
        else {
            return;
        };

        for serial_id in self.model.serial_number_ids_with_text(text_id) {
            let Some(serial) = self
                .preview_serials
                .get(&serial_id)
                .cloned()
                .or_else(|| self.model.serial_number(serial_id).ok().cloned())
            else {
                continue;
            };
            let Some(connection) = resolve_serial_number_text_connection(&serial, &text) else {
                continue;
            };
            let bounds =
                serial_connector_bounds(&connection, resolve_serial_number_stroke_width(&serial));
            if bounds_visible(bounds, self.viewport) {
                items.push(scene_item_from_serial_connector(
                    serial_id, &serial, connection,
                ));
            }
        }
    }
}

fn serial_bound_text_index(items: &[SceneDisplayItem]) -> HashMap<DisplayItemId, DisplayItemId> {
    items
        .iter()
        .filter_map(|item| match item {
            SceneDisplayItem::SerialNumber(serial) => {
                serial.bound_text_id.map(|text_id| (serial.id, text_id))
            }
            _ => None,
        })
        .collect()
}

fn connector_bound_text_id(
    model: &DocumentModel,
    serial_bound_texts: &HashMap<DisplayItemId, DisplayItemId>,
    connector_id: DisplayItemId,
) -> Option<DisplayItemId> {
    let serial_id = ElementId {
        index: connector_id.index,
        generation: connector_id.generation,
    };
    model
        .bound_text_id_for_serial_number(serial_id)
        .map(display_item_id)
        .or_else(|| serial_bound_texts.get(&connector_id).copied())
}

fn restack_serial_connectors_above_bound_text(
    items: &mut Vec<SceneDisplayItem>,
    model: &DocumentModel,
) {
    let serial_bound_texts = serial_bound_text_index(items);
    let mut connectors_by_text = HashMap::<DisplayItemId, Vec<SceneDisplayItem>>::new();
    let mut rest = Vec::with_capacity(items.len());
    for item in items.drain(..) {
        let SceneDisplayItem::SerialNumberConnector(connector) = &item else {
            rest.push(item);
            continue;
        };
        match connector_bound_text_id(model, &serial_bound_texts, connector.id) {
            Some(text_id) => connectors_by_text.entry(text_id).or_default().push(item),
            None => rest.push(item),
        }
    }

    let connector_count = connectors_by_text.values().map(Vec::len).sum::<usize>();
    let mut stacked = Vec::with_capacity(rest.len() + connector_count);
    for item in rest {
        let text_id = match &item {
            SceneDisplayItem::Text(text) => Some(text.id),
            _ => None,
        };
        stacked.push(item);
        if let Some(text_id) = text_id
            && let Some(connectors) = connectors_by_text.remove(&text_id)
        {
            stacked.extend(connectors);
        }
    }
    stacked.extend(connectors_by_text.into_values().flatten());
    *items = stacked;
}

fn serial_connectors_follow_bound_text(items: &[SceneDisplayItem], model: &DocumentModel) -> bool {
    let serial_bound_texts = serial_bound_text_index(items);
    for (index, item) in items.iter().enumerate() {
        let SceneDisplayItem::SerialNumberConnector(connector) = item else {
            continue;
        };
        let Some(text_display) = connector_bound_text_id(model, &serial_bound_texts, connector.id)
        else {
            continue;
        };
        let Some(previous) = index
            .checked_sub(1)
            .and_then(|previous| items.get(previous))
        else {
            return false;
        };
        match previous {
            SceneDisplayItem::Text(text) if text.id == text_display => {}
            SceneDisplayItem::SerialNumberConnector(previous_connector) => {
                if connector_bound_text_id(model, &serial_bound_texts, previous_connector.id)
                    != Some(text_display)
                {
                    return false;
                }
            }
            _ => return false,
        }
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::{Camera, SurfaceSize};
    use snow_draw_engine_document::{ElementMeta, Transaction};
    use snow_draw_engine_editor::{ActiveTextDraftPresentation, ActiveTextDraftTarget};

    fn assert_close(left: f64, right: f64) {
        assert!(
            (left - right).abs() <= 1e-9,
            "expected {left} to be close to {right}"
        );
    }

    fn default_frame_view() -> FrameView {
        FrameView {
            surface: SurfaceSize {
                width: 1000,
                height: 1000,
            },
            camera: Camera {
                center: Point::new(0.0, 0.0),
                zoom: 1.0,
            },
            clear_color: ColorRgba8::default(),
        }
    }

    #[test]
    fn arrow_text_composition_tracks_preview_gap_and_preserves_paint_order() {
        use snow_draw_engine_core::arrow::{ArrowType, StrokeStyle};
        for kind in [ArrowType::Straight, ArrowType::Curve, ArrowType::Elbow] {
            let owner = ElementId {
                index: 0,
                generation: 1,
            };
            let text_id = ElementId {
                index: 1,
                generation: 1,
            };
            let mut arrow = ArrowData::from_global_points(
                &[Point::new(-200.0, 0.0), Point::new(200.0, 0.0)],
                ColorRgba8::default(),
                2.0,
                StrokeStyle::Solid,
                kind,
                None,
                None,
            )
            .unwrap();
            arrow.text_element_id = Some(text_id);
            let text = TextData {
                text: "visible label".to_owned(),
                width: 150.0,
                height: 60.0,
                ..TextData::default()
            };
            let mut model = DocumentModel::new();
            let mut tx = Transaction::new("arrow and text");
            tx.insert_arrow(owner, ElementMeta::default(), arrow.clone());
            tx.insert_text(text_id, ElementMeta::default(), text.clone());
            model.apply_transaction(tx).unwrap();
            let mut cache = DocumentSceneCache::new();
            cache.sync(&model, None);
            arrow.y += 75.0;
            arrow.opacity *= 0.5;
            let presentation = EditorPresentationState {
                preview_arrows: vec![SelectionArrowState {
                    id: owner,
                    arrow: arrow.clone(),
                }],
                active_text_draft: Some(ActiveTextDraftPresentation {
                    target: ActiveTextDraftTarget::Existing(text_id),
                    revision: 1,
                    text: TextData {
                        width: 210.0,
                        height: 90.0,
                        text: "draft label".to_owned(),
                        ..text
                    },
                }),
                ..EditorPresentationState::default()
            };
            let items = compose_scene_items(&cache, &model, &presentation, default_frame_view());
            assert_eq!(items.len(), 2);
            let SceneDisplayItem::Arrow(display) = &items[0] else {
                panic!("arrow precedes label");
            };
            let SceneDisplayItem::Text(text) = &items[1] else {
                panic!("exactly one label follows arrow");
            };
            assert_eq!(text.text, "draft label");
            assert_eq!(text.center_y, 75.0);
            assert_eq!(text.opacity, 0.5);
            assert_eq!(display.label_bounds.unwrap().min_y, 30.0);
            assert_eq!(display.bound_text_id, Some(display_item_id(text_id)));

            // A narrow view can see the label while the line is entirely above it.
            let mut view = default_frame_view();
            view.surface = SurfaceSize {
                width: 100,
                height: 20,
            };
            view.camera.center = Point::new(0.0, 110.0);
            let items = compose_scene_items(&cache, &model, &presentation, view);
            assert!(
                items
                    .iter()
                    .all(|item| !matches!(item, SceneDisplayItem::Arrow(_)))
            );
            assert_eq!(text_items(&items).len(), 1);
        }
    }

    fn serial_item(items: &[SceneDisplayItem]) -> &SerialNumberDisplayItem {
        items
            .iter()
            .find_map(|item| match item {
                SceneDisplayItem::SerialNumber(serial) => Some(serial),
                _ => None,
            })
            .expect("scene should emit a serial item")
    }

    fn text_items(items: &[SceneDisplayItem]) -> Vec<&TextDisplayItem> {
        items
            .iter()
            .filter_map(|item| match item {
                SceneDisplayItem::Text(text) => Some(text),
                _ => None,
            })
            .collect()
    }

    fn serial_connector_item(items: &[SceneDisplayItem]) -> &SerialNumberConnectorDisplayItem {
        items
            .iter()
            .find_map(|item| match item {
                SceneDisplayItem::SerialNumberConnector(connector) => Some(connector),
                _ => None,
            })
            .expect("scene should emit a serial connector")
    }

    #[test]
    fn active_existing_text_draft_replaces_committed_text_item() {
        let text_id = ElementId {
            index: 0,
            generation: 1,
        };
        let committed = TextData {
            center: Point::new(0.0, 0.0),
            width: 40.0,
            height: 20.0,
            text: "committed".to_owned(),
            ..TextData::default()
        };
        let draft = TextData {
            center: Point::new(180.0, 30.0),
            width: 90.0,
            height: 36.0,
            text: "draft".to_owned(),
            ..committed.clone()
        };

        let mut model = DocumentModel::new();
        let mut transaction = Transaction::new("setup");
        transaction.insert_text(text_id, ElementMeta::default(), committed);
        model.apply_transaction(transaction).unwrap();

        let mut cache = DocumentSceneCache::new();
        cache.sync(&model, None);
        let presentation = EditorPresentationState {
            active_text_draft: Some(ActiveTextDraftPresentation {
                target: ActiveTextDraftTarget::Existing(text_id),
                revision: 7,
                text: draft,
            }),
            ..EditorPresentationState::default()
        };

        let items = compose_scene_items(&cache, &model, &presentation, default_frame_view());
        let texts = text_items(&items);

        assert_eq!(texts.len(), 1);
        assert_eq!(
            texts[0].id,
            DisplayItemId {
                index: text_id.index,
                generation: text_id.generation,
            }
        );
        assert_eq!(texts[0].text, "draft");
        assert_close(texts[0].center_x, 180.0);
        assert_close(texts[0].center_y, 30.0);
        assert_close(texts[0].width, 90.0);
        assert_close(texts[0].height, 36.0);
    }

    #[test]
    fn active_existing_text_draft_connector_uses_draft_text_geometry() {
        let serial_id = ElementId {
            index: 0,
            generation: 1,
        };
        let text_id = ElementId {
            index: 1,
            generation: 1,
        };
        let serial = SerialNumberData {
            center: Point::new(0.0, 0.0),
            diameter: 24.0,
            stroke_width: 2.0,
            text_element_id: Some(text_id),
            ..SerialNumberData::default()
        };
        let committed = TextData {
            center: Point::new(90.0, 0.0),
            width: 40.0,
            height: 20.0,
            ..TextData::default()
        };
        let draft = TextData {
            center: Point::new(220.0, 0.0),
            width: 80.0,
            height: 30.0,
            ..committed.clone()
        };

        let mut model = DocumentModel::new();
        let mut transaction = Transaction::new("setup");
        transaction.insert_serial_number(serial_id, ElementMeta::default(), serial.clone());
        transaction.insert_text(text_id, ElementMeta::default(), committed);
        model.apply_transaction(transaction).unwrap();

        let mut cache = DocumentSceneCache::new();
        cache.sync(&model, None);
        let presentation = EditorPresentationState {
            active_text_draft: Some(ActiveTextDraftPresentation {
                target: ActiveTextDraftTarget::Existing(text_id),
                revision: 3,
                text: draft.clone(),
            }),
            ..EditorPresentationState::default()
        };

        let items = compose_scene_items(&cache, &model, &presentation, default_frame_view());
        let connector = serial_connector_item(&items);
        let expected = resolve_serial_number_text_connection(&serial, &draft).unwrap();

        assert_close(connector.end_x, expected.end.x);
        assert_close(connector.end_y, expected.end.y);
        assert_serial_connectors_follow_bound_text(&items, &model);
    }

    #[test]
    fn active_new_text_draft_emits_synthetic_text_item() {
        let draft = TextData {
            center: Point::new(24.0, 32.0),
            width: 120.0,
            height: 48.0,
            text: "new draft".to_owned(),
            ..TextData::default()
        };
        let presentation = EditorPresentationState {
            active_text_draft: Some(ActiveTextDraftPresentation {
                target: ActiveTextDraftTarget::New,
                revision: 11,
                text: draft,
            }),
            ..EditorPresentationState::default()
        };
        let model = DocumentModel::new();
        let cache = DocumentSceneCache::new();

        let items = compose_scene_items(&cache, &model, &presentation, default_frame_view());
        let texts = text_items(&items);

        assert_eq!(texts.len(), 1);
        assert_eq!(
            texts[0].id,
            DisplayItemId {
                index: u32::MAX,
                generation: 11,
            }
        );
        assert_eq!(texts[0].text, "new draft");
        assert_close(texts[0].center_x, 24.0);
        assert_close(texts[0].center_y, 32.0);
    }

    #[test]
    fn connector_uses_selection_preview_text_position() {
        let serial_id = ElementId {
            index: 0,
            generation: 1,
        };
        let text_id = ElementId {
            index: 1,
            generation: 1,
        };
        let serial = SerialNumberData {
            center: Point::new(0.0, 0.0),
            diameter: 24.0,
            stroke_width: 2.0,
            text_element_id: Some(text_id),
            ..SerialNumberData::default()
        };
        let text = TextData {
            center: Point::new(90.0, 0.0),
            width: 40.0,
            height: 20.0,
            ..TextData::default()
        };

        let mut model = DocumentModel::new();
        let mut transaction = Transaction::new("setup");
        transaction.insert_serial_number(serial_id, ElementMeta::default(), serial.clone());
        transaction.insert_text(text_id, ElementMeta::default(), text.clone());
        model.apply_transaction(transaction).unwrap();

        let mut cache = DocumentSceneCache::new();
        cache.sync(&model, None);

        let preview_rect = RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: Point::new(180.0, 0.0),
            width: text.width,
            height: text.height,
            rotation: text.rotation,
            fill: text.fill,
            fill_style: text.fill_style,
            stroke: text.stroke,
            stroke_width: text.stroke_width,
            stroke_style: StrokeStyle::Solid,
            corner_radii: text.corner_radii,
            opacity: text.opacity,
        };
        let presentation = EditorPresentationState {
            preview_elements: vec![SelectionRectState {
                id: text_id,
                rect: preview_rect,
            }],
            ..EditorPresentationState::default()
        };
        let frame_view = default_frame_view();

        let items = compose_scene_items(&cache, &model, &presentation, frame_view);
        let connector = items
            .iter()
            .find_map(|item| match item {
                SceneDisplayItem::SerialNumberConnector(connector) => Some(connector),
                _ => None,
            })
            .expect("preview text should emit a serial connector");

        let mut preview_text = text;
        preview_text.center = preview_rect.center;
        preview_text.width = preview_rect.width;
        preview_text.height = preview_rect.height;
        let expected = resolve_serial_number_text_connection(&serial, &preview_text).unwrap();

        assert_close(connector.end_x, expected.end.x);
        assert_close(connector.end_y, expected.end.y);
        assert_serial_connectors_follow_bound_text(&items, &model);
    }

    fn filled_bound_text(center: Point<f64>) -> TextData {
        TextData {
            center,
            width: 40.0,
            height: 20.0,
            fill: ColorRgba8 {
                r: 0xff,
                g: 0xff,
                b: 0xff,
                a: 0xff,
            },
            ..TextData::default()
        }
    }

    fn bound_serial(text_id: ElementId, center: Point<f64>) -> SerialNumberData {
        SerialNumberData {
            center,
            diameter: 24.0,
            text_element_id: Some(text_id),
            ..SerialNumberData::default()
        }
    }

    fn compose_default(model: &DocumentModel) -> Vec<SceneDisplayItem> {
        let mut cache = DocumentSceneCache::new();
        cache.sync(model, None);
        compose_scene_items(
            &cache,
            model,
            &EditorPresentationState::default(),
            default_frame_view(),
        )
    }

    fn assert_serial_connectors_follow_bound_text(
        items: &[SceneDisplayItem],
        model: &DocumentModel,
    ) {
        assert!(
            items
                .iter()
                .any(|item| matches!(item, SceneDisplayItem::SerialNumberConnector(_))),
            "scene should emit a serial connector"
        );
        assert!(
            serial_connectors_follow_bound_text(items, model),
            "serial connector must paint immediately above its bound text so text background fills cannot occlude it"
        );
    }

    #[test]
    fn committed_serial_connector_paints_above_filled_text() {
        for text_painted_first in [false, true] {
            let serial_id = ElementId {
                index: 0,
                generation: 1,
            };
            let text_id = ElementId {
                index: 1,
                generation: 1,
            };
            let mut model = DocumentModel::new();
            let mut transaction = Transaction::new("setup");
            transaction.insert_serial_number(
                serial_id,
                ElementMeta::default(),
                bound_serial(text_id, Point::new(0.0, 0.0)),
            );
            transaction.insert_text(
                text_id,
                ElementMeta::default(),
                filled_bound_text(Point::new(90.0, 40.0)),
            );
            model.apply_transaction(transaction).unwrap();
            if text_painted_first {
                let mut reorder = Transaction::new("text below serial");
                reorder.reorder_elements([text_id], 0);
                model.apply_transaction(reorder).unwrap();
            }

            let items = compose_default(&model);
            assert_serial_connectors_follow_bound_text(&items, &model);
        }
    }

    #[test]
    fn restack_moves_preceding_connector_above_bound_text() {
        let serial_id = ElementId {
            index: 0,
            generation: 1,
        };
        let text_id = ElementId {
            index: 1,
            generation: 1,
        };
        let serial = bound_serial(text_id, Point::new(0.0, 0.0));
        let text = filled_bound_text(Point::new(90.0, 40.0));
        let connection = resolve_serial_number_text_connection(&serial, &text).unwrap();

        let mut model = DocumentModel::new();
        let mut transaction = Transaction::new("setup");
        transaction.insert_serial_number(serial_id, ElementMeta::default(), serial.clone());
        transaction.insert_text(text_id, ElementMeta::default(), text.clone());
        model.apply_transaction(transaction).unwrap();

        let mut items = vec![
            scene_item_from_serial_number(serial_id, serial.clone(), Some(text_id)),
            scene_item_from_serial_connector(serial_id, &serial, connection),
            scene_item_from_text(text_id, text),
        ];
        assert!(!serial_connectors_follow_bound_text(&items, &model));
        restack_serial_connectors_above_bound_text(&mut items, &model);
        assert_serial_connectors_follow_bound_text(&items, &model);
        assert!(matches!(
            (&items[1], &items[2]),
            (
                SceneDisplayItem::Text(text),
                SceneDisplayItem::SerialNumberConnector(_)
            ) if text.id == display_item_id(text_id)
        ));
    }

    #[test]
    fn multiple_serials_keep_connectors_above_shared_text() {
        let first_serial_id = ElementId {
            index: 0,
            generation: 1,
        };
        let second_serial_id = ElementId {
            index: 1,
            generation: 1,
        };
        let text_id = ElementId {
            index: 2,
            generation: 1,
        };
        let mut model = DocumentModel::new();
        let mut transaction = Transaction::new("two serials one text");
        transaction.insert_serial_number(
            first_serial_id,
            ElementMeta::default(),
            bound_serial(text_id, Point::new(0.0, 0.0)),
        );
        transaction.insert_serial_number(
            second_serial_id,
            ElementMeta::default(),
            bound_serial(text_id, Point::new(0.0, 80.0)),
        );
        transaction.insert_text(
            text_id,
            ElementMeta::default(),
            filled_bound_text(Point::new(90.0, 40.0)),
        );
        model.apply_transaction(transaction).unwrap();

        let items = compose_default(&model);
        assert_eq!(
            items
                .iter()
                .filter(|item| matches!(item, SceneDisplayItem::SerialNumberConnector(_)))
                .count(),
            2
        );
        assert_serial_connectors_follow_bound_text(&items, &model);
    }

    #[test]
    fn serial_selection_preview_keeps_connector_above_bound_text() {
        let serial_id = ElementId {
            index: 0,
            generation: 1,
        };
        let text_id = ElementId {
            index: 1,
            generation: 1,
        };
        let serial = bound_serial(text_id, Point::new(0.0, 0.0));
        let text = filled_bound_text(Point::new(90.0, 40.0));
        let mut model = DocumentModel::new();
        let mut transaction = Transaction::new("setup");
        transaction.insert_serial_number(serial_id, ElementMeta::default(), serial.clone());
        transaction.insert_text(text_id, ElementMeta::default(), text.clone());
        model.apply_transaction(transaction).unwrap();

        let mut cache = DocumentSceneCache::new();
        cache.sync(&model, None);
        let preview_rect = RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: Point::new(20.0, 30.0),
            width: serial.diameter,
            height: serial.diameter,
            rotation: serial.rotation,
            fill: serial.fill,
            fill_style: serial.fill_style,
            stroke: serial.color,
            stroke_width: serial.stroke_width,
            stroke_style: serial.stroke_style,
            corner_radii: Default::default(),
            opacity: serial.opacity,
        };
        let items = compose_scene_items(
            &cache,
            &model,
            &EditorPresentationState {
                preview_elements: vec![SelectionRectState {
                    id: serial_id,
                    rect: preview_rect,
                }],
                ..EditorPresentationState::default()
            },
            default_frame_view(),
        );
        assert_serial_connectors_follow_bound_text(&items, &model);

        let preview_serial = serial_number_with_selection_rect(&serial, preview_rect);
        let expected = resolve_serial_number_text_connection(&preview_serial, &text).unwrap();
        let connector = serial_connector_item(&items);
        assert_close(connector.end_x, expected.end.x);
        assert_close(connector.end_y, expected.end.y);
    }

    #[test]
    fn arrow_label_restack_does_not_bury_serial_connector() {
        use snow_draw_engine_core::arrow::{ArrowType, StrokeStyle};
        let serial_id = ElementId {
            index: 0,
            generation: 1,
        };
        let serial_text_id = ElementId {
            index: 1,
            generation: 1,
        };
        let arrow_id = ElementId {
            index: 2,
            generation: 1,
        };
        let arrow_text_id = ElementId {
            index: 3,
            generation: 1,
        };
        let mut arrow = ArrowData::from_global_points(
            &[Point::new(-200.0, 120.0), Point::new(200.0, 120.0)],
            ColorRgba8::default(),
            2.0,
            StrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )
        .unwrap();
        arrow.text_element_id = Some(arrow_text_id);
        let mut model = DocumentModel::new();
        let mut transaction = Transaction::new("serial and arrow labels");
        transaction.insert_serial_number(
            serial_id,
            ElementMeta::default(),
            bound_serial(serial_text_id, Point::new(0.0, 0.0)),
        );
        transaction.insert_text(
            serial_text_id,
            ElementMeta::default(),
            filled_bound_text(Point::new(90.0, 40.0)),
        );
        transaction.insert_arrow(arrow_id, ElementMeta::default(), arrow);
        transaction.insert_text(
            arrow_text_id,
            ElementMeta::default(),
            TextData {
                text: "arrow label".to_owned(),
                width: 150.0,
                height: 60.0,
                ..TextData::default()
            },
        );
        model.apply_transaction(transaction).unwrap();

        let items = compose_default(&model);
        assert_serial_connectors_follow_bound_text(&items, &model);
        let arrow_position = items
            .iter()
            .position(|item| matches!(item, SceneDisplayItem::Arrow(_)))
            .expect("scene should emit the arrow");
        let label_position = items
            .iter()
            .position(|item| {
                matches!(
                    item,
                    SceneDisplayItem::Text(text) if text.id == display_item_id(arrow_text_id)
                )
            })
            .expect("scene should emit the arrow label");
        assert_eq!(label_position, arrow_position + 1);
    }

    #[test]
    fn serial_preview_uses_committed_resize_transform() {
        let serial_id = ElementId {
            index: 0,
            generation: 1,
        };
        let serial = SerialNumberData {
            center: Point::new(0.0, 0.0),
            diameter: 40.0,
            font_size: 16.0,
            ..SerialNumberData::default()
        };

        let mut model = DocumentModel::new();
        let mut transaction = Transaction::new("setup");
        transaction.insert_serial_number(serial_id, ElementMeta::default(), serial);
        model.apply_transaction(transaction).unwrap();

        let mut cache = DocumentSceneCache::new();
        cache.sync(&model, None);

        let presentation = EditorPresentationState {
            preview_elements: vec![SelectionRectState {
                id: serial_id,
                rect: RectangleData {
                    rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
                    highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
                    center: Point::new(10.0, 20.0),
                    width: 80.0,
                    height: 100.0,
                    rotation: 0.25,
                    fill: ColorRgba8::default(),
                    fill_style: FillStyle::Solid,
                    stroke: ColorRgba8::default(),
                    stroke_width: 0.0,
                    stroke_style: StrokeStyle::Solid,
                    corner_radii: Default::default(),
                    opacity: 1.0,
                },
            }],
            ..EditorPresentationState::default()
        };
        let frame_view = default_frame_view();

        let items = compose_scene_items(&cache, &model, &presentation, frame_view);
        let preview = items
            .iter()
            .find_map(|item| match item {
                SceneDisplayItem::SerialNumber(serial) => Some(serial),
                _ => None,
            })
            .expect("serial resize preview should emit a serial item");

        assert_close(preview.center_x, 10.0);
        assert_close(preview.center_y, 20.0);
        assert_close(preview.diameter, 80.0);
        assert_close(preview.font_size, 32.0);
    }

    #[test]
    fn selection_previews_preserve_text_and_serial_opacity() {
        let text_id = ElementId {
            index: 0,
            generation: 1,
        };
        let serial_id = ElementId {
            index: 1,
            generation: 1,
        };
        let mut model = DocumentModel::new();
        let mut transaction = Transaction::new("setup");
        transaction.insert_text(
            text_id,
            ElementMeta::default(),
            TextData {
                center: Point::new(-40.0, 0.0),
                width: 60.0,
                height: 24.0,
                opacity: 0.8,
                ..TextData::default()
            },
        );
        transaction.insert_serial_number(
            serial_id,
            ElementMeta::default(),
            SerialNumberData {
                center: Point::new(40.0, 0.0),
                diameter: 24.0,
                opacity: 0.8,
                ..SerialNumberData::default()
            },
        );
        model.apply_transaction(transaction).unwrap();

        let mut text_preview = model.element_rect_proxy(text_id).unwrap();
        text_preview.opacity = 0.4;
        let mut serial_preview = model.element_rect_proxy(serial_id).unwrap();
        serial_preview.opacity = 0.4;
        let presentation = EditorPresentationState {
            preview_elements: vec![
                SelectionRectState {
                    id: text_id,
                    rect: text_preview,
                },
                SelectionRectState {
                    id: serial_id,
                    rect: serial_preview,
                },
            ],
            ..EditorPresentationState::default()
        };
        let mut cache = DocumentSceneCache::new();
        cache.sync(&model, None);

        let items = compose_scene_items(&cache, &model, &presentation, default_frame_view());
        let text = text_items(&items)
            .into_iter()
            .find(|item| item.id.index == text_id.index && item.id.generation == text_id.generation)
            .expect("scene should emit the text preview");
        let serial = items
            .iter()
            .find_map(|item| match item {
                SceneDisplayItem::SerialNumber(item)
                    if item.id.index == serial_id.index
                        && item.id.generation == serial_id.generation =>
                {
                    Some(item)
                }
                _ => None,
            })
            .expect("scene should emit the serial-number preview");

        assert_close(text.opacity, 0.4);
        assert_close(serial.opacity, 0.4);
    }

    #[test]
    fn serial_item_omits_missing_bound_text_id() {
        let serial_id = ElementId {
            index: 0,
            generation: 1,
        };
        let missing_text_id = ElementId {
            index: 1,
            generation: 1,
        };
        let serial = SerialNumberData {
            center: Point::new(0.0, 0.0),
            diameter: 24.0,
            text_element_id: Some(missing_text_id),
            ..SerialNumberData::default()
        };

        let mut model = DocumentModel::new();
        let mut transaction = Transaction::new("setup");
        transaction.insert_serial_number(serial_id, ElementMeta::default(), serial);
        model.apply_transaction(transaction).unwrap();

        let mut cache = DocumentSceneCache::new();
        cache.sync(&model, None);

        let items = compose_scene_items(
            &cache,
            &model,
            &EditorPresentationState::default(),
            default_frame_view(),
        );

        assert_eq!(serial_item(&items).bound_text_id, None);
    }

    #[test]
    fn serial_item_clears_cached_bound_text_id_after_text_removal() {
        let serial_id = ElementId {
            index: 0,
            generation: 1,
        };
        let text_id = ElementId {
            index: 1,
            generation: 1,
        };
        let serial = SerialNumberData {
            center: Point::new(0.0, 0.0),
            diameter: 24.0,
            text_element_id: Some(text_id),
            ..SerialNumberData::default()
        };
        let text = TextData {
            center: Point::new(90.0, 0.0),
            width: 40.0,
            height: 20.0,
            ..TextData::default()
        };

        let mut model = DocumentModel::new();
        let mut transaction = Transaction::new("setup");
        transaction.insert_serial_number(serial_id, ElementMeta::default(), serial);
        transaction.insert_text(text_id, ElementMeta::default(), text);
        model.apply_transaction(transaction).unwrap();

        let mut cache = DocumentSceneCache::new();
        cache.sync(&model, None);
        let items = compose_scene_items(
            &cache,
            &model,
            &EditorPresentationState::default(),
            default_frame_view(),
        );
        assert_eq!(
            serial_item(&items).bound_text_id,
            Some(DisplayItemId {
                index: text_id.index,
                generation: text_id.generation,
            })
        );

        let mut transaction = Transaction::new("remove text only");
        transaction.remove_element(text_id);
        let result = model.apply_transaction(transaction).unwrap();
        cache.sync(&model, Some(&result.changes));

        let items = compose_scene_items(
            &cache,
            &model,
            &EditorPresentationState::default(),
            default_frame_view(),
        );

        assert_eq!(serial_item(&items).bound_text_id, None);
    }

    #[test]
    fn offscreen_committed_item_is_emitted_when_preview_moves_into_viewport() {
        let id = ElementId {
            index: 0,
            generation: 1,
        };
        let committed = RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: Point::new(2000.0, 0.0),
            width: 80.0,
            height: 60.0,
            rotation: 0.0,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        };
        let preview = RectangleData {
            center: Point::new(0.0, 0.0),
            ..committed
        };
        let mut model = DocumentModel::new();
        let mut transaction = Transaction::new("offscreen preview");
        transaction.insert_rectangle(id, ElementMeta::default(), committed);
        model.apply_transaction(transaction).unwrap();
        let mut cache = DocumentSceneCache::new();
        cache.sync(&model, None);

        let items = compose_scene_items(
            &cache,
            &model,
            &EditorPresentationState {
                preview_elements: vec![SelectionRectState { id, rect: preview }],
                ..EditorPresentationState::default()
            },
            default_frame_view(),
        );
        let rectangle = items.iter().find_map(|item| match item {
            SceneDisplayItem::Rectangle(item) => Some(item),
            _ => None,
        });
        assert_eq!(rectangle.map(|item| item.center_x), Some(0.0));
    }
}
