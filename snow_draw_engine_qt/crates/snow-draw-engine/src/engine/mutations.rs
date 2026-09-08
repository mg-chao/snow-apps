use super::*;

impl Engine {
    pub fn history_state(&self) -> HistoryState {
        HistoryState {
            can_undo: self.history.can_undo(),
            can_redo: self.history.can_redo(),
        }
    }

    pub fn undo(&mut self) -> Result<bool, ErrorCode> {
        self.undo_with_viewport_changes()
            .map(|result| !result.changed_viewports.is_empty())
    }

    pub fn undo_with_viewport_changes(&mut self) -> Result<MutationResult, ErrorCode> {
        let follows_serial_number = self.editor.serial_number_follows_document(&self.model);
        let Some(history_result) = self.history.undo(&mut self.model)? else {
            return Ok(MutationResult::default());
        };
        if follows_serial_number {
            self.editor
                .sync_serial_number_after_history_change(&self.model);
        }
        Ok(self.finish_document_change(
            &history_result.snapshot,
            &history_result.apply_result.changes,
        ))
    }

    pub fn redo(&mut self) -> Result<bool, ErrorCode> {
        self.redo_with_viewport_changes()
            .map(|result| !result.changed_viewports.is_empty())
    }

    pub fn redo_with_viewport_changes(&mut self) -> Result<MutationResult, ErrorCode> {
        let follows_serial_number = self.editor.serial_number_follows_document(&self.model);
        let Some(history_result) = self.history.redo(&mut self.model)? else {
            return Ok(MutationResult::default());
        };
        if follows_serial_number {
            self.editor
                .sync_serial_number_after_history_change(&self.model);
        }
        Ok(self.finish_document_change(
            &history_result.snapshot,
            &history_result.apply_result.changes,
        ))
    }

    pub(super) fn apply_editor_command(
        &mut self,
        source_viewport_id: ViewportId,
        command: EditorCommand,
    ) -> Result<MutationResult, ErrorCode> {
        match command {
            EditorCommand::ApplyTransaction(command) => {
                self.commit_transaction(source_viewport_id, command)
            }
            EditorCommand::Undo => self.undo_with_viewport_changes(),
            EditorCommand::Redo => self.redo_with_viewport_changes(),
        }
    }

    pub(super) fn commit_transaction(
        &mut self,
        _source_viewport_id: ViewportId,
        command: ApplyTransactionCommand,
    ) -> Result<MutationResult, ErrorCode> {
        let ApplyTransactionCommand {
            transaction,
            history_undo_snapshot,
        } = command;
        if transaction.is_empty() {
            return Ok(MutationResult::default());
        }
        let redo_snapshot = self.capture_session_snapshot();
        let undo_snapshot = history_undo_snapshot.unwrap_or_else(|| redo_snapshot.clone());
        let label = transaction.label().to_owned();
        let redo = transaction.clone();
        let apply_result = self.model.apply_transaction(transaction)?;
        let mutation_result = self.finish_document_change(&redo_snapshot, &apply_result.changes);
        self.history.push_committed(
            label,
            redo,
            apply_result.inverse.clone(),
            undo_snapshot,
            redo_snapshot,
        );
        Ok(mutation_result)
    }

    pub(super) fn capture_session_snapshot(&self) -> DocumentSyncSnapshot {
        self.editor.capture_document_sync_snapshot(&self.model)
    }

    pub(super) fn finish_document_change(
        &mut self,
        snapshot: &DocumentSyncSnapshot,
        changes: &snow_draw_engine_document::DocumentDelta,
    ) -> MutationResult {
        self.scene_cache.sync(&self.model, Some(changes));
        self.editor
            .sync_after_document_change(&self.model, snapshot);
        self.refresh_all_viewports().unwrap_or_default()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_document::{ElementMeta, SerialNumberData, Transaction};

    fn engine_with_serial_numbers(numbers: &[i64], next: i64) -> Engine {
        let mut config = EngineConfig::default();
        config.style_defaults.editor.serial_number.number = next;
        let mut engine = Engine::new(config);
        for (index, number) in numbers.iter().enumerate() {
            let mut transaction = Transaction::new("Create serial number");
            transaction.insert_serial_number(
                ElementId {
                    index: index as u32,
                    generation: 1,
                },
                ElementMeta::default(),
                SerialNumberData {
                    number: *number,
                    ..Default::default()
                },
            );
            engine
                .commit_transaction(
                    ViewportId(0),
                    ApplyTransactionCommand {
                        transaction,
                        history_undo_snapshot: None,
                    },
                )
                .unwrap();
        }
        engine
    }

    #[test]
    fn serial_number_history_keeps_following_document_maximum() {
        for numbers in [vec![1, 2, 3], vec![1, 5], vec![1], vec![1, i64::MAX]] {
            let next = numbers.last().unwrap().saturating_add(1);
            let mut engine = engine_with_serial_numbers(&numbers, next);
            engine.undo().unwrap();
            let expected = numbers.iter().rev().nth(1).copied().unwrap_or(0) + 1;
            assert_eq!(
                engine.editor.serial_number_style(&engine.model).number,
                expected
            );
            engine.redo().unwrap();
            assert_eq!(
                engine.editor.serial_number_style(&engine.model).number,
                next
            );
        }
    }

    #[test]
    fn serial_number_history_preserves_default_changed_before_redo() {
        let mut engine = engine_with_serial_numbers(&[1, 2, 3], 4);
        engine.undo().unwrap();
        let mut style = engine.editor.serial_number_style(&engine.model);
        style.number = 10;
        engine
            .editor
            .set_serial_number_style(&engine.model, style)
            .unwrap();
        engine.redo().unwrap();
        assert_eq!(engine.editor.serial_number_style(&engine.model).number, 10);
    }

    #[test]
    fn serial_number_history_repeated_undo_redo_and_empty_history() {
        let mut engine = engine_with_serial_numbers(&[1, 2, 3], 4);
        for expected in [3, 2, 1, 1] {
            engine.undo().unwrap();
            assert_eq!(
                engine.editor.serial_number_style(&engine.model).number,
                expected
            );
        }
        for expected in [2, 3, 4, 4] {
            engine.redo().unwrap();
            assert_eq!(
                engine.editor.serial_number_style(&engine.model).number,
                expected
            );
        }
    }

    #[test]
    fn serial_number_history_preserves_custom_defaults_and_unchanged_maximum() {
        for (numbers, next) in [
            (vec![1, 2, 3], 10),
            (vec![1, 2, 3], 2),
            (vec![3, 1], 4),
            (vec![3, 3], 4),
        ] {
            let mut engine = engine_with_serial_numbers(&numbers, next);
            engine.undo().unwrap();
            assert_eq!(
                engine.editor.serial_number_style(&engine.model).number,
                next
            );
            engine.redo().unwrap();
            assert_eq!(
                engine.editor.serial_number_style(&engine.model).number,
                next
            );
        }
    }
}
