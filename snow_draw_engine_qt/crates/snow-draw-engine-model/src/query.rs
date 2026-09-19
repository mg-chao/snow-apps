mod relation_index;
mod snap_index;
mod spatial_index;

pub use relation_index::RelationIndex;
pub use snap_index::SnapIndex;
pub use spatial_index::SpatialIndex;

use snow_draw_engine_core::{
    Point, SnapQuery, SnapResult, ViewportQuery, canvas_viewport, rectangle_intersects_viewport,
};
use snow_draw_engine_document::{
    Document, DocumentDelta, ElementId, ElementKind, RectangleData, arrow_hit_test,
    filter_hit_test, rectangle_hit_test, serial_number_hit_test, text_hit_test,
};

#[derive(Clone, Debug, Default)]
pub struct QueryStore {
    spatial_index: SpatialIndex,
    snap_index: SnapIndex,
    relation_index: RelationIndex,
    paint_ranks: Vec<Option<(u32, u32)>>,
}

impl QueryStore {
    pub fn new(document: &Document) -> Self {
        let mut store = Self::default();
        store.rebuild(document);
        store
    }

    pub fn rebuild(&mut self, document: &Document) {
        self.spatial_index.rebuild(document);
        self.snap_index.rebuild(document);
        self.relation_index.rebuild(document);
        self.rebuild_paint_ranks(document);
    }

    pub fn refresh(&mut self, document: &Document, delta: &DocumentDelta) {
        self.spatial_index.refresh(document, delta);
        self.snap_index.refresh(document, delta);
        self.relation_index.refresh(document, delta);
        if delta.z_order_changed || !delta.created.is_empty() || !delta.removed.is_empty() {
            self.rebuild_paint_ranks(document);
        }
    }

    pub fn bound_arrow_ids(&self, bindable_id: ElementId) -> &[ElementId] {
        self.relation_index.bound_arrow_ids(bindable_id)
    }

    pub fn bound_bindable_ids(&self, arrow_id: ElementId) -> &[ElementId] {
        self.relation_index.bound_bindable_ids(arrow_id)
    }

    pub fn relations(&self) -> &RelationIndex {
        &self.relation_index
    }

    pub fn paint_rank(&self, id: ElementId) -> Option<u32> {
        self.paint_ranks
            .get(id.index as usize)
            .and_then(|rank| *rank)
            .and_then(|(generation, rank)| (generation == id.generation).then_some(rank))
    }

    pub fn visible_element_ids(&self, document: &Document, query: ViewportQuery) -> Vec<ElementId> {
        if query.surface.width == 0 || query.surface.height == 0 {
            return Vec::new();
        }
        let viewport = canvas_viewport(query.camera, query.surface);
        self.spatial_index
            .with_viewport_candidate_ids(viewport, |candidates| {
                let mut visible = candidates
                    .iter()
                    .copied()
                    .filter(|id| {
                        let Some(bounds) = self.spatial_index.bounds(*id) else {
                            return false;
                        };
                        bounds.max_x >= viewport.0
                            && bounds.min_x <= viewport.2
                            && bounds.max_y >= viewport.1
                            && bounds.min_y <= viewport.3
                            && document.element(*id).is_ok()
                    })
                    .collect::<Vec<_>>();
                visible.sort_unstable_by_key(|id| self.paint_rank(*id).unwrap_or(u32::MAX));
                visible
            })
    }

    fn rebuild_paint_ranks(&mut self, document: &Document) {
        self.paint_ranks.clear();
        for (rank, id) in document.paint_order().iter().copied().enumerate() {
            let index = id.index as usize;
            if self.paint_ranks.len() <= index {
                self.paint_ranks.resize(index + 1, None);
            }
            self.paint_ranks[index] = Some((id.generation, rank as u32));
        }
    }

    pub fn visible_scene_item_count(&self, document: &Document, query: ViewportQuery) -> u32 {
        let mut count = 0u32;
        self.for_each_visible_scene_rect(document, query, |_, _| {
            count += 1;
        });
        count
    }

    pub fn for_each_visible_scene_rect(
        &self,
        document: &Document,
        query: ViewportQuery,
        mut f: impl FnMut(ElementId, &RectangleData),
    ) {
        if query.surface.width == 0 || query.surface.height == 0 {
            return;
        }

        let viewport = canvas_viewport(query.camera, query.surface);
        self.spatial_index
            .with_viewport_candidate_marks(viewport, |marks, stamp| {
                for id in document.paint_order() {
                    let index = id.index as usize;
                    if marks.get(index).copied().unwrap_or_default() != stamp {
                        continue;
                    }
                    let Some(bounds) = self.spatial_index.bounds(*id) else {
                        continue;
                    };
                    if bounds.max_x < viewport.0
                        || bounds.min_x > viewport.2
                        || bounds.max_y < viewport.1
                        || bounds.min_y > viewport.3
                    {
                        continue;
                    }
                    let Ok(rect) = document.rectangle(*id) else {
                        continue;
                    };
                    if rectangle_intersects_viewport(
                        rect.center,
                        rect.width,
                        rect.height,
                        rect.rotation,
                        rect.stroke_width,
                        viewport,
                    ) {
                        f(*id, rect);
                    }
                }
            });
    }

    pub fn topmost_element_at_with_tolerance(
        &self,
        document: &Document,
        point: Point<f64>,
        hit_tolerance: f64,
    ) -> Option<(ElementId, ElementKind)> {
        self.topmost_element_matching_at_with_tolerance(document, point, hit_tolerance, |_, _| true)
    }

    pub fn elements_at_with_tolerance(
        &self,
        document: &Document,
        point: Point<f64>,
        hit_tolerance: f64,
    ) -> Vec<(ElementId, ElementKind)> {
        self.spatial_index
            .with_point_candidate_marks(point, hit_tolerance, |marks, stamp| {
                document
                    .paint_order()
                    .iter()
                    .copied()
                    .filter_map(|id| {
                        if marks.get(id.index as usize).copied().unwrap_or_default() != stamp {
                            return None;
                        }
                        let element = document.element(id).ok()?;
                        if !element.meta.visible || element.meta.locked {
                            return None;
                        }
                        let hit = match &element.data {
                            snow_draw_engine_document::ElementData::Rectangle(rect) => {
                                rectangle_hit_test(rect, point, hit_tolerance)
                            }
                            snow_draw_engine_document::ElementData::Filter(filter) => {
                                filter_hit_test(filter, point, hit_tolerance)
                            }
                            snow_draw_engine_document::ElementData::PenFilter(filter) => {
                                snow_draw_engine_document::pen_filter_hit_test(
                                    filter,
                                    point,
                                    hit_tolerance,
                                )
                            }
                            snow_draw_engine_document::ElementData::Arrow(arrow) => {
                                arrow_hit_test(arrow, point, hit_tolerance)
                            }
                            snow_draw_engine_document::ElementData::FreeDraw(free_draw) => {
                                snow_draw_engine_document::free_draw_hit_test(
                                    free_draw,
                                    point,
                                    hit_tolerance,
                                )
                            }
                            snow_draw_engine_document::ElementData::Text(text) => {
                                text_hit_test(text, point, hit_tolerance)
                            }
                            snow_draw_engine_document::ElementData::SerialNumber(serial) => {
                                serial_number_hit_test(serial, point, hit_tolerance)
                            }
                        };
                        hit.then_some((id, element.data.kind()))
                    })
                    .collect()
            })
    }

    pub fn topmost_element_matching_at_with_tolerance(
        &self,
        document: &Document,
        point: Point<f64>,
        hit_tolerance: f64,
        mut include: impl FnMut(ElementId, ElementKind) -> bool,
    ) -> Option<(ElementId, ElementKind)> {
        self.spatial_index
            .with_point_candidate_marks(point, hit_tolerance, |marks, stamp| {
                document.paint_order().iter().rev().find_map(|id| {
                    let index = id.index as usize;
                    if marks.get(index).copied().unwrap_or_default() != stamp {
                        return None;
                    }
                    let element = document.element(*id).ok()?;
                    if !element.meta.visible {
                        return None;
                    }
                    let hit = match &element.data {
                        snow_draw_engine_document::ElementData::Rectangle(rect)
                            if rectangle_hit_test(rect, point, hit_tolerance) =>
                        {
                            true
                        }
                        snow_draw_engine_document::ElementData::Arrow(arrow)
                            if arrow_hit_test(arrow, point, hit_tolerance) =>
                        {
                            true
                        }
                        snow_draw_engine_document::ElementData::FreeDraw(free_draw)
                            if snow_draw_engine_document::free_draw_hit_test(
                                free_draw,
                                point,
                                hit_tolerance,
                            ) =>
                        {
                            true
                        }
                        snow_draw_engine_document::ElementData::Text(text)
                            if text_hit_test(text, point, hit_tolerance) =>
                        {
                            true
                        }
                        snow_draw_engine_document::ElementData::SerialNumber(serial)
                            if serial_number_hit_test(serial, point, hit_tolerance) =>
                        {
                            true
                        }
                        _ => return None,
                    };
                    let kind = element.data.kind();
                    if hit && include(*id, kind) {
                        Some((*id, kind))
                    } else {
                        None
                    }
                })
            })
    }

    pub fn snap_point(&self, document: &Document, query: &SnapQuery) -> SnapResult {
        self.snap_point_filtered(document, query, |_, _| true)
    }

    pub fn snap_point_filtered(
        &self,
        document: &Document,
        query: &SnapQuery,
        include_candidate: impl FnMut(ElementId, &RectangleData) -> bool,
    ) -> SnapResult {
        self.snap_index
            .snap_point_filtered(document, &self.spatial_index, query, include_candidate)
    }
}
