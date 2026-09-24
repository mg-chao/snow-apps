use std::collections::{HashMap, HashSet};

use serde::{Deserialize, Serialize};
use snow_draw_engine_core::{ErrorCode, Point};
use snow_draw_engine_document::{
    ElementData, ElementId, ElementRecord, Transaction, validate_element_data,
};
use snow_draw_engine_model::DocumentModel;

use crate::{ApplyTransactionCommand, Editor, EditorCommand, expanded_duplicate_ids};

pub const DRAW_TEMPLATE_SCHEMA_VERSION: u32 = 1;

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct DrawTemplate {
    pub schema_version: u32,
    pub source_center: Point<f64>,
    pub selected_ids: Vec<ElementId>,
    pub elements: Vec<ElementRecord>,
}

impl Editor {
    pub fn selected_draw_template(
        &self,
        document: &DocumentModel,
    ) -> Result<DrawTemplate, ErrorCode> {
        let selected_ids = self.state.selection.ids.clone();
        if selected_ids.is_empty() {
            return Err(ErrorCode::InvalidState);
        }
        let expanded: HashSet<_> = expanded_duplicate_ids(document, &selected_ids)
            .into_iter()
            .collect();
        let elements: Vec<_> = document
            .paint_order()
            .iter()
            .filter(|id| expanded.contains(id))
            .map(|id| document.element(*id).cloned())
            .collect::<Result<_, _>>()?;
        if elements.is_empty() {
            return Err(ErrorCode::InvalidState);
        }
        let mut bounds = document.element_bounds(elements[0].id)?;
        for element in elements.iter().skip(1) {
            let next = document.element_bounds(element.id)?;
            bounds.min_x = bounds.min_x.min(next.min_x);
            bounds.min_y = bounds.min_y.min(next.min_y);
            bounds.max_x = bounds.max_x.max(next.max_x);
            bounds.max_y = bounds.max_y.max(next.max_y);
        }
        Ok(DrawTemplate {
            schema_version: DRAW_TEMPLATE_SCHEMA_VERSION,
            source_center: Point::new(
                (bounds.min_x + bounds.max_x) / 2.0,
                (bounds.min_y + bounds.max_y) / 2.0,
            ),
            selected_ids,
            elements,
        })
    }

    pub fn insert_draw_template(
        &mut self,
        document: &DocumentModel,
        template: &DrawTemplate,
        center: Point<f64>,
    ) -> Result<EditorCommand, ErrorCode> {
        if template.schema_version != DRAW_TEMPLATE_SCHEMA_VERSION {
            return Err(ErrorCode::Unsupported);
        }
        if template.elements.is_empty()
            || template.elements.len() > 1_000_000
            || !center.x.is_finite()
            || !center.y.is_finite()
            || !template.source_center.x.is_finite()
            || !template.source_center.y.is_finite()
        {
            return Err(ErrorCode::InvalidArgument);
        }
        let offset = Point::new(
            center.x - template.source_center.x,
            center.y - template.source_center.y,
        );
        if !offset.x.is_finite() || !offset.y.is_finite() {
            return Err(ErrorCode::InvalidArgument);
        }
        let mut old_ids = HashSet::with_capacity(template.elements.len());
        let mut id_map = HashMap::with_capacity(template.elements.len());
        let first_id = document.peek_next_element_id();
        for (index, element) in template.elements.iter().enumerate() {
            if !old_ids.insert(element.id) {
                return Err(ErrorCode::InvalidArgument);
            }
            validate_element_data(&element.data)?;
            let index = u32::try_from(index).map_err(|_| ErrorCode::InvalidArgument)?;
            let new_index = first_id
                .index
                .checked_add(index)
                .ok_or(ErrorCode::InvalidState)?;
            id_map.insert(
                element.id,
                ElementId {
                    index: new_index,
                    generation: first_id.generation,
                },
            );
        }
        if template.selected_ids.is_empty()
            || template.selected_ids.iter().any(|id| !old_ids.contains(id))
            || template
                .selected_ids
                .iter()
                .copied()
                .collect::<HashSet<_>>()
                .len()
                != template.selected_ids.len()
        {
            return Err(ErrorCode::InvalidArgument);
        }

        let mut transaction = Transaction::new("insert draw template");
        for element in &template.elements {
            let new_id = id_map[&element.id];
            match &element.data {
                ElementData::Rectangle(data) => {
                    let mut data = *data;
                    data.center.x += offset.x;
                    data.center.y += offset.y;
                    validate_element_data(&ElementData::Rectangle(data))?;
                    transaction.insert_rectangle(new_id, element.meta, data);
                }
                ElementData::Filter(data) => {
                    let mut data = *data;
                    data.center.x += offset.x;
                    data.center.y += offset.y;
                    data.auto_region_id = None;
                    validate_element_data(&ElementData::Filter(data))?;
                    transaction.insert_filter(new_id, element.meta, data);
                }
                ElementData::PenFilter(data) => {
                    let mut data = data.clone();
                    data.x += offset.x;
                    data.y += offset.y;
                    validate_element_data(&ElementData::PenFilter(data.clone()))?;
                    transaction.insert_pen_filter(new_id, element.meta, data);
                }
                ElementData::Arrow(data) => {
                    let mut data = data.clone();
                    data.x += offset.x;
                    data.y += offset.y;
                    data.text_element_id =
                        data.text_element_id.and_then(|id| id_map.get(&id).copied());
                    for binding in [&mut data.start_binding, &mut data.end_binding] {
                        *binding = binding.take().and_then(|mut value| {
                            value.element_id = *id_map.get(&value.element_id)?;
                            Some(value)
                        });
                    }
                    validate_element_data(&ElementData::Arrow(data.clone()))?;
                    transaction.insert_arrow(new_id, element.meta, data);
                }
                ElementData::FreeDraw(data) => {
                    let mut data = data.clone();
                    data.x += offset.x;
                    data.y += offset.y;
                    validate_element_data(&ElementData::FreeDraw(data.clone()))?;
                    transaction.insert_free_draw(new_id, element.meta, data);
                }
                ElementData::Text(data) => {
                    let mut data = data.clone();
                    data.center.x += offset.x;
                    data.center.y += offset.y;
                    validate_element_data(&ElementData::Text(data.clone()))?;
                    transaction.insert_text(new_id, element.meta, data);
                }
                ElementData::SerialNumber(data) => {
                    let mut data = data.clone();
                    data.center.x += offset.x;
                    data.center.y += offset.y;
                    data.text_element_id =
                        data.text_element_id.and_then(|id| id_map.get(&id).copied());
                    validate_element_data(&ElementData::SerialNumber(data.clone()))?;
                    transaction.insert_serial_number(new_id, element.meta, data);
                }
            }
        }
        // Validate bindings and all operations on a disposable document before changing selection.
        document.clone().apply_transaction(transaction.clone())?;
        let undo = self.capture_document_sync_snapshot(document);
        let next_selection: Vec<_> = template.selected_ids.iter().map(|id| id_map[id]).collect();
        self.set_selection_state(next_selection.clone(), next_selection.last().copied());
        Ok(EditorCommand::ApplyTransaction(
            ApplyTransactionCommand::with_history_undo_snapshot(transaction, undo),
        ))
    }
}
