use std::collections::BTreeMap;

use crate::ElementId;
use snow_draw_engine_core::arrow::{
    ArrowEndpointEdge, Arrowhead, BindMode, BindableShape, EngineContext, FixedSegment,
};

pub(crate) type Point = [f64; 2];
pub(crate) type Bounds = [f64; 4];
pub(crate) type PatchValue<T> = Option<Option<T>>;
pub(crate) type BindableLookupRecord = BTreeMap<ElementId, BindableState>;

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct FixedPointBinding {
    pub element_id: ElementId,
    pub fixed_point: Point,
    pub mode: BindMode,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct BindableState {
    pub id: ElementId,
    pub shape: BindableShape,
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
    pub angle: f64,
    pub stroke_width: f64,
    pub z_index: Option<f64>,
    pub background_opaque: Option<bool>,
    pub binding_enabled: Option<bool>,
    pub interior_hit_enabled: Option<bool>,
    pub visibility_bounds: Option<Bounds>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct ArrowState {
    pub id: ElementId,
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
    pub points: Vec<Point>,
    pub start_binding: Option<FixedPointBinding>,
    pub end_binding: Option<FixedPointBinding>,
    pub start_arrowhead: Option<Arrowhead>,
    pub end_arrowhead: Option<Arrowhead>,
    pub elbowed: bool,
    pub fixed_segments: Option<Vec<FixedSegment>>,
    pub start_is_special: Option<bool>,
    pub end_is_special: Option<bool>,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct ArrowPatch {
    pub x: Option<f64>,
    pub y: Option<f64>,
    pub width: Option<f64>,
    pub height: Option<f64>,
    pub points: Option<Vec<Point>>,
    pub start_binding: PatchValue<FixedPointBinding>,
    pub end_binding: PatchValue<FixedPointBinding>,
    pub fixed_segments: PatchValue<Vec<FixedSegment>>,
    pub start_is_special: PatchValue<bool>,
    pub end_is_special: PatchValue<bool>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct BindablePatch {
    pub id: ElementId,
    pub add_bound_arrow_id: Option<ElementId>,
    pub remove_bound_arrow_id: Option<ElementId>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct SuggestedBinding {
    pub bindable_id: Option<ElementId>,
    pub element: BindableState,
    pub mid_point: Option<Point>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum ArrowEngineEvent {
    ReorderArrow {
        arrow_id: ElementId,
        bindable_id: ElementId,
    },
    BindingBroken {
        arrow_id: ElementId,
        edge: ArrowEndpointEdge,
    },
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct EngineResult {
    pub arrow_patch: ArrowPatch,
    pub bindable_patches: Vec<BindablePatch>,
    pub suggested_binding: Option<SuggestedBinding>,
    pub events: Vec<ArrowEngineEvent>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct ValidationReport {
    pub valid: bool,
    pub violations: Vec<String>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct EndpointBindingStrategy {
    pub mode: Option<BindMode>,
    pub bindable_id: Option<ElementId>,
    pub element: Option<BindableState>,
    pub focus_point: Option<Point>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct PointUpdate {
    pub index: usize,
    pub point: Point,
}

pub(crate) type PointUpdates = Vec<PointUpdate>;

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct ComputeEndpointDragInput {
    pub arrow: ArrowState,
    pub dragged_points: PointUpdates,
    pub pointer: Point,
    pub bindables: Vec<BindableState>,
    pub context: EngineContext,
    pub options: Option<snow_draw_engine_core::arrow::ComputeEndpointDragOptions>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct RecomputeAfterBindableChangeInput {
    pub arrow: ArrowState,
    pub bindables: Vec<BindableState>,
    pub changed_bindable_ids: Option<Vec<ElementId>>,
    pub context: EngineContext,
    pub options: Option<snow_draw_engine_core::arrow::RecomputeAfterBindableChangeOptions>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct RecomputeElbowInput {
    pub arrow: ArrowState,
    pub bindables: Vec<BindableState>,
    pub context: EngineContext,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct ElbowUpdatePatch {
    pub points: Option<Vec<Point>>,
    pub fixed_segments: PatchValue<Vec<FixedSegment>>,
    pub start_binding: PatchValue<FixedPointBinding>,
    pub end_binding: PatchValue<FixedPointBinding>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct UpdateElbowArrowInput {
    pub arrow: ArrowState,
    pub updates: ElbowUpdatePatch,
    pub bindables: Vec<BindableState>,
    pub context: EngineContext,
    pub options: Option<snow_draw_engine_core::arrow::UpdateElbowArrowOptions>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct MoveFixedSegmentToPointResult {
    pub patch: ArrowPatch,
    pub active_segment_index: Option<usize>,
    pub active_segment_mid_point: Option<Point>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct FocusPointDescriptor {
    pub edge: ArrowEndpointEdge,
    pub point: Point,
    pub binding: FixedPointBinding,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct ListVisibleFocusPointsInput {
    pub arrow: ArrowState,
    pub bindables: Vec<BindableState>,
    pub context: snow_draw_engine_core::arrow::FocusPointContext,
    pub options: Option<snow_draw_engine_core::arrow::FocusPointOptions>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct ComputeFocusPointDragInput {
    pub arrow: ArrowState,
    pub dragged_edge: ArrowEndpointEdge,
    pub pointer: Point,
    pub bindables: Vec<BindableState>,
    pub context: EngineContext,
    pub options: Option<snow_draw_engine_core::arrow::ComputeFocusPointDragOptions>,
}

pub(crate) fn point_updates_to_pairs(input: &PointUpdates) -> Vec<(usize, Point)> {
    input
        .iter()
        .map(|update| (update.index, update.point))
        .collect()
}
