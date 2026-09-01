use std::collections::BTreeMap;

use crate::arrow_binding_core::{BASE_ARROW_MIN_LENGTH, BASE_BINDING_GAP, BASE_BINDING_GAP_ELBOW};
use crate::arrow_geom::{
    Heading, center, clamp, distance, distance_sq, get_global_fixed_point,
    get_point_at_index_global, heading_from_bindable, inflate_bounds, normalize_fixed_point,
    points_equal, rotate_point, rotated_bindable_bounds, to_local_point, unrotate_point,
};
use crate::arrow_hit_test::{
    distance_to_bindable_outline, get_binding_side_mid_point, is_point_in_bindable,
};
use crate::{
    ArrowEndpointEdge, ArrowEndpointSelector, ArrowState, BindMode, BindableLookupRecord,
    BindableState, Bounds, FixedPointBinding, Point,
    normalize_arrow_endpoint_edge,
};

#[path = "binding_geometry_direction.rs"]
mod direction;
#[path = "binding_geometry_outline.rs"]
mod outline;
#[path = "binding_geometry_projection.rs"]
mod projection;
#[path = "binding_geometry_snap_points.rs"]
mod snap_points;

pub(crate) use direction::heading_for_point_from_bindable;
pub use projection::{
    bind_point_to_outline, calculate_fixed_point_for_binding,
    calculate_fixed_point_for_elbow_binding, update_bound_point,
};
pub use snap_points::{get_snap_outline_mid_point, project_fixed_point_onto_diagonal};

pub fn get_binding_gap(bind_target: &BindableState, elbowed: bool) -> f64 {
    (if elbowed {
        BASE_BINDING_GAP_ELBOW
    } else {
        BASE_BINDING_GAP
    }) + bind_target.stroke_width / 2.0
}

pub fn max_binding_distance(zoom: f64) -> f64 {
    let base_distance = BASE_BINDING_GAP.max(15.0);
    let safe_zoom = if zoom < 1.0 { zoom } else { 1.0 };
    clamp(
        base_distance / (safe_zoom * 1.5),
        base_distance,
        base_distance * 2.0,
    )
}
