use std::hash::{Hash, Hasher};

use snow_draw_engine_core::{ErrorCode, Point};
use snow_draw_engine_document::{
    ArrowData, ElementData, ElementId, Operation, TextData, TextLayoutSize, Transaction,
    arrow_text_anchor, arrow_text_max_width, text_hit_test, text_with_measured_layout,
    validate_text_layout_size,
};
use snow_draw_engine_model::DocumentModel;

use crate::Editor;

#[derive(Clone, Debug, PartialEq)]
pub struct ArrowTextLayoutRequest {
    pub arrow_id: ElementId,
    pub text_id: ElementId,
    pub text: TextData,
    pub max_width: f64,
    pub key: u64,
    text_key: u64,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct ArrowTextMeasurement {
    pub text_id: ElementId,
    pub key: u64,
    pub size: TextLayoutSize,
    pub text_key: u64,
    pub natural_width: f64,
}

impl ArrowTextMeasurement {
    fn matches(&self, request: &ArrowTextLayoutRequest) -> bool {
        self.text_id == request.text_id
            && (self.key == request.key
                || (self.text_key == request.text_key
                    && self.natural_width > 0.0
                    && self.size.width() == self.natural_width
                    && request.max_width >= self.natural_width))
    }
}

fn request(
    arrow_id: ElementId,
    text_id: ElementId,
    arrow: &ArrowData,
    text: &TextData,
    generation: u64,
) -> ArrowTextLayoutRequest {
    let max_width = arrow_text_max_width(arrow, text.font_size);
    let mut key = std::collections::hash_map::DefaultHasher::new();
    generation.hash(&mut key);
    text_id.hash(&mut key);
    text.text.hash(&mut key);
    text.font_family.hash(&mut key);
    text.font_size.to_bits().hash(&mut key);
    (text.horizontal_align as u8).hash(&mut key);
    (text.vertical_align as u8).hash(&mut key);
    let text_key = key.finish();
    max_width.to_bits().hash(&mut key);
    let mut text = text.clone();
    text.center = arrow_text_anchor(arrow);
    text.rotation = 0.0;
    ArrowTextLayoutRequest {
        arrow_id,
        text_id,
        text,
        max_width,
        key: key.finish(),
        text_key,
    }
}

impl Editor {
    pub fn invalidate_arrow_text_measurements(&mut self) {
        self.state.arrow_text_measurements.clear();
        self.state.arrow_text_measurement_generation =
            self.state.arrow_text_measurement_generation.wrapping_add(1);
    }

    pub(crate) fn arrow_label_hit(
        &self,
        document: &DocumentModel,
        arrow_id: ElementId,
        canvas_point: Point<f64>,
    ) -> bool {
        let Some(text_id) = document.bound_text_id_for_arrow(arrow_id) else {
            return false;
        };
        self.arrow_text_previews(document)
            .into_iter()
            .find(|(id, _)| *id == text_id)
            .is_some_and(|(_, text)| {
                !text.text.trim().is_empty() && text_hit_test(&text, canvas_point, 0.0)
            })
    }

    pub(crate) fn arrow_text_selection_bounds(
        &self,
        document: &DocumentModel,
        bounds: Option<crate::SelectionBounds>,
        arrows: &[crate::SelectionArrowState],
    ) -> Option<crate::SelectionBounds> {
        let bounds = bounds?;
        let (sin, cos) = bounds.rotation.sin_cos();
        let (mut min_x, mut min_y) = (-bounds.width / 2.0, -bounds.height / 2.0);
        let (mut max_x, mut max_y) = (-min_x, -min_y);
        for arrow in arrows {
            let Some(text_id) = arrow.arrow.text_element_id else {
                continue;
            };
            let Ok(text) = document.text(text_id) else {
                continue;
            };
            let mut text = self
                .active_text_draft_text_for_id(text_id)
                .unwrap_or_else(|| {
                    self.measured_arrow_text(request(
                        arrow.id,
                        text_id,
                        &arrow.arrow,
                        text,
                        self.state.arrow_text_measurement_generation,
                    ))
                });
            text.center = arrow_text_anchor(&arrow.arrow);
            text.rotation = 0.0;
            let rect = snow_draw_engine_document::text_bounds(&text);
            for (x, y) in [
                (rect.min_x, rect.min_y),
                (rect.max_x, rect.min_y),
                (rect.max_x, rect.max_y),
                (rect.min_x, rect.max_y),
            ] {
                let (x, y) = (x - bounds.center.x, y - bounds.center.y);
                let (x, y) = (x * cos + y * sin, -x * sin + y * cos);
                min_x = min_x.min(x);
                max_x = max_x.max(x);
                min_y = min_y.min(y);
                max_y = max_y.max(y);
            }
        }
        let (x, y) = (f64::midpoint(min_x, max_x), f64::midpoint(min_y, max_y));
        Some(crate::SelectionBounds {
            center: snow_draw_engine_core::Point::new(
                bounds.center.x + x * cos - y * sin,
                bounds.center.y + x * sin + y * cos,
            ),
            width: max_x - min_x,
            height: max_y - min_y,
            rotation: bounds.rotation,
        })
    }

    pub fn arrow_text_layout_requests(
        &self,
        document: &DocumentModel,
    ) -> Vec<ArrowTextLayoutRequest> {
        let arrows = self.preview_selection_arrows(document);
        document
            .arrow_text_bindings()
            .into_iter()
            .filter_map(|(arrow_id, text_id)| {
                let arrow = arrows
                    .iter()
                    .find(|a| a.id == arrow_id)
                    .map(|a| &a.arrow)
                    .or_else(|| document.arrow(arrow_id).ok())?;
                let text = document.text(text_id).ok()?;
                let request = request(
                    arrow_id,
                    text_id,
                    arrow,
                    text,
                    self.state.arrow_text_measurement_generation,
                );
                (!self
                    .state
                    .arrow_text_measurements
                    .iter()
                    .any(|m| m.matches(&request)))
                .then_some(request)
            })
            .collect()
    }

    pub fn apply_arrow_text_measurement(
        &mut self,
        document: &DocumentModel,
        text_id: ElementId,
        key: u64,
        size: TextLayoutSize,
        natural_width: f64,
    ) -> Result<bool, ErrorCode> {
        let size = validate_text_layout_size(size)?;
        // Zero preserves the exact-constraint contract for hosts that do not
        // supply natural metrics. Never infer an unwrapped layout from its size.
        if !natural_width.is_finite() || natural_width < 0.0 {
            return Err(ErrorCode::InvalidArgument);
        }
        let Some(request) = self
            .arrow_text_layout_requests(document)
            .into_iter()
            .find(|r| r.text_id == text_id && r.key == key)
        else {
            return Ok(false);
        };
        if size.width() > request.max_width + 0.01 {
            return Err(ErrorCode::InvalidArgument);
        }
        if natural_width > 0.0 && (size.width() - natural_width.min(request.max_width)).abs() > 0.01
        {
            return Err(ErrorCode::InvalidArgument);
        }
        self.state
            .arrow_text_measurements
            .retain(|m| m.text_id != text_id && document.text(m.text_id).is_ok());
        self.state
            .arrow_text_measurements
            .push(ArrowTextMeasurement {
                text_id,
                key,
                size,
                text_key: request.text_key,
                natural_width,
            });
        self.bump_scene_state_revision();
        self.bump_overlay_state_revision();
        Ok(true)
    }

    fn measured_arrow_text(&self, request: ArrowTextLayoutRequest) -> TextData {
        let measurement = self
            .state
            .arrow_text_measurements
            .iter()
            .find(|m| m.matches(&request));
        let mut text = request.text;
        if let Some(measurement) = measurement {
            text = text_with_measured_layout(&text, measurement.size)
                .expect("arrow measurements are validated when they are applied");
        }
        text
    }

    pub(crate) fn arrow_text_previews(
        &self,
        document: &DocumentModel,
    ) -> Vec<(ElementId, TextData)> {
        let arrows = self.preview_selection_arrows(document);
        document
            .arrow_text_bindings()
            .into_iter()
            .filter_map(|(arrow_id, text_id)| {
                let arrow = arrows
                    .iter()
                    .find(|a| a.id == arrow_id)
                    .map(|a| &a.arrow)
                    .or_else(|| document.arrow(arrow_id).ok())?;
                let text = document.text(text_id).ok()?;
                Some((
                    text_id,
                    self.measured_arrow_text(request(
                        arrow_id,
                        text_id,
                        arrow,
                        text,
                        self.state.arrow_text_measurement_generation,
                    )),
                ))
            })
            .collect()
    }

    /// Add host measurements to the originating command before it enters history.
    pub fn append_arrow_text_layouts(
        &self,
        document: &DocumentModel,
        transaction: &mut Transaction,
    ) {
        let mut arrows: std::collections::HashMap<_, _> = document
            .arrow_text_bindings()
            .into_iter()
            .filter_map(|(id, _)| document.arrow(id).ok().map(|a| (id, a.clone())))
            .collect();
        let mut texts = std::collections::HashMap::new();
        let mut removed = std::collections::HashSet::new();
        for operation in transaction.operations() {
            match operation {
                Operation::UpdateElementData { id, data }
                | Operation::InsertElement { id, data, .. } => match data {
                    ElementData::Arrow(arrow) => {
                        arrows.insert(*id, arrow.clone());
                    }
                    ElementData::Text(text) => {
                        texts.insert(*id, text.clone());
                    }
                    _ => {}
                },
                Operation::RemoveElement { id } => {
                    removed.insert(*id);
                }
                _ => {}
            }
        }
        let mut arrows: Vec<_> = arrows.into_iter().collect();
        arrows.sort_by_key(|(id, _)| *id);
        for (id, arrow) in arrows {
            let Some(text_id) = arrow.text_element_id else {
                continue;
            };
            if removed.contains(&id) || removed.contains(&text_id) {
                continue;
            }
            let Some(text) = texts.get(&text_id).or_else(|| document.text(text_id).ok()) else {
                continue;
            };
            let updated = self.measured_arrow_text(request(
                id,
                text_id,
                &arrow,
                text,
                self.state.arrow_text_measurement_generation,
            ));
            if &updated != text {
                transaction.update_text(text_id, updated);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::{
        ColorRgba8, Point,
        arrow::{ArrowType, StrokeStyle},
    };
    use snow_draw_engine_document::{ElementMeta, RectangleData, text_bounds};

    fn measurement_fixture() -> (Editor, DocumentModel, ElementId, ElementId) {
        let owner = ElementId {
            index: 0,
            generation: 1,
        };
        let label = ElementId {
            index: 1,
            generation: 1,
        };
        let mut arrow = ArrowData::from_global_points(
            &[Point::new(0.0, 0.0), Point::new(1000.0, 0.0)],
            ColorRgba8::default(),
            2.0,
            StrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )
        .unwrap();
        arrow.text_element_id = Some(label);
        let mut document = DocumentModel::new();
        let mut tx = Transaction::new("label measurement fixture");
        tx.insert_arrow(owner, ElementMeta::default(), arrow);
        tx.insert_text(
            label,
            ElementMeta::default(),
            TextData {
                text: "unchanged label".to_owned(),
                font_size: 20.0,
                layout: TextLayoutSize::new(60.0, 20.0),
                ..TextData::default()
            },
        );
        document.apply_transaction(tx).unwrap();
        (
            Editor::new(Default::default()).unwrap(),
            document,
            owner,
            label,
        )
    }

    fn change_width(document: &mut DocumentModel, owner: ElementId, width: f64) {
        let mut arrow = document.arrow(owner).unwrap().clone();
        arrow.width = width;
        arrow.points[1][0] = width;
        let mut tx = Transaction::new("reshape arrow");
        tx.update_arrow(owner, arrow);
        document.apply_transaction(tx).unwrap();
    }

    #[test]
    fn natural_label_layout_survives_geometry_changes_until_wrapping_is_required() {
        let (mut editor, mut document, owner, label) = measurement_fixture();
        let request = editor.arrow_text_layout_requests(&document).remove(0);
        let size = TextLayoutSize::with_content(360.0, 25.0, 355.0, 25.0);
        assert!(
            editor
                .apply_arrow_text_measurement(&document, label, request.key, size, 360.0)
                .unwrap()
        );
        for width in [1001.0, 2400.0, 600.0] {
            change_width(&mut document, owner, width);
            assert!(editor.arrow_text_layout_requests(&document).is_empty());
            let preview = &editor.arrow_text_previews(&document)[0].1;
            assert_eq!(preview.layout, size);
            assert_eq!(preview.center, Point::new(width / 2.0, 0.0));
        }
        change_width(&mut document, owner, 300.0);
        let wrapped = editor.arrow_text_layout_requests(&document).remove(0);
        assert_eq!(wrapped.max_width, 220.0);
        assert!(
            !editor
                .apply_arrow_text_measurement(&document, label, request.key, size, 360.0)
                .unwrap()
        );
        editor
            .apply_arrow_text_measurement(
                &document,
                label,
                wrapped.key,
                TextLayoutSize::new(220.0, 50.0),
                360.0,
            )
            .unwrap();
        change_width(&mut document, owner, 400.0);
        assert_eq!(editor.arrow_text_layout_requests(&document).len(), 1);
        change_width(&mut document, owner, 1000.0);
        assert_eq!(editor.arrow_text_layout_requests(&document).len(), 1);
    }

    #[test]
    fn natural_label_metrics_invalidate_for_text_font_and_alignment_changes() {
        let (mut editor, mut document, _owner, label) = measurement_fixture();
        for variant in 0..4 {
            let request = editor.arrow_text_layout_requests(&document).remove(0);
            editor
                .apply_arrow_text_measurement(
                    &document,
                    label,
                    request.key,
                    TextLayoutSize::new(100.0, 25.0),
                    100.0,
                )
                .unwrap();
            let mut text = document.text(label).unwrap().clone();
            match variant {
                0 => text.text.push('!'),
                1 => text.font_size += 1.0,
                2 => text.font_family = Some("another font".to_owned()),
                _ => text.horizontal_align = snow_draw_engine_document::TextHorizontalAlign::Right,
            }
            let mut tx = Transaction::new("change label typography");
            tx.update_text(label, text);
            document.apply_transaction(tx).unwrap();
            assert_eq!(editor.arrow_text_layout_requests(&document).len(), 1);
            assert!(
                !editor
                    .apply_arrow_text_measurement(
                        &document,
                        label,
                        request.key,
                        TextLayoutSize::new(100.0, 25.0),
                        100.0
                    )
                    .unwrap()
            );
        }
    }

    #[test]
    fn unknown_natural_metrics_only_reuse_the_exact_constraint() {
        let (mut editor, mut document, owner, label) = measurement_fixture();
        let request = editor.arrow_text_layout_requests(&document).remove(0);
        editor
            .apply_arrow_text_measurement(
                &document,
                label,
                request.key,
                TextLayoutSize::new(100.0, 25.0),
                0.0,
            )
            .unwrap();
        assert!(editor.arrow_text_layout_requests(&document).is_empty());
        change_width(&mut document, owner, 1001.0);
        assert_eq!(editor.arrow_text_layout_requests(&document).len(), 1);
    }

    #[test]
    fn host_font_invalidation_rejects_old_results_and_requests_new_metrics() {
        let (mut editor, document, _owner, label) = measurement_fixture();
        let request = editor.arrow_text_layout_requests(&document).remove(0);
        let size = TextLayoutSize::new(100.0, 25.0);
        editor
            .apply_arrow_text_measurement(&document, label, request.key, size, 100.0)
            .unwrap();
        editor.invalidate_arrow_text_measurements();
        let replacement = editor.arrow_text_layout_requests(&document).remove(0);
        assert_ne!(replacement.key, request.key);
        assert!(
            !editor
                .apply_arrow_text_measurement(&document, label, request.key, size, 100.0)
                .unwrap()
        );
        assert!(
            editor
                .apply_arrow_text_measurement(
                    &document,
                    label,
                    replacement.key,
                    TextLayoutSize::new(120.0, 28.0),
                    120.0
                )
                .unwrap()
        );
        assert_eq!(editor.arrow_text_previews(&document)[0].1.width(), 120.0);
    }

    #[test]
    fn natural_label_metrics_reject_invalid_or_inconsistent_widths() {
        let (mut editor, document, _owner, label) = measurement_fixture();
        let request = editor.arrow_text_layout_requests(&document).remove(0);
        for natural in [-1.0, f64::NAN, f64::INFINITY, 99.0, 101.0] {
            assert_eq!(
                editor.apply_arrow_text_measurement(
                    &document,
                    label,
                    request.key,
                    TextLayoutSize::new(100.0, 25.0),
                    natural
                ),
                Err(ErrorCode::InvalidArgument)
            );
        }
        assert_eq!(editor.arrow_text_layout_requests(&document).len(), 1);
    }

    #[test]
    fn selected_arrow_bounds_follow_live_label_draft_and_revert_on_cancel() {
        let owner = ElementId {
            index: 0,
            generation: 1,
        };
        let text_id = ElementId {
            index: 1,
            generation: 1,
        };
        let mut arrow = ArrowData::from_global_points(
            &[Point::new(-40.0, 0.0), Point::new(40.0, 0.0)],
            ColorRgba8::default(),
            2.0,
            StrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )
        .unwrap();
        arrow.text_element_id = Some(text_id);
        let mut document = DocumentModel::new();
        let mut transaction = Transaction::new("bound label");
        transaction.insert_arrow(owner, ElementMeta::default(), arrow);
        transaction.insert_text(
            text_id,
            ElementMeta::default(),
            TextData {
                text: "label".to_owned(),
                layout: TextLayoutSize::new(30.0, 20.0),
                ..TextData::default()
            },
        );
        document.apply_transaction(transaction).unwrap();
        let mut editor = Editor::new(Default::default()).unwrap();
        editor.select_element(&document, owner).unwrap();
        let committed = editor
            .presentation_state(&document)
            .selection_bounds
            .unwrap();

        for (revision, width, height) in [(1, 240.0, 75.0), (2, 20.0, 12.0)] {
            let mut text = document.text(text_id).unwrap().clone();
            text.layout = TextLayoutSize::new(width, height);
            editor
                .set_active_text_draft_presentation(
                    &document,
                    crate::ActiveTextDraftPresentation {
                        target: crate::ActiveTextDraftTarget::Existing(text_id),
                        revision,
                        text,
                    },
                )
                .unwrap();
            let bounds = editor
                .presentation_state(&document)
                .selection_bounds
                .unwrap();
            assert_eq!(editor.selection_bounds_snapshot(&document), Some(bounds));
            if revision == 1 {
                assert!(bounds.width > committed.width + 100.0);
                assert!(bounds.height > committed.height + 40.0);
            } else {
                assert!((bounds.width - committed.width).abs() < 1e-9);
                assert!(bounds.height < committed.height);
            }
        }
        editor.clear_active_text_draft_presentation();
        assert_eq!(
            editor.presentation_state(&document).selection_bounds,
            Some(committed)
        );
    }

    #[test]
    fn arrow_text_selection_encloses_horizontal_label_in_rotated_and_multiple_selection() {
        let owner = ElementId {
            index: 0,
            generation: 1,
        };
        let text_id = ElementId {
            index: 1,
            generation: 1,
        };
        let rectangle_id = ElementId {
            index: 2,
            generation: 1,
        };
        let mut arrow = ArrowData::from_global_points(
            &[Point::new(-80.0, 0.0), Point::new(80.0, 0.0)],
            ColorRgba8::default(),
            2.0,
            StrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )
        .unwrap();
        arrow.rotation = 0.7;
        arrow.text_element_id = Some(text_id);
        let mut document = DocumentModel::new();
        let mut tx = Transaction::new("rotated label selection");
        tx.insert_arrow(owner, ElementMeta::default(), arrow);
        tx.insert_text(
            text_id,
            ElementMeta::default(),
            TextData {
                text: "wide label".to_owned(),
                layout: TextLayoutSize::new(210.0, 80.0),
                ..TextData::default()
            },
        );
        tx.insert_rectangle(
            rectangle_id,
            ElementMeta::default(),
            RectangleData {
                center: Point::new(250.0, 100.0),
                width: 50.0,
                height: 60.0,
                rotation: 0.0,
                rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
                highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
                fill: ColorRgba8::default(),
                fill_style: snow_draw_engine_document::FillStyle::Solid,
                stroke: ColorRgba8::default(),
                stroke_width: 0.0,
                stroke_style: StrokeStyle::Solid,
                corner_radii: Default::default(),
                opacity: 1.0,
            },
        );
        document.apply_transaction(tx).unwrap();
        let mut editor = Editor::new(Default::default()).unwrap();
        for ids in [vec![text_id], vec![owner, text_id, rectangle_id]] {
            editor.set_selection_state_with_document(Some(&document), ids, Some(text_id));
            assert!(!editor.state.selection.ids.contains(&text_id));
            let presentation = editor.presentation_state(&document);
            let bounds = presentation.selection_bounds.unwrap();
            let rect = text_bounds(document.text(text_id).unwrap());
            let (sin, cos) = bounds.rotation.sin_cos();
            for (x, y) in [
                (rect.min_x, rect.min_y),
                (rect.max_x, rect.min_y),
                (rect.max_x, rect.max_y),
                (rect.min_x, rect.max_y),
            ] {
                let (x, y) = (x - bounds.center.x, y - bounds.center.y);
                assert!((x * cos + y * sin).abs() <= bounds.width / 2.0 + 1e-9);
                assert!((-x * sin + y * cos).abs() <= bounds.height / 2.0 + 1e-9);
            }
        }
    }
}
