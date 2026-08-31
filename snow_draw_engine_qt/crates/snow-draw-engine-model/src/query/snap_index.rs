use snow_draw_engine_core::{
    DrawRect, Point, SnapGuide, SnapGuideAxis, SnapGuideKind, SnapQuery, SnapResult,
};
use snow_draw_engine_document::{
    Document, DocumentDelta, ElementData, ElementId, RectangleData, rect_bounds,
};

use super::SpatialIndex;

#[derive(Clone, Debug, Default)]
pub struct SnapIndex {
    rects: Vec<Option<DrawRect>>,
}

impl SnapIndex {
    pub fn rebuild(&mut self, document: &Document) {
        self.rects.clear();
        for state in document.element_states() {
            if !state.visible {
                continue;
            }
            let ElementData::Rectangle(rect) = state.data else {
                continue;
            };
            self.ensure_capacity(state.id.index as usize);
            self.rects[state.id.index as usize] = Some(rect_bounds(rect));
        }
    }

    pub fn refresh(&mut self, document: &Document, delta: &DocumentDelta) {
        let mut ids = delta.touched.clone();
        for id in &delta.removed {
            if !ids.contains(id) {
                ids.push(*id);
            }
        }

        for id in ids {
            self.ensure_capacity(id.index as usize);
            self.rects[id.index as usize] = None;
            let Ok(element) = document.element(id) else {
                continue;
            };
            if !element.meta.visible {
                continue;
            }
            let ElementData::Rectangle(rect) = &element.data else {
                continue;
            };
            self.rects[id.index as usize] = Some(rect_bounds(rect));
        }
    }

    pub fn rect(&self, id: ElementId) -> Option<DrawRect> {
        self.rects
            .get(id.index as usize)
            .and_then(|bounds| bounds.as_ref().copied())
    }

    pub fn snap_point_filtered(
        &self,
        document: &Document,
        spatial_index: &SpatialIndex,
        query: &SnapQuery,
        mut include_candidate: impl FnMut(ElementId, &RectangleData) -> bool,
    ) -> SnapResult {
        let mut snap_x = nearest_grid_value(query.point.x, query.include_grid, query.grid_size);
        let mut snap_y = nearest_grid_value(query.point.y, query.include_grid, query.grid_size);
        let mut best_x_distance = snap_x
            .map(|candidate| (candidate - query.point.x).abs())
            .unwrap_or(f64::INFINITY);
        let mut best_y_distance = snap_y
            .map(|candidate| (candidate - query.point.y).abs())
            .unwrap_or(f64::INFINITY);

        spatial_index.with_point_candidate_ids(query.point, query.threshold, |candidate_ids| {
            for id in candidate_ids {
                let Some(bounds) = self.rect(*id) else {
                    continue;
                };
                let Ok(rect) = document.rectangle(*id) else {
                    continue;
                };
                if !include_candidate(*id, rect) {
                    continue;
                }

                let candidates_x = [bounds.center_x(), bounds.min_x, bounds.max_x];
                for candidate in candidates_x {
                    let distance = (candidate - query.point.x).abs();
                    if distance <= query.threshold
                        && (distance < best_x_distance
                            || (distance == best_x_distance
                                && snap_x.is_none_or(|value| candidate < value)))
                    {
                        snap_x = Some(candidate);
                        best_x_distance = distance;
                    }
                }

                let candidates_y = [bounds.center_y(), bounds.min_y, bounds.max_y];
                for candidate in candidates_y {
                    let distance = (candidate - query.point.y).abs();
                    if distance <= query.threshold
                        && (distance < best_y_distance
                            || (distance == best_y_distance
                                && snap_y.is_none_or(|value| candidate < value)))
                    {
                        snap_y = Some(candidate);
                        best_y_distance = distance;
                    }
                }
            }
        });

        let mut point = query.point;
        let mut guides = Vec::new();

        if let Some(value) =
            snap_x.filter(|value| (*value - query.point.x).abs() <= query.threshold)
        {
            point.x = value;
            guides.push(SnapGuide {
                kind: SnapGuideKind::Point,
                axis: SnapGuideAxis::Vertical,
                start: Point::new(value, query.point.y),
                end: Point::new(value, query.point.y),
                markers: vec![Point::new(value, query.point.y)],
                label: None,
            });
        }
        if let Some(value) =
            snap_y.filter(|value| (*value - query.point.y).abs() <= query.threshold)
        {
            point.y = value;
            guides.push(SnapGuide {
                kind: SnapGuideKind::Point,
                axis: SnapGuideAxis::Horizontal,
                start: Point::new(query.point.x, value),
                end: Point::new(query.point.x, value),
                markers: vec![Point::new(query.point.x, value)],
                label: None,
            });
        }

        SnapResult { point, guides }
    }

    fn ensure_capacity(&mut self, index: usize) {
        if self.rects.len() <= index {
            self.rects.resize(index + 1, None);
        }
    }
}

fn nearest_grid_value(value: f64, include_grid: bool, grid_size: f64) -> Option<f64> {
    if !include_grid || !grid_size.is_finite() || grid_size <= 0.0 {
        return None;
    }
    Some((value / grid_size).round() * grid_size)
}
