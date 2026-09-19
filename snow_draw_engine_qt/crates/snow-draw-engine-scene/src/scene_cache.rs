use crate::scene_order::{OrderNode, SceneOrderPlan};
use std::collections::HashMap;

use snow_draw_engine_core::DrawRect;
use snow_draw_engine_display::{DisplaySpotlightCutout, SceneDisplayItem};
use snow_draw_engine_document::{
    DocumentDelta, DocumentRevision, ElementData, ElementId, arrow_bounds, arrow_is_degenerate,
    filter_bounds, free_draw_bounds, pen_filter_bounds, serial_number_bounds, text_bounds,
};
use snow_draw_engine_model::DocumentModel;

use crate::{
    item_conversions::{
        scene_item_from_arrow_revision, scene_item_from_filter, scene_item_from_free_draw,
        scene_item_from_pen_filter, scene_item_from_rect, scene_item_from_serial_number,
        scene_item_from_text,
    },
    rect_bounds,
};

#[derive(Clone, Debug, PartialEq)]
struct CachedSceneEntry {
    item: SceneDisplayItem,
    bounds: DrawRect,
}

#[derive(Debug, Default)]
pub struct DocumentSceneCache {
    document_revision: DocumentRevision,
    pub(crate) order_plan: SceneOrderPlan,
    order_plan_builds: u64,
    relation_revision: u64,
    pub(crate) preview_order_plan: std::sync::Mutex<SceneOrderPlan>,
    pub(crate) duplicate_scene:
        std::sync::Mutex<Option<std::rc::Rc<crate::scene_composition::DuplicateScene>>>,
    pub(crate) preview_order_builds: std::sync::atomic::AtomicU64,
    entries: HashMap<ElementId, CachedSceneEntry>,
    spotlight_entries: HashMap<ElementId, DisplaySpotlightCutout>,
}

impl DocumentSceneCache {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn document_revision(&self) -> DocumentRevision {
        self.document_revision
    }

    pub fn sync(&mut self, model: &DocumentModel, delta: Option<&DocumentDelta>) {
        let document_revision = model.document_revision();
        let needs_full_rebuild = self.document_revision != document_revision
            && (self.document_revision.0 == 0 || delta.is_none());

        if needs_full_rebuild || (delta.is_none() && self.document_revision != document_revision) {
            self.rebuild(model);
            return;
        }

        if let Some(delta) = delta {
            self.refresh(model, delta);
            self.document_revision = document_revision;
        }
    }

    pub fn order_plan_build_count(&self) -> u64 {
        self.order_plan_builds
            + self
                .preview_order_builds
                .load(std::sync::atomic::Ordering::Relaxed)
    }

    fn rebuild_order(&mut self, model: &DocumentModel) {
        // Labels are parts of their owners, even when only the label is in view.
        // Removing their original positions is essential to filter source boundaries.
        let mut labels = HashMap::<ElementId, Vec<ElementId>>::new();
        let label_ids: std::collections::HashSet<_> = model
            .arrow_label_bindings()
            .iter()
            .map(|(_, text)| *text)
            .collect();
        for &(owner, text) in model.arrow_label_bindings() {
            if model
                .element(owner)
                .is_ok_and(|element| element.meta.visible)
            {
                labels.entry(owner).or_default().push(text);
            }
        }
        let mut nodes = Vec::with_capacity(self.entries.len());
        for &id in model.paint_order() {
            if !label_ids.contains(&id)
                && let Some(item) = self.entry(id)
            {
                nodes.push(OrderNode::new(id, item));
            }
            if let Some(texts) = labels.get(&id) {
                nodes.extend(texts.iter().map(|&id| OrderNode {
                    id,
                    effect: None,
                    smart_erase: false,
                }));
            }
        }
        self.order_plan = SceneOrderPlan::new(nodes);
        self.relation_revision = model.relation_index_build_count();
        self.order_plan_builds += 1;
    }

    pub fn entry(&self, id: ElementId) -> Option<&SceneDisplayItem> {
        self.entries.get(&id).map(|entry| &entry.item)
    }

    pub fn bounds(&self, id: ElementId) -> Option<DrawRect> {
        self.entries.get(&id).map(|entry| entry.bounds)
    }

    pub fn spotlight_entry(&self, id: ElementId) -> Option<DisplaySpotlightCutout> {
        self.spotlight_entries.get(&id).copied()
    }

    fn rebuild(&mut self, model: &DocumentModel) {
        self.entries.clear();
        self.spotlight_entries.clear();
        for state in model.element_states() {
            self.refresh_entry(model, state.id);
        }
        self.rebuild_order(model);
        self.document_revision = model.document_revision();
    }

    fn refresh(&mut self, model: &DocumentModel, delta: &DocumentDelta) {
        let mut topology_changed = self.relation_revision != model.relation_index_build_count()
            || delta.z_order_changed
            || !delta.created.is_empty()
            || !delta.removed.is_empty();
        for id in &delta.removed {
            self.entries.remove(id);
            self.spotlight_entries.remove(id);
        }
        for id in delta.touched.iter().chain(&delta.created) {
            let old = self.entry(*id).map(|item| OrderNode::new(*id, item));
            self.refresh_entry(model, *id);
            topology_changed |= old != self.entry(*id).map(|item| OrderNode::new(*id, item));
        }
        if topology_changed {
            self.rebuild_order(model);
        }
    }

    fn refresh_entry(&mut self, model: &DocumentModel, id: ElementId) {
        let Ok(state) = model.element_state(id) else {
            self.entries.remove(&id);
            self.spotlight_entries.remove(&id);
            return;
        };
        if !state.visible {
            self.entries.remove(&id);
            self.spotlight_entries.remove(&id);
            return;
        }
        if let ElementData::Rectangle(rect) = state.data
            && rect.is_spotlight()
        {
            self.entries.remove(&id);
            self.spotlight_entries.insert(
                id,
                DisplaySpotlightCutout {
                    center_x: rect.center.x,
                    center_y: rect.center.y,
                    width: rect.width,
                    height: rect.height,
                    rotation: rect.rotation,
                },
            );
            return;
        }
        self.spotlight_entries.remove(&id);
        let entry = match state.data {
            ElementData::Rectangle(rect)
                if state.rect.width() > 0.0 && state.rect.height() > 0.0 =>
            {
                Some(CachedSceneEntry {
                    item: scene_item_from_rect(id, *rect),
                    bounds: model
                        .element_bounds(id)
                        .unwrap_or_else(|_| rect_bounds(*rect)),
                })
            }
            ElementData::Filter(filter)
                if state.rect.width() > 0.0 && state.rect.height() > 0.0 && state.opacity > 0.0 =>
            {
                Some(CachedSceneEntry {
                    item: scene_item_from_filter(id, *filter),
                    bounds: model
                        .element_bounds(id)
                        .unwrap_or_else(|_| filter_bounds(filter)),
                })
            }
            ElementData::PenFilter(filter) if state.opacity > 0.0 => Some(CachedSceneEntry {
                item: scene_item_from_pen_filter(id, filter.clone()),
                bounds: model
                    .element_bounds(id)
                    .unwrap_or_else(|_| pen_filter_bounds(filter)),
            }),
            ElementData::Arrow(arrow) if !arrow_is_degenerate(arrow) => Some(CachedSceneEntry {
                item: scene_item_from_arrow_revision(
                    id,
                    arrow.clone(),
                    self.document_revision.0.wrapping_add(1).max(1),
                ),
                bounds: model
                    .element_bounds(id)
                    .unwrap_or_else(|_| arrow_bounds(arrow)),
            }),
            ElementData::FreeDraw(free_draw) => Some(CachedSceneEntry {
                item: scene_item_from_free_draw(
                    id,
                    free_draw.clone(),
                    std::sync::Arc::new(
                        free_draw.path_geometry(self.document_revision.0.wrapping_add(1).max(1)),
                    ),
                ),
                bounds: model
                    .element_bounds(id)
                    .unwrap_or_else(|_| free_draw_bounds(free_draw)),
            }),
            ElementData::Text(text) if state.rect.width() > 0.0 && state.rect.height() > 0.0 => {
                Some(CachedSceneEntry {
                    item: scene_item_from_text(id, text.clone()),
                    bounds: model
                        .element_bounds(id)
                        .unwrap_or_else(|_| text_bounds(text)),
                })
            }
            ElementData::SerialNumber(serial)
                if state.rect.width() > 0.0 && state.rect.height() > 0.0 =>
            {
                Some(CachedSceneEntry {
                    item: scene_item_from_serial_number(
                        id,
                        serial.clone(),
                        model.bound_text_id_for_serial_number(id),
                    ),
                    bounds: model
                        .element_bounds(id)
                        .unwrap_or_else(|_| serial_number_bounds(serial)),
                })
            }
            _ => None,
        };
        if let Some(entry) = entry {
            self.entries.insert(id, entry);
        } else {
            self.entries.remove(&id);
        }
    }
}
