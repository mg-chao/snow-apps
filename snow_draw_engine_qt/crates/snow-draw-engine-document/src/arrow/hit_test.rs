use std::f64::consts::PI;

use crate::arrow_geom::{
    center, clamp, distance, normalize_fixed_point, rotate_point, unrotate_point,
};
use crate::{
    BindableState, Bounds,
    BindableShape, Point,
};

#[path = "hit_test_binding_hit.rs"]
mod binding_hit;
#[path = "hit_test_shape_geometry.rs"]
mod shape_geometry;
#[path = "hit_test_side_midpoint.rs"]
mod side_midpoint;
#[path = "hit_test_visibility.rs"]
mod visibility;

pub(crate) use binding_hit::{
    get_bindables_over_point, get_hovered_bindable, is_bindable_inside_other_bindable,
};
pub(crate) use shape_geometry::{distance_to_bindable_outline, is_point_in_bindable};
pub(crate) use side_midpoint::get_binding_side_mid_point;
pub(crate) use visibility::{
    is_bindable_background_opaque, is_bindable_binding_enabled, is_bindable_interior_hit_enabled,
    is_bindable_visible_at_point, sort_bindables_by_z_index,
};
