use std::collections::{BTreeMap, BTreeSet};

use crate::arrow_geom::{
    Heading, center, compute_bounds_from_points, get_global_fixed_point, get_point_at_index_global,
    to_global_point, vector_to_heading,
};
use crate::arrow_hit_test::{
    distance_to_bindable_outline, get_bindables_over_point, get_hovered_bindable,
    is_bindable_background_opaque, is_bindable_inside_other_bindable, is_point_in_bindable,
};
use crate::{
    ArrowEndpointEdge, ArrowEndpointSelector, ArrowEngineEvent, ArrowPatch, ArrowState, BindMode,
    BindablePatch, BindableState, Bounds, ComputeEndpointDragInput, ElementId,
    EndpointBindingStrategy, EngineResult, FixedPointBinding, Point, SuggestedBinding,
    point_updates_to_pairs,
};

use crate::arrow_binding_geometry::heading_for_point_from_bindable;
pub use crate::arrow_binding_geometry::{
    OutlineMidPointMode, bind_point_to_outline, calculate_fixed_point_for_binding,
    calculate_fixed_point_for_elbow_binding, max_binding_distance, outline_mid_point_suggestion,
    project_fixed_point_onto_diagonal, update_bound_point,
};

#[path = "binding_core_decisions.rs"]
mod decisions;
#[path = "binding_core_patch.rs"]
mod patch;

pub use patch::compute_simple_binding_patch;

pub const BASE_BINDING_GAP: f64 = 5.0;
pub const BASE_BINDING_GAP_ELBOW: f64 = 5.0;
pub const BASE_ARROW_MIN_LENGTH: f64 = 10.0;

pub fn get_heading_for_elbow_snap(
    point: Point,
    other_point: Point,
    bindable: Option<&BindableState>,
    aabb: Option<Bounds>,
    origin_point: Option<Point>,
    zoom: Option<f64>,
) -> Heading {
    let other_point_heading = vector_to_heading(point, other_point);
    let Some(bindable) = bindable else {
        return other_point_heading;
    };
    let Some(aabb) = aabb else {
        return other_point_heading;
    };

    let distance = distance_to_bindable_outline(origin_point.unwrap_or(point), bindable);
    let bind_distance = max_binding_distance(zoom.unwrap_or(1.0));
    let resolved_distance = if distance > bind_distance {
        None
    } else {
        Some(distance)
    };

    if resolved_distance.is_none() || resolved_distance == Some(0.0) {
        let bindable_center = center(bindable.x, bindable.y, bindable.width, bindable.height);
        return vector_to_heading(bindable_center, point);
    }

    heading_for_point_from_bindable(point, bindable, aabb)
}
