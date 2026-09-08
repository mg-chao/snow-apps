use std::hash::{Hash, Hasher};

use snow_draw_engine_core::ErrorCode;
use snow_draw_engine_document::{
    ArrowData, ElementData, ElementId, Operation, TextData, TextLayoutSize, Transaction,
    arrow_text_anchor, arrow_text_max_width, validate_text_layout_size,
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
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct ArrowTextMeasurement {
    pub text_id: ElementId,
    pub key: u64,
    pub size: TextLayoutSize,
}

fn request(
    arrow_id: ElementId,
    text_id: ElementId,
    arrow: &ArrowData,
    text: &TextData,
) -> ArrowTextLayoutRequest {
    let max_width = arrow_text_max_width(arrow, text.font_size);
    let mut key = std::collections::hash_map::DefaultHasher::new();
    text_id.hash(&mut key);
    text.text.hash(&mut key);
    text.font_family.hash(&mut key);
    text.font_size.to_bits().hash(&mut key);
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
    }
}

impl Editor {
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
            let text = self.measured_arrow_text(request(arrow.id, text_id, &arrow.arrow, text));
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
                let request = request(arrow_id, text_id, arrow, text);
                (!self
                    .state
                    .arrow_text_measurements
                    .iter()
                    .any(|m| m.text_id == text_id && m.key == request.key))
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
    ) -> Result<bool, ErrorCode> {
        let size = validate_text_layout_size(size)?;
        let Some(request) = self
            .arrow_text_layout_requests(document)
            .into_iter()
            .find(|r| r.text_id == text_id && r.key == key)
        else {
            return Ok(false);
        };
        if size.width > request.max_width + 0.01 {
            return Err(ErrorCode::InvalidArgument);
        }
        self.state
            .arrow_text_measurements
            .retain(|m| m.text_id != text_id && document.text(m.text_id).is_ok());
        self.state
            .arrow_text_measurements
            .push(ArrowTextMeasurement { text_id, key, size });
        self.bump_scene_state_revision();
        self.bump_overlay_state_revision();
        Ok(true)
    }

    fn measured_arrow_text(&self, request: ArrowTextLayoutRequest) -> TextData {
        let mut text = request.text;
        if let Some(measurement) = self
            .state
            .arrow_text_measurements
            .iter()
            .find(|m| m.text_id == request.text_id && m.key == request.key)
        {
            text.width = measurement.size.width;
            text.height = measurement.size.height;
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
                    self.measured_arrow_text(request(arrow_id, text_id, arrow, text)),
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
            let updated = self.measured_arrow_text(request(id, text_id, &arrow, text));
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
                width: 210.0,
                height: 80.0,
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
