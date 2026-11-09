use crate::*;
use serde::{Deserialize, Serialize};
use snow_draw_engine_core::{
    ColorRgba8, CornerRadii, ErrorCode, MagnifierConnectionType, Point, arrow::Arrowhead,
};
use snow_draw_engine_document::{ElementId, MagnifierData, Transaction};
use snow_draw_engine_model::DocumentModel;
use std::collections::HashMap;

pub const MAGNIFIER_STYLE_PROPERTY_SHAPE: u32 = 1;
pub const MAGNIFIER_STYLE_PROPERTY_STROKE: u32 = 2;
pub const MAGNIFIER_STYLE_PROPERTY_STROKE_WIDTH: u32 = 4;
pub const MAGNIFIER_STYLE_PROPERTY_FACTOR: u32 = 8;
pub const MAGNIFIER_STYLE_PROPERTY_CONNECTION_TYPE: u32 = 16;
pub const MAGNIFIER_STYLE_PROPERTY_LEADER_ARROWHEAD: u32 = 32;
pub const MAGNIFIER_STYLE_PROPERTY_CORNER_RADII: u32 = 64;
pub const MAGNIFIER_STYLE_PROPERTY_ARROW_RATIO: u32 = 128;
pub const MAGNIFIER_STYLE_PROPERTY_SOURCE_OUTLINE: u32 = 256;
pub const MAGNIFIER_STYLE_PROPERTY_ALL: u32 = 511;

#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct MagnifierStyle {
    pub shape: snow_draw_engine_document::HighlightShape,
    pub stroke: ColorRgba8,
    pub stroke_width: f64,
    pub factor: f64,
    pub connection_type: MagnifierConnectionType,
    pub source_outline: bool,
    pub leader_arrowhead: Option<Arrowhead>,
    #[serde(
        default = "snow_draw_engine_core::arrow::default_arrow_ratio",
        deserialize_with = "snow_draw_engine_core::arrow::deserialize_arrow_ratio"
    )]
    pub arrow_ratio: f64,
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
            connection_type: value.connection_type,
            source_outline: value.source_outline,
            leader_arrowhead: value.leader_arrowhead,
            arrow_ratio: value.arrow_ratio,
            corner_radii: value.corner_radii,
        }
    }
    pub fn apply(self, value: &mut MagnifierData) {
        value.shape = self.shape;
        value.stroke = self.stroke;
        value.stroke_width = self.stroke_width;
        value.factor = self.factor;
        value.connection_type = self.connection_type;
        value.source_outline = self.source_outline;
        value.leader_arrowhead = self.leader_arrowhead;
        value.arrow_ratio = self.arrow_ratio;
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
            connection_type: if properties & MAGNIFIER_STYLE_PROPERTY_CONNECTION_TYPE != 0 {
                next.connection_type
            } else {
                self.connection_type
            },
            source_outline: if properties & MAGNIFIER_STYLE_PROPERTY_SOURCE_OUTLINE != 0 {
                next.source_outline
            } else {
                self.source_outline
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
            arrow_ratio: if properties & MAGNIFIER_STYLE_PROPERTY_ARROW_RATIO != 0 {
                next.arrow_ratio
            } else {
                self.arrow_ratio
            },
        }
    }
    fn differences(self, other: Self) -> u32 {
        u32::from(self.shape != other.shape)
            | (u32::from(self.stroke != other.stroke) << 1)
            | (u32::from(self.stroke_width != other.stroke_width) << 2)
            | (u32::from(self.factor != other.factor) << 3)
            | (u32::from(self.connection_type != other.connection_type) << 4)
            | (u32::from(self.leader_arrowhead != other.leader_arrowhead) << 5)
            | (u32::from(self.corner_radii != other.corner_radii) << 6)
            | (u32::from(self.arrow_ratio != other.arrow_ratio) << 7)
            | (u32::from(self.source_outline != other.source_outline) << 8)
    }
}
pub fn validate_magnifier_style(style: MagnifierStyle) -> Result<(), ErrorCode> {
    if style.shape == snow_draw_engine_document::HighlightShape::ThoughtBubble {
        return Err(ErrorCode::InvalidArgument);
    }
    if !style.stroke_width.is_finite()
        || !(0.0..=72.0).contains(&style.stroke_width)
        || !style.factor.is_finite()
        || !(1.0..=10.0).contains(&style.factor)
        || !style.arrow_ratio.is_finite()
        || !(0.5..=3.0).contains(&style.arrow_ratio)
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

// Input remainders are transient and do not belong to document history or persisted styles.
#[derive(Clone, Debug)]
pub(crate) struct MagnifierWheelAccumulator {
    selection: Vec<ElementId>,
    kind: WheelDeltaKind,
    direction: f64,
    factor: f64,
    delta: f64,
}

impl Editor {
    /// Document mutations own appearance; a pointer gesture owns only its geometry snapshot.
    /// Rebase after successful commits and history replay so finishing or cancelling a drag
    /// cannot undo a separately committed style edit.
    pub(crate) fn sync_magnifier_interaction_styles(&mut self, document: &DocumentModel) {
        let zoom = self.camera().zoom;
        match &mut self.state.interaction {
            InteractionState::EditingMagnifier(state) => {
                if let Ok(committed) = document.magnifier(state.id) {
                    let style = MagnifierStyle::from_data(committed);
                    style.apply(&mut state.original);
                    style.apply(&mut state.preview);
                    state.original.opacity = committed.opacity;
                    state.preview.opacity = committed.opacity;
                }
            }
            InteractionState::PendingSelectionMove(state) => {
                sync_selection_magnifier_styles(
                    document,
                    &mut state.original_magnifiers,
                    &mut state.original_elements,
                    &mut [],
                    state.original_arrows.is_empty(),
                    false,
                );
            }
            InteractionState::EditingSelection(state) => {
                let contour_changed = sync_selection_magnifier_styles(
                    document,
                    &mut state.original_magnifiers,
                    &mut state.original_elements,
                    &mut state.preview_elements,
                    state.original_arrows.is_empty(),
                    matches!(state.mode, SelectionEditMode::CornerRadius { .. }),
                );
                if contour_changed {
                    rebase_magnifier_corner_input(state, zoom);
                }
            }
            _ => {}
        }
    }

    pub(crate) fn process_magnifier_wheel_event(
        &mut self,
        document: &DocumentModel,
        event: WheelEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        if event.delta.y != 0.0 {
            let creation_defaults = !self.has_homogeneous_magnifier_selection(document);
            let selection = if creation_defaults {
                Vec::new()
            } else {
                self.state.selection.ids.clone()
            };
            let mut style = self.magnifier_style(document);
            let direction = event.delta.y.signum();
            let pending = self.magnifier_wheel.take().filter(|pending| {
                pending.selection == selection
                    && pending.kind == event.delta_kind
                    && pending.direction == direction
                    && pending.factor == style.factor
            });
            let delta = pending.map_or(0.0, |pending| pending.delta) + event.delta.y;
            let unit = match event.delta_kind {
                WheelDeltaKind::Angle => 120.0,
                WheelDeltaKind::Pixel => 100.0,
            };
            let steps = (delta / unit).trunc();
            if steps != 0.0 {
                style.factor = ((style.factor + steps * 0.1) * 10.0)
                    .round()
                    .clamp(10.0, 100.0)
                    / 10.0;
                if let Some(command) = self.set_magnifier_style_patch(
                    document,
                    style,
                    MAGNIFIER_STYLE_PROPERTY_FACTOR,
                    creation_defaults,
                )? {
                    self.queue_command(command);
                }
            }
            self.magnifier_wheel = Some(MagnifierWheelAccumulator {
                selection,
                kind: event.delta_kind,
                direction,
                factor: style.factor,
                delta: if (style.factor == 10.0 && direction > 0.0)
                    || (style.factor == 1.0 && direction < 0.0)
                {
                    0.0
                } else {
                    delta % unit
                },
            });
        }
        Ok(InteractionOutput {
            consumed: true,
            capture: PointerCaptureCommand::NoChange,
            cursor: CursorCommand::NoChange,
        })
    }

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
            .primary
            .and_then(|id| document.magnifier(id).ok())
            .or_else(|| {
                self.state
                    .selection
                    .ids
                    .iter()
                    .find_map(|id| document.magnifier(*id).ok())
            })
            .map(MagnifierStyle::from_data)
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
    /// Creation-only patches keep the selection untouched. Both targets remember
    /// the requested properties for subsequent magnifiers.
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
        let next_default = self.state.default_magnifier_style.merged(style, properties);
        validate_magnifier_style(next_default)?;
        if properties & MAGNIFIER_STYLE_PROPERTY_FACTOR != 0 {
            self.magnifier_wheel = None;
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
        }
        self.state.default_magnifier_style = next_default;
        self.edit_creation_colors(
            ActiveTool::Magnifier,
            (properties & MAGNIFIER_STYLE_PROPERTY_STROKE != 0).then_some(style.stroke),
            None,
        );
        let mut preview_style = next_default;
        preview_style.stroke = self.creation_stroke(preview_style.stroke);
        if let Some(ElementCreationPreview::Magnifier(value)) = &mut self.state.creation_preview {
            let previous = *value;
            preview_style.apply(value);
            if *value != previous {
                self.bump_scene_state_revision();
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
        let mut style = self.state.default_magnifier_style;
        style.stroke = self.creation_stroke(style.stroke);
        style.apply(&mut value);
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
            && let Some(value) = state.magnifier_preview(id)
        {
            return Some(value);
        }
        document.magnifier(id).ok().copied()
    }
    pub(crate) fn selection_magnifier_previews(
        &self,
        state: &EditSelectionState,
    ) -> Vec<SelectionMagnifierState> {
        state
            .preview_elements
            .iter()
            .filter_map(|preview| {
                state
                    .magnifier_preview(preview.id)
                    .map(|magnifier| SelectionMagnifierState {
                        id: preview.id,
                        magnifier,
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
        if event.event_type == PointerEventType::Up
            && event.button != Some(state.button)
            && !(state.button == PointerButton::Primary && event.button.is_none())
        {
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

impl EditSelectionState {
    /// Geometry comes from the pointer-down snapshot; appearance is rebased on document edits.
    /// Target lookups and bulk previews share this transform without rebuilding the whole group.
    pub(crate) fn magnifier_preview(&self, id: ElementId) -> Option<MagnifierData> {
        let original = *self.original_magnifiers.get(&id)?;
        let single = self.original_elements.len() == 1 && self.original_arrows.is_empty();
        let source_preview = if single {
            Some(self.preview_elements.first()?.rect)
        } else {
            None
        };
        let mut next = original;
        match self.preview_transform {
            SelectionTransform::Identity => {
                if let SelectionEditMode::CornerRadius { .. } = self.mode
                    && let Some(preview) = source_preview
                {
                    let source = original.source_rect().corner_radii;
                    let target = original.magnified_rect().corner_radii;
                    let target_radius = |radius, previous, raw, normalized| {
                        if radius == previous && raw == normalized {
                            raw
                        } else {
                            radius * next.factor
                        }
                    };
                    next.corner_radii = if preview.corner_radii == source {
                        original.corner_radii
                    } else {
                        CornerRadii {
                            top_left: target_radius(
                                preview.corner_radii.top_left,
                                source.top_left,
                                original.corner_radii.top_left,
                                target.top_left,
                            ),
                            top_right: target_radius(
                                preview.corner_radii.top_right,
                                source.top_right,
                                original.corner_radii.top_right,
                                target.top_right,
                            ),
                            bottom_right: target_radius(
                                preview.corner_radii.bottom_right,
                                source.bottom_right,
                                original.corner_radii.bottom_right,
                                target.bottom_right,
                            ),
                            bottom_left: target_radius(
                                preview.corner_radii.bottom_left,
                                source.bottom_left,
                                original.corner_radii.bottom_left,
                                target.bottom_left,
                            ),
                        }
                    };
                }
            }
            SelectionTransform::Move { delta } => next = original.translated(delta),
            transform @ SelectionTransform::Resize {
                scale_x, scale_y, ..
            } => {
                next.source_center =
                    transform.map_point(&self.original_bounds, original.source_center);
                if let Some(preview) = source_preview {
                    next.width = preview.width;
                    next.height = preview.height;
                } else {
                    next.magnified_center =
                        transform.map_point(&self.original_bounds, original.magnified_center);
                    next.width *= scale_x.abs();
                    next.height *= scale_y.abs();
                }
                next.corner_radii = flipped_corner_radii(
                    original.shape,
                    original.corner_radii,
                    scale_x < -f64::EPSILON,
                    scale_y < -f64::EPSILON,
                );
            }
            transform @ SelectionTransform::Rotate { angle } => {
                if !single {
                    next.source_center =
                        transform.map_point(&self.original_bounds, original.source_center);
                    next.magnified_center =
                        transform.map_point(&self.original_bounds, original.magnified_center);
                }
                next.rotation = normalize_rotation(original.rotation + angle);
            }
        }
        Some(next)
    }
}

fn sync_selection_magnifier_styles(
    document: &DocumentModel,
    magnifiers: &mut HashMap<ElementId, MagnifierData>,
    original_elements: &mut [SelectionRectState],
    preview_elements: &mut [SelectionRectState],
    no_arrows: bool,
    corner_edit: bool,
) -> bool {
    let single = original_elements.len() == 1 && no_arrows;
    let mut contour_changed = false;
    for (id, value) in magnifiers {
        let Ok(committed) = document.magnifier(*id) else {
            continue;
        };
        let style = MagnifierStyle::from_data(committed);
        let properties = MagnifierStyle::from_data(value).differences(style);
        let previous_factor = value.factor;
        style.apply(value);
        value.opacity = committed.opacity;
        // A group proxy is the union of source and lens, frozen at pointer down. Only a
        // single source proxy carries the shape/corner appearance used by its handles.
        if single {
            for element in original_elements
                .iter_mut()
                .map(|element| (element, false))
                .chain(preview_elements.iter_mut().map(|element| (element, true)))
            {
                let (element, preview) = element;
                if element.id != *id {
                    continue;
                }
                if properties & MAGNIFIER_STYLE_PROPERTY_SHAPE != 0 {
                    element.rect.highlight_shape = style.shape;
                }
                if properties
                    & (MAGNIFIER_STYLE_PROPERTY_SHAPE
                        | MAGNIFIER_STYLE_PROPERTY_CORNER_RADII
                        | MAGNIFIER_STYLE_PROPERTY_FACTOR)
                    != 0
                {
                    contour_changed = true;
                    let target_radii = if preview
                        && corner_edit
                        && properties
                            & (MAGNIFIER_STYLE_PROPERTY_SHAPE
                                | MAGNIFIER_STYLE_PROPERTY_CORNER_RADII)
                            == 0
                    {
                        element.rect.corner_radii.scaled(previous_factor)
                    } else {
                        style.corner_radii
                    };
                    element.rect.corner_radii =
                        snow_draw_engine_document::normalize_shape_corner_radii(
                            element.rect.highlight_shape,
                            element.rect.width * style.factor,
                            element.rect.height * style.factor,
                            target_radii,
                        )
                        .scaled(1.0 / style.factor);
                }
                element.rect.stroke = style.stroke;
                element.rect.opacity = committed.opacity;
            }
        }
    }
    contour_changed
}

fn rebase_magnifier_corner_input(state: &mut EditSelectionState, zoom: f64) {
    let SelectionEditMode::CornerRadius { corner } = state.mode else {
        return;
    };
    let Some((pointer, modifiers)) = state.last_magnifier_corner_input else {
        return;
    };
    let Some(original) = state.original_elements.first() else {
        return;
    };
    let Some(preview) = state.preview_elements.first() else {
        return;
    };
    let rect = original.rect;
    let target_radius = corner_radius(preview.rect.corner_radii, corner);
    let current = canvas_to_rect_local(
        rect.center,
        rect.rotation,
        Point::new(
            pointer.x + state.magnifier_corner_input_offset.x,
            pointer.y + state.magnifier_corner_input_offset.y,
        ),
    );
    let (direction, inset) =
        if rect.highlight_shape == snow_draw_engine_document::HighlightShape::Diamond {
            match diamond_corner_index(corner) {
                0 => (Point::new(0.0, 1.0), current.y + rect.height / 2.0),
                1 => (Point::new(-1.0, 0.0), rect.width / 2.0 - current.x),
                2 => (Point::new(0.0, -1.0), rect.height / 2.0 - current.y),
                _ => (Point::new(1.0, 0.0), current.x + rect.width / 2.0),
            }
        } else {
            let tip = rect_corner_point(rect.width, rect.height, corner);
            let inward_x = (tip.x - current.x) * corner.x_sign();
            let inward_y = (tip.y - current.y) * corner.y_sign();
            (
                Point::new(-corner.x_sign(), -corner.y_sign()),
                inward_x.min(inward_y),
            )
        };
    let tip = Point::new(
        current.x - direction.x * inset,
        current.y - direction.y * inset,
    );
    let extent = rect.width.max(rect.height);
    // Calibrate the existing handle mapping only when style edits rebase it.
    // Pointer moves then keep their normal constant-cost path and stay continuous.
    let mapped_point = |t: f64| {
        rect_local_to_canvas(
            rect.center,
            rect.rotation,
            Point::new(
                tip.x + direction.x * extent * t,
                tip.y + direction.y * extent * t,
            ),
        )
    };
    let mut low = 0.0;
    let mut high = 1.0;
    for _ in 0..48 {
        let middle = f64::midpoint(low, high);
        let candidate = preview_rect_for_corner_radius(
            &rect,
            corner,
            mapped_point(middle),
            modifiers.shift,
            zoom,
        );
        let radius = corner_radius(candidate.corner_radii, corner);
        if radius < target_radius || (target_radius == 0.0 && radius == 0.0) {
            low = middle;
        } else {
            high = middle;
        }
    }
    let mapped = mapped_point(f64::midpoint(low, high));
    state.magnifier_corner_input_offset = Point::new(mapped.x - pointer.x, mapped.y - pointer.y);
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
