use super::*;

impl Editor {
    pub(crate) fn resolve_primary_pointer_intent(
        &self,
        document: &DocumentModel,
        policy: ToolPolicy,
        canvas_point: Point<f64>,
        modifiers: Modifiers,
    ) -> PrimaryPointerIntent {
        if modifiers.shift
            && policy.allow_shift_toggle
            && let CanvasHit::EligibleElement(id, _) =
                self.resolve_canvas_hit(document, policy, canvas_point, false)
        {
            return PrimaryPointerIntent::ToggleSelection { id };
        }

        if matches!(
            self.state.active_tool,
            ActiveTool::Arrow | ActiveTool::Line | ActiveTool::Distance | ActiveTool::Angle
        ) {
            return match self.resolve_canvas_hit(document, policy, canvas_point, true) {
                CanvasHit::SelectionHandle(target) => {
                    PrimaryPointerIntent::BeginSelectionInteraction { target }
                }
                CanvasHit::ArrowHandle(target) => {
                    PrimaryPointerIntent::BeginSelectedArrowInteraction { target }
                }
                CanvasHit::EligibleElement(id, ElementKind::Arrow) => {
                    PrimaryPointerIntent::BeginArrowElementInteraction { id }
                }
                CanvasHit::EligibleElement(id, ElementKind::Distance | ElementKind::Angle) => {
                    PrimaryPointerIntent::BeginArrowElementInteraction { id }
                }
                CanvasHit::EligibleElement(id, ElementKind::Line) => {
                    PrimaryPointerIntent::BeginArrowElementInteraction { id }
                }
                _ => PrimaryPointerIntent::EmptyCanvas,
            };
        }

        match self.resolve_canvas_hit(document, policy, canvas_point, true) {
            CanvasHit::SelectionHandle(target) => {
                if target == SelectionHitTarget::Move
                    && let Some(id) = self.hit_text_at(document, canvas_point)
                    && self.can_begin_selected_text_edit_at(
                        document,
                        id,
                        modifiers.shift,
                        canvas_point,
                    )
                {
                    return PrimaryPointerIntent::TextEditCandidate { id };
                }

                PrimaryPointerIntent::BeginSelectionInteraction { target }
            }
            CanvasHit::ArrowHandle(target) => {
                PrimaryPointerIntent::BeginSelectedArrowInteraction { target }
            }
            CanvasHit::EligibleElement(id, kind) => {
                if self.state.active_tool == ActiveTool::Text && kind == ElementKind::Text {
                    return PrimaryPointerIntent::TextEditCandidate { id };
                }

                if kind == ElementKind::Text
                    && self.can_begin_selected_text_edit(document, id, modifiers.shift)
                {
                    return PrimaryPointerIntent::TextEditCandidate { id };
                }

                if matches!(
                    kind,
                    ElementKind::Arrow
                        | ElementKind::Line
                        | ElementKind::Distance
                        | ElementKind::Angle
                ) {
                    return PrimaryPointerIntent::BeginArrowElementInteraction { id };
                }

                PrimaryPointerIntent::BeginElementSelectionMove { id }
            }
            CanvasHit::Empty => PrimaryPointerIntent::EmptyCanvas,
        }
    }

    pub(crate) fn hover_feedback_for_primary_pointer_intent(
        &self,
        document: &DocumentModel,
        policy: ToolPolicy,
        intent: PrimaryPointerIntent,
    ) -> (CursorStyle, Option<ElementId>) {
        match intent {
            PrimaryPointerIntent::ToggleSelection { id }
            | PrimaryPointerIntent::BeginElementSelectionMove { id }
            | PrimaryPointerIntent::BeginArrowElementInteraction { id } => (
                CursorStyle::Move,
                (!self.state.selection.contains(id)).then_some(id),
            ),
            PrimaryPointerIntent::BeginSelectionInteraction { target } => {
                let rotation = self
                    .selection_bounds_snapshot(document)
                    .map(|bounds| bounds.rotation)
                    .unwrap_or_default();
                (hover_cursor_for_selection_target(target, rotation), None)
            }
            PrimaryPointerIntent::BeginSelectedArrowInteraction { target } => {
                (hover_cursor_for_arrow_target(target), None)
            }
            PrimaryPointerIntent::TextEditCandidate { id } => (
                CursorStyle::Text,
                (!self.state.selection.contains(id)).then_some(id),
            ),
            PrimaryPointerIntent::EmptyCanvas => (policy.default_cursor, None),
        }
    }
}
