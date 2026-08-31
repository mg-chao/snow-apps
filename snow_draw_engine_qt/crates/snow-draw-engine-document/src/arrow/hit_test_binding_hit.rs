use super::shape_geometry::get_diamond_vertices;
use super::*;

fn resolve_binding_hit_threshold(bindable: &BindableState, tolerance: f64) -> f64 {
    if is_bindable_interior_hit_enabled(bindable) {
        tolerance
    } else {
        1.0_f64.max(tolerance)
    }
}

pub fn is_point_near_bindable_for_binding_hit(
    point: Point,
    bindable: &BindableState,
    tolerance: f64,
) -> bool {
    if !is_bindable_binding_enabled(bindable) {
        return false;
    }
    if !is_bindable_visible_at_point(point, bindable) {
        return false;
    }

    let threshold = resolve_binding_hit_threshold(bindable, tolerance);
    if !is_bindable_interior_hit_enabled(bindable) {
        return distance_to_bindable_outline(point, bindable) <= threshold;
    }

    is_point_in_bindable(point, bindable)
        || distance_to_bindable_outline(point, bindable) <= threshold
}

pub fn get_hovered_bindable(
    point: Point,
    z_ordered_bindables: &[BindableState],
    tolerance: f64,
) -> Option<BindableState> {
    let bindables = sort_bindables_by_z_index(z_ordered_bindables);
    let mut candidates = Vec::new();
    for bindable in bindables.iter().rev() {
        if !is_point_near_bindable_for_binding_hit(point, bindable, tolerance) {
            continue;
        }
        candidates.push(bindable.clone());
        if is_bindable_background_opaque(bindable) {
            break;
        }
    }

    if candidates.is_empty() {
        return None;
    }
    if candidates.len() == 1 {
        return candidates.pop();
    }

    candidates.sort_by(|a, b| {
        let left = b.width.powi(2) + b.height.powi(2);
        let right = a.width.powi(2) + a.height.powi(2);
        left.partial_cmp(&right)
            .unwrap_or(std::cmp::Ordering::Equal)
    });
    candidates.pop()
}

pub fn get_bindables_over_point(
    point: Point,
    z_ordered_bindables: &[BindableState],
    tolerance: f64,
) -> Vec<BindableState> {
    sort_bindables_by_z_index(z_ordered_bindables)
        .into_iter()
        .filter(|bindable| is_point_near_bindable_for_binding_hit(point, bindable, tolerance))
        .collect()
}

fn sample_outline_points(bindable: &BindableState, offset: f64) -> Vec<Point> {
    let shape = bindable.shape;
    let bindable_center = center(bindable.x, bindable.y, bindable.width, bindable.height);

    if shape == BindableShape::Diamond {
        let [top, right, bottom, left] = get_diamond_vertices(bindable);
        return vec![
            rotate_point([top[0], top[1] - offset], bindable_center, bindable.angle),
            rotate_point(
                [right[0] + offset, right[1]],
                bindable_center,
                bindable.angle,
            ),
            rotate_point(
                [bottom[0], bottom[1] + offset],
                bindable_center,
                bindable.angle,
            ),
            rotate_point([left[0] - offset, left[1]], bindable_center, bindable.angle),
        ];
    }

    if shape == BindableShape::Ellipse {
        let cx = bindable.x + bindable.width / 2.0;
        let cy = bindable.y + bindable.height / 2.0;
        let rx = bindable.width / 2.0;
        let ry = bindable.height / 2.0;
        return vec![
            rotate_point([cx, cy - ry - offset], bindable_center, bindable.angle),
            rotate_point([cx + rx + offset, cy], bindable_center, bindable.angle),
            rotate_point([cx, cy + ry + offset], bindable_center, bindable.angle),
            rotate_point([cx - rx - offset, cy], bindable_center, bindable.angle),
        ];
    }

    vec![
        rotate_point(
            [bindable.x - offset, bindable.y - offset],
            bindable_center,
            bindable.angle,
        ),
        rotate_point(
            [bindable.x + bindable.width + offset, bindable.y - offset],
            bindable_center,
            bindable.angle,
        ),
        rotate_point(
            [
                bindable.x + bindable.width + offset,
                bindable.y + bindable.height + offset,
            ],
            bindable_center,
            bindable.angle,
        ),
        rotate_point(
            [bindable.x - offset, bindable.y + bindable.height + offset],
            bindable_center,
            bindable.angle,
        ),
    ]
}

pub fn is_bindable_inside_other_bindable(
    inner_bindable: &BindableState,
    outer_bindable: &BindableState,
) -> bool {
    let offset = -inner_bindable.width.max(inner_bindable.height) / 20.0;
    sample_outline_points(inner_bindable, offset)
        .into_iter()
        .all(|sample| is_point_in_bindable(sample, outer_bindable))
}
