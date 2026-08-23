use super::*;
use snow_draw_engine_core::arrow::{BindMode, EngineContext};

impl Editor {
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

    pub(crate) fn effective_snapping_mode(&self, modifiers: Modifiers) -> SnappingMode {
        resolve_effective_snapping_mode(
            self.config.grid.enabled,
            self.config.snap.enabled,
            modifiers.ctrl,
        )
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
            preview = preview.map(RectangleData::into_spotlight);
        }

        let should_object_snap = snapping_mode == SnappingMode::Object
            && preview.is_some()
            && (self.config.snap.enable_point_snaps || self.config.snap.enable_gap_snaps);
        if !should_object_snap {
            return (preview, Vec::new());
        }

        let Some(rect) = preview else {
            return (None, Vec::new());
        };
        let move_min_x = constrained_current.x < start.x;
        let move_min_y = constrained_current.y < start.y;
        let reference_rects = Self::visible_reference_rects(document, &[]);
        let snap_distance = self.zoom_adjusted_snap_distance();
        let mut snap_result = OBJECT_SNAP_SERVICE.snap_rect(ObjectSnapRectRequest {
            target_rect: rectangle_to_draw_rect(&rect),
            reference_rects: &reference_rects,
            snap_distance,
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
            enable_point_snaps: self.config.snap.enable_point_snaps,
            enable_gap_snaps: self.config.snap.enable_gap_snaps,
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
                return (
                    preview,
                    if self.config.snap.show_guides {
                        snap_result.guides
                    } else {
                        Vec::new()
                    },
                );
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
                    let verified_snap = OBJECT_SNAP_SERVICE.snap_rect(ObjectSnapRectRequest {
                        target_rect: rectangle_to_draw_rect(&next),
                        reference_rects: &reference_rects,
                        snap_distance,
                        target_anchors_x: &target_anchors_x,
                        target_anchors_y: &target_anchors_y,
                        enable_point_snaps: self.config.snap.enable_point_snaps,
                        enable_gap_snaps: self.config.snap.enable_gap_snaps,
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

        (
            preview,
            if self.config.snap.show_guides {
                snap_result.guides
            } else {
                Vec::new()
            },
        )
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
                if !self.config.snap.enable_point_snaps && !self.config.snap.enable_gap_snaps {
                    return ObjectSnapResult::new(base_dx, base_dy, Vec::new());
                }

                let excluded = original_elements
                    .iter()
                    .map(|element| element.id)
                    .chain(original_arrows.iter().map(|arrow| arrow.id))
                    .collect::<Vec<_>>();
                let snap_result = OBJECT_SNAP_SERVICE.snap_move(
                    target_rect,
                    &Self::visible_reference_rects(document, &excluded),
                    self.zoom_adjusted_snap_distance(),
                    self.config.snap.enable_point_snaps,
                    self.config.snap.enable_gap_snaps,
                );

                ObjectSnapResult::new(
                    base_dx + snap_result.dx,
                    base_dy + snap_result.dy,
                    if self.config.snap.show_guides {
                        snap_result.guides
                    } else {
                        Vec::new()
                    },
                )
            }
            SnappingMode::None => ObjectSnapResult::new(base_dx, base_dy, Vec::new()),
        }
    }

    pub(crate) fn zoom_adjusted_snap_distance(&self) -> f64 {
        self.config.snap.distance / self.camera().zoom.max(0.0001)
    }

    pub(crate) fn visible_reference_rects(
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
