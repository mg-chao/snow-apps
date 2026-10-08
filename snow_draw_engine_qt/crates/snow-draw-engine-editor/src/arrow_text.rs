use std::collections::{HashMap, HashSet};
use std::hash::{Hash, Hasher};

use snow_draw_engine_core::{DrawRect, ErrorCode, Point, ViewportQuery, canvas_viewport};
use snow_draw_engine_document::{
    ArrowData, DocumentDelta, DocumentRevision, ElementData, ElementId, Operation, TextData,
    TextLayoutSize, Transaction, arrow_text_anchor, arrow_text_max_width, text_hit_test,
    text_with_measured_layout, validate_text_layout_size,
};
use snow_draw_engine_model::{DocumentModel, SpatialIndex};

use crate::Editor;

#[derive(Clone, Debug, PartialEq)]
pub struct ArrowTextLayoutRequest {
    pub arrow_id: ElementId,
    pub arrow_width: f64,
    pub text_id: ElementId,
    pub text: TextData,
    pub max_width: f64,
    pub key: u64,
    text_key: u64,
    angle_geometry: Option<snow_draw_engine_document::AngleGeometry>,
    angle_stroke_width: f64,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct ArrowTextMeasurement {
    pub text_id: ElementId,
    pub key: u64,
    pub size: TextLayoutSize,
    pub text_key: u64,
    pub natural_width: f64,
}

#[derive(Clone, Debug)]
struct CachedArrowText {
    request: ArrowTextLayoutRequest,
    preview: TextData,
    preview_bounds: DrawRect,
    preview_needed: bool,
}

#[derive(Clone, Debug, Default)]
pub(crate) struct ArrowTextCache {
    revision: Option<DocumentRevision>,
    generation: u64,
    relation_revision: u64,
    order: Vec<ElementId>,
    entries: HashMap<ElementId, CachedArrowText>,
    dirty: HashSet<ElementId>,
    pending: HashSet<ElementId>,
    preview_index: SpatialIndex,
    preview_candidate_count: usize,
    builds: u64,
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
    let derived = snow_draw_engine_document::generated_annotation_label(arrow, Some(text.layout));
    let text = derived.as_ref().unwrap_or(text);
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
    if !arrow.is_angle() {
        text.center = arrow_text_anchor(arrow);
    }
    text.rotation = 0.0;
    ArrowTextLayoutRequest {
        angle_geometry: snow_draw_engine_document::angle_geometry(arrow),
        angle_stroke_width: arrow.stroke_width,
        arrow_id,
        arrow_width: arrow.width,
        text_id,
        text,
        max_width,
        key: key.finish(),
        text_key,
    }
}

impl Editor {
    pub fn arrow_text_request_build_count(&self) -> u64 {
        self.arrow_text_cache.borrow().builds
    }

    pub fn arrow_text_cached_owner_count(&self) -> usize {
        self.arrow_text_cache.borrow().entries.len()
    }

    pub fn arrow_text_preview_candidate_count(&self) -> usize {
        self.arrow_text_cache.borrow().preview_candidate_count
    }

    pub fn sync_arrow_text_cache_after_document_change(
        &mut self,
        document: &DocumentModel,
        delta: &DocumentDelta,
    ) {
        for id in &delta.removed {
            self.state.arrow_text_measurements.remove(id);
        }
        let mut cache = self.arrow_text_cache.borrow_mut();
        if cache.revision.is_none() {
            return;
        }
        for id in delta.touched.iter().chain(&delta.created) {
            if document.bound_text_id_for_arrow(*id).is_some() {
                cache.dirty.insert(*id);
            }
            if let Some(owner) = document.arrow_label_owner(*id) {
                cache.dirty.insert(owner);
            }
        }
        for id in &delta.removed {
            cache.entries.remove(id);
            cache.pending.remove(id);
            cache.dirty.remove(id);
            cache.preview_index.update_bounds(*id, None);
        }
        if cache.relation_revision != document.relation_index_build_count() {
            cache.order = document
                .arrow_label_bindings()
                .iter()
                .map(|(owner, _)| *owner)
                .collect();
            let invalid: Vec<_> = cache
                .entries
                .iter()
                .filter_map(|(owner, entry)| {
                    (document.bound_text_id_for_arrow(*owner) != Some(entry.request.text_id))
                        .then_some(*owner)
                })
                .collect();
            for owner in invalid {
                cache.entries.remove(&owner);
                cache.preview_index.update_bounds(owner, None);
            }
            let live: HashSet<_> = cache.order.iter().copied().collect();
            cache.pending.retain(|owner| live.contains(owner));
            cache.dirty.retain(|owner| live.contains(owner));
            cache.relation_revision = document.relation_index_build_count();
        }
        cache.revision = Some(document.document_revision());
    }

    fn prepare_arrow_text_cache(&self, document: &DocumentModel) {
        let mut cache = self.arrow_text_cache.borrow_mut();
        if cache.revision != Some(document.document_revision())
            || cache.generation != self.state.arrow_text_measurement_generation
        {
            let builds = cache.builds;
            *cache = ArrowTextCache {
                builds,
                ..Default::default()
            };
            cache.revision = Some(document.document_revision());
            cache.generation = self.state.arrow_text_measurement_generation;
            cache.relation_revision = document.relation_index_build_count();
            cache.order = document
                .arrow_label_bindings()
                .iter()
                .map(|(owner, _)| *owner)
                .collect();
            cache.dirty = cache.order.iter().copied().collect();
        }
        let dirty = std::mem::take(&mut cache.dirty);
        for owner in dirty {
            let Some(text_id) = document.bound_text_id_for_arrow(owner) else {
                cache.entries.remove(&owner);
                cache.pending.remove(&owner);
                cache.preview_index.update_bounds(owner, None);
                continue;
            };
            let (Ok(arrow), Ok(text)) = (document.arrow(owner), document.text(text_id)) else {
                continue;
            };
            let request = request(owner, text_id, arrow, text, cache.generation);
            let pending = !self
                .state
                .arrow_text_measurements
                .get(&text_id)
                .is_some_and(|measurement| measurement.matches(&request));
            let preview = self.measured_arrow_text(request.clone());
            let preview_bounds = snow_draw_engine_document::text_bounds(&preview);
            let preview_needed = &preview != text;
            cache
                .preview_index
                .update_bounds(owner, preview_needed.then_some(preview_bounds));
            cache.builds += 1;
            if pending {
                cache.pending.insert(owner);
            } else {
                cache.pending.remove(&owner);
            }
            cache.entries.insert(
                owner,
                CachedArrowText {
                    request,
                    preview,
                    preview_bounds,
                    preview_needed,
                },
            );
        }
    }

    pub fn invalidate_arrow_text_measurements(&mut self) {
        self.state.arrow_text_measurements.clear();
        self.state.arrow_text_measurement_generation =
            self.state.arrow_text_measurement_generation.wrapping_add(1);
        *self.arrow_text_cache.borrow_mut() = ArrowTextCache::default();
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
        let preview = self
            .preview_selection_arrows(document)
            .into_iter()
            .find(|arrow| arrow.id == arrow_id);
        let Some(arrow) = preview
            .as_ref()
            .map(|state| &state.arrow)
            .or_else(|| document.arrow(arrow_id).ok())
        else {
            return false;
        };
        let Ok(text) = document.text(text_id) else {
            return false;
        };
        let text = self.measured_arrow_text(request(
            arrow_id,
            text_id,
            arrow,
            text,
            self.state.arrow_text_measurement_generation,
        ));
        !text.text.trim().is_empty() && text_hit_test(&text, canvas_point, 0.0)
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
            if !arrow.arrow.is_angle() {
                text.center = arrow_text_anchor(&arrow.arrow);
            }
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
        let arrows: HashMap<_, _> = self
            .preview_selection_arrows(document)
            .into_iter()
            .map(|arrow| (arrow.id, arrow.arrow))
            .collect();
        self.prepare_arrow_text_cache(document);
        let cache = self.arrow_text_cache.borrow();
        let mut requests: Vec<_> = cache
            .pending
            .iter()
            .filter(|owner| !arrows.contains_key(owner))
            .filter_map(|owner| cache.entries.get(owner).map(|entry| entry.request.clone()))
            .collect();
        for (owner, arrow) in arrows {
            let Some(text_id) = arrow.text_element_id else {
                continue;
            };
            let Ok(text) = document.text(text_id) else {
                continue;
            };
            let request = request(
                owner,
                text_id,
                &arrow,
                text,
                self.state.arrow_text_measurement_generation,
            );
            if !self
                .state
                .arrow_text_measurements
                .get(&text_id)
                .is_some_and(|measurement| measurement.matches(&request))
            {
                requests.push(request);
            }
        }
        requests.sort_unstable_by_key(|request| document.paint_rank(request.arrow_id));
        drop(cache);
        if let Some(request) = self.distance_creation_layout_request(document)
            && !self
                .state
                .arrow_text_measurements
                .get(&request.text_id)
                .is_some_and(|m| m.matches(&request))
        {
            requests.push(request);
        }
        requests
    }

    pub(crate) fn distance_creation_layout_request(
        &self,
        document: &DocumentModel,
    ) -> Option<ArrowTextLayoutRequest> {
        let crate::ElementCreationPreview::Arrow(arrow) = self.state.creation_preview.as_ref()?
        else {
            return None;
        };
        let text = snow_draw_engine_document::generated_annotation_label(arrow, None)?;
        Some(request(
            document.peek_next_element_id(),
            self.distance_creation_text_id(),
            arrow,
            &text,
            self.state.arrow_text_measurement_generation,
        ))
    }

    pub(crate) fn measured_distance_label(
        &self,
        arrow_id: ElementId,
        arrow: &ArrowData,
        text: &TextData,
    ) -> TextData {
        self.measured_arrow_text(request(
            arrow_id,
            self.distance_creation_text_id(),
            arrow,
            text,
            self.state.arrow_text_measurement_generation,
        ))
    }

    pub(crate) fn distance_creation_text(
        &self,
        document: &DocumentModel,
    ) -> Option<(ElementId, TextData)> {
        let request = self.distance_creation_layout_request(document)?;
        Some((request.text_id, self.measured_arrow_text(request)))
    }

    pub fn apply_arrow_text_measurement(
        &mut self,
        document: &DocumentModel,
        text_id: ElementId,
        key: u64,
        size: TextLayoutSize,
        natural_width: f64,
    ) -> Result<bool, ErrorCode> {
        self.apply_arrow_text_measurements(document, &[(text_id, key, size, natural_width)])
    }

    pub fn apply_arrow_text_measurements(
        &mut self,
        document: &DocumentModel,
        layouts: &[(ElementId, u64, TextLayoutSize, f64)],
    ) -> Result<bool, ErrorCode> {
        if layouts.is_empty() {
            return Ok(false);
        }
        // Resolve previews and pending requests once for the complete host batch.
        let mut requests: HashMap<_, _> = self
            .arrow_text_layout_requests(document)
            .into_iter()
            .map(|request| (request.text_id, request))
            .collect();
        let mut measured = Vec::with_capacity(layouts.len());
        for &(text_id, key, size, natural_width) in layouts {
            let size = validate_text_layout_size(size)?;
            // Zero preserves the exact-constraint contract for older hosts.
            if !natural_width.is_finite() || natural_width < 0.0 {
                return Err(ErrorCode::InvalidArgument);
            }
            let Some(request) = requests.get(&text_id).filter(|r| r.key == key) else {
                continue;
            };
            if size.width() > request.max_width + 0.01
                || (natural_width > 0.0
                    && (size.width() - natural_width.min(request.max_width)).abs() > 0.01)
            {
                return Err(ErrorCode::InvalidArgument);
            }
            measured.push(ArrowTextMeasurement {
                text_id,
                key,
                size,
                text_key: request.text_key,
                natural_width,
            });
            // Duplicate/stale metrics in the same batch cannot overwrite a result.
            requests.remove(&text_id);
        }
        if measured.is_empty() {
            return Ok(false);
        }
        // Validate the entire batch before changing state. This makes errors
        // atomic without cloning the editor and every retained measurement.
        let preview_id = matches!(self.state.creation_preview.as_ref(),
            Some(crate::ElementCreationPreview::Arrow(arrow)) if arrow.is_generated_annotation())
        .then(|| self.distance_creation_text_id());
        self.state
            .arrow_text_measurements
            .retain(|id, _| document.text(*id).is_ok() || Some(*id) == preview_id);
        for measurement in measured {
            if let Some(owner) = document.arrow_label_owner(measurement.text_id) {
                self.arrow_text_cache.borrow_mut().dirty.insert(owner);
            }
            self.state
                .arrow_text_measurements
                .insert(measurement.text_id, measurement);
        }
        self.bump_scene_state_revision();
        self.bump_overlay_state_revision();
        Ok(true)
    }

    fn measured_arrow_text(&self, request: ArrowTextLayoutRequest) -> TextData {
        let measurement = self
            .state
            .arrow_text_measurements
            .get(&request.text_id)
            .filter(|m| m.matches(&request));
        let mut text = request.text;
        if let Some(measurement) = measurement {
            text = text_with_measured_layout(&text, measurement.size)
                .expect("arrow measurements are validated when they are applied");
        }
        if let Some(geometry) = request.angle_geometry {
            let offset = geometry.radius
                + text.layout.width().hypot(text.layout.height()) * 0.5
                + 6.0
                + request.angle_stroke_width / 2.0;
            text.center = Point::new(
                geometry.vertex.x + geometry.bisector.x * offset,
                geometry.vertex.y + geometry.bisector.y * offset,
            );
        }
        text
    }

    pub(crate) fn arrow_text_previews(
        &self,
        document: &DocumentModel,
    ) -> Vec<(ElementId, TextData)> {
        let arrows: HashMap<_, _> = self
            .preview_selection_arrows(document)
            .into_iter()
            .map(|arrow| (arrow.id, arrow.arrow))
            .collect();
        self.prepare_arrow_text_cache(document);
        let mut cache = self.arrow_text_cache.borrow_mut();
        let surface = self.surface_size();
        let viewport = canvas_viewport(self.camera(), surface);
        // Keep overrides for visible committed owners/text as well as labels whose
        // measured bounds enter the viewport. This also removes stale estimated
        // labels that become invisible after host measurement.
        let mut owners: Vec<_> = if surface.width > 0 && surface.height > 0 {
            let visible = document
                .visible_element_ids(ViewportQuery {
                    surface,
                    camera: self.camera(),
                })
                .into_iter()
                .map(|id| document.arrow_label_owner(id).unwrap_or(id))
                .collect::<HashSet<_>>();
            let mut candidates = cache
                .preview_index
                .with_viewport_candidate_ids(viewport, |ids| {
                    ids.iter()
                        .copied()
                        .filter(|owner| {
                            cache.entries.get(owner).is_some_and(|entry| {
                                entry.preview_bounds.max_x >= viewport.0
                                    && entry.preview_bounds.min_x <= viewport.2
                                    && entry.preview_bounds.max_y >= viewport.1
                                    && entry.preview_bounds.min_y <= viewport.3
                            })
                        })
                        .collect::<HashSet<_>>()
                });
            candidates.extend(visible.into_iter().filter(|owner| {
                cache
                    .entries
                    .get(owner)
                    .is_some_and(|entry| entry.preview_needed)
            }));
            candidates.extend(arrows.keys().copied());
            candidates.into_iter().collect()
        } else {
            cache.order.clone()
        };
        owners.sort_unstable_by_key(|owner| document.paint_rank(*owner));
        cache.preview_candidate_count = owners.len();
        let mut previews = Vec::new();
        for owner in &owners {
            if let Some(arrow) = arrows.get(owner) {
                let Some(text_id) = arrow.text_element_id else {
                    continue;
                };
                let Ok(text) = document.text(text_id) else {
                    continue;
                };
                previews.push((
                    text_id,
                    self.measured_arrow_text(request(
                        *owner,
                        text_id,
                        arrow,
                        text,
                        cache.generation,
                    )),
                ));
            } else if let Some(entry) = cache
                .entries
                .get(owner)
                .filter(|entry| entry.preview_needed)
            {
                previews.push((entry.request.text_id, entry.preview.clone()));
            }
        }
        previews
    }

    /// Add host measurements to the originating command before it enters history.
    pub fn append_arrow_text_layouts(
        &self,
        document: &DocumentModel,
        transaction: &mut Transaction,
    ) {
        self.append_arrow_text_layouts_preserving_labels(document, transaction, &HashSet::new());
    }

    /// Restored history labels retain their exact data even when host metrics are warmer.
    pub fn append_arrow_text_layouts_preserving_labels(
        &self,
        document: &DocumentModel,
        transaction: &mut Transaction,
        preserved_labels: &HashSet<ElementId>,
    ) {
        // Measurements join the command that changes their owner or text. Unrelated
        // measured labels remain previews instead of expanding this command's history.
        let mut arrows = std::collections::HashMap::new();
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
                        if let Some(owner) = document.arrow_id_for_text(*id)
                            && let Ok(arrow) = document.arrow(owner)
                        {
                            arrows.entry(owner).or_insert_with(|| arrow.clone());
                        }
                    }
                    _ => {}
                },
                Operation::RemoveElement { id } => {
                    removed.insert(*id);
                }
                Operation::UpdateElementMeta { id, .. } => {
                    let owner = document.arrow_id_for_text(*id).unwrap_or(*id);
                    if let Ok(arrow) = document.arrow(owner) {
                        arrows.entry(owner).or_insert_with(|| arrow.clone());
                    }
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
            if removed.contains(&id)
                || removed.contains(&text_id)
                || preserved_labels.contains(&text_id)
            {
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
