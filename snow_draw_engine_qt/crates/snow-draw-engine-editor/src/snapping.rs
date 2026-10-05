use super::*;
use snow_draw_engine_core::arrow::{BindMode, EngineContext};

impl Editor {
    pub(crate) fn snap_point_to_external_guides(
        &self,
        source: Point<f64>,
        mut snapped: Point<f64>,
        mut guides: Vec<SnapGuide>,
    ) -> (Point<f64>, Vec<SnapGuide>) {
        for (axis, lines, coordinate) in [
            (
                SnapGuideAxis::Vertical,
                &self.view.snap_guide_targets.vertical_xs[..],
                source.x,
            ),
            (
                SnapGuideAxis::Horizontal,
                &self.view.snap_guide_targets.horizontal_ys[..],
                source.y,
            ),
        ] {
            let nearest = lines
                .iter()
                .flatten()
                .map(|line| *line - coordinate)
                .filter(|offset| offset.abs() <= self.zoom_adjusted_snap_distance())
                .min_by(|left, right| left.abs().total_cmp(&right.abs()));
            if let Some(offset) = nearest {
                let object_present = guides.iter().any(|guide| guide.axis == axis);
                let object_offset = if axis == SnapGuideAxis::Vertical {
                    snapped.x - source.x
                } else {
                    snapped.y - source.y
                };
                if !object_present || offset.abs() <= object_offset.abs() {
                    guides.retain(|guide| guide.axis != axis);
                    if axis == SnapGuideAxis::Vertical {
                        snapped.x = coordinate + offset;
                    } else {
                        snapped.y = coordinate + offset;
                    }
                }
            }
        }
        (snapped, guides)
    }

    pub(crate) fn bindable_elements(
        &self,
        document: &DocumentModel,
        overrides: &[SelectionRectState],
    ) -> Vec<BindableElementState> {
        let mut overrides = overrides
            .iter()
            .map(|element| (element.id, element.rect))
            .collect::<Vec<_>>();
        if let Some(id) = self.active_text_draft_existing_id()
            && !overrides.iter().any(|(override_id, _)| *override_id == id)
            && let Some(rect) = self.active_text_draft_rect_for_id(id)
        {
            overrides.push((id, rect));
        }
        document.bindable_element_states_with_overrides(&overrides)
    }

    pub(crate) fn snap_linear_arrow_control_point(
        &self,
        point: Point<f64>,
        modifiers: Modifiers,
    ) -> Point<f64> {
        if self.effective_snapping_mode(modifiers) == SnappingMode::Grid {
            GRID_SNAP_SERVICE.snap_point(point, self.config.grid.size)
        } else {
            point
        }
    }

    pub(crate) fn arrow_engine_context(&self, modifiers: Modifiers) -> EngineContext {
        EngineContext {
            zoom: self.camera().zoom,
            is_binding_enabled: !modifiers.ctrl,
            bind_mode: if modifiers.alt {
                BindMode::Inside
            } else {
                BindMode::Orbit
            },
            max_coordinate: DEFAULT_ARROW_MAX_COORDINATE,
        }
    }

    pub(crate) fn append_arrow_reorder_targets(
        &self,
        transaction: &mut Transaction,
        document: &DocumentModel,
        arrow_id: ElementId,
        reorder_targets: &[ElementId],
    ) {
        let positions = document
            .paint_order()
            .iter()
            .enumerate()
            .map(|(index, id)| (*id, index))
            .collect::<std::collections::HashMap<_, _>>();
        let Some(arrow_index) = positions.get(&arrow_id).copied() else {
            return;
        };

        let mut target_index = arrow_index;
        for target in reorder_targets {
            if let Some(bindable_index) = positions.get(target).copied() {
                target_index = target_index.max(bindable_index.saturating_add(1));
            }
        }
        if target_index == arrow_index {
            return;
        }
        if arrow_index < target_index {
            target_index = target_index.saturating_sub(1);
        }
        transaction.reorder_elements(vec![arrow_id], target_index as u32);
    }

    pub(crate) fn recompute_bound_arrows(
        &self,
        document: &DocumentModel,
        preview_elements: &[SelectionRectState],
    ) -> Vec<(ElementId, ArrowData, Vec<ElementId>)> {
        let changed_bindable_ids = preview_elements
            .iter()
            .map(|element| element.id)
            .collect::<Vec<_>>();
        if changed_bindable_ids.is_empty() {
            return Vec::new();
        }

        let mut affected_arrow_ids = Vec::new();
        for bindable_id in &changed_bindable_ids {
            for arrow_id in document.bound_arrow_ids(*bindable_id) {
                if !affected_arrow_ids.contains(arrow_id) {
                    affected_arrow_ids.push(*arrow_id);
                }
            }
        }
        if affected_arrow_ids.is_empty() {
            return Vec::new();
        }

        let bindables = self.bindable_elements(document, preview_elements);

        affected_arrow_ids
            .into_iter()
            .filter_map(|id| {
                let arrow = document.arrow(id).ok()?;
                let result = recompute_arrow_after_bindable_change(
                    id,
                    arrow,
                    &bindables,
                    &changed_bindable_ids,
                    self.arrow_engine_context(Modifiers::default()),
                );
                (result.arrow != *arrow).then_some((id, result.arrow, result.reorder_targets))
            })
            .collect()
    }

    fn element_snapping_mode(&self, modifiers: Modifiers) -> SnappingMode {
        resolve_effective_snapping_mode(
            self.config.grid.enabled,
            self.config.snap.enabled,
            modifiers.ctrl,
        )
    }

    pub(crate) fn effective_snapping_mode(&self, modifiers: Modifiers) -> SnappingMode {
        let mode = self.element_snapping_mode(modifiers);
        let guides = self.view.snap_guide_targets;
        let has_guide_targets = guides
            .vertical_xs
            .iter()
            .chain(guides.horizontal_ys.iter())
            .any(Option::is_some);
        // Visible guides are additional targets, not a persistent element-snap
        // setting. They must not reverse Ctrl's temporary element-snap toggle.
        if mode == SnappingMode::None && !modifiers.ctrl && has_guide_targets {
            SnappingMode::Object
        } else {
            mode
        }
    }

    pub(crate) fn snap_creation_point(
        &self,
        document: &DocumentModel,
        current: Point<f64>,
        modifiers: Modifiers,
    ) -> (Point<f64>, Vec<SnapGuide>) {
        match self.effective_snapping_mode(modifiers) {
            SnappingMode::Grid => (
                GRID_SNAP_SERVICE.snap_point(current, self.config.grid.size),
                Vec::new(),
            ),
            SnappingMode::Object => {
                let (point, guides) = if self.element_snapping_mode(modifiers)
                    == SnappingMode::Object
                    && self.config.snap.enable_point_snaps
                {
                    let snap = document.snap_point(&snow_draw_engine_core::SnapQuery {
                        point: current,
                        threshold: self.zoom_adjusted_snap_distance(),
                        include_grid: false,
                        grid_size: self.config.grid.size,
                    });
                    (snap.point, snap.guides)
                } else {
                    (current, Vec::new())
                };
                let (point, guides) = self.snap_point_to_external_guides(current, point, guides);
                (
                    point,
                    if self.config.snap.show_guides {
                        guides
                    } else {
                        Vec::new()
                    },
                )
            }
            SnappingMode::None => (current, Vec::new()),
        }
    }

    pub(crate) fn snap_creation_start_position(
        &self,
        point: Point<f64>,
        modifiers: Modifiers,
    ) -> Point<f64> {
        if self.effective_snapping_mode(modifiers) == SnappingMode::Grid {
            GRID_SNAP_SERVICE.snap_point(point, self.config.grid.size)
        } else {
            point
        }
    }

    pub(crate) fn preview_rectangle_with_snapping(
        &self,
        document: &DocumentModel,
        start: Point<f64>,
        current: Point<f64>,
        modifiers: Modifiers,
    ) -> (Option<RectangleData>, Vec<SnapGuide>) {
        let snapping_mode = self.effective_snapping_mode(modifiers);
        let current = if snapping_mode == SnappingMode::Grid {
            GRID_SNAP_SERVICE.snap_point(current, self.config.grid.size)
        } else {
            current
        };
        let highlight = self.state.active_tool == ActiveTool::RectangleHighlight;
        let spotlight = self.state.active_tool == ActiveTool::Spotlight;
        let style = if highlight {
            self.state
                .default_rectangle_highlight_style
                .rectangle_shape_style()
        } else {
            self.state.default_rectangle_shape_style
        };
        let constrained_current = if modifiers.shift {
            square_constrained_rectangle_end(start, current)
        } else {
            current
        };
        let mut preview = preview_rectangle(start, current, style, modifiers.shift, modifiers.alt);
        if highlight {
            preview = preview.map(|mut rect| {
                rect = rect.into_highlight(snow_draw_engine_document::HighlightShape::Rectangle);
                rect.opacity = self.state.default_rectangle_highlight_style.opacity;
                rect
            });
        }
        if spotlight {
            preview = preview.map(|mut rect| {
                rect.highlight_shape = self.state.default_spotlight_shape;
                rect.into_spotlight()
            });
        }

        let Some(plan) = self.object_snap_plan(
            document,
            ObjectSnapActor::Creation(self.state.active_tool),
            &[],
            modifiers,
        ) else {
            return (preview, Vec::new());
        };
        let Some(rect) = preview else {
            return (None, Vec::new());
        };
        self.snap_created_rectangle(&plan, start, constrained_current, modifiers, rect, style)
    }

    fn snap_created_rectangle(
        &self,
        plan: &ObjectSnapPlan,
        start: Point<f64>,
        constrained_current: Point<f64>,
        modifiers: Modifiers,
        rect: RectangleData,
        style: RectangleShapeStyle,
    ) -> (Option<RectangleData>, Vec<SnapGuide>) {
        let mut preview = Some(rect);
        let move_min_x = constrained_current.x < start.x;
        let move_min_y = constrained_current.y < start.y;
        let mut snap_result = plan.snap_rect(ObjectSnapRectRequest {
            target_rect: rectangle_to_draw_rect(&rect),
            reference_rects: &plan.references,
            snap_distance: plan.snap_distance,
            target_anchors_x: if move_min_x {
                &[SnapAxisAnchor::Start]
            } else {
                &[SnapAxisAnchor::End]
            },
            target_anchors_y: if move_min_y {
                &[SnapAxisAnchor::Start]
            } else {
                &[SnapAxisAnchor::End]
            },
            enable_point_snaps: plan.enable_point_snaps,
            enable_gap_snaps: plan.enable_gap_snaps,
        });
        if snap_result.has_snap() {
            if !modifiers.alt && !modifiers.shift {
                let rect_bounds = rectangle_to_draw_rect(&rect);
                preview = Some(rectangle_from_draw_rect(
                    DrawRect::new(
                        rect_bounds.min_x + if move_min_x { snap_result.dx } else { 0.0 },
                        rect_bounds.min_y + if move_min_y { snap_result.dy } else { 0.0 },
                        rect_bounds.max_x + if move_min_x { 0.0 } else { snap_result.dx },
                        rect_bounds.max_y + if move_min_y { 0.0 } else { snap_result.dy },
                    ),
                    &rect,
                ));
                return (preview, plan.guides(snap_result.guides));
            }
            // Rebuild from the constrained dragged edge rather than the raw
            // pointer. A single-axis snap drives both square dimensions, so
            // inward and outward snaps preserve the constraint equally.
            let constrained_snap =
                constrained_rectangle_snap_end(start, constrained_current, &snap_result);
            let snapped_current = if modifiers.shift {
                constrained_snap.end
            } else {
                Point::new(
                    constrained_current.x + snap_result.dx,
                    constrained_current.y + snap_result.dy,
                )
            };
            let snapped_preview = preview_rectangle(
                start,
                snapped_current,
                style,
                modifiers.shift,
                modifiers.alt,
            );
            if let Some(snapped) = snapped_preview {
                // `preview` may carry a highlight or spotlight kind and its
                // opacity. Preserve those semantic/style fields while taking
                // only the snapped geometry from the base preview.
                let mut next = rect;
                next.center = snapped.center;
                next.width = snapped.width;
                next.height = snapped.height;
                next.corner_radii =
                    normalize_corner_radii(next.width, next.height, next.corner_radii);

                if modifiers.shift {
                    let snapped_move_min_x = snapped_current.x < start.x;
                    let snapped_move_min_y = snapped_current.y < start.y;
                    let target_anchors_x = if constrained_snap.snap_x {
                        if snapped_move_min_x {
                            vec![SnapAxisAnchor::Start]
                        } else {
                            vec![SnapAxisAnchor::End]
                        }
                    } else {
                        Vec::new()
                    };
                    let target_anchors_y = if constrained_snap.snap_y {
                        if snapped_move_min_y {
                            vec![SnapAxisAnchor::Start]
                        } else {
                            vec![SnapAxisAnchor::End]
                        }
                    } else {
                        Vec::new()
                    };
                    let verified_snap = plan.snap_rect(ObjectSnapRectRequest {
                        target_rect: rectangle_to_draw_rect(&next),
                        reference_rects: &plan.references,
                        snap_distance: plan.snap_distance,
                        target_anchors_x: &target_anchors_x,
                        target_anchors_y: &target_anchors_y,
                        enable_point_snaps: plan.enable_point_snaps,
                        enable_gap_snaps: plan.enable_gap_snaps,
                    });
                    let verified = !verified_snap.guides.is_empty()
                        && (!constrained_snap.snap_x || verified_snap.dx.abs() <= 1e-6)
                        && (!constrained_snap.snap_y || verified_snap.dy.abs() <= 1e-6);
                    if verified {
                        preview = Some(next);
                        snap_result.guides = verified_snap.guides;
                    } else {
                        snap_result.guides.clear();
                    }
                } else {
                    preview = Some(next);
                }
            } else if modifiers.shift {
                // Do not show a guide for a snap that would collapse the
                // constrained preview and therefore was not applied.
                snap_result.guides.clear();
            }
        }

        (preview, plan.guides(snap_result.guides))
    }

    pub(crate) fn resolve_move_snap(&self, request: MoveSnapRequest<'_>) -> ObjectSnapResult {
        let MoveSnapRequest {
            document,
            original_bounds,
            original_elements,
            original_arrows,
            base_dx,
            base_dy,
            modifiers,
        } = request;
        let snapping_mode = self.effective_snapping_mode(modifiers);
        let target_rect =
            selection_bounds_to_draw_rect(original_bounds).translate(Point::new(base_dx, base_dy));
        match snapping_mode {
            SnappingMode::Grid => {
                let snapped_rect = snap_rect_to_grid_min_corner(target_rect, self.config.grid.size);
                ObjectSnapResult::new(
                    base_dx + snapped_rect.min_x - target_rect.min_x,
                    base_dy + snapped_rect.min_y - target_rect.min_y,
                    Vec::new(),
                )
            }
            SnappingMode::Object => {
                let excluded = original_elements
                    .iter()
                    .map(|element| element.id)
                    .chain(original_arrows.iter().map(|arrow| arrow.id))
                    .collect::<Vec<_>>();
                let Some(plan) = self.object_snap_plan(
                    document,
                    ObjectSnapActor::selection(document, original_elements, original_arrows),
                    &excluded,
                    modifiers,
                ) else {
                    return ObjectSnapResult::new(base_dx, base_dy, Vec::new());
                };
                let snap_result = plan.snap_rect(ObjectSnapRectRequest {
                    target_rect,
                    reference_rects: &plan.references,
                    snap_distance: plan.snap_distance,
                    target_anchors_x: &[
                        SnapAxisAnchor::Start,
                        SnapAxisAnchor::Center,
                        SnapAxisAnchor::End,
                    ],
                    target_anchors_y: &[
                        SnapAxisAnchor::Start,
                        SnapAxisAnchor::Center,
                        SnapAxisAnchor::End,
                    ],
                    enable_point_snaps: plan.enable_point_snaps,
                    enable_gap_snaps: plan.enable_gap_snaps,
                });

                ObjectSnapResult::new(
                    base_dx + snap_result.dx,
                    base_dy + snap_result.dy,
                    plan.guides(snap_result.guides),
                )
            }
            SnappingMode::None => ObjectSnapResult::new(base_dx, base_dy, Vec::new()),
        }
    }

    pub(crate) fn zoom_adjusted_snap_distance(&self) -> f64 {
        self.config.snap.distance / self.camera().zoom.max(0.0001)
    }

    /// Sole object-snap gateway for creation, move, and resize.
    ///
    /// Returns `None` unless the actor participates. The reference set never
    /// includes filter overlays, even when the actor is layout geometry.
    pub(crate) fn object_snap_plan(
        &self,
        document: &DocumentModel,
        actor: ObjectSnapActor<'_>,
        excluded_ids: &[ElementId],
        modifiers: Modifiers,
    ) -> Option<ObjectSnapPlan> {
        if self.effective_snapping_mode(modifiers) != SnappingMode::Object || !actor.participates()
        {
            return None;
        }
        let element_snapping = self.element_snapping_mode(modifiers) == SnappingMode::Object;
        let targets = self.view.snap_guide_targets;
        let has_targets = targets
            .vertical_xs
            .iter()
            .chain(targets.horizontal_ys.iter())
            .any(Option::is_some);
        if !has_targets
            && (!element_snapping
                || (!self.config.snap.enable_point_snaps && !self.config.snap.enable_gap_snaps))
        {
            return None;
        }
        Some(ObjectSnapPlan {
            references: if element_snapping {
                Self::visible_reference_rects(document, excluded_ids)
            } else {
                Vec::new()
            },
            snap_distance: self.zoom_adjusted_snap_distance(),
            enable_point_snaps: element_snapping && self.config.snap.enable_point_snaps,
            enable_gap_snaps: element_snapping && self.config.snap.enable_gap_snaps,
            show_guides: self.config.snap.show_guides,
            guide_targets: targets,
        })
    }

    fn visible_reference_rects(
        document: &DocumentModel,
        excluded_ids: &[ElementId],
    ) -> Vec<DrawRect> {
        let mut result = Vec::new();
        for id in document.paint_order() {
            if excluded_ids.contains(id) {
                continue;
            }
            let Ok(element) = document.element(*id) else {
                continue;
            };
            if !element.meta.visible {
                continue;
            }
            if element.data.is_filter() {
                continue;
            }
            if let Some(rect) = document.element_rect_proxy(*id) {
                result.push(rotated_rectangle_aabb(&rect));
            }
        }
        result
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct ConstrainedRectangleSnap {
    end: Point<f64>,
    snap_x: bool,
    snap_y: bool,
}

fn constrained_rectangle_snap_end(
    start: Point<f64>,
    constrained_end: Point<f64>,
    snap: &ObjectSnapResult,
) -> ConstrainedRectangleSnap {
    let snapped_x = constrained_end.x + snap.dx;
    let snapped_y = constrained_end.y + snap.dy;
    let x_side = (snapped_x - start.x).abs();
    let y_side = (snapped_y - start.y).abs();
    let snap_x = snap.dx != 0.0;
    let snap_y = snap.dy != 0.0;

    if snap_x && snap_y {
        let side_tolerance = 1e-9 * x_side.max(y_side).max(1.0);
        if (x_side - y_side).abs() <= side_tolerance {
            return ConstrainedRectangleSnap {
                end: Point::new(snapped_x, snapped_y),
                snap_x: true,
                snap_y: true,
            };
        }
        if snap.dx.abs() <= snap.dy.abs() {
            return constrained_rectangle_snap_from_x(start, constrained_end, snapped_x);
        }
        return constrained_rectangle_snap_from_y(start, constrained_end, snapped_y);
    }
    if snap_x {
        return constrained_rectangle_snap_from_x(start, constrained_end, snapped_x);
    }
    if snap_y {
        return constrained_rectangle_snap_from_y(start, constrained_end, snapped_y);
    }
    ConstrainedRectangleSnap {
        end: constrained_end,
        snap_x: false,
        snap_y: false,
    }
}

fn constrained_rectangle_snap_from_x(
    start: Point<f64>,
    constrained_end: Point<f64>,
    snapped_x: f64,
) -> ConstrainedRectangleSnap {
    let side = (snapped_x - start.x).abs();
    ConstrainedRectangleSnap {
        end: Point::new(
            snapped_x,
            start.y + (constrained_end.y - start.y).signum() * side,
        ),
        snap_x: true,
        snap_y: false,
    }
}

fn constrained_rectangle_snap_from_y(
    start: Point<f64>,
    constrained_end: Point<f64>,
    snapped_y: f64,
) -> ConstrainedRectangleSnap {
    let side = (snapped_y - start.y).abs();
    ConstrainedRectangleSnap {
        end: Point::new(
            start.x + (constrained_end.x - start.x).signum() * side,
            snapped_y,
        ),
        snap_x: false,
        snap_y: true,
    }
}

pub(crate) struct MoveSnapRequest<'a> {
    pub(crate) document: &'a DocumentModel,
    pub(crate) original_bounds: &'a SelectionBounds,
    pub(crate) original_elements: &'a [SelectionRectState],
    pub(crate) original_arrows: &'a [SelectionArrowState],
    pub(crate) base_dx: f64,
    pub(crate) base_dy: f64,
    pub(crate) modifiers: Modifiers,
}

/// Subject of an object-snap attempt: the geometry being created, moved, or resized.
///
/// Object snapping aligns layout geometry. Filter overlays never participate as
/// this actor. Every object-snap application goes through `object_snap_plan`,
/// which also omits filter overlays from the reference set. Grid snapping is a
/// separate mode and is unchanged.
#[derive(Clone, Copy)]
pub(crate) enum ObjectSnapActor<'a> {
    Creation(ActiveTool),
    Selection {
        document: &'a DocumentModel,
        elements: &'a [SelectionRectState],
        arrows: &'a [SelectionArrowState],
    },
}

impl<'a> ObjectSnapActor<'a> {
    pub(crate) fn selection(
        document: &'a DocumentModel,
        elements: &'a [SelectionRectState],
        arrows: &'a [SelectionArrowState],
    ) -> Self {
        Self::Selection {
            document,
            elements,
            arrows,
        }
    }

    pub(crate) fn participates(&self) -> bool {
        match self {
            Self::Creation(tool) => !tool.is_filter(),
            Self::Selection {
                document,
                elements,
                arrows,
            } => !selection_is_filter_only(document, elements, arrows),
        }
    }
}

pub(crate) struct ObjectSnapPlan {
    pub(crate) references: Vec<DrawRect>,
    pub(crate) snap_distance: f64,
    pub(crate) enable_point_snaps: bool,
    enable_gap_snaps: bool,
    show_guides: bool,
    guide_targets: SnapGuideTargets,
}

impl ObjectSnapPlan {
    pub(crate) fn snap_rect(&self, request: ObjectSnapRectRequest<'_>) -> ObjectSnapResult {
        let mut result = OBJECT_SNAP_SERVICE.snap_rect(request);
        for (axis, lines, anchors) in [
            (
                SnapGuideAxis::Vertical,
                &self.guide_targets.vertical_xs[..],
                request.target_anchors_x,
            ),
            (
                SnapGuideAxis::Horizontal,
                &self.guide_targets.horizontal_ys[..],
                request.target_anchors_y,
            ),
        ] {
            let coordinates = match axis {
                SnapGuideAxis::Vertical => [
                    request.target_rect.min_x,
                    request.target_rect.center_x(),
                    request.target_rect.max_x,
                ],
                SnapGuideAxis::Horizontal => [
                    request.target_rect.min_y,
                    request.target_rect.center_y(),
                    request.target_rect.max_y,
                ],
            };
            let nearest = lines
                .iter()
                .flatten()
                .flat_map(|line| {
                    anchors.iter().map(move |anchor| {
                        let index = match anchor {
                            SnapAxisAnchor::Start => 0,
                            SnapAxisAnchor::Center => 1,
                            SnapAxisAnchor::End => 2,
                        };
                        *line - coordinates[index]
                    })
                })
                .filter(|offset| offset.abs() <= self.snap_distance)
                .min_by(|left, right| left.abs().total_cmp(&right.abs()));
            if let Some(offset) = nearest {
                let object_present = result.guides.iter().any(|guide| guide.axis == axis);
                let object_offset = if axis == SnapGuideAxis::Vertical {
                    result.dx
                } else {
                    result.dy
                };
                if !object_present || offset.abs() <= object_offset.abs() {
                    result.guides.retain(|guide| guide.axis != axis);
                    if axis == SnapGuideAxis::Vertical {
                        result.dx = offset;
                    } else {
                        result.dy = offset;
                    }
                }
            }
        }
        result
    }

    pub(crate) fn guides(&self, guides: Vec<SnapGuide>) -> Vec<SnapGuide> {
        if self.show_guides { guides } else { Vec::new() }
    }
}

fn selection_is_filter_only(
    document: &DocumentModel,
    elements: &[SelectionRectState],
    arrows: &[SelectionArrowState],
) -> bool {
    arrows.is_empty()
        && !elements.is_empty()
        && elements.iter().all(|element| {
            document
                .element_kind(element.id)
                .is_ok_and(ElementKind::is_filter)
        })
}

fn preview_rectangle(
    start: Point<f64>,
    end: Point<f64>,
    style: RectangleShapeStyle,
    lock_aspect_ratio: bool,
    scale_from_center: bool,
) -> Option<RectangleData> {
    let (center, width, height) = if scale_from_center {
        let constrained_end = if lock_aspect_ratio {
            square_constrained_rectangle_end(start, end)
        } else {
            end
        };
        (
            start,
            2.0 * (constrained_end.x - start.x).abs(),
            2.0 * (constrained_end.y - start.y).abs(),
        )
    } else {
        let end = if lock_aspect_ratio {
            square_constrained_rectangle_end(start, end)
        } else {
            end
        };
        (
            Point {
                x: f64::midpoint(start.x, end.x),
                y: f64::midpoint(start.y, end.y),
            },
            (end.x - start.x).abs(),
            (end.y - start.y).abs(),
        )
    };
    if width <= 0.0 || height <= 0.0 {
        return None;
    }

    Some(RectangleData {
        rectangle_kind: RectangleElementKind::Rectangle,
        highlight_shape: style.shape,
        center,
        width,
        height,
        rotation: 0.0,
        fill: style.fill,
        fill_style: style.fill_style,
        stroke: style.stroke,
        stroke_width: style.stroke_width,
        stroke_style: style.stroke_style,
        corner_radii: normalize_corner_radii(width, height, style.corner_radii),
        opacity: 1.0,
    })
}

#[cfg(test)]
mod external_guide_tests {
    use super::*;
    use snow_draw_engine_document::ElementMeta;

    fn element_snap_fixture(with_guides: bool) -> (Editor, DocumentModel) {
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        if with_guides {
            // Keep guides away from the element so they cannot mask its snap.
            editor.view.snap_guide_targets.vertical_xs[0] = Some(400.0);
            editor.view.snap_guide_targets.horizontal_ys[0] = Some(400.0);
        }
        let mut document = DocumentModel::new();
        let rectangle = preview_rectangle(
            Point::new(-100.0, -100.0),
            Point::new(100.0, 100.0),
            editor.state.default_rectangle_shape_style,
            false,
            false,
        )
        .unwrap();
        let mut transaction = Transaction::new("insert snap reference");
        transaction.insert_rectangle(
            document.peek_next_element_id(),
            ElementMeta::default(),
            rectangle,
        );
        document.apply_transaction(transaction).unwrap();
        (editor, document)
    }

    #[test]
    fn ctrl_temporarily_enables_element_snapping_with_or_without_visible_guides() {
        for with_guides in [false, true] {
            let (editor, document) = element_snap_fixture(with_guides);
            for ctrl in [false, true] {
                let (preview, guides) = editor.preview_rectangle_with_snapping(
                    &document,
                    Point::new(250.0, -180.0),
                    Point::new(103.0, -103.0),
                    Modifiers {
                        ctrl,
                        ..Default::default()
                    },
                );
                let bounds = rectangle_to_draw_rect(&preview.unwrap());
                assert_eq!(bounds.min_x, if ctrl { 100.0 } else { 103.0 });
                assert_eq!(bounds.max_y, if ctrl { -100.0 } else { -103.0 });
                assert_eq!(!guides.is_empty(), ctrl);
                assert!(!editor.snap_config().enabled);
            }
        }
    }

    #[test]
    fn ctrl_temporarily_enables_arrow_and_serial_number_point_snapping() {
        for with_guides in [false, true] {
            let (editor, document) = element_snap_fixture(with_guides);
            for ctrl in [false, true] {
                let source = Point::new(-97.0, -103.0);
                let modifiers = Modifiers {
                    ctrl,
                    ..Default::default()
                };
                let expected = if ctrl {
                    // Point snapping uses painted bounds, including the 2 px stroke.
                    Point::new(-101.0, -101.0)
                } else {
                    source
                };
                for (point, guides) in [
                    editor.snap_arrow_creation_point(&document, source, modifiers),
                    editor.snap_serial_number_creation_center(&document, source, modifiers),
                ] {
                    assert_eq!(point, expected);
                    assert_eq!(!guides.is_empty(), ctrl);
                }
            }
        }
    }

    #[test]
    fn ctrl_toggles_element_snapping_for_move_and_resize_without_changing_config() {
        for with_guides in [false, true] {
            let (mut editor, mut document) = element_snap_fixture(with_guides);
            let rectangle = preview_rectangle(
                Point::new(200.0, 200.0),
                Point::new(300.0, 300.0),
                editor.state.default_rectangle_shape_style,
                false,
                false,
            )
            .unwrap();
            let id = document.peek_next_element_id();
            let mut transaction = Transaction::new("insert selected rectangle");
            transaction.insert_rectangle(id, ElementMeta::default(), rectangle);
            document.apply_transaction(transaction).unwrap();
            let elements = [SelectionRectState {
                id,
                rect: rectangle,
            }];
            let bounds = SelectionBounds {
                center: rectangle.center,
                width: rectangle.width,
                height: rectangle.height,
                rotation: 0.0,
            };
            for persistent in [false, true] {
                editor.config.snap.enabled = persistent;
                for ctrl in [false, true] {
                    let modifiers = Modifiers {
                        ctrl,
                        ..Default::default()
                    };
                    let should_snap = persistent != ctrl;
                    let moved = editor.resolve_move_snap(MoveSnapRequest {
                        document: &document,
                        original_bounds: &bounds,
                        original_elements: &elements,
                        original_arrows: &[],
                        base_dx: -97.0,
                        base_dy: -97.0,
                        modifiers,
                    });
                    let expected_delta = if should_snap { -100.0 } else { -97.0 };
                    assert_eq!((moved.dx, moved.dy), (expected_delta, expected_delta));
                    assert_eq!(!moved.guides.is_empty(), should_snap);

                    let resized = editor.resize_selection_preview(
                        &document,
                        ResizeSelectionContext {
                            original_elements: &elements,
                            original_arrows: &[],
                            original_bounds: &bounds,
                            handle: ResizeHandle::TopLeft,
                            handle_offset_canvas: Point::new(0.0, 0.0),
                            frame_padding: 0.0,
                            corner_handle_outset: 0.0,
                        },
                        Point::new(103.0, 103.0),
                        modifiers,
                        None,
                    );
                    let rect = rectangle_to_draw_rect(&resized.elements[0].rect);
                    let expected_min = if should_snap { 100.0 } else { 103.0 };
                    assert!((rect.min_x - expected_min).abs() < 1e-9);
                    assert!((rect.min_y - expected_min).abs() < 1e-9);
                    assert_eq!(!resized.snap_guides.is_empty(), should_snap);
                    assert_eq!(editor.snap_config().enabled, persistent);
                }
            }
        }
    }

    #[test]
    fn temporary_element_snapping_respects_disabled_snap_kinds() {
        let (mut editor, document) = element_snap_fixture(false);
        editor.config.snap.enable_point_snaps = false;
        editor.config.snap.enable_gap_snaps = false;
        let modifiers = Modifiers {
            ctrl: true,
            ..Default::default()
        };
        let (preview, guides) = editor.preview_rectangle_with_snapping(
            &document,
            Point::new(250.0, -180.0),
            Point::new(103.0, -103.0),
            modifiers,
        );
        assert_eq!(rectangle_to_draw_rect(&preview.unwrap()).min_x, 103.0);
        assert!(guides.is_empty());
        let source = Point::new(-97.0, -103.0);
        assert_eq!(
            editor.snap_creation_point(&document, source, modifiers),
            (source, Vec::new())
        );
    }

    fn plan(references: Vec<DrawRect>, element_snapping: bool) -> ObjectSnapPlan {
        ObjectSnapPlan {
            references,
            snap_distance: 8.0,
            enable_point_snaps: element_snapping,
            enable_gap_snaps: false,
            show_guides: true,
            guide_targets: SnapGuideTargets {
                vertical_xs: [Some(100.0), None],
                horizontal_ys: [Some(80.0), None],
            },
        }
    }

    fn snap(plan: &ObjectSnapPlan, rect: DrawRect) -> ObjectSnapResult {
        plan.snap_rect(ObjectSnapRectRequest {
            target_rect: rect,
            reference_rects: &plan.references,
            snap_distance: plan.snap_distance,
            target_anchors_x: &[SnapAxisAnchor::Center],
            target_anchors_y: &[SnapAxisAnchor::Center],
            enable_point_snaps: plan.enable_point_snaps,
            enable_gap_snaps: false,
        })
    }

    #[test]
    fn stationary_guides_snap_without_document_elements() {
        let snap = snap(
            &plan(Vec::new(), false),
            DrawRect::new(91.0, 72.0, 107.0, 88.0),
        );
        assert_eq!((snap.dx, snap.dy), (1.0, 0.0));
    }

    #[test]
    fn closer_guide_beats_element_candidate() {
        let references = vec![DrawRect::new(93.0, 72.0, 109.0, 88.0)];
        let snap = snap(
            &plan(references, true),
            DrawRect::new(91.0, 72.0, 107.0, 88.0),
        );
        assert_eq!(snap.dx, 1.0);
    }

    #[test]
    fn guide_wins_an_exact_tie_with_an_element_candidate() {
        let references = vec![DrawRect::new(92.0, 72.0, 108.0, 88.0)];
        let snapped = snap(
            &plan(references, true),
            DrawRect::new(91.0, 72.0, 107.0, 88.0),
        );
        assert_eq!(snapped.dx, 1.0);
        assert!(
            snapped
                .guides
                .iter()
                .all(|guide| guide.axis != SnapGuideAxis::Vertical),
            "the visible guide should replace the element snap marker on a tie"
        );
    }

    #[test]
    fn resize_snap_uses_only_the_dragged_edge_anchor() {
        let plan = plan(Vec::new(), false);
        let target_rect = DrawRect::new(20.0, 20.0, 97.0, 60.0);
        let snapped = plan.snap_rect(ObjectSnapRectRequest {
            target_rect,
            reference_rects: &[],
            snap_distance: plan.snap_distance,
            target_anchors_x: &[SnapAxisAnchor::End],
            target_anchors_y: &[],
            enable_point_snaps: false,
            enable_gap_snaps: false,
        });
        assert_eq!((snapped.dx, snapped.dy), (3.0, 0.0));
    }

    #[test]
    fn visible_guides_do_not_change_the_persistent_snapping_toggle() {
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.view.snap_guide_targets.vertical_xs[0] = Some(100.0);
        assert_eq!(
            editor.effective_snapping_mode(Modifiers::default()),
            SnappingMode::Object
        );
        assert_eq!(
            editor.effective_snapping_mode(Modifiers {
                ctrl: true,
                ..Default::default()
            }),
            SnappingMode::Object
        );
        editor.config.snap.enabled = true;
        assert_eq!(
            editor.effective_snapping_mode(Modifiers {
                ctrl: true,
                ..Default::default()
            }),
            SnappingMode::None
        );
        editor.config.grid.enabled = true;
        assert_eq!(
            editor.effective_snapping_mode(Modifiers::default()),
            SnappingMode::Grid
        );
        assert_eq!(
            editor.effective_snapping_mode(Modifiers {
                ctrl: true,
                ..Default::default()
            }),
            SnappingMode::None
        );
    }
}
