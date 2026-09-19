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
    let preview_text_paints: HashMap<_, _> = presentation
        .preview_text_paints
        .iter()
        .copied()
        .map(|preview| (preview.id, preview))
        .collect();
    let active_existing_text = presentation
        .active_text_draft
        .as_ref()
        .and_then(|draft| draft.existing_id().map(|id| (id, draft.text.clone())));
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
        preview_serials: &preview_serials,
        viewport,
    };

    let mut present: std::collections::HashSet<_> = ordered_ids.iter().copied().collect();
    let mut extra: Vec<_> = preview_rects
        .keys()
        .chain(preview_arrows.keys())
        .chain(active_existing_text.iter().map(|(id, _)| id))
        .copied()
        .filter(|id| model.paint_rank(*id).is_some() && present.insert(*id))
        .collect();
    extra.sort_unstable_by_key(|id| model.paint_rank(*id));
    if !extra.is_empty() {
        let mut merged = Vec::with_capacity(ordered_ids.len() + extra.len());
        let mut extra = extra.into_iter().peekable();
        for id in ordered_ids {
            while extra
                .peek()
                .is_some_and(|next| model.paint_rank(*next) < model.paint_rank(id))
            {
                merged.push(extra.next().unwrap());
            }
            merged.push(id);
        }
        merged.extend(extra);
        ordered_ids = merged;
    }

    for id in &ordered_ids {
        if let Some((active_id, active_text)) = active_existing_text.as_ref()
            && id == active_id
        {
            emitted_preview_ids.insert(*id, true);
            if bounds_visible(text_bounds(active_text), viewport) {
                items.push(scene_item_from_text(*id, active_text.clone()));
            }
            continue;
        }
        if let Some(preview_rect) = preview_rects.get(id) {
            emitted_preview_ids.insert(*id, true);
            if let Some((item, bounds)) = scene_item_from_selection_preview(
                model,
                *id,
                *preview_rect,
                preview_text_paints.get(id).copied(),
            ) && bounds_visible(bounds, viewport)
            {
                items.push(item);
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
            items.push(if matches!(item, SceneDisplayItem::SerialNumber(_)) {
                scene_item_with_serial_bound_text(
                    item.clone(),
                    model.bound_text_id_for_serial_number(*id),
                )
            } else {
                item.clone()
            });
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
            preview_text_paints.get(&preview.id).copied(),
        ) && bounds_visible(bounds, viewport)
        {
            items.push(item);
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
    compose_arrow_text(&mut items, model, presentation, &preview_arrows, viewport);
    serial_connectors.append_from_displayed_items(&mut items);
    if let Some(copy) = duplicate_preview_scene(cache, presentation) {
        let mut copies = compose_scene_items(
            &copy.cache,
            &copy.model,
            &EditorPresentationState::default(),
            frame_view,
        );
        for item in &mut copies {
            remap_copy_display_ids(item, &copy.ids);
        }
        items.extend(copies);
    }
    if presentation.creation_preview.is_some() || presentation.duplicate_preview.is_some() {
        // Smart Erase previews keep the fixed bottom layer.
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
    // Connector parts are emitted immediately after their bound text.
    debug_assert!(
        serial_connectors_follow_bound_text(&items, model),
        "serial connectors must paint immediately above their bound text"
    );
    items
}

pub(crate) fn compose_scene_render_plan(
    cache: &DocumentSceneCache,
    model: &DocumentModel,
    presentation: &EditorPresentationState,
    items: &[SceneDisplayItem],
) -> Vec<snow_draw_engine_display::SceneRenderRun> {
    use crate::scene_order::{OrderNode, SceneOrderPlan};
    let mut overrides = HashMap::new();
    for preview in &presentation.preview_elements {
        let node = scene_item_from_selection_preview(model, preview.id, preview.rect, None)
            .map(|(item, _)| OrderNode::new(preview.id, &item));
        overrides.insert(preview.id, node);
    }
    for preview in &presentation.preview_arrows {
        overrides.insert(
            preview.id,
            (!arrow_is_degenerate(&preview.arrow)).then_some(OrderNode {
                id: preview.id,
                effect: None,
                smart_erase: false,
            }),
        );
    }
    if let Some(draft) = &presentation.active_text_draft
        && let Some(id) = draft.existing_id()
    {
        overrides.insert(
            id,
            Some(OrderNode {
                id,
                effect: None,
                smart_erase: false,
            }),
        );
    }
    if presentation.creation_preview.is_none()
        && presentation.duplicate_preview.is_none()
        && presentation
            .active_text_draft
            .as_ref()
            .is_none_or(|draft| draft.existing_id().is_some())
        && overrides
            .iter()
            .all(|(id, node)| cache.order_plan.node(*id) == node.as_ref())
    {
        return cache.order_plan.project(items);
    }
    let mut nodes = Vec::with_capacity(cache.order_plan.nodes.len() + overrides.len());
    for node in &cache.order_plan.nodes {
        if let Some(replacement) = overrides.remove(&node.id) {
            nodes.extend(replacement);
        } else {
            nodes.push(*node);
        }
    }
    if !overrides.is_empty() {
        nodes.extend(overrides.into_values().flatten());
        nodes.sort_by_key(|node| model.paint_rank(node.id).unwrap_or(u32::MAX));
    }
    if let Some(preview) = &presentation.creation_preview {
        let id = model.peek_next_element_id();
        let item = match preview {
            ElementCreationPreview::Filter(filter) => Some(scene_item_from_filter(id, *filter)),
            ElementCreationPreview::PenFilter(filter) => {
                scene_item_from_pen_filter_preview(id, filter).map(|(item, _)| item)
            }
            ElementCreationPreview::Arrow(arrow) if arrow_is_degenerate(arrow) => None,
            ElementCreationPreview::Rectangle(rect) if rect.is_spotlight() => None,
            _ => Some(SceneDisplayItem::Image),
        };
        if let Some(item) = item {
            nodes.push(OrderNode::new(id, &item));
        }
    }
    if let Some(draft) = &presentation.active_text_draft
        && draft.existing_id().is_none()
    {
        let node = OrderNode {
            id: draft.display_id(),
            effect: None,
            smart_erase: false,
        };
        if let snow_draw_engine_editor::ActiveTextDraftTarget::NewArrow(owner) = draft.target {
            if model
                .element(owner)
                .is_ok_and(|element| element.meta.visible)
                && let Some(rank) = model.paint_rank(owner)
            {
                let position = nodes
                    .iter()
                    .position(|node| model.paint_rank(node.id).is_none_or(|r| r > rank))
                    .unwrap_or(nodes.len());
                nodes.insert(position, node);
            }
        } else {
            nodes.push(node);
        }
    }
    if let Some(copy) = duplicate_preview_scene(cache, presentation) {
        nodes.extend(copy.cache.order_plan.nodes.iter().map(|node| OrderNode {
            id: copy.ids[node.id.index as usize],
            effect: node.effect,
            smart_erase: node.smart_erase,
        }));
    }
    // Smart Erase is a drawable at the bottom, not an ordinary filter source run.
    // Use the document/preview type, including offscreen nodes, to place it.
    nodes.sort_by_key(|node| {
        if node.smart_erase {
            (0, node.id.index)
        } else {
            (1, 0)
        }
    });
    let mut retained = cache.preview_order_plan.lock().expect("preview plan lock");
    if retained.nodes != nodes {
        *retained = SceneOrderPlan::new(nodes);
        cache
            .preview_order_builds
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    }
    retained.project(items)
}

#[derive(Debug)]
pub(crate) struct DuplicateScene {
    pub model: DocumentModel,
    pub cache: DocumentSceneCache,
    pub ids: Vec<ElementId>,
    transaction: snow_draw_engine_document::Transaction,
}

// Build only the copied subset, never clone the full document. This keeps links,
// arrow labels and serial connectors on the normal scene composition path.
pub(crate) fn duplicate_preview_scene(
    scene_cache: &DocumentSceneCache,
    presentation: &EditorPresentationState,
) -> Option<std::rc::Rc<DuplicateScene>> {
    use snow_draw_engine_document::{Operation, Transaction};
    let Some(transaction) = presentation.duplicate_preview.as_ref() else {
        *scene_cache
            .duplicate_scene
            .lock()
            .expect("duplicate scene lock") = None;
        return None;
    };
    let mut retained = scene_cache
        .duplicate_scene
        .lock()
        .expect("duplicate scene lock");
    if let Some(copy) = retained.as_ref()
        && copy.transaction == *transaction
    {
        return Some(copy.clone());
    }
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
    if let Some(previous) = retained.as_mut().and_then(std::rc::Rc::get_mut)
        && previous.ids == ids
    {
        let mut update = Transaction::new("move copy preview");
        for op in compact.operations() {
            if let Operation::InsertElement { id, meta, data } = op {
                let current = previous.model.element(*id).ok()?;
                if current.data != *data {
                    update.push(Operation::UpdateElementData {
                        id: *id,
                        data: data.clone(),
                    });
                }
                if current.meta != *meta {
                    update.push(Operation::UpdateElementMeta {
                        id: *id,
                        meta: *meta,
                    });
                }
            }
        }
        if !update.is_empty() {
            let result = previous.model.apply_transaction(update).ok()?;
            previous.cache.sync(&previous.model, Some(&result.changes));
        }
        previous.transaction = transaction.clone();
    } else {
        let mut model = DocumentModel::new();
        model.apply_transaction(compact).ok()?;
        let mut cache = DocumentSceneCache::new();
        cache.sync(&model, None);
        *retained = Some(std::rc::Rc::new(DuplicateScene {
            model,
            cache,
            ids,
            transaction: transaction.clone(),
        }));
    }
    retained.clone()
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
    let mut bindings = model.arrow_label_bindings().to_vec();
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
    let label_ids: std::collections::HashSet<_> = bindings
        .iter()
        .map(|(_, id)| display_item_id(*id))
        .collect();
    items.retain(
        |item| !matches!(item, SceneDisplayItem::Text(text) if label_ids.contains(&text.id)),
    );
    let arrow_positions: HashMap<_, _> = items
        .iter()
        .enumerate()
        .filter_map(|(index, item)| {
            if let SceneDisplayItem::Arrow(arrow) = item {
                Some((arrow.id, index))
            } else {
                None
            }
        })
        .collect();
    let mut labels = Vec::with_capacity(bindings.len());
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
        if !owner.meta.visible {
            continue;
        }
        if !text.text.trim().is_empty()
            && let Some(&index) = arrow_positions.get(&display_item_id(arrow_id))
            && let SceneDisplayItem::Arrow(item) = &mut items[index]
        {
            item.bound_text_id = Some(text_display_id);
            item.label_bounds = Some(text_bounds(&text));
        }
        if !bounds_visible(text_bounds(&text), viewport) {
            continue;
        }
        labels.push((
            model.paint_rank(arrow_id).unwrap_or(u32::MAX),
            scene_item_from_text(text_id, text),
        ));
    }
    labels.sort_by_key(|(rank, _)| *rank);
    let mut labels = labels.into_iter().peekable();
    let mut expanded = Vec::with_capacity(items.len() + labels.len());
    for item in items.drain(..) {
        let rank = scene_element_id(&item)
            .and_then(|id| model.paint_rank(id))
            .unwrap_or(u32::MAX);
        while labels
            .peek()
            .is_some_and(|(label_rank, _)| *label_rank < rank)
        {
            expanded.push(labels.next().unwrap().1);
        }
        expanded.push(item);
        while labels
            .peek()
            .is_some_and(|(label_rank, _)| *label_rank == rank)
        {
            expanded.push(labels.next().unwrap().1);
        }
    }
    expanded.extend(labels.map(|(_, item)| item));
    *items = expanded;
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
// earlier. Connectors are derived after every scene item has been emitted, from
// those displayed items — never from a separately rebuilt document element.
struct SerialConnectorEmission<'a> {
    model: &'a DocumentModel,
    preview_serials: &'a HashMap<ElementId, SerialNumberData>,
    viewport: (f64, f64, f64, f64),
}

#[derive(Clone, Copy)]
struct PresentedSerial {
    geometry: SerialPaintGeometry,
    color: ColorRgba8,
    opacity: f64,
}

impl SerialConnectorEmission<'_> {
    /// One pass over the emitted items: index the displayed serial badges and
    /// collect the displayed texts, then derive each text's connectors from
    /// that index. A badge culled out of the viewport falls back to preview or
    /// committed data so a connector that spans into view is still emitted.
    fn append_from_displayed_items(&self, items: &mut Vec<SceneDisplayItem>) {
        let mut displayed_serials = HashMap::<DisplayItemId, PresentedSerial>::new();
        for item in items.iter() {
            if let SceneDisplayItem::SerialNumber(serial) = item {
                displayed_serials.insert(
                    serial.id,
                    PresentedSerial {
                        geometry: serial_paint_geometry_from_display_item(serial),
                        color: serial.color,
                        opacity: serial.opacity,
                    },
                );
            }
        }
        let mut expanded = Vec::with_capacity(items.len());
        for item in items.drain(..) {
            let text = match &item {
                SceneDisplayItem::Text(text) => Some((
                    ElementId {
                        index: text.id.index,
                        generation: text.id.generation,
                    },
                    text_paint_geometry_from_display_item(text),
                )),
                _ => None,
            };
            expanded.push(item);
            if let Some((id, geometry)) = text {
                self.append_connectors_for_text(&mut expanded, &displayed_serials, id, &geometry);
            }
        }
        *items = expanded;
    }

    fn presented_serial(
        &self,
        displayed_serials: &HashMap<DisplayItemId, PresentedSerial>,
        serial_id: ElementId,
    ) -> Option<PresentedSerial> {
        if let Some(serial) = displayed_serials.get(&display_item_id(serial_id)) {
            return Some(*serial);
        }
        let serial = self
            .preview_serials
            .get(&serial_id)
            .cloned()
            .or_else(|| self.model.serial_number(serial_id).ok().cloned())?;
        Some(PresentedSerial {
            geometry: SerialPaintGeometry::from_serial(&serial),
            color: serial.color,
            opacity: serial.opacity,
        })
    }

    fn append_connectors_for_text(
        &self,
        items: &mut Vec<SceneDisplayItem>,
        displayed_serials: &HashMap<DisplayItemId, PresentedSerial>,
        text_id: ElementId,
        text: &TextPaintGeometry,
    ) {
        for &serial_id in self.model.serials_for_text(text_id) {
            let Some(serial) = self.presented_serial(displayed_serials, serial_id) else {
                continue;
            };
            let Some(connection) = resolve_serial_paint_text_connection(&serial.geometry, text)
            else {
                continue;
            };
            let bounds = serial_connector_bounds(&connection, serial.geometry.stroke_width);
            if bounds_visible(bounds, self.viewport) {
                items.push(scene_item_from_serial_connector_paint(
                    serial_id,
                    serial.color,
                    serial.geometry.stroke_width,
                    serial.opacity,
                    connection,
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
    use snow_draw_engine_document::{
        ElementMeta, InkBox, TextLayoutSize, Transaction, resolve_serial_number_text_connection,
    };
    use snow_draw_engine_editor::{
        ActiveTextDraftPresentation, ActiveTextDraftTarget, TextPreviewPaint,
    };

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
                layout: TextLayoutSize::new(150.0, 60.0),
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
                        text: "draft label".to_owned(),
                        layout: TextLayoutSize::new(210.0, 90.0),
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

    fn displayed_text(items: &[SceneDisplayItem], text_id: ElementId) -> &TextDisplayItem {
        text_items(items)
            .into_iter()
            .find(|item| item.id == display_item_id(text_id))
            .expect("scene should emit the bound text")
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
            text: "committed".to_owned(),
            layout: TextLayoutSize::new(40.0, 20.0),
            ..TextData::default()
        };
        let draft = TextData {
            center: Point::new(180.0, 30.0),
            text: "draft".to_owned(),
            layout: TextLayoutSize::new(90.0, 36.0),
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
            layout: TextLayoutSize::new(40.0, 20.0),
            ..TextData::default()
        };
        let draft = TextData {
            center: Point::new(220.0, 0.0),
            layout: TextLayoutSize::new(80.0, 30.0),
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
        let expected = resolve_serial_number_text_connection(&serial, &draft).unwrap();
        let connector = serial_connector_item(&items);

        assert_close(connector.end_x, expected.end.x);
        assert_close(connector.end_y, expected.end.y);
        assert_serial_connectors_follow_bound_text(&items, &model);
    }

    #[test]
    fn connector_tracks_preview_text_paint_bounds_during_resize() {
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
            text: "note".to_owned(),
            font_size: 20.0,
            ..filled_bound_text(Point::new(90.0, 0.0))
        };
        let mut model = DocumentModel::new();
        let mut transaction = Transaction::new("setup");
        transaction.insert_serial_number(serial_id, ElementMeta::default(), serial.clone());
        transaction.insert_text(text_id, ElementMeta::default(), committed);
        model.apply_transaction(transaction).unwrap();
        let mut cache = DocumentSceneCache::new();
        cache.sync(&model, None);

        // A live resize drag: the preview rect carries the new dimensions and
        // the font-size preview carries the scaled font the renderer paints.
        let presentation = EditorPresentationState {
            preview_elements: vec![SelectionRectState {
                id: text_id,
                rect: RectangleData {
                    rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
                    highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
                    center: Point::new(130.0, 10.0),
                    width: 80.0,
                    height: 40.0,
                    rotation: 0.0,
                    fill: ColorRgba8::default(),
                    fill_style: FillStyle::Solid,
                    stroke: ColorRgba8::default(),
                    stroke_width: 0.0,
                    stroke_style: StrokeStyle::Solid,
                    corner_radii: CornerRadii::default(),
                    opacity: 1.0,
                },
            }],
            preview_text_paints: vec![TextPreviewPaint {
                id: text_id,
                font_size: Some(40.0),
                ink: None,
            }],
            ..EditorPresentationState::default()
        };

        let items = compose_scene_items(&cache, &model, &presentation, default_frame_view());
        let text = displayed_text(&items, text_id);
        assert_close(text.font_size, 40.0);
        assert_close(text.width, 80.0);
        assert_close(text.height, 40.0);

        // Independent of the production adapter: the connector anchors on the
        // aligned ink box and ignores the background fill padding.
        let connector = serial_connector_item(&items);
        assert_close(connector.baseline_start_x, 130.0 - 40.0);
        assert_close(connector.baseline_end_x, 130.0 + 40.0);
        assert_close(connector.baseline_start_y, 10.0 + 20.0);
        assert_close(connector.baseline_end_y, 10.0 + 20.0);

        let expected = resolve_serial_paint_text_connection(
            &SerialPaintGeometry::from_serial(&serial),
            &text_paint_geometry_from_display_item(text),
        )
        .unwrap();
        assert_close(connector.end_x, expected.end.x);
        assert_close(connector.end_y, expected.end.y);
        assert_serial_connectors_follow_bound_text(&items, &model);
    }

    #[test]
    fn connector_tracks_measured_ink_during_width_only_resize() {
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
        let mut committed = filled_bound_text(Point::new(120.0, 0.0));
        committed.layout = TextLayoutSize::with_content(160.0, 20.0, 150.0, 20.0);
        committed.font_size = 20.0;
        committed.horizontal_align = snow_draw_engine_document::TextHorizontalAlign::Left;
        committed.vertical_align = snow_draw_engine_document::TextVerticalAlign::Top;
        committed.text = "a wrapped annotation note".to_owned();
        let mut model = DocumentModel::new();
        let mut transaction = Transaction::new("setup");
        transaction.insert_serial_number(serial_id, ElementMeta::default(), serial);
        transaction.insert_text(text_id, ElementMeta::default(), committed);
        model.apply_transaction(transaction).unwrap();
        let mut cache = DocumentSceneCache::new();
        cache.sync(&model, None);

        // Width-only wrap: the font stays put, the host re-measures the ink,
        // and the preview rect adopts the wrapped height. Connectors must use
        // that measured ink, not the committed 150-wide one-line box.
        let presentation = EditorPresentationState {
            preview_elements: vec![SelectionRectState {
                id: text_id,
                rect: RectangleData {
                    rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
                    highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
                    center: Point::new(120.0, 10.0),
                    width: 80.0,
                    height: 40.0,
                    rotation: 0.0,
                    fill: ColorRgba8::default(),
                    fill_style: FillStyle::Solid,
                    stroke: ColorRgba8::default(),
                    stroke_width: 0.0,
                    stroke_style: StrokeStyle::Solid,
                    corner_radii: CornerRadii::default(),
                    opacity: 1.0,
                },
            }],
            preview_text_paints: vec![TextPreviewPaint {
                id: text_id,
                font_size: None,
                ink: InkBox::new(40.0, 40.0),
            }],
            ..EditorPresentationState::default()
        };

        let items = compose_scene_items(&cache, &model, &presentation, default_frame_view());
        let text = displayed_text(&items, text_id);
        assert_close(text.font_size, 20.0);
        assert_close(text.width, 80.0);
        assert_close(text.height, 40.0);
        assert_close(text.content_width, 40.0);
        assert_close(text.content_height, 40.0);

        let connector = serial_connector_item(&items);
        let ink_left = 120.0 - 40.0;
        let ink_bottom = 10.0 - 20.0 + 40.0;
        assert_close(connector.baseline_start_x, ink_left);
        assert_close(connector.baseline_end_x, ink_left + 40.0);
        assert_close(connector.baseline_start_y, ink_bottom);
        assert!(
            connector.baseline_end_x < 120.0 + 40.0,
            "underline must not span the wrap rectangle or the committed one-line ink"
        );
        assert_serial_connectors_follow_bound_text(&items, &model);
    }

    #[test]
    fn active_new_text_draft_emits_synthetic_text_item() {
        let draft = TextData {
            center: Point::new(24.0, 32.0),
            text: "new draft".to_owned(),
            layout: TextLayoutSize::new(120.0, 48.0),
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
            layout: TextLayoutSize::new(40.0, 20.0),
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
            width: text.width(),
            height: text.height(),
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
        let mut preview_text = text;
        preview_text.center = preview_rect.center;
        preview_text.layout = preview_text
            .layout
            .with_wrap(preview_rect.width, preview_rect.height);
        let expected = resolve_serial_number_text_connection(&serial, &preview_text).unwrap();
        let connector = serial_connector_item(&items);

        assert_close(connector.end_x, expected.end.x);
        assert_close(connector.end_y, expected.end.y);
        assert_serial_connectors_follow_bound_text(&items, &model);
    }

    fn filled_bound_text(center: Point<f64>) -> TextData {
        TextData {
            center,
            layout: TextLayoutSize::new(40.0, 20.0),
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
    fn connector_spans_into_view_when_serial_badge_is_culled() {
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
            bound_serial(text_id, Point::new(-600.0, 0.0)),
        );
        transaction.insert_text(
            text_id,
            ElementMeta::default(),
            filled_bound_text(Point::new(0.0, 0.0)),
        );
        model.apply_transaction(transaction).unwrap();

        let items = compose_default(&model);
        // The default viewport is (-500, -500, 500, 500): the badge is culled
        // while its bound text stays visible, so the connector geometry can only
        // come from the committed serial fallback.
        assert!(
            items
                .iter()
                .all(|item| !matches!(item, SceneDisplayItem::SerialNumber(_))),
            "badge centered at x = -600 lies outside the viewport"
        );
        let connector = serial_connector_item(&items);
        assert!(
            connector.start_x < -500.0 && connector.end_x > -500.0,
            "connector must still be emitted because it spans into the viewport"
        );
        assert_serial_connectors_follow_bound_text(&items, &model);
    }

    #[test]
    fn connector_underlines_wrapped_ink_not_wrap_rectangle() {
        let serial_id = ElementId {
            index: 0,
            generation: 1,
        };
        let text_id = ElementId {
            index: 1,
            generation: 1,
        };
        let mut text = filled_bound_text(Point::new(120.0, 0.0));
        // A width-resized label: wrap rectangle 80×40, wrapped ink 40×20
        // hugging the top-left.
        text.layout = TextLayoutSize::with_content(80.0, 40.0, 40.0, 20.0);
        text.horizontal_align = snow_draw_engine_document::TextHorizontalAlign::Left;
        text.vertical_align = snow_draw_engine_document::TextVerticalAlign::Top;
        let mut model = DocumentModel::new();
        let mut transaction = Transaction::new("setup");
        transaction.insert_serial_number(
            serial_id,
            ElementMeta::default(),
            bound_serial(text_id, Point::new(0.0, 0.0)),
        );
        transaction.insert_text(text_id, ElementMeta::default(), text);
        model.apply_transaction(transaction).unwrap();

        let items = compose_default(&model);
        let connector = serial_connector_item(&items);
        // Top-aligned ink: the underline sits at item top + ink height, not at
        // the wrap rectangle's bottom.
        let ink_bottom = 0.0 - 20.0 + 20.0;
        assert_close(connector.baseline_start_y, ink_bottom);
        // Left-aligned 40-wide ink inside the 80-wide wrap rectangle: the
        // underline ends at the ink's right edge, not the wrap rectangle's.
        assert_close(connector.baseline_start_x, 120.0 - 40.0);
        assert_close(connector.baseline_end_x, 120.0 - 40.0 + 40.0);
        assert_serial_connectors_follow_bound_text(&items, &model);
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
                layout: TextLayoutSize::new(150.0, 60.0),
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
                opacity: 0.8,
                layout: TextLayoutSize::new(60.0, 24.0),
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
            layout: TextLayoutSize::new(40.0, 20.0),
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
