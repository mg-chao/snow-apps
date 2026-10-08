use crate::{
    ApplyTransactionCommand, Editor, EditorCommand, ElementCreationPreview, SelectionArrowState,
};
use serde::{Deserialize, Serialize};
use snow_draw_engine_core::{
    ColorRgba8, ErrorCode, Point,
    arrow::{ArrowType, StrokeStyle},
};
use snow_draw_engine_document::{
    AngleAnnotation, AngleUnit, ArrowData, LinearElementKind, Transaction,
};
use snow_draw_engine_model::DocumentModel;

pub const ANGLE_STYLE_PROPERTY_STROKE: u32 = 1;
pub const ANGLE_STYLE_PROPERTY_STROKE_WIDTH: u32 = 2;
pub const ANGLE_STYLE_PROPERTY_UNIT: u32 = 4;
pub const ANGLE_STYLE_PROPERTY_DECIMAL_PLACES: u32 = 8;
pub const ANGLE_STYLE_PROPERTY_ALL: u32 = 15;
pub const ANGLE_STYLE_MIXED_STROKE: u32 = ANGLE_STYLE_PROPERTY_STROKE;
pub const ANGLE_STYLE_MIXED_STROKE_WIDTH: u32 = ANGLE_STYLE_PROPERTY_STROKE_WIDTH;
pub const ANGLE_STYLE_MIXED_UNIT: u32 = ANGLE_STYLE_PROPERTY_UNIT;
pub const ANGLE_STYLE_MIXED_DECIMAL_PLACES: u32 = ANGLE_STYLE_PROPERTY_DECIMAL_PLACES;

#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
pub struct AngleStyle {
    pub stroke: ColorRgba8,
    pub stroke_width: f64,
    pub unit: AngleUnit,
    pub decimal_places: u8,
}
impl Default for AngleStyle {
    fn default() -> Self {
        Self {
            stroke: ColorRgba8 {
                r: 0xf5,
                g: 0x22,
                b: 0x2d,
                a: 255,
            },
            stroke_width: 2.0,
            unit: AngleUnit::Degrees,
            decimal_places: 0,
        }
    }
}
pub fn validate_angle_style(style: AngleStyle) -> Result<(), ErrorCode> {
    if !style.stroke_width.is_finite()
        || !(1.0..=72.0).contains(&style.stroke_width)
        || style.decimal_places > 3
    {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok(())
}
impl AngleStyle {
    pub fn from_arrow(arrow: &ArrowData) -> Option<Self> {
        let angle = arrow.angle?;
        Some(Self {
            stroke: arrow.stroke,
            stroke_width: arrow.stroke_width,
            unit: angle.unit,
            decimal_places: angle.decimal_places,
        })
    }
    pub fn apply_to_arrow(self, arrow: &mut ArrowData) {
        arrow.linear_kind = LinearElementKind::Angle;
        arrow.distance = None;
        arrow.stroke = self.stroke;
        arrow.stroke_width = self.stroke_width;
        arrow.arrow_type = ArrowType::Straight;
        arrow.arrow_shaft_type = Default::default();
        arrow.stroke_style = StrokeStyle::Solid;
        arrow.fill = Default::default();
        arrow.start_arrowhead = None;
        arrow.end_arrowhead = None;
        arrow.start_binding = None;
        arrow.end_binding = None;
        arrow.text_path_fraction = None;
        let angle = arrow.angle.get_or_insert_with(AngleAnnotation::default);
        angle.unit = self.unit;
        angle.decimal_places = self.decimal_places;
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
            unit: if properties & 4 != 0 {
                value.unit
            } else {
                self.unit
            },
            decimal_places: if properties & 8 != 0 {
                value.decimal_places
            } else {
                self.decimal_places
            },
        }
    }
    fn differences(self, other: Self) -> u32 {
        u32::from(self.stroke != other.stroke)
            | (u32::from(self.stroke_width != other.stroke_width) << 1)
            | (u32::from(self.unit != other.unit) << 2)
            | (u32::from(self.decimal_places != other.decimal_places) << 3)
    }
}

pub(crate) fn angle_with_moved_point(
    arrow: &ArrowData,
    index: usize,
    point: Point<f64>,
) -> Option<ArrowData> {
    let mut points = arrow.global_points();
    *points.get_mut(index)? = point;
    let mut next = ArrowData::from_global_points(
        &points,
        arrow.stroke,
        arrow.stroke_width,
        StrokeStyle::Solid,
        ArrowType::Straight,
        None,
        None,
    )?;
    next.inherit_linear_metadata_from(arrow);
    next.rotation = arrow.rotation;
    if next.angle.is_some_and(|angle| angle.full_turn)
        && snow_draw_engine_document::angle_geometry(&next).is_none()
    {
        next.angle.as_mut()?.full_turn = false;
    }
    if snow_draw_engine_document::validate_arrow(&next).is_err() {
        return None;
    }
    Some(next)
}

fn adjusted_angle(arrow: &ArrowData, delta_radians: f64) -> Option<ArrowData> {
    let current = snow_draw_engine_document::angle_value(arrow)?;
    let next_value = (current + delta_radians).clamp(0.0, std::f64::consts::TAU);
    if next_value == current {
        return Some(arrow.clone());
    }
    let points = arrow.global_points();
    let vertex = points[1];
    let first = Point::new(points[0].x - vertex.x, points[0].y - vertex.y);
    let bearing = first.y.atan2(first.x) - next_value;
    let length = (points[2].x - vertex.x).hypot(points[2].y - vertex.y);
    let endpoint = Point::new(
        vertex.x + length * bearing.cos(),
        vertex.y + length * bearing.sin(),
    );
    let mut next = angle_with_moved_point(arrow, 2, endpoint)?;
    next.angle.as_mut()?.full_turn = next_value == std::f64::consts::TAU;
    Some(next)
}

impl Editor {
    /// An allocation-free equality token for hosts retaining fractional wheel input.
    /// Geometry/style changes keep the token; changing the wheel target replaces it.
    pub fn angle_adjustment_target_revision(&self, document: &DocumentModel) -> u64 {
        use std::hash::{Hash, Hasher};
        let mut token = std::collections::hash_map::DefaultHasher::new();
        (self.state.active_tool as u8).hash(&mut token);
        if self.state.active_tool == crate::ActiveTool::Angle
            && let crate::InteractionState::CreatingArrow(state) = &self.state.interaction
        {
            1_u8.hash(&mut token);
            document.peek_next_element_id().hash(&mut token);
            state.pointer_id.hash(&mut token);
            state.committed_points.len().hash(&mut token);
            for point in &state.committed_points {
                point.x.to_bits().hash(&mut token);
                point.y.to_bits().hash(&mut token);
            }
            matches!(&self.state.creation_preview, Some(ElementCreationPreview::Arrow(arrow)) if arrow.is_angle()).hash(&mut token);
        } else {
            2_u8.hash(&mut token);
            self.state.selection.ids.hash(&mut token);
        }
        token.finish().max(1)
    }

    pub(crate) fn has_homogeneous_angle_selection(&self, document: &DocumentModel) -> bool {
        !self.state.selection.ids.is_empty()
            && self
                .state
                .selection
                .ids
                .iter()
                .all(|id| document.arrow(*id).is_ok_and(ArrowData::is_angle))
    }
    pub fn angle_style(&self, document: &DocumentModel) -> AngleStyle {
        if !self.has_homogeneous_angle_selection(document) {
            return self.state.default_angle_style;
        }
        self.state
            .selection
            .primary
            .and_then(|id| document.arrow(id).ok())
            .and_then(AngleStyle::from_arrow)
            .or_else(|| {
                self.state
                    .selection
                    .ids
                    .iter()
                    .find_map(|id| document.arrow(*id).ok().and_then(AngleStyle::from_arrow))
            })
            .unwrap_or(self.state.default_angle_style)
    }
    pub fn angle_style_mixed(&self, document: &DocumentModel) -> u32 {
        if !self.has_homogeneous_angle_selection(document) {
            return 0;
        }
        let mut styles = self
            .state
            .selection
            .ids
            .iter()
            .filter_map(|id| document.arrow(*id).ok().and_then(AngleStyle::from_arrow));
        let Some(first) = styles.next() else {
            return 0;
        };
        styles.fold(0, |mixed, style| mixed | first.differences(style))
    }
    pub fn set_angle_style_patch(
        &mut self,
        document: &DocumentModel,
        style: AngleStyle,
        properties: u32,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        if properties & !ANGLE_STYLE_PROPERTY_ALL != 0 {
            return Err(ErrorCode::InvalidArgument);
        }
        let next_default = self.state.default_angle_style.merged(style, properties);
        validate_angle_style(next_default)?;
        let mut transaction = Transaction::new("update angle style properties");
        let mut updates = Vec::new();
        let selected_ids = if self.has_homogeneous_angle_selection(document) {
            self.state.selection.ids.as_slice()
        } else {
            &[]
        };
        for id in selected_ids.iter().copied() {
            let Ok(arrow) = document.arrow(id) else {
                continue;
            };
            let Some(current) = AngleStyle::from_arrow(arrow) else {
                continue;
            };
            let updated_style = current.merged(style, properties);
            validate_angle_style(updated_style)?;
            let mut next = arrow.clone();
            updated_style.apply_to_arrow(&mut next);
            if &next != arrow {
                transaction.update_arrow(id, next.clone());
            }
            updates.push(SelectionArrowState { id, arrow: next });
        }
        let edits_selection = !updates.is_empty();
        if !edits_selection {
            self.state.default_angle_style = next_default;
        }
        if !edits_selection
            && let Some(ElementCreationPreview::Arrow(arrow)) = &mut self.state.creation_preview
            && self.state.active_tool == crate::ActiveTool::Angle
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
        if !edits_selection
            && let crate::InteractionState::CreatingArrow(state) = &mut self.state.interaction
            && let Some((_, arrow)) = &mut state.angle_wheel_lock
        {
            next_default.apply_to_arrow(arrow);
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
    pub fn adjust_angle_value(
        &mut self,
        document: &DocumentModel,
        delta_radians: f64,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        if !delta_radians.is_finite() {
            return Err(ErrorCode::InvalidArgument);
        }
        if delta_radians == 0.0 {
            return Ok(None);
        }
        if self.state.active_tool == crate::ActiveTool::Angle
            && let crate::InteractionState::CreatingArrow(state) = &self.state.interaction
        {
            if state.committed_points.len() != 2 {
                return Ok(None);
            }
            let Some(ElementCreationPreview::Arrow(arrow)) = self.state.creation_preview.as_ref()
            else {
                return Ok(None);
            };
            let Some(next) = adjusted_angle(arrow, delta_radians) else {
                return Ok(None);
            };
            if &next == arrow {
                return Ok(None);
            }
            let lock_position = state
                .angle_last_view_position
                .unwrap_or(state.press_view_position);
            if let crate::InteractionState::CreatingArrow(state) = &mut self.state.interaction {
                state.angle_wheel_lock = Some((lock_position, next.clone()));
            }
            self.set_creation_preview(Some(ElementCreationPreview::Arrow(next)), Vec::new());
            return Ok(None);
        }
        if !matches!(self.state.interaction, crate::InteractionState::Idle)
            || !self.has_homogeneous_angle_selection(document)
        {
            return Ok(None);
        }
        let snapshot = self.capture_document_sync_snapshot(document);
        let mut transaction = Transaction::new("adjust angle");
        for id in self.state.selection.ids.iter().copied() {
            let arrow = document.arrow(id)?;
            let Some(next) = adjusted_angle(arrow, delta_radians) else {
                continue;
            };
            if &next != arrow {
                transaction.update_arrow(id, next);
            }
        }
        if transaction.is_empty() {
            return Ok(None);
        }
        Ok(Some(EditorCommand::ApplyTransaction(
            ApplyTransactionCommand::with_history_undo_snapshot(transaction, snapshot),
        )))
    }
    pub(crate) fn process_angle_creation_pointer_event(
        &mut self,
        document: &DocumentModel,
        event: crate::PointerEvent,
    ) -> Result<crate::InteractionOutput, ErrorCode> {
        use crate::{
            ArrowCreationPhase, CursorCommand, CursorStyle, InteractionState, PointerButton,
            PointerCaptureCommand, PointerEventType,
        };
        let InteractionState::CreatingArrow(state) = &self.state.interaction else {
            return Ok(Default::default());
        };
        if state.pointer_id != event.pointer_id {
            return Ok(Default::default());
        }
        let mut state = state.clone();
        state.angle_last_view_position = Some(event.position);
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
        if state
            .angle_wheel_lock
            .as_ref()
            .is_some_and(|(position, _)| {
                crate::pointer_drag_distance(*position, event.position) > 3.0
            })
        {
            state.angle_wheel_lock = None;
        }
        let point = crate::view_to_canvas(event.position, &self.camera(), self.surface_size());
        let (mut point, guides) = self.snap_arrow_creation_point(document, point, event.modifiers);
        if event.modifiers.shift {
            point = crate::lock_linear_point_to_discrete_angle(
                *state.committed_points.last().unwrap(),
                point,
            );
        }
        let preview = if let Some((_, arrow)) = &state.angle_wheel_lock {
            Some(arrow.clone())
        } else {
            let mut points = state.committed_points.clone();
            points.push(point);
            ArrowData::from_global_points(
                &points,
                self.state.default_angle_style.stroke,
                self.state.default_angle_style.stroke_width,
                StrokeStyle::Solid,
                ArrowType::Straight,
                None,
                None,
            )
            .and_then(|mut arrow| {
                if points.len() == 3 {
                    self.state.default_angle_style.apply_to_arrow(&mut arrow);
                }
                (points.len() != 3 || snow_draw_engine_document::validate_arrow(&arrow).is_ok())
                    .then_some(arrow)
            })
        };
        let mut capture = PointerCaptureCommand::NoChange;
        match event.event_type {
            PointerEventType::Down | PointerEventType::DoubleClick
                if event.button == Some(PointerButton::Primary)
                    && state.phase == ArrowCreationPhase::AwaitingEndpoint =>
            {
                state.phase = ArrowCreationPhase::EndpointPress;
                state.press_view_position = event.position;
                capture = self.capture_command_for_start(event.pointer_id);
            }
            PointerEventType::Up => {
                capture = self.release_capture_command();
                if state.phase == ArrowCreationPhase::EndpointPress {
                    if state.committed_points.len() == 1 {
                        let first = state.committed_points[0];
                        if (first.x - point.x).hypot(first.y - point.y) > 1e-6 {
                            state.committed_points.push(point);
                        }
                    } else if let Some(arrow) = preview.as_ref().filter(|a| a.is_angle()) {
                        self.queue_arrow_creation(document, arrow.clone())?;
                        self.cancel_interaction();
                        return Ok(crate::InteractionOutput {
                            consumed: true,
                            capture,
                            cursor: CursorCommand::Set(CursorStyle::Crosshair),
                        });
                    }
                }
                state.phase = ArrowCreationPhase::AwaitingEndpoint;
            }
            _ => {}
        }
        self.state.interaction = InteractionState::CreatingArrow(state);
        self.set_creation_preview(preview.map(ElementCreationPreview::Arrow), guides);
        Ok(crate::InteractionOutput {
            consumed: true,
            capture,
            cursor: CursorCommand::Set(CursorStyle::Crosshair),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn angle() -> ArrowData {
        let mut arrow = ArrowData::from_global_points(
            &[
                Point::new(100.0, 0.0),
                Point::new(0.0, 0.0),
                Point::new(0.0, -100.0),
            ],
            AngleStyle::default().stroke,
            2.0,
            StrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )
        .unwrap();
        AngleStyle::default().apply_to_arrow(&mut arrow);
        arrow
    }

    #[test]
    fn angle_point_edits_preserve_other_points_and_reject_zero_length_rays() {
        let original = angle();
        let vertex = angle_with_moved_point(&original, 1, Point::new(-50.0, 20.0)).unwrap();
        assert_eq!(vertex.global_points()[0], original.global_points()[0]);
        assert_eq!(vertex.global_points()[2], original.global_points()[2]);
        assert_eq!(vertex.global_points()[1], Point::new(-50.0, 20.0));
        assert_eq!(vertex.points.len(), 3);
        assert!(angle_with_moved_point(&original, 0, Point::new(0.0, 0.0)).is_none());
        assert!(angle_with_moved_point(&original, 1, Point::new(100.0, 0.0)).is_none());
        assert!(angle_with_moved_point(&original, 2, Point::new(0.0, 0.0)).is_none());
        let reflex = angle_with_moved_point(&original, 2, Point::new(0.0, 100.0)).unwrap();
        assert!(
            (snow_draw_engine_document::angle_value(&reflex)
                .unwrap()
                .to_degrees()
                - 270.0)
                .abs()
                < 1e-9
        );
    }

    #[test]
    fn angle_full_turn_point_edits_keep_identity_only_when_rays_stay_aligned() {
        let full_turn = adjusted_angle(&angle(), std::f64::consts::TAU).unwrap();
        assert!(full_turn.angle.unwrap().full_turn);
        let longer = angle_with_moved_point(&full_turn, 2, Point::new(200.0, 0.0)).unwrap();
        assert!(longer.angle.unwrap().full_turn);
        assert_eq!(
            snow_draw_engine_document::angle_value(&longer),
            Some(std::f64::consts::TAU)
        );
        let turned = angle_with_moved_point(&full_turn, 2, Point::new(0.0, -100.0)).unwrap();
        assert!(!turned.angle.unwrap().full_turn);
        assert_eq!(
            snow_draw_engine_document::angle_value(&turned),
            Some(std::f64::consts::FRAC_PI_2)
        );
        let zero = adjusted_angle(&full_turn, -std::f64::consts::TAU).unwrap();
        assert!(!zero.angle.unwrap().full_turn);
        assert_eq!(snow_draw_engine_document::angle_value(&zero), Some(0.0));
    }
}
