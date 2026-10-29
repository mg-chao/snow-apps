use crate::{
    ApplyTransactionCommand, Editor, EditorCommand, ElementCreationPreview, SelectionArrowState,
};
use serde::{Deserialize, Serialize};
use snow_draw_engine_core::{
    ColorRgba8, ErrorCode, Point,
    arrow::{ArrowType, StrokeStyle},
};
use snow_draw_engine_document::{ArrowData, LinearElementKind, Transaction};
use snow_draw_engine_model::DocumentModel;

pub const BRACKET_STYLE_PROPERTY_STROKE: u32 = 1;
pub const BRACKET_STYLE_PROPERTY_STROKE_WIDTH: u32 = 2;
pub const BRACKET_STYLE_PROPERTY_ALL: u32 = 3;
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
pub struct BracketStyle {
    pub stroke: ColorRgba8,
    pub stroke_width: f64,
}
impl Default for BracketStyle {
    fn default() -> Self {
        Self {
            stroke: ColorRgba8 {
                r: 245,
                g: 34,
                b: 45,
                a: 255,
            },
            stroke_width: 2.0,
        }
    }
}
pub fn validate_bracket_style(style: BracketStyle) -> Result<(), ErrorCode> {
    if !style.stroke_width.is_finite() || !(1.0..=72.0).contains(&style.stroke_width) {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok(())
}
impl BracketStyle {
    pub fn from_arrow(arrow: &ArrowData) -> Option<Self> {
        arrow.is_bracket().then_some(Self {
            stroke: arrow.stroke,
            stroke_width: arrow.stroke_width,
        })
    }
    pub fn apply_to_arrow(self, arrow: &mut ArrowData) {
        arrow.linear_kind = LinearElementKind::Bracket;
        arrow.stroke = self.stroke;
        arrow.stroke_width = self.stroke_width;
        arrow.angle = None;
        arrow.distance = None;
        arrow.text_element_id = None;
        arrow.text_path_fraction = None;
        arrow.arrow_type = ArrowType::Straight;
        arrow.arrow_shaft_type = Default::default();
        arrow.stroke_style = StrokeStyle::Solid;
        arrow.fill = Default::default();
        arrow.fill_style = Default::default();
        arrow.start_arrowhead = None;
        arrow.end_arrowhead = None;
        arrow.start_binding = None;
        arrow.end_binding = None;
        arrow.fixed_segments = None;
        arrow.start_is_special = None;
        arrow.end_is_special = None;
    }
    pub fn merged(self, value: Self, properties: u32) -> Self {
        Self {
            stroke: if properties & 1 != 0 {
                value.stroke
            } else {
                self.stroke
            },
            stroke_width: if properties & 2 != 0 {
                value.stroke_width
            } else {
                self.stroke_width
            },
        }
    }
    fn differences(self, other: Self) -> u32 {
        u32::from(self.stroke != other.stroke)
            | (u32::from(self.stroke_width != other.stroke_width) << 1)
    }
}
impl Editor {
    pub(crate) fn has_homogeneous_bracket_selection(&self, document: &DocumentModel) -> bool {
        !self.state.selection.ids.is_empty()
            && self
                .state
                .selection
                .ids
                .iter()
                .all(|id| document.arrow(*id).is_ok_and(ArrowData::is_bracket))
    }
    pub fn bracket_style(&self, document: &DocumentModel) -> BracketStyle {
        if !self.has_homogeneous_bracket_selection(document) {
            return self.state.default_bracket_style;
        }
        self.state
            .selection
            .primary
            .and_then(|id| document.arrow(id).ok())
            .and_then(BracketStyle::from_arrow)
            .or_else(|| {
                self.state
                    .selection
                    .ids
                    .iter()
                    .find_map(|id| document.arrow(*id).ok().and_then(BracketStyle::from_arrow))
            })
            .unwrap_or(self.state.default_bracket_style)
    }
    pub fn bracket_style_mixed(&self, document: &DocumentModel) -> u32 {
        if !self.has_homogeneous_bracket_selection(document) {
            return 0;
        }
        let mut styles = self
            .state
            .selection
            .ids
            .iter()
            .filter_map(|id| document.arrow(*id).ok().and_then(BracketStyle::from_arrow));
        let Some(first) = styles.next() else {
            return 0;
        };
        styles.fold(0, |mixed, style| mixed | first.differences(style))
    }
    pub fn set_bracket_style_patch(
        &mut self,
        document: &DocumentModel,
        style: BracketStyle,
        properties: u32,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        if properties & !BRACKET_STYLE_PROPERTY_ALL != 0 {
            return Err(ErrorCode::InvalidArgument);
        }
        let next_default = self.state.default_bracket_style.merged(style, properties);
        validate_bracket_style(next_default)?;
        let mut transaction = Transaction::new("update bracket style properties");
        let mut updates = Vec::new();
        let selected_ids = if self.has_homogeneous_bracket_selection(document) {
            self.state.selection.ids.as_slice()
        } else {
            &[]
        };
        for id in selected_ids.iter().copied() {
            let Ok(arrow) = document.arrow(id) else {
                continue;
            };
            let Some(current) = BracketStyle::from_arrow(arrow) else {
                continue;
            };
            let updated_style = current.merged(style, properties);
            validate_bracket_style(updated_style)?;
            let mut next = arrow.clone();
            updated_style.apply_to_arrow(&mut next);
            if &next != arrow {
                transaction.update_arrow(id, next.clone());
            }
            updates.push(SelectionArrowState { id, arrow: next });
        }
        self.state.default_bracket_style = next_default;
        if let Some(ElementCreationPreview::Arrow(arrow)) = &mut self.state.creation_preview
            && self.state.active_tool == crate::ActiveTool::Bracket
        {
            let previous = arrow.clone();
            if arrow.points.len() == 3 {
                next_default.apply_to_arrow(arrow);
            } else {
                arrow.stroke = next_default.stroke;
                arrow.stroke_width = next_default.stroke_width;
            }
            if *arrow != previous {
                self.bump_scene_state_revision();
            }
        }
        if transaction.is_empty() {
            return Ok(None);
        }
        let snapshot = self.capture_document_sync_snapshot(document);
        for next in updates {
            if let Some(current) = self
                .state
                .selection
                .arrows
                .iter_mut()
                .find(|a| a.id == next.id)
            {
                *current = next;
            }
        }
        Ok(Some(EditorCommand::ApplyTransaction(
            ApplyTransactionCommand::with_history_undo_snapshot(transaction, snapshot),
        )))
    }

    pub(crate) fn process_bracket_creation_pointer_event(
        &mut self,
        document: &DocumentModel,
        event: crate::PointerEvent,
    ) -> Result<crate::InteractionOutput, ErrorCode> {
        use crate::{
            CursorCommand, CursorStyle, InteractionState, PointerButton, PointerCaptureCommand,
            PointerEventType,
        };
        let InteractionState::CreatingArrow(state) = &self.state.interaction else {
            return Ok(Default::default());
        };
        if state.pointer_id != event.pointer_id {
            return Ok(Default::default());
        }
        if event.event_type == PointerEventType::Cancel
            || event.button == Some(PointerButton::Secondary)
        {
            self.cancel_interaction();
            return Ok(crate::InteractionOutput {
                consumed: true,
                capture: self.release_capture_command(),
                cursor: CursorCommand::Set(CursorStyle::Crosshair),
            });
        }
        if !matches!(
            event.event_type,
            PointerEventType::Move | PointerEventType::Enter | PointerEventType::Up
        ) {
            return Ok(Default::default());
        }
        let start = state.committed_points[0];
        let moved = crate::pointer_drag_distance(state.press_view_position, event.position) >= 3.0;
        let end = crate::view_to_canvas(event.position, &self.camera(), self.surface_size());
        let (end, guides) = self.snap_arrow_creation_point(document, end, event.modifiers);
        let style = self.state.default_bracket_style;
        let preview = if (end.y - start.y).abs() > 1e-6 && (end.x - start.x).abs() > 1e-6 {
            ArrowData::from_global_points(
                &[
                    Point::new(start.x, start.y.min(end.y)),
                    Point::new(end.x, f64::midpoint(start.y, end.y)),
                    Point::new(start.x, start.y.max(end.y)),
                ],
                style.stroke,
                style.stroke_width,
                StrokeStyle::Solid,
                ArrowType::Straight,
                None,
                None,
            )
            .and_then(|mut arrow| {
                style.apply_to_arrow(&mut arrow);
                snow_draw_engine_document::validate_arrow(&arrow)
                    .ok()
                    .map(|()| arrow)
            })
        } else {
            None
        };
        let capture = if event.event_type == PointerEventType::Up {
            self.cancel_interaction();
            if moved && let Some(arrow) = preview {
                self.queue_arrow_creation(document, arrow)?;
            }
            self.release_capture_command()
        } else {
            self.set_creation_preview(preview.map(ElementCreationPreview::Arrow), guides);
            PointerCaptureCommand::NoChange
        };
        Ok(crate::InteractionOutput {
            consumed: true,
            capture,
            cursor: CursorCommand::Set(CursorStyle::Crosshair),
        })
    }
}
