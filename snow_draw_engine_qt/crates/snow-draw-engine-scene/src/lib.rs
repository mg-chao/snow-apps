mod dirty_regions;
mod item_bounds;
mod item_conversions;
mod overlay_composition;
mod scene_cache;
mod scene_composition;
mod selection_visuals;

use dirty_regions::finalize_dirty_regions;
use item_bounds::*;
use item_conversions::*;
use overlay_composition::*;
pub use scene_cache::DocumentSceneCache;
use scene_composition::*;
use selection_visuals::*;
use snow_draw_engine_core::{
    ColorRgba8, CornerRadii, DrawRect, Point, SnapConfig, SnapGuide, ViewportQuery,
    arrow::{
        ArrowEndpointPosition, ArrowPathCommand, StrokeStyle, ArrowType, ArrowheadDashMode,
        ArrowheadFillMode, ArrowheadRenderPrimitive,
    },
    canvas_viewport,
};
use snow_draw_engine_display::{
    ArrowDisplayItem, ArrowheadDisplayDashMode, ArrowheadDisplayFillMode,
    ArrowheadDisplayPrimitive, ArrowheadDisplayPrimitiveKind, DecorationPatch, DecorationRevision,
    DecorationView, DirtyRegion, DisplayFillStyle, DisplayItemId, DisplaySpotlightCutout,
    DisplayTextHorizontalAlign, DisplayTextVerticalAlign, FrameView,
    LayerPatch, OverlayDisplayItem, OverlayRevision, PatchCursor, PathChunkReplacement,
    PathGeometryPatch, PenFilterGeometryPatch, RectangleDisplayItem, ReplaceRangeOp,
    SceneDisplayItem, SceneRevision, SerialNumberConnectorDisplayItem, SerialNumberDisplayItem,
    SnapGuideDisplayItem, TextDisplayItem, UiFocusConnectionDisplayItem, UiRectangleDisplayItem,
    UiShapeKind, ViewportPatch, full_surface_dirty_region,
};
use snow_draw_engine_document::{
    ArrowData, ElementData, ElementId, FillStyle, RectangleData, SerialNumberData,
    TextData, TextHorizontalAlign, TextVerticalAlign, arrow_bounds, arrow_is_degenerate,
    arrowhead_render_primitives, filter_bounds, resolve_serial_number_stroke_width,
    resolve_serial_number_text_connection, serial_number_bounds, serial_number_with_selection_rect,
    text_bounds,
};
use snow_draw_engine_editor::{
    ArrowHandleKind, ArrowHandleState, EditorPresentationState, EditorSession,
    EditorViewportState, ElementCreationPreview, SelectionArrowState, SelectionBounds,
    SelectionRectState, selection_box_visible_for_members,
};
use snow_draw_engine_model::DocumentModel;
use std::collections::HashMap;
use std::sync::Arc;

#[derive(Clone, Debug)]
pub struct ViewportComposer {
    frame_view: FrameView,
    decoration_view: DecorationView,
    spotlight_cutouts: Vec<DisplaySpotlightCutout>,
    scene_items: Vec<SceneDisplayItem>,
    overlay_items: Vec<OverlayDisplayItem>,
    scene_revision: SceneRevision,
    decoration_revision: DecorationRevision,
    overlay_revision: OverlayRevision,
    pen_geometry_revisions: HashMap<DisplayItemId, u64>,
    next_pen_geometry_revision: u64,
    last_patch: Arc<ViewportPatch>,
    initialized: bool,
}

impl Default for ViewportComposer {
    fn default() -> Self {
        Self {
            frame_view: FrameView::default(),
            decoration_view: DecorationView::default(),
            spotlight_cutouts: Vec::new(),
            scene_items: Vec::new(),
            overlay_items: Vec::new(),
            scene_revision: SceneRevision(0),
            decoration_revision: DecorationRevision(0),
            overlay_revision: OverlayRevision(0),
            pen_geometry_revisions: HashMap::new(),
            next_pen_geometry_revision: 0,
            last_patch: Arc::new(ViewportPatch::default()),
            initialized: false,
        }
    }
}

impl ViewportComposer {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn current_cursor(&self) -> PatchCursor {
        PatchCursor {
            scene_revision: self.scene_revision,
            decoration_revision: self.decoration_revision,
            overlay_revision: self.overlay_revision,
        }
    }

    pub fn refresh(
        &mut self,
        cache: &DocumentSceneCache,
        model: &DocumentModel,
        session: &mut EditorSession,
        view: &EditorViewportState,
    ) {
        let view_state = session.view_state(view);
        let frame_view = FrameView {
            surface: view_state.surface,
            camera: view_state.camera,
            clear_color: view_state.clear_color,
        };
        let presentation = session.presentation_state_for_refresh(model, view);
        self.refresh_with_presentation(
            cache,
            model,
            frame_view,
            &presentation,
            session.snap_config(),
        );
    }

    fn refresh_with_presentation(
        &mut self,
        cache: &DocumentSceneCache,
        model: &DocumentModel,
        frame_view: FrameView,
        presentation: &EditorPresentationState,
        snap_config: SnapConfig,
    ) {
        let next_scene_items = compose_scene_items(cache, model, presentation, frame_view);
        let next_overlay_items = compose_overlay_items(snap_config, presentation, frame_view);
        let next_decoration_view = display_decoration(model, presentation);
        let next_spotlight_cutouts =
            compose_spotlight_cutouts(cache, model, presentation, frame_view);
        let force_reset = !self.initialized;
        let surface_or_camera_changed = force_reset
            || self.frame_view.surface != frame_view.surface
            || self.frame_view.camera != frame_view.camera;
        let clear_color_changed =
            force_reset || self.frame_view.clear_color != frame_view.clear_color;

        let (scene_patch, scene_revision) = build_scene_layer_patch(
            &self.scene_items,
            self.scene_revision.0,
            &next_scene_items,
            frame_view,
            force_reset,
            surface_or_camera_changed || clear_color_changed,
        );
        let (overlay_patch, overlay_revision) = build_layer_patch(
            &self.overlay_items,
            self.overlay_revision.0,
            &next_overlay_items,
            frame_view,
            force_reset,
            surface_or_camera_changed,
            overlay_display_item_bounds,
        );
        let (decoration_patch, decoration_revision) = build_decoration_patch(
            &self.decoration_view,
            &self.spotlight_cutouts,
            self.decoration_revision.0,
            next_decoration_view,
            &next_spotlight_cutouts,
            DecorationPatchOptions {
                frame_view,
                force_reset,
                force_full_dirty: surface_or_camera_changed,
            },
        );

        let pen_filter_geometry_ops = build_pen_filter_geometry_ops(
            &self.scene_items,
            &scene_patch,
            &mut self.pen_geometry_revisions,
            &mut self.next_pen_geometry_revision,
        );
        let path_geometry_ops = build_path_geometry_ops(&self.scene_items, &scene_patch);
        self.frame_view = frame_view;
        self.decoration_view = next_decoration_view;
        self.spotlight_cutouts = next_spotlight_cutouts;
        self.scene_items = next_scene_items;
        self.overlay_items = next_overlay_items;
        self.scene_revision = SceneRevision(scene_revision);
        self.decoration_revision = DecorationRevision(decoration_revision);
        self.overlay_revision = OverlayRevision(overlay_revision);
        self.last_patch = Arc::new(ViewportPatch {
            frame_view,
            decoration: decoration_patch,
            scene: scene_patch,
            pen_filter_geometry_ops,
            path_geometry_ops,
            overlay: overlay_patch,
        });
        self.initialized = true;
    }

    /// Returns the patch to apply for the given cursor.
    ///
    /// The common incremental branch (the client applied the previous patch)
    /// hands out the cached patch behind an [`Arc`], so repeated acquisition
    /// never deep-copies item payloads.
    pub fn acquire_patch(&self, cursor: Option<PatchCursor>) -> Arc<ViewportPatch> {
        if !self.initialized {
            return Arc::new(ViewportPatch::default());
        }

        let current_cursor = self.current_cursor();
        if let Some(cursor) = cursor {
            if cursor == current_cursor {
                return Arc::new(ViewportPatch {
                    frame_view: self.frame_view,
                    decoration: noop_decoration(self.decoration_revision.0, self.decoration_view),
                    scene: noop_layer(self.scene_revision.0),
                    pen_filter_geometry_ops: Vec::new(),
                    path_geometry_ops: Vec::new(),
                    overlay: noop_layer(self.overlay_revision.0),
                });
            }

            let incremental_base = PatchCursor {
                scene_revision: SceneRevision(self.last_patch.scene.base_revision),
                decoration_revision: DecorationRevision(self.last_patch.decoration.base_revision),
                overlay_revision: OverlayRevision(self.last_patch.overlay.base_revision),
            };
            if cursor == incremental_base {
                return Arc::clone(&self.last_patch);
            }
        }

        Arc::new(self.full_reset_patch(cursor))
    }

    fn full_reset_patch(&self, cursor: Option<PatchCursor>) -> ViewportPatch {
        let cursor = cursor.unwrap_or_default();
        let scene = LayerPatch {
            base_revision: cursor.scene_revision.0,
            revision: self.scene_revision.0,
            reset: true,
            ops: vec![ReplaceRangeOp {
                start: 0,
                delete_count: 0,
                insert_items: self.scene_items.clone(),
            }],
            dirty_regions: full_surface_dirty_region(self.frame_view.surface),
        };
        let pen_filter_geometry_ops = scene
            .ops
            .iter()
            .flat_map(|operation| operation.insert_items.iter())
            .filter_map(|item| {
                let SceneDisplayItem::Filter(filter) = item else {
                    return None;
                };
                if !filter.is_pen_filter {
                    return None;
                }
                let revision = self
                    .pen_geometry_revisions
                    .get(&filter.id)
                    .copied()
                    .unwrap_or(self.scene_revision.0.max(1));
                Some(PenFilterGeometryPatch {
                    id: filter.id,
                    expected_geometry_revision: 0,
                    resulting_geometry_revision: revision,
                    retain_prefix_count: 0,
                    appended_points: filter.points.clone(),
                    old_changed_bounds: [0.0; 4],
                    new_changed_bounds: pen_tail_canvas_bounds(filter, 0),
                    full_reset: true,
                    element_removed: false,
                })
            })
            .collect();
        let path_geometry_ops = scene
            .ops
            .iter()
            .flat_map(|operation| operation.insert_items.iter())
            .filter_map(|item| {
                let SceneDisplayItem::Arrow(path) = item else {
                    return None;
                };
                Some(PathGeometryPatch {
                    id: path.id,
                    expected_geometry_revision: 0,
                    resulting_geometry_revision: path.geometry.revision,
                    ranges: vec![PathChunkReplacement {
                        start: 0,
                        delete_count: 0,
                        insert_chunks: path.geometry.chunks.to_vec(),
                    }],
                    old_changed_bounds: [0.0; 4],
                    new_changed_bounds: path.geometry.canvas_bounds,
                    closed: path.geometry.closed,
                    full_reset: true,
                    element_removed: false,
                })
            })
            .collect();
        ViewportPatch {
            frame_view: self.frame_view,
            decoration: DecorationPatch {
                base_revision: cursor.decoration_revision.0,
                revision: self.decoration_revision.0,
                reset: true,
                view: self.decoration_view,
                spotlight_ops: vec![ReplaceRangeOp {
                    start: 0,
                    delete_count: 0,
                    insert_items: self.spotlight_cutouts.clone(),
                }],
                dirty_regions: full_surface_dirty_region(self.frame_view.surface),
            },
            scene,
            pen_filter_geometry_ops,
            path_geometry_ops,
            overlay: LayerPatch {
                base_revision: cursor.overlay_revision.0,
                revision: self.overlay_revision.0,
                reset: true,
                ops: vec![ReplaceRangeOp {
                    start: 0,
                    delete_count: 0,
                    insert_items: self.overlay_items.clone(),
                }],
                dirty_regions: full_surface_dirty_region(self.frame_view.surface),
            },
        }
    }
}

fn build_path_geometry_ops(
    current_items: &[SceneDisplayItem],
    scene_patch: &LayerPatch<SceneDisplayItem>,
) -> Vec<PathGeometryPatch> {
    let mut result = Vec::new();
    for operation in &scene_patch.ops {
        let start = operation.start as usize;
        let old_items = if scene_patch.reset {
            &[][..]
        } else {
            &current_items[start.min(current_items.len())
                ..start
                    .saturating_add(operation.delete_count as usize)
                    .min(current_items.len())]
        };
        let paired = old_items.len().min(operation.insert_items.len());
        for (old, next) in old_items.iter().zip(&operation.insert_items) {
            append_path_geometry_change(&mut result, Some(old), Some(next));
        }
        for old in &old_items[paired..] {
            append_path_geometry_change(&mut result, Some(old), None);
        }
        for next in &operation.insert_items[paired..] {
            append_path_geometry_change(&mut result, None, Some(next));
        }
    }
    result
}

fn append_path_geometry_change(
    result: &mut Vec<PathGeometryPatch>,
    old_item: Option<&SceneDisplayItem>,
    next_item: Option<&SceneDisplayItem>,
) {
    let old = old_item.and_then(|item| match item {
        SceneDisplayItem::Arrow(path) => Some(path),
        _ => None,
    });
    let next = next_item.and_then(|item| match item {
        SceneDisplayItem::Arrow(path) => Some(path),
        _ => None,
    });
    match (old, next) {
        (Some(old), Some(next)) if old.id == next.id => {
            let style_unchanged = old.stroke == next.stroke
                && old.stroke_width == next.stroke_width
                && old.stroke_style == next.stroke_style
                && old.fill == next.fill
                && old.fill_style == next.fill_style
                && old.opacity == next.opacity
                && old.blend_mode == next.blend_mode;
            if old.geometry == next.geometry && style_unchanged {
                return;
            }
            let full_reset = !style_unchanged || old.geometry.revision == next.geometry.revision;
            let ranges = if full_reset {
                vec![PathChunkReplacement {
                    start: 0,
                    delete_count: old.geometry.chunks.len() as u32,
                    insert_chunks: next.geometry.chunks.to_vec(),
                }]
            } else {
                diff_path_chunks(&old.geometry.chunks, &next.geometry.chunks)
            };
            let (old_bounds, new_bounds) =
                changed_chunk_bounds(&old.geometry.chunks, &next.geometry.chunks, &ranges);
            result.push(PathGeometryPatch {
                id: next.id,
                expected_geometry_revision: old.geometry.revision,
                resulting_geometry_revision: next.geometry.revision,
                ranges,
                old_changed_bounds: if full_reset {
                    old.geometry.canvas_bounds
                } else {
                    old_bounds
                },
                new_changed_bounds: if full_reset {
                    next.geometry.canvas_bounds
                } else {
                    new_bounds
                },
                closed: next.geometry.closed,
                full_reset,
                element_removed: false,
            });
        }
        (Some(old), _) => result.push(PathGeometryPatch {
            id: old.id,
            expected_geometry_revision: old.geometry.revision,
            resulting_geometry_revision: 0,
            ranges: Vec::new(),
            old_changed_bounds: old.geometry.canvas_bounds,
            new_changed_bounds: [0.0; 4],
            closed: false,
            full_reset: false,
            element_removed: true,
        }),
        (_, Some(next)) => result.push(PathGeometryPatch {
            id: next.id,
            expected_geometry_revision: 0,
            resulting_geometry_revision: next.geometry.revision,
            ranges: vec![PathChunkReplacement {
                start: 0,
                delete_count: 0,
                insert_chunks: next.geometry.chunks.to_vec(),
            }],
            old_changed_bounds: [0.0; 4],
            new_changed_bounds: next.geometry.canvas_bounds,
            closed: next.geometry.closed,
            full_reset: true,
            element_removed: false,
        }),
        _ => {}
    }
}

fn diff_path_chunks(
    old: &[snow_draw_engine_core::PathChunk],
    next: &[snow_draw_engine_core::PathChunk],
) -> Vec<PathChunkReplacement> {
    if old.len() == next.len() {
        let mut ranges = Vec::new();
        let mut index = 0;
        while index < old.len() {
            if old[index] == next[index] {
                index += 1;
                continue;
            }
            let start = index;
            while index < old.len() && old[index] != next[index] {
                index += 1;
            }
            ranges.push(PathChunkReplacement {
                start: start as u32,
                delete_count: (index - start) as u32,
                insert_chunks: next[start..index].to_vec(),
            });
        }
        return ranges;
    }
    let prefix = old
        .iter()
        .zip(next)
        .take_while(|(left, right)| left == right)
        .count();
    let suffix = old[prefix..]
        .iter()
        .rev()
        .zip(next[prefix..].iter().rev())
        .take_while(|(left, right)| left == right)
        .count();
    vec![PathChunkReplacement {
        start: prefix as u32,
        delete_count: old.len().saturating_sub(prefix + suffix) as u32,
        insert_chunks: next[prefix..next.len().saturating_sub(suffix)].to_vec(),
    }]
}

fn changed_chunk_bounds(
    old: &[snow_draw_engine_core::PathChunk],
    _next: &[snow_draw_engine_core::PathChunk],
    ranges: &[PathChunkReplacement],
) -> ([f64; 4], [f64; 4]) {
    let mut old_bounds = None;
    let mut new_bounds = None;
    for range in ranges {
        for chunk in old
            .iter()
            .skip(range.start as usize)
            .take(range.delete_count as usize)
        {
            old_bounds = union_bounds_array(old_bounds, chunk.canvas_bounds);
        }
        for chunk in &range.insert_chunks {
            new_bounds = union_bounds_array(new_bounds, chunk.canvas_bounds);
        }
    }
    (
        old_bounds.unwrap_or([0.0; 4]),
        new_bounds.unwrap_or([0.0; 4]),
    )
}

fn union_bounds_array(current: Option<[f64; 4]>, next: [f64; 4]) -> Option<[f64; 4]> {
    Some(current.map_or(next, |current| {
        [
            current[0].min(next[0]),
            current[1].min(next[1]),
            current[2].max(next[2]),
            current[3].max(next[3]),
        ]
    }))
}

fn next_pen_geometry_revision(counter: &mut u64) -> u64 {
    *counter = counter.wrapping_add(1).max(1);
    *counter
}

fn pen_tail_canvas_bounds(
    filter: &snow_draw_engine_display::FilterDisplayItem,
    prefix: usize,
) -> [f64; 4] {
    if filter.points.is_empty() {
        return [0.0; 4];
    }
    let changed_start = prefix.saturating_sub(1).min(filter.points.len() - 1);
    let mut min_x = f64::INFINITY;
    let mut min_y = f64::INFINITY;
    let mut max_x = f64::NEG_INFINITY;
    let mut max_y = f64::NEG_INFINITY;
    for point in &filter.points[changed_start..] {
        if !point[0].is_finite() || !point[1].is_finite() {
            return [0.0; 4];
        }
        min_x = min_x.min(point[0]);
        min_y = min_y.min(point[1]);
        max_x = max_x.max(point[0]);
        max_y = max_y.max(point[1]);
    }
    let outset = (filter.stroke_width.abs() * 0.5 + 1.0).max(1.0);
    [
        min_x - outset,
        min_y - outset,
        max_x + outset,
        max_y + outset,
    ]
}

fn build_pen_filter_geometry_ops(
    current_items: &[SceneDisplayItem],
    scene_patch: &LayerPatch<SceneDisplayItem>,
    revisions: &mut HashMap<DisplayItemId, u64>,
    revision_counter: &mut u64,
) -> Vec<PenFilterGeometryPatch> {
    let mut geometry_ops = Vec::new();
    for operation in &scene_patch.ops {
        let start = operation.start as usize;
        let old_items = if scene_patch.reset {
            &[][..]
        } else {
            &current_items[start.min(current_items.len())
                ..start
                    .saturating_add(operation.delete_count as usize)
                    .min(current_items.len())]
        };
        let paired = old_items.len().min(operation.insert_items.len());
        for (old, next) in old_items.iter().zip(&operation.insert_items) {
            let old_filter = match old {
                SceneDisplayItem::Filter(filter) if filter.is_pen_filter => Some(filter),
                _ => None,
            };
            let next_filter = match next {
                SceneDisplayItem::Filter(filter) if filter.is_pen_filter => Some(filter),
                _ => None,
            };
            if let (Some(old), Some(next)) = (old_filter, next_filter)
                && old.id == next.id
            {
                let geometry_style_unchanged =
                    old.stroke_width == next.stroke_width && old.rotation == next.rotation;
                if old.points == next.points && geometry_style_unchanged {
                    revisions
                        .entry(next.id)
                        .or_insert_with(|| next_pen_geometry_revision(revision_counter));
                    continue;
                }
                let prefix = if geometry_style_unchanged {
                    old.points
                        .iter()
                        .zip(next.points.iter())
                        .take_while(|(left, right)| left == right)
                        .count()
                } else {
                    0
                };
                let expected = revisions.get(&old.id).copied().unwrap_or(0);
                let resulting = next_pen_geometry_revision(revision_counter);
                revisions.insert(next.id, resulting);
                geometry_ops.push(PenFilterGeometryPatch {
                    id: next.id,
                    expected_geometry_revision: expected,
                    resulting_geometry_revision: resulting,
                    retain_prefix_count: prefix as u32,
                    appended_points: next.points[prefix..].to_vec().into(),
                    old_changed_bounds: pen_tail_canvas_bounds(old, prefix),
                    new_changed_bounds: pen_tail_canvas_bounds(next, prefix),
                    full_reset: !geometry_style_unchanged,
                    element_removed: false,
                });
                continue;
            }
            if let Some(old) = old_filter {
                geometry_ops.push(PenFilterGeometryPatch {
                    id: old.id,
                    expected_geometry_revision: revisions.remove(&old.id).unwrap_or(0),
                    resulting_geometry_revision: 0,
                    retain_prefix_count: 0,
                    appended_points: Arc::from([]),
                    old_changed_bounds: pen_tail_canvas_bounds(old, 0),
                    new_changed_bounds: [0.0; 4],
                    full_reset: false,
                    element_removed: true,
                });
            }
            if let Some(next) = next_filter {
                let resulting = next_pen_geometry_revision(revision_counter);
                revisions.insert(next.id, resulting);
                geometry_ops.push(PenFilterGeometryPatch {
                    id: next.id,
                    expected_geometry_revision: 0,
                    resulting_geometry_revision: resulting,
                    retain_prefix_count: 0,
                    appended_points: next.points.clone(),
                    old_changed_bounds: [0.0; 4],
                    new_changed_bounds: pen_tail_canvas_bounds(next, 0),
                    full_reset: true,
                    element_removed: false,
                });
            }
        }
        for old in &old_items[paired..] {
            if let SceneDisplayItem::Filter(old) = old
                && old.is_pen_filter
            {
                geometry_ops.push(PenFilterGeometryPatch {
                    id: old.id,
                    expected_geometry_revision: revisions.remove(&old.id).unwrap_or(0),
                    resulting_geometry_revision: 0,
                    retain_prefix_count: 0,
                    appended_points: Arc::from([]),
                    old_changed_bounds: pen_tail_canvas_bounds(old, 0),
                    new_changed_bounds: [0.0; 4],
                    full_reset: false,
                    element_removed: true,
                });
            }
        }
        for next in &operation.insert_items[paired..] {
            if let SceneDisplayItem::Filter(next) = next
                && next.is_pen_filter
            {
                let resulting = next_pen_geometry_revision(revision_counter);
                revisions.insert(next.id, resulting);
                geometry_ops.push(PenFilterGeometryPatch {
                    id: next.id,
                    expected_geometry_revision: 0,
                    resulting_geometry_revision: resulting,
                    retain_prefix_count: 0,
                    appended_points: next.points.clone(),
                    old_changed_bounds: [0.0; 4],
                    new_changed_bounds: pen_tail_canvas_bounds(next, 0),
                    full_reset: true,
                    element_removed: false,
                });
            }
        }
    }
    geometry_ops
}

fn display_decoration(
    model: &DocumentModel,
    presentation: &EditorPresentationState,
) -> DecorationView {
    let config = model.watermark_config();
    let mut out = snow_draw_engine_display::DisplayWatermarkConfig {
        color: config.color,
        font_size: config.font_size,
        angle: config.angle,
        gap: config.gap,
        opacity: config.opacity,
        ..snow_draw_engine_display::DisplayWatermarkConfig::default()
    };
    let mut text_len = config.text.len().min(out.text.len());
    while text_len > 0 && !config.text.is_char_boundary(text_len) {
        text_len -= 1;
    }
    out.text[..text_len].copy_from_slice(&config.text.as_bytes()[..text_len]);
    out.text_len = text_len as u16;
    let mut family_len = config.font_family.len().min(out.font_family.len());
    while family_len > 0 && !config.font_family.is_char_boundary(family_len) {
        family_len -= 1;
    }
    out.font_family[..family_len].copy_from_slice(&config.font_family.as_bytes()[..family_len]);
    out.font_family_len = family_len as u16;
    let spotlight_config = model.spotlight_config();
    let preview_active = presentation.creation_preview.as_ref().is_some_and(
        |preview| matches!(preview, ElementCreationPreview::Rectangle(rect) if rect.is_spotlight()),
    );
    DecorationView {
        watermark: out,
        spotlight: snow_draw_engine_display::DisplaySpotlightConfig {
            color: spotlight_config.color,
            opacity: spotlight_config.opacity,
            active: model.has_visible_spotlight() || preview_active,
        },
    }
}

fn spotlight_cutout(rect: RectangleData) -> DisplaySpotlightCutout {
    DisplaySpotlightCutout {
        center_x: rect.center.x,
        center_y: rect.center.y,
        width: rect.width,
        height: rect.height,
        rotation: rect.rotation,
    }
}

fn compose_spotlight_cutouts(
    cache: &DocumentSceneCache,
    model: &DocumentModel,
    presentation: &EditorPresentationState,
    frame_view: FrameView,
) -> Vec<DisplaySpotlightCutout> {
    let viewport = canvas_viewport(frame_view.camera, frame_view.surface);
    let previews = presentation
        .preview_elements
        .iter()
        .map(|preview| (preview.id, preview.rect))
        .collect::<std::collections::HashMap<_, _>>();
    let mut cutouts = Vec::new();

    for state in model.element_states() {
        let ElementData::Rectangle(committed) = state.data else {
            continue;
        };
        if !state.visible || !committed.is_spotlight() {
            continue;
        }
        if let Some(preview) = previews.get(&state.id) {
            if bounds_visible(rect_bounds(*preview), viewport) {
                cutouts.push(spotlight_cutout(*preview));
            }
        } else if bounds_visible(rect_bounds(*committed), viewport)
            && let Some(cutout) = cache.spotlight_entry(state.id)
        {
            cutouts.push(cutout);
        }
    }

    if let Some(ElementCreationPreview::Rectangle(rect)) = presentation.creation_preview.as_ref()
        && rect.is_spotlight()
        && bounds_visible(rect_bounds(*rect), viewport)
    {
        cutouts.push(spotlight_cutout(*rect));
    }
    cutouts
}

fn noop_decoration(revision: u64, view: DecorationView) -> DecorationPatch {
    DecorationPatch {
        base_revision: revision,
        revision,
        reset: false,
        view,
        spotlight_ops: Vec::new(),
        dirty_regions: Vec::new(),
    }
}

struct DecorationPatchOptions {
    frame_view: FrameView,
    force_reset: bool,
    force_full_dirty: bool,
}

fn build_decoration_patch(
    current: &DecorationView,
    current_cutouts: &[DisplaySpotlightCutout],
    current_revision: u64,
    next: DecorationView,
    next_cutouts: &[DisplaySpotlightCutout],
    options: DecorationPatchOptions,
) -> (DecorationPatch, u64) {
    if options.force_reset {
        let revision = current_revision.wrapping_add(1);
        return (
            DecorationPatch {
                base_revision: current_revision,
                revision,
                reset: true,
                view: next,
                spotlight_ops: vec![ReplaceRangeOp {
                    start: 0,
                    delete_count: current_cutouts.len() as u32,
                    insert_items: next_cutouts.to_vec(),
                }],
                dirty_regions: full_surface_dirty_region(options.frame_view.surface),
            },
            revision,
        );
    }

    if *current == next && current_cutouts == next_cutouts && !options.force_full_dirty {
        return (noop_decoration(current_revision, next), current_revision);
    }

    let revision = current_revision.wrapping_add(1);
    let style_changed = *current != next;
    let cutout_patch = build_spotlight_ops(current_cutouts, next_cutouts);
    let mut geometry_dirty = Vec::new();
    if let Some(op) = cutout_patch.first() {
        let old_start = (op.start as usize).min(current_cutouts.len());
        let old_end = old_start
            .saturating_add(op.delete_count as usize)
            .min(current_cutouts.len());
        for cutout in current_cutouts[old_start..old_end]
            .iter()
            .chain(op.insert_items.iter())
        {
            if let Some(bounds) = draw_rect_bounds(
                options.frame_view,
                cutout.center_x,
                cutout.center_y,
                cutout.width,
                cutout.height,
                cutout.rotation,
                0.0,
            ) {
                geometry_dirty.push(bounds);
            }
        }
    }
    (
        DecorationPatch {
            base_revision: current_revision,
            revision,
            reset: false,
            view: next,
            spotlight_ops: cutout_patch,
            dirty_regions: if options.force_full_dirty || style_changed {
                full_surface_dirty_region(options.frame_view.surface)
            } else {
                finalize_dirty_regions(geometry_dirty, options.frame_view.surface)
            },
        },
        revision,
    )
}

fn build_spotlight_ops(
    current: &[DisplaySpotlightCutout],
    next: &[DisplaySpotlightCutout],
) -> Vec<ReplaceRangeOp<DisplaySpotlightCutout>> {
    if current == next {
        return Vec::new();
    }
    let prefix = current
        .iter()
        .zip(next)
        .take_while(|(left, right)| left == right)
        .count();
    let max_suffix = current.len().min(next.len()).saturating_sub(prefix);
    let suffix = current[prefix..]
        .iter()
        .rev()
        .zip(next[prefix..].iter().rev())
        .take(max_suffix)
        .take_while(|(left, right)| left == right)
        .count();
    let current_end = current.len().saturating_sub(suffix);
    let next_end = next.len().saturating_sub(suffix);
    vec![ReplaceRangeOp {
        start: prefix as u32,
        delete_count: (current_end - prefix) as u32,
        insert_items: next[prefix..next_end].to_vec(),
    }]
}

fn noop_layer<T>(revision: u64) -> LayerPatch<T> {
    LayerPatch {
        base_revision: revision,
        revision,
        reset: false,
        ops: Vec::new(),
        dirty_regions: Vec::new(),
    }
}

fn intersect_dirty_regions(left: DirtyRegion, right: DirtyRegion) -> Option<DirtyRegion> {
    let intersection = DirtyRegion::new(
        left.min_x.max(right.min_x),
        left.min_y.max(right.min_y),
        left.max_x.min(right.max_x),
        left.max_y.min(right.max_y),
    );
    (!intersection.is_empty()).then_some(intersection)
}

fn filter_sampling_radius(
    item: &snow_draw_engine_display::FilterDisplayItem,
    frame: FrameView,
) -> f64 {
    item.filter.sampling_radius * frame.camera.zoom.max(0.0)
}

fn dirty_region_through_filters(
    items: &[SceneDisplayItem],
    changed_index: usize,
    base: DirtyRegion,
    frame: FrameView,
) -> DirtyRegion {
    let mut affected = base;
    let mut index = changed_index.saturating_add(1);
    while index < items.len() {
        if !matches!(items[index], SceneDisplayItem::Filter(_)) {
            index += 1;
            continue;
        }
        let entering = affected;
        while index < items.len() {
            let SceneDisplayItem::Filter(filter) = &items[index] else {
                break;
            };
            if filter.opacity > 0.0
                && let Some(filter_bounds) = scene_display_item_bounds(&items[index], frame)
            {
                let radius = filter_sampling_radius(filter, frame);
                let expanded = DirtyRegion::new(
                    entering.min_x - radius,
                    entering.min_y - radius,
                    entering.max_x + radius,
                    entering.max_y + radius,
                );
                if let Some(filtered) = intersect_dirty_regions(expanded, filter_bounds) {
                    affected = affected.union(filtered);
                }
            }
            index += 1;
        }
    }
    affected
}

fn pen_filter_tail_dirty_region(
    old_item: &SceneDisplayItem,
    new_item: &SceneDisplayItem,
    frame: FrameView,
) -> Option<DirtyRegion> {
    let (SceneDisplayItem::Filter(old), SceneDisplayItem::Filter(new)) = (old_item, new_item)
    else {
        return None;
    };
    if !old.is_pen_filter
        || !new.is_pen_filter
        || old.id != new.id
        || old.filter != new.filter
        || old.stroke_width != new.stroke_width
        || old.opacity != new.opacity
        || old.rotation != new.rotation
        || old.points.len() < 2
        || new.points.len() < 2
    {
        return None;
    }

    let common_prefix = old
        .points
        .iter()
        .zip(new.points.iter())
        .take_while(|(left, right)| left == right)
        .count();
    if common_prefix == 0
        || (common_prefix == old.points.len() && old.points.len() == new.points.len())
    {
        return None;
    }

    // Include the segment entering the changed tail. Its endpoint is stable,
    // but its coverage changes when provisional tail vertices are replaced.
    let changed_start = common_prefix.saturating_sub(1);
    let mut min_x = f64::INFINITY;
    let mut min_y = f64::INFINITY;
    let mut max_x = f64::NEG_INFINITY;
    let mut max_y = f64::NEG_INFINITY;
    for point in old.points[changed_start..]
        .iter()
        .chain(new.points[changed_start..].iter())
    {
        if !point[0].is_finite() || !point[1].is_finite() {
            return None;
        }
        let view = canvas_point_to_surface(frame, Point::new(point[0], point[1]));
        min_x = min_x.min(view.x);
        min_y = min_y.min(view.y);
        max_x = max_x.max(view.x);
        max_y = max_y.max(view.y);
    }
    let outset = old.stroke_width * frame.camera.zoom.max(0.0) / 2.0 + 2.0;
    outset.is_finite().then_some(DirtyRegion::new(
        min_x - outset,
        min_y - outset,
        max_x + outset,
        max_y + outset,
    ))
}

fn path_geometry_dirty_region(
    old_item: &SceneDisplayItem,
    new_item: &SceneDisplayItem,
    frame: FrameView,
) -> Option<DirtyRegion> {
    let (SceneDisplayItem::Arrow(old), SceneDisplayItem::Arrow(new)) = (old_item, new_item) else {
        return None;
    };
    let style_unchanged = old.id == new.id
        && old.stroke == new.stroke
        && old.stroke_width == new.stroke_width
        && old.stroke_style == new.stroke_style
        && old.fill == new.fill
        && old.fill_style == new.fill_style
        && old.opacity == new.opacity
        && old.blend_mode == new.blend_mode;
    if !style_unchanged
        || old.geometry == new.geometry
        || old.geometry.revision == new.geometry.revision
        || ((old.geometry.closed || new.geometry.closed) && (old.fill.a != 0 || new.fill.a != 0))
    {
        return None;
    }

    let ranges = diff_path_chunks(&old.geometry.chunks, &new.geometry.chunks);
    if ranges.is_empty() {
        return None;
    }
    let mut bounds = None;
    for range in &ranges {
        for chunk in old
            .geometry
            .chunks
            .iter()
            .skip(range.start as usize)
            .take(range.delete_count as usize)
        {
            bounds = union_bounds_array(bounds, chunk.canvas_bounds);
        }
        for chunk in &range.insert_chunks {
            bounds = union_bounds_array(bounds, chunk.canvas_bounds);
        }
    }
    if old.arrowhead_primitives != new.arrowhead_primitives {
        for primitive in old
            .arrowhead_primitives
            .iter()
            .chain(new.arrowhead_primitives.iter())
        {
            for point in &primitive.points {
                bounds = union_bounds_array(bounds, [point[0], point[1], point[0], point[1]]);
            }
            if primitive.diameter > 0.0 {
                let radius = primitive.diameter * 0.5;
                bounds = union_bounds_array(
                    bounds,
                    [
                        primitive.center[0] - radius,
                        primitive.center[1] - radius,
                        primitive.center[0] + radius,
                        primitive.center[1] + radius,
                    ],
                );
            }
        }
    }
    let bounds = bounds?;
    if bounds.iter().any(|value| !value.is_finite()) {
        return None;
    }
    let outset = old.stroke_width.max(0.0) * 0.5;
    let top_left =
        canvas_point_to_surface(frame, Point::new(bounds[0] - outset, bounds[1] - outset));
    let bottom_right =
        canvas_point_to_surface(frame, Point::new(bounds[2] + outset, bounds[3] + outset));
    Some(DirtyRegion::new(
        top_left.x.min(bottom_right.x) - 2.0,
        top_left.y.min(bottom_right.y) - 2.0,
        top_left.x.max(bottom_right.x) + 2.0,
        top_left.y.max(bottom_right.y) + 2.0,
    ))
}

fn build_scene_layer_patch(
    current_items: &[SceneDisplayItem],
    current_revision: u64,
    next_items: &[SceneDisplayItem],
    frame_view: FrameView,
    force_reset: bool,
    force_full_dirty: bool,
) -> (LayerPatch<SceneDisplayItem>, u64) {
    let (mut patch, revision) = build_layer_patch(
        current_items,
        current_revision,
        next_items,
        frame_view,
        force_reset,
        force_full_dirty,
        scene_display_item_bounds,
    );
    if force_reset || force_full_dirty || patch.ops.len() != 1 {
        return (patch, revision);
    }
    let operation = &patch.ops[0];
    let start = operation.start as usize;
    let old_end = start
        .saturating_add(operation.delete_count as usize)
        .min(current_items.len());
    let new_end = start
        .saturating_add(operation.insert_items.len())
        .min(next_items.len());
    if operation.delete_count == 1
        && operation.insert_items.len() == 1
        && let Some(delta) =
            path_geometry_dirty_region(&current_items[start], &next_items[start], frame_view)
    {
        patch.dirty_regions = finalize_dirty_regions(
            vec![dirty_region_through_filters(
                next_items, start, delta, frame_view,
            )],
            frame_view.surface,
        );
        return (patch, revision);
    }
    if operation.delete_count == 1
        && operation.insert_items.len() == 1
        && let Some(delta) =
            pen_filter_tail_dirty_region(&current_items[start], &next_items[start], frame_view)
    {
        patch.dirty_regions = finalize_dirty_regions(
            vec![dirty_region_through_filters(
                next_items, start, delta, frame_view,
            )],
            frame_view.surface,
        );
        return (patch, revision);
    }
    let mut dirty = Vec::new();
    for (index, item) in current_items.iter().enumerate().take(old_end).skip(start) {
        if let Some(bounds) = scene_display_item_bounds(item, frame_view) {
            dirty.push(dirty_region_through_filters(
                current_items,
                index,
                bounds,
                frame_view,
            ));
        }
    }
    for (index, item) in next_items.iter().enumerate().take(new_end).skip(start) {
        if let Some(bounds) = scene_display_item_bounds(item, frame_view) {
            dirty.push(dirty_region_through_filters(
                next_items, index, bounds, frame_view,
            ));
        }
    }
    patch.dirty_regions = finalize_dirty_regions(dirty, frame_view.surface);
    (patch, revision)
}

fn build_layer_patch<T: Clone + PartialEq>(
    current_items: &[T],
    current_revision: u64,
    next_items: &[T],
    frame_view: FrameView,
    force_reset: bool,
    force_full_dirty: bool,
    bounds_fn: impl Fn(&T, FrameView) -> Option<DirtyRegion>,
) -> (LayerPatch<T>, u64) {
    if force_reset {
        let revision = current_revision.wrapping_add(1);
        return (
            LayerPatch {
                base_revision: current_revision,
                revision,
                reset: true,
                ops: vec![ReplaceRangeOp {
                    start: 0,
                    delete_count: current_items.len() as u32,
                    insert_items: next_items.to_vec(),
                }],
                dirty_regions: full_surface_dirty_region(frame_view.surface),
            },
            revision,
        );
    }

    if current_items == next_items && !force_full_dirty {
        return (noop_layer(current_revision), current_revision);
    }

    let revision = current_revision.wrapping_add(1);
    if current_items == next_items {
        return (
            LayerPatch {
                base_revision: current_revision,
                revision,
                reset: false,
                ops: Vec::new(),
                dirty_regions: full_surface_dirty_region(frame_view.surface),
            },
            revision,
        );
    }
    let prefix = current_items
        .iter()
        .zip(next_items.iter())
        .take_while(|(left, right)| left == right)
        .count();
    let max_suffix = current_items
        .len()
        .min(next_items.len())
        .saturating_sub(prefix);
    let suffix = current_items[prefix..]
        .iter()
        .rev()
        .zip(next_items[prefix..].iter().rev())
        .take(max_suffix)
        .take_while(|(left, right)| left == right)
        .count();

    let current_end = current_items.len().saturating_sub(suffix);
    let next_end = next_items.len().saturating_sub(suffix);
    let mut dirty = Vec::new();
    for item in &current_items[prefix..current_end] {
        if let Some(bounds) = bounds_fn(item, frame_view) {
            dirty.push(bounds);
        }
    }
    for item in &next_items[prefix..next_end] {
        if let Some(bounds) = bounds_fn(item, frame_view) {
            dirty.push(bounds);
        }
    }

    (
        LayerPatch {
            base_revision: current_revision,
            revision,
            reset: false,
            ops: vec![ReplaceRangeOp {
                start: prefix as u32,
                delete_count: (current_end - prefix) as u32,
                insert_items: next_items[prefix..next_end].to_vec(),
            }],
            dirty_regions: if force_full_dirty {
                full_surface_dirty_region(frame_view.surface)
            } else {
                finalize_dirty_regions(dirty, frame_view.surface)
            },
        },
        revision,
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::{Camera, PathCommand, PathGeometry, SurfaceSize};
    use snow_draw_engine_document::{ElementMeta, Transaction, WatermarkConfig};

    fn frame_view() -> FrameView {
        FrameView {
            surface: SurfaceSize {
                width: 1000,
                height: 1000,
            },
            camera: Camera {
                center: Point::new(0.0, 0.0),
                zoom: 1.0,
            },
            clear_color: ColorRgba8::default(),
        }
    }

    fn insert_bound_serial_and_text(
        model: &mut DocumentModel,
        serial_id: ElementId,
        text_id: ElementId,
        text_center: Point<f64>,
    ) {
        let serial = SerialNumberData {
            center: Point::new(0.0, 0.0),
            diameter: 24.0,
            text_element_id: Some(text_id),
            ..SerialNumberData::default()
        };
        let text = TextData {
            center: text_center,
            width: 40.0,
            height: 20.0,
            ..TextData::default()
        };
        let mut transaction = Transaction::new("setup bound serial");
        transaction.insert_serial_number(serial_id, ElementMeta::default(), serial);
        transaction.insert_text(text_id, ElementMeta::default(), text);
        model.apply_transaction(transaction).unwrap();
    }

    fn connector_bounds(items: &[SceneDisplayItem], frame_view: FrameView) -> DirtyRegion {
        let connector = items
            .iter()
            .find(|item| matches!(item, SceneDisplayItem::SerialNumberConnector(_)))
            .expect("scene should include a serial connector");
        scene_display_item_bounds(connector, frame_view)
            .expect("serial connector should have drawable bounds")
    }

    fn dirty_regions_cover(regions: &[DirtyRegion], target: DirtyRegion) -> bool {
        const EPSILON: f64 = 1e-9;
        regions.iter().any(|region| {
            region.min_x <= target.min_x + EPSILON
                && region.min_y <= target.min_y + EPSILON
                && region.max_x + EPSILON >= target.max_x
                && region.max_y + EPSILON >= target.max_y
        })
    }

    fn watermark_config(text: &str) -> WatermarkConfig {
        WatermarkConfig {
            text: text.to_owned(),
            ..WatermarkConfig::default()
        }
    }

    fn spotlight_rect(center: Point<f64>, width: f64, height: f64) -> RectangleData {
        RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center,
            width,
            height,
            rotation: 0.0,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        }
        .into_spotlight()
    }

    fn assert_layer_noop<T>(patch: &LayerPatch<T>) {
        assert!(!patch.reset);
        assert_eq!(patch.base_revision, patch.revision);
        assert!(patch.ops.is_empty());
        assert!(patch.dirty_regions.is_empty());
    }

    fn assert_decoration_noop(patch: &ViewportPatch) {
        assert!(!patch.decoration.reset);
        assert_eq!(patch.decoration.base_revision, patch.decoration.revision);
        assert!(patch.decoration.spotlight_ops.is_empty());
        assert!(patch.decoration.dirty_regions.is_empty());
    }

    fn pen_filter_display(points: Vec<[f64; 2]>, opacity: f64) -> SceneDisplayItem {
        let min_x = points
            .iter()
            .map(|point| point[0])
            .fold(f64::INFINITY, f64::min);
        let max_x = points
            .iter()
            .map(|point| point[0])
            .fold(f64::NEG_INFINITY, f64::max);
        let min_y = points
            .iter()
            .map(|point| point[1])
            .fold(f64::INFINITY, f64::min);
        let max_y = points
            .iter()
            .map(|point| point[1])
            .fold(f64::NEG_INFINITY, f64::max);
        SceneDisplayItem::Filter(snow_draw_engine_display::FilterDisplayItem {
            id: DisplayItemId {
                index: 77,
                generation: 2,
            },
            center_x: (min_x + max_x) / 2.0,
            center_y: (min_y + max_y) / 2.0,
            width: max_x - min_x,
            height: max_y - min_y,
            rotation: 0.0,
            points: points.into(),
            stroke_width: 30.0,
            is_pen_filter: true,
            filter: snow_draw_engine_display::FilterRenderSpec::resolve(
                snow_draw_engine_display::DisplayFilterType::Inversion,
                1.0,
            ),
            opacity,
        })
    }

    fn long_path_display(revision: u64, tail_x: f64) -> SceneDisplayItem {
        let mut commands = vec![PathCommand::MoveTo {
            point: [-400.0, 0.0],
        }];
        for index in 1..=128 {
            commands.push(PathCommand::LineTo {
                point: [-400.0 + 400.0 * index as f64 / 128.0, 0.0],
            });
        }
        commands.push(PathCommand::LineTo {
            point: [tail_x, 4.0],
        });
        let geometry = Arc::new(PathGeometry::from_commands(
            revision,
            commands.clone(),
            false,
        ));
        SceneDisplayItem::Arrow(ArrowDisplayItem {
            id: DisplayItemId {
                index: 91,
                generation: 3,
            },
            points: vec![[-400.0, 0.0], [tail_x, 4.0]],
            path_commands: commands,
            geometry,
            arrow_type: ArrowType::Straight,
            start_arrowhead: None,
            end_arrowhead: None,
            stroke: ColorRgba8 {
                r: 0,
                g: 0,
                b: 0,
                a: 255,
            },
            stroke_width: 4.0,
            stroke_style: StrokeStyle::Solid,
            fill: ColorRgba8::default(),
            fill_style: DisplayFillStyle::Solid,
            arrowhead_primitives: Vec::new(),
            opacity: 1.0,
            is_free_draw: true,
            blend_mode: snow_draw_engine_display::DisplayBlendMode::Normal,
        })
    }

    #[test]
    fn late_path_append_dirties_only_changed_chunks() {
        let old = long_path_display(1, 10.0);
        let new = long_path_display(2, 24.0);
        let current = vec![old];
        let next = vec![new];
        let (patch, _) = build_scene_layer_patch(&current, 4, &next, frame_view(), false, false);
        let geometry = build_path_geometry_ops(&current, &patch);

        assert_eq!(geometry.len(), 1);
        assert!(!geometry[0].full_reset);
        assert_eq!(geometry[0].ranges.len(), 1);
        assert_eq!(geometry[0].ranges[0].start, 2);
        assert_eq!(patch.dirty_regions.len(), 1);
        let dirty = patch.dirty_regions[0];
        assert!(
            dirty.min_x >= 492.0,
            "unchanged path prefix was invalidated: {dirty:?}"
        );
        assert!(dirty.max_x >= 528.0);
    }

    fn patch_for_creation_preview(preview: ElementCreationPreview) -> Arc<ViewportPatch> {
        let model = DocumentModel::new();
        let cache = DocumentSceneCache::new();
        let frame_view = frame_view();
        let mut composer = ViewportComposer::new();
        composer.refresh_with_presentation(
            &cache,
            &model,
            frame_view,
            &EditorPresentationState::default(),
            SnapConfig::default(),
        );
        let cursor = composer.current_cursor();
        composer.refresh_with_presentation(
            &cache,
            &model,
            frame_view,
            &EditorPresentationState {
                creation_preview: Some(preview),
                ..EditorPresentationState::default()
            },
            SnapConfig::default(),
        );
        composer.acquire_patch(Some(cursor))
    }

    fn inserted_scene_item(patch: &ViewportPatch) -> &SceneDisplayItem {
        assert_eq!(patch.scene.ops.len(), 1);
        assert_eq!(patch.scene.ops[0].insert_items.len(), 1);
        assert!(patch.scene.revision > patch.scene.base_revision);
        assert_layer_noop(&patch.overlay);
        assert_decoration_noop(patch);
        &patch.scene.ops[0].insert_items[0]
    }

    #[test]
    fn late_pen_filter_append_dirties_only_the_join_and_appended_tail() {
        let old = pen_filter_display(vec![[-400.0, 0.0], [0.0, 0.0]], 1.0);
        let new = pen_filter_display(
            vec![[-400.0, 0.0], [0.0, 0.0], [10.0, 4.0], [20.0, 0.0]],
            1.0,
        );
        let current = vec![old];
        let next = vec![new];
        let (patch, _) = build_scene_layer_patch(&current, 4, &next, frame_view(), false, false);
        let geometry = build_pen_filter_geometry_ops(&current, &patch, &mut HashMap::new(), &mut 0);

        assert_eq!(patch.ops.len(), 1);
        assert_eq!(geometry.len(), 1);
        assert!(!geometry[0].full_reset);
        assert_eq!(geometry[0].retain_prefix_count, 2);
        assert_eq!(
            geometry[0].appended_points.as_ref(),
            &[[10.0, 4.0], [20.0, 0.0]]
        );
        assert_eq!(patch.dirty_regions.len(), 1);
        let dirty = patch.dirty_regions[0];
        assert!(
            dirty.min_x >= 483.0,
            "unchanged prefix was invalidated: {dirty:?}"
        );
        assert!(dirty.max_x >= 537.0);
        assert!(dirty.min_y <= 483.0 && dirty.max_y >= 517.0);
    }

    #[test]
    fn late_pen_filter_tail_replacement_preserves_prefix_dirty_bounds() {
        let old = pen_filter_display(
            vec![[-400.0, 0.0], [0.0, 0.0], [10.0, 8.0], [20.0, 4.0]],
            1.0,
        );
        let new = pen_filter_display(
            vec![[-400.0, 0.0], [0.0, 0.0], [12.0, -6.0], [24.0, 0.0]],
            1.0,
        );
        let current = vec![old];
        let next = vec![new];
        let (patch, _) = build_scene_layer_patch(&current, 4, &next, frame_view(), false, false);
        let geometry = build_pen_filter_geometry_ops(&current, &patch, &mut HashMap::new(), &mut 0);

        assert_eq!(geometry[0].retain_prefix_count, 2);
        assert_eq!(
            geometry[0].appended_points.as_ref(),
            &[[12.0, -6.0], [24.0, 0.0]]
        );
        let dirty = patch.dirty_regions[0];
        assert!(
            dirty.min_x >= 483.0,
            "unchanged prefix was invalidated: {dirty:?}"
        );
        assert!(dirty.max_x >= 541.0);
        assert!(dirty.min_y <= 481.0 && dirty.max_y >= 525.0);
    }

    #[test]
    fn pen_geometry_patches_use_monotonic_revisions_and_report_removal() {
        let first = pen_filter_display(vec![[-40.0, 0.0], [0.0, 0.0]], 1.0);
        let second = pen_filter_display(vec![[-40.0, 0.0], [0.0, 0.0], [10.0, 4.0]], 1.0);
        let third = pen_filter_display(vec![[-40.0, 0.0], [0.0, 0.0], [12.0, -3.0]], 1.0);
        let id = match &first {
            SceneDisplayItem::Filter(filter) => filter.id,
            _ => unreachable!(),
        };
        let mut revisions = HashMap::from([(id, 40)]);
        let mut counter = 40;
        let (append_patch, _) = build_scene_layer_patch(
            std::slice::from_ref(&first),
            4,
            std::slice::from_ref(&second),
            frame_view(),
            false,
            false,
        );
        let append =
            build_pen_filter_geometry_ops(&[first], &append_patch, &mut revisions, &mut counter);
        assert_eq!(append.len(), 1);
        assert_eq!(append[0].expected_geometry_revision, 40);
        assert_eq!(append[0].resulting_geometry_revision, 41);
        assert_eq!(append[0].retain_prefix_count, 2);
        assert!(!append[0].full_reset && !append[0].element_removed);
        assert!(append[0].old_changed_bounds[0] < append[0].old_changed_bounds[2]);
        assert!(append[0].new_changed_bounds[0] < append[0].new_changed_bounds[2]);

        let (replace_patch, _) = build_scene_layer_patch(
            std::slice::from_ref(&second),
            5,
            std::slice::from_ref(&third),
            frame_view(),
            false,
            false,
        );
        let replace =
            build_pen_filter_geometry_ops(&[second], &replace_patch, &mut revisions, &mut counter);
        assert_eq!(replace[0].expected_geometry_revision, 41);
        assert_eq!(replace[0].resulting_geometry_revision, 42);
        assert_eq!(replace[0].retain_prefix_count, 2);

        let removal_patch = LayerPatch {
            base_revision: 6,
            revision: 7,
            reset: false,
            ops: vec![ReplaceRangeOp {
                start: 0,
                delete_count: 1,
                insert_items: Vec::new(),
            }],
            dirty_regions: Vec::new(),
        };
        let removal =
            build_pen_filter_geometry_ops(&[third], &removal_patch, &mut revisions, &mut counter);
        assert_eq!(removal.len(), 1);
        assert!(removal[0].element_removed);
        assert_eq!(removal[0].expected_geometry_revision, 42);
        assert_eq!(removal[0].resulting_geometry_revision, 0);
        assert!(!revisions.contains_key(&id));
    }

    #[test]
    fn pen_geometry_style_change_forces_a_full_reset() {
        let old = pen_filter_display(vec![[-40.0, 0.0], [40.0, 0.0]], 1.0);
        let mut next = old.clone();
        let SceneDisplayItem::Filter(filter) = &mut next else {
            unreachable!();
        };
        filter.stroke_width *= 2.0;
        let id = filter.id;
        let (patch, _) = build_scene_layer_patch(
            std::slice::from_ref(&old),
            8,
            &[next],
            frame_view(),
            false,
            false,
        );
        let mut revisions = HashMap::from([(id, 90)]);
        let geometry = build_pen_filter_geometry_ops(&[old], &patch, &mut revisions, &mut 90);
        assert_eq!(geometry.len(), 1);
        assert!(geometry[0].full_reset);
        assert_eq!(geometry[0].retain_prefix_count, 0);
        assert_eq!(geometry[0].expected_geometry_revision, 90);
        assert_eq!(geometry[0].resulting_geometry_revision, 91);
    }

    #[test]
    fn pen_filter_style_change_uses_full_old_and_new_bounds() {
        let old = pen_filter_display(vec![[-400.0, 0.0], [0.0, 0.0]], 1.0);
        let new = pen_filter_display(vec![[-400.0, 0.0], [0.0, 0.0], [20.0, 0.0]], 0.5);
        let (patch, _) = build_scene_layer_patch(&[old], 4, &[new], frame_view(), false, false);

        assert!(patch.dirty_regions.iter().any(|dirty| dirty.min_x <= 85.0));
    }

    #[test]
    fn filter_above_pen_append_expands_the_tail_delta_without_touching_prefix() {
        let old_pen = pen_filter_display(vec![[-400.0, 0.0], [0.0, 0.0]], 1.0);
        let new_pen = pen_filter_display(vec![[-400.0, 0.0], [0.0, 0.0], [20.0, 0.0]], 1.0);
        let above = SceneDisplayItem::Filter(snow_draw_engine_display::FilterDisplayItem {
            id: DisplayItemId {
                index: 78,
                generation: 2,
            },
            center_x: 10.0,
            center_y: 0.0,
            width: 200.0,
            height: 200.0,
            filter: snow_draw_engine_display::FilterRenderSpec::resolve(
                snow_draw_engine_display::DisplayFilterType::GaussianBlur,
                1.0,
            ),
            opacity: 1.0,
            ..snow_draw_engine_display::FilterDisplayItem::default()
        });
        let (patch, _) = build_scene_layer_patch(
            &[old_pen, above.clone()],
            4,
            &[new_pen, above],
            frame_view(),
            false,
            false,
        );
        let dirty = patch.dirty_regions[0];
        assert!(dirty.min_x < 483.0);
        assert!(
            dirty.min_x > 300.0,
            "filter propagation reached unchanged prefix: {dirty:?}"
        );
    }

    #[test]
    fn watermark_only_commit_advances_only_decoration_channel() {
        let mut model = DocumentModel::new();
        let cache = DocumentSceneCache::new();
        let frame = frame_view();
        let presentation = EditorPresentationState::default();
        let mut composer = ViewportComposer::new();
        composer.refresh_with_presentation(
            &cache,
            &model,
            frame,
            &presentation,
            SnapConfig::default(),
        );
        let cursor = composer.current_cursor();

        let mut transaction = Transaction::new("watermark only");
        transaction.update_watermark(watermark_config("DECORATION"));
        model.apply_transaction(transaction).unwrap();
        composer.refresh_with_presentation(
            &cache,
            &model,
            frame,
            &presentation,
            SnapConfig::default(),
        );
        let patch = composer.acquire_patch(Some(cursor));

        assert_layer_noop(&patch.scene);
        assert_layer_noop(&patch.overlay);
        assert!(!patch.decoration.reset);
        assert!(patch.decoration.revision > patch.decoration.base_revision);
        assert_eq!(patch.decoration.view.watermark.text_len as usize, 10);
        assert_eq!(&patch.decoration.view.watermark.text[..10], b"DECORATION");
        assert_eq!(
            patch.decoration.dirty_regions,
            full_surface_dirty_region(frame.surface)
        );
    }

    #[test]
    fn spotlight_decoration_tracks_first_drag_and_visible_cutouts() {
        let mut model = DocumentModel::new();
        let spotlight = RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: Point::new(100.0, 80.0),
            width: 60.0,
            height: 40.0,
            rotation: 0.0,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        }
        .into_spotlight();

        let inactive = display_decoration(&model, &EditorPresentationState::default());
        assert!(!inactive.spotlight.active);
        assert_eq!(inactive.spotlight.opacity, 0.64);
        assert_eq!(
            inactive.spotlight.color,
            ColorRgba8 {
                r: 0,
                g: 0,
                b: 0,
                a: 255
            }
        );

        let preview = EditorPresentationState {
            creation_preview: Some(ElementCreationPreview::Rectangle(spotlight)),
            ..EditorPresentationState::default()
        };
        assert!(display_decoration(&model, &preview).spotlight.active);

        let id = model.allocate_element_id();
        let mut insert = Transaction::new("insert spotlight");
        insert.insert_rectangle(
            id,
            snow_draw_engine_document::ElementMeta::default(),
            spotlight,
        );
        model.apply_transaction(insert).unwrap();
        assert!(
            display_decoration(&model, &EditorPresentationState::default())
                .spotlight
                .active
        );

        let mut hide = Transaction::new("hide spotlight");
        hide.update_element_meta(
            id,
            snow_draw_engine_document::ElementMeta {
                visible: false,
                ..snow_draw_engine_document::ElementMeta::default()
            },
        );
        model.apply_transaction(hide).unwrap();
        assert!(
            !display_decoration(&model, &EditorPresentationState::default())
                .spotlight
                .active
        );
    }

    #[test]
    fn spotlight_geometry_updates_only_the_decoration_channel() {
        let mut model = DocumentModel::new();
        let mut cache = DocumentSceneCache::new();
        cache.sync(&model, None);
        let frame = frame_view();
        let mut composer = ViewportComposer::new();
        composer.refresh_with_presentation(
            &cache,
            &model,
            frame,
            &EditorPresentationState::default(),
            SnapConfig::default(),
        );

        let spotlight = RectangleData {
            rotation: 0.1,
            ..spotlight_rect(Point::new(100.0, 80.0), 60.0, 40.0)
        };
        let cursor = composer.current_cursor();
        composer.refresh_with_presentation(
            &cache,
            &model,
            frame,
            &EditorPresentationState {
                creation_preview: Some(ElementCreationPreview::Rectangle(spotlight)),
                ..EditorPresentationState::default()
            },
            SnapConfig::default(),
        );
        let creation = composer.acquire_patch(Some(cursor));
        assert_layer_noop(&creation.scene);
        assert_eq!(creation.decoration.spotlight_ops.len(), 1);
        assert_eq!(creation.decoration.spotlight_ops[0].insert_items.len(), 1);
        assert_eq!(
            creation.decoration.dirty_regions,
            full_surface_dirty_region(frame.surface),
            "first activation must repaint the complete mask"
        );

        let id = model.allocate_element_id();
        let mut insert = Transaction::new("insert spotlight");
        insert.insert_rectangle(id, ElementMeta::default(), spotlight);
        let result = model.apply_transaction(insert).unwrap();
        cache.sync(&model, Some(&result.changes));
        composer.refresh_with_presentation(
            &cache,
            &model,
            frame,
            &EditorPresentationState::default(),
            SnapConfig::default(),
        );

        let cursor = composer.current_cursor();
        let moved = RectangleData {
            center: Point::new(140.0, 100.0),
            rotation: 0.25,
            ..spotlight
        };
        composer.refresh_with_presentation(
            &cache,
            &model,
            frame,
            &EditorPresentationState {
                preview_elements: vec![SelectionRectState { id, rect: moved }],
                ..EditorPresentationState::default()
            },
            SnapConfig::default(),
        );
        let moved_patch = composer.acquire_patch(Some(cursor));
        assert_layer_noop(&moved_patch.scene);
        assert_eq!(moved_patch.decoration.spotlight_ops.len(), 1);
        assert!(!moved_patch.decoration.dirty_regions.is_empty());
        assert_ne!(
            moved_patch.decoration.dirty_regions,
            full_surface_dirty_region(frame.surface),
            "subsequent geometry previews must repaint only old/new cutout bounds"
        );
    }

    #[test]
    fn offscreen_spotlight_keeps_mask_active_without_emitting_cutout_geometry() {
        let mut model = DocumentModel::new();
        let id = model.allocate_element_id();
        let spotlight = spotlight_rect(Point::new(5000.0, 5000.0), 100.0, 80.0);
        let mut insert = Transaction::new("offscreen spotlight");
        insert.insert_rectangle(id, ElementMeta::default(), spotlight);
        model.apply_transaction(insert).unwrap();
        let mut cache = DocumentSceneCache::new();
        cache.sync(&model, None);

        let cutouts = compose_spotlight_cutouts(
            &cache,
            &model,
            &EditorPresentationState::default(),
            frame_view(),
        );
        assert!(cutouts.is_empty());
        assert!(
            display_decoration(&model, &EditorPresentationState::default())
                .spotlight
                .active
        );
        assert!(
            compose_scene_items(
                &cache,
                &model,
                &EditorPresentationState::default(),
                frame_view(),
            )
            .is_empty()
        );
    }

    #[test]
    fn overlay_only_presentation_advances_only_overlay_channel() {
        let model = DocumentModel::new();
        let cache = DocumentSceneCache::new();
        let frame = frame_view();
        let mut composer = ViewportComposer::new();
        composer.refresh_with_presentation(
            &cache,
            &model,
            frame,
            &EditorPresentationState::default(),
            SnapConfig::default(),
        );
        let cursor = composer.current_cursor();

        let presentation = EditorPresentationState {
            eraser_cursor: Some(Point::new(120.0, 180.0)),
            ..EditorPresentationState::default()
        };
        composer.refresh_with_presentation(
            &cache,
            &model,
            frame,
            &presentation,
            SnapConfig::default(),
        );
        let patch = composer.acquire_patch(Some(cursor));

        assert_layer_noop(&patch.scene);
        assert_decoration_noop(&patch);
        assert!(!patch.overlay.reset);
        assert!(patch.overlay.revision > patch.overlay.base_revision);
        assert_eq!(patch.overlay.ops.len(), 1);
        assert!(!patch.overlay.dirty_regions.is_empty());
    }

    #[test]
    fn clear_color_change_advances_only_scene_channel() {
        let model = DocumentModel::new();
        let cache = DocumentSceneCache::new();
        let initial = frame_view();
        let presentation = EditorPresentationState::default();
        let mut composer = ViewportComposer::new();
        composer.refresh_with_presentation(
            &cache,
            &model,
            initial,
            &presentation,
            SnapConfig::default(),
        );
        let cursor = composer.current_cursor();
        let changed = FrameView {
            clear_color: ColorRgba8 {
                r: 18,
                g: 24,
                b: 36,
                a: 255,
            },
            ..initial
        };
        composer.refresh_with_presentation(
            &cache,
            &model,
            changed,
            &presentation,
            SnapConfig::default(),
        );
        let patch = composer.acquire_patch(Some(cursor));

        assert!(!patch.scene.reset);
        assert!(patch.scene.revision > patch.scene.base_revision);
        assert!(patch.scene.ops.is_empty());
        assert_eq!(
            patch.scene.dirty_regions,
            full_surface_dirty_region(changed.surface)
        );
        assert_decoration_noop(&patch);
        assert_layer_noop(&patch.overlay);
    }

    #[test]
    fn surface_change_advances_and_dirties_all_three_channels() {
        let model = DocumentModel::new();
        let cache = DocumentSceneCache::new();
        let initial = frame_view();
        let presentation = EditorPresentationState::default();
        let mut composer = ViewportComposer::new();
        composer.refresh_with_presentation(
            &cache,
            &model,
            initial,
            &presentation,
            SnapConfig::default(),
        );
        let cursor = composer.current_cursor();
        let changed = FrameView {
            surface: SurfaceSize {
                width: 800,
                height: 600,
            },
            ..initial
        };
        composer.refresh_with_presentation(
            &cache,
            &model,
            changed,
            &presentation,
            SnapConfig::default(),
        );
        let patch = composer.acquire_patch(Some(cursor));
        let full = full_surface_dirty_region(changed.surface);

        assert!(!patch.scene.reset);
        assert!(patch.scene.revision > patch.scene.base_revision);
        assert_eq!(patch.scene.dirty_regions, full);
        assert!(!patch.decoration.reset);
        assert!(patch.decoration.revision > patch.decoration.base_revision);
        assert_eq!(patch.decoration.dirty_regions, full);
        assert!(!patch.overlay.reset);
        assert!(patch.overlay.revision > patch.overlay.base_revision);
        assert_eq!(patch.overlay.dirty_regions, full);
    }

    #[test]
    fn rectangle_creation_preview_updates_only_scene_channel() {
        let patch = patch_for_creation_preview(ElementCreationPreview::Rectangle(RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: Point::new(20.0, 30.0),
            width: 80.0,
            height: 60.0,
            rotation: 0.0,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 2.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        }));

        let SceneDisplayItem::Rectangle(item) = inserted_scene_item(&patch) else {
            panic!("rectangle creation preview should be a scene rectangle");
        };
        assert_eq!(item.center_x, 20.0);
        assert_eq!(item.center_y, 30.0);
        assert_eq!(item.width, 80.0);
        assert_eq!(item.height, 60.0);
    }

    #[test]
    fn arrow_creation_preview_updates_only_scene_channel() {
        let arrow = ArrowData::from_global_points(
            &[Point::new(-40.0, -20.0), Point::new(60.0, 50.0)],
            ColorRgba8::default(),
            2.0,
            StrokeStyle::Solid,
            ArrowType::Straight,
            None,
            None,
        )
        .unwrap();
        let patch = patch_for_creation_preview(ElementCreationPreview::Arrow(arrow));

        let SceneDisplayItem::Arrow(item) = inserted_scene_item(&patch) else {
            panic!("arrow creation preview should be a scene arrow");
        };
        assert_eq!(item.points.len(), 2);
    }

    #[test]
    fn serial_number_creation_preview_updates_only_scene_channel() {
        let patch =
            patch_for_creation_preview(ElementCreationPreview::SerialNumber(SerialNumberData {
                center: Point::new(15.0, -25.0),
                diameter: 32.0,
                number: 7,
                ..SerialNumberData::default()
            }));

        let SceneDisplayItem::SerialNumber(item) = inserted_scene_item(&patch) else {
            panic!("serial number creation preview should be a scene serial number");
        };
        assert_eq!(item.center_x, 15.0);
        assert_eq!(item.center_y, -25.0);
        assert_eq!(item.diameter, 32.0);
        assert_eq!(item.number, 7);
    }

    #[test]
    fn serial_connector_move_patch_dirties_old_and_new_connector_bounds() {
        let serial_id = ElementId {
            index: 0,
            generation: 1,
        };
        let text_id = ElementId {
            index: 1,
            generation: 1,
        };
        let mut model = DocumentModel::new();
        insert_bound_serial_and_text(&mut model, serial_id, text_id, Point::new(90.0, 0.0));

        let mut cache = DocumentSceneCache::new();
        cache.sync(&model, None);
        let frame_view = frame_view();
        let presentation = EditorPresentationState::default();
        let old_items = compose_scene_items(&cache, &model, &presentation, frame_view);
        let old_connector_bounds = connector_bounds(&old_items, frame_view);

        let mut composer = ViewportComposer::new();
        composer.refresh_with_presentation(
            &cache,
            &model,
            frame_view,
            &presentation,
            SnapConfig::default(),
        );
        let cursor = composer.current_cursor();

        let mut text = model.text(text_id).unwrap().clone();
        text.center = Point::new(180.0, 40.0);
        let mut transaction = Transaction::new("move text");
        transaction.update_text(text_id, text);
        let result = model.apply_transaction(transaction).unwrap();
        cache.sync(&model, Some(&result.changes));

        let new_items = compose_scene_items(&cache, &model, &presentation, frame_view);
        let new_connector_bounds = connector_bounds(&new_items, frame_view);
        composer.refresh_with_presentation(
            &cache,
            &model,
            frame_view,
            &presentation,
            SnapConfig::default(),
        );
        let patch = composer.acquire_patch(Some(cursor));

        assert!(!patch.scene.reset);
        assert_layer_noop(&patch.overlay);
        assert_decoration_noop(&patch);
        assert!(dirty_regions_cover(
            &patch.scene.dirty_regions,
            old_connector_bounds
        ));
        assert!(dirty_regions_cover(
            &patch.scene.dirty_regions,
            new_connector_bounds
        ));
    }

    #[test]
    fn serial_connector_removal_patch_dirties_old_connector_bounds() {
        let serial_id = ElementId {
            index: 0,
            generation: 1,
        };
        let text_id = ElementId {
            index: 1,
            generation: 1,
        };
        let mut model = DocumentModel::new();
        insert_bound_serial_and_text(&mut model, serial_id, text_id, Point::new(90.0, 0.0));

        let mut cache = DocumentSceneCache::new();
        cache.sync(&model, None);
        let frame_view = frame_view();
        let presentation = EditorPresentationState::default();
        let old_items = compose_scene_items(&cache, &model, &presentation, frame_view);
        let old_connector_bounds = connector_bounds(&old_items, frame_view);

        let mut composer = ViewportComposer::new();
        composer.refresh_with_presentation(
            &cache,
            &model,
            frame_view,
            &presentation,
            SnapConfig::default(),
        );
        let cursor = composer.current_cursor();

        let mut transaction = Transaction::new("remove bound text");
        transaction.remove_element(text_id);
        let result = model.apply_transaction(transaction).unwrap();
        cache.sync(&model, Some(&result.changes));

        composer.refresh_with_presentation(
            &cache,
            &model,
            frame_view,
            &presentation,
            SnapConfig::default(),
        );
        let patch = composer.acquire_patch(Some(cursor));

        assert!(!patch.scene.reset);
        assert_layer_noop(&patch.overlay);
        assert_decoration_noop(&patch);
        assert!(dirty_regions_cover(
            &patch.scene.dirty_regions,
            old_connector_bounds
        ));
    }

    #[test]
    fn camera_change_dirties_surface_without_resetting_items() {
        let model = DocumentModel::new();
        let cache = DocumentSceneCache::new();
        let presentation = EditorPresentationState::default();
        let mut composer = ViewportComposer::new();
        let initial_view = frame_view();
        composer.refresh_with_presentation(
            &cache,
            &model,
            initial_view,
            &presentation,
            SnapConfig::default(),
        );
        let cursor = composer.current_cursor();
        let moved_view = FrameView {
            camera: Camera {
                center: Point::new(50.0, 25.0),
                ..initial_view.camera
            },
            ..initial_view
        };
        composer.refresh_with_presentation(
            &cache,
            &model,
            moved_view,
            &presentation,
            SnapConfig::default(),
        );
        let patch = composer.acquire_patch(Some(cursor));

        assert!(!patch.scene.reset);
        assert!(!patch.overlay.reset);
        assert!(patch.scene.ops.is_empty());
        assert!(patch.overlay.ops.is_empty());
        assert_eq!(
            patch.scene.dirty_regions,
            full_surface_dirty_region(moved_view.surface)
        );
        assert_eq!(
            patch.overlay.dirty_regions,
            full_surface_dirty_region(moved_view.surface)
        );
        assert!(!patch.decoration.reset);
        assert!(patch.decoration.revision > patch.decoration.base_revision);
        assert_eq!(
            patch.decoration.dirty_regions,
            full_surface_dirty_region(moved_view.surface)
        );
    }

    #[test]
    fn cursor_incremental_noop_stale_and_reset_paths_include_decoration_channel() {
        let mut model = DocumentModel::new();
        let cache = DocumentSceneCache::new();
        let frame = frame_view();
        let presentation = EditorPresentationState::default();
        let mut composer = ViewportComposer::new();
        composer.refresh_with_presentation(
            &cache,
            &model,
            frame,
            &presentation,
            SnapConfig::default(),
        );

        let initial_reset = composer.acquire_patch(None);
        assert!(initial_reset.scene.reset);
        assert!(initial_reset.decoration.reset);
        assert!(initial_reset.overlay.reset);
        let initial_cursor = composer.current_cursor();

        let mut transaction = Transaction::new("cursor decoration update");
        transaction.update_watermark(watermark_config("CURSOR"));
        model.apply_transaction(transaction).unwrap();
        composer.refresh_with_presentation(
            &cache,
            &model,
            frame,
            &presentation,
            SnapConfig::default(),
        );

        let incremental = composer.acquire_patch(Some(initial_cursor));
        assert_layer_noop(&incremental.scene);
        assert_layer_noop(&incremental.overlay);
        assert!(incremental.decoration.revision > incremental.decoration.base_revision);
        assert!(!incremental.decoration.reset);

        let current_cursor = composer.current_cursor();
        let noop = composer.acquire_patch(Some(current_cursor));
        assert_layer_noop(&noop.scene);
        assert_decoration_noop(&noop);
        assert_layer_noop(&noop.overlay);

        let stale_cursor = PatchCursor {
            scene_revision: SceneRevision(0),
            decoration_revision: current_cursor.decoration_revision,
            overlay_revision: current_cursor.overlay_revision,
        };
        let reset = composer.acquire_patch(Some(stale_cursor));
        assert!(reset.scene.reset);
        assert!(reset.decoration.reset);
        assert!(reset.overlay.reset);
        assert_eq!(reset.scene.base_revision, 0);
        assert_eq!(
            reset.decoration.base_revision,
            current_cursor.decoration_revision.0
        );
    }

    #[test]
    fn watermark_revert_keeps_undo_and_redo_work_in_decoration_channel() {
        let mut model = DocumentModel::new();
        let cache = DocumentSceneCache::new();
        let frame = frame_view();
        let presentation = EditorPresentationState::default();
        let mut composer = ViewportComposer::new();
        composer.refresh_with_presentation(
            &cache,
            &model,
            frame,
            &presentation,
            SnapConfig::default(),
        );

        let mut first = Transaction::new("watermark first");
        first.update_watermark(watermark_config("FIRST"));
        model.apply_transaction(first).unwrap();
        composer.refresh_with_presentation(
            &cache,
            &model,
            frame,
            &presentation,
            SnapConfig::default(),
        );
        let first_cursor = composer.current_cursor();

        let mut second = Transaction::new("watermark second");
        second.update_watermark(watermark_config("SECOND"));
        model.apply_transaction(second).unwrap();
        composer.refresh_with_presentation(
            &cache,
            &model,
            frame,
            &presentation,
            SnapConfig::default(),
        );
        let second_patch = composer.acquire_patch(Some(first_cursor));
        assert_layer_noop(&second_patch.scene);
        assert_layer_noop(&second_patch.overlay);
        assert!(second_patch.decoration.revision > second_patch.decoration.base_revision);

        let second_cursor = composer.current_cursor();
        let mut redo = Transaction::new("watermark redo");
        redo.update_watermark(watermark_config("FIRST"));
        model.apply_transaction(redo).unwrap();
        composer.refresh_with_presentation(
            &cache,
            &model,
            frame,
            &presentation,
            SnapConfig::default(),
        );
        let redo_patch = composer.acquire_patch(Some(second_cursor));
        assert_layer_noop(&redo_patch.scene);
        assert_layer_noop(&redo_patch.overlay);
        assert!(redo_patch.decoration.revision > redo_patch.decoration.base_revision);
        assert_eq!(
            redo_patch.decoration.view.watermark.text_len as usize,
            "FIRST".len()
        );
    }

    #[test]
    fn blur_above_changed_content_expands_its_dirty_region() {
        let frame = frame_view();
        let rectangle = SceneDisplayItem::Rectangle(RectangleDisplayItem {
            center_x: 0.0,
            center_y: 0.0,
            width: 20.0,
            height: 20.0,
            opacity: 1.0,
            ..RectangleDisplayItem::default()
        });
        let filter = SceneDisplayItem::Filter(snow_draw_engine_display::FilterDisplayItem {
            center_x: 0.0,
            center_y: 0.0,
            width: 100.0,
            height: 100.0,
            filter: snow_draw_engine_display::FilterRenderSpec::resolve(
                snow_draw_engine_display::DisplayFilterType::GaussianBlur,
                1.0,
            ),
            opacity: 1.0,
            ..snow_draw_engine_display::FilterDisplayItem::default()
        });
        let base = scene_display_item_bounds(&rectangle, frame).unwrap();
        let affected = dirty_region_through_filters(&[rectangle, filter], 0, base, frame);

        assert!(affected.min_x < base.min_x);
        assert!(affected.min_y < base.min_y);
        assert!(affected.max_x > base.max_x);
        assert!(affected.max_y > base.max_y);
    }

    #[test]
    fn filter_below_changed_content_does_not_expand_its_dirty_region() {
        let frame = frame_view();
        let filter = SceneDisplayItem::Filter(snow_draw_engine_display::FilterDisplayItem {
            center_x: 0.0,
            center_y: 0.0,
            width: 100.0,
            height: 100.0,
            filter: snow_draw_engine_display::FilterRenderSpec::resolve(
                snow_draw_engine_display::DisplayFilterType::GaussianBlur,
                1.0,
            ),
            opacity: 1.0,
            ..snow_draw_engine_display::FilterDisplayItem::default()
        });
        let rectangle = SceneDisplayItem::Rectangle(RectangleDisplayItem {
            center_x: 0.0,
            center_y: 0.0,
            width: 20.0,
            height: 20.0,
            opacity: 1.0,
            ..RectangleDisplayItem::default()
        });
        let base = scene_display_item_bounds(&rectangle, frame).unwrap();

        assert_eq!(
            dirty_region_through_filters(&[filter, rectangle], 1, base, frame),
            base
        );
    }

    #[test]
    fn adjacent_filters_expand_from_one_pre_layer_region() {
        let frame = frame_view();
        let rectangle = SceneDisplayItem::Rectangle(RectangleDisplayItem {
            center_x: 0.0,
            center_y: 0.0,
            width: 20.0,
            height: 20.0,
            opacity: 1.0,
            ..RectangleDisplayItem::default()
        });
        let filter = SceneDisplayItem::Filter(snow_draw_engine_display::FilterDisplayItem {
            center_x: 0.0,
            center_y: 0.0,
            width: 200.0,
            height: 200.0,
            filter: snow_draw_engine_display::FilterRenderSpec::resolve(
                snow_draw_engine_display::DisplayFilterType::GaussianBlur,
                1.0,
            ),
            opacity: 1.0,
            ..snow_draw_engine_display::FilterDisplayItem::default()
        });
        let base = scene_display_item_bounds(&rectangle, frame).unwrap();
        let one =
            dirty_region_through_filters(&[rectangle.clone(), filter.clone()], 0, base, frame);
        let stacked =
            dirty_region_through_filters(&[rectangle, filter.clone(), filter], 0, base, frame);
        assert_eq!(stacked, one);
    }
}
