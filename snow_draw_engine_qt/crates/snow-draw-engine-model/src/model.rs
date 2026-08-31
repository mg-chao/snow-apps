use std::ops::Deref;

use crate::query::QueryStore;
use snow_draw_engine_core::{ErrorCode, Point, SnapQuery, SnapResult, ViewportQuery};
use snow_draw_engine_document::{
    ApplyResult, BindableElementState, Document, DocumentRevision, ElementId, ElementKind,
    RectangleData, Transaction,
};

#[derive(Debug)]
pub struct DocumentModel {
    document: Document,
    queries: QueryStore,
}

impl Clone for DocumentModel {
    fn clone(&self) -> Self {
        let document = self.document.clone();
        let queries = QueryStore::new(&document);
        Self { document, queries }
    }
}

impl Default for DocumentModel {
    fn default() -> Self {
        let document = Document::new();
        let queries = QueryStore::new(&document);
        Self { document, queries }
    }
}

impl DocumentModel {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn from_document(document: Document) -> Result<Self, ErrorCode> {
        document.validate_session()?;
        let queries = QueryStore::new(&document);
        Ok(Self { document, queries })
    }

    pub fn document(&self) -> &Document {
        &self.document
    }

    pub fn document_revision(&self) -> DocumentRevision {
        self.document.document_revision()
    }

    pub fn allocate_element_id(&mut self) -> ElementId {
        self.document.allocate_element_id()
    }

    pub fn peek_next_element_id(&self) -> ElementId {
        self.document.peek_next_element_id()
    }

    pub fn apply_transaction(
        &mut self,
        transaction: Transaction,
    ) -> Result<ApplyResult, ErrorCode> {
        self.apply_transaction_ref(&transaction)
    }

    pub fn apply_history_transaction(
        &mut self,
        transaction: &Transaction,
    ) -> Result<ApplyResult, ErrorCode> {
        self.apply_transaction_ref(transaction)
    }

    fn apply_transaction_ref(
        &mut self,
        transaction: &Transaction,
    ) -> Result<ApplyResult, ErrorCode> {
        let apply_result = self.document.apply(transaction)?;
        self.queries.refresh(&self.document, &apply_result.changes);
        Ok(apply_result)
    }

    pub fn visible_scene_item_count(&self, query: ViewportQuery) -> u32 {
        self.queries.visible_scene_item_count(&self.document, query)
    }

    pub fn visible_element_ids(&self, query: ViewportQuery) -> Vec<ElementId> {
        self.queries.visible_element_ids(&self.document, query)
    }

    pub fn paint_rank(&self, id: ElementId) -> Option<u32> {
        self.queries.paint_rank(id)
    }

    pub fn for_each_visible_scene_rect(
        &self,
        query: ViewportQuery,
        f: impl FnMut(ElementId, &RectangleData),
    ) {
        self.queries
            .for_each_visible_scene_rect(&self.document, query, f)
    }

    pub fn topmost_element_at_with_tolerance(
        &self,
        point: Point<f64>,
        hit_tolerance: f64,
    ) -> Option<(ElementId, ElementKind)> {
        self.queries
            .topmost_element_at_with_tolerance(&self.document, point, hit_tolerance)
    }

    pub fn elements_at_with_tolerance(
        &self,
        point: Point<f64>,
        hit_tolerance: f64,
    ) -> Vec<(ElementId, ElementKind)> {
        self.queries
            .elements_at_with_tolerance(&self.document, point, hit_tolerance)
    }

    pub fn topmost_element_matching_at_with_tolerance(
        &self,
        point: Point<f64>,
        hit_tolerance: f64,
        include: impl FnMut(ElementId, ElementKind) -> bool,
    ) -> Option<(ElementId, ElementKind)> {
        self.queries.topmost_element_matching_at_with_tolerance(
            &self.document,
            point,
            hit_tolerance,
            include,
        )
    }

    pub fn bound_arrow_ids(&self, bindable_id: ElementId) -> &[ElementId] {
        self.queries.bound_arrow_ids(bindable_id)
    }

    pub fn bound_bindable_ids(&self, arrow_id: ElementId) -> &[ElementId] {
        self.queries.bound_bindable_ids(arrow_id)
    }


    pub fn bindable_element_states_with_overrides(
        &self,
        overrides: &[(ElementId, RectangleData)],
    ) -> Vec<BindableElementState> {
        let overrides_by_id = overrides
            .iter()
            .copied()
            .collect::<std::collections::HashMap<_, _>>();

        self.document
            .element_states()
            .filter_map(|state| {
                if !state.visible {
                    return None;
                }
                let rect = overrides_by_id
                    .get(&state.id)
                    .copied()
                    .or_else(|| self.document.element_rect_proxy(state.id))?;
                Some(BindableElementState::rectangle_proxy(
                    state.id,
                    rect,
                    state.z_index as u32,
                    state.locked,
                ))
            })
            .collect()
    }

    pub fn snap_point(&self, query: &SnapQuery) -> SnapResult {
        self.queries.snap_point(&self.document, query)
    }

    pub fn snap_point_filtered(
        &self,
        query: &SnapQuery,
        include_candidate: impl FnMut(ElementId, &RectangleData) -> bool,
    ) -> SnapResult {
        self.queries
            .snap_point_filtered(&self.document, query, include_candidate)
    }
}

impl Deref for DocumentModel {
    type Target = Document;

    fn deref(&self) -> &Self::Target {
        &self.document
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::{Camera, ColorRgba8, CornerRadii, SurfaceSize};
    use snow_draw_engine_document::{ElementMeta, FillStyle};

    fn rect(center_x: f64) -> RectangleData {
        RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: Point::new(center_x, 0.0),
            width: 40.0,
            height: 40.0,
            rotation: 0.0,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: snow_draw_engine_document::StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        }
    }

    fn viewport_query() -> ViewportQuery {
        ViewportQuery {
            camera: Camera {
                center: Point::new(0.0, 0.0),
                zoom: 1.0,
            },
            surface: SurfaceSize {
                width: 400,
                height: 300,
            },
        }
    }

    #[test]
    fn visible_element_ids_are_spatially_filtered_and_paint_ordered() {
        let first = ElementId {
            index: 0,
            generation: 1,
        };
        let second = ElementId {
            index: 1,
            generation: 1,
        };
        let offscreen = ElementId {
            index: 2,
            generation: 1,
        };
        let mut model = DocumentModel::new();
        let mut transaction = Transaction::new("visible candidates");
        transaction.insert_rectangle(first, ElementMeta::default(), rect(-50.0));
        transaction.insert_rectangle(second, ElementMeta::default(), rect(50.0));
        transaction.insert_rectangle(offscreen, ElementMeta::default(), rect(1000.0));
        model.apply_transaction(transaction).unwrap();

        assert_eq!(
            model.visible_element_ids(viewport_query()),
            vec![first, second]
        );

        let mut reorder = Transaction::new("reorder visible candidates");
        reorder.reorder_elements(vec![second], 0);
        model.apply_transaction(reorder).unwrap();
        assert_eq!(
            model.visible_element_ids(viewport_query()),
            vec![second, first]
        );
    }
}
