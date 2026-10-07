use std::sync::Arc;

use super::*;

impl Engine {
    /// Defer presentation composition until the host has supplied text metrics.
    /// Document/editor queries remain current; patches remain at the previous
    /// published cursor. Scopes nest and must be balanced, including on errors.
    pub fn begin_presentation_update(&mut self) -> Result<(), ErrorCode> {
        self.presentation_update_depth = self
            .presentation_update_depth
            .checked_add(1)
            .ok_or(ErrorCode::InvalidState)?;
        Ok(())
    }

    pub fn end_presentation_update(&mut self) -> Result<MutationResult, ErrorCode> {
        self.presentation_update_depth = self
            .presentation_update_depth
            .checked_sub(1)
            .ok_or(ErrorCode::InvalidState)?;
        if self.presentation_update_depth != 0 {
            return Ok(MutationResult::default());
        }
        let pending = std::mem::take(&mut self.pending_viewports);
        let mut changed_viewports = Vec::with_capacity(pending.len());
        for id in pending {
            changed_viewports.extend(self.refresh_single_viewport(id)?.changed_viewports);
        }
        Ok(MutationResult { changed_viewports })
    }

    pub fn set_viewport_snap_guide_targets(
        &mut self,
        id: ViewportId,
        targets: snow_draw_engine_editor::SnapGuideTargets,
    ) -> Result<MutationResult, ErrorCode> {
        if targets
            .vertical_xs
            .iter()
            .chain(targets.horizontal_ys.iter())
            .flatten()
            .any(|value| !value.is_finite())
        {
            return Err(ErrorCode::InvalidArgument);
        }
        let slot = self.viewport_slot_mut(id)?;
        if slot.view.snap_guide_targets == targets {
            return Ok(MutationResult::default());
        }
        slot.view.snap_guide_targets = targets;
        self.refresh_single_viewport(id)
    }

    pub fn create_viewport(&mut self, config: ViewportConfig) -> Result<ViewportId, ErrorCode> {
        if !self.session_config_seeded {
            self.editor.set_config(config.engine)?;
            self.session_config_seeded = true;
        }

        let id = ViewportId(self.next_viewport_id);
        self.next_viewport_id = self.next_viewport_id.wrapping_add(1);
        self.viewports.insert(
            id,
            ViewportSlot {
                view: EditorViewportState::default(),
                composer: ViewportComposer::new(),
            },
        );
        self.refresh_viewport(id)?;
        Ok(id)
    }

    pub fn destroy_viewport(&mut self, id: ViewportId) -> Result<(), ErrorCode> {
        self.pending_viewports.remove(&id);
        self.viewports
            .remove(&id)
            .map(|_| ())
            .ok_or(ErrorCode::NotFound)
    }

    pub fn set_viewport_surface_size(
        &mut self,
        id: ViewportId,
        width: u32,
        height: u32,
    ) -> Result<MutationResult, ErrorCode> {
        let before = self.viewport_slot(id)?.view;
        self.viewport_slot_mut(id)?
            .view
            .set_surface_size(width, height);
        if self.viewport_slot(id)?.view == before {
            return Ok(MutationResult::default());
        }
        self.refresh_single_viewport(id)
    }

    pub fn set_viewport_camera(
        &mut self,
        id: ViewportId,
        camera: Camera,
    ) -> Result<MutationResult, ErrorCode> {
        let before = self.viewport_slot(id)?.view;
        self.viewport_slot_mut(id)?.view.set_camera(camera)?;
        if self.viewport_slot(id)?.view == before {
            return Ok(MutationResult::default());
        }
        self.refresh_single_viewport(id)
    }

    pub fn acquire_patch(
        &self,
        id: ViewportId,
        cursor: Option<PatchCursor>,
    ) -> Result<Arc<ViewportPatch>, ErrorCode> {
        Ok(self.viewport_slot(id)?.composer.acquire_patch(cursor))
    }

    pub(super) fn refresh_single_viewport(
        &mut self,
        id: ViewportId,
    ) -> Result<MutationResult, ErrorCode> {
        let before = self.viewport_slot(id)?.composer.current_cursor();
        if self.presentation_update_depth != 0 {
            self.pending_viewports.insert(id);
            return Ok(MutationResult::default());
        }
        self.refresh_viewport(id)?;
        let after = self.viewport_slot(id)?.composer.current_cursor();
        Ok(MutationResult {
            changed_viewports: (before != after).then_some(id).into_iter().collect(),
        })
    }

    pub(super) fn refresh_viewport(&mut self, id: ViewportId) -> Result<(), ErrorCode> {
        let slot = self.viewports.get_mut(&id).ok_or(ErrorCode::NotFound)?;
        slot.composer
            .refresh(&self.scene_cache, &self.model, &mut self.editor, &slot.view);
        Ok(())
    }

    pub(crate) fn refresh_all_viewports(&mut self) -> Result<MutationResult, ErrorCode> {
        let mut changed_viewports = Vec::new();
        for id in self.all_viewport_ids() {
            let result = self.refresh_single_viewport(id)?;
            changed_viewports.extend(result.changed_viewports);
        }
        Ok(MutationResult { changed_viewports })
    }

    pub(super) fn refresh_after_session_mutation(
        &mut self,
        before: EditorSessionSnapshot,
    ) -> Result<MutationResult, ErrorCode> {
        if self.editor.snapshot() == before {
            return Ok(MutationResult::default());
        }
        self.refresh_all_viewports()
    }

    pub(super) fn all_viewport_ids(&self) -> Vec<ViewportId> {
        let mut ids = self.viewports.keys().copied().collect::<Vec<_>>();
        ids.sort_by_key(|id| id.0);
        ids
    }

    pub(super) fn ensure_viewport(&self, id: ViewportId) -> Result<(), ErrorCode> {
        self.viewport_slot(id).map(|_| ())
    }

    pub(super) fn viewport_slot(&self, id: ViewportId) -> Result<&ViewportSlot, ErrorCode> {
        self.viewports.get(&id).ok_or(ErrorCode::NotFound)
    }

    pub(super) fn viewport_slot_mut(
        &mut self,
        id: ViewportId,
    ) -> Result<&mut ViewportSlot, ErrorCode> {
        self.viewports.get_mut(&id).ok_or(ErrorCode::NotFound)
    }
}

#[cfg(test)]
mod external_guide_tests {
    use super::*;

    #[test]
    fn guide_targets_are_transient_and_per_viewport() {
        let mut engine = Engine::default();
        let first = engine.create_viewport(ViewportConfig::default()).unwrap();
        let second = engine.create_viewport(ViewportConfig::default()).unwrap();
        let mut targets = snow_draw_engine_editor::SnapGuideTargets::default();
        targets.vertical_xs[0] = Some(120.0);
        engine
            .set_viewport_snap_guide_targets(first, targets)
            .unwrap();
        assert_eq!(
            engine.viewport_slot(first).unwrap().view.snap_guide_targets,
            targets
        );
        assert_eq!(
            engine
                .viewport_slot(second)
                .unwrap()
                .view
                .snap_guide_targets,
            snow_draw_engine_editor::SnapGuideTargets::default()
        );
        engine
            .set_viewport_snap_guide_targets(
                first,
                snow_draw_engine_editor::SnapGuideTargets::default(),
            )
            .unwrap();
        assert_eq!(
            engine.viewport_slot(first).unwrap().view.snap_guide_targets,
            snow_draw_engine_editor::SnapGuideTargets::default()
        );
    }
}

#[cfg(test)]
mod presentation_update_tests {
    use super::*;

    #[test]
    fn nested_scopes_flush_once_and_empty_scopes_do_not_publish() {
        let mut engine = Engine::default();
        let id = engine.create_viewport(ViewportConfig::default()).unwrap();
        let before = engine.viewport_slot(id).unwrap().composer.current_cursor();
        assert_eq!(
            engine.end_presentation_update(),
            Err(ErrorCode::InvalidState)
        );
        engine.begin_presentation_update().unwrap();
        engine.begin_presentation_update().unwrap();
        assert!(
            engine
                .set_viewport_surface_size(id, 800, 600)
                .unwrap()
                .changed_viewports
                .is_empty()
        );
        assert!(
            engine
                .end_presentation_update()
                .unwrap()
                .changed_viewports
                .is_empty()
        );
        assert_eq!(
            engine.viewport_slot(id).unwrap().composer.current_cursor(),
            before
        );
        assert_eq!(
            engine.end_presentation_update().unwrap().changed_viewports,
            vec![id]
        );
        engine.begin_presentation_update().unwrap();
        assert!(
            engine
                .end_presentation_update()
                .unwrap()
                .changed_viewports
                .is_empty()
        );
    }

    #[test]
    fn destroyed_viewports_and_cloned_sessions_do_not_inherit_pending_scopes() {
        let mut engine = Engine::default();
        let id = engine.create_viewport(ViewportConfig::default()).unwrap();
        engine.begin_presentation_update().unwrap();
        engine.set_viewport_surface_size(id, 800, 600).unwrap();
        let mut clone = engine.clone_document_session();
        assert_eq!(
            clone.end_presentation_update(),
            Err(ErrorCode::InvalidState)
        );
        assert_eq!(
            clone
                .set_viewport_surface_size(id, 900, 600)
                .unwrap()
                .changed_viewports,
            vec![id]
        );
        engine.destroy_viewport(id).unwrap();
        assert!(
            engine
                .end_presentation_update()
                .unwrap()
                .changed_viewports
                .is_empty()
        );
    }
}
