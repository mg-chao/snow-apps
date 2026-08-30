use std::collections::HashMap;

use snow_draw_engine_core::{ErrorCode, Point};
use snow_draw_engine_document::{
    ElementData, ElementId, ElementMeta, TextLayoutSize, Transaction,
    serial_number_with_label_style, serial_number_with_selection_rect, text_hit_test,
    text_with_auto_resize_layout, text_with_content_and_layout, validate_arrow, validate_filter,
    validate_free_draw, validate_pen_filter, validate_rectangle, validate_serial_number,
    validate_text,
};
use snow_draw_engine_model::DocumentModel;

use crate::{
    ActiveTool, ApplyTransactionCommand, Editor, EditorCommand, SelectionRectState,
    SerialNumberTextOperation, TextDraftCommit, TextLayoutOverride,
    geometry::element_hit_tolerance,
    polyline_simplification::simplify_pen_filter_geometry,
    state::ResizeHandle,
    style::stepped_font_size,
    text::{
        SerialNumberTextCreationRequest, TextSelectionResizeHandle,
        create_serial_number_text_creation_plan, text_layout_override_size,
        text_with_committed_draft, text_with_selection_rect, text_with_style_attributes,
    },
};

pub(crate) fn append_selection_element_update(
    transaction: &mut Transaction,
    document: &DocumentModel,
    preview: SelectionRectState,
    resize_handle: Option<ResizeHandle>,
    single_text_resize: bool,
    single_text_resize_font_size: Option<f64>,
) -> Result<(), ErrorCode> {
    if document.rectangle(preview.id).is_ok() {
        validate_rectangle(&preview.rect)?;
        transaction.update_rectangle(preview.id, preview.rect);
        return Ok(());
    }
    if let Ok(filter) = document.filter(preview.id) {
        let mut updated = *filter;
        updated.center = preview.rect.center;
        updated.width = preview.rect.width;
        updated.height = preview.rect.height;
        updated.rotation = preview.rect.rotation;
        updated.opacity = preview.rect.opacity;
        validate_filter(&updated)?;
        if *filter != updated {
            transaction.update_filter(preview.id, updated);
        }
        return Ok(());
    }
    if let Ok(filter) = document.pen_filter(preview.id) {
        let mut updated = filter.clone();
        // Selection proxies represent the painted outer contour. Convert back
        // to the raw centerline rectangle before persisting the filter.
        updated.width = (preview.rect.width - filter.stroke_width.max(0.0)).max(0.0);
        updated.height = (preview.rect.height - filter.stroke_width.max(0.0)).max(0.0);
        updated.x = preview.rect.center.x - updated.width / 2.0;
        updated.y = preview.rect.center.y - updated.height / 2.0;
        updated.rotation = preview.rect.rotation;
        updated.opacity = preview.rect.opacity;
        let geometry_changed = updated.x != filter.x
            || updated.y != filter.y
            || updated.width != filter.width
            || updated.height != filter.height
            || updated.rotation != filter.rotation;
        if geometry_changed {
            simplify_pen_filter_geometry(&mut updated);
        }
        validate_pen_filter(&updated)?;
        if filter != &updated {
            transaction.update_pen_filter(preview.id, updated);
        }
        return Ok(());
    }
    if let Ok(free_draw) = document.free_draw(preview.id) {
        let mut updated = free_draw.clone();
        updated.x = preview.rect.center.x - preview.rect.width / 2.0;
        updated.y = preview.rect.center.y - preview.rect.height / 2.0;
        updated.width = preview.rect.width;
        updated.height = preview.rect.height;
        updated.rotation = preview.rect.rotation;
        updated.opacity = preview.rect.opacity;
        validate_free_draw(&updated)?;
        if free_draw != &updated {
            transaction.update_free_draw(preview.id, updated);
        }
        return Ok(());
    }
    if let Ok(text) = document.text(preview.id) {
        let updated = text_with_selection_rect(
            text,
            preview.rect,
            resize_handle.map(|handle| TextSelectionResizeHandle {
                x_sign: handle.x_sign(),
                y_sign: handle.y_sign(),
            }),
            single_text_resize,
            single_text_resize_font_size,
        );
        validate_text(&updated)?;
        if *text != updated {
            transaction.update_text(preview.id, updated);
        }
        return Ok(());
    }
    if let Ok(serial) = document.serial_number(preview.id) {
        let updated = serial_number_with_selection_rect(serial, preview.rect);
        validate_serial_number(&updated)?;
        if *serial != updated {
            transaction.update_serial_number(preview.id, updated);
        }
    }
    Ok(())
}

pub(crate) fn next_serial_number(document: &DocumentModel) -> i64 {
    document
        .paint_order()
        .iter()
        .filter_map(|id| document.serial_number(*id).ok())
        .map(|serial| serial.number.max(0))
        .max()
        .unwrap_or(0)
        .saturating_add(1)
        .max(1)
}

pub(crate) fn append_delete_dependencies(
    transaction: &mut Transaction,
    document: &DocumentModel,
    ids: &[ElementId],
) {
    let mut delete_ids = Vec::new();
    for id in ids.iter().copied() {
        if !delete_ids.contains(&id) && document.element(id).is_ok() {
            delete_ids.push(id);
        }
        if let Some(text_id) = document.bound_text_id_for_serial_number(id)
            && !delete_ids.contains(&text_id)
        {
            delete_ids.push(text_id);
        }
    }

    for serial_id in ids
        .iter()
        .copied()
        .filter(|id| document.text(*id).is_ok())
        .flat_map(|text_id| document.serial_number_ids_with_text(text_id))
    {
        if delete_ids.contains(&serial_id) {
            continue;
        }
        if let Ok(serial) = document.serial_number(serial_id) {
            let mut updated = serial.clone();
            updated.text_element_id = None;
            if updated != *serial {
                transaction.update_serial_number(serial_id, updated);
            }
        }
    }

    for id in delete_ids {
        transaction.remove_element(id);
    }
}

pub(crate) fn expanded_duplicate_ids(
    document: &DocumentModel,
    selected_ids: &[ElementId],
) -> Vec<ElementId> {
    let mut ids = Vec::new();
    for id in selected_ids.iter().copied() {
        if !ids.contains(&id) && document.element(id).is_ok() {
            ids.push(id);
        }
        if let Some(text_id) = document.bound_text_id_for_serial_number(id)
            && !ids.contains(&text_id)
        {
            ids.push(text_id);
        }
    }
    ids
}

pub(crate) fn duplicate_id_map(
    document: &DocumentModel,
    ids: &[ElementId],
) -> HashMap<ElementId, ElementId> {
    let mut next = document.peek_next_element_id();
    let mut map = HashMap::new();
    for id in ids.iter().copied() {
        map.insert(id, next);
        next.index = next.index.saturating_add(1);
    }
    map
}

impl Editor {
    pub fn hit_text_at(&self, document: &DocumentModel, point: Point<f64>) -> Option<ElementId> {
        let active_text = self.active_text_draft_existing_id().and_then(|id| {
            self.active_text_draft_text_for_id(id)
                .map(|text| (id, text))
        });
        let hit_tolerance = element_hit_tolerance(self.camera().zoom);
        for id in document.paint_order().iter().rev() {
            let Ok(element) = document.element(*id) else {
                continue;
            };
            if !element.meta.visible {
                continue;
            }
            if active_text
                .as_ref()
                .is_some_and(|(active_id, _)| id == active_id)
            {
                let Some((_, text)) = active_text.as_ref() else {
                    continue;
                };
                if text_hit_test(text, point, hit_tolerance) {
                    return Some(*id);
                }
                continue;
            }
            let Ok(text) = document.text(*id) else {
                continue;
            };
            if text_hit_test(text, point, hit_tolerance) {
                return Some(*id);
            }
        }
        None
    }

    pub fn selected_ids(&self) -> Vec<ElementId> {
        self.state.selection.ids.clone()
    }

    pub fn select_element(
        &mut self,
        document: &DocumentModel,
        id: ElementId,
    ) -> Result<(), ErrorCode> {
        document.element(id)?;
        self.set_selection_state_with_document(Some(document), vec![id], Some(id));
        Ok(())
    }

    pub fn update_text_element(
        &mut self,
        document: &DocumentModel,
        id: ElementId,
        text_content: impl Into<String>,
        layout: TextLayoutSize,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        let current = document.text(id)?.clone();
        let text_content = text_content.into();
        let history_undo_snapshot = self.capture_document_sync_snapshot(document);
        let mut transaction = Transaction::new(if text_content.trim().is_empty() {
            "delete text"
        } else {
            "edit text"
        });
        if text_content.trim().is_empty() {
            append_delete_dependencies(&mut transaction, document, &[id]);
        } else {
            let updated = text_with_content_and_layout(&current, text_content, layout)?;
            validate_text(&updated)?;
            if updated == current {
                self.clear_selection();
                return Ok(None);
            }
            transaction.update_text(id, updated);
        }
        if transaction.is_empty() {
            self.clear_selection();
            return Ok(None);
        }
        self.clear_selection();
        Ok(Some(EditorCommand::ApplyTransaction(
            ApplyTransactionCommand::with_history_undo_snapshot(transaction, history_undo_snapshot),
        )))
    }

    pub fn commit_text_draft(
        &mut self,
        document: &DocumentModel,
        draft: TextDraftCommit,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        // Selection transforms are staged in the active draft while text editing is focused.
        // Preserve its rotation when the text payload is committed; the commit payload does not
        // carry selection transforms.
        let active_text_rotation = draft
            .existing_id()
            .and_then(|id| self.active_text_draft_rect_for_id(id))
            .map(|rect| rect.rotation);
        self.clear_active_text_draft_presentation();
        let text_is_empty = draft.text_is_empty();
        let existing_id = draft.existing_id();
        if existing_id.is_none() && text_is_empty {
            return Ok(None);
        }

        let history_undo_snapshot = self.capture_document_sync_snapshot(document);
        let mut transaction = Transaction::new(if text_is_empty {
            "delete text"
        } else if existing_id.is_some() {
            "edit text"
        } else {
            "create text"
        });

        if let Some(id) = existing_id {
            if text_is_empty {
                append_delete_dependencies(&mut transaction, document, &[id]);
            } else {
                let current = document.text(id)?.clone();
                let mut updated = text_with_committed_draft(&current, &draft)?;
                if let Some(rotation) = active_text_rotation {
                    updated.rotation = rotation;
                }
                validate_text(&updated)?;
                if current != updated {
                    transaction.update_text(id, updated);
                }
            }
        } else {
            let updated = text_with_committed_draft(&self.state.default_text, &draft)?;
            validate_text(&updated)?;
            transaction.insert_text(
                document.peek_next_element_id(),
                ElementMeta::default(),
                updated,
            );
        }

        if draft.update_default_style && !text_is_empty {
            self.state.default_text =
                text_with_style_attributes(&self.state.default_text, &draft.style);
        }
        self.clear_selection();
        if transaction.is_empty() {
            return Ok(None);
        }
        Ok(Some(EditorCommand::ApplyTransaction(
            ApplyTransactionCommand::with_history_undo_snapshot(transaction, history_undo_snapshot),
        )))
    }

    pub fn delete_selected(
        &mut self,
        document: &DocumentModel,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        if self.state.selection.is_empty() {
            return Ok(None);
        }
        let ids = self.state.selection.ids.clone();
        let history_undo_snapshot = self.capture_document_sync_snapshot(document);
        let mut transaction = Transaction::new("delete selection");
        append_delete_dependencies(&mut transaction, document, &ids);
        if transaction.is_empty() {
            return Ok(None);
        }
        self.clear_selection();
        Ok(Some(EditorCommand::ApplyTransaction(
            ApplyTransactionCommand::with_history_undo_snapshot(transaction, history_undo_snapshot),
        )))
    }

    pub fn duplicate_selected(
        &mut self,
        document: &DocumentModel,
        offset: Point<f64>,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        if self.state.selection.is_empty() {
            return Ok(None);
        }
        let history_undo_snapshot = self.capture_document_sync_snapshot(document);
        let ids = expanded_duplicate_ids(document, &self.state.selection.ids);
        if ids.is_empty() {
            return Ok(None);
        }
        let id_map = duplicate_id_map(document, &ids);
        let mut transaction = Transaction::new("duplicate selection");
        let mut next_selection = Vec::new();
        for id in ids {
            let Some(new_id) = id_map.get(&id).copied() else {
                continue;
            };
            let element = document.element(id)?;
            match &element.data {
                ElementData::Rectangle(rect) => {
                    let mut duplicate = *rect;
                    duplicate.center.x += offset.x;
                    duplicate.center.y += offset.y;
                    validate_rectangle(&duplicate)?;
                    transaction.insert_rectangle(new_id, element.meta, duplicate);
                }
                ElementData::Filter(filter) => {
                    let mut duplicate = *filter;
                    duplicate.center.x += offset.x;
                    duplicate.center.y += offset.y;
                    validate_filter(&duplicate)?;
                    transaction.insert_filter(new_id, element.meta, duplicate);
                }
                ElementData::PenFilter(filter) => {
                    let mut duplicate = filter.clone();
                    duplicate.x += offset.x;
                    duplicate.y += offset.y;
                    simplify_pen_filter_geometry(&mut duplicate);
                    validate_pen_filter(&duplicate)?;
                    transaction.insert_pen_filter(new_id, element.meta, duplicate);
                }
                ElementData::Arrow(arrow) => {
                    let mut duplicate = arrow.clone();
                    duplicate.x += offset.x;
                    duplicate.y += offset.y;
                    validate_arrow(&duplicate)?;
                    transaction.insert_arrow(new_id, element.meta, duplicate);
                }
                ElementData::FreeDraw(free_draw) => {
                    let mut duplicate = free_draw.clone();
                    duplicate.x += offset.x;
                    duplicate.y += offset.y;
                    validate_free_draw(&duplicate)?;
                    transaction.insert_free_draw(new_id, element.meta, duplicate);
                }
                ElementData::Text(text) => {
                    let mut duplicate = text.clone();
                    duplicate.center.x += offset.x;
                    duplicate.center.y += offset.y;
                    validate_text(&duplicate)?;
                    transaction.insert_text(new_id, element.meta, duplicate);
                }
                ElementData::SerialNumber(serial) => {
                    let mut duplicate = serial.clone();
                    duplicate.center.x += offset.x;
                    duplicate.center.y += offset.y;
                    duplicate.text_element_id = document
                        .bound_text_id_for_serial_number(id)
                        .and_then(|text_id| id_map.get(&text_id).copied());
                    validate_serial_number(&duplicate)?;
                    transaction.insert_serial_number(new_id, element.meta, duplicate);
                }
            }
            if self.state.selection.contains(id) {
                next_selection.push(new_id);
            }
        }
        if transaction.is_empty() {
            return Ok(None);
        }
        self.set_selection_state(next_selection.clone(), next_selection.last().copied());
        Ok(Some(EditorCommand::ApplyTransaction(
            ApplyTransactionCommand::with_history_undo_snapshot(transaction, history_undo_snapshot),
        )))
    }

    pub fn reorder_selected(
        &mut self,
        document: &DocumentModel,
        action: u32,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        if self.state.selection.is_empty() {
            return Ok(None);
        }
        let selected: std::collections::HashSet<_> =
            self.state.selection.ids.iter().copied().collect();
        let mut order = document.paint_order().to_vec();
        let before = order.clone();
        match action {
            0 => order.sort_by_key(|id| !selected.contains(id)),
            1 => {
                for index in 1..order.len() {
                    if selected.contains(&order[index]) && !selected.contains(&order[index - 1]) {
                        order.swap(index, index - 1);
                    }
                }
            }
            2 => {
                for index in (0..order.len().saturating_sub(1)).rev() {
                    if selected.contains(&order[index]) && !selected.contains(&order[index + 1]) {
                        order.swap(index, index + 1);
                    }
                }
            }
            3 => order.sort_by_key(|id| selected.contains(id)),
            _ => return Err(ErrorCode::InvalidArgument),
        }
        if order == before {
            return Ok(None);
        }
        let mut transaction = Transaction::new("reorder selection");
        transaction.reorder_elements(order, 0);
        Ok(Some(EditorCommand::ApplyTransaction(
            ApplyTransactionCommand::new(transaction),
        )))
    }

    pub fn delete_elements(
        &mut self,
        document: &DocumentModel,
        ids: &[ElementId],
        label: &str,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        if ids.is_empty() {
            return Ok(None);
        }
        let history_undo_snapshot = self.capture_document_sync_snapshot(document);
        let mut transaction = Transaction::new(label);
        append_delete_dependencies(&mut transaction, document, ids);
        if transaction.is_empty() {
            return Ok(None);
        }
        self.state.selection.ids.retain(|id| !ids.contains(id));
        Ok(Some(EditorCommand::ApplyTransaction(
            ApplyTransactionCommand::with_history_undo_snapshot(transaction, history_undo_snapshot),
        )))
    }

    pub fn set_selected_opacity(
        &mut self,
        document: &DocumentModel,
        opacity: f64,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        if !opacity.is_finite() || !(0.0..=1.0).contains(&opacity) {
            return Err(ErrorCode::InvalidArgument);
        }
        let mut transaction = Transaction::new("set selection opacity");
        for id in &self.state.selection.ids {
            let element = document.element(*id)?;
            match &element.data {
                ElementData::Rectangle(value) => {
                    if value.is_spotlight() {
                        continue;
                    }
                    let mut updated = *value;
                    updated.opacity = opacity;
                    if updated != *value {
                        transaction.update_rectangle(*id, updated);
                    }
                }
                ElementData::Filter(value) => {
                    let mut updated = *value;
                    updated.opacity = opacity;
                    if updated != *value {
                        transaction.update_filter(*id, updated);
                    }
                }
                ElementData::PenFilter(value) => {
                    let mut updated = value.clone();
                    updated.opacity = opacity;
                    if updated != *value {
                        transaction.update_pen_filter(*id, updated);
                    }
                }
                ElementData::Arrow(value) => {
                    let mut updated = value.clone();
                    updated.opacity = opacity;
                    if updated != *value {
                        transaction.update_arrow(*id, updated);
                    }
                }
                ElementData::FreeDraw(value) => {
                    let mut updated = value.clone();
                    updated.opacity = opacity;
                    if updated != *value {
                        transaction.update_free_draw(*id, updated);
                    }
                }
                ElementData::Text(value) => {
                    let mut updated = value.clone();
                    updated.opacity = opacity;
                    if updated != *value {
                        transaction.update_text(*id, updated);
                    }
                }
                ElementData::SerialNumber(value) => {
                    let mut updated = value.clone();
                    updated.opacity = opacity;
                    if updated != *value {
                        transaction.update_serial_number(*id, updated);
                    }
                }
            }
        }
        if transaction.is_empty() {
            return Ok(None);
        }
        Ok(Some(EditorCommand::ApplyTransaction(
            ApplyTransactionCommand::new(transaction),
        )))
    }

    pub fn adjust_selected_serial_numbers(
        &mut self,
        document: &DocumentModel,
        delta: i64,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        if self.state.selection.is_empty() || delta == 0 {
            return Ok(None);
        }
        let history_undo_snapshot = self.capture_document_sync_snapshot(document);
        let mut transaction = Transaction::new(if delta > 0 {
            "increase serial number"
        } else {
            "decrease serial number"
        });
        let selected_ids = self.state.selection.ids.clone();
        for id in selected_ids {
            let Ok(current) = document.serial_number(id) else {
                continue;
            };
            let next_number = if delta < 0 {
                current.number.saturating_sub(delta.saturating_abs()).max(0)
            } else {
                current.number.saturating_add(delta)
            };
            let updated = serial_number_with_label_style(current, next_number, current.font_size);
            if updated != *current {
                validate_serial_number(&updated)?;
                transaction.update_serial_number(id, updated);
            }
        }
        if transaction.is_empty() {
            return Ok(None);
        }
        Ok(Some(EditorCommand::ApplyTransaction(
            ApplyTransactionCommand::with_history_undo_snapshot(transaction, history_undo_snapshot),
        )))
    }

    pub fn create_serial_number_text_elements(
        &mut self,
        document: &DocumentModel,
        layout: TextLayoutSize,
    ) -> Result<SerialNumberTextOperation, ErrorCode> {
        let plan = create_serial_number_text_creation_plan(
            document,
            SerialNumberTextCreationRequest {
                selected_ids: &self.state.selection.ids,
                default_text: &self.state.default_text,
                measured_layout: layout,
                next_text_id: document.peek_next_element_id(),
            },
        )?;
        if plan.transaction.is_empty() {
            return Ok(SerialNumberTextOperation {
                command: None,
                single_text_id: plan.single_text_id,
            });
        }
        let history_undo_snapshot = self.capture_document_sync_snapshot(document);
        Ok(SerialNumberTextOperation {
            command: Some(EditorCommand::ApplyTransaction(
                ApplyTransactionCommand::with_history_undo_snapshot(
                    plan.transaction,
                    history_undo_snapshot,
                ),
            )),
            single_text_id: plan.single_text_id,
        })
    }

    pub fn adjust_font_size_step(
        &mut self,
        document: &DocumentModel,
        increase: bool,
        layouts: &[TextLayoutOverride],
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        let base = self
            .selected_average_text_or_serial_font_size(document)
            .unwrap_or_else(|| {
                if self.state.active_tool == ActiveTool::SerialNumber {
                    self.state.default_serial_number.font_size
                } else {
                    self.state.default_text.font_size
                }
            });
        let next = stepped_font_size(base, increase);
        if (next - base).abs() <= f64::EPSILON {
            return Ok(None);
        }

        let history_undo_snapshot = self.capture_document_sync_snapshot(document);
        let mut transaction = Transaction::new("adjust font size");
        for id in self.state.selection.ids.iter().copied() {
            if let Ok(current) = document.text(id) {
                let mut updated = current.clone();
                updated.font_size = next;
                if updated.auto_resize {
                    let layout = text_layout_override_size(layouts, id)?;
                    updated = text_with_auto_resize_layout(&updated, layout)?;
                }
                validate_text(&updated)?;
                if updated != *current {
                    transaction.update_text(id, updated);
                }
                continue;
            }
            if let Ok(current) = document.serial_number(id) {
                let updated = serial_number_with_label_style(current, current.number, next);
                validate_serial_number(&updated)?;
                if updated != *current {
                    transaction.update_serial_number(id, updated);
                }
            }
        }

        self.state.default_text.font_size = next;
        if self.state.active_tool == ActiveTool::SerialNumber
            || self
                .state
                .selection
                .ids
                .iter()
                .any(|id| document.serial_number(*id).is_ok())
        {
            self.state.default_serial_number = serial_number_with_label_style(
                &self.state.default_serial_number,
                self.state.default_serial_number.number,
                next,
            );
        }

        if transaction.is_empty() {
            self.bump_overlay_state_revision();
            return Ok(None);
        }
        Ok(Some(EditorCommand::ApplyTransaction(
            ApplyTransactionCommand::with_history_undo_snapshot(transaction, history_undo_snapshot),
        )))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{ActiveTextDraftPresentation, ActiveTextDraftTarget, TextCommitTarget, TextStyle};
    use snow_draw_engine_core::{ColorRgba8, EngineConfig, PathSegmentMode};
    use snow_draw_engine_document::{
        ArrowData, CanvasFilterType, ElementData, ElementMeta, FreeDrawData, Operation,
        PenFilterData, RectangleData, SerialNumberData, TextData,
    };

    fn apply_editor_command(document: &mut DocumentModel, command: EditorCommand) {
        let EditorCommand::ApplyTransaction(command) = command else {
            panic!("expected transaction command");
        };
        document.apply_transaction(command.transaction).unwrap();
    }

    fn insert_rectangle(document: &mut DocumentModel, rectangle: RectangleData) -> ElementId {
        let id = document.peek_next_element_id();
        let mut transaction = Transaction::new("insert rectangle");
        transaction.insert_rectangle(id, ElementMeta::default(), rectangle);
        document.apply_transaction(transaction).unwrap();
        id
    }

    fn insert_arrow(document: &mut DocumentModel, arrow: ArrowData) -> ElementId {
        let id = document.peek_next_element_id();
        let mut transaction = Transaction::new("insert arrow");
        transaction.insert_arrow(id, ElementMeta::default(), arrow);
        document.apply_transaction(transaction).unwrap();
        id
    }

    fn insert_free_draw(document: &mut DocumentModel, free_draw: FreeDrawData) -> ElementId {
        let id = document.peek_next_element_id();
        let mut transaction = Transaction::new("insert free draw");
        transaction.insert_free_draw(id, ElementMeta::default(), free_draw);
        document.apply_transaction(transaction).unwrap();
        id
    }

    fn insert_pen_filter(document: &mut DocumentModel, filter: PenFilterData) -> ElementId {
        let id = document.peek_next_element_id();
        let mut transaction = Transaction::new("insert pen filter");
        transaction.insert_pen_filter(id, ElementMeta::default(), filter);
        document.apply_transaction(transaction).unwrap();
        id
    }

    #[test]
    fn pen_filter_selection_uses_outer_contour_and_commits_raw_geometry() {
        let mut document = DocumentModel::new();
        let filter = PenFilterData::from_global_points(
            &[Point::new(10.0, 20.0), Point::new(110.0, 60.0)],
            CanvasFilterType::Mosaic,
            0.5,
            20.0,
            1.0,
        )
        .unwrap();
        let id = insert_pen_filter(&mut document, filter.clone());
        let proxy = document.element_rect_proxy(id).unwrap();

        assert_eq!(proxy.center, filter.center());
        assert_eq!(proxy.width, filter.width + filter.stroke_width);
        assert_eq!(proxy.height, filter.height + filter.stroke_width);

        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.select_element(&document, id).unwrap();
        let presentation = editor.presentation_state(&document);
        let selection_bounds = presentation.selection_bounds.unwrap();
        assert_eq!(selection_bounds.width, proxy.width);
        assert_eq!(selection_bounds.height, proxy.height);

        let preview = SelectionRectState {
            id,
            rect: RectangleData {
                center: Point::new(200.0, 300.0),
                ..proxy
            },
        };
        let mut transaction = Transaction::new("move pen filter");
        append_selection_element_update(&mut transaction, &document, preview, None, false, None)
            .unwrap();
        document.apply_transaction(transaction).unwrap();

        let updated = document.pen_filter(id).unwrap();
        assert_eq!(updated.x, 150.0);
        assert_eq!(updated.y, 280.0);
        assert_eq!(updated.width, filter.width);
        assert_eq!(updated.height, filter.height);
    }

    #[test]
    fn selection_rect_commit_updates_free_draw_transform() {
        let mut document = DocumentModel::new();
        let id = insert_free_draw(
            &mut document,
            FreeDrawData::from_global_vertices(
                &[Point::new(10.0, 20.0), Point::new(30.0, 40.0)],
                vec![PathSegmentMode::Curve],
                false,
                snow_draw_engine_document::FreeDrawStyle {
                    stroke: ColorRgba8::default(),
                    stroke_width: 2.0,
                    stroke_style: snow_draw_engine_document::StrokeStyle::Solid,
                    fill: ColorRgba8::default(),
                    fill_style: snow_draw_engine_document::FillStyle::Solid,
                    opacity: 1.0,
                },
            )
            .unwrap(),
        );
        let preview = SelectionRectState {
            id,
            rect: RectangleData {
                center: Point::new(100.0, 200.0),
                width: 80.0,
                height: 40.0,
                rotation: 0.5,
                opacity: 0.6,
                ..document.element_rect_proxy(id).unwrap()
            },
        };
        let mut transaction = Transaction::new("transform free draw");
        append_selection_element_update(&mut transaction, &document, preview, None, false, None)
            .unwrap();
        document.apply_transaction(transaction).unwrap();

        let updated = document.free_draw(id).unwrap();
        assert_eq!((updated.x, updated.y), (60.0, 180.0));
        assert_eq!((updated.width, updated.height), (80.0, 40.0));
        assert_eq!(updated.rotation, 0.5);
        assert_eq!(updated.opacity, 0.6);
        assert_eq!(updated.vertices, vec![[0.0, 0.0], [1.0, 1.0]]);
    }

    fn rectangle_with_colors(fill: ColorRgba8, stroke: ColorRgba8) -> RectangleData {
        RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: Point::new(50.0, 50.0),
            width: 80.0,
            height: 60.0,
            rotation: 0.0,
            fill,
            fill_style: snow_draw_engine_document::FillStyle::Solid,
            stroke,
            stroke_width: 2.0,
            stroke_style: snow_draw_engine_document::StrokeStyle::Solid,
            corner_radii: snow_draw_engine_core::CornerRadii::default(),
            opacity: 1.0,
        }
    }

    fn insert_text(document: &mut DocumentModel, text: TextData) -> ElementId {
        let id = document.peek_next_element_id();
        let mut transaction = Transaction::new("insert text");
        transaction.insert_text(id, ElementMeta::default(), text);
        document.apply_transaction(transaction).unwrap();
        id
    }

    fn insert_serial_number(document: &mut DocumentModel, serial: SerialNumberData) -> ElementId {
        let id = document.peek_next_element_id();
        let mut transaction = Transaction::new("insert serial number");
        transaction.insert_serial_number(id, ElementMeta::default(), serial);
        document.apply_transaction(transaction).unwrap();
        id
    }

    fn text_style(font_size: f64, color: ColorRgba8) -> TextStyle {
        TextStyle {
            color,
            font_size,
            font_family: Some("Inter".to_owned()),
            fill: ColorRgba8 {
                r: 12,
                g: 34,
                b: 56,
                a: 255,
            },
            fill_style: snow_draw_engine_document::FillStyle::Line,
            stroke: ColorRgba8 {
                r: 200,
                g: 10,
                b: 20,
                a: 255,
            },
            stroke_width: 2.0,
            corner_radii: snow_draw_engine_core::CornerRadii::splat(3.0),
            horizontal_align: snow_draw_engine_document::TextHorizontalAlign::Center,
            vertical_align: snow_draw_engine_document::TextVerticalAlign::Bottom,
            opacity: 0.7,
        }
    }

    #[test]
    fn hit_text_at_uses_active_draft_geometry_and_excludes_committed_text_geometry() {
        let mut document = DocumentModel::new();
        let id = insert_text(
            &mut document,
            TextData {
                center: Point::new(0.0, 0.0),
                width: 40.0,
                height: 20.0,
                text: "committed".to_owned(),
                auto_resize: false,
                ..TextData::default()
            },
        );
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        let draft_text = TextData {
            center: Point::new(160.0, 0.0),
            width: 60.0,
            height: 30.0,
            text: "draft".to_owned(),
            auto_resize: false,
            ..document.text(id).unwrap().clone()
        };

        editor
            .set_active_text_draft_presentation(
                &document,
                ActiveTextDraftPresentation {
                    target: ActiveTextDraftTarget::Existing(id),
                    revision: 1,
                    text: draft_text,
                },
            )
            .unwrap();

        assert_eq!(
            editor.hit_text_at(&document, Point::new(160.0, 0.0)),
            Some(id)
        );
        assert_eq!(editor.hit_text_at(&document, Point::new(0.0, 0.0)), None);
    }

    #[test]
    fn selected_rectangle_opacity_can_recover_from_zero() {
        let mut document = DocumentModel::new();
        let id = insert_rectangle(
            &mut document,
            rectangle_with_colors(
                ColorRgba8 {
                    r: 10,
                    g: 20,
                    b: 30,
                    a: 255,
                },
                ColorRgba8 {
                    r: 40,
                    g: 50,
                    b: 60,
                    a: 255,
                },
            ),
        );
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.select_element(&document, id).unwrap();

        let command = editor
            .set_selected_opacity(&document, 0.0)
            .unwrap()
            .expect("zero opacity should update the rectangle");
        apply_editor_command(&mut document, command);
        assert_eq!(document.rectangle(id).unwrap().opacity, 0.0);
        assert_eq!(document.rectangle(id).unwrap().fill.a, 255);
        assert_eq!(document.rectangle(id).unwrap().stroke.a, 255);

        let command = editor
            .set_selected_opacity(&document, 0.5)
            .unwrap()
            .expect("non-zero opacity should restore the rectangle");
        apply_editor_command(&mut document, command);
        assert_eq!(document.rectangle(id).unwrap().opacity, 0.5);
        assert_eq!(document.rectangle(id).unwrap().fill.a, 255);
        assert_eq!(document.rectangle(id).unwrap().stroke.a, 255);
    }

    #[test]
    fn selected_rectangle_opacity_keeps_transparent_fill_disabled() {
        let mut document = DocumentModel::new();
        let id = insert_rectangle(
            &mut document,
            rectangle_with_colors(
                ColorRgba8 {
                    r: 10,
                    g: 20,
                    b: 30,
                    a: 0,
                },
                ColorRgba8 {
                    r: 40,
                    g: 50,
                    b: 60,
                    a: 255,
                },
            ),
        );
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.select_element(&document, id).unwrap();

        let command = editor
            .set_selected_opacity(&document, 0.0)
            .unwrap()
            .expect("zero opacity should update the rectangle stroke");
        apply_editor_command(&mut document, command);
        let command = editor
            .set_selected_opacity(&document, 0.5)
            .unwrap()
            .expect("non-zero opacity should restore the rectangle stroke");
        apply_editor_command(&mut document, command);

        assert_eq!(document.rectangle(id).unwrap().opacity, 0.5);
        assert_eq!(document.rectangle(id).unwrap().fill.a, 0);
        assert_eq!(document.rectangle(id).unwrap().stroke.a, 255);
    }

    #[test]
    fn selected_opacity_skips_spotlight_cutouts() {
        let mut document = DocumentModel::new();
        let spotlight_id = insert_rectangle(
            &mut document,
            rectangle_with_colors(ColorRgba8::default(), ColorRgba8::default()).into_spotlight(),
        );
        let rectangle_id = insert_rectangle(
            &mut document,
            rectangle_with_colors(
                ColorRgba8 {
                    r: 10,
                    g: 20,
                    b: 30,
                    a: 255,
                },
                ColorRgba8 {
                    r: 40,
                    g: 50,
                    b: 60,
                    a: 255,
                },
            ),
        );
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_selection_state(vec![spotlight_id], Some(spotlight_id));
        assert!(
            editor
                .set_selected_opacity(&document, 0.25)
                .unwrap()
                .is_none()
        );

        editor.set_selection_state(vec![spotlight_id, rectangle_id], Some(rectangle_id));
        let command = editor
            .set_selected_opacity(&document, 0.5)
            .unwrap()
            .expect("the non-Spotlight member should still update");
        apply_editor_command(&mut document, command);
        assert_eq!(document.rectangle(spotlight_id).unwrap().opacity, 1.0);
        assert_eq!(document.rectangle(rectangle_id).unwrap().opacity, 0.5);
    }

    #[test]
    fn selected_arrow_opacity_can_recover_from_zero() {
        let mut document = DocumentModel::new();
        let stroke = ColorRgba8 {
            r: 40,
            g: 50,
            b: 60,
            a: 255,
        };
        let id = insert_arrow(
            &mut document,
            ArrowData::from_global_points(
                &[Point::new(0.0, 0.0), Point::new(100.0, 100.0)],
                stroke,
                2.0,
                snow_draw_engine_core::arrow::ArrowStrokeStyle::Solid,
                snow_draw_engine_core::arrow::ArrowType::Straight,
                None,
                None,
            )
            .unwrap(),
        );
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.select_element(&document, id).unwrap();

        let command = editor
            .set_selected_opacity(&document, 0.0)
            .unwrap()
            .expect("zero opacity should update the arrow");
        apply_editor_command(&mut document, command);
        assert_eq!(document.arrow(id).unwrap().opacity, 0.0);
        assert_eq!(document.arrow(id).unwrap().stroke, stroke);

        let command = editor
            .set_selected_opacity(&document, 0.4)
            .unwrap()
            .expect("non-zero opacity should restore the arrow");
        apply_editor_command(&mut document, command);
        assert_eq!(document.arrow(id).unwrap().opacity, 0.4);
        assert_eq!(document.arrow(id).unwrap().stroke, stroke);
    }

    #[test]
    fn new_active_text_draft_clears_existing_selection() {
        let mut document = DocumentModel::new();
        let id = insert_text(
            &mut document,
            TextData {
                text: "selected".to_owned(),
                ..TextData::default()
            },
        );
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.select_element(&document, id).unwrap();

        editor
            .set_active_text_draft_presentation(
                &document,
                ActiveTextDraftPresentation {
                    target: ActiveTextDraftTarget::New,
                    revision: 1,
                    text: TextData {
                        text: String::new(),
                        ..TextData::default()
                    },
                },
            )
            .unwrap();

        assert!(editor.selected_ids().is_empty());
    }

    #[test]
    fn commit_text_draft_updates_existing_text_with_draft_style() {
        let mut document = DocumentModel::new();
        let id = insert_text(
            &mut document,
            TextData {
                text: "old".to_owned(),
                width: 80.0,
                height: 24.0,
                ..TextData::default()
            },
        );
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.select_element(&document, id).unwrap();
        // A selection transform leaves this angle staged in the active draft.
        let mut active_draft_text = document.text(id).unwrap().clone();
        active_draft_text.rotation = 0.75;
        editor
            .set_active_text_draft_presentation(
                &document,
                ActiveTextDraftPresentation {
                    target: ActiveTextDraftTarget::Existing(id),
                    revision: 1,
                    text: active_draft_text,
                },
            )
            .unwrap();

        let style = text_style(
            42.0,
            ColorRgba8 {
                r: 1,
                g: 2,
                b: 3,
                a: 255,
            },
        );
        let command = editor
            .commit_text_draft(
                &document,
                TextDraftCommit::new(
                    TextCommitTarget::Existing(id),
                    Point::new(10.0, 20.0),
                    "new",
                    TextLayoutSize {
                        width: 120.0,
                        height: 48.0,
                    },
                    style.clone(),
                    false,
                    true,
                ),
            )
            .unwrap()
            .expect("changed draft should produce a transaction");

        let EditorCommand::ApplyTransaction(command) = command else {
            panic!("expected transaction command");
        };
        let [
            Operation::UpdateElementData {
                id: updated_id,
                data,
            },
        ] = command.transaction.operations()
        else {
            panic!("expected a single text update operation");
        };
        assert_eq!(*updated_id, id);
        let ElementData::Text(updated) = data else {
            panic!("expected text update");
        };
        assert_eq!(updated.text, "new");
        assert_eq!(updated.center, Point::new(10.0, 20.0));
        assert_eq!(updated.rotation, 0.75);
        assert_eq!(updated.width, 120.0);
        assert_eq!(updated.height, 48.0);
        assert_eq!(updated.font_size, style.font_size);
        assert_eq!(updated.color, style.color);
        assert_eq!(updated.font_family, style.font_family);
        assert_eq!(updated.fill_style, style.fill_style);
        assert_eq!(updated.horizontal_align, style.horizontal_align);
        assert_eq!(updated.vertical_align, style.vertical_align);
        assert!(!updated.auto_resize);
        assert_eq!(editor.state.default_text.font_size, style.font_size);
        assert_eq!(editor.state.default_text.color, style.color);
        assert_eq!(editor.state.default_text.font_family, style.font_family);
        assert!(editor.state.selection.is_empty());
    }

    #[test]
    fn commit_text_draft_creates_text_with_draft_style() {
        let document = DocumentModel::new();
        let mut editor = Editor::new(EngineConfig::default()).unwrap();

        let style = text_style(
            31.0,
            ColorRgba8 {
                r: 9,
                g: 8,
                b: 7,
                a: 255,
            },
        );
        let command = editor
            .commit_text_draft(
                &document,
                TextDraftCommit::new(
                    TextCommitTarget::New,
                    Point::new(45.0, 67.0),
                    "created",
                    TextLayoutSize {
                        width: 180.0,
                        height: 64.0,
                    },
                    style.clone(),
                    true,
                    true,
                ),
            )
            .unwrap()
            .expect("non-empty new draft should produce a transaction");

        let EditorCommand::ApplyTransaction(command) = command else {
            panic!("expected transaction command");
        };
        let [Operation::InsertElement { id, data, .. }] = command.transaction.operations() else {
            panic!("expected a single text insert operation");
        };
        assert_eq!(*id, document.peek_next_element_id());
        let ElementData::Text(created) = data else {
            panic!("expected text insert");
        };
        assert_eq!(created.text, "created");
        assert_eq!(created.center, Point::new(45.0, 67.0));
        assert_eq!(created.width, 180.0);
        assert_eq!(created.height, 64.0);
        assert_eq!(created.font_size, style.font_size);
        assert_eq!(created.color, style.color);
        assert_eq!(created.font_family, style.font_family);
        assert_eq!(created.fill_style, style.fill_style);
        assert_eq!(created.horizontal_align, style.horizontal_align);
        assert_eq!(created.vertical_align, style.vertical_align);
        assert!(created.auto_resize);
        assert_eq!(editor.state.default_text.font_size, style.font_size);
        assert_eq!(editor.state.default_text.color, style.color);
        assert_eq!(editor.state.default_text.font_family, style.font_family);
        assert!(editor.state.selection.is_empty());
    }

    #[test]
    fn commit_text_draft_deletes_empty_existing_text_and_unbinds_serial_number() {
        let mut document = DocumentModel::new();
        let text_id = insert_text(
            &mut document,
            TextData {
                text: "bound".to_owned(),
                width: 80.0,
                height: 24.0,
                ..TextData::default()
            },
        );
        let serial_id = insert_serial_number(
            &mut document,
            SerialNumberData {
                text_element_id: Some(text_id),
                ..SerialNumberData::default()
            },
        );
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.select_element(&document, text_id).unwrap();

        let command = editor
            .commit_text_draft(
                &document,
                TextDraftCommit::new(
                    TextCommitTarget::Existing(text_id),
                    Point::new(0.0, 0.0),
                    "   ",
                    TextLayoutSize {
                        width: 1.0,
                        height: 1.0,
                    },
                    text_style(
                        12.0,
                        ColorRgba8 {
                            r: 0,
                            g: 0,
                            b: 0,
                            a: 255,
                        },
                    ),
                    true,
                    false,
                ),
            )
            .unwrap()
            .expect("empty existing draft should delete the text");

        let EditorCommand::ApplyTransaction(command) = command else {
            panic!("expected transaction command");
        };
        let [
            Operation::UpdateElementData {
                id: updated_id,
                data: ElementData::SerialNumber(updated_serial),
            },
            Operation::RemoveElement { id: removed_id },
        ] = command.transaction.operations()
        else {
            panic!("expected serial unbind followed by text removal");
        };
        assert_eq!(*updated_id, serial_id);
        assert_eq!(updated_serial.text_element_id, None);
        assert_eq!(*removed_id, text_id);
        assert!(editor.state.selection.is_empty());
    }

    #[test]
    fn delete_selected_serial_number_removes_existing_bound_text() {
        let mut document = DocumentModel::new();
        let text_id = insert_text(
            &mut document,
            TextData {
                text: "bound".to_owned(),
                width: 80.0,
                height: 24.0,
                ..TextData::default()
            },
        );
        let serial_id = insert_serial_number(
            &mut document,
            SerialNumberData {
                text_element_id: Some(text_id),
                ..SerialNumberData::default()
            },
        );
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.select_element(&document, serial_id).unwrap();

        let command = editor
            .delete_selected(&document)
            .unwrap()
            .expect("selected serial should be deleted");

        let EditorCommand::ApplyTransaction(command) = command else {
            panic!("expected transaction command");
        };
        let [
            Operation::RemoveElement {
                id: removed_serial_id,
            },
            Operation::RemoveElement {
                id: removed_text_id,
            },
        ] = command.transaction.operations()
        else {
            panic!("expected serial and bound text removals");
        };
        assert_eq!(*removed_serial_id, serial_id);
        assert_eq!(*removed_text_id, text_id);
        assert!(editor.state.selection.is_empty());
    }

    #[test]
    fn delete_selected_serial_number_ignores_missing_bound_text() {
        let mut document = DocumentModel::new();
        let missing_text_id = ElementId {
            index: 42,
            generation: 7,
        };
        let serial_id = insert_serial_number(
            &mut document,
            SerialNumberData {
                text_element_id: Some(missing_text_id),
                ..SerialNumberData::default()
            },
        );
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.select_element(&document, serial_id).unwrap();

        let command = editor
            .delete_selected(&document)
            .unwrap()
            .expect("selected serial should be deleted");

        let EditorCommand::ApplyTransaction(command) = command else {
            panic!("expected transaction command");
        };
        let [Operation::RemoveElement { id }] = command.transaction.operations() else {
            panic!("expected only the serial removal");
        };
        assert_eq!(*id, serial_id);
    }

    #[test]
    fn duplicate_selected_serial_number_duplicates_existing_bound_text() {
        let mut document = DocumentModel::new();
        let text_id = insert_text(
            &mut document,
            TextData {
                center: Point::new(100.0, 0.0),
                text: "bound".to_owned(),
                width: 80.0,
                height: 24.0,
                ..TextData::default()
            },
        );
        let serial_id = insert_serial_number(
            &mut document,
            SerialNumberData {
                center: Point::new(0.0, 0.0),
                text_element_id: Some(text_id),
                ..SerialNumberData::default()
            },
        );
        let duplicate_serial_id = document.peek_next_element_id();
        let duplicate_text_id = ElementId {
            index: duplicate_serial_id.index + 1,
            generation: duplicate_serial_id.generation,
        };
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.select_element(&document, serial_id).unwrap();

        let command = editor
            .duplicate_selected(&document, Point::new(12.0, 8.0))
            .unwrap()
            .expect("selected serial should be duplicated");

        let EditorCommand::ApplyTransaction(command) = command else {
            panic!("expected transaction command");
        };
        let [
            Operation::InsertElement {
                id: inserted_serial_id,
                data: ElementData::SerialNumber(inserted_serial),
                ..
            },
            Operation::InsertElement {
                id: inserted_text_id,
                data: ElementData::Text(inserted_text),
                ..
            },
        ] = command.transaction.operations()
        else {
            panic!("expected duplicated serial and bound text inserts");
        };
        assert_eq!(*inserted_serial_id, duplicate_serial_id);
        assert_eq!(inserted_serial.text_element_id, Some(duplicate_text_id));
        assert_eq!(inserted_serial.center, Point::new(12.0, 8.0));
        assert_eq!(*inserted_text_id, duplicate_text_id);
        assert_eq!(inserted_text.center, Point::new(112.0, 8.0));
        assert_eq!(editor.state.selection.ids, vec![duplicate_serial_id]);
    }

    #[test]
    fn duplicate_selected_serial_number_clears_missing_bound_text() {
        let mut document = DocumentModel::new();
        let missing_text_id = ElementId {
            index: 42,
            generation: 7,
        };
        let serial_id = insert_serial_number(
            &mut document,
            SerialNumberData {
                text_element_id: Some(missing_text_id),
                ..SerialNumberData::default()
            },
        );
        let duplicate_serial_id = document.peek_next_element_id();
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.select_element(&document, serial_id).unwrap();

        let command = editor
            .duplicate_selected(&document, Point::new(12.0, 8.0))
            .unwrap()
            .expect("selected serial should be duplicated");

        let EditorCommand::ApplyTransaction(command) = command else {
            panic!("expected transaction command");
        };
        let [
            Operation::InsertElement {
                id,
                data: ElementData::SerialNumber(inserted_serial),
                ..
            },
        ] = command.transaction.operations()
        else {
            panic!("expected only the duplicated serial insert");
        };
        assert_eq!(*id, duplicate_serial_id);
        assert_eq!(inserted_serial.text_element_id, None);
        assert_eq!(editor.state.selection.ids, vec![duplicate_serial_id]);
    }

    #[test]
    fn commit_text_draft_preserves_default_style_without_update_flag() {
        let mut document = DocumentModel::new();
        let id = insert_text(
            &mut document,
            TextData {
                text: "old".to_owned(),
                width: 80.0,
                height: 24.0,
                ..TextData::default()
            },
        );
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        let default_before = editor.state.default_text.clone();
        let style = text_style(
            42.0,
            ColorRgba8 {
                r: 1,
                g: 2,
                b: 3,
                a: 255,
            },
        );

        let command = editor
            .commit_text_draft(
                &document,
                TextDraftCommit::new(
                    TextCommitTarget::Existing(id),
                    Point::new(10.0, 20.0),
                    "new",
                    TextLayoutSize {
                        width: 120.0,
                        height: 48.0,
                    },
                    style,
                    false,
                    false,
                ),
            )
            .unwrap();

        assert!(command.is_some());
        assert_eq!(editor.state.default_text, default_before);
    }
}
