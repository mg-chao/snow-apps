use serde::{Deserialize, Deserializer, Serialize, Serializer};
use snow_draw_engine_core::{ErrorCode, Point, SnapGuide, arrow::ArrowEndpointEdge};
use snow_draw_engine_document::{
    ArrowData, ElementId, ElementKind, FilterData, PenFilterData, RectangleData, SerialNumberData,
    TextData,
};
use snow_draw_engine_interaction::CursorStyle;
use snow_draw_engine_model::DocumentModel;
use std::collections::{HashMap, HashSet};

use super::{
    ActiveTextDraftPresentation, ActiveTool, ArrowStyle, ElementCreationPreview,
    RectangleShapeStyle, SelectionArrowState, SelectionBounds, SelectionRectState, ShapeStyle,
};
use crate::defaults::EditorStyleDefaults;
use crate::text::TextResizeLayoutOverride;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ToolSelectionScope {
    None,
    All,
    RectangleOnly,
    ArrowOnly,
    LineOnly,
    FreeDrawOnly,
    RectangleHighlightOnly,
    SpotlightOnly,
    PenHighlightOnly,
    FilterOnly,
    PenFilterOnly,
    TextOnly,
    SerialNumberOnly,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ToolEmptyCanvasAction {
    Configure,
    MarqueeSelect,
    CreateRectangle,
    CreateArrow,
    CreateFreeDraw,
    CreateHighlight,
    CreatePenHighlight,
    CreatePenFilter,
    CreateText,
    CreateSerialNumber,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ToolPolicy {
    pub(crate) selection_scope: ToolSelectionScope,
    pub(crate) quick_selection_enabled: bool,
    pub(crate) clear_selection_on_activate: bool,
    pub(crate) empty_canvas_action: ToolEmptyCanvasAction,
    pub(crate) allow_shift_toggle: bool,
    pub(crate) default_cursor: CursorStyle,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct SelectionState {
    pub(crate) ids: Vec<ElementId>,
    pub(crate) primary: Option<ElementId>,
    pub(crate) bounds: Option<SelectionBounds>,
    pub(crate) elements: Vec<SelectionRectState>,
    pub(crate) arrows: Vec<SelectionArrowState>,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct UiState {
    pub(crate) marquee: Option<RectangleData>,
    pub(crate) snap_guides: Vec<SnapGuide>,
    pub(crate) hovered_element: Option<ElementId>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) struct CreateRectangleState {
    pub(crate) pointer_id: u32,
    pub(crate) start_canvas_position: Point<f64>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) enum ArrowCreationPhase {
    #[default]
    InitialPress,
    AwaitingEndpoint,
    EndpointPress,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct CreateArrowState {
    pub(crate) pointer_id: u32,
    pub(crate) committed_points: Vec<Point<f64>>,
    pub(crate) press_view_position: Point<f64>,
    pub(crate) phase: ArrowCreationPhase,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct EraserState {
    pub(crate) active_pointers: HashMap<u32, Point<f64>>,
    pub(crate) pending_ids: Vec<ElementId>,
    pub(crate) cursor_canvas_position: Option<Point<f64>>,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct CreateFreeDrawState {
    pub(crate) pointer_id: u32,
    pub(crate) builder: crate::free_draw_workflow::StreamingFreeDrawBuilder,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct CreatePenFilterState {
    pub(crate) pointer_id: u32,
    pub(crate) committed_points: Vec<Point<f64>>,
    pub(crate) pending_simplified_points: Vec<Point<f64>>,
    pub(crate) pending_raw_points: Vec<Point<f64>>,
    pub(crate) preview_committed_points: Vec<Point<f64>>,
    pub(crate) epsilon: f64,
    pub(crate) straight_anchor: Option<Point<f64>>,
    pub(crate) straight_endpoint: Option<Point<f64>>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct CreateSerialNumberState {
    pub(crate) pointer_id: u32,
    pub(crate) preview: SerialNumberData,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct MarqueeSelectionState {
    pub(crate) pointer_id: u32,
    pub(crate) start_canvas_position: Point<f64>,
    pub(crate) additive: bool,
    pub(crate) base_selection: SelectionState,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum RectCorner {
    TopLeft,
    TopRight,
    BottomRight,
    BottomLeft,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ResizeHandle {
    TopLeft,
    Top,
    TopRight,
    Right,
    BottomRight,
    Bottom,
    BottomLeft,
    Left,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SelectionHitTarget {
    Move,
    Resize(ResizeHandle),
    Rotate,
    CornerRadius(RectCorner),
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct AxisAlignedBounds {
    pub(crate) left: f64,
    pub(crate) top: f64,
    pub(crate) right: f64,
    pub(crate) bottom: f64,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) enum SelectionEditMode {
    Move {
        start_canvas_position: Point<f64>,
    },
    Resize {
        handle: ResizeHandle,
        handle_offset_canvas: Point<f64>,
        frame_padding: f64,
        corner_handle_outset: f64,
        scale_from_center: bool,
        text_layout_override: Option<TextResizeLayoutOverride>,
    },
    Rotate {
        start_pointer_angle: f64,
    },
    CornerRadius {
        corner: RectCorner,
    },
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct EditSelectionState {
    pub(crate) pointer_id: u32,
    pub(crate) original_elements: Vec<SelectionRectState>,
    pub(crate) preview_elements: Vec<SelectionRectState>,
    pub(crate) original_arrows: Vec<SelectionArrowState>,
    pub(crate) preview_arrows: Vec<SelectionArrowState>,
    pub(crate) original_bounds: SelectionBounds,
    pub(crate) preview_bounds: SelectionBounds,
    pub(crate) mode: SelectionEditMode,
}

pub(crate) struct BeginSelectionEditRequest {
    pub(crate) pointer_id: u32,
    pub(crate) original_elements: Vec<SelectionRectState>,
    pub(crate) original_arrows: Vec<SelectionArrowState>,
    pub(crate) original_bounds: SelectionBounds,
    pub(crate) target: SelectionHitTarget,
    pub(crate) canvas_point: Point<f64>,
    pub(crate) frame_padding_override: Option<f64>,
}

pub(crate) struct BeginSelectionInteractionRequest {
    pub(crate) pointer_id: u32,
    pub(crate) start_view_position: Point<f64>,
    pub(crate) original_elements: Vec<SelectionRectState>,
    pub(crate) original_arrows: Vec<SelectionArrowState>,
    pub(crate) original_bounds: SelectionBounds,
    pub(crate) target: SelectionHitTarget,
    pub(crate) canvas_point: Point<f64>,
    pub(crate) frame_padding: f64,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct PendingSelectionMoveState {
    pub(crate) pointer_id: u32,
    pub(crate) original_elements: Vec<SelectionRectState>,
    pub(crate) original_arrows: Vec<SelectionArrowState>,
    pub(crate) original_bounds: SelectionBounds,
    pub(crate) start_canvas_position: Point<f64>,
    pub(crate) start_view_position: Point<f64>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct PendingArrowMoveState {
    pub(crate) pointer_id: u32,
    pub(crate) arrow_id: ElementId,
    pub(crate) original_arrow: ArrowData,
    pub(crate) start_canvas_position: Point<f64>,
    pub(crate) start_view_position: Point<f64>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ArrowHitTarget {
    Move,
    Endpoint(ArrowEndpointEdge),
    Point(usize),
    FocusPoint(ArrowEndpointEdge),
    Segment(usize),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ArrowEditMode {
    Move,
    Endpoint(ArrowEndpointEdge),
    Point(usize),
    FocusPoint(ArrowEndpointEdge),
    Segment(usize),
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct EditArrowState {
    pub(crate) pointer_id: u32,
    pub(crate) arrow_id: ElementId,
    pub(crate) original_arrow: ArrowData,
    pub(crate) preview_arrow: ArrowData,
    pub(crate) mode: ArrowEditMode,
    pub(crate) start_canvas_position: Point<f64>,
    pub(crate) drag_offset: Point<f64>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum CanvasHit {
    SelectionHandle(SelectionHitTarget),
    ArrowHandle(ArrowHitTarget),
    EligibleElement(ElementId, ElementKind),
    Empty,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum PrimaryPointerIntent {
    ToggleSelection { id: ElementId },
    BeginSelectionInteraction { target: SelectionHitTarget },
    BeginSelectedArrowInteraction { target: ArrowHitTarget },
    BeginArrowElementInteraction { id: ElementId },
    BeginElementSelectionMove { id: ElementId },
    TextEditCandidate { id: ElementId },
    EmptyCanvas,
}

impl RectCorner {
    pub(crate) fn x_sign(self) -> f64 {
        match self {
            Self::TopLeft | Self::BottomLeft => -1.0,
            Self::TopRight | Self::BottomRight => 1.0,
        }
    }

    pub(crate) fn y_sign(self) -> f64 {
        match self {
            Self::TopLeft | Self::TopRight => -1.0,
            Self::BottomRight | Self::BottomLeft => 1.0,
        }
    }
}

impl ResizeHandle {
    pub(crate) fn from_corner(corner: RectCorner) -> Self {
        match corner {
            RectCorner::TopLeft => Self::TopLeft,
            RectCorner::TopRight => Self::TopRight,
            RectCorner::BottomRight => Self::BottomRight,
            RectCorner::BottomLeft => Self::BottomLeft,
        }
    }

    pub(crate) fn x_sign(self) -> f64 {
        match self {
            Self::TopLeft | Self::BottomLeft | Self::Left => -1.0,
            Self::TopRight | Self::BottomRight | Self::Right => 1.0,
            Self::Top | Self::Bottom => 0.0,
        }
    }

    pub(crate) fn y_sign(self) -> f64 {
        match self {
            Self::TopLeft | Self::TopRight | Self::Top => -1.0,
            Self::BottomRight | Self::BottomLeft | Self::Bottom => 1.0,
            Self::Left | Self::Right => 0.0,
        }
    }

    pub(crate) fn is_corner(self) -> bool {
        matches!(
            self,
            Self::TopLeft | Self::TopRight | Self::BottomRight | Self::BottomLeft
        )
    }

    pub(crate) fn local_point(self, width: f64, height: f64) -> Point<f64> {
        Point {
            x: self.x_sign() * width / 2.0,
            y: self.y_sign() * height / 2.0,
        }
    }

    pub(crate) fn anchor_local_point(
        self,
        width: f64,
        height: f64,
        scale_from_center: bool,
    ) -> Point<f64> {
        Point {
            x: if scale_from_center || self.x_sign().abs() <= f64::EPSILON {
                0.0
            } else {
                -self.x_sign() * width / 2.0
            },
            y: if scale_from_center || self.y_sign().abs() <= f64::EPSILON {
                0.0
            } else {
                -self.y_sign() * height / 2.0
            },
        }
    }
}

impl SelectionState {
    pub(crate) fn is_empty(&self) -> bool {
        self.ids.is_empty()
    }

    pub(crate) fn contains(&self, id: ElementId) -> bool {
        self.ids.contains(&id)
    }

    pub(crate) fn has_cached_members(&self) -> bool {
        !self.elements.is_empty() || !self.arrows.is_empty()
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) enum InteractionState {
    #[default]
    Idle,
    CreatingRectangle(CreateRectangleState),
    CreatingPenHighlight(CreateRectangleState),
    CreatingArrow(CreateArrowState),
    CreatingFreeDraw(CreateFreeDrawState),
    CreatingPenFilter(CreatePenFilterState),
    CreatingSerialNumber(CreateSerialNumberState),
    MarqueeSelection(MarqueeSelectionState),
    PendingSelectionMove(PendingSelectionMoveState),
    PendingArrowMove(PendingArrowMoveState),
    EditingSelection(EditSelectionState),
    EditingArrow(EditArrowState),
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct EditorState {
    pub(crate) active_tool: ActiveTool,
    pub(crate) selection: SelectionState,
    pub(crate) creation_preview: Option<ElementCreationPreview>,
    pub(crate) ui: UiState,
    pub(crate) interaction: InteractionState,
    pub(crate) active_text_draft: Option<ActiveTextDraftPresentation>,
    pub(crate) default_rectangle_shape_style: RectangleShapeStyle,
    pub(crate) default_arrow_style: ArrowStyle,
    pub(crate) default_line_style: ShapeStyle,
    pub(crate) default_free_draw_style: ShapeStyle,
    pub(crate) default_rectangle_highlight_style: ShapeStyle,
    pub(crate) default_pen_highlight_style: ShapeStyle,
    pub(crate) default_filter: FilterData,
    pub(crate) default_filter_stroke_width: f64,
    pub(crate) default_pen_filter: PenFilterData,
    pub(crate) default_text: TextData,
    pub(crate) default_serial_number: SerialNumberData,
    pub(crate) eraser: EraserState,
    pub(crate) stroke_cursor_canvas_position: Option<Point<f64>>,
}

impl Default for EditorState {
    fn default() -> Self {
        Self::with_style_defaults(&EditorStyleDefaults::default())
    }
}

impl EditorState {
    pub(crate) fn with_style_defaults(default_styles: &EditorStyleDefaults) -> Self {
        let default_filter = FilterData {
            filter_type: default_styles.rectangle_filter.filter_type,
            strength: default_styles.rectangle_filter.strength,
            opacity: default_styles.rectangle_filter.opacity,
            ..FilterData::default()
        };
        let default_pen_filter = PenFilterData {
            filter_type: default_styles.pen_filter.filter_type,
            strength: default_styles.pen_filter.strength,
            opacity: default_styles.pen_filter.opacity,
            stroke_width: default_styles.pen_filter.stroke_width,
            ..PenFilterData::default()
        };
        let default_text = TextData {
            color: default_styles.text.color,
            font_size: default_styles.text.font_size,
            font_family: default_styles.text.font_family.clone(),
            fill: default_styles.text.fill,
            fill_style: default_styles.text.fill_style,
            stroke: default_styles.text.stroke,
            stroke_width: default_styles.text.stroke_width,
            corner_radii: default_styles.text.corner_radii,
            horizontal_align: default_styles.text.horizontal_align,
            vertical_align: default_styles.text.vertical_align,
            opacity: default_styles.text.opacity,
            ..TextData::default()
        };
        let default_serial_number = SerialNumberData {
            number: default_styles.serial_number.number,
            color: default_styles.serial_number.color,
            fill: default_styles.serial_number.fill,
            fill_style: default_styles.serial_number.fill_style,
            font_size: default_styles.serial_number.font_size,
            font_family: default_styles.serial_number.font_family.clone(),
            stroke_width: default_styles.serial_number.stroke_width,
            stroke_style: default_styles.serial_number.stroke_style,
            opacity: default_styles.serial_number.opacity,
            ..SerialNumberData::default()
        };

        Self {
            active_tool: ActiveTool::default(),
            selection: SelectionState::default(),
            creation_preview: None,
            ui: UiState::default(),
            interaction: InteractionState::default(),
            active_text_draft: None,
            default_rectangle_shape_style: default_styles.rectangle,
            default_arrow_style: default_styles.arrow,
            default_line_style: default_styles.line,
            default_free_draw_style: default_styles.free_draw,
            default_rectangle_highlight_style: default_styles.rectangle_highlight,
            default_pen_highlight_style: default_styles.pen_highlight,
            default_filter,
            default_filter_stroke_width: default_styles.rectangle_filter.stroke_width,
            default_pen_filter,
            default_text,
            default_serial_number,
            eraser: EraserState::default(),
            stroke_cursor_canvas_position: None,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct DocumentSyncSnapshot {
    pub(crate) selection: SelectionState,
}

impl DocumentSyncSnapshot {
    pub fn validate_session(&self, document: &DocumentModel) -> Result<(), ErrorCode> {
        if self
            .selection
            .ids
            .iter()
            .any(|id| document.document().element(*id).is_err())
        {
            return Err(ErrorCode::InvalidArgument);
        }
        Ok(())
    }
}

impl Serialize for DocumentSyncSnapshot {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        #[derive(Serialize)]
        #[serde(rename_all = "camelCase")]
        struct Snapshot<'a> {
            selected_ids: &'a [ElementId],
            primary_id: Option<ElementId>,
        }
        Snapshot {
            selected_ids: &self.selection.ids,
            primary_id: self.selection.primary,
        }
        .serialize(serializer)
    }
}

impl<'de> Deserialize<'de> for DocumentSyncSnapshot {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        #[derive(Deserialize)]
        #[serde(rename_all = "camelCase", deny_unknown_fields)]
        struct Snapshot {
            selected_ids: Vec<ElementId>,
            primary_id: Option<ElementId>,
        }
        let snapshot = Snapshot::deserialize(deserializer)?;
        let unique_ids: HashSet<_> = snapshot.selected_ids.iter().copied().collect();
        if snapshot.selected_ids.len() > 1_000_000
            || unique_ids.len() != snapshot.selected_ids.len()
            || snapshot
                .primary_id
                .is_some_and(|id| !snapshot.selected_ids.contains(&id))
        {
            return Err(serde::de::Error::custom("invalid selection snapshot"));
        }
        Ok(Self {
            selection: SelectionState {
                ids: snapshot.selected_ids,
                primary: snapshot.primary_id,
                ..SelectionState::default()
            },
        })
    }
}
