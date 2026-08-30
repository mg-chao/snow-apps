use std::ffi::c_char;

use snow_draw_engine::{
    DirtyRegion, DisplayItemId, OverlayDisplayItem, PatchCursor, SceneDisplayItem, SnapGuideAxis,
    SnapGuideKind, UiShapeKind as UiRectKind, ViewportPatch,
};

use crate::abi::types::*;

use super::{
    snow_arrow_path_commands_from_rust, snow_arrow_points_from_rust,
    snow_arrow_stroke_style_from_rust, snow_arrow_type_from_rust, snow_arrowhead_from_rust,
    snow_arrowhead_primitives_from_rust, snow_fill_style_from_rust, snow_stroke_style_from_rust,
    snow_text_horizontal_align_from_rust, snow_text_vertical_align_from_rust,
};

fn slice_ptr_or_null<T>(value: &[T]) -> *const T {
    if value.is_empty() {
        std::ptr::null()
    } else {
        value.as_ptr()
    }
}

fn utf8_bytes(value: Option<&str>) -> Box<[c_char]> {
    value
        .unwrap_or_default()
        .as_bytes()
        .iter()
        .map(|byte| *byte as c_char)
        .collect::<Vec<_>>()
        .into_boxed_slice()
}

#[derive(Default)]
pub(crate) struct SnowScenePatchItem {
    pub(crate) view: SnowSceneDisplayItem,
    arrow_points: Box<[SnowArrowPoint]>,
    arrow_path_commands: Box<[SnowArrowPathCommand]>,
    arrowhead_primitives: Box<[SnowArrowheadPrimitive]>,
    text_utf8: Box<[c_char]>,
    font_family_utf8: Box<[c_char]>,
}

impl SnowScenePatchItem {
    fn refresh_pointers(&mut self) {
        self.view.arrow_points = slice_ptr_or_null(&self.arrow_points);
        self.view.arrow_path_commands = slice_ptr_or_null(&self.arrow_path_commands);
        self.view.arrowhead_primitives = slice_ptr_or_null(&self.arrowhead_primitives);
        self.view.text_utf8 = slice_ptr_or_null(&self.text_utf8);
        self.view.font_family_utf8 = slice_ptr_or_null(&self.font_family_utf8);
    }
}

#[derive(Default)]
pub(crate) struct SnowOverlayPatchItem {
    pub(crate) view: SnowOverlayDisplayItem,
    arrow_points: Box<[SnowArrowPoint]>,
    arrow_path_commands: Box<[SnowArrowPathCommand]>,
    arrowhead_primitives: Box<[SnowArrowheadPrimitive]>,
}

impl SnowOverlayPatchItem {
    fn refresh_pointers(&mut self) {
        self.view.arrow_points = slice_ptr_or_null(&self.arrow_points);
        self.view.arrow_path_commands = slice_ptr_or_null(&self.arrow_path_commands);
        self.view.arrowhead_primitives = slice_ptr_or_null(&self.arrowhead_primitives);
    }
}

fn encode_bound_text_id(out: &mut SnowSceneDisplayItem, id: Option<DisplayItemId>) {
    out.bound_text_element_index = 0;
    out.bound_text_element_generation = 0;
    out.has_bound_text_element = 0;
    if let Some(id) = id {
        out.bound_text_element_index = id.index;
        out.bound_text_element_generation = id.generation;
        out.has_bound_text_element = 1;
    }
}

pub(crate) fn snow_patch_info_from_rust(patch: &ViewportPatch) -> SnowPatchInfo {
    SnowPatchInfo {
        scene_base_revision: patch.scene.base_revision,
        scene_revision: patch.scene.revision,
        decoration_base_revision: patch.decoration.base_revision,
        decoration_revision: patch.decoration.revision,
        overlay_base_revision: patch.overlay.base_revision,
        overlay_revision: patch.overlay.revision,
        scene_reset: u8::from(patch.scene.reset),
        decoration_reset: u8::from(patch.decoration.reset),
        overlay_reset: u8::from(patch.overlay.reset),
        reserved0: [0; 5],
        scene_op_count: patch.scene.ops.len() as u32,
        overlay_op_count: patch.overlay.ops.len() as u32,
        spotlight_op_count: patch.decoration.spotlight_ops.len() as u32,
        scene_item_count: patch
            .scene
            .ops
            .iter()
            .map(|op| op.insert_items.len() as u32)
            .sum(),
        overlay_item_count: patch
            .overlay
            .ops
            .iter()
            .map(|op| op.insert_items.len() as u32)
            .sum(),
        spotlight_item_count: patch
            .decoration
            .spotlight_ops
            .iter()
            .map(|op| op.insert_items.len() as u32)
            .sum(),
        scene_dirty_rect_count: patch.scene.dirty_regions.len() as u32,
        decoration_dirty_rect_count: patch.decoration.dirty_regions.len() as u32,
        overlay_dirty_rect_count: patch.overlay.dirty_regions.len() as u32,
        surface_width: patch.frame_view.surface.width,
        surface_height: patch.frame_view.surface.height,
        camera_center_x: patch.frame_view.camera.center.x,
        camera_center_y: patch.frame_view.camera.center.y,
        camera_zoom: patch.frame_view.camera.zoom,
        clear_color: patch.frame_view.clear_color.into(),
        watermark_color: patch.decoration.view.watermark.color.into(),
        watermark_text_len: patch.decoration.view.watermark.text_len,
        watermark_font_family_len: patch.decoration.view.watermark.font_family_len,
        watermark_text: patch.decoration.view.watermark.text,
        watermark_font_size: patch.decoration.view.watermark.font_size,
        watermark_font_family: patch.decoration.view.watermark.font_family,
        watermark_angle: patch.decoration.view.watermark.angle,
        watermark_gap: patch.decoration.view.watermark.gap,
        watermark_opacity: patch.decoration.view.watermark.opacity,
        spotlight_color: patch.decoration.view.spotlight.color.into(),
        spotlight_opacity: patch.decoration.view.spotlight.opacity,
        spotlight_active: u8::from(patch.decoration.view.spotlight.active),
        reserved1: [0; 7],
    }
}

pub(crate) fn snow_dirty_rect_from_rust(value: DirtyRegion) -> SnowDirtyRect {
    SnowDirtyRect {
        min_x: value.min_x,
        min_y: value.min_y,
        max_x: value.max_x,
        max_y: value.max_y,
    }
}

pub(crate) fn snow_spotlight_cutout_from_rust(
    value: snow_draw_engine::DisplaySpotlightCutout,
) -> SnowSpotlightCutout {
    SnowSpotlightCutout {
        center_x: value.center_x,
        center_y: value.center_y,
        width: value.width,
        height: value.height,
        rotation: value.rotation,
    }
}

/// Converts a scene item into its C representation without cloning the Rust item.
///
/// `omit_arrow_points` must be set for pen filters (their points travel through
/// `pen_filter_geometry_ops` instead) and `omit_path_commands` for arrows whose
/// commands travel through `path_geometry_ops`; skipping the conversion here
/// avoids allocating buffers that the patch payload would discard.
pub(crate) fn snow_scene_display_item_from_rust(
    value: &SceneDisplayItem,
    omit_arrow_points: bool,
    omit_path_commands: bool,
) -> SnowScenePatchItem {
    let mut converted = SnowScenePatchItem::default();
    let out = &mut converted.view;
    match value {
        SceneDisplayItem::Rectangle(item) => {
            out.kind = SnowSceneDisplayItemKind::DrawRect;
            out.element_id = SnowElementId {
                index: item.id.index,
                generation: item.id.generation,
            };
            out.center_x = item.center_x;
            out.center_y = item.center_y;
            out.width = item.width;
            out.height = item.height;
            out.rotation = item.rotation;
            out.fill = item.fill.into();
            out.fill_style = snow_fill_style_from_rust(item.fill_style);
            out.stroke = item.stroke.into();
            out.fill = item.fill.into();
            out.fill_style = snow_fill_style_from_rust(item.fill_style);
            out.stroke_width = item.stroke_width;
            out.stroke_style = snow_stroke_style_from_rust(item.stroke_style);
            out.corner_radii = item.corner_radii.into();
            out.opacity = item.opacity;
            out.rect_shape = match item.shape {
                snow_draw_engine::DisplayRectangleShape::Rectangle => {
                    SnowDisplayRectShape::Rectangle as u8
                }
                snow_draw_engine::DisplayRectangleShape::Ellipse => {
                    SnowDisplayRectShape::Ellipse as u8
                }
                snow_draw_engine::DisplayRectangleShape::Diamond => {
                    SnowDisplayRectShape::Diamond as u8
                }
            };
            out.blend_mode = match item.blend_mode {
                snow_draw_engine::DisplayBlendMode::Normal => SnowBlendMode::Normal,
                snow_draw_engine::DisplayBlendMode::Multiply => SnowBlendMode::Multiply,
            };
        }
        SceneDisplayItem::Filter(item) => {
            out.kind = SnowSceneDisplayItemKind::Filter;
            out.element_id = SnowElementId {
                index: item.id.index,
                generation: item.id.generation,
            };
            out.center_x = item.center_x;
            out.center_y = item.center_y;
            out.width = item.width;
            out.height = item.height;
            out.rotation = item.rotation;
            out.stroke_width = item.stroke_width;
            out.is_free_draw = u8::from(item.is_pen_filter);
            out.opacity = item.opacity;
            out.filter.filter_type = match item.filter.filter_type {
                snow_draw_engine::DisplayFilterType::Mosaic => 0,
                snow_draw_engine::DisplayFilterType::GaussianBlur => 1,
                snow_draw_engine::DisplayFilterType::Grayscale => 2,
                snow_draw_engine::DisplayFilterType::Inversion => 3,
            };
            out.filter.strength = item.filter.strength;
            out.filter.mosaic_block_size = item.filter.mosaic_block_size;
            out.filter.blur_sigma = item.filter.blur_sigma;
            out.filter.sampling_radius = item.filter.sampling_radius;
            if !omit_arrow_points {
                converted.arrow_points =
                    snow_arrow_points_from_rust(&item.points).into_boxed_slice();
                out.arrow_point_count = converted.arrow_points.len() as u32;
            }
        }
        SceneDisplayItem::Arrow(item) => {
            out.kind = SnowSceneDisplayItemKind::Arrow;
            out.element_id = SnowElementId {
                index: item.id.index,
                generation: item.id.generation,
            };
            out.stroke = item.stroke.into();
            out.stroke_width = item.stroke_width;
            out.fill = item.fill.into();
            out.fill_style = snow_fill_style_from_rust(item.fill_style);
            out.arrow_type = snow_arrow_type_from_rust(item.arrow_type);
            out.is_free_draw = u8::from(item.is_free_draw);
            out.arrow_start_head = snow_arrowhead_from_rust(item.start_arrowhead);
            out.arrow_end_head = snow_arrowhead_from_rust(item.end_arrowhead);
            out.arrow_stroke_style = snow_arrow_stroke_style_from_rust(item.stroke_style);
            out.opacity = item.opacity;
            out.blend_mode = match item.blend_mode {
                snow_draw_engine::DisplayBlendMode::Normal => SnowBlendMode::Normal,
                snow_draw_engine::DisplayBlendMode::Multiply => SnowBlendMode::Multiply,
            };

            if !omit_arrow_points {
                converted.arrow_points =
                    snow_arrow_points_from_rust(&item.points).into_boxed_slice();
                out.arrow_point_count = converted.arrow_points.len() as u32;
            }
            if !omit_path_commands {
                converted.arrow_path_commands =
                    snow_arrow_path_commands_from_rust(&item.path_commands).into_boxed_slice();
                out.arrow_path_command_count = converted.arrow_path_commands.len() as u32;
            }
            converted.arrowhead_primitives =
                snow_arrowhead_primitives_from_rust(&item.arrowhead_primitives).into_boxed_slice();
            out.arrowhead_primitive_count = converted.arrowhead_primitives.len() as u32;
        }
        SceneDisplayItem::Text(item) => {
            out.kind = SnowSceneDisplayItemKind::Text;
            out.element_id = SnowElementId {
                index: item.id.index,
                generation: item.id.generation,
            };
            out.center_x = item.center_x;
            out.center_y = item.center_y;
            out.width = item.width;
            out.height = item.height;
            out.rotation = item.rotation;
            out.fill = item.fill.into();
            out.stroke = item.stroke.into();
            out.text_color = item.color.into();
            out.stroke_width = item.stroke_width;
            out.corner_radii = item.corner_radii.into();
            out.font_size = item.font_size;
            out.opacity = item.opacity;
            out.text_horizontal_align = snow_text_horizontal_align_from_rust(item.horizontal_align);
            out.text_vertical_align = snow_text_vertical_align_from_rust(item.vertical_align);
            out.fill_style = snow_fill_style_from_rust(item.fill_style);
            out.stroke_style = SnowStrokeStyle::Solid;
            converted.text_utf8 = utf8_bytes(Some(&item.text));
            out.text_utf8_len = converted.text_utf8.len() as u32;
            converted.font_family_utf8 = utf8_bytes(item.font_family.as_deref());
            out.font_family_utf8_len = converted.font_family_utf8.len() as u32;
        }
        SceneDisplayItem::SerialNumber(item) => {
            out.kind = SnowSceneDisplayItemKind::SerialNumber;
            out.element_id = SnowElementId {
                index: item.id.index,
                generation: item.id.generation,
            };
            out.center_x = item.center_x;
            out.center_y = item.center_y;
            out.width = item.diameter;
            out.height = item.diameter;
            out.rotation = item.rotation;
            out.fill = item.fill.into();
            out.stroke = item.color.into();
            out.text_color = item.color.into();
            out.stroke_width = item.stroke_width;
            out.font_size = item.font_size;
            out.opacity = item.opacity;
            out.serial_number = item.number;
            out.fill_style = snow_fill_style_from_rust(item.fill_style);
            out.stroke_style = snow_stroke_style_from_rust(item.stroke_style);
            converted.font_family_utf8 = utf8_bytes(item.font_family.as_deref());
            out.font_family_utf8_len = converted.font_family_utf8.len() as u32;
            encode_bound_text_id(out, item.bound_text_id);
        }
        SceneDisplayItem::SerialNumberConnector(item) => {
            out.kind = SnowSceneDisplayItemKind::SerialNumberConnector;
            out.element_id = SnowElementId {
                index: item.id.index,
                generation: item.id.generation,
            };
            out.center_x = item.start_x;
            out.center_y = item.start_y;
            out.width = item.end_x;
            out.height = item.end_y;
            out.stroke = item.stroke.into();
            out.stroke_width = item.stroke_width;
            out.opacity = item.opacity;
            if item.has_baseline {
                converted.arrow_points = vec![
                    SnowArrowPoint {
                        x: item.baseline_start_x,
                        y: item.baseline_start_y,
                    },
                    SnowArrowPoint {
                        x: item.baseline_end_x,
                        y: item.baseline_end_y,
                    },
                ]
                .into_boxed_slice();
                out.arrow_point_count = 2;
            }
        }
        SceneDisplayItem::Stroke => out.kind = SnowSceneDisplayItemKind::Stroke,
        SceneDisplayItem::Image => out.kind = SnowSceneDisplayItemKind::Image,
    }
    converted.refresh_pointers();
    converted
}

pub(crate) fn snow_scene_patch_item_from_rust(
    value: &SceneDisplayItem,
    omit_path_commands: bool,
) -> SnowScenePatchItem {
    let omit_arrow_points =
        matches!(value, SceneDisplayItem::Filter(filter) if filter.is_pen_filter);
    snow_scene_display_item_from_rust(value, omit_arrow_points, omit_path_commands)
}

pub(crate) fn snow_overlay_display_item_from_rust(
    value: &OverlayDisplayItem,
) -> SnowOverlayPatchItem {
    let mut converted = SnowOverlayPatchItem::default();
    let out = &mut converted.view;
    match value {
        OverlayDisplayItem::Rectangle(item) => {
            out.kind = SnowOverlayDisplayItemKind::DrawRect;
            out.rect_kind = snow_overlay_rect_kind_from_rust(item.kind);
            out.center_x = item.center_x;
            out.center_y = item.center_y;
            out.width = item.width;
            out.height = item.height;
            out.rotation = item.rotation;
            out.fill = item.fill.into();
            out.stroke = item.stroke.into();
            out.stroke_width = item.stroke_width;
            out.corner_radii = item.corner_radii.into();
            out.fill_style = snow_fill_style_from_rust(item.fill_style);
        }
        OverlayDisplayItem::FocusConnection(item) => {
            out.kind = SnowOverlayDisplayItemKind::FocusConnection;
            out.stroke = item.stroke.into();
            out.stroke_width = item.stroke_width;
            out.arrow_type = snow_arrow_type_from_rust(item.arrow_type);
            out.arrow_start_head = snow_arrowhead_from_rust(item.start_arrowhead);
            out.arrow_end_head = snow_arrowhead_from_rust(item.end_arrowhead);
            out.arrow_stroke_style = snow_arrow_stroke_style_from_rust(item.stroke_style);
            converted.arrow_points = snow_arrow_points_from_rust(&item.points).into_boxed_slice();
            out.arrow_point_count = converted.arrow_points.len() as u32;
            converted.arrow_path_commands =
                snow_arrow_path_commands_from_rust(&item.path_commands).into_boxed_slice();
            out.arrow_path_command_count = converted.arrow_path_commands.len() as u32;
            converted.arrowhead_primitives =
                snow_arrowhead_primitives_from_rust(&item.arrowhead_primitives).into_boxed_slice();
            out.arrowhead_primitive_count = converted.arrowhead_primitives.len() as u32;
        }
        OverlayDisplayItem::PenFilterContour(item) => {
            out.kind = SnowOverlayDisplayItemKind::PenFilterContour;
            out.stroke = item.stroke.into();
            out.stroke_width = item.stroke_width;
            converted.arrow_points = snow_arrow_points_from_rust(&item.points).into_boxed_slice();
            out.arrow_point_count = converted.arrow_points.len() as u32;
            converted.arrow_path_commands =
                snow_arrow_path_commands_from_rust(&item.path_commands).into_boxed_slice();
            out.arrow_path_command_count = converted.arrow_path_commands.len() as u32;
        }
        OverlayDisplayItem::SnapGuide(item) => {
            out.kind = SnowOverlayDisplayItemKind::SnapGuide;
            out.snap_guide_kind = match item.kind {
                SnapGuideKind::Point => SnowSnapGuideKind::Point,
                SnapGuideKind::Gap => SnowSnapGuideKind::Gap,
            };
            out.snap_guide_axis = match item.axis {
                SnapGuideAxis::Horizontal => SnowSnapGuideAxis::Horizontal,
                SnapGuideAxis::Vertical => SnowSnapGuideAxis::Vertical,
            };
            out.snap_marker_count = item.marker_count;
            out.snap_has_label = u8::from(item.label.is_some());
            out.snap_start_x = item.start.x;
            out.snap_start_y = item.start.y;
            out.snap_end_x = item.end.x;
            out.snap_end_y = item.end.y;
            out.snap_marker0_x = item.markers[0].x;
            out.snap_marker0_y = item.markers[0].y;
            out.snap_marker1_x = item.markers[1].x;
            out.snap_marker1_y = item.markers[1].y;
            out.snap_label = item.label.unwrap_or(0.0);
            out.snap_color = item.color.into();
            out.snap_line_width = item.line_width;
            out.snap_marker_size = item.marker_size;
            out.snap_gap_dash_length = item.gap_dash_length;
            out.snap_gap_dash_gap = item.gap_dash_gap;
        }
    }
    converted.refresh_pointers();
    converted
}

fn snow_overlay_rect_kind_from_rust(value: UiRectKind) -> SnowOverlayRectKind {
    match value {
        UiRectKind::SelectionMarquee => SnowOverlayRectKind::SelectionMarquee,
        UiRectKind::SelectionCandidateFrame => SnowOverlayRectKind::SelectionCandidateFrame,
        UiRectKind::SelectionFrame => SnowOverlayRectKind::SelectionFrame,
        UiRectKind::SelectionMultiFrame => SnowOverlayRectKind::SelectionMultiFrame,
        UiRectKind::TextActualFrame => SnowOverlayRectKind::TextActualFrame,
        UiRectKind::TextHoverUnderline => SnowOverlayRectKind::TextHoverUnderline,
        UiRectKind::SelectionResizeHandle => SnowOverlayRectKind::SelectionResizeHandle,
        UiRectKind::SelectionRotationHandle => SnowOverlayRectKind::SelectionRotationHandle,
        UiRectKind::SelectionCornerRadiusHandle => SnowOverlayRectKind::SelectionCornerRadiusHandle,
        UiRectKind::ArrowEndpointHandle => SnowOverlayRectKind::ArrowEndpointHandle,
        UiRectKind::ArrowFocusHandle => SnowOverlayRectKind::ArrowFocusHandle,
        UiRectKind::ArrowSegmentHandle => SnowOverlayRectKind::ArrowSegmentHandle,
        UiRectKind::EraserCursor => SnowOverlayRectKind::EraserCursor,
    }
}

pub(crate) fn snow_patch_cursor_to_rust(value: SnowPatchCursor) -> PatchCursor {
    PatchCursor {
        scene_revision: snow_draw_engine::SceneRevision(value.scene_revision),
        decoration_revision: snow_draw_engine::DecorationRevision(value.decoration_revision),
        overlay_revision: snow_draw_engine::OverlayRevision(value.overlay_revision),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::abi::patch::SnowPatchPayload;
    use snow_draw_engine::{
        ArrowDisplayItem, ArrowPathCommand, ArrowheadDisplayDashMode, ArrowheadDisplayFillMode,
        ArrowheadDisplayPrimitive, ArrowheadDisplayPrimitiveKind, ColorRgba8, DecorationPatch,
        DecorationView, DirtyRegion, DisplayFillStyle, DisplayFilterType, DisplaySpotlightConfig,
        DisplaySpotlightCutout, DisplayStrokeStyle, DisplayWatermarkConfig, FilterDisplayItem,
        FrameView, LayerPatch, RectangleDisplayItem, ReplaceRangeOp, SerialNumberDisplayItem,
        TextDisplayItem, UiFocusConnectionDisplayItem, UiRectangleDisplayItem, ViewportPatch,
    };

    #[test]
    fn patch_protocol_layout_and_decoration_payload_are_stable() {
        assert_eq!(std::mem::size_of::<SnowPatchCursor>(), 24);
        assert_eq!(
            std::mem::offset_of!(SnowPatchCursor, decoration_revision),
            8
        );
        assert_eq!(std::mem::offset_of!(SnowPatchCursor, overlay_revision), 16);
        assert_eq!(
            std::mem::offset_of!(SnowPatchInfo, decoration_base_revision),
            16
        );
        assert_eq!(std::mem::offset_of!(SnowPatchInfo, decoration_revision), 24);
        assert_eq!(
            std::mem::offset_of!(SnowPatchInfo, decoration_dirty_rect_count),
            84
        );
        assert_eq!(std::mem::size_of::<SnowPatchInfo>(), 584);

        let mut watermark = DisplayWatermarkConfig::default();
        watermark.text[..3].copy_from_slice(b"ABI");
        watermark.text_len = 3;
        let patch = ViewportPatch {
            path_geometry_ops: Vec::new(),
            pen_filter_geometry_ops: Vec::new(),
            frame_view: FrameView::default(),
            decoration: DecorationPatch {
                base_revision: 11,
                revision: 12,
                reset: false,
                view: DecorationView {
                    watermark,
                    spotlight: DisplaySpotlightConfig {
                        color: ColorRgba8 {
                            r: 0x40,
                            g: 0x80,
                            b: 0xc0,
                            a: 0xff,
                        },
                        opacity: 0.64,
                        active: true,
                    },
                },
                spotlight_ops: vec![ReplaceRangeOp {
                    start: 2,
                    delete_count: 1,
                    insert_items: vec![DisplaySpotlightCutout {
                        center_x: 10.0,
                        center_y: 20.0,
                        width: 30.0,
                        height: 40.0,
                        rotation: 0.5,
                    }],
                }],
                dirty_regions: vec![DirtyRegion::new(1.0, 2.0, 30.0, 40.0)],
            },
            scene: LayerPatch::default(),
            overlay: LayerPatch::default(),
        };

        let info = snow_patch_info_from_rust(&patch);
        assert_eq!(info.decoration_base_revision, 11);
        assert_eq!(info.decoration_revision, 12);
        assert_eq!(info.decoration_reset, 0);
        assert_eq!(info.decoration_dirty_rect_count, 1);
        assert_eq!(info.watermark_text_len, 3);
        assert_eq!(&info.watermark_text[..3], b"ABI");
        assert_eq!(info.spotlight_color.r, 0x40);
        assert_eq!(info.spotlight_color.g, 0x80);
        assert_eq!(info.spotlight_color.b, 0xc0);
        assert_eq!(info.spotlight_color.a, 0xff);
        assert_eq!(info.spotlight_opacity, 0.64);
        assert_eq!(info.spotlight_active, 1);
        assert_eq!(info.spotlight_op_count, 1);
        assert_eq!(info.spotlight_item_count, 1);

        let payload = SnowPatchPayload::from_patch(&patch);
        assert_eq!(payload.decoration_dirty_rects.len(), 1);
        assert_eq!(payload.decoration_dirty_rects[0].min_x, 1.0);
        assert_eq!(payload.decoration_dirty_rects[0].max_y, 40.0);
        assert_eq!(payload.spotlight_ops.len(), 1);
        assert_eq!(payload.spotlight_ops[0].start, 2);
        assert_eq!(payload.spotlight_ops[0].delete_count, 1);
        assert_eq!(payload.spotlight_ops[0].insert_offset, 0);
        assert_eq!(payload.spotlight_ops[0].insert_count, 1);
        assert_eq!(payload.spotlight_cutouts.len(), 1);
        assert_eq!(payload.spotlight_cutouts[0].center_x, 10.0);
        assert_eq!(payload.spotlight_cutouts[0].rotation, 0.5);
    }

    #[test]
    fn patch_cursor_conversion_preserves_all_three_revisions() {
        let cursor = SnowPatchCursor {
            scene_revision: 7,
            decoration_revision: 8,
            overlay_revision: 9,
        };
        let converted = snow_patch_cursor_to_rust(cursor);
        assert_eq!(converted.scene_revision.0, 7);
        assert_eq!(converted.decoration_revision.0, 8);
        assert_eq!(converted.overlay_revision.0, 9);
    }

    #[test]
    fn compact_display_views_stay_below_the_abi_size_budget() {
        assert!(std::mem::size_of::<SnowSceneDisplayItem>() <= 320);
        assert!(std::mem::size_of::<SnowOverlayDisplayItem>() <= 384);
    }

    #[test]
    fn ui_rectangle_display_item_exports_fill_style() {
        let item = snow_overlay_display_item_from_rust(&OverlayDisplayItem::Rectangle(
            UiRectangleDisplayItem {
                kind: UiRectKind::SelectionCandidateFrame,
                width: 80.0,
                height: 60.0,
                fill_style: DisplayFillStyle::CrossLine,
                stroke_width: 2.0,
                ..UiRectangleDisplayItem::default()
            },
        ));
        assert_eq!(item.view.fill_style, SnowFillStyle::CrossLine);
    }

    #[test]
    fn text_hover_underline_exports_its_dedicated_rectangle_kind() {
        let item = snow_overlay_display_item_from_rust(&OverlayDisplayItem::Rectangle(
            UiRectangleDisplayItem {
                kind: UiRectKind::TextHoverUnderline,
                ..UiRectangleDisplayItem::default()
            },
        ));

        assert_eq!(item.view.kind, SnowOverlayDisplayItemKind::DrawRect);
        assert_eq!(item.view.rect_kind, SnowOverlayRectKind::TextHoverUnderline);
    }

    #[test]
    fn pen_filter_contour_exports_its_path_and_stroke_width() {
        let item = snow_overlay_display_item_from_rust(&OverlayDisplayItem::PenFilterContour(
            UiFocusConnectionDisplayItem {
                points: vec![[10.0, 20.0], [50.0, 60.0]],
                path_commands: vec![
                    ArrowPathCommand::MoveTo {
                        point: [10.0, 20.0],
                    },
                    ArrowPathCommand::LineTo {
                        point: [50.0, 60.0],
                    },
                ],
                stroke_width: 30.0,
                ..UiFocusConnectionDisplayItem::default()
            },
        ));

        assert_eq!(item.view.kind, SnowOverlayDisplayItemKind::PenFilterContour);
        assert_eq!(item.view.stroke_width, 30.0);
        assert_eq!(item.view.arrow_point_count, 2);
        assert_eq!(item.view.arrow_path_command_count, 2);
    }

    #[test]
    fn text_display_view_preserves_full_utf8_content() {
        let text = "text".repeat(SNOW_TEXT_UTF8_CAPACITY);
        let item = snow_scene_display_item_from_rust(
            &SceneDisplayItem::Text(TextDisplayItem {
                text: text.clone(),
                ..TextDisplayItem::default()
            }),
            false,
            false,
        );
        assert_eq!(item.view.text_utf8_len as usize, text.len());
        let bytes =
            unsafe { std::slice::from_raw_parts(item.view.text_utf8.cast::<u8>(), text.len()) };
        assert_eq!(bytes, text.as_bytes());
    }

    #[test]
    fn rectangle_display_item_exports_stroke_style() {
        let item = snow_scene_display_item_from_rust(
            &SceneDisplayItem::Rectangle(RectangleDisplayItem {
                stroke_style: DisplayStrokeStyle::Dashed,
                ..RectangleDisplayItem::default()
            }),
            false,
            false,
        );

        assert_eq!(item.view.stroke_style, SnowStrokeStyle::Dashed);
    }

    #[test]
    fn filter_display_item_exports_effect_strength_opacity_and_geometry() {
        let item = snow_scene_display_item_from_rust(
            &SceneDisplayItem::Filter(FilterDisplayItem {
                id: DisplayItemId {
                    index: 17,
                    generation: 4,
                },
                center_x: 21.0,
                center_y: 34.0,
                width: 55.0,
                height: 89.0,
                rotation: 0.75,
                filter: snow_draw_engine::FilterRenderSpec::resolve(
                    DisplayFilterType::GaussianBlur,
                    0.625,
                ),
                opacity: 0.4,
                ..FilterDisplayItem::default()
            }),
            false,
            false,
        );

        assert_eq!(item.view.kind, SnowSceneDisplayItemKind::Filter);
        assert_eq!(item.view.element_id.index, 17);
        assert_eq!(item.view.element_id.generation, 4);
        assert_eq!(item.view.filter.filter_type, 1);
        assert_eq!(item.view.filter.strength, 0.625);
        assert_eq!(item.view.filter.mosaic_block_size, 8.25);
        assert_eq!(item.view.filter.blur_sigma, 11.75);
        assert_eq!(item.view.filter.sampling_radius, 36.25);
        assert_eq!(item.view.opacity, 0.4);
        assert_eq!((item.view.center_x, item.view.center_y), (21.0, 34.0));
        assert_eq!((item.view.width, item.view.height), (55.0, 89.0));
        assert_eq!(item.view.rotation, 0.75);
    }

    #[test]
    fn pen_filter_display_item_reuses_the_filter_point_slice_and_width_fields() {
        let points = vec![[1.0, 2.0], [3.0, 5.0], [8.0, 13.0]];
        let item = snow_scene_display_item_from_rust(
            &SceneDisplayItem::Filter(FilterDisplayItem {
                points: points.clone().into(),
                stroke_width: 12.0,
                is_pen_filter: true,
                filter: snow_draw_engine::FilterRenderSpec::resolve(
                    DisplayFilterType::Inversion,
                    1.0,
                ),
                ..FilterDisplayItem::default()
            }),
            false,
            false,
        );

        assert_eq!(item.view.kind, SnowSceneDisplayItemKind::Filter);
        assert_eq!(item.view.is_free_draw, 1);
        assert_eq!(item.view.stroke_width, 12.0);
        assert_eq!(item.view.arrow_point_count, points.len() as u32);
        assert!(!item.view.arrow_points.is_null());
        let exported = unsafe {
            std::slice::from_raw_parts(item.view.arrow_points, item.view.arrow_point_count as usize)
        };
        assert_eq!(
            exported
                .iter()
                .map(|point| [point.x, point.y])
                .collect::<Vec<_>>(),
            points
        );
    }

    #[test]
    fn arrow_display_view_preserves_dynamic_outer_slices() {
        let primitive = ArrowheadDisplayPrimitive {
            kind: ArrowheadDisplayPrimitiveKind::Line,
            points: vec![[1.0, 2.0]; SNOW_ARROWHEAD_PRIMITIVE_POINT_CAPACITY + 1],
            center: [3.0, 4.0],
            diameter: 5.0,
            fill_mode: ArrowheadDisplayFillMode::Stroke,
            dash_mode: ArrowheadDisplayDashMode::Inherit,
        };
        let item = snow_scene_display_item_from_rust(
            &SceneDisplayItem::Arrow(ArrowDisplayItem {
                points: vec![[6.0, 7.0]; 65],
                path_commands: vec![ArrowPathCommand::MoveTo { point: [8.0, 9.0] }; 129],
                arrowhead_primitives: vec![primitive; 17],
                fill: snow_draw_engine::ColorRgba8 {
                    r: 10,
                    g: 20,
                    b: 30,
                    a: 40,
                },
                fill_style: snow_draw_engine::DisplayFillStyle::CrossLine,
                is_free_draw: true,
                ..ArrowDisplayItem::default()
            }),
            false,
            false,
        );

        assert_eq!(item.view.arrow_point_count, 65);
        assert_eq!(item.view.arrow_path_command_count, 129);
        assert_eq!(item.view.arrowhead_primitive_count, 17);
        assert_eq!(
            item.view.fill,
            SnowColorRgba8 {
                r: 10,
                g: 20,
                b: 30,
                a: 40
            }
        );
        assert_eq!(item.view.fill_style, SnowFillStyle::CrossLine);
        assert_eq!(item.view.is_free_draw, 1);
        let primitives = unsafe {
            std::slice::from_raw_parts(
                item.view.arrowhead_primitives,
                item.view.arrowhead_primitive_count as usize,
            )
        };
        assert_eq!(
            primitives.last().unwrap().point_count as usize,
            SNOW_ARROWHEAD_PRIMITIVE_POINT_CAPACITY
        );
    }

    #[test]
    fn serial_number_display_item_exports_explicit_bound_text_id() {
        let item = snow_scene_display_item_from_rust(
            &SceneDisplayItem::SerialNumber(SerialNumberDisplayItem {
                id: DisplayItemId {
                    index: 3,
                    generation: 11,
                },
                bound_text_id: Some(DisplayItemId {
                    index: 42,
                    generation: 7,
                }),
                ..SerialNumberDisplayItem::default()
            }),
            false,
            false,
        );
        assert_eq!(item.view.has_bound_text_element, 1);
        assert_eq!(item.view.bound_text_element_index, 42);
        assert_eq!(item.view.bound_text_element_generation, 7);
    }

    #[test]
    fn serial_number_display_item_clears_absent_bound_text_id() {
        let item = snow_scene_display_item_from_rust(
            &SceneDisplayItem::SerialNumber(SerialNumberDisplayItem {
                bound_text_id: None,
                ..SerialNumberDisplayItem::default()
            }),
            false,
            false,
        );
        assert_eq!(item.view.has_bound_text_element, 0);
    }
}
