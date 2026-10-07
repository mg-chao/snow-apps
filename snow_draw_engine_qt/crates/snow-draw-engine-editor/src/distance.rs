use serde::{Deserialize, Serialize};
use snow_draw_engine_core::{
    ColorRgba8, ErrorCode, Point,
    arrow::{ArrowType, Arrowhead, StrokeStyle},
};
use snow_draw_engine_document::{
    ArrowData, DistanceAnnotation, DistanceUnit, ElementId, LinearElementKind, Transaction,
};
use snow_draw_engine_model::DocumentModel;

use crate::{
    ApplyTransactionCommand, Editor, EditorCommand, ElementCreationPreview, SelectionArrowState,
};

pub const DISTANCE_STYLE_PROPERTY_STROKE: u32 = 1 << 0;
pub const DISTANCE_STYLE_PROPERTY_STROKE_WIDTH: u32 = 1 << 1;
pub const DISTANCE_STYLE_PROPERTY_FACTOR: u32 = 1 << 2;
pub const DISTANCE_STYLE_PROPERTY_UNIT: u32 = 1 << 3;
pub const DISTANCE_STYLE_PROPERTY_DECIMAL_PLACES: u32 = 1 << 4;
pub const DISTANCE_STYLE_PROPERTY_ENDPOINT_RATIO: u32 = 1 << 5;
pub const DISTANCE_STYLE_PROPERTY_ENDPOINT_STYLE: u32 = 1 << 6;
pub const DISTANCE_STYLE_PROPERTY_ALL: u32 = (1 << 7) - 1;

pub const DISTANCE_STYLE_MIXED_STROKE: u32 = DISTANCE_STYLE_PROPERTY_STROKE;
pub const DISTANCE_STYLE_MIXED_STROKE_WIDTH: u32 = DISTANCE_STYLE_PROPERTY_STROKE_WIDTH;
pub const DISTANCE_STYLE_MIXED_FACTOR: u32 = DISTANCE_STYLE_PROPERTY_FACTOR;
pub const DISTANCE_STYLE_MIXED_UNIT: u32 = DISTANCE_STYLE_PROPERTY_UNIT;
pub const DISTANCE_STYLE_MIXED_DECIMAL_PLACES: u32 = DISTANCE_STYLE_PROPERTY_DECIMAL_PLACES;
pub const DISTANCE_STYLE_MIXED_ENDPOINT_RATIO: u32 = DISTANCE_STYLE_PROPERTY_ENDPOINT_RATIO;
pub const DISTANCE_STYLE_MIXED_ENDPOINT_STYLE: u32 = DISTANCE_STYLE_PROPERTY_ENDPOINT_STYLE;

#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
pub struct DistanceStyle {
    pub stroke: ColorRgba8,
    pub stroke_width: f64,
    pub factor: f64,
    pub unit: DistanceUnit,
    pub decimal_places: u8,
    pub endpoint_ratio: f64,
    pub endpoint_style: Option<Arrowhead>,
}

impl Default for DistanceStyle {
    fn default() -> Self {
        Self {
            stroke: ColorRgba8 {
                r: 0x21,
                g: 0x6b,
                b: 0xa5,
                a: 255,
            },
            stroke_width: 2.0,
            factor: 1.0,
            unit: DistanceUnit::default(),
            decimal_places: 0,
            endpoint_ratio: 1.0,
            endpoint_style: Some(Arrowhead::Bar),
        }
    }
}

pub fn validate_distance_style(style: DistanceStyle) -> Result<(), ErrorCode> {
    snow_draw_engine_document::validate_distance_annotation(DistanceAnnotation {
        factor: style.factor,
        unit: style.unit,
        decimal_places: style.decimal_places,
        ..Default::default()
    })?;
    if !style.stroke_width.is_finite()
        || !(1.0..=72.0).contains(&style.stroke_width)
        || !style.endpoint_ratio.is_finite()
        || !(0.5..=3.0).contains(&style.endpoint_ratio)
    {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok(())
}

impl DistanceStyle {
    pub fn from_arrow(arrow: &ArrowData) -> Option<Self> {
        let distance = arrow.distance?;
        Some(Self {
            stroke: arrow.stroke,
            stroke_width: arrow.stroke_width,
            factor: distance.factor,
            unit: distance.unit,
            decimal_places: distance.decimal_places,
            endpoint_ratio: arrow.arrow_ratio,
            endpoint_style: arrow.start_arrowhead,
        })
    }

    pub fn apply_to_arrow(self, arrow: &mut ArrowData) {
        arrow.linear_kind = LinearElementKind::Distance;
        arrow.stroke = self.stroke;
        arrow.stroke_width = self.stroke_width;
        arrow.start_arrowhead = self.endpoint_style;
        arrow.end_arrowhead = self.endpoint_style;
        arrow.arrow_ratio = self.endpoint_ratio;
        arrow.arrow_type = ArrowType::Straight;
        arrow.arrow_shaft_type = Default::default();
        arrow.stroke_style = StrokeStyle::Solid;
        arrow.text_path_fraction = None;
        let distance = arrow.distance.get_or_insert_with(Default::default);
        distance.factor = self.factor;
        distance.unit = self.unit;
        distance.decimal_places = self.decimal_places;
    }

    pub fn merged(self, value: Self, properties: u32) -> Self {
        let mut next = self;
        if properties & DISTANCE_STYLE_PROPERTY_STROKE != 0 {
            next.stroke = value.stroke;
        }
        if properties & DISTANCE_STYLE_PROPERTY_STROKE_WIDTH != 0 {
            next.stroke_width = value.stroke_width;
        }
        if properties & DISTANCE_STYLE_PROPERTY_FACTOR != 0 {
            next.factor = value.factor;
        }
        if properties & DISTANCE_STYLE_PROPERTY_UNIT != 0 {
            next.unit = value.unit;
        }
        if properties & DISTANCE_STYLE_PROPERTY_DECIMAL_PLACES != 0 {
            next.decimal_places = value.decimal_places;
        }
        if properties & DISTANCE_STYLE_PROPERTY_ENDPOINT_RATIO != 0 {
            next.endpoint_ratio = value.endpoint_ratio;
        }
        if properties & DISTANCE_STYLE_PROPERTY_ENDPOINT_STYLE != 0 {
            next.endpoint_style = value.endpoint_style;
        }
        next
    }

    fn differences(self, other: Self) -> u32 {
        let mut mixed = 0;
        if self.stroke != other.stroke {
            mixed |= DISTANCE_STYLE_PROPERTY_STROKE;
        }
        if self.stroke_width != other.stroke_width {
            mixed |= DISTANCE_STYLE_PROPERTY_STROKE_WIDTH;
        }
        if self.factor != other.factor {
            mixed |= DISTANCE_STYLE_PROPERTY_FACTOR;
        }
        if self.unit != other.unit {
            mixed |= DISTANCE_STYLE_PROPERTY_UNIT;
        }
        if self.decimal_places != other.decimal_places {
            mixed |= DISTANCE_STYLE_PROPERTY_DECIMAL_PLACES;
        }
        if self.endpoint_ratio != other.endpoint_ratio {
            mixed |= DISTANCE_STYLE_PROPERTY_ENDPOINT_RATIO;
        }
        if self.endpoint_style != other.endpoint_style {
            mixed |= DISTANCE_STYLE_PROPERTY_ENDPOINT_STYLE;
        }
        mixed
    }
}

impl Editor {
    pub(crate) fn distance_preview(
        &self,
        point: Point<f64>,
        modifiers: crate::Modifiers,
    ) -> Option<ArrowData> {
        let crate::state::InteractionState::CreatingArrow(state) = &self.state.interaction else {
            return None;
        };
        let start = *state.committed_points.first()?;
        let point = if modifiers.shift {
            crate::lock_linear_point_to_discrete_angle(start, point)
        } else {
            point
        };
        let mut arrow = ArrowData::from_global_points(
            &[start, point],
            self.state.default_distance_style.stroke,
            self.state.default_distance_style.stroke_width,
            StrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )?;
        arrow.distance = Some(DistanceAnnotation {
            pixel_scale_x: state.distance_pixel_scale.x,
            pixel_scale_y: state.distance_pixel_scale.y,
            ..Default::default()
        });
        self.state.default_distance_style.apply_to_arrow(&mut arrow);
        Some(arrow)
    }

    pub(crate) fn process_distance_creation_pointer_event(
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
        let state = state.clone();
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
        let point = crate::view_to_canvas(event.position, &self.camera(), self.surface_size());
        let (point, guides) = self.snap_arrow_creation_point(document, point, event.modifiers);
        let arrow = self.distance_preview(point, event.modifiers);
        match event.event_type {
            PointerEventType::Down
                if event.button == Some(PointerButton::Primary)
                    && state.phase == ArrowCreationPhase::AwaitingEndpoint =>
            {
                self.arm_arrow_endpoint_creation(state, event.pointer_id, event.position);
                self.set_creation_preview(arrow.map(ElementCreationPreview::Arrow), guides);
                Ok(crate::InteractionOutput {
                    consumed: true,
                    capture: self.capture_command_for_start(event.pointer_id),
                    cursor: CursorCommand::Set(CursorStyle::Crosshair),
                })
            }
            PointerEventType::Move | PointerEventType::Enter => {
                self.set_creation_preview(arrow.map(ElementCreationPreview::Arrow), guides);
                Ok(crate::InteractionOutput {
                    consumed: true,
                    capture: PointerCaptureCommand::NoChange,
                    cursor: CursorCommand::Set(CursorStyle::Crosshair),
                })
            }
            PointerEventType::Up => {
                let drag =
                    crate::pointer_drag_distance(state.press_view_position, event.position) >= 3.0;
                if (state.phase == ArrowCreationPhase::EndpointPress || drag)
                    && let Some(arrow) = arrow
                        .as_ref()
                        .filter(|arrow| crate::document_arrow_length(arrow) > 1e-6)
                {
                    self.queue_arrow_creation(document, arrow.clone())?;
                    self.cancel_interaction();
                } else if state.phase == ArrowCreationPhase::EndpointPress || drag {
                    self.cancel_interaction();
                } else {
                    self.set_creation_preview(arrow.map(ElementCreationPreview::Arrow), guides);
                    if let InteractionState::CreatingArrow(state) = &mut self.state.interaction {
                        state.phase = ArrowCreationPhase::AwaitingEndpoint;
                    }
                }
                Ok(crate::InteractionOutput {
                    consumed: true,
                    capture: self.release_capture_command(),
                    cursor: CursorCommand::Set(CursorStyle::Crosshair),
                })
            }
            _ => Ok(Default::default()),
        }
    }

    pub fn distance_style(&self, document: &DocumentModel) -> DistanceStyle {
        self.state
            .selection
            .primary
            .and_then(|id| document.arrow(id).ok())
            .and_then(DistanceStyle::from_arrow)
            .or_else(|| {
                self.state
                    .selection
                    .ids
                    .iter()
                    .find_map(|id| document.arrow(*id).ok().and_then(DistanceStyle::from_arrow))
            })
            .unwrap_or(self.state.default_distance_style)
    }

    pub fn distance_style_mixed(&self, document: &DocumentModel) -> u32 {
        let mut selected = self
            .state
            .selection
            .ids
            .iter()
            .filter_map(|id| document.arrow(*id).ok().and_then(DistanceStyle::from_arrow));
        let Some(first) = selected.next() else {
            return 0;
        };
        selected.fold(0, |mixed, style| mixed | first.differences(style))
    }

    pub fn set_distance_style_patch(
        &mut self,
        document: &DocumentModel,
        style: DistanceStyle,
        properties: u32,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        if properties & !DISTANCE_STYLE_PROPERTY_ALL != 0 {
            return Err(ErrorCode::InvalidArgument);
        }
        let next_default = self.state.default_distance_style.merged(style, properties);
        validate_distance_style(next_default)?;
        let mut transaction = Transaction::new("update distance style properties");
        let mut updated_arrows = Vec::new();
        for id in self.state.selection.ids.iter().copied() {
            let Ok(arrow) = document.arrow(id) else {
                continue;
            };
            let Some(current) = DistanceStyle::from_arrow(arrow) else {
                continue;
            };
            let updated_style = current.merged(style, properties);
            validate_distance_style(updated_style)?;
            let mut updated = arrow.clone();
            updated_style.apply_to_arrow(&mut updated);
            snow_draw_engine_document::validate_arrow(&updated)?;
            if &updated != arrow {
                transaction.update_arrow(id, updated.clone());
            }
            updated_arrows.push(SelectionArrowState { id, arrow: updated });
        }
        let preview = self.state.creation_preview.as_ref().and_then(|preview| {
            let ElementCreationPreview::Arrow(arrow) = preview else {
                return None;
            };
            arrow.is_distance().then(|| {
                let mut updated = arrow.clone();
                next_default.apply_to_arrow(&mut updated);
                updated
            })
        });
        if let Some(arrow) = preview.as_ref() {
            snow_draw_engine_document::validate_arrow(arrow)?;
        }
        self.state.default_distance_style = next_default;
        if let Some(updated) = preview
            && let Some(ElementCreationPreview::Arrow(arrow)) = self.state.creation_preview.as_mut()
            && *arrow != updated
        {
            *arrow = updated;
            self.bump_scene_state_revision();
        }
        if transaction.is_empty() {
            return Ok(None);
        }
        let history_undo_snapshot = self.capture_document_sync_snapshot(document);
        for updated in updated_arrows {
            if let Some(arrow) = self
                .state
                .selection
                .arrows
                .iter_mut()
                .find(|a| a.id == updated.id)
            {
                *arrow = updated;
            }
        }
        Ok(Some(EditorCommand::ApplyTransaction(
            ApplyTransactionCommand::with_history_undo_snapshot(transaction, history_undo_snapshot),
        )))
    }

    pub(crate) fn distance_creation_text_id(&self) -> ElementId {
        ElementId {
            index: u32::MAX - 1,
            generation: self.state.distance_creation_generation,
        }
    }
}
