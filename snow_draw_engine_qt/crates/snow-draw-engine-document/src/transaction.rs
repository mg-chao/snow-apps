use serde::{Deserialize, Serialize};
use snow_draw_engine_core::DrawRect;

use crate::document::{
    Document, DocumentRevision, ElementData, ElementId, ElementKind, ElementMeta, ElementRecord,
    RectangleData, SerialNumberData, TextData,
};

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub enum Operation {
    InsertElement {
        id: ElementId,
        meta: ElementMeta,
        data: ElementData,
    },
    UpdateElementData {
        id: ElementId,
        data: ElementData,
    },
    UpdateElementMeta {
        id: ElementId,
        meta: ElementMeta,
    },
    RemoveElement {
        id: ElementId,
    },
    RestoreElement {
        element: ElementRecord,
        paint_index: u32,
    },
    ReorderElements {
        ids: Vec<ElementId>,
        paint_index: u32,
    },
    UpdateWatermark {
        config: crate::WatermarkConfig,
    },
    UpdateSpotlight {
        config: crate::SpotlightConfig,
    },
}

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct Transaction {
    pub(crate) label: String,
    pub(crate) operations: Vec<Operation>,
}

impl Transaction {
    pub fn new(label: impl Into<String>) -> Self {
        Self {
            label: label.into(),
            operations: Vec::new(),
        }
    }

    pub fn label(&self) -> &str {
        &self.label
    }

    pub fn operations(&self) -> &[Operation] {
        &self.operations
    }

    pub fn is_empty(&self) -> bool {
        self.operations.is_empty()
    }

    pub fn push(&mut self, operation: Operation) -> &mut Self {
        self.operations.push(operation);
        self
    }

    pub fn insert_rectangle(
        &mut self,
        id: ElementId,
        meta: ElementMeta,
        rect: RectangleData,
    ) -> &mut Self {
        self.push(Operation::InsertElement {
            id,
            meta,
            data: ElementData::Rectangle(rect),
        })
    }

    pub fn insert_filter(
        &mut self,
        id: ElementId,
        meta: ElementMeta,
        filter: crate::FilterData,
    ) -> &mut Self {
        self.push(Operation::InsertElement {
            id,
            meta,
            data: ElementData::Filter(filter),
        })
    }

    pub fn insert_pen_filter(
        &mut self,
        id: ElementId,
        meta: ElementMeta,
        filter: crate::PenFilterData,
    ) -> &mut Self {
        self.push(Operation::InsertElement {
            id,
            meta,
            data: ElementData::PenFilter(filter),
        })
    }

    pub fn insert_arrow(
        &mut self,
        id: ElementId,
        meta: ElementMeta,
        arrow: crate::ArrowData,
    ) -> &mut Self {
        self.push(Operation::InsertElement {
            id,
            meta,
            data: ElementData::Arrow(arrow),
        })
    }

    pub fn insert_free_draw(
        &mut self,
        id: ElementId,
        meta: ElementMeta,
        free_draw: crate::FreeDrawData,
    ) -> &mut Self {
        self.push(Operation::InsertElement {
            id,
            meta,
            data: ElementData::FreeDraw(free_draw),
        })
    }

    pub fn insert_text(&mut self, id: ElementId, meta: ElementMeta, text: TextData) -> &mut Self {
        self.push(Operation::InsertElement {
            id,
            meta,
            data: ElementData::Text(text),
        })
    }

    pub fn insert_serial_number(
        &mut self,
        id: ElementId,
        meta: ElementMeta,
        serial: SerialNumberData,
    ) -> &mut Self {
        self.push(Operation::InsertElement {
            id,
            meta,
            data: ElementData::SerialNumber(serial),
        })
    }

    pub fn update_rectangle(&mut self, id: ElementId, rect: RectangleData) -> &mut Self {
        self.push(Operation::UpdateElementData {
            id,
            data: ElementData::Rectangle(rect),
        })
    }

    pub fn update_filter(&mut self, id: ElementId, filter: crate::FilterData) -> &mut Self {
        self.push(Operation::UpdateElementData {
            id,
            data: ElementData::Filter(filter),
        })
    }

    pub fn update_pen_filter(&mut self, id: ElementId, filter: crate::PenFilterData) -> &mut Self {
        self.push(Operation::UpdateElementData {
            id,
            data: ElementData::PenFilter(filter),
        })
    }

    pub fn update_arrow(&mut self, id: ElementId, arrow: crate::ArrowData) -> &mut Self {
        self.push(Operation::UpdateElementData {
            id,
            data: ElementData::Arrow(arrow),
        })
    }

    pub fn update_free_draw(&mut self, id: ElementId, free_draw: crate::FreeDrawData) -> &mut Self {
        self.push(Operation::UpdateElementData {
            id,
            data: ElementData::FreeDraw(free_draw),
        })
    }

    pub fn update_text(&mut self, id: ElementId, text: TextData) -> &mut Self {
        self.push(Operation::UpdateElementData {
            id,
            data: ElementData::Text(text),
        })
    }

    pub fn update_serial_number(&mut self, id: ElementId, serial: SerialNumberData) -> &mut Self {
        self.push(Operation::UpdateElementData {
            id,
            data: ElementData::SerialNumber(serial),
        })
    }

    pub fn update_element_meta(&mut self, id: ElementId, meta: ElementMeta) -> &mut Self {
        self.push(Operation::UpdateElementMeta { id, meta })
    }

    pub fn remove_element(&mut self, id: ElementId) -> &mut Self {
        self.push(Operation::RemoveElement { id })
    }

    pub fn reorder_elements(
        &mut self,
        ids: impl Into<Vec<ElementId>>,
        paint_index: u32,
    ) -> &mut Self {
        self.push(Operation::ReorderElements {
            ids: ids.into(),
            paint_index,
        })
    }

    pub fn update_watermark(&mut self, config: crate::WatermarkConfig) -> &mut Self {
        self.push(Operation::UpdateWatermark { config })
    }

    pub fn update_spotlight(&mut self, config: crate::SpotlightConfig) -> &mut Self {
        self.push(Operation::UpdateSpotlight { config })
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct DocumentDelta {
    pub touched: Vec<ElementId>,
    pub created: Vec<ElementId>,
    pub removed: Vec<ElementId>,
    pub element_changes: Vec<ElementChange>,
    pub dirty_bounds: Option<DrawRect>,
    pub z_order_changed: bool,
    pub relations_changed: bool,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ElementChange {
    pub id: ElementId,
    pub kind: ElementKind,
    pub old_bounds: Option<DrawRect>,
    pub new_bounds: Option<DrawRect>,
    pub old_paint_index: Option<u32>,
    pub new_paint_index: Option<u32>,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct ElementChangeSnapshot {
    pub(crate) kind: ElementKind,
    pub(crate) bounds: Option<DrawRect>,
    pub(crate) paint_index: Option<u32>,
}

impl DocumentDelta {
    pub(crate) fn touch(&mut self, id: ElementId) {
        if !self.touched.contains(&id) {
            self.touched.push(id);
        }
    }

    fn union_bounds(&mut self, bounds: DrawRect) {
        self.dirty_bounds = Some(match self.dirty_bounds {
            Some(existing) => DrawRect::new(
                existing.min_x.min(bounds.min_x),
                existing.min_y.min(bounds.min_y),
                existing.max_x.max(bounds.max_x),
                existing.max_y.max(bounds.max_y),
            ),
            None => bounds,
        });
    }

    pub(crate) fn note_existing_bounds(&mut self, document: &Document, id: ElementId) {
        if let Ok(bounds) = document.element_bounds(id) {
            self.union_bounds(bounds);
        }
    }

    pub(crate) fn note_element_bounds(&mut self, data: &ElementData) {
        self.union_bounds(data.bounds());
    }

    pub(crate) fn note_element_transition(
        &mut self,
        id: ElementId,
        old: Option<ElementChangeSnapshot>,
        new: Option<ElementChangeSnapshot>,
    ) {
        let Some(kind) = old
            .map(|snapshot| snapshot.kind)
            .or_else(|| new.map(|snapshot| snapshot.kind))
        else {
            return;
        };

        if let Some(existing) = self
            .element_changes
            .iter_mut()
            .find(|change| change.id == id)
        {
            if existing.old_bounds.is_none() {
                existing.old_bounds = old.and_then(|snapshot| snapshot.bounds);
            }
            if existing.old_paint_index.is_none() {
                existing.old_paint_index = old.and_then(|snapshot| snapshot.paint_index);
            }
            existing.kind = kind;
            existing.new_bounds = new.and_then(|snapshot| snapshot.bounds);
            existing.new_paint_index = new.and_then(|snapshot| snapshot.paint_index);
            return;
        }

        self.element_changes.push(ElementChange {
            id,
            kind,
            old_bounds: old.and_then(|snapshot| snapshot.bounds),
            new_bounds: new.and_then(|snapshot| snapshot.bounds),
            old_paint_index: old.and_then(|snapshot| snapshot.paint_index),
            new_paint_index: new.and_then(|snapshot| snapshot.paint_index),
        });
    }
}

pub type ChangeSet = DocumentDelta;

#[derive(Clone, Debug, PartialEq)]
pub struct ApplyResult {
    pub document_revision: DocumentRevision,
    pub inverse: Transaction,
    pub changes: DocumentDelta,
}
