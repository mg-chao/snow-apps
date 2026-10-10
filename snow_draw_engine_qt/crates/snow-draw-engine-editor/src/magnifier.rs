use crate::*;
use serde::{Deserialize, Serialize};
use snow_draw_engine_core::{ColorRgba8, CornerRadii, ErrorCode, Point, arrow::Arrowhead};
use snow_draw_engine_document::{ElementId, MagnifierData, Transaction};
use snow_draw_engine_model::DocumentModel;

pub const MAGNIFIER_STYLE_PROPERTY_SHAPE: u32 = 1;
pub const MAGNIFIER_STYLE_PROPERTY_STROKE: u32 = 2;
pub const MAGNIFIER_STYLE_PROPERTY_STROKE_WIDTH: u32 = 4;
pub const MAGNIFIER_STYLE_PROPERTY_FACTOR: u32 = 8;
pub const MAGNIFIER_STYLE_PROPERTY_SHOW_LEADER: u32 = 16;
pub const MAGNIFIER_STYLE_PROPERTY_LEADER_ARROWHEAD: u32 = 32;
pub const MAGNIFIER_STYLE_PROPERTY_CORNER_RADII: u32 = 64;
pub const MAGNIFIER_STYLE_PROPERTY_ALL: u32 = 127;

#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
pub struct MagnifierStyle {
    pub shape: snow_draw_engine_document::HighlightShape,
    pub stroke: ColorRgba8,
    pub stroke_width: f64,
    pub factor: f64,
    pub show_leader: bool,
    pub leader_arrowhead: Option<Arrowhead>,
    #[serde(default = "default_corner_radii")]
    pub corner_radii: CornerRadii,
}
fn default_corner_radii() -> CornerRadii {
    CornerRadii::splat(6.0)
}
impl Default for MagnifierStyle {
    fn default() -> Self {
        Self::from_data(&MagnifierData::default())
    }
}
impl MagnifierStyle {
    pub fn from_data(value: &MagnifierData) -> Self {
        Self {
            shape: value.shape,
            stroke: value.stroke,
            stroke_width: value.stroke_width,
            factor: value.factor,
            show_leader: value.show_leader,
            leader_arrowhead: value.leader_arrowhead,
            corner_radii: value.corner_radii,
        }
    }
    pub fn apply(self, value: &mut MagnifierData) {
        value.shape = self.shape;
        value.stroke = self.stroke;
        value.stroke_width = self.stroke_width;
        value.factor = self.factor;
        value.show_leader = self.show_leader;
        value.leader_arrowhead = self.leader_arrowhead;
        value.corner_radii = self.corner_radii;
    }
    pub fn merged(self, next: Self, properties: u32) -> Self {
        Self {
            shape: if properties & 1 != 0 {
                next.shape
            } else {
                self.shape
            },
            stroke: if properties & 2 != 0 {
                next.stroke
            } else {
                self.stroke
            },
            stroke_width: if properties & 4 != 0 {
                next.stroke_width
            } else {
                self.stroke_width
            },
            factor: if properties & 8 != 0 {
                next.factor
            } else {
                self.factor
            },
            show_leader: if properties & 16 != 0 {
                next.show_leader
            } else {
                self.show_leader
            },
            leader_arrowhead: if properties & 32 != 0 {
                next.leader_arrowhead
            } else {
                self.leader_arrowhead
            },
            corner_radii: if properties & MAGNIFIER_STYLE_PROPERTY_CORNER_RADII != 0 {
                next.corner_radii
            } else {
                self.corner_radii
            },
        }
    }
    fn differences(self, other: Self) -> u32 {
        u32::from(self.shape != other.shape)
            | (u32::from(self.stroke != other.stroke) << 1)
            | (u32::from(self.stroke_width != other.stroke_width) << 2)
            | (u32::from(self.factor != other.factor) << 3)
            | (u32::from(self.show_leader != other.show_leader) << 4)
            | (u32::from(self.leader_arrowhead != other.leader_arrowhead) << 5)
            | (u32::from(self.corner_radii != other.corner_radii) << 6)
    }
}
pub fn validate_magnifier_style(style: MagnifierStyle) -> Result<(), ErrorCode> {
    if !style.stroke_width.is_finite()
        || !(0.0..=72.0).contains(&style.stroke_width)
        || !style.factor.is_finite()
        || !(1.0..=10.0).contains(&style.factor)
        || [
            style.corner_radii.top_left,
            style.corner_radii.top_right,
            style.corner_radii.bottom_right,
            style.corner_radii.bottom_left,
        ]
        .iter()
        .any(|radius| !radius.is_finite() || *radius < 0.0)
    {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok(())
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SelectionMagnifierState {
    pub id: ElementId,
    pub magnifier: MagnifierData,
}

impl Editor {
    pub(crate) fn has_homogeneous_magnifier_selection(&self, document: &DocumentModel) -> bool {
        !self.state.selection.ids.is_empty()
            && self
                .state
                .selection
                .ids
                .iter()
                .all(|id| document.magnifier(*id).is_ok())
    }
    pub fn magnifier_style(&self, document: &DocumentModel) -> MagnifierStyle {
        self.state
            .selection
            .ids
            .iter()
            .find_map(|id| document.magnifier(*id).ok().map(MagnifierStyle::from_data))
            .unwrap_or(self.state.default_magnifier_style)
    }
    pub fn magnifier_style_mixed(&self, document: &DocumentModel) -> u32 {
        let mut values = self
            .state
            .selection
            .ids
            .iter()
            .filter_map(|id| document.magnifier(*id).ok().map(MagnifierStyle::from_data));
        let Some(first) = values.next() else {
            return 0;
        };
        values.fold(0, |mask, value| mask | first.differences(value))
    }
    pub fn set_magnifier_style_patch(
        &mut self,
        document: &DocumentModel,
        style: MagnifierStyle,
        properties: u32,
        creation_defaults: bool,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        if properties & !MAGNIFIER_STYLE_PROPERTY_ALL != 0 {
            return Err(ErrorCode::InvalidArgument);
        }
        validate_magnifier_style(style)?;
        if !creation_defaults && !self.has_homogeneous_magnifier_selection(document) {
            return Err(ErrorCode::InvalidState);
        }
        let mut transaction = Transaction::new("set magnifier style");
        if !creation_defaults && self.has_homogeneous_magnifier_selection(document) {
            for id in &self.state.selection.ids {
                let previous = document.magnifier(*id)?;
                let mut next = *previous;
                MagnifierStyle::from_data(previous)
                    .merged(style, properties)
                    .apply(&mut next);
                snow_draw_engine_document::validate_magnifier(&next)?;
                if next != *previous {
                    transaction.update_magnifier(*id, next);
                }
            }
        } else {
            let next = self.state.default_magnifier_style.merged(style, properties);
            validate_magnifier_style(next)?;
            self.state.default_magnifier_style = next;
            if let Some(ElementCreationPreview::Magnifier(value)) = &mut self.state.creation_preview
            {
                let previous = *value;
                next.apply(value);
                if *value != previous {
                    self.bump_scene_state_revision();
                }
            }
        }
        if transaction.is_empty() {
            return Ok(None);
        }
        let snapshot = self.capture_document_sync_snapshot(document);
        Ok(Some(EditorCommand::ApplyTransaction(
            ApplyTransactionCommand::with_history_undo_snapshot(transaction, snapshot),
        )))
    }
    pub(crate) fn magnifier_creation_data(&self, rect: RectangleData) -> MagnifierData {
        let mut value = MagnifierData {
            source_center: rect.center,
            magnified_center: rect.center,
            width: rect.width,
            height: rect.height,
            rotation: rect.rotation,
            ..MagnifierData::default()
        };
        self.state.default_magnifier_style.apply(&mut value);
        value
    }
    pub(crate) fn magnifier_snapshot(
        &self,
        document: &DocumentModel,
        id: ElementId,
    ) -> Option<MagnifierData> {
        if let InteractionState::EditingMagnifier(state) = &self.state.interaction
            && state.id == id
        {
            return Some(state.preview);
        }
        if let InteractionState::EditingSelection(state) = &self.state.interaction
            && let Some(value) = self
                .selection_magnifier_previews(document, state)
                .into_iter()
                .find(|value| value.id == id)
        {
            return Some(value.magnifier);
        }
        document.magnifier(id).ok().copied()
    }
    pub(crate) fn selection_magnifier_previews(
        &self,
        _document: &DocumentModel,
        state: &EditSelectionState,
    ) -> Vec<SelectionMagnifierState> {
        let single = state.original_elements.len() == 1 && state.original_arrows.is_empty();
        state
            .preview_elements
            .iter()
            .filter_map(|preview| {
                let original = state
                    .original_magnifiers
                    .iter()
                    .find(|value| value.id == preview.id)?
                    .magnifier;
                let old_proxy = state
                    .original_elements
                    .iter()
                    .find(|value| value.id == preview.id)?
                    .rect;
                let mut next = original;
                if single {
                    match state.mode {
                        SelectionEditMode::Move { .. } => {
                            next = original.translated(Point::new(
                                preview.rect.center.x - old_proxy.center.x,
                                preview.rect.center.y - old_proxy.center.y,
                            ))
                        }
                        SelectionEditMode::Resize { .. } => {
                            next.source_center = preview.rect.center;
                            next.width = preview.rect.width;
                            next.height = preview.rect.height;
                        }
                        SelectionEditMode::Rotate { .. } => next.rotation = preview.rect.rotation,
                        SelectionEditMode::CornerRadius { .. } => {
                            next.corner_radii = preview.rect.corner_radii;
                        }
                    }
                } else {
                    let delta_angle = preview.rect.rotation - old_proxy.rotation;
                    let sx = preview.rect.width / old_proxy.width.max(0.0001);
                    let sy = preview.rect.height / old_proxy.height.max(0.0001);
                    let map = |point: Point<f64>| {
                        let local =
                            canvas_to_rect_local(old_proxy.center, old_proxy.rotation, point);
                        rect_local_to_canvas(
                            preview.rect.center,
                            preview.rect.rotation,
                            Point::new(local.x * sx, local.y * sy),
                        )
                    };
                    next.source_center = map(original.source_center);
                    next.magnified_center = map(original.magnified_center);
                    next.width *= sx;
                    next.height *= sy;
                    next.rotation = normalize_rotation(original.rotation + delta_angle);
                }
                Some(SelectionMagnifierState {
                    id: preview.id,
                    magnifier: next,
                })
            })
            .collect()
    }
    pub(crate) fn process_magnifier_pointer_event(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        let InteractionState::EditingMagnifier(state) = &self.state.interaction else {
            return Ok(Default::default());
        };
        if state.pointer_id != event.pointer_id {
            return Ok(Default::default());
        }
        let mut state = *state;
        match event.event_type {
            PointerEventType::Move | PointerEventType::Enter | PointerEventType::Up => {
                let point = view_to_canvas(event.position, &self.camera(), self.surface_size());
                let delta = Point::new(point.x - state.start.x, point.y - state.start.y);
                state.preview = state.original;
                state.preview.magnified_center.x += delta.x;
                state.preview.magnified_center.y += delta.y;
                self.state.interaction = InteractionState::EditingMagnifier(state);
                self.bump_scene_state_revision();
                self.bump_overlay_state_revision();
                if event.event_type == PointerEventType::Up {
                    self.cancel_interaction();
                    if state.preview != state.original {
                        let mut transaction = Transaction::new("move magnifier lens");
                        transaction.update_magnifier(state.id, state.preview);
                        self.queue_command(EditorCommand::ApplyTransaction(
                            ApplyTransactionCommand::with_history_undo_snapshot(
                                transaction,
                                self.capture_document_sync_snapshot(document),
                            ),
                        ));
                    }
                }
                Ok(InteractionOutput {
                    consumed: true,
                    capture: if event.event_type == PointerEventType::Up {
                        self.release_capture_command()
                    } else {
                        PointerCaptureCommand::NoChange
                    },
                    cursor: CursorCommand::Set(CursorStyle::Move),
                })
            }
            PointerEventType::Cancel => {
                self.cancel_interaction();
                Ok(InteractionOutput {
                    consumed: true,
                    capture: self.release_capture_command(),
                    cursor: CursorCommand::Set(CursorStyle::Move),
                })
            }
            _ => Ok(Default::default()),
        }
    }
}

/// Outside the lens so its body stays reachable when source and destination coincide.
pub fn magnifier_move_grip(value: MagnifierData, zoom: f64) -> Point<f64> {
    let offset = value.height * value.factor / 2.0 + 24.0 / zoom.max(0.0001);
    let (sin, cos) = value.rotation.sin_cos();
    Point::new(
        value.magnified_center.x - offset * sin,
        value.magnified_center.y + offset * cos,
    )
}
