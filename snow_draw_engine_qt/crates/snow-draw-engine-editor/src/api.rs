use serde::{Deserialize, Serialize};
use snow_draw_engine_core::{
    Camera, ColorRgba8, CornerRadii, ErrorCode, PathGeometry, Point, SnapGuide, SurfaceSize,
    arrow::{ArrowStrokeStyle, ArrowType, Arrowhead},
    validate_camera,
};
use snow_draw_engine_document::{
    ArrowData, CanvasFilterType, ElementId, FillStyle, FilterData, FreeDrawData, HighlightShape,
    PenFilterData, RectangleData, SerialNumberData, StrokeStyle, TextData,
};
use std::sync::Arc;

use crate::text::{SerialNumberStyle, TextPreviewFontSize, TextStyle};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum ActiveTool {
    Select,
    #[default]
    Shape,
    Arrow,
    Line,
    FreeDraw,
    RectangleHighlight,
    PenHighlight,
    RectangleFilter,
    PenFilter,
    Watermark,
    Eraser,
    Text,
    SerialNumber,
    Spotlight,
}

impl ActiveTool {
    #[allow(non_upper_case_globals)]
    pub const Filter: Self = Self::RectangleFilter;

    pub const fn policy_bit(self) -> u64 {
        1_u64 << (self as u32)
    }

    pub(crate) const fn uses_stroke_cursor(self) -> bool {
        matches!(self, Self::FreeDraw | Self::PenHighlight | Self::PenFilter)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
pub struct RectangleShapeStyle {
    pub fill: ColorRgba8,
    pub fill_style: FillStyle,
    pub stroke: ColorRgba8,
    pub stroke_width: f64,
    pub stroke_style: StrokeStyle,
    pub corner_radii: CornerRadii,
    pub shape: HighlightShape,
}

#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
pub struct ArrowStyle {
    pub stroke: ColorRgba8,
    pub stroke_width: f64,
    pub start_arrowhead: Option<Arrowhead>,
    pub end_arrowhead: Option<Arrowhead>,
    pub stroke_style: ArrowStrokeStyle,
    pub arrow_type: ArrowType,
}

#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
pub struct ShapeStyle {
    pub fill: ColorRgba8,
    pub fill_style: FillStyle,
    pub stroke: ColorRgba8,
    pub stroke_width: f64,
    pub corner_radii: CornerRadii,
    pub start_arrowhead: Option<Arrowhead>,
    pub end_arrowhead: Option<Arrowhead>,
    pub stroke_style: ArrowStrokeStyle,
    pub arrow_type: ArrowType,
    pub opacity: f64,
    pub highlight_shape: HighlightShape,
    pub shape: HighlightShape,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StyleToolbarSource {
    DefaultRectangle,
    SelectedRectangle,
    DefaultArrow,
    SelectedArrow,
    DefaultLine,
    SelectedLine,
    DefaultFreeDraw,
    SelectedFreeDraw,
    DefaultRectangleHighlight,
    SelectedRectangleHighlight,
    DefaultPenHighlight,
    SelectedPenHighlight,
    DefaultText,
    SelectedText,
    DefaultSerialNumber,
    SelectedSerialNumber,
    Eraser,
    DefaultRectangleFilter,
    SelectedRectangleFilter,
    DefaultPenFilter,
    SelectedPenFilter,
    Watermark,
    DefaultSpotlight,
    SelectedSpotlight,
}

impl StyleToolbarSource {
    #[allow(non_upper_case_globals)]
    pub const DefaultFilter: Self = Self::DefaultRectangleFilter;
    #[allow(non_upper_case_globals)]
    pub const SelectedFilter: Self = Self::SelectedRectangleFilter;
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FilterStyle {
    pub filter_type: snow_draw_engine_document::CanvasFilterType,
    pub strength: f64,
    pub opacity: f64,
    pub stroke_width: f64,
}

impl Default for FilterStyle {
    fn default() -> Self {
        Self {
            filter_type: Default::default(),
            strength: 0.5,
            opacity: 1.0,
            stroke_width: 2.0,
        }
    }
}

pub const FILTER_STYLE_PROPERTY_TYPE: u32 = 1 << 0;
pub const FILTER_STYLE_PROPERTY_STRENGTH: u32 = 1 << 1;
pub const FILTER_STYLE_PROPERTY_OPACITY: u32 = 1 << 2;
pub const FILTER_STYLE_PROPERTY_STROKE_WIDTH: u32 = 1 << 3;
pub const FILTER_STYLE_PROPERTY_ALL: u32 = FILTER_STYLE_PROPERTY_TYPE
    | FILTER_STYLE_PROPERTY_STRENGTH
    | FILTER_STYLE_PROPERTY_OPACITY
    | FILTER_STYLE_PROPERTY_STROKE_WIDTH;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ShapeKind {
    Rectangle,
    Arrow,
    Line,
    FreeDraw,
    RectangleHighlight,
    PenHighlight,
    Spotlight,
}

pub const SHAPE_STYLE_PROPERTY_FILL: u32 = 1 << 0;
pub const SHAPE_STYLE_PROPERTY_FILL_STYLE: u32 = 1 << 1;
pub const SHAPE_STYLE_PROPERTY_STROKE: u32 = 1 << 2;
pub const SHAPE_STYLE_PROPERTY_STROKE_WIDTH: u32 = 1 << 3;
pub const SHAPE_STYLE_PROPERTY_CORNER_RADII: u32 = 1 << 4;
pub const SHAPE_STYLE_PROPERTY_START_ARROWHEAD: u32 = 1 << 5;
pub const SHAPE_STYLE_PROPERTY_END_ARROWHEAD: u32 = 1 << 6;
pub const SHAPE_STYLE_PROPERTY_STROKE_STYLE: u32 = 1 << 7;
pub const SHAPE_STYLE_PROPERTY_ARROW_TYPE: u32 = 1 << 8;
pub const SHAPE_STYLE_PROPERTY_OPACITY: u32 = 1 << 9;
pub const SHAPE_STYLE_PROPERTY_HIGHLIGHT_SHAPE: u32 = 1 << 10;
pub const SHAPE_STYLE_PROPERTY_SHAPE: u32 = 1 << 11;

pub const SHAPE_STYLE_PROPERTY_RECTANGLE: u32 = SHAPE_STYLE_PROPERTY_FILL
    | SHAPE_STYLE_PROPERTY_FILL_STYLE
    | SHAPE_STYLE_PROPERTY_STROKE
    | SHAPE_STYLE_PROPERTY_STROKE_WIDTH
    | SHAPE_STYLE_PROPERTY_CORNER_RADII
    | SHAPE_STYLE_PROPERTY_STROKE_STYLE
    | SHAPE_STYLE_PROPERTY_SHAPE;
pub const SHAPE_STYLE_PROPERTY_ARROW: u32 = SHAPE_STYLE_PROPERTY_STROKE
    | SHAPE_STYLE_PROPERTY_STROKE_WIDTH
    | SHAPE_STYLE_PROPERTY_START_ARROWHEAD
    | SHAPE_STYLE_PROPERTY_END_ARROWHEAD
    | SHAPE_STYLE_PROPERTY_STROKE_STYLE
    | SHAPE_STYLE_PROPERTY_ARROW_TYPE;
pub const SHAPE_STYLE_PROPERTY_LINE: u32 = SHAPE_STYLE_PROPERTY_FILL
    | SHAPE_STYLE_PROPERTY_FILL_STYLE
    | SHAPE_STYLE_PROPERTY_STROKE
    | SHAPE_STYLE_PROPERTY_STROKE_WIDTH
    | SHAPE_STYLE_PROPERTY_STROKE_STYLE
    | SHAPE_STYLE_PROPERTY_OPACITY;
pub const SHAPE_STYLE_PROPERTY_ALL: u32 = SHAPE_STYLE_PROPERTY_RECTANGLE
    | SHAPE_STYLE_PROPERTY_ARROW
    | SHAPE_STYLE_PROPERTY_LINE
    | SHAPE_STYLE_PROPERTY_HIGHLIGHT_SHAPE
    | SHAPE_STYLE_PROPERTY_SHAPE;

impl ShapeKind {
    pub const fn supported_properties(self) -> u32 {
        match self {
            Self::Rectangle => SHAPE_STYLE_PROPERTY_RECTANGLE,
            Self::Arrow => SHAPE_STYLE_PROPERTY_ARROW,
            Self::Line => SHAPE_STYLE_PROPERTY_LINE,
            Self::FreeDraw => SHAPE_STYLE_PROPERTY_LINE,
            Self::RectangleHighlight => {
                SHAPE_STYLE_PROPERTY_FILL
                    | SHAPE_STYLE_PROPERTY_STROKE
                    | SHAPE_STYLE_PROPERTY_STROKE_WIDTH
            }
            Self::PenHighlight => SHAPE_STYLE_PROPERTY_STROKE | SHAPE_STYLE_PROPERTY_STROKE_WIDTH,
            Self::Spotlight => 0,
        }
    }
}

pub const SHAPE_STYLE_MIXED_FILL: u32 = SHAPE_STYLE_PROPERTY_FILL;
pub const SHAPE_STYLE_MIXED_FILL_STYLE: u32 = SHAPE_STYLE_PROPERTY_FILL_STYLE;
pub const SHAPE_STYLE_MIXED_STROKE: u32 = SHAPE_STYLE_PROPERTY_STROKE;
pub const SHAPE_STYLE_MIXED_STROKE_WIDTH: u32 = SHAPE_STYLE_PROPERTY_STROKE_WIDTH;
pub const SHAPE_STYLE_MIXED_CORNER_RADII: u32 = SHAPE_STYLE_PROPERTY_CORNER_RADII;
pub const SHAPE_STYLE_MIXED_START_ARROWHEAD: u32 = SHAPE_STYLE_PROPERTY_START_ARROWHEAD;
pub const SHAPE_STYLE_MIXED_END_ARROWHEAD: u32 = SHAPE_STYLE_PROPERTY_END_ARROWHEAD;
pub const SHAPE_STYLE_MIXED_STROKE_STYLE: u32 = SHAPE_STYLE_PROPERTY_STROKE_STYLE;
pub const SHAPE_STYLE_MIXED_ARROW_TYPE: u32 = SHAPE_STYLE_PROPERTY_ARROW_TYPE;
pub const SHAPE_STYLE_MIXED_OPACITY: u32 = 1 << 9;
pub const SHAPE_STYLE_MIXED_SHAPE: u32 = SHAPE_STYLE_PROPERTY_SHAPE;
pub const SHAPE_STYLE_MIXED_HIGHLIGHT_SHAPE: u32 = 1 << 10;

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ShapeStylePatch {
    pub kind: ShapeKind,
    pub style: ShapeStyle,
    pub properties: u32,
}

pub const TEXT_STYLE_MIXED_COLOR: u32 = 1 << 0;
pub const TEXT_STYLE_MIXED_FONT_SIZE: u32 = 1 << 1;
pub const TEXT_STYLE_MIXED_FONT_FAMILY: u32 = 1 << 2;
pub const TEXT_STYLE_MIXED_FILL: u32 = 1 << 3;
pub const TEXT_STYLE_MIXED_FILL_STYLE: u32 = 1 << 4;
pub const TEXT_STYLE_MIXED_STROKE: u32 = 1 << 5;
pub const TEXT_STYLE_MIXED_STROKE_WIDTH: u32 = 1 << 6;
pub const TEXT_STYLE_MIXED_CORNER_RADII: u32 = 1 << 7;
pub const TEXT_STYLE_MIXED_HORIZONTAL_ALIGN: u32 = 1 << 8;
pub const TEXT_STYLE_MIXED_VERTICAL_ALIGN: u32 = 1 << 9;
pub const TEXT_STYLE_MIXED_OPACITY: u32 = 1 << 10;

pub const SERIAL_NUMBER_STYLE_MIXED_NUMBER: u32 = 1 << 0;
pub const SERIAL_NUMBER_STYLE_MIXED_COLOR: u32 = 1 << 1;
pub const SERIAL_NUMBER_STYLE_MIXED_FILL: u32 = 1 << 2;
pub const SERIAL_NUMBER_STYLE_MIXED_FILL_STYLE: u32 = 1 << 3;
pub const SERIAL_NUMBER_STYLE_MIXED_FONT_SIZE: u32 = 1 << 4;
pub const SERIAL_NUMBER_STYLE_MIXED_FONT_FAMILY: u32 = 1 << 5;
pub const SERIAL_NUMBER_STYLE_MIXED_STROKE_WIDTH: u32 = 1 << 6;
pub const SERIAL_NUMBER_STYLE_MIXED_STROKE_STYLE: u32 = 1 << 7;
pub const SERIAL_NUMBER_STYLE_MIXED_OPACITY: u32 = 1 << 8;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct HistoryState {
    pub can_undo: bool,
    pub can_redo: bool,
}

#[derive(Clone, Debug, PartialEq)]
pub struct StyleToolbarState {
    pub source: StyleToolbarSource,
    pub shape_style: ShapeStyle,
    pub text_style: TextStyle,
    pub serial_number_style: SerialNumberStyle,
    pub text_style_mixed: u32,
    pub serial_number_style_mixed: u32,
    pub shape_style_mixed: u32,
    pub filter_style: FilterStyle,
    pub filter_style_mixed: u32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SerialNumberToolbarState {
    pub visible: bool,
    pub left: f64,
    pub top: f64,
    pub width: f64,
    pub height: f64,
    pub can_decrease: bool,
    pub can_increase: bool,
    pub can_create_text: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct EditorViewState {
    pub surface: SurfaceSize,
    pub camera: Camera,
    pub clear_color: ColorRgba8,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct EditorStrokeCursor {
    pub position: Point<f64>,
    pub stroke_width: f64,
}

#[derive(Clone, Debug, PartialEq)]
pub struct PenFilterPreview {
    pub global_points: Vec<Point<f64>>,
    pub filter_type: CanvasFilterType,
    pub strength: f64,
    pub stroke_width: f64,
    pub opacity: f64,
}

#[derive(Clone, Debug, PartialEq)]
pub struct FreeDrawPreview {
    pub geometry: Arc<PathGeometry>,
    pub stroke: ColorRgba8,
    pub stroke_width: f64,
    pub stroke_style: StrokeStyle,
    pub fill: ColorRgba8,
    pub fill_style: FillStyle,
    pub opacity: f64,
}

#[derive(Clone, Debug, PartialEq)]
pub enum ElementCreationPreview {
    Rectangle(RectangleData),
    Filter(FilterData),
    PenFilter(PenFilterPreview),
    FreeDraw(Arc<FreeDrawPreview>),
    Arrow(ArrowData),
    SerialNumber(SerialNumberData),
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct EditorPresentationState {
    pub creation_preview: Option<ElementCreationPreview>,
    pub active_text_draft: Option<ActiveTextDraftPresentation>,
    pub preview_arrows: Vec<SelectionArrowState>,
    pub preview_elements: Vec<SelectionRectState>,
    pub preview_text_font_sizes: Vec<TextPreviewFontSize>,
    pub marquee: Option<RectangleData>,
    pub marquee_candidate_elements: Vec<SelectionRectState>,
    pub marquee_candidate_arrows: Vec<SelectionArrowState>,
    pub text_rect_ids: Vec<ElementId>,
    pub hovered_rect: Option<RectangleData>,
    pub hovered_free_draw: Option<FreeDrawData>,
    pub hovered_pen_filter: Option<PenFilterData>,
    pub hovered_text_rect: Option<RectangleData>,
    pub hovered_arrow: Option<ArrowData>,
    pub selection_bounds: Option<SelectionBounds>,
    pub selection_elements: Vec<SelectionRectState>,
    pub selection_pen_filters: Vec<PenFilterData>,
    pub selection_arrows: Vec<SelectionArrowState>,
    pub selected_single_rect: Option<RectangleData>,
    pub selected_single_text_rect: Option<RectangleData>,
    pub selected_single_arrow: Option<ArrowData>,
    pub arrow_handles: Vec<ArrowHandleState>,
    pub snap_guides: Vec<SnapGuide>,
    pub eraser_cursor: Option<Point<f64>>,
    pub stroke_cursor: Option<EditorStrokeCursor>,
}

/// Whether the generic selection frame and its controls apply to these members.
pub fn selection_box_visible_for_members(
    selection_elements: &[SelectionRectState],
    selection_arrows: &[SelectionArrowState],
) -> bool {
    let selected_item_count = selection_elements.len() + selection_arrows.len();
    selected_item_count != 1
        || !matches!(
            selection_arrows,
            [arrow] if arrow.arrow.points.len() == 2
        )
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct EditorViewportState {
    pub surface: SurfaceSize,
    pub camera: Camera,
}

impl Default for EditorViewportState {
    fn default() -> Self {
        Self {
            surface: SurfaceSize::default(),
            camera: Camera {
                center: Point::default(),
                zoom: 1.0,
            },
        }
    }
}

impl EditorViewportState {
    pub fn set_surface_size(&mut self, width: u32, height: u32) {
        self.surface = SurfaceSize { width, height };
    }

    pub fn set_camera(&mut self, camera: Camera) -> Result<(), ErrorCode> {
        validate_camera(&camera)?;
        self.camera = camera;
        Ok(())
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SelectionRectState {
    pub id: snow_draw_engine_document::ElementId,
    pub rect: RectangleData,
}

#[derive(Clone, Debug, PartialEq)]
pub struct SelectionArrowState {
    pub id: snow_draw_engine_document::ElementId,
    pub arrow: ArrowData,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ArrowHandleKind {
    Endpoint,
    LoopStart,
    LoopEnd,
    FocusPoint,
    Segment,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ArrowHandleState {
    pub kind: ArrowHandleKind,
    pub center: Point<f64>,
    pub anchor: Option<Point<f64>>,
    pub fixed_segment: bool,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SelectionBounds {
    pub center: Point<f64>,
    pub width: f64,
    pub height: f64,
    pub rotation: f64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ActiveTextDraftTarget {
    New,
    Existing(ElementId),
}

#[derive(Clone, Debug, PartialEq)]
pub struct ActiveTextDraftPresentation {
    pub target: ActiveTextDraftTarget,
    pub revision: u64,
    pub text: TextData,
}

impl ActiveTextDraftPresentation {
    pub fn existing_id(&self) -> Option<ElementId> {
        match self.target {
            ActiveTextDraftTarget::New => None,
            ActiveTextDraftTarget::Existing(id) => Some(id),
        }
    }

    pub fn display_id(&self) -> ElementId {
        self.existing_id().unwrap_or(ElementId {
            index: u32::MAX,
            generation: self.revision as u32,
        })
    }

    pub fn rect(&self) -> RectangleData {
        RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: self.text.center,
            width: self.text.width,
            height: self.text.height,
            rotation: self.text.rotation,
            fill: self.text.fill,
            fill_style: self.text.fill_style,
            stroke: self.text.stroke,
            stroke_width: self.text.stroke_width,
            stroke_style: StrokeStyle::Solid,
            corner_radii: self.text.corner_radii,
            opacity: self.text.opacity,
        }
    }

    pub fn with_rect(&self, rect: RectangleData) -> Self {
        let mut next = self.clone();
        next.text.center = rect.center;
        next.text.width = rect.width;
        next.text.height = rect.height;
        next.text.rotation = rect.rotation;
        next.text.corner_radii = rect.corner_radii;
        next
    }
}
