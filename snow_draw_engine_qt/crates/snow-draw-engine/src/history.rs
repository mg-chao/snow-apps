use serde::{Deserialize, Serialize};
use snow_draw_engine_core::ErrorCode;
use std::time::{Duration, Instant};

use snow_draw_engine_document::{
    ElementData, ElementId, FilterData, Operation, SerialNumberData, SpotlightConfig, Transaction,
    WatermarkConfig,
};
use snow_draw_engine_editor::DocumentSyncSnapshot;
use snow_draw_engine_model::{ApplyResult, DocumentModel};

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct HistoryEntry {
    pub label: String,
    pub undo: Transaction,
    pub redo: Transaction,
    pub undo_snapshot: DocumentSyncSnapshot,
    pub redo_snapshot: DocumentSyncSnapshot,
}

#[derive(Clone, Debug, PartialEq)]
pub struct HistoryApplyResult {
    pub restore_selection: bool,
    pub apply_result: ApplyResult,
    pub snapshot: DocumentSyncSnapshot,
}

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct HistoryStore {
    undo_stack: Vec<HistoryEntry>,
    redo_stack: Vec<HistoryEntry>,
    #[serde(skip)]
    last_watermark_text_commit: Option<Instant>,
    #[serde(skip)]
    last_spotlight_commit: Option<Instant>,
    #[serde(skip)]
    last_filter_commit: Option<(FilterCoalesceKey, Instant)>,
}

impl HistoryStore {
    pub fn validate_session(&self, document: &DocumentModel) -> Result<(), ErrorCode> {
        const MAX_HISTORY_ENTRIES: usize = 100_000;
        if self.undo_stack.len().saturating_add(self.redo_stack.len()) > MAX_HISTORY_ENTRIES {
            return Err(ErrorCode::InvalidArgument);
        }
        let validate_entry = |entry: &HistoryEntry| {
            if entry.label.len() > 16_384
                || entry.undo.operations().len() > 1_000_000
                || entry.redo.operations().len() > 1_000_000
            {
                return Err(ErrorCode::InvalidArgument);
            }
            Ok(())
        };
        for entry in self.undo_stack.iter().chain(self.redo_stack.iter()) {
            validate_entry(entry)?;
        }

        let mut undo_document = document.clone();
        for entry in self.undo_stack.iter().rev() {
            let before_undo = undo_document.document().clone();
            undo_document.apply_history_transaction(&entry.undo)?;
            entry.undo_snapshot.validate_session(&undo_document)?;
            let mut round_trip = undo_document.clone();
            round_trip.apply_history_transaction(&entry.redo)?;
            entry.redo_snapshot.validate_session(&round_trip)?;
            if !round_trip.document().has_same_session_content(&before_undo) {
                return Err(ErrorCode::InvalidArgument);
            }
        }
        let mut redo_document = document.clone();
        for entry in self.redo_stack.iter().rev() {
            let before_redo = redo_document.document().clone();
            redo_document.apply_history_transaction(&entry.redo)?;
            entry.redo_snapshot.validate_session(&redo_document)?;
            let mut round_trip = redo_document.clone();
            round_trip.apply_history_transaction(&entry.undo)?;
            entry.undo_snapshot.validate_session(&round_trip)?;
            if !round_trip.document().has_same_session_content(&before_redo) {
                return Err(ErrorCode::InvalidArgument);
            }
        }
        Ok(())
    }
}

impl HistoryStore {
    pub fn push_committed(
        &mut self,
        label: impl Into<String>,
        redo: Transaction,
        undo: Transaction,
        undo_snapshot: DocumentSyncSnapshot,
        redo_snapshot: DocumentSyncSnapshot,
    ) {
        let label = label.into();
        let now = Instant::now();
        if watermark_text_only_change(&undo, &redo) {
            let can_coalesce = self
                .last_watermark_text_commit
                .is_some_and(|at| now.duration_since(at) <= Duration::from_millis(220))
                && self.undo_stack.last().is_some_and(|last| {
                    watermark_text_only_change(&last.undo, &last.redo)
                        && watermark_config(&last.redo) == watermark_config(&undo)
                });
            if can_coalesce {
                if self
                    .undo_stack
                    .last()
                    .is_some_and(|last| watermark_config(&last.undo) == watermark_config(&redo))
                {
                    self.undo_stack.pop();
                    self.last_watermark_text_commit = None;
                } else if let Some(last) = self.undo_stack.last_mut() {
                    last.label = label;
                    last.redo = redo;
                    last.redo_snapshot = redo_snapshot;
                    self.last_watermark_text_commit = Some(now);
                }
                self.redo_stack.clear();
                return;
            }
            self.last_watermark_text_commit = Some(now);
        } else {
            self.last_watermark_text_commit = None;
        }
        if spotlight_opacity_change(&undo, &redo) {
            let can_coalesce = self
                .last_spotlight_commit
                .is_some_and(|at| now.duration_since(at) <= Duration::from_millis(220));
            if can_coalesce {
                if self
                    .undo_stack
                    .last()
                    .is_some_and(|last| spotlight_config(&last.undo) == spotlight_config(&redo))
                {
                    self.undo_stack.pop();
                    self.last_spotlight_commit = None;
                } else if let Some(last) = self.undo_stack.last_mut() {
                    last.label = label;
                    last.redo = redo;
                    last.redo_snapshot = redo_snapshot;
                    self.last_spotlight_commit = Some(now);
                }
                self.redo_stack.clear();
                return;
            }
            self.last_spotlight_commit = Some(now);
        } else {
            self.last_spotlight_commit = None;
        }
        if let Some(key) = filter_coalesce_key(&undo, &redo) {
            let can_coalesce = self
                .last_filter_commit
                .as_ref()
                .is_some_and(|(last_key, at)| {
                    *last_key == key && now.duration_since(*at) <= Duration::from_millis(220)
                });
            if can_coalesce {
                if self
                    .undo_stack
                    .last()
                    .is_some_and(|last| filter_update_data_equal(&last.undo, &redo))
                {
                    self.undo_stack.pop();
                    self.last_filter_commit = None;
                } else if let Some(last) = self.undo_stack.last_mut() {
                    last.label = label;
                    last.redo = redo;
                    last.redo_snapshot = redo_snapshot;
                    self.last_filter_commit = Some((key, now));
                }
                self.redo_stack.clear();
                return;
            }
            self.last_filter_commit = Some((key, now));
        } else {
            self.last_filter_commit = None;
        }
        if let Some(key) = serial_number_step_coalesce_key(&undo, &redo) {
            let last_key_matches = self
                .undo_stack
                .last()
                .and_then(|last| serial_number_step_coalesce_key(&last.undo, &last.redo))
                .is_some_and(|last_key| last_key == key);
            if last_key_matches {
                if self
                    .undo_stack
                    .last()
                    .is_some_and(|last| serial_number_update_data_equal(&last.undo, &redo))
                {
                    self.undo_stack.pop();
                } else if let Some(last) = self.undo_stack.last_mut() {
                    last.label = label;
                    last.redo = redo;
                    last.redo_snapshot = redo_snapshot;
                }
                self.redo_stack.clear();
                return;
            }
        }

        self.undo_stack.push(HistoryEntry {
            label,
            undo,
            redo,
            undo_snapshot,
            redo_snapshot,
        });
        self.redo_stack.clear();
    }

    pub fn can_undo(&self) -> bool {
        !self.undo_stack.is_empty()
    }

    pub fn can_redo(&self) -> bool {
        !self.redo_stack.is_empty()
    }

    pub fn undo(
        &mut self,
        model: &mut DocumentModel,
    ) -> Result<Option<HistoryApplyResult>, ErrorCode> {
        let Some(entry) = self.undo_stack.pop() else {
            return Ok(None);
        };
        self.last_watermark_text_commit = None;
        self.last_spotlight_commit = None;
        self.last_filter_commit = None;

        let apply_result = match model.apply_history_transaction(&entry.undo) {
            Ok(result) => result,
            Err(error) => {
                self.undo_stack.push(entry);
                return Err(error);
            }
        };

        let restore_selection = entry.label == "duplicate selection";
        let snapshot = entry.undo_snapshot.clone();
        self.redo_stack.push(entry);
        Ok(Some(HistoryApplyResult {
            restore_selection,
            apply_result,
            snapshot,
        }))
    }

    pub fn redo(
        &mut self,
        model: &mut DocumentModel,
    ) -> Result<Option<HistoryApplyResult>, ErrorCode> {
        let Some(entry) = self.redo_stack.pop() else {
            return Ok(None);
        };
        self.last_watermark_text_commit = None;
        self.last_spotlight_commit = None;
        self.last_filter_commit = None;

        let apply_result = match model.apply_history_transaction(&entry.redo) {
            Ok(result) => result,
            Err(error) => {
                self.redo_stack.push(entry);
                return Err(error);
            }
        };

        let restore_selection = entry.label == "duplicate selection";
        let snapshot = entry.redo_snapshot.clone();
        self.undo_stack.push(entry);
        Ok(Some(HistoryApplyResult {
            restore_selection,
            apply_result,
            snapshot,
        }))
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct FilterCoalesceKey {
    ids: Vec<ElementId>,
    property: u8,
}

fn filter_updates(transaction: &Transaction) -> Option<Vec<(ElementId, FilterData)>> {
    if transaction.is_empty() {
        return None;
    }
    transaction
        .operations()
        .iter()
        .map(|operation| match operation {
            Operation::UpdateElementData {
                id,
                data: ElementData::Filter(filter),
            } => Some((*id, *filter)),
            _ => None,
        })
        .collect()
}

fn filter_coalesce_key(undo: &Transaction, redo: &Transaction) -> Option<FilterCoalesceKey> {
    let updates_auto_fill = redo.operations().iter().any(|op| match op {
        Operation::UpdateElementData {
            data: ElementData::Filter(fill),
            ..
        } => fill.auto_region_id.is_some(),
        _ => false,
    });
    if updates_auto_fill {
        return None;
    }
    let mut old_updates = filter_updates(undo)?;
    let mut new_updates = filter_updates(redo)?;
    if old_updates.len() != new_updates.len() {
        return None;
    }
    old_updates.sort_by_key(|(id, _)| *id);
    new_updates.sort_by_key(|(id, _)| *id);
    let mut ids = Vec::with_capacity(old_updates.len());
    let mut common_property = None;
    for ((old_id, old), (new_id, new)) in old_updates.iter().zip(new_updates.iter()) {
        if old_id != new_id {
            return None;
        }
        let property = filter_single_changed_property(old, new)?;
        if common_property.is_some_and(|current| current != property) {
            return None;
        }
        common_property = Some(property);
        ids.push(*old_id);
    }
    Some(FilterCoalesceKey {
        ids,
        property: common_property?,
    })
}

fn filter_single_changed_property(old: &FilterData, new: &FilterData) -> Option<u8> {
    if old == new {
        return None;
    }
    let mut normalized = *old;
    normalized.filter_type = new.filter_type;
    if normalized == *new {
        return Some(0);
    }
    normalized = *old;
    normalized.strength = new.strength;
    if normalized == *new {
        return Some(1);
    }
    normalized = *old;
    normalized.opacity = new.opacity;
    (normalized == *new).then_some(2)
}

fn filter_update_data_equal(lhs: &Transaction, rhs: &Transaction) -> bool {
    let Some(mut lhs_updates) = filter_updates(lhs) else {
        return false;
    };
    let Some(mut rhs_updates) = filter_updates(rhs) else {
        return false;
    };
    lhs_updates.sort_by_key(|(id, _)| *id);
    rhs_updates.sort_by_key(|(id, _)| *id);
    lhs_updates == rhs_updates
}

fn watermark_config(transaction: &Transaction) -> Option<&WatermarkConfig> {
    match transaction.operations() {
        [Operation::UpdateWatermark { config }] => Some(config),
        _ => None,
    }
}

fn watermark_text_only_change(undo: &Transaction, redo: &Transaction) -> bool {
    let (Some(old), Some(new)) = (watermark_config(undo), watermark_config(redo)) else {
        return false;
    };
    old.text != new.text
        && old.color == new.color
        && old.template_value == new.template_value
        && old.template_application_time == new.template_application_time
        && old.font_size == new.font_size
        && old.font_family == new.font_family
        && old.angle == new.angle
        && old.gap == new.gap
        && old.opacity == new.opacity
}

fn spotlight_config(transaction: &Transaction) -> Option<&SpotlightConfig> {
    match transaction.operations() {
        [Operation::UpdateSpotlight { config }] => Some(config),
        _ => None,
    }
}

fn spotlight_opacity_change(undo: &Transaction, redo: &Transaction) -> bool {
    let Some(old) = spotlight_config(undo) else {
        return false;
    };
    let Some(new) = spotlight_config(redo) else {
        return false;
    };
    old.color == new.color && old.opacity != new.opacity
}

fn serial_number_step_coalesce_key(
    undo: &Transaction,
    redo: &Transaction,
) -> Option<Vec<ElementId>> {
    let mut old_updates = serial_number_updates(undo)?;
    let mut new_updates = serial_number_updates(redo)?;
    if old_updates.len() != new_updates.len() {
        return None;
    }

    old_updates.sort_by_key(|(id, _)| *id);
    new_updates.sort_by_key(|(id, _)| *id);
    let mut ids = Vec::with_capacity(old_updates.len());
    for ((old_id, old_serial), (new_id, new_serial)) in old_updates.iter().zip(new_updates.iter()) {
        if old_id != new_id || !serial_number_step_only_changed(old_serial, new_serial) {
            return None;
        }
        ids.push(*old_id);
    }
    Some(ids)
}

fn serial_number_updates(transaction: &Transaction) -> Option<Vec<(ElementId, SerialNumberData)>> {
    if transaction.is_empty() {
        return None;
    }

    transaction
        .operations()
        .iter()
        .map(|operation| match operation {
            Operation::UpdateElementData {
                id,
                data: ElementData::SerialNumber(serial),
            } => Some((*id, serial.clone())),
            _ => None,
        })
        .collect()
}

fn serial_number_update_data_equal(lhs: &Transaction, rhs: &Transaction) -> bool {
    let Some(mut lhs_updates) = serial_number_updates(lhs) else {
        return false;
    };
    let Some(mut rhs_updates) = serial_number_updates(rhs) else {
        return false;
    };
    lhs_updates.sort_by_key(|(id, _)| *id);
    rhs_updates.sort_by_key(|(id, _)| *id);
    lhs_updates == rhs_updates
}

fn serial_number_step_only_changed(old: &SerialNumberData, new: &SerialNumberData) -> bool {
    let mut normalized = old.clone();
    normalized.number = new.number;
    normalized.diameter = new.diameter;
    normalized == *new
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::{ColorRgba8, EngineConfig};
    use snow_draw_engine_editor::Editor;
    use snow_draw_engine_model::DocumentModel;

    fn id(index: u32) -> ElementId {
        ElementId {
            index,
            generation: 0,
        }
    }

    fn serial(number: i64) -> SerialNumberData {
        SerialNumberData {
            number,
            diameter: 21.0 + number as f64,
            ..SerialNumberData::default()
        }
    }

    fn serial_step_transaction(id: ElementId, number: i64) -> Transaction {
        let mut transaction = Transaction::new("serial step");
        transaction.update_serial_number(id, serial(number));
        transaction
    }

    fn serial_color_transaction(id: ElementId, number: i64, color: ColorRgba8) -> Transaction {
        let mut updated = serial(number);
        updated.color = color;
        let mut transaction = Transaction::new("serial color");
        transaction.update_serial_number(id, updated);
        transaction
    }

    fn snapshot() -> DocumentSyncSnapshot {
        let document = DocumentModel::new();
        Editor::new(EngineConfig::default())
            .unwrap()
            .capture_document_sync_snapshot(&document)
    }

    fn watermark_text_transaction(text: &str) -> Transaction {
        let mut transaction = Transaction::new("Update watermark");
        transaction.update_watermark(WatermarkConfig {
            text: text.to_owned(),
            ..WatermarkConfig::default()
        });
        transaction
    }

    fn filter_strength_transaction(filter_id: ElementId, strength: f64) -> Transaction {
        let mut transaction = Transaction::new("Update filter style");
        transaction.update_filter(
            filter_id,
            FilterData {
                strength,
                ..FilterData::default()
            },
        );
        transaction
    }

    fn spotlight_transaction(opacity: f64, color: ColorRgba8) -> Transaction {
        let mut transaction = Transaction::new("Update spotlight");
        transaction.update_spotlight(SpotlightConfig { color, opacity });
        transaction
    }

    fn watermark_font_size_transaction(font_size: f64) -> Transaction {
        let mut transaction = Transaction::new("Update watermark");
        transaction.update_watermark(WatermarkConfig {
            font_size,
            ..WatermarkConfig::default()
        });
        transaction
    }

    #[test]
    fn rapid_spotlight_opacity_commits_coalesce_but_color_changes_do_not() {
        let black = ColorRgba8 {
            r: 0,
            g: 0,
            b: 0,
            a: 255,
        };
        let red = ColorRgba8 {
            r: 255,
            g: 0,
            b: 0,
            a: 255,
        };
        let mut history = HistoryStore::default();
        history.push_committed(
            "Update spotlight",
            spotlight_transaction(0.7, black),
            spotlight_transaction(0.64, black),
            snapshot(),
            snapshot(),
        );
        history.push_committed(
            "Update spotlight",
            spotlight_transaction(0.8, black),
            spotlight_transaction(0.7, black),
            snapshot(),
            snapshot(),
        );
        assert_eq!(history.undo_stack.len(), 1);
        assert_eq!(
            history.undo_stack[0].undo,
            spotlight_transaction(0.64, black)
        );
        assert_eq!(
            history.undo_stack[0].redo,
            spotlight_transaction(0.8, black)
        );

        history.push_committed(
            "Update spotlight",
            spotlight_transaction(0.8, red),
            spotlight_transaction(0.8, black),
            snapshot(),
            snapshot(),
        );
        assert_eq!(history.undo_stack.len(), 2);
    }

    #[test]
    fn rapid_filter_strength_commits_coalesce_to_latest_redo() {
        let filter_id = id(8);
        let mut history = HistoryStore::default();
        history.push_committed(
            "Update filter style",
            filter_strength_transaction(filter_id, 0.6),
            filter_strength_transaction(filter_id, 0.5),
            snapshot(),
            snapshot(),
        );
        history.push_committed(
            "Update filter style",
            filter_strength_transaction(filter_id, 0.9),
            filter_strength_transaction(filter_id, 0.6),
            snapshot(),
            snapshot(),
        );

        assert_eq!(history.undo_stack.len(), 1);
        assert_eq!(
            history.undo_stack[0].undo,
            filter_strength_transaction(filter_id, 0.5)
        );
        assert_eq!(
            history.undo_stack[0].redo,
            filter_strength_transaction(filter_id, 0.9)
        );
    }

    #[test]
    fn rapid_watermark_text_commits_coalesce_to_latest_redo() {
        let mut history = HistoryStore::default();
        history.push_committed(
            "Update watermark",
            watermark_text_transaction("DR"),
            watermark_text_transaction(""),
            snapshot(),
            snapshot(),
        );
        history.push_committed(
            "Update watermark",
            watermark_text_transaction("DRAFT"),
            watermark_text_transaction("DR"),
            snapshot(),
            snapshot(),
        );
        assert_eq!(history.undo_stack.len(), 1);
        assert_eq!(history.undo_stack[0].undo, watermark_text_transaction(""));
        assert_eq!(
            history.undo_stack[0].redo,
            watermark_text_transaction("DRAFT")
        );
    }

    #[test]
    fn watermark_non_text_commits_remain_separate_history_entries() {
        let mut history = HistoryStore::default();
        history.push_committed(
            "Update watermark",
            watermark_font_size_transaction(18.0),
            watermark_font_size_transaction(16.0),
            snapshot(),
            snapshot(),
        );
        history.push_committed(
            "Update watermark",
            watermark_font_size_transaction(20.0),
            watermark_font_size_transaction(18.0),
            snapshot(),
            snapshot(),
        );

        assert_eq!(history.undo_stack.len(), 2);
        assert_eq!(
            history.undo_stack[0].redo,
            watermark_font_size_transaction(18.0)
        );
        assert_eq!(
            history.undo_stack[1].redo,
            watermark_font_size_transaction(20.0)
        );
    }

    #[test]
    fn serial_number_steps_for_same_ids_coalesce_to_latest_redo() {
        let serial_id = id(1);
        let mut history = HistoryStore::default();

        history.push_committed(
            "increase serial number",
            serial_step_transaction(serial_id, 2),
            serial_step_transaction(serial_id, 1),
            snapshot(),
            snapshot(),
        );
        history.push_committed(
            "increase serial number",
            serial_step_transaction(serial_id, 3),
            serial_step_transaction(serial_id, 2),
            snapshot(),
            snapshot(),
        );

        assert_eq!(history.undo_stack.len(), 1);
        assert_eq!(
            history.undo_stack[0].undo,
            serial_step_transaction(serial_id, 1)
        );
        assert_eq!(
            history.undo_stack[0].redo,
            serial_step_transaction(serial_id, 3)
        );
        assert!(history.redo_stack.is_empty());
    }

    #[test]
    fn serial_number_step_back_to_original_removes_coalesced_entry() {
        let serial_id = id(1);
        let mut history = HistoryStore::default();

        history.push_committed(
            "increase serial number",
            serial_step_transaction(serial_id, 2),
            serial_step_transaction(serial_id, 1),
            snapshot(),
            snapshot(),
        );
        history.push_committed(
            "decrease serial number",
            serial_step_transaction(serial_id, 1),
            serial_step_transaction(serial_id, 2),
            snapshot(),
            snapshot(),
        );

        assert!(history.undo_stack.is_empty());
        assert!(history.redo_stack.is_empty());
    }

    #[test]
    fn serial_number_steps_for_same_multi_selection_coalesce() {
        let first_id = id(1);
        let second_id = id(2);
        let mut first_redo = Transaction::new("serial step");
        first_redo.update_serial_number(first_id, serial(2));
        first_redo.update_serial_number(second_id, serial(6));
        let mut first_undo = Transaction::new("serial step");
        first_undo.update_serial_number(first_id, serial(1));
        first_undo.update_serial_number(second_id, serial(5));
        let mut second_redo = Transaction::new("serial step");
        second_redo.update_serial_number(second_id, serial(7));
        second_redo.update_serial_number(first_id, serial(3));
        let mut second_undo = Transaction::new("serial step");
        second_undo.update_serial_number(second_id, serial(6));
        second_undo.update_serial_number(first_id, serial(2));

        let mut history = HistoryStore::default();
        history.push_committed(
            "increase serial number",
            first_redo,
            first_undo.clone(),
            snapshot(),
            snapshot(),
        );
        history.push_committed(
            "increase serial number",
            second_redo.clone(),
            second_undo,
            snapshot(),
            snapshot(),
        );

        assert_eq!(history.undo_stack.len(), 1);
        assert_eq!(history.undo_stack[0].undo, first_undo);
        assert_eq!(history.undo_stack[0].redo, second_redo);
    }

    #[test]
    fn serial_number_non_step_style_update_does_not_coalesce() {
        let serial_id = id(1);
        let mut history = HistoryStore::default();

        history.push_committed(
            "increase serial number",
            serial_step_transaction(serial_id, 2),
            serial_step_transaction(serial_id, 1),
            snapshot(),
            snapshot(),
        );
        history.push_committed(
            "update serial number style",
            serial_color_transaction(
                serial_id,
                2,
                ColorRgba8 {
                    r: 255,
                    g: 0,
                    b: 0,
                    a: 255,
                },
            ),
            serial_step_transaction(serial_id, 2),
            snapshot(),
            snapshot(),
        );

        assert_eq!(history.undo_stack.len(), 2);
        assert!(history.redo_stack.is_empty());
    }
}
