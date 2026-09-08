use snow_draw_engine_document::{ArrowData, ElementId, FreeDrawData, PenFilterData, RectangleData};
use snow_draw_engine_model::DocumentModel;

use crate::{
    Editor, SelectionArrowState, SelectionRectState,
    geometry::{
        selection_bounds_from_selection, selection_frame_padding_for_members,
        selection_outline_for_rect, text_selection_frame_padding,
    },
    state::{AxisAlignedBounds, InteractionState, SelectionState, ToolSelectionScope},
};

impl Editor {
    pub(crate) fn clear_selection(&mut self) {
        self.set_selection_state(Vec::new(), None);
    }

    pub(crate) fn selection_frame_padding_for_selected_members(
        &self,
        document: &DocumentModel,
        elements: &[SelectionRectState],
        arrows: &[SelectionArrowState],
    ) -> f64 {
        if arrows.is_empty() && elements.len() == 1 && document.text(elements[0].id).is_ok() {
            text_selection_frame_padding(self.camera().zoom)
        } else {
            selection_frame_padding_for_members(self.camera().zoom, elements.len(), arrows.len())
        }
    }

    pub(crate) fn toggle_selection(&mut self, document: &DocumentModel, id: ElementId) {
        let mut ids = self.state.selection.ids.clone();
        if let Some(index) = ids.iter().position(|candidate| *candidate == id) {
            ids.remove(index);
            let primary = if self.state.selection.primary == Some(id) {
                ids.last().copied()
            } else {
                self.state.selection.primary
            };
            self.set_selection_state_with_document(Some(document), ids, primary);
        } else {
            ids.push(id);
            self.set_selection_state_with_document(Some(document), ids, Some(id));
        }
    }

    pub(crate) fn selected_average_text_or_serial_font_size(
        &self,
        document: &DocumentModel,
    ) -> Option<f64> {
        let mut sum = 0.0;
        let mut count = 0usize;
        for id in self.state.selection.ids.iter().copied() {
            if let Ok(text) = document.text(id) {
                sum += text.font_size;
                count += 1;
                continue;
            }
            if let Ok(serial) = document.serial_number(id) {
                sum += serial.font_size;
                count += 1;
            }
        }
        (count > 0).then_some(sum / count as f64)
    }

    pub(crate) fn set_selection_state(
        &mut self,
        selection: Vec<ElementId>,
        primary: Option<ElementId>,
    ) {
        self.set_selection_state_with_document(None, selection, primary);
    }

    pub(crate) fn set_selection_state_with_document(
        &mut self,
        document: Option<&DocumentModel>,
        selection: Vec<ElementId>,
        primary: Option<ElementId>,
    ) {
        let mut next_ids = Vec::with_capacity(selection.len());
        for id in selection {
            let id = document
                .and_then(|document| document.arrow_id_for_text(id))
                .unwrap_or(id);
            let missing = document.is_some_and(|document| document.element(id).is_err());
            if next_ids.contains(&id) || missing {
                continue;
            }
            next_ids.push(id);
        }
        let next_primary = primary
            .filter(|id| next_ids.contains(id))
            .or_else(|| next_ids.last().copied());
        let next_elements = document
            .map(|document| Self::selection_elements_from_ids(document, &next_ids))
            .unwrap_or_default();
        let next_arrows = document
            .map(|document| Self::selection_arrows_from_ids(document, &next_ids))
            .unwrap_or_default();
        let next_bounds = document.map_or_else(
            || selection_bounds_from_selection(&next_elements, &next_arrows),
            |document| {
                self.arrow_text_selection_bounds(
                    document,
                    selection_bounds_from_selection(&next_elements, &next_arrows),
                    &next_arrows,
                )
            },
        );
        let next = SelectionState {
            ids: next_ids,
            primary: next_primary,
            bounds: next_bounds,
            elements: next_elements,
            arrows: next_arrows,
        };
        if self.state.selection != next {
            if !matches!(&self.state.interaction, InteractionState::Idle) {
                self.cancel_interaction();
            }
            self.state.selection = next;
            if document.is_none_or(|document| {
                self.hovered_rect(document).is_none()
                    && self.hovered_free_draw(document).is_none()
                    && self.hovered_pen_filter(document).is_none()
                    && self.hovered_text_rect(document).is_none()
            }) {
                self.state.ui.hovered_element = None;
            }
            self.bump_overlay_state_revision();
        }
    }

    pub(crate) fn sync_selection_after_document_change(
        &mut self,
        document: &DocumentModel,
        previous_selection: &SelectionState,
    ) {
        let interaction_contains_missing = match &self.state.interaction {
            InteractionState::PendingSelectionMove(state) => {
                state
                    .original_elements
                    .iter()
                    .any(|element| document.element(element.id).is_err())
                    || state
                        .original_arrows
                        .iter()
                        .any(|arrow| document.element(arrow.id).is_err())
            }
            InteractionState::EditingSelection(state) => {
                state
                    .original_elements
                    .iter()
                    .any(|element| document.element(element.id).is_err())
                    || state
                        .original_arrows
                        .iter()
                        .any(|arrow| document.element(arrow.id).is_err())
            }
            _ => false,
        };
        if interaction_contains_missing {
            self.cancel_interaction();
        }

        let filtered_ids = self
            .state
            .selection
            .ids
            .iter()
            .copied()
            .filter(|id| document.element(*id).is_ok())
            .collect::<Vec<_>>();
        let filtered_primary = self
            .state
            .selection
            .primary
            .filter(|id| filtered_ids.contains(id))
            .or_else(|| filtered_ids.last().copied());
        let current_elements = Self::selection_elements_from_ids(document, &filtered_ids);
        let current_arrows = Self::selection_arrows_from_ids(document, &filtered_ids);
        let selection_changed = previous_selection.ids != filtered_ids
            || previous_selection.primary != filtered_primary;
        let selection_members_changed = previous_selection.elements != current_elements
            || previous_selection.arrows != current_arrows;
        let next_elements = current_elements;
        let next_arrows = current_arrows;
        let next_bounds = if selection_changed
            || selection_members_changed
            || !previous_selection.has_cached_members()
        {
            selection_bounds_from_selection(&next_elements, &next_arrows)
        } else {
            previous_selection
                .bounds
                .or_else(|| selection_bounds_from_selection(&next_elements, &next_arrows))
        };
        let next_bounds = self.arrow_text_selection_bounds(document, next_bounds, &next_arrows);
        let next = SelectionState {
            ids: filtered_ids,
            primary: filtered_primary,
            bounds: next_bounds,
            elements: next_elements,
            arrows: next_arrows,
        };
        if self.state.selection != next {
            self.state.selection = next;
            if self.hovered_rect(document).is_none()
                && self.hovered_free_draw(document).is_none()
                && self.hovered_pen_filter(document).is_none()
                && self.hovered_text_rect(document).is_none()
                && self.hovered_arrow(document).is_none()
            {
                self.state.ui.hovered_element = None;
            }
            self.bump_overlay_state_revision();
        }
    }

    pub(crate) fn set_hovered_element(&mut self, hovered_element: Option<ElementId>) {
        if self.state.ui.hovered_element != hovered_element {
            self.state.ui.hovered_element = hovered_element;
            self.bump_overlay_state_revision();
        }
    }

    pub(crate) fn hovered_rect(&self, document: &DocumentModel) -> Option<RectangleData> {
        let hovered_id = self.state.ui.hovered_element?;
        if self.state.selection.contains(hovered_id) {
            return None;
        }
        if document.pen_filter(hovered_id).is_ok() || document.free_draw(hovered_id).is_ok() {
            return None;
        }
        let rect = document.element_rect_proxy(hovered_id)?;
        document
            .text(hovered_id)
            .is_err()
            .then(|| selection_outline_for_rect(&rect, self.camera().zoom))
    }

    pub(crate) fn hovered_free_draw(&self, document: &DocumentModel) -> Option<FreeDrawData> {
        let hovered_id = self.state.ui.hovered_element?;
        if self.state.selection.contains(hovered_id) {
            return None;
        }
        document.free_draw(hovered_id).ok().cloned()
    }

    pub(crate) fn hovered_pen_filter(&self, document: &DocumentModel) -> Option<PenFilterData> {
        let hovered_id = self.state.ui.hovered_element?;
        if self.state.selection.contains(hovered_id) {
            return None;
        }
        document.pen_filter(hovered_id).ok().cloned()
    }

    pub(crate) fn hovered_text_rect(&self, document: &DocumentModel) -> Option<RectangleData> {
        let hovered_id = self.state.ui.hovered_element?;
        if self.state.selection.contains(hovered_id) || document.text(hovered_id).is_err() {
            return None;
        }
        document.element_rect_proxy(hovered_id)
    }

    pub(crate) fn hovered_arrow(&self, document: &DocumentModel) -> Option<ArrowData> {
        let hovered_id = self.state.ui.hovered_element?;
        if self.state.selection.contains(hovered_id) {
            return None;
        }
        document.arrow(hovered_id).ok().cloned()
    }

    pub(crate) fn marquee_candidate_elements(
        &self,
        document: &DocumentModel,
    ) -> Vec<SelectionRectState> {
        if !matches!(
            self.state.interaction,
            InteractionState::MarqueeSelection(_)
        ) {
            return Vec::new();
        }

        let Some(rect) = &self.state.ui.marquee else {
            return Vec::new();
        };
        let bounds = AxisAlignedBounds {
            left: rect.center.x - rect.width / 2.0,
            top: rect.center.y - rect.height / 2.0,
            right: rect.center.x + rect.width / 2.0,
            bottom: rect.center.y + rect.height / 2.0,
        };
        let ids = self.selection_ids_intersecting_bounds(document, bounds, ToolSelectionScope::All);
        Self::selection_elements_from_ids(document, &ids)
    }

    pub(crate) fn marquee_candidate_arrows(
        &self,
        document: &DocumentModel,
    ) -> Vec<SelectionArrowState> {
        if !matches!(
            self.state.interaction,
            InteractionState::MarqueeSelection(_)
        ) {
            return Vec::new();
        }

        let Some(rect) = &self.state.ui.marquee else {
            return Vec::new();
        };
        let bounds = AxisAlignedBounds {
            left: rect.center.x - rect.width / 2.0,
            top: rect.center.y - rect.height / 2.0,
            right: rect.center.x + rect.width / 2.0,
            bottom: rect.center.y + rect.height / 2.0,
        };
        let ids = self.selection_ids_intersecting_bounds(document, bounds, ToolSelectionScope::All);
        Self::selection_arrows_from_ids(document, &ids)
    }

    pub(crate) fn selection_elements_from_ids(
        document: &DocumentModel,
        ids: &[ElementId],
    ) -> Vec<SelectionRectState> {
        ids.iter()
            .filter_map(|id| {
                document
                    .element_rect_proxy(*id)
                    .map(|rect| SelectionRectState { id: *id, rect })
            })
            .collect()
    }

    pub(crate) fn selection_arrows_from_ids(
        document: &DocumentModel,
        ids: &[ElementId],
    ) -> Vec<SelectionArrowState> {
        ids.iter()
            .filter_map(|id| {
                document
                    .arrow(*id)
                    .ok()
                    .cloned()
                    .map(|arrow| SelectionArrowState { id: *id, arrow })
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::EngineConfig;
    use snow_draw_engine_document::{
        ElementMeta, SerialNumberData, Transaction, resolve_serial_number_diameter,
    };

    fn assert_close(actual: f64, expected: f64) {
        assert!(
            (actual - expected).abs() <= f64::EPSILON,
            "expected {actual} to equal {expected}"
        );
    }

    #[test]
    fn document_change_refreshes_selected_serial_number_bounds() {
        let mut document = DocumentModel::new();
        let id = document.peek_next_element_id();
        let mut serial = SerialNumberData {
            font_size: 42.0,
            ..SerialNumberData::default()
        };
        serial.diameter =
            resolve_serial_number_diameter(serial.number, serial.font_size, serial.diameter);

        let mut transaction = Transaction::new("insert serial number");
        transaction.insert_serial_number(id, ElementMeta::default(), serial.clone());
        document.apply_transaction(transaction).unwrap();

        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_selection_state_with_document(Some(&document), vec![id], Some(id));
        let snapshot = editor.capture_document_sync_snapshot(&document);
        let old_bounds = editor.state.selection.bounds.unwrap();

        serial.font_size = 16.0;
        serial.diameter = resolve_serial_number_diameter(
            serial.number,
            serial.font_size,
            SerialNumberData::default().diameter,
        );
        let mut transaction = Transaction::new("update serial number");
        transaction.update_serial_number(id, serial.clone());
        document.apply_transaction(transaction).unwrap();

        editor.sync_selection_after_document_change(&document, &snapshot.selection);

        let next_bounds = editor.state.selection.bounds.unwrap();
        assert!(next_bounds.width < old_bounds.width);
        assert_close(next_bounds.width, serial.diameter);
        assert_close(next_bounds.height, serial.diameter);
    }
}
