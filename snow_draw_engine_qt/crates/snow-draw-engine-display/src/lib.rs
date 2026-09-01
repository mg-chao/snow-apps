use snow_draw_engine_core::{
    Camera, ColorRgba8, CornerRadii, PathGeometry, Point, SnapGuideAxis, SnapGuideKind,
    SurfaceSize,
    arrow::{ArrowPathCommand, ArrowType, Arrowhead, StrokeStyle},
};
use std::sync::Arc;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct DisplayItemId {
    pub index: u32,
    pub generation: u32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SceneRevision(pub u64);

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct OverlayRevision(pub u64);

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DecorationRevision(pub u64);

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PatchCursor {
    pub scene_revision: SceneRevision,
    pub decoration_revision: DecorationRevision,
    pub overlay_revision: OverlayRevision,
}

pub const WATERMARK_TEXT_CAPACITY: usize = 256;
pub const WATERMARK_FONT_FAMILY_CAPACITY: usize = 128;

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DisplayWatermarkConfig {
    pub color: ColorRgba8,
    pub text_len: u16,
    pub text: [u8; WATERMARK_TEXT_CAPACITY],
    pub font_size: f64,
    pub font_family_len: u16,
    pub font_family: [u8; WATERMARK_FONT_FAMILY_CAPACITY],
    pub angle: f64,
    pub gap: f64,
    pub opacity: f64,
}

impl Default for DisplayWatermarkConfig {
    fn default() -> Self {
        Self {
            color: ColorRgba8::default(),
            text_len: 0,
            text: [0; WATERMARK_TEXT_CAPACITY],
            font_size: 12.0,
            font_family_len: 0,
            font_family: [0; WATERMARK_FONT_FAMILY_CAPACITY],
            angle: 30.0,
            gap: 56.0,
            opacity: 0.16,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FrameView {
    pub surface: SurfaceSize,
    pub camera: Camera,
    pub clear_color: ColorRgba8,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DisplaySpotlightConfig {
    pub color: ColorRgba8,
    pub opacity: f64,
    pub active: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct DisplaySpotlightCutout {
    pub center_x: f64,
    pub center_y: f64,
    pub width: f64,
    pub height: f64,
    pub rotation: f64,
}

impl Default for DisplaySpotlightConfig {
    fn default() -> Self {
        Self {
            color: ColorRgba8 {
                r: 0,
                g: 0,
                b: 0,
                a: 0xff,
            },
            opacity: 0.64,
            active: false,
        }
    }
}

impl Default for FrameView {
    fn default() -> Self {
        Self {
            surface: SurfaceSize::default(),
            camera: Camera {
                center: Point::default(),
                zoom: 1.0,
            },
            clear_color: ColorRgba8::default(),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct DecorationView {
    pub watermark: DisplayWatermarkConfig,
    pub spotlight: DisplaySpotlightConfig,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct DirtyRegion {
    pub min_x: f64,
    pub min_y: f64,
    pub max_x: f64,
    pub max_y: f64,
}

impl DirtyRegion {
    pub const fn new(min_x: f64, min_y: f64, max_x: f64, max_y: f64) -> Self {
        Self {
            min_x,
            min_y,
            max_x,
            max_y,
        }
    }

    pub fn is_empty(self) -> bool {
        self.max_x <= self.min_x || self.max_y <= self.min_y
    }

    pub fn union(self, other: Self) -> Self {
        Self::new(
            self.min_x.min(other.min_x),
            self.min_y.min(other.min_y),
            self.max_x.max(other.max_x),
            self.max_y.max(other.max_y),
        )
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum DisplayBlendMode {
    #[default]
    Normal,
    Multiply,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RectangleDisplayItem {
    pub id: DisplayItemId,
    pub center_x: f64,
    pub center_y: f64,
    pub width: f64,
    pub height: f64,
    pub rotation: f64,
    pub fill: ColorRgba8,
    pub fill_style: DisplayFillStyle,
    pub stroke: ColorRgba8,
    pub stroke_width: f64,
    pub stroke_style: StrokeStyle,
    pub corner_radii: CornerRadii,
    pub opacity: f64,
    pub shape: DisplayRectangleShape,
    pub blend_mode: DisplayBlendMode,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum DisplayFilterType {
    #[default]
    Mosaic,
    GaussianBlur,
    Grayscale,
    Inversion,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FilterRenderSpec {
    pub filter_type: DisplayFilterType,
    pub strength: f64,
    pub mosaic_block_size: f64,
    pub blur_sigma: f64,
    pub sampling_radius: f64,
}

impl FilterRenderSpec {
    pub fn resolve(filter_type: DisplayFilterType, strength: f64) -> Self {
        let normalized_strength = if strength.is_nan() {
            1.0
        } else if strength.is_finite() {
            strength.clamp(0.0, 1.0)
        } else if strength.is_sign_negative() {
            0.0
        } else {
            1.0
        };
        let strength = match filter_type {
            DisplayFilterType::Grayscale | DisplayFilterType::Inversion => 1.0,
            DisplayFilterType::Mosaic | DisplayFilterType::GaussianBlur => normalized_strength,
        };
        let mosaic_block_size = 2.0 + 10.0 * strength;
        let blur_sigma = 0.5 + 18.0 * strength;
        let sampling_radius = match filter_type {
            DisplayFilterType::Mosaic => (mosaic_block_size + 1.0) / 2.0,
            DisplayFilterType::GaussianBlur => 3.0 * blur_sigma + 1.0,
            DisplayFilterType::Grayscale | DisplayFilterType::Inversion => 0.0,
        };
        Self {
            filter_type,
            strength,
            mosaic_block_size,
            blur_sigma,
            sampling_radius,
        }
    }
}

impl Default for FilterRenderSpec {
    fn default() -> Self {
        Self::resolve(DisplayFilterType::Mosaic, 0.0)
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct FilterDisplayItem {
    pub id: DisplayItemId,
    pub center_x: f64,
    pub center_y: f64,
    pub width: f64,
    pub height: f64,
    pub rotation: f64,
    pub points: Arc<[[f64; 2]]>,
    pub stroke_width: f64,
    pub is_pen_filter: bool,
    pub filter: FilterRenderSpec,
    pub opacity: f64,
}

#[cfg(test)]
mod filter_render_spec_tests {
    use super::{DisplayFilterType, FilterRenderSpec};

    #[test]
    fn resolves_canonical_filter_parameters() {
        let mosaic = FilterRenderSpec::resolve(DisplayFilterType::Mosaic, 0.5);
        assert_eq!(mosaic.strength, 0.5);
        assert_eq!(mosaic.mosaic_block_size, 7.0);
        assert_eq!(mosaic.blur_sigma, 9.5);
        assert_eq!(mosaic.sampling_radius, 4.0);

        let blur = FilterRenderSpec::resolve(DisplayFilterType::GaussianBlur, 1.0);
        assert_eq!(blur.blur_sigma, 18.5);
        assert_eq!(blur.sampling_radius, 56.5);
    }

    #[test]
    fn normalizes_non_finite_and_out_of_range_strengths_for_strength_based_filters() {
        assert_eq!(
            FilterRenderSpec::resolve(DisplayFilterType::Mosaic, f64::NAN).strength,
            1.0
        );
        assert_eq!(
            FilterRenderSpec::resolve(DisplayFilterType::Mosaic, f64::NEG_INFINITY).strength,
            0.0
        );
        assert_eq!(
            FilterRenderSpec::resolve(DisplayFilterType::Mosaic, f64::INFINITY).strength,
            1.0
        );
        assert_eq!(
            FilterRenderSpec::resolve(DisplayFilterType::Mosaic, -2.0).strength,
            0.0
        );
        assert_eq!(
            FilterRenderSpec::resolve(DisplayFilterType::Mosaic, 2.0).strength,
            1.0
        );
    }

    #[test]
    fn grayscale_and_inversion_ignore_strength() {
        for filter_type in [DisplayFilterType::Grayscale, DisplayFilterType::Inversion] {
            for strength in [0.0, 0.5, 1.0, f64::NAN, f64::NEG_INFINITY, f64::INFINITY] {
                let spec = FilterRenderSpec::resolve(filter_type, strength);
                assert_eq!(spec.strength, 1.0);
                assert_eq!(spec.sampling_radius, 0.0);
            }
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum DisplayRectangleShape {
    #[default]
    Rectangle,
    Ellipse,
    Diamond,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ArrowheadDisplayPrimitiveKind {
    Line,
    Polygon,
    Circle,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ArrowheadDisplayFillMode {
    Stroke,
    Background,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ArrowheadDisplayDashMode {
    Inherit,
    Solid,
    DottedCap,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ArrowheadDisplayPrimitive {
    pub kind: ArrowheadDisplayPrimitiveKind,
    pub points: Vec<[f64; 2]>,
    pub center: [f64; 2],
    pub diameter: f64,
    pub fill_mode: ArrowheadDisplayFillMode,
    pub dash_mode: ArrowheadDisplayDashMode,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ArrowDisplayItem {
    pub id: DisplayItemId,
    pub points: Vec<[f64; 2]>,
    pub path_commands: Vec<ArrowPathCommand>,
    pub geometry: Arc<PathGeometry>,
    pub arrow_type: ArrowType,
    pub start_arrowhead: Option<Arrowhead>,
    pub end_arrowhead: Option<Arrowhead>,
    pub stroke: ColorRgba8,
    pub stroke_width: f64,
    pub stroke_style: StrokeStyle,
    pub fill: ColorRgba8,
    pub fill_style: DisplayFillStyle,
    pub arrowhead_primitives: Vec<ArrowheadDisplayPrimitive>,
    pub opacity: f64,
    pub is_free_draw: bool,
    pub blend_mode: DisplayBlendMode,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum DisplayTextHorizontalAlign {
    #[default]
    Left,
    Center,
    Right,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum DisplayTextVerticalAlign {
    Top,
    #[default]
    Center,
    Bottom,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum DisplayFillStyle {
    Line,
    CrossLine,
    #[default]
    Solid,
}

#[derive(Clone, Debug, PartialEq)]
pub struct TextDisplayItem {
    pub id: DisplayItemId,
    pub center_x: f64,
    pub center_y: f64,
    pub width: f64,
    pub height: f64,
    pub rotation: f64,
    pub text: String,
    pub color: ColorRgba8,
    pub font_size: f64,
    pub font_family: Option<String>,
    pub fill: ColorRgba8,
    pub fill_style: DisplayFillStyle,
    pub stroke: ColorRgba8,
    pub stroke_width: f64,
    pub corner_radii: CornerRadii,
    pub horizontal_align: DisplayTextHorizontalAlign,
    pub vertical_align: DisplayTextVerticalAlign,
    pub opacity: f64,
}

#[derive(Clone, Debug, PartialEq)]
pub struct SerialNumberDisplayItem {
    pub id: DisplayItemId,
    pub center_x: f64,
    pub center_y: f64,
    pub diameter: f64,
    pub rotation: f64,
    pub number: i64,
    pub color: ColorRgba8,
    pub fill: ColorRgba8,
    pub fill_style: DisplayFillStyle,
    pub font_size: f64,
    pub font_family: Option<String>,
    pub stroke_width: f64,
    pub stroke_style: StrokeStyle,
    pub opacity: f64,
    pub bound_text_id: Option<DisplayItemId>,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SerialNumberConnectorDisplayItem {
    pub id: DisplayItemId,
    pub start_x: f64,
    pub start_y: f64,
    pub end_x: f64,
    pub end_y: f64,
    pub baseline_start_x: f64,
    pub baseline_start_y: f64,
    pub baseline_end_x: f64,
    pub baseline_end_y: f64,
    pub has_baseline: bool,
    pub stroke: ColorRgba8,
    pub stroke_width: f64,
    pub opacity: f64,
}

impl Default for ArrowDisplayItem {
    fn default() -> Self {
        Self {
            id: DisplayItemId::default(),
            points: Vec::new(),
            path_commands: Vec::new(),
            geometry: Arc::new(PathGeometry::default()),
            arrow_type: ArrowType::Straight,
            start_arrowhead: None,
            end_arrowhead: None,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: StrokeStyle::Solid,
            fill: ColorRgba8::default(),
            fill_style: DisplayFillStyle::Solid,
            arrowhead_primitives: Vec::new(),
            opacity: 1.0,
            is_free_draw: false,
            blend_mode: DisplayBlendMode::Normal,
        }
    }
}

impl Default for RectangleDisplayItem {
    fn default() -> Self {
        Self {
            id: DisplayItemId::default(),
            center_x: 0.0,
            center_y: 0.0,
            width: 0.0,
            height: 0.0,
            rotation: 0.0,
            fill: ColorRgba8::default(),
            fill_style: DisplayFillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
            shape: DisplayRectangleShape::Rectangle,
            blend_mode: DisplayBlendMode::Normal,
        }
    }
}

impl Default for TextDisplayItem {
    fn default() -> Self {
        Self {
            id: DisplayItemId::default(),
            center_x: 0.0,
            center_y: 0.0,
            width: 0.0,
            height: 0.0,
            rotation: 0.0,
            text: String::new(),
            color: ColorRgba8::default(),
            font_size: 0.0,
            font_family: None,
            fill: ColorRgba8::default(),
            fill_style: DisplayFillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            corner_radii: CornerRadii::default(),
            horizontal_align: DisplayTextHorizontalAlign::Left,
            vertical_align: DisplayTextVerticalAlign::Center,
            opacity: 1.0,
        }
    }
}

impl Default for SerialNumberDisplayItem {
    fn default() -> Self {
        Self {
            id: DisplayItemId::default(),
            center_x: 0.0,
            center_y: 0.0,
            diameter: 0.0,
            rotation: 0.0,
            number: 0,
            color: ColorRgba8::default(),
            fill: ColorRgba8::default(),
            fill_style: DisplayFillStyle::Solid,
            font_size: 0.0,
            font_family: None,
            stroke_width: 0.0,
            stroke_style: StrokeStyle::Solid,
            opacity: 1.0,
            bound_text_id: None,
        }
    }
}

impl Default for SerialNumberConnectorDisplayItem {
    fn default() -> Self {
        Self {
            id: DisplayItemId::default(),
            start_x: 0.0,
            start_y: 0.0,
            end_x: 0.0,
            end_y: 0.0,
            baseline_start_x: 0.0,
            baseline_start_y: 0.0,
            baseline_end_x: 0.0,
            baseline_end_y: 0.0,
            has_baseline: false,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            opacity: 1.0,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UiShapeKind {
    SelectionMarquee,
    SelectionCandidateFrame,
    SelectionFrame,
    SelectionMultiFrame,
    TextActualFrame,
    TextHoverUnderline,
    SelectionResizeHandle,
    SelectionRotationHandle,
    SelectionCornerRadiusHandle,
    ArrowEndpointHandle,
    ArrowFocusHandle,
    ArrowSegmentHandle,
    EraserCursor,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct UiRectangleDisplayItem {
    pub kind: UiShapeKind,
    pub center_x: f64,
    pub center_y: f64,
    pub width: f64,
    pub height: f64,
    pub rotation: f64,
    pub fill: ColorRgba8,
    pub fill_style: DisplayFillStyle,
    pub stroke: ColorRgba8,
    pub stroke_width: f64,
    pub corner_radii: CornerRadii,
}

#[derive(Clone, Debug, PartialEq)]
pub struct UiFocusConnectionDisplayItem {
    pub points: Vec<[f64; 2]>,
    pub path_commands: Vec<ArrowPathCommand>,
    pub arrow_type: ArrowType,
    pub start_arrowhead: Option<Arrowhead>,
    pub end_arrowhead: Option<Arrowhead>,
    pub stroke: ColorRgba8,
    pub stroke_width: f64,
    pub stroke_style: StrokeStyle,
    pub arrowhead_primitives: Vec<ArrowheadDisplayPrimitive>,
}

impl Default for UiFocusConnectionDisplayItem {
    fn default() -> Self {
        Self {
            points: Vec::new(),
            path_commands: Vec::new(),
            arrow_type: ArrowType::Straight,
            start_arrowhead: None,
            end_arrowhead: None,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: StrokeStyle::Solid,
            arrowhead_primitives: Vec::new(),
        }
    }
}

impl Default for UiRectangleDisplayItem {
    fn default() -> Self {
        Self {
            kind: UiShapeKind::SelectionCandidateFrame,
            center_x: 0.0,
            center_y: 0.0,
            width: 0.0,
            height: 0.0,
            rotation: 0.0,
            fill: ColorRgba8::default(),
            fill_style: DisplayFillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            corner_radii: CornerRadii::default(),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnapGuideDisplayItem {
    pub kind: SnapGuideKind,
    pub axis: SnapGuideAxis,
    pub start: Point<f64>,
    pub end: Point<f64>,
    pub marker_count: u8,
    pub markers: [Point<f64>; 2],
    pub label: Option<f64>,
    pub color: ColorRgba8,
    pub line_width: f64,
    pub marker_size: f64,
    pub gap_dash_length: f64,
    pub gap_dash_gap: f64,
}

impl Default for SnapGuideDisplayItem {
    fn default() -> Self {
        Self {
            kind: SnapGuideKind::Point,
            axis: SnapGuideAxis::Horizontal,
            start: Point::default(),
            end: Point::default(),
            marker_count: 0,
            markers: [Point::default(), Point::default()],
            label: None,
            color: ColorRgba8::default(),
            line_width: 0.0,
            marker_size: 0.0,
            gap_dash_length: 0.0,
            gap_dash_gap: 0.0,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum SceneDisplayItem {
    Rectangle(RectangleDisplayItem),
    Filter(FilterDisplayItem),
    Arrow(ArrowDisplayItem),
    Text(TextDisplayItem),
    SerialNumber(SerialNumberDisplayItem),
    SerialNumberConnector(SerialNumberConnectorDisplayItem),
    Stroke,
    Image,
}

impl Default for SceneDisplayItem {
    fn default() -> Self {
        Self::Rectangle(RectangleDisplayItem::default())
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum OverlayDisplayItem {
    Rectangle(UiRectangleDisplayItem),
    FocusConnection(UiFocusConnectionDisplayItem),
    PenFilterContour(UiFocusConnectionDisplayItem),
    SnapGuide(SnapGuideDisplayItem),
}

impl Default for OverlayDisplayItem {
    fn default() -> Self {
        Self::Rectangle(UiRectangleDisplayItem::default())
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct ReplaceRangeOp<T> {
    pub start: u32,
    pub delete_count: u32,
    pub insert_items: Vec<T>,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct PenFilterGeometryPatch {
    pub id: DisplayItemId,
    pub expected_geometry_revision: u64,
    pub resulting_geometry_revision: u64,
    pub retain_prefix_count: u32,
    pub appended_points: Arc<[[f64; 2]]>,
    /// Canvas-space bounds of the old and new tails, including stroke coverage.
    pub old_changed_bounds: [f64; 4],
    pub new_changed_bounds: [f64; 4],
    pub full_reset: bool,
    pub element_removed: bool,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct PathChunkReplacement {
    pub start: u32,
    pub delete_count: u32,
    pub insert_chunks: Vec<snow_draw_engine_core::PathChunk>,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct PathGeometryPatch {
    pub id: DisplayItemId,
    pub expected_geometry_revision: u64,
    pub resulting_geometry_revision: u64,
    pub ranges: Vec<PathChunkReplacement>,
    pub old_changed_bounds: [f64; 4],
    pub new_changed_bounds: [f64; 4],
    pub closed: bool,
    pub full_reset: bool,
    pub element_removed: bool,
}

#[derive(Clone, Debug, PartialEq)]
pub struct LayerPatch<T> {
    pub base_revision: u64,
    pub revision: u64,
    pub reset: bool,
    pub ops: Vec<ReplaceRangeOp<T>>,
    pub dirty_regions: Vec<DirtyRegion>,
}

impl<T> Default for LayerPatch<T> {
    fn default() -> Self {
        Self {
            base_revision: 0,
            revision: 0,
            reset: true,
            ops: Vec::new(),
            dirty_regions: Vec::new(),
        }
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct ViewportPatch {
    pub frame_view: FrameView,
    pub decoration: DecorationPatch,
    pub scene: LayerPatch<SceneDisplayItem>,
    pub pen_filter_geometry_ops: Vec<PenFilterGeometryPatch>,
    pub path_geometry_ops: Vec<PathGeometryPatch>,
    pub overlay: LayerPatch<OverlayDisplayItem>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct DecorationPatch {
    pub base_revision: u64,
    pub revision: u64,
    pub reset: bool,
    pub view: DecorationView,
    pub spotlight_ops: Vec<ReplaceRangeOp<DisplaySpotlightCutout>>,
    pub dirty_regions: Vec<DirtyRegion>,
}

impl Default for DecorationPatch {
    fn default() -> Self {
        Self {
            base_revision: 0,
            revision: 0,
            reset: true,
            view: DecorationView::default(),
            spotlight_ops: Vec::new(),
            dirty_regions: Vec::new(),
        }
    }
}

pub fn full_surface_dirty_region(surface: SurfaceSize) -> Vec<DirtyRegion> {
    if surface.width == 0 || surface.height == 0 {
        Vec::new()
    } else {
        vec![DirtyRegion::new(
            0.0,
            0.0,
            surface.width as f64,
            surface.height as f64,
        )]
    }
}
