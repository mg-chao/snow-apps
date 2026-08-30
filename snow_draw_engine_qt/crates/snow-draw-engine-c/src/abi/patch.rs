use snow_draw_engine::ViewportPatch;

use crate::abi::convert::{
    SnowOverlayPatchItem, SnowScenePatchItem, snow_arrow_path_commands_from_rust,
    snow_dirty_rect_from_rust, snow_overlay_display_item_from_rust, snow_patch_info_from_rust,
    snow_scene_patch_item_from_rust, snow_spotlight_cutout_from_rust,
};
use crate::abi::types::*;

pub(crate) struct SnowPatchPayload {
    pub(crate) info: SnowPatchInfo,
    pub(crate) scene_ops: Box<[SnowPatchOp]>,
    pub(crate) overlay_ops: Box<[SnowPatchOp]>,
    pub(crate) spotlight_ops: Box<[SnowPatchOp]>,
    pub(crate) pen_filter_geometry_ops: Box<[SnowPenFilterGeometryPatch]>,
    pub(crate) pen_filter_geometry_points: Box<[SnowArrowPoint]>,
    pub(crate) path_geometry_ops: Box<[SnowPathGeometryPatch]>,
    pub(crate) path_geometry_ranges: Box<[SnowPathChunkRange]>,
    pub(crate) path_geometry_chunks: Box<[SnowPathChunk]>,
    pub(crate) path_geometry_commands: Box<[SnowArrowPathCommand]>,
    pub(crate) scene_items: Box<[SnowSceneDisplayItem]>,
    pub(crate) overlay_items: Box<[SnowOverlayDisplayItem]>,
    pub(crate) spotlight_cutouts: Box<[SnowSpotlightCutout]>,
    _scene_item_backing: Box<[SnowScenePatchItem]>,
    _overlay_item_backing: Box<[SnowOverlayPatchItem]>,
    pub(crate) scene_dirty_rects: Box<[SnowDirtyRect]>,
    pub(crate) decoration_dirty_rects: Box<[SnowDirtyRect]>,
    pub(crate) overlay_dirty_rects: Box<[SnowDirtyRect]>,
}

impl SnowPatchPayload {
    pub(crate) fn from_patch(patch: &ViewportPatch) -> Self {
        let mut scene_item_backing = Vec::new();
        let scene_ops = patch
            .scene
            .ops
            .iter()
            .map(|op| {
                let insert_offset = scene_item_backing.len() as u32;
                scene_item_backing.extend(op.insert_items.iter().map(|item| {
                    let omit_path_commands = match item {
                        snow_draw_engine::SceneDisplayItem::Arrow(path) => patch
                            .path_geometry_ops
                            .iter()
                            .any(|geometry| geometry.id == path.id && !geometry.element_removed),
                        _ => false,
                    };
                    snow_scene_patch_item_from_rust(item, omit_path_commands)
                }));
                SnowPatchOp {
                    start: op.start,
                    delete_count: op.delete_count,
                    insert_offset,
                    insert_count: (scene_item_backing.len() as u32).saturating_sub(insert_offset),
                }
            })
            .collect::<Vec<_>>()
            .into_boxed_slice();

        let mut pen_filter_geometry_points = Vec::new();
        let pen_filter_geometry_ops = patch
            .pen_filter_geometry_ops
            .iter()
            .map(|op| {
                let append_offset = pen_filter_geometry_points.len() as u32;
                pen_filter_geometry_points.extend(op.appended_points.iter().map(|point| {
                    SnowArrowPoint {
                        x: point[0],
                        y: point[1],
                    }
                }));
                SnowPenFilterGeometryPatch {
                    element_id: SnowElementId {
                        index: op.id.index,
                        generation: op.id.generation,
                    },
                    expected_geometry_revision: op.expected_geometry_revision,
                    resulting_geometry_revision: op.resulting_geometry_revision,
                    retain_prefix_count: op.retain_prefix_count,
                    append_offset,
                    append_count: (pen_filter_geometry_points.len() as u32)
                        .saturating_sub(append_offset),
                    old_changed_min_x: op.old_changed_bounds[0],
                    old_changed_min_y: op.old_changed_bounds[1],
                    old_changed_max_x: op.old_changed_bounds[2],
                    old_changed_max_y: op.old_changed_bounds[3],
                    new_changed_min_x: op.new_changed_bounds[0],
                    new_changed_min_y: op.new_changed_bounds[1],
                    new_changed_max_x: op.new_changed_bounds[2],
                    new_changed_max_y: op.new_changed_bounds[3],
                    full_reset: u8::from(op.full_reset),
                    element_removed: u8::from(op.element_removed),
                    reserved0: [0; 6],
                }
            })
            .collect::<Vec<_>>()
            .into_boxed_slice();

        let mut path_geometry_ranges = Vec::new();
        let mut path_geometry_chunks = Vec::new();
        let mut path_geometry_commands = Vec::new();
        let path_geometry_ops = patch
            .path_geometry_ops
            .iter()
            .map(|op| {
                let range_offset = path_geometry_ranges.len() as u32;
                for range in &op.ranges {
                    let insert_chunk_offset = path_geometry_chunks.len() as u32;
                    for chunk in &range.insert_chunks {
                        let command_offset = path_geometry_commands.len() as u32;
                        path_geometry_commands
                            .extend(snow_arrow_path_commands_from_rust(&chunk.commands));
                        path_geometry_chunks.push(SnowPathChunk {
                            stable_id: chunk.stable_id,
                            command_start: chunk.command_start,
                            command_offset,
                            command_count: chunk.commands.len() as u32,
                            reserved0: 0,
                            start_x: chunk.start_point[0],
                            start_y: chunk.start_point[1],
                            min_x: chunk.canvas_bounds[0],
                            min_y: chunk.canvas_bounds[1],
                            max_x: chunk.canvas_bounds[2],
                            max_y: chunk.canvas_bounds[3],
                            cumulative_start_length: chunk.cumulative_start_length,
                        });
                    }
                    path_geometry_ranges.push(SnowPathChunkRange {
                        start: range.start,
                        delete_count: range.delete_count,
                        insert_chunk_offset,
                        insert_chunk_count: (path_geometry_chunks.len() as u32)
                            .saturating_sub(insert_chunk_offset),
                    });
                }
                SnowPathGeometryPatch {
                    element_id: SnowElementId {
                        index: op.id.index,
                        generation: op.id.generation,
                    },
                    expected_geometry_revision: op.expected_geometry_revision,
                    resulting_geometry_revision: op.resulting_geometry_revision,
                    range_offset,
                    range_count: (path_geometry_ranges.len() as u32).saturating_sub(range_offset),
                    old_changed_min_x: op.old_changed_bounds[0],
                    old_changed_min_y: op.old_changed_bounds[1],
                    old_changed_max_x: op.old_changed_bounds[2],
                    old_changed_max_y: op.old_changed_bounds[3],
                    new_changed_min_x: op.new_changed_bounds[0],
                    new_changed_min_y: op.new_changed_bounds[1],
                    new_changed_max_x: op.new_changed_bounds[2],
                    new_changed_max_y: op.new_changed_bounds[3],
                    closed: u8::from(op.closed),
                    full_reset: u8::from(op.full_reset),
                    element_removed: u8::from(op.element_removed),
                    reserved0: [0; 5],
                }
            })
            .collect::<Vec<_>>()
            .into_boxed_slice();

        let mut overlay_item_backing = Vec::new();
        let overlay_ops = patch
            .overlay
            .ops
            .iter()
            .map(|op| {
                let insert_offset = overlay_item_backing.len() as u32;
                overlay_item_backing.extend(
                    op.insert_items
                        .iter()
                        .map(snow_overlay_display_item_from_rust),
                );
                SnowPatchOp {
                    start: op.start,
                    delete_count: op.delete_count,
                    insert_offset,
                    insert_count: (overlay_item_backing.len() as u32).saturating_sub(insert_offset),
                }
            })
            .collect::<Vec<_>>()
            .into_boxed_slice();

        let mut spotlight_cutouts = Vec::new();
        let spotlight_ops = patch
            .decoration
            .spotlight_ops
            .iter()
            .map(|op| {
                let insert_offset = spotlight_cutouts.len() as u32;
                spotlight_cutouts.extend(
                    op.insert_items
                        .iter()
                        .copied()
                        .map(snow_spotlight_cutout_from_rust),
                );
                SnowPatchOp {
                    start: op.start,
                    delete_count: op.delete_count,
                    insert_offset,
                    insert_count: (spotlight_cutouts.len() as u32).saturating_sub(insert_offset),
                }
            })
            .collect::<Vec<_>>()
            .into_boxed_slice();

        let scene_item_backing = scene_item_backing.into_boxed_slice();
        let scene_items = scene_item_backing
            .iter()
            .map(|item| item.view)
            .collect::<Vec<_>>()
            .into_boxed_slice();
        let overlay_item_backing = overlay_item_backing.into_boxed_slice();
        let overlay_items = overlay_item_backing
            .iter()
            .map(|item| item.view)
            .collect::<Vec<_>>()
            .into_boxed_slice();

        Self {
            info: snow_patch_info_from_rust(patch),
            scene_ops,
            overlay_ops,
            spotlight_ops,
            pen_filter_geometry_ops,
            pen_filter_geometry_points: pen_filter_geometry_points.into_boxed_slice(),
            path_geometry_ops,
            path_geometry_ranges: path_geometry_ranges.into_boxed_slice(),
            path_geometry_chunks: path_geometry_chunks.into_boxed_slice(),
            path_geometry_commands: path_geometry_commands.into_boxed_slice(),
            scene_items,
            overlay_items,
            spotlight_cutouts: spotlight_cutouts.into_boxed_slice(),
            _scene_item_backing: scene_item_backing,
            _overlay_item_backing: overlay_item_backing,
            scene_dirty_rects: patch
                .scene
                .dirty_regions
                .iter()
                .copied()
                .map(snow_dirty_rect_from_rust)
                .collect::<Vec<_>>()
                .into_boxed_slice(),
            decoration_dirty_rects: patch
                .decoration
                .dirty_regions
                .iter()
                .copied()
                .map(snow_dirty_rect_from_rust)
                .collect::<Vec<_>>()
                .into_boxed_slice(),
            overlay_dirty_rects: patch
                .overlay
                .dirty_regions
                .iter()
                .copied()
                .map(snow_dirty_rect_from_rust)
                .collect::<Vec<_>>()
                .into_boxed_slice(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine::{
        DisplayItemId, FilterDisplayItem, PathChunkReplacement, PathCommand, PathGeometry,
        PathGeometryPatch, PenFilterGeometryPatch, ReplaceRangeOp, SceneDisplayItem,
    };

    #[test]
    fn pen_filter_patch_exports_only_the_changed_geometry_tail() {
        let id = DisplayItemId {
            index: 41,
            generation: 3,
        };
        let points = (0..4096)
            .map(|index| [index as f64, (index % 17) as f64])
            .collect::<Vec<_>>();
        let mut patch = ViewportPatch::default();
        patch.scene.ops = vec![ReplaceRangeOp {
            start: 0,
            delete_count: 1,
            insert_items: vec![SceneDisplayItem::Filter(FilterDisplayItem {
                id,
                points: points.clone().into(),
                is_pen_filter: true,
                stroke_width: 20.0,
                ..FilterDisplayItem::default()
            })],
        }];
        patch.pen_filter_geometry_ops = vec![PenFilterGeometryPatch {
            id,
            expected_geometry_revision: 7,
            resulting_geometry_revision: 8,
            retain_prefix_count: 4094,
            appended_points: points[4094..].to_vec().into(),
            old_changed_bounds: [4093.0, 0.0, 4096.0, 17.0],
            new_changed_bounds: [4093.0, 0.0, 4096.0, 17.0],
            full_reset: false,
            element_removed: false,
        }];

        let payload = SnowPatchPayload::from_patch(&patch);
        assert_eq!(payload.scene_items[0].arrow_point_count, 0);
        assert!(payload.scene_items[0].arrow_points.is_null());
        assert_eq!(payload.pen_filter_geometry_ops.len(), 1);
        assert_eq!(payload.pen_filter_geometry_ops[0].retain_prefix_count, 4094);
        assert_eq!(payload.pen_filter_geometry_points.len(), 2);
    }

    #[test]
    fn path_patch_flattens_ranges_chunks_and_commands_for_c() {
        let id = DisplayItemId {
            index: 52,
            generation: 4,
        };
        let mut commands = vec![PathCommand::MoveTo { point: [1.0, 2.0] }];
        commands.extend((1..=65).map(|index| PathCommand::LineTo {
            point: [index as f64, index as f64 + 2.0],
        }));
        let geometry = PathGeometry::from_commands(9, commands, false);
        assert_eq!(geometry.chunks.len(), 2);
        let inserted = geometry.chunks[1].clone();
        let patch = ViewportPatch {
            path_geometry_ops: vec![PathGeometryPatch {
                id,
                expected_geometry_revision: 8,
                resulting_geometry_revision: 9,
                ranges: vec![PathChunkReplacement {
                    start: 1,
                    delete_count: 1,
                    insert_chunks: vec![inserted.clone()],
                }],
                old_changed_bounds: [0.0, 0.0, 64.0, 66.0],
                new_changed_bounds: inserted.canvas_bounds,
                closed: false,
                full_reset: false,
                element_removed: false,
            }],
            ..ViewportPatch::default()
        };

        let payload = SnowPatchPayload::from_patch(&patch);
        assert_eq!(payload.path_geometry_ops.len(), 1);
        assert_eq!(payload.path_geometry_ops[0].range_offset, 0);
        assert_eq!(payload.path_geometry_ops[0].range_count, 1);
        assert_eq!(payload.path_geometry_ranges[0].start, 1);
        assert_eq!(payload.path_geometry_ranges[0].insert_chunk_count, 1);
        assert_eq!(payload.path_geometry_chunks[0].command_start, 64);
        assert_eq!(
            payload.path_geometry_chunks[0].start_x,
            inserted.start_point[0]
        );
        assert_eq!(
            payload.path_geometry_chunks[0].start_y,
            inserted.start_point[1]
        );
        assert_eq!(
            payload.path_geometry_commands.len(),
            inserted.commands.len()
        );
        assert_eq!(
            payload.path_geometry_commands[0].kind,
            SnowArrowPathCommandKind::LineTo
        );
    }
}
