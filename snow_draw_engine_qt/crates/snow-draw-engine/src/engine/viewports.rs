use std::sync::Arc;

use super::*;

impl Engine {
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
