use crate::arrow::apply_arrow_patch;
use crate::{
    ArrowData, BindableElementState, ElementId, arrow_elbow_core as routing,
    arrow_engine as editing, arrow_focus_core as focus,
};
use crate::{
    ArrowEngineEvent, ArrowState, BindableState, ComputeEndpointDragInput,
    ComputeFocusPointDragInput, ElbowUpdatePatch, EngineResult, ListVisibleFocusPointsInput,
    PointUpdate, RecomputeAfterBindableChangeInput, UpdateElbowArrowInput,
};
use snow_draw_engine_core::{
    Point,
    arrow::{
        ArrowEndpointEdge, ArrowType, ComputeEndpointDragOptions, ComputeFocusPointDragOptions,
        EngineContext, FocusPointContext, UpdateElbowArrowOptions,
    },
};

#[derive(Clone, Debug, PartialEq)]
pub struct ArrowEditResult {
    pub arrow: ArrowData,
    pub reorder_targets: Vec<ElementId>,
    pub suggested_binding: Option<ArrowSuggestedBinding>,
}

/// Bindable currently suggested by an endpoint/focus drag, for hover feedback.
#[derive(Clone, Debug, PartialEq)]
pub struct ArrowSuggestedBinding {
    pub bindable_id: ElementId,
    /// Side midpoint the drag would snap to, when within its snap threshold.
    pub mid_point: Option<Point<f64>>,
    /// Remaining side midpoints shown as proximity hints.
    pub near_mid_points: Vec<Point<f64>>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ArrowEndpointDragOptions {
    pub alt_key: bool,
    pub finalize: bool,
    pub angle_locked: bool,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ArrowFocusPointState {
    pub edge: ArrowEndpointEdge,
    pub point: Point<f64>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ArrowFocusDragOptions {
    pub switch_to_inside_binding: bool,
    pub grid_size: Option<f64>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ArrowSegmentDragResult {
    pub arrow: ArrowData,
    pub next_segment_index: usize,
}

pub fn compute_arrow_endpoint_drag(
    arrow_id: ElementId,
    arrow: &ArrowData,
    edge: ArrowEndpointEdge,
    canvas_point: Point<f64>,
    bindables: &[BindableElementState],
    context: EngineContext,
    options: ArrowEndpointDragOptions,
) -> ArrowEditResult {
    let local_point = PointUpdate {
        index: arrow_endpoint_index(arrow.points.len(), edge),
        point: [canvas_point.x - arrow.x, canvas_point.y - arrow.y],
    };
    let input = ComputeEndpointDragInput {
        arrow: arrow_engine_state(arrow_id, arrow),
        dragged_points: vec![local_point],
        pointer: [canvas_point.x, canvas_point.y],
        bindables: bindable_states_from_elements(bindables),
        context,
        options: Some(ComputeEndpointDragOptions {
            alt_key: options.alt_key.then_some(true),
            finalize: options.finalize.then_some(true),
            angle_locked: options.angle_locked.then_some(true),
            ..ComputeEndpointDragOptions::default()
        }),
    };
    let result = if options.finalize {
        editing::finalize_endpoint_drag(&input)
    } else {
        editing::compute_endpoint_drag(&input)
    };

    arrow_edit_result_from_engine_result(arrow, bindables, result)
}

pub fn compute_arrow_focus_drag(
    arrow_id: ElementId,
    arrow: &ArrowData,
    edge: ArrowEndpointEdge,
    canvas_point: Point<f64>,
    bindables: &[BindableElementState],
    context: EngineContext,
    options: ArrowFocusDragOptions,
) -> ArrowEditResult {
    let result = editing::compute_focus_drag(&ComputeFocusPointDragInput {
        arrow: arrow_engine_state(arrow_id, arrow),
        dragged_edge: edge,
        pointer: [canvas_point.x, canvas_point.y],
        bindables: bindable_states_from_elements(bindables),
        context,
        options: Some(ComputeFocusPointDragOptions {
            switch_to_inside_binding: Some(options.switch_to_inside_binding),
            grid_size: options.grid_size,
        }),
    });

    arrow_edit_result_from_engine_result(arrow, bindables, result)
}

pub fn recompute_arrow_after_bindable_change(
    arrow_id: ElementId,
    arrow: &ArrowData,
    bindables: &[BindableElementState],
    changed_bindable_ids: &[ElementId],
    context: EngineContext,
) -> ArrowEditResult {
    let changed_bindable_ids =
        (!changed_bindable_ids.is_empty()).then(|| changed_bindable_ids.to_vec());
    let result = editing::recompute_after_bindable_change(&RecomputeAfterBindableChangeInput {
        arrow: arrow_engine_state(arrow_id, arrow),
        bindables: bindable_states_from_elements(bindables),
        changed_bindable_ids,
        context,
        options: None,
    });

    arrow_edit_result_from_engine_result(arrow, bindables, result)
}

pub fn visible_arrow_focus_points(
    arrow_id: ElementId,
    arrow: &ArrowData,
    bindables: &[BindableElementState],
    zoom: f64,
) -> Vec<ArrowFocusPointState> {
    focus::list_visible_focus_points(&ListVisibleFocusPointsInput {
        arrow: arrow_engine_state(arrow_id, arrow),
        bindables: bindable_states_from_elements(bindables),
        context: FocusPointContext {
            zoom,
            is_binding_enabled: true,
        },
        options: None,
    })
    .into_iter()
    .map(|focus| ArrowFocusPointState {
        edge: focus.edge,
        point: Point::new(focus.point[0], focus.point[1]),
    })
    .collect()
}

pub fn drag_elbow_arrow_segment(
    arrow_id: ElementId,
    arrow: &ArrowData,
    segment_index: usize,
    canvas_point: Point<f64>,
    bindables: &[BindableElementState],
    context: EngineContext,
    finalize: bool,
) -> ArrowSegmentDragResult {
    let mut source_arrow = arrow.clone();
    source_arrow.arrow_type = ArrowType::Elbow;
    let arrow_state = arrow_engine_state(arrow_id, &source_arrow);
    let fixed_segment = routing::move_fixed_segment_to_point(
        &arrow_state,
        segment_index,
        [canvas_point.x, canvas_point.y],
    );
    let fixed_segment_offset = fixed_segment
        .patch
        .fixed_segments
        .as_ref()
        .and_then(|segments| segments.as_ref())
        .map(|segments| {
            segments
                .iter()
                .filter(|segment| segment.index < segment_index)
                .count()
        })
        .unwrap_or(0);
    let patch = routing::update_elbow_arrow_patch(UpdateElbowArrowInput {
        arrow: arrow_state,
        updates: ElbowUpdatePatch {
            fixed_segments: fixed_segment.patch.fixed_segments,
            ..ElbowUpdatePatch::default()
        },
        bindables: bindable_states_from_elements(bindables),
        context,
        options: Some(UpdateElbowArrowOptions {
            is_dragging: Some(!finalize),
            validate_invariants: Some(finalize),
            midpoint_snapping_enabled: None,
        }),
    });
    let preview_arrow = apply_arrow_patch(&source_arrow, &patch);
    let next_segment_index = preview_arrow
        .fixed_segments
        .as_ref()
        .and_then(|segments| segments.get(fixed_segment_offset))
        .map(|segment| segment.index)
        .unwrap_or(segment_index);

    ArrowSegmentDragResult {
        arrow: preview_arrow,
        next_segment_index,
    }
}

fn bindable_states_from_elements(bindables: &[BindableElementState]) -> Vec<BindableState> {
    bindables
        .iter()
        .map(BindableElementState::arrow_bindable_state)
        .collect()
}

fn arrow_engine_state(arrow_id: ElementId, arrow: &ArrowData) -> ArrowState {
    ArrowState::from_arrow_data(arrow_id, arrow)
}

fn arrow_edit_result_from_engine_result(
    arrow: &ArrowData,
    bindables: &[BindableElementState],
    result: EngineResult,
) -> ArrowEditResult {
    ArrowEditResult {
        arrow: apply_arrow_patch(arrow, &result.arrow_patch),
        reorder_targets: reorder_targets_from_events(bindables, &result.events),
        suggested_binding: result
            .suggested_binding
            .map(|suggested| ArrowSuggestedBinding {
                bindable_id: suggested.bindable_id.unwrap_or(suggested.element.id),
                mid_point: suggested
                    .mid_point
                    .map(|point| Point::new(point[0], point[1])),
                near_mid_points: suggested
                    .near_mid_points
                    .into_iter()
                    .map(|point| Point::new(point[0], point[1]))
                    .collect(),
            }),
    }
}

fn reorder_targets_from_events(
    bindables: &[BindableElementState],
    events: &[ArrowEngineEvent],
) -> Vec<ElementId> {
    let mut targets = Vec::new();
    for event in events {
        let ArrowEngineEvent::ReorderArrow { bindable_id, .. } = event else {
            continue;
        };
        let Some(target) = bindables
            .iter()
            .find(|bindable| bindable.matches_arrow_bindable_id(*bindable_id))
            .map(|bindable| bindable.id())
        else {
            continue;
        };
        if !targets.contains(&target) {
            targets.push(target);
        }
    }
    targets
}

fn arrow_endpoint_index(point_count: usize, edge: ArrowEndpointEdge) -> usize {
    match edge {
        ArrowEndpointEdge::Start => 0,
        ArrowEndpointEdge::End => point_count.saturating_sub(1),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{ArrowEndpointBinding, FillStyle, RectangleData, arrow_binding_core as binding};
    use snow_draw_engine_core::{
        ColorRgba8, CornerRadii,
        arrow::{ArrowType, StrokeStyle},
    };

    fn test_rectangle(center: Point<f64>, width: f64, height: f64) -> RectangleData {
        RectangleData {
            rectangle_kind: crate::RectangleElementKind::Rectangle,
            highlight_shape: crate::HighlightShape::Rectangle,
            center,
            width,
            height,
            rotation: 0.0,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 2.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        }
    }

    fn straight_arrow(from: Point<f64>, to: Point<f64>) -> ArrowData {
        ArrowData::from_global_points(
            &[from, to],
            ColorRgba8::default(),
            2.0,
            StrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )
        .expect("straight arrow fixture")
    }

    fn drag_end(
        arrow: &ArrowData,
        target: Point<f64>,
        bindables: &[BindableElementState],
    ) -> ArrowEditResult {
        compute_arrow_endpoint_drag(
            ElementId::default(),
            arrow,
            ArrowEndpointEdge::End,
            target,
            bindables,
            EngineContext {
                zoom: 1.0,
                is_binding_enabled: true,
                bind_mode: snow_draw_engine_core::arrow::BindMode::Orbit,
                max_coordinate: 1e6,
            },
            ArrowEndpointDragOptions::default(),
        )
    }

    // A rectangle spanning (-50..50, -50..50) with a 2px stroke has a binding
    // gap of 6, so an orbit-bound tip sits on the -56..56 offset outline.
    fn centered_bindable() -> Vec<BindableElementState> {
        vec![BindableElementState::rectangle_proxy(
            ElementId::default(),
            test_rectangle(Point::new(0.0, 0.0), 100.0, 100.0),
            0,
            false,
        )]
    }

    #[test]
    fn linear_endpoint_attaches_on_the_edge_facing_the_other_end() {
        let bindables = centered_bindable();
        let arrow = straight_arrow(Point::new(-150.0, 0.0), Point::new(-100.0, 0.0));

        // Pointer hovers just outside the RIGHT edge while the arrow comes
        // from the left: the tip must attach to the left edge with the gap,
        // never snap across the shape to the pointer-side edge.
        let result = drag_end(&arrow, Point::new(55.0, 0.0), &bindables);
        let binding = result.arrow.end_binding.as_ref().expect("orbit binding");
        assert_eq!(binding.mode, snow_draw_engine_core::arrow::BindMode::Orbit);

        let tip = result.arrow.end();
        assert!(
            (tip.x - (-56.0)).abs() < 0.5,
            "tip should sit on the left edge plus gap, got {tip:?}"
        );
        assert!(
            tip.y.abs() < 0.5,
            "tip should stay near the arrow axis, got {tip:?}"
        );
    }

    #[test]
    fn linear_endpoint_snaps_anchor_to_nearby_midpoint_only() {
        let bindables = centered_bindable();
        let arrow = straight_arrow(Point::new(-150.0, 0.0), Point::new(-100.0, 0.0));

        // Within the midpoint snap threshold of the right-edge midpoint the
        // binding anchor locks to that midpoint and the suggestion reports it.
        let near_midpoint = drag_end(&arrow, Point::new(53.0, 3.0), &bindables);
        let binding = near_midpoint.arrow.end_binding.as_ref().expect("binding");
        assert!((binding.fixed_point[0] - 1.0).abs() < 0.02);
        assert!((binding.fixed_point[1] - 0.5).abs() < 0.02);
        let suggested = near_midpoint.suggested_binding.expect("suggested binding");
        assert!(suggested.mid_point.is_some());

        // Far from every midpoint the anchor must not quantize: it follows the
        // diagonal projection of the drag ray instead.
        let far_from_midpoint = drag_end(&arrow, Point::new(55.0, 35.0), &bindables);
        let binding = far_from_midpoint
            .arrow
            .end_binding
            .as_ref()
            .expect("binding");
        assert!(
            (binding.fixed_point[0] - 1.0).abs() > 0.1
                || (binding.fixed_point[1] - 0.5).abs() > 0.1,
            "anchor should not snap to the right midpoint, got {:?}",
            binding.fixed_point
        );
        assert!(
            far_from_midpoint
                .suggested_binding
                .is_some_and(|s| s.mid_point.is_none())
        );

        // The tip still slides along the edge facing the other end.
        let tip = far_from_midpoint.arrow.end();
        assert!(
            (tip.x - (-56.0)).abs() < 0.5,
            "tip stays on the left edge, got {tip:?}"
        );
        assert!(
            tip.y > 5.0 && tip.y < 30.0,
            "tip tracks the pointer height, got {tip:?}"
        );
    }

    #[test]
    fn linear_endpoint_inside_bindable_follows_the_pointer() {
        let bindables = centered_bindable();
        let arrow = straight_arrow(Point::new(-150.0, 0.0), Point::new(-100.0, 0.0));

        // Penetrating the element switches to an inside binding whose tip
        // stays exactly at the pointer instead of crossing to an edge.
        let result = drag_end(&arrow, Point::new(30.0, 10.0), &bindables);
        let binding = result.arrow.end_binding.as_ref().expect("inside binding");
        assert_eq!(binding.mode, snow_draw_engine_core::arrow::BindMode::Inside);

        let tip = result.arrow.end();
        assert!((tip.x - 30.0).abs() < 1e-6 && (tip.y - 10.0).abs() < 1e-6);
    }

    #[test]
    fn new_arrow_endpoint_inside_target_binds_inside() {
        let bindables = centered_bindable();
        let arrow = straight_arrow(Point::new(-150.0, 0.0), Point::new(-100.0, 0.0));

        // Drawing a new arrow into a shape keeps the tip at the pointer with
        // an inside binding, matching the reference behavior.
        let result = compute_arrow_endpoint_drag(
            ElementId::default(),
            &arrow,
            ArrowEndpointEdge::End,
            Point::new(30.0, 10.0),
            &bindables,
            EngineContext {
                zoom: 1.0,
                is_binding_enabled: true,
                bind_mode: snow_draw_engine_core::arrow::BindMode::Orbit,
                max_coordinate: 1e6,
            },
            ArrowEndpointDragOptions::default(),
        );
        let binding = result.arrow.end_binding.as_ref().expect("inside binding");
        assert_eq!(binding.mode, snow_draw_engine_core::arrow::BindMode::Inside);

        let tip = result.arrow.end();
        assert!((tip.x - 30.0).abs() < 1e-6 && (tip.y - 10.0).abs() < 1e-6);
    }

    fn elbow_arrow(from: Point<f64>, to: Point<f64>) -> ArrowData {
        ArrowData::from_global_points(
            &[from, to],
            ColorRgba8::default(),
            2.0,
            StrokeStyle::Solid,
            ArrowType::Elbow,
            None,
            None,
        )
        .expect("elbow arrow fixture")
    }

    #[test]
    fn elbow_endpoint_uses_the_same_midpoint_suggestion_as_linear() {
        let bindables = centered_bindable();
        let arrow = elbow_arrow(Point::new(-150.0, 0.0), Point::new(-100.0, 0.0));

        let near_midpoint = drag_end(&arrow, Point::new(53.0, 3.0), &bindables);
        let suggested = near_midpoint.suggested_binding.expect("suggested binding");
        assert_eq!(suggested.bindable_id, ElementId::default());
        let mid_point = suggested.mid_point.expect("snapped midpoint");
        assert!((mid_point.x - 50.0).abs() < 0.1 && mid_point.y.abs() < 0.1);

        let far_from_midpoint = drag_end(&arrow, Point::new(55.0, 35.0), &bindables);
        assert!(
            far_from_midpoint
                .suggested_binding
                .is_some_and(|s| s.mid_point.is_none())
        );
    }

    #[test]
    fn elbow_endpoint_drag_reorders_behind_the_bindable() {
        let bindables = centered_bindable();
        let arrow = elbow_arrow(Point::new(-150.0, 0.0), Point::new(-100.0, 0.0));
        let result = drag_end(&arrow, Point::new(53.0, 3.0), &bindables);
        assert_eq!(result.reorder_targets, vec![ElementId::default()]);
    }

    fn drag_end_with_options(
        arrow: &ArrowData,
        target: Point<f64>,
        bindables: &[BindableElementState],
        options: ArrowEndpointDragOptions,
    ) -> ArrowEditResult {
        compute_arrow_endpoint_drag(
            ElementId::default(),
            arrow,
            ArrowEndpointEdge::End,
            target,
            bindables,
            EngineContext {
                zoom: 1.0,
                is_binding_enabled: true,
                bind_mode: snow_draw_engine_core::arrow::BindMode::Orbit,
                max_coordinate: 1e6,
            },
            options,
        )
    }

    #[test]
    fn angle_locked_drag_hides_the_midpoint_suggestion() {
        let bindables = centered_bindable();
        let straight = straight_arrow(Point::new(-150.0, 0.0), Point::new(-100.0, 0.0));
        let elbow = elbow_arrow(Point::new(-150.0, 0.0), Point::new(-100.0, 0.0));

        for arrow in [&straight, &elbow] {
            let result = drag_end_with_options(
                arrow,
                Point::new(53.0, 3.0),
                &bindables,
                ArrowEndpointDragOptions {
                    angle_locked: true,
                    ..ArrowEndpointDragOptions::default()
                },
            );
            // The bindable is still suggested for the outline highlight, but
            // the midpoint dot is suppressed while angle-locked.
            assert!(
                result
                    .suggested_binding
                    .is_some_and(|s| s.mid_point.is_none()),
                "midpoint suggestion must be hidden while angle-locked"
            );
        }
    }

    #[test]
    fn angle_locked_drag_keeps_opposite_midpoint_snapping() {
        let bindables = centered_bindable();
        let mut arrow = straight_arrow(Point::new(53.0, 3.0), Point::new(-200.0, 0.0));
        arrow.set_start_element_binding(Some(ArrowEndpointBinding {
            element_id: ElementId::default(),
            fixed_point: binding::calculate_fixed_point_for_binding(
                &bindable_states_from_elements(&bindables)[0],
                [53.0, 3.0],
            ),
            mode: snow_draw_engine_core::arrow::BindMode::Orbit,
        }));

        // Shift-dragging the free end away from the shape re-projects the
        // bound start anchor; the opposite end keeps midpoint snapping, so the
        // start tip near the right-edge midpoint locks onto it.
        let result = drag_end_with_options(
            &arrow,
            Point::new(-150.0, 40.0),
            &bindables,
            ArrowEndpointDragOptions {
                angle_locked: true,
                ..ArrowEndpointDragOptions::default()
            },
        );
        let start_binding = result.arrow.start_binding.as_ref().expect("start binding");
        assert_eq!(
            start_binding.mode,
            snow_draw_engine_core::arrow::BindMode::Orbit
        );
        assert!(
            (start_binding.fixed_point[0] - 1.0).abs() < 0.02
                && (start_binding.fixed_point[1] - 0.5).abs() < 0.02,
            "opposite anchor should snap to the right midpoint, got {:?}",
            start_binding.fixed_point
        );
    }

    #[test]
    fn new_arrow_shift_drag_preserves_opposite_inside_binding() {
        let bindables = centered_bindable();
        let mut arrow = straight_arrow(Point::new(30.0, 10.0), Point::new(-200.0, 0.0));
        arrow.set_start_element_binding(Some(ArrowEndpointBinding {
            element_id: ElementId::default(),
            fixed_point: binding::calculate_fixed_point_for_binding(
                &bindable_states_from_elements(&bindables)[0],
                [30.0, 10.0],
            ),
            mode: snow_draw_engine_core::arrow::BindMode::Inside,
        }));

        // Drawing with shift out of the shape the arrow started in must keep
        // the pressed end's inside binding (Excalidraw's arrowStartIsInside).
        let result = drag_end_with_options(
            &arrow,
            Point::new(-150.0, 40.0),
            &bindables,
            ArrowEndpointDragOptions {
                angle_locked: true,
                ..ArrowEndpointDragOptions::default()
            },
        );
        let start_binding = result.arrow.start_binding.as_ref().expect("start binding");
        assert_eq!(
            start_binding.mode,
            snow_draw_engine_core::arrow::BindMode::Inside,
            "opposite inside binding must survive a shift-dragged new arrow"
        );
    }

    #[test]
    fn new_arrow_pressed_in_binding_gap_binds_orbit() {
        let bindables = centered_bindable();
        let arrow = straight_arrow(Point::new(53.0, 0.0), Point::new(-100.0, 0.0));

        // Pressing the start of a new arrow in the binding gap outside the
        // shape binds orbit (anchored to the outline) rather than inside,
        // matching the reference's non-complex strategy.
        let result = compute_arrow_endpoint_drag(
            ElementId::default(),
            &arrow,
            ArrowEndpointEdge::Start,
            Point::new(53.0, 0.0),
            &bindables,
            EngineContext {
                zoom: 1.0,
                is_binding_enabled: true,
                bind_mode: snow_draw_engine_core::arrow::BindMode::Orbit,
                max_coordinate: 1e6,
            },
            ArrowEndpointDragOptions::default(),
        );
        let binding = result.arrow.start_binding.as_ref().expect("orbit binding");
        assert_eq!(
            binding.mode,
            snow_draw_engine_core::arrow::BindMode::Orbit,
            "gap press must orbit-bind the pressed end"
        );
    }

    #[test]
    fn angle_locked_elbow_drag_skips_midpoint_anchor() {
        let bindables = centered_bindable();
        let arrow = elbow_arrow(Point::new(-150.0, 0.0), Point::new(-100.0, 0.0));

        // The elbow anchor at the right midpoint sits one binding gap outside
        // the edge, centered vertically (ratio y ~0.5 despite the pointer
        // being 3px off-axis).
        let snapped = drag_end(&arrow, Point::new(53.0, 3.0), &bindables);
        let snapped_binding = snapped.arrow.end_binding.as_ref().expect("binding");
        assert!(
            (snapped_binding.fixed_point[1] - 0.5).abs() < 0.01,
            "elbow anchor should snap to the right midpoint, got {:?}",
            snapped_binding.fixed_point
        );

        // Shift disables the elbow midpoint magnet: the anchor keeps tracking
        // the pointer height instead of centering on the midpoint.
        let locked = drag_end_with_options(
            &arrow,
            Point::new(53.0, 3.0),
            &bindables,
            ArrowEndpointDragOptions {
                angle_locked: true,
                ..ArrowEndpointDragOptions::default()
            },
        );
        let locked_binding = locked.arrow.end_binding.as_ref().expect("binding");
        assert!(
            (locked_binding.fixed_point[1] - 0.5).abs() > 0.015,
            "angle-locked elbow anchor must not snap to the midpoint, got {:?}",
            locked_binding.fixed_point
        );
    }

    #[test]
    fn midpoint_suggestion_reports_proximity_dots() {
        let bindables = centered_bindable();

        // Linear: right at the midpoint it becomes the highlighted snap dot
        // and no proximity hints remain.
        let linear = straight_arrow(Point::new(-150.0, 0.0), Point::new(-100.0, 0.0));
        let snapped = drag_end(&linear, Point::new(53.0, 3.0), &bindables);
        let suggested = snapped.suggested_binding.expect("suggested binding");
        assert!(suggested.mid_point.is_some());
        assert!(suggested.near_mid_points.is_empty());

        // Elbow: all four side midpoints are hinted, the closest highlighted.
        let elbow = elbow_arrow(Point::new(-150.0, 0.0), Point::new(-100.0, 0.0));
        let hovering = drag_end(&elbow, Point::new(53.0, 3.0), &bindables);
        let suggested = hovering.suggested_binding.expect("suggested binding");
        assert!(suggested.mid_point.is_some());
        assert_eq!(suggested.near_mid_points.len(), 3);

        // Elbow: with the cursor inside the bindable the dots remain visible
        // (orbit anchors still target the outline midpoints).
        let inside = drag_end(&elbow, Point::new(10.0, 4.0), &bindables);
        let suggested = inside.suggested_binding.expect("suggested binding");
        assert_eq!(suggested.near_mid_points.len(), 4);

        // Linear: with the cursor inside the bindable the dots are hidden.
        let inside = drag_end(&linear, Point::new(10.0, 4.0), &bindables);
        let suggested = inside.suggested_binding.expect("suggested binding");
        assert!(suggested.mid_point.is_none());
        assert!(suggested.near_mid_points.is_empty());
    }
}
