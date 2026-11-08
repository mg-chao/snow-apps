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
            && let CanvasHit::SelectionHandle(target @ SelectionHitTarget::CornerRadius(_)) =
                self.resolve_canvas_hit(document, policy, canvas_point, true)
        {
            return PrimaryPointerIntent::BeginSelectionInteraction { target };
        }
        if modifiers.shift
            && policy.allow_shift_toggle
            && let CanvasHit::EligibleElement(id, _) =
                self.resolve_canvas_hit(document, policy, canvas_point, false)
        {
            return PrimaryPointerIntent::ToggleSelection { id };
        }

        let hit = self.resolve_canvas_hit(document, policy, canvas_point, true);
        if self.state.active_distance_value_owner.is_none()
            && !modifiers.shift
            && !modifiers.alt
            && !modifiers.ctrl
            && !modifiers.meta
        {
            let owner = match hit {
                CanvasHit::EligibleElement(id, ElementKind::Distance) => Some(id),
                CanvasHit::ArrowHandle(ArrowHitTarget::Move) => self.state.selection.primary,
                CanvasHit::SelectionHandle(SelectionHitTarget::Move) => {
                    match self.resolve_canvas_hit(document, policy, canvas_point, false) {
                        CanvasHit::EligibleElement(id, ElementKind::Distance) => Some(id),
                        _ => None,
                    }
                }
                _ => None,
            };
            if let Some(id) = owner
                && self.distance_value_hit(document, id, canvas_point)
            {
                return PrimaryPointerIntent::DistanceValueEdit { id };
            }
        }
        if matches!(
            self.state.active_tool,
            ActiveTool::Arrow
                | ActiveTool::Line
                | ActiveTool::Distance
                | ActiveTool::Angle
                | ActiveTool::Bracket
        ) {
            return match hit {
                CanvasHit::SelectionHandle(target) => {
                    PrimaryPointerIntent::BeginSelectionInteraction { target }
                }
                CanvasHit::ArrowHandle(target) => {
                    PrimaryPointerIntent::BeginSelectedArrowInteraction { target }
                }
                CanvasHit::EligibleElement(id, ElementKind::Arrow) => {
                    PrimaryPointerIntent::BeginArrowElementInteraction { id }
                }
                CanvasHit::EligibleElement(
                    id,
                    ElementKind::Distance | ElementKind::Angle | ElementKind::Bracket,
                ) => PrimaryPointerIntent::BeginArrowElementInteraction { id },
                CanvasHit::EligibleElement(id, ElementKind::Line) => {
                    PrimaryPointerIntent::BeginArrowElementInteraction { id }
                }
                _ => PrimaryPointerIntent::EmptyCanvas,
            };
        }

        match hit {
            CanvasHit::MagnifierLens(id) => PrimaryPointerIntent::BeginMagnifierLens { id },
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
                        | ElementKind::Bracket
                ) {
                    return PrimaryPointerIntent::BeginArrowElementInteraction { id };
                }

                PrimaryPointerIntent::BeginElementSelectionMove { id }
            }
            CanvasHit::Empty => PrimaryPointerIntent::EmptyCanvas,
        }
    }

    pub(crate) fn update_hover_feedback_for_primary_pointer_intent(
        &mut self,
        document: &DocumentModel,
        policy: ToolPolicy,
        intent: PrimaryPointerIntent,
        canvas_point: Point<f64>,
    ) -> CursorStyle {
        let (cursor, hovered_element) =
            self.hover_feedback_for_primary_pointer_intent(document, policy, intent);
        self.set_hovered_element(hovered_element);
        let hovered_handle = match intent {
            PrimaryPointerIntent::BeginSelectedArrowInteraction {
                target: ArrowHitTarget::AngleArc,
            } => self
                .selected_single_arrow_snapshot(document)
                .and_then(|(_, arrow)| snow_draw_engine_document::angle_arc_control_point(&arrow))
                .filter(|center| {
                    point_distance(*center, canvas_point)
                        <= selection_handle_hit_size(self.camera().zoom) / 2.0 * 1.11
                })
                .map(|_| ArrowHitTarget::AngleArc),
            PrimaryPointerIntent::BeginSelectedArrowInteraction { target } => Some(target),
            _ => None,
        };
        self.set_hovered_arrow_handle(hovered_handle);
        cursor
    }

    pub(crate) fn hover_feedback_for_primary_pointer_intent(
        &self,
        document: &DocumentModel,
        policy: ToolPolicy,
        intent: PrimaryPointerIntent,
    ) -> (CursorStyle, Option<ElementId>) {
        match intent {
            PrimaryPointerIntent::BeginMagnifierLens { id } => (
                CursorStyle::Move,
                (!self.state.selection.contains(id)).then_some(id),
            ),
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
            PrimaryPointerIntent::TextEditCandidate { id }
            | PrimaryPointerIntent::DistanceValueEdit { id } => (
                CursorStyle::Text,
                (!self.state.selection.contains(id)).then_some(id),
            ),
            PrimaryPointerIntent::EmptyCanvas => (policy.default_cursor, None),
        }
    }
}
