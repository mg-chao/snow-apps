use snow_draw_engine_core::{
    ColorRgba8, CornerRadii, ErrorCode, Point,
    arrow::{StrokeStyle, ArrowType, Arrowhead},
};
use snow_draw_engine_document::{
    ArrowData, FillStyle, FilterData, MIN_SERIAL_NUMBER_FONT_SIZE, MIN_TEXT_FONT_SIZE,
    RectangleData, SerialNumberData, SpotlightConfig, TextData, Transaction,
    WatermarkConfig, normalize_corner_radii, normalize_font_family, serial_number_rect_proxy,
    serial_number_with_label_style, text_with_auto_resize_layout, validate_serial_number,
    validate_text,
};
use snow_draw_engine_interaction::Modifiers;
use snow_draw_engine_model::DocumentModel;

use crate::{
    ActiveTool, ApplyTransactionCommand, ArrowStyle, Editor, EditorCommand, ElementCreationPreview,
    FILTER_STYLE_PROPERTY_OPACITY, FILTER_STYLE_PROPERTY_STRENGTH,
    FILTER_STYLE_PROPERTY_STROKE_WIDTH, FILTER_STYLE_PROPERTY_TYPE, FilterStyle,
    RectangleShapeStyle, SERIAL_NUMBER_STYLE_MIXED_COLOR, SERIAL_NUMBER_STYLE_MIXED_FILL,
    SERIAL_NUMBER_STYLE_MIXED_FILL_STYLE, SERIAL_NUMBER_STYLE_MIXED_FONT_FAMILY,
    SERIAL_NUMBER_STYLE_MIXED_FONT_SIZE, SERIAL_NUMBER_STYLE_MIXED_NUMBER,
    SERIAL_NUMBER_STYLE_MIXED_OPACITY, SERIAL_NUMBER_STYLE_MIXED_STROKE_STYLE,
    SERIAL_NUMBER_STYLE_MIXED_STROKE_WIDTH, SHAPE_STYLE_MIXED_ARROW_TYPE,
    SHAPE_STYLE_MIXED_CORNER_RADII, SHAPE_STYLE_MIXED_END_ARROWHEAD, SHAPE_STYLE_MIXED_FILL,
    SHAPE_STYLE_MIXED_FILL_STYLE, SHAPE_STYLE_MIXED_HIGHLIGHT_SHAPE, SHAPE_STYLE_MIXED_OPACITY,
    SHAPE_STYLE_MIXED_SHAPE, SHAPE_STYLE_MIXED_START_ARROWHEAD, SHAPE_STYLE_MIXED_STROKE,
    SHAPE_STYLE_MIXED_STROKE_STYLE, SHAPE_STYLE_MIXED_STROKE_WIDTH,
    SHAPE_STYLE_PROPERTY_ARROW_TYPE, SHAPE_STYLE_PROPERTY_CORNER_RADII,
    SHAPE_STYLE_PROPERTY_END_ARROWHEAD, SHAPE_STYLE_PROPERTY_FILL, SHAPE_STYLE_PROPERTY_FILL_STYLE,
    SHAPE_STYLE_PROPERTY_HIGHLIGHT_SHAPE, SHAPE_STYLE_PROPERTY_OPACITY, SHAPE_STYLE_PROPERTY_SHAPE,
    SHAPE_STYLE_PROPERTY_START_ARROWHEAD, SHAPE_STYLE_PROPERTY_STROKE,
    SHAPE_STYLE_PROPERTY_STROKE_STYLE, SHAPE_STYLE_PROPERTY_STROKE_WIDTH, SelectionArrowState,
    SelectionRectState, SerialNumberStyle, ShapeKind, ShapeStyle, ShapeStylePatch,
    StyleToolbarSource, TEXT_STYLE_MIXED_COLOR, TEXT_STYLE_MIXED_CORNER_RADII,
    TEXT_STYLE_MIXED_FILL, TEXT_STYLE_MIXED_FILL_STYLE, TEXT_STYLE_MIXED_FONT_FAMILY,
    TEXT_STYLE_MIXED_FONT_SIZE, TEXT_STYLE_MIXED_HORIZONTAL_ALIGN, TEXT_STYLE_MIXED_OPACITY,
    TEXT_STYLE_MIXED_STROKE, TEXT_STYLE_MIXED_STROKE_WIDTH, TEXT_STYLE_MIXED_VERTICAL_ALIGN,
    TextLayoutOverride, TextStyle, arrow_with_style, selection_bounds_from_selection,
    text::{text_layout_override_size, text_with_style_attributes},
};

const FONT_SIZE_STEPS: [f64; 5] = [MIN_TEXT_FONT_SIZE, 16.0, 21.0, 27.0, 42.0];

pub(crate) fn stepped_font_size(current: f64, increase: bool) -> f64 {
    if increase {
        FONT_SIZE_STEPS
            .iter()
            .copied()
            .find(|step| *step > current + f64::EPSILON)
            .unwrap_or(
                *FONT_SIZE_STEPS
                    .last()
                    .expect("font size steps are nonempty"),
            )
    } else {
        FONT_SIZE_STEPS
            .iter()
            .rev()
            .copied()
            .find(|step| *step < current - f64::EPSILON)
            .unwrap_or(FONT_SIZE_STEPS[0])
    }
}

impl ShapeStyle {
    pub fn rectangle_shape_style(self) -> RectangleShapeStyle {
        RectangleShapeStyle {
            fill: self.fill,
            fill_style: self.fill_style,
            stroke: self.stroke,
            stroke_width: self.stroke_width,
            stroke_style: self.stroke_style,
            corner_radii: self.corner_radii,
            shape: self.shape,
        }
    }

    pub fn arrow_style(self) -> ArrowStyle {
        ArrowStyle {
            stroke: self.stroke,
            stroke_width: self.stroke_width,
            start_arrowhead: self.start_arrowhead,
            end_arrowhead: self.end_arrowhead,
            stroke_style: self.stroke_style,
            arrow_type: self.arrow_type,
        }
    }

    pub fn with_rectangle_shape_style(mut self, style: RectangleShapeStyle) -> Self {
        self.fill = style.fill;
        self.fill_style = style.fill_style;
        self.stroke = style.stroke;
        self.stroke_width = style.stroke_width;
        self.stroke_style = style.stroke_style;
        self.corner_radii = style.corner_radii;
        self.shape = style.shape;
        self
    }

    pub fn with_arrow_style(mut self, style: ArrowStyle) -> Self {
        self.stroke = style.stroke;
        self.stroke_width = style.stroke_width;
        self.start_arrowhead = style.start_arrowhead;
        self.end_arrowhead = style.end_arrowhead;
        self.stroke_style = style.stroke_style;
        self.arrow_type = style.arrow_type;
        self
    }

    pub(crate) fn from_rectangle(rect: &RectangleData) -> Self {
        let mut style = Self::from_rectangle_shape_style(RectangleShapeStyle::from_rectangle(rect));
        style.opacity = rect.opacity;
        style.highlight_shape = rect.highlight_shape;
        style.shape = rect.highlight_shape;
        style
    }

    pub(crate) fn from_rectangle_shape_style(style: RectangleShapeStyle) -> Self {
        Self {
            fill: style.fill,
            fill_style: style.fill_style,
            stroke: style.stroke,
            stroke_width: style.stroke_width,
            corner_radii: style.corner_radii,
            start_arrowhead: None,
            end_arrowhead: None,
            stroke_style: style.stroke_style,
            arrow_type: ArrowType::Straight,
            opacity: 1.0,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            shape: style.shape,
        }
    }

    pub(crate) fn with_opacity(mut self, opacity: f64) -> Self {
        self.opacity = opacity;
        self
    }
}

impl ShapeStylePatch {
    fn active_properties(self) -> u32 {
        self.properties & self.kind.supported_properties()
    }

    fn is_empty(self) -> bool {
        self.active_properties() == 0
    }

    fn has_unsupported_properties(self) -> bool {
        self.properties & !self.kind.supported_properties() != 0
    }

    fn apply_to_rectangle_shape(self, mut current: RectangleShapeStyle) -> RectangleShapeStyle {
        let properties = self.active_properties();
        if properties & SHAPE_STYLE_PROPERTY_FILL != 0 {
            current.fill = self.style.fill;
        }
        if properties & SHAPE_STYLE_PROPERTY_FILL_STYLE != 0 {
            current.fill_style = self.style.fill_style;
        }
        if properties & SHAPE_STYLE_PROPERTY_STROKE != 0 {
            current.stroke = self.style.stroke;
        }
        if properties & SHAPE_STYLE_PROPERTY_STROKE_WIDTH != 0 {
            current.stroke_width = self.style.stroke_width;
        }
        if properties & SHAPE_STYLE_PROPERTY_CORNER_RADII != 0 {
            current.corner_radii = self.style.corner_radii;
        }
        if properties & SHAPE_STYLE_PROPERTY_STROKE_STYLE != 0 {
            current.stroke_style = self.style.stroke_style;
        }
        if properties & SHAPE_STYLE_PROPERTY_SHAPE != 0 {
            current.shape = self.style.shape;
        }
        current
    }

    fn apply_to_arrow(self, mut current: ArrowStyle) -> ArrowStyle {
        let properties = self.active_properties();
        if properties & SHAPE_STYLE_PROPERTY_STROKE != 0 {
            current.stroke = self.style.stroke;
        }
        if properties & SHAPE_STYLE_PROPERTY_STROKE_WIDTH != 0 {
            current.stroke_width = self.style.stroke_width;
        }
        if properties & SHAPE_STYLE_PROPERTY_START_ARROWHEAD != 0 {
            current.start_arrowhead = self.style.start_arrowhead;
        }
        if properties & SHAPE_STYLE_PROPERTY_END_ARROWHEAD != 0 {
            current.end_arrowhead = self.style.end_arrowhead;
        }
        if properties & SHAPE_STYLE_PROPERTY_STROKE_STYLE != 0 {
            current.stroke_style = self.style.stroke_style;
        }
        if properties & SHAPE_STYLE_PROPERTY_ARROW_TYPE != 0 {
            current.arrow_type = self.style.arrow_type;
        }
        current
    }

    fn apply_to_line(self, mut current: ShapeStyle) -> ShapeStyle {
        let properties = self.active_properties();
        if properties & SHAPE_STYLE_PROPERTY_FILL != 0 {
            current.fill = self.style.fill;
        }
        if properties & SHAPE_STYLE_PROPERTY_FILL_STYLE != 0 {
            current.fill_style = self.style.fill_style;
        }
        if properties & SHAPE_STYLE_PROPERTY_STROKE != 0 {
            current.stroke = self.style.stroke;
        }
        if properties & SHAPE_STYLE_PROPERTY_STROKE_WIDTH != 0 {
            current.stroke_width = self.style.stroke_width;
        }
        if properties & SHAPE_STYLE_PROPERTY_STROKE_STYLE != 0 {
            current.stroke_style = self.style.stroke_style;
        }
        if properties & SHAPE_STYLE_PROPERTY_OPACITY != 0 {
            current.opacity = self.style.opacity;
        }
        current.start_arrowhead = None;
        current.end_arrowhead = None;
        current.arrow_type = ArrowType::Curve;
        current
    }

    fn apply_to_highlight(self, mut current: ShapeStyle) -> ShapeStyle {
        let properties = self.active_properties();
        let rectangle = self.apply_to_rectangle_shape(current.rectangle_shape_style());
        current = current.with_rectangle_shape_style(rectangle);
        if properties & SHAPE_STYLE_PROPERTY_OPACITY != 0 {
            current.opacity = self.style.opacity;
        }
        if properties & SHAPE_STYLE_PROPERTY_HIGHLIGHT_SHAPE != 0 {
            current.highlight_shape = self.style.highlight_shape;
        }
        current
    }
}

#[derive(Clone, Copy)]
struct ShapeStyleSample {
    fill: Option<ColorRgba8>,
    fill_style: Option<FillStyle>,
    stroke: Option<ColorRgba8>,
    stroke_width: Option<f64>,
    corner_radii: Option<CornerRadii>,
    stroke_style: Option<StrokeStyle>,
    start_arrowhead: Option<Option<Arrowhead>>,
    end_arrowhead: Option<Option<Arrowhead>>,
    arrow_type: Option<ArrowType>,
    opacity: f64,
    highlight_shape: Option<snow_draw_engine_document::HighlightShape>,
    shape: Option<snow_draw_engine_document::HighlightShape>,
}

impl ShapeStyleSample {
    fn from_rectangle(rectangle: &RectangleData) -> Self {
        Self {
            fill: Some(rectangle.fill),
            fill_style: Some(rectangle.fill_style),
            stroke: Some(rectangle.stroke),
            stroke_width: Some(rectangle.stroke_width),
            corner_radii: Some(rectangle.corner_radii),
            stroke_style: Some(rectangle.stroke_style),
            start_arrowhead: None,
            end_arrowhead: None,
            arrow_type: None,
            opacity: rectangle.opacity,
            highlight_shape: rectangle
                .is_highlight()
                .then_some(rectangle.highlight_shape),
            shape: (!rectangle.is_highlight()).then_some(rectangle.highlight_shape),
        }
    }

    fn from_arrow(arrow: &ArrowData) -> Self {
        Self {
            fill: arrow.is_line().then_some(arrow.fill),
            fill_style: arrow.is_line().then_some(arrow.fill_style),
            stroke: Some(arrow.stroke),
            stroke_width: Some(arrow.stroke_width),
            corner_radii: None,
            stroke_style: Some(arrow.stroke_style),
            start_arrowhead: (!arrow.is_line()).then_some(arrow.start_arrowhead),
            end_arrowhead: (!arrow.is_line()).then_some(arrow.end_arrowhead),
            arrow_type: (!arrow.is_line()).then_some(arrow.arrow_type),
            opacity: arrow.opacity,
            highlight_shape: None,
            shape: None,
        }
    }

    fn from_free_draw(free_draw: &snow_draw_engine_document::FreeDrawData) -> Self {
        Self {
            fill: Some(free_draw.fill),
            fill_style: Some(free_draw.fill_style),
            stroke: Some(free_draw.stroke),
            stroke_width: Some(free_draw.stroke_width),
            corner_radii: None,
            stroke_style: Some(free_draw.stroke_style),
            start_arrowhead: None,
            end_arrowhead: None,
            arrow_type: None,
            opacity: free_draw.opacity,
            highlight_shape: None,
            shape: None,
        }
    }
}

impl RectangleShapeStyle {
    pub(crate) fn from_rectangle(rect: &RectangleData) -> Self {
        Self {
            fill: rect.fill,
            fill_style: rect.fill_style,
            stroke: rect.stroke,
            stroke_width: rect.stroke_width,
            stroke_style: rect.stroke_style,
            corner_radii: rect.corner_radii,
            shape: rect.highlight_shape,
        }
    }
}

impl ArrowStyle {
    pub(crate) fn from_arrow(arrow: &ArrowData) -> Self {
        Self {
            stroke: arrow.stroke,
            stroke_width: arrow.stroke_width,
            start_arrowhead: arrow.start_arrowhead,
            end_arrowhead: arrow.end_arrowhead,
            stroke_style: arrow.stroke_style,
            arrow_type: arrow.arrow_type,
        }
    }
}

impl ShapeStyle {
    pub(crate) fn from_line(line: &ArrowData) -> Self {
        Self {
            fill: line.fill,
            fill_style: line.fill_style,
            stroke: line.stroke,
            stroke_width: line.stroke_width,
            corner_radii: CornerRadii::default(),
            start_arrowhead: None,
            end_arrowhead: None,
            stroke_style: line.stroke_style,
            arrow_type: ArrowType::Curve,
            opacity: line.opacity,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            shape: snow_draw_engine_document::HighlightShape::Rectangle,
        }
    }

    pub(crate) fn from_free_draw(free_draw: &snow_draw_engine_document::FreeDrawData) -> Self {
        Self {
            fill: free_draw.fill,
            fill_style: free_draw.fill_style,
            stroke: free_draw.stroke,
            stroke_width: free_draw.stroke_width,
            corner_radii: CornerRadii::default(),
            start_arrowhead: None,
            end_arrowhead: None,
            stroke_style: free_draw.stroke_style,
            arrow_type: ArrowType::Curve,
            opacity: free_draw.opacity,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            shape: snow_draw_engine_document::HighlightShape::Rectangle,
        }
    }
}

impl SerialNumberStyle {
    pub(crate) fn from_serial_number(serial: &SerialNumberData) -> Self {
        Self {
            number: serial.number.max(0),
            color: serial.color,
            fill: serial.fill,
            fill_style: serial.fill_style,
            font_size: serial.font_size,
            font_family: serial.font_family.clone(),
            stroke_width: serial.stroke_width,
            stroke_style: serial.stroke_style,
            opacity: serial.opacity,
        }
    }
}
fn validate_shape_style_patch(patch: ShapeStylePatch) -> Result<(), ErrorCode> {
    if patch.has_unsupported_properties() {
        return Err(ErrorCode::InvalidArgument);
    }

    match patch.kind {
        ShapeKind::Rectangle => validate_rectangle_shape_style(patch.style.rectangle_shape_style()),
        ShapeKind::Arrow => validate_arrow_style(patch.style.arrow_style()),
        ShapeKind::Line
        | ShapeKind::FreeDraw
        | ShapeKind::RectangleHighlight
        | ShapeKind::PenHighlight => validate_line_style(patch.style),
        ShapeKind::Spotlight => Ok(()),
    }
}

pub(crate) fn validate_line_style(style: ShapeStyle) -> Result<(), ErrorCode> {
    if !style.stroke_width.is_finite()
        || style.stroke_width < 0.0
        || !style.opacity.is_finite()
        || !(0.0..=1.0).contains(&style.opacity)
    {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok(())
}

pub(crate) fn validate_rectangle_shape_style(style: RectangleShapeStyle) -> Result<(), ErrorCode> {
    let scalar_fields = [
        style.stroke_width,
        style.corner_radii.top_left,
        style.corner_radii.top_right,
        style.corner_radii.bottom_right,
        style.corner_radii.bottom_left,
    ];
    if scalar_fields
        .iter()
        .any(|value| !value.is_finite() || *value < 0.0)
    {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok(())
}

pub(crate) fn validate_arrow_style(style: ArrowStyle) -> Result<(), ErrorCode> {
    if !style.stroke_width.is_finite() || style.stroke_width < 0.0 {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok(())
}

pub(crate) fn validate_text_style(style: &TextStyle) -> Result<(), ErrorCode> {
    let scalar_fields = [
        style.font_size,
        style.stroke_width,
        style.corner_radii.top_left,
        style.corner_radii.top_right,
        style.corner_radii.bottom_right,
        style.corner_radii.bottom_left,
        style.opacity,
    ];
    if scalar_fields
        .iter()
        .any(|value| !value.is_finite() || *value < 0.0)
        || style.opacity > 1.0
        || style.font_size < MIN_TEXT_FONT_SIZE
    {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok(())
}

pub(crate) fn validate_serial_number_style(style: &SerialNumberStyle) -> Result<(), ErrorCode> {
    let scalar_fields = [style.font_size, style.stroke_width, style.opacity];
    if style.number < 0
        || scalar_fields
            .iter()
            .any(|value| !value.is_finite() || *value < 0.0)
        || style.font_size < MIN_SERIAL_NUMBER_FONT_SIZE
        || style.opacity > 1.0
    {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok(())
}

pub(crate) fn rectangle_with_style(
    rect: &RectangleData,
    style: RectangleShapeStyle,
) -> RectangleData {
    RectangleData {
        rectangle_kind: rect.rectangle_kind,
        highlight_shape: style.shape,
        center: rect.center,
        width: rect.width,
        height: rect.height,
        rotation: rect.rotation,
        fill: style.fill,
        fill_style: style.fill_style,
        stroke: style.stroke,
        stroke_width: style.stroke_width,
        stroke_style: style.stroke_style,
        corner_radii: normalize_corner_radii(rect.width, rect.height, style.corner_radii),
        opacity: rect.opacity,
    }
}

fn text_with_style(
    id: snow_draw_engine_document::ElementId,
    text: &TextData,
    style: &TextStyle,
    layouts: &[TextLayoutOverride],
) -> Result<TextData, ErrorCode> {
    let mut updated = text_with_style_attributes(text, style);
    if updated.auto_resize {
        let layout = text_layout_override_size(layouts, id)?;
        updated = text_with_auto_resize_layout(&updated, layout)?;
    }
    Ok(updated)
}

fn serial_number_with_style(
    serial: &SerialNumberData,
    style: &SerialNumberStyle,
) -> SerialNumberData {
    let size_affecting_style_changed =
        serial.number != style.number.max(0) || serial.font_size != style.font_size;
    let mut updated = if size_affecting_style_changed {
        serial_number_with_label_style(serial, style.number, style.font_size)
    } else {
        serial.clone()
    };
    updated.color = style.color;
    updated.fill = style.fill;
    updated.fill_style = style.fill_style;
    updated.font_family = normalize_font_family(style.font_family.clone());
    updated.stroke_width = style.stroke_width;
    updated.stroke_style = style.stroke_style;
    updated.opacity = style.opacity;
    updated
}

impl Editor {
    fn active_stroke_cursor_width(&self) -> Option<f64> {
        match self.state.active_tool {
            ActiveTool::FreeDraw => Some(self.state.default_free_draw_style.stroke_width),
            ActiveTool::PenHighlight => Some(self.state.default_pen_highlight_style.stroke_width),
            ActiveTool::PenFilter => Some(self.state.default_pen_filter.stroke_width),
            _ => None,
        }
    }

    pub fn watermark_config(&self, document: &DocumentModel) -> WatermarkConfig {
        document.watermark_config().clone()
    }

    pub fn set_watermark_config(
        &mut self,
        document: &DocumentModel,
        config: WatermarkConfig,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        let config = config.normalized();
        snow_draw_engine_document::validate_watermark_config(&config)?;
        if document.watermark_config() == &config {
            return Ok(None);
        }
        let snapshot = self.capture_document_sync_snapshot(document);
        let mut transaction = Transaction::new("Update watermark");
        transaction.update_watermark(config);
        Ok(Some(EditorCommand::ApplyTransaction(
            ApplyTransactionCommand::with_history_undo_snapshot(transaction, snapshot),
        )))
    }

    pub fn spotlight_config(&self, document: &DocumentModel) -> SpotlightConfig {
        document.spotlight_config()
    }

    pub fn set_spotlight_config(
        &mut self,
        document: &DocumentModel,
        config: SpotlightConfig,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        let config = config.normalized();
        snow_draw_engine_document::validate_spotlight_config(&config)?;
        if document.spotlight_config() == config {
            return Ok(None);
        }
        let snapshot = self.capture_document_sync_snapshot(document);
        let mut transaction = Transaction::new("Update spotlight");
        transaction.update_spotlight(config);
        Ok(Some(EditorCommand::ApplyTransaction(
            ApplyTransactionCommand::with_history_undo_snapshot(transaction, snapshot),
        )))
    }

    pub fn style_toolbar_source(&self, document: &DocumentModel) -> StyleToolbarSource {
        if self.state.active_tool == ActiveTool::Watermark {
            return StyleToolbarSource::Watermark;
        }
        if self.state.active_tool == ActiveTool::Spotlight && self.state.selection.ids.is_empty() {
            return StyleToolbarSource::DefaultSpotlight;
        }
        if self.state.active_tool == ActiveTool::Eraser {
            return StyleToolbarSource::Eraser;
        }
        let primary = self.state.selection.primary;
        if primary.is_some_and(|id| document.pen_filter(id).is_ok()) {
            StyleToolbarSource::SelectedPenFilter
        } else if primary.is_some_and(|id| document.filter(id).is_ok()) {
            StyleToolbarSource::SelectedRectangleFilter
        } else if self
            .state
            .selection
            .ids
            .iter()
            .any(|id| document.pen_filter(*id).is_ok())
        {
            StyleToolbarSource::SelectedPenFilter
        } else if self
            .state
            .selection
            .ids
            .iter()
            .any(|id| document.filter(*id).is_ok())
        {
            StyleToolbarSource::SelectedRectangleFilter
        } else if primary.is_some_and(|id| document.text(id).is_ok()) {
            StyleToolbarSource::SelectedText
        } else if primary.is_some_and(|id| document.serial_number(id).is_ok()) {
            StyleToolbarSource::SelectedSerialNumber
        } else if self
            .state
            .selection
            .ids
            .iter()
            .any(|id| document.text(*id).is_ok())
        {
            StyleToolbarSource::SelectedText
        } else if self
            .state
            .selection
            .ids
            .iter()
            .any(|id| document.serial_number(*id).is_ok())
        {
            StyleToolbarSource::SelectedSerialNumber
        } else if self.state.selection.ids.iter().any(|id| {
            document
                .rectangle(*id)
                .is_ok_and(RectangleData::is_spotlight)
        }) {
            StyleToolbarSource::SelectedSpotlight
        } else if self.selected_rectangle_style(document).is_some() {
            StyleToolbarSource::SelectedRectangle
        } else if self.selected_rectangle_highlight_style(document).is_some() {
            StyleToolbarSource::SelectedRectangleHighlight
        } else if self.selected_pen_highlight_style(document).is_some() {
            StyleToolbarSource::SelectedPenHighlight
        } else if self.selected_line_style(document).is_some() {
            StyleToolbarSource::SelectedLine
        } else if self.selected_free_draw_style(document).is_some() {
            StyleToolbarSource::SelectedFreeDraw
        } else if self.selected_arrow_style(document).is_some() {
            StyleToolbarSource::SelectedArrow
        } else if self.state.active_tool == ActiveTool::Line {
            StyleToolbarSource::DefaultLine
        } else if self.state.active_tool == ActiveTool::FreeDraw {
            StyleToolbarSource::DefaultFreeDraw
        } else if self.state.active_tool == ActiveTool::RectangleHighlight {
            StyleToolbarSource::DefaultRectangleHighlight
        } else if self.state.active_tool == ActiveTool::Spotlight {
            StyleToolbarSource::DefaultSpotlight
        } else if self.state.active_tool == ActiveTool::PenHighlight {
            StyleToolbarSource::DefaultPenHighlight
        } else if self.state.active_tool == ActiveTool::PenFilter {
            StyleToolbarSource::DefaultPenFilter
        } else if self.state.active_tool == ActiveTool::RectangleFilter {
            StyleToolbarSource::DefaultRectangleFilter
        } else if self.state.active_tool == ActiveTool::Arrow {
            StyleToolbarSource::DefaultArrow
        } else if self.state.active_tool == ActiveTool::Text {
            StyleToolbarSource::DefaultText
        } else if self.state.active_tool == ActiveTool::SerialNumber {
            StyleToolbarSource::DefaultSerialNumber
        } else {
            StyleToolbarSource::DefaultRectangle
        }
    }

    pub fn filter_style(&self, document: &DocumentModel) -> FilterStyle {
        let selected = self.state.selection.ids.iter().find_map(|id| {
            if let Ok(filter) = document.pen_filter(*id) {
                return Some(FilterStyle {
                    filter_type: filter.filter_type,
                    strength: filter.strength,
                    opacity: filter.opacity,
                    stroke_width: filter.stroke_width,
                });
            }
            document.filter(*id).ok().map(|filter| FilterStyle {
                filter_type: filter.filter_type,
                strength: filter.strength,
                opacity: filter.opacity,
                stroke_width: 2.0,
            })
        });
        selected.unwrap_or_else(|| {
            if self.state.active_tool == ActiveTool::PenFilter {
                let filter = &self.state.default_pen_filter;
                FilterStyle {
                    filter_type: filter.filter_type,
                    strength: filter.strength,
                    opacity: filter.opacity,
                    stroke_width: filter.stroke_width,
                }
            } else {
                let filter = &self.state.default_filter;
                FilterStyle {
                    filter_type: filter.filter_type,
                    strength: filter.strength,
                    opacity: filter.opacity,
                    stroke_width: self.state.default_filter_stroke_width,
                }
            }
        })
    }

    pub fn filter_style_mixed(&self, document: &DocumentModel) -> u32 {
        let selected = self
            .state
            .selection
            .ids
            .iter()
            .filter_map(|id| {
                document
                    .pen_filter(*id)
                    .ok()
                    .map(|filter| FilterStyle {
                        filter_type: filter.filter_type,
                        strength: filter.strength,
                        opacity: filter.opacity,
                        stroke_width: filter.stroke_width,
                    })
                    .or_else(|| {
                        document.filter(*id).ok().map(|filter| FilterStyle {
                            filter_type: filter.filter_type,
                            strength: filter.strength,
                            opacity: filter.opacity,
                            stroke_width: 0.0,
                        })
                    })
            })
            .collect::<Vec<_>>();
        let Some(first) = selected.first() else {
            return 0;
        };
        selected.iter().skip(1).fold(0, |mixed, filter| {
            mixed
                | if filter.filter_type != first.filter_type {
                    FILTER_STYLE_PROPERTY_TYPE
                } else {
                    0
                }
                | if (filter.strength - first.strength).abs() > f64::EPSILON {
                    FILTER_STYLE_PROPERTY_STRENGTH
                } else {
                    0
                }
                | if (filter.opacity - first.opacity).abs() > f64::EPSILON {
                    FILTER_STYLE_PROPERTY_OPACITY
                } else {
                    0
                }
                | if (filter.stroke_width - first.stroke_width).abs() > f64::EPSILON {
                    FILTER_STYLE_PROPERTY_STROKE_WIDTH
                } else {
                    0
                }
        })
    }

    pub fn set_filter_style(
        &mut self,
        document: &DocumentModel,
        style: FilterStyle,
        properties: u32,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        if properties
            & !(FILTER_STYLE_PROPERTY_TYPE
                | FILTER_STYLE_PROPERTY_STRENGTH
                | FILTER_STYLE_PROPERTY_OPACITY
                | FILTER_STYLE_PROPERTY_STROKE_WIDTH)
            != 0
            || !style.opacity.is_finite()
            || !(0.0..=1.0).contains(&style.opacity)
            || (properties & FILTER_STYLE_PROPERTY_STROKE_WIDTH != 0
                && (!style.stroke_width.is_finite() || !(1.0..=72.0).contains(&style.stroke_width)))
        {
            return Err(ErrorCode::InvalidArgument);
        }
        let strength = FilterData::normalized_strength(style.strength);
        let selected_ids = self
            .state
            .selection
            .ids
            .iter()
            .copied()
            .filter(|id| document.filter(*id).is_ok() || document.pen_filter(*id).is_ok())
            .collect::<Vec<_>>();
        if selected_ids.is_empty() {
            let previous_stroke_cursor_width = self.active_stroke_cursor_width();
            if self.state.active_tool == ActiveTool::PenFilter {
                if properties & FILTER_STYLE_PROPERTY_TYPE != 0 {
                    self.state.default_pen_filter.filter_type = style.filter_type;
                }
                if properties & FILTER_STYLE_PROPERTY_STRENGTH != 0 {
                    self.state.default_pen_filter.strength = strength;
                }
                if properties & FILTER_STYLE_PROPERTY_OPACITY != 0 {
                    self.state.default_pen_filter.opacity = style.opacity;
                }
                if properties & FILTER_STYLE_PROPERTY_STROKE_WIDTH != 0 {
                    self.state.default_pen_filter.stroke_width = style.stroke_width;
                }
            } else {
                if properties & FILTER_STYLE_PROPERTY_TYPE != 0 {
                    self.state.default_filter.filter_type = style.filter_type;
                }
                if properties & FILTER_STYLE_PROPERTY_STRENGTH != 0 {
                    self.state.default_filter.strength = strength;
                }
                if properties & FILTER_STYLE_PROPERTY_OPACITY != 0 {
                    self.state.default_filter.opacity = style.opacity;
                }
                if properties & FILTER_STYLE_PROPERTY_STROKE_WIDTH != 0 {
                    self.state.default_filter_stroke_width = style.stroke_width;
                }
            }
            if self.active_stroke_cursor_width() != previous_stroke_cursor_width {
                self.bump_overlay_state_revision();
            }
            return Ok(None);
        }
        let mut transaction = Transaction::new("update filter style");
        for id in selected_ids {
            if let Ok(current) = document.pen_filter(id) {
                let mut updated = current.clone();
                if properties & FILTER_STYLE_PROPERTY_TYPE != 0 {
                    updated.filter_type = style.filter_type;
                }
                if properties & FILTER_STYLE_PROPERTY_STRENGTH != 0 {
                    updated.strength = strength;
                }
                if properties & FILTER_STYLE_PROPERTY_OPACITY != 0 {
                    updated.opacity = style.opacity;
                }
                if properties & FILTER_STYLE_PROPERTY_STROKE_WIDTH != 0 {
                    updated.stroke_width = style.stroke_width;
                }
                if updated != *current {
                    transaction.update_pen_filter(id, updated);
                }
            } else if let Ok(current) = document.filter(id) {
                let mut updated = *current;
                if properties & FILTER_STYLE_PROPERTY_TYPE != 0 {
                    updated.filter_type = style.filter_type;
                }
                if properties & FILTER_STYLE_PROPERTY_STRENGTH != 0 {
                    updated.strength = strength;
                }
                if properties & FILTER_STYLE_PROPERTY_OPACITY != 0 {
                    updated.opacity = style.opacity;
                }
                if updated != *current {
                    transaction.update_filter(id, updated);
                }
            }
        }
        if transaction.is_empty() {
            return Ok(None);
        }
        Ok(Some(EditorCommand::ApplyTransaction(
            ApplyTransactionCommand::new(transaction),
        )))
    }

    pub fn shape_style(&self, document: &DocumentModel) -> ShapeStyle {
        self.selected_rectangle_style(document)
            .or_else(|| self.selected_rectangle_highlight_style(document))
            .or_else(|| self.selected_pen_highlight_style(document))
            .or_else(|| self.selected_line_style(document))
            .or_else(|| self.selected_free_draw_style(document))
            .or_else(|| {
                self.selected_arrow_style(document).map(|style| {
                    ShapeStyle::from_rectangle_shape_style(self.state.default_rectangle_shape_style)
                        .with_arrow_style(style)
                        .with_opacity(self.selected_arrow_opacity(document).unwrap_or(1.0))
                })
            })
            .or_else(|| {
                (self.state.active_tool == ActiveTool::Line)
                    .then_some(self.state.default_line_style)
            })
            .or_else(|| {
                (self.state.active_tool == ActiveTool::FreeDraw)
                    .then_some(self.state.default_free_draw_style)
            })
            .or_else(|| {
                (self.state.active_tool == ActiveTool::RectangleHighlight)
                    .then_some(self.state.default_rectangle_highlight_style)
            })
            .or_else(|| {
                (self.state.active_tool == ActiveTool::PenHighlight)
                    .then_some(self.state.default_pen_highlight_style)
            })
            .or_else(|| {
                (self.state.active_tool == ActiveTool::Arrow).then(|| {
                    ShapeStyle::from_rectangle_shape_style(self.state.default_rectangle_shape_style)
                        .with_arrow_style(self.state.default_arrow_style)
                })
            })
            .unwrap_or_else(|| {
                ShapeStyle::from_rectangle_shape_style(self.state.default_rectangle_shape_style)
            })
    }

    pub fn rectangle_shape_style(&self, document: &DocumentModel) -> RectangleShapeStyle {
        self.selected_rectangle_style(document)
            .map(|style| style.rectangle_shape_style())
            .unwrap_or(self.state.default_rectangle_shape_style)
    }

    pub fn arrow_style(&self, document: &DocumentModel) -> ArrowStyle {
        self.selected_arrow_style(document)
            .unwrap_or(self.state.default_arrow_style)
    }

    pub fn shape_style_mixed(&self, document: &DocumentModel) -> u32 {
        let source = self.style_toolbar_source(document);
        let mut selected = self
            .state
            .selection
            .ids
            .iter()
            .filter_map(|id| match source {
                StyleToolbarSource::SelectedRectangle => document
                    .rectangle(*id)
                    .ok()
                    .filter(|rect| !rect.is_highlight())
                    .map(ShapeStyleSample::from_rectangle),
                StyleToolbarSource::SelectedRectangleHighlight => document
                    .rectangle(*id)
                    .ok()
                    .filter(|rect| rect.is_highlight())
                    .map(ShapeStyleSample::from_rectangle),
                StyleToolbarSource::SelectedPenHighlight => document
                    .arrow(*id)
                    .ok()
                    .filter(|arrow| arrow.is_pen_highlight())
                    .map(ShapeStyleSample::from_arrow),
                StyleToolbarSource::SelectedArrow => document
                    .arrow(*id)
                    .ok()
                    .filter(|arrow| !arrow.is_line() && !arrow.is_pen_highlight())
                    .map(ShapeStyleSample::from_arrow),
                StyleToolbarSource::SelectedLine => document
                    .arrow(*id)
                    .ok()
                    .filter(|arrow| arrow.is_line())
                    .map(ShapeStyleSample::from_arrow),
                StyleToolbarSource::SelectedFreeDraw => document
                    .free_draw(*id)
                    .ok()
                    .map(ShapeStyleSample::from_free_draw),
                _ => None,
            });
        let Some(first) = selected.next() else {
            return 0;
        };

        selected.fold(0, |mut mixed, style| {
            if style.fill != first.fill {
                mixed |= SHAPE_STYLE_MIXED_FILL;
            }
            if style.fill_style != first.fill_style {
                mixed |= SHAPE_STYLE_MIXED_FILL_STYLE;
            }
            if style.stroke != first.stroke {
                mixed |= SHAPE_STYLE_MIXED_STROKE;
            }
            if style.stroke_width != first.stroke_width {
                mixed |= SHAPE_STYLE_MIXED_STROKE_WIDTH;
            }
            if style.corner_radii != first.corner_radii {
                mixed |= SHAPE_STYLE_MIXED_CORNER_RADII;
            }
            if style.stroke_style != first.stroke_style {
                mixed |= SHAPE_STYLE_MIXED_STROKE_STYLE;
            }
            if style.start_arrowhead != first.start_arrowhead {
                mixed |= SHAPE_STYLE_MIXED_START_ARROWHEAD;
            }
            if style.end_arrowhead != first.end_arrowhead {
                mixed |= SHAPE_STYLE_MIXED_END_ARROWHEAD;
            }
            if style.arrow_type != first.arrow_type {
                mixed |= SHAPE_STYLE_MIXED_ARROW_TYPE;
            }
            if style.opacity != first.opacity {
                mixed |= SHAPE_STYLE_MIXED_OPACITY;
            }
            if style.highlight_shape != first.highlight_shape {
                mixed |= SHAPE_STYLE_MIXED_HIGHLIGHT_SHAPE;
            }
            if style.shape != first.shape {
                mixed |= SHAPE_STYLE_MIXED_SHAPE;
            }
            mixed
        })
    }

    fn selected_rectangle_style(&self, document: &DocumentModel) -> Option<ShapeStyle> {
        self.selected_primary_rectangle_snapshot(document)
            .map(|(_, rectangle)| rectangle)
            .filter(|rectangle| !rectangle.is_highlight())
            .or_else(|| {
                self.state
                    .selection
                    .ids
                    .iter()
                    .find_map(|id| {
                        document
                            .rectangle(*id)
                            .ok()
                            .filter(|rect| !rect.is_highlight())
                    })
                    .cloned()
            })
            .map(|rectangle| ShapeStyle::from_rectangle(&rectangle))
    }

    fn selected_rectangle_highlight_style(&self, document: &DocumentModel) -> Option<ShapeStyle> {
        self.state
            .selection
            .primary
            .and_then(|id| document.rectangle(id).ok())
            .filter(|rect| rect.is_highlight())
            .or_else(|| {
                self.state.selection.ids.iter().find_map(|id| {
                    document
                        .rectangle(*id)
                        .ok()
                        .filter(|rect| rect.is_highlight())
                })
            })
            .map(ShapeStyle::from_rectangle)
    }

    fn selected_pen_highlight_style(&self, document: &DocumentModel) -> Option<ShapeStyle> {
        self.state
            .selection
            .primary
            .and_then(|id| document.arrow(id).ok())
            .filter(|pen| pen.is_pen_highlight())
            .or_else(|| {
                self.state.selection.ids.iter().find_map(|id| {
                    document
                        .arrow(*id)
                        .ok()
                        .filter(|pen| pen.is_pen_highlight())
                })
            })
            .map(ShapeStyle::from_line)
    }

    fn selected_arrow_style(&self, document: &DocumentModel) -> Option<ArrowStyle> {
        self.state
            .selection
            .primary
            .and_then(|id| document.arrow(id).ok())
            .filter(|arrow| !arrow.is_line() && !arrow.is_pen_highlight())
            .or_else(|| {
                self.state.selection.ids.iter().find_map(|id| {
                    document
                        .arrow(*id)
                        .ok()
                        .filter(|arrow| !arrow.is_line() && !arrow.is_pen_highlight())
                })
            })
            .map(ArrowStyle::from_arrow)
    }

    fn selected_line_style(&self, document: &DocumentModel) -> Option<ShapeStyle> {
        self.state
            .selection
            .primary
            .and_then(|id| document.arrow(id).ok())
            .filter(|line| line.is_line())
            .or_else(|| {
                self.state
                    .selection
                    .ids
                    .iter()
                    .find_map(|id| document.arrow(*id).ok().filter(|line| line.is_line()))
            })
            .map(ShapeStyle::from_line)
    }

    fn selected_free_draw_style(&self, document: &DocumentModel) -> Option<ShapeStyle> {
        self.state
            .selection
            .primary
            .and_then(|id| document.free_draw(id).ok())
            .or_else(|| {
                self.state
                    .selection
                    .ids
                    .iter()
                    .find_map(|id| document.free_draw(*id).ok())
            })
            .map(ShapeStyle::from_free_draw)
    }

    fn selected_arrow_opacity(&self, document: &DocumentModel) -> Option<f64> {
        self.state
            .selection
            .primary
            .and_then(|id| document.arrow(id).ok())
            .filter(|arrow| !arrow.is_line())
            .or_else(|| {
                self.state
                    .selection
                    .ids
                    .iter()
                    .find_map(|id| document.arrow(*id).ok().filter(|arrow| !arrow.is_line()))
            })
            .map(|arrow| arrow.opacity)
    }

    pub fn text_style(&self, document: &DocumentModel) -> TextStyle {
        self.state
            .selection
            .primary
            .and_then(|id| document.text(id).ok())
            .or_else(|| {
                self.state
                    .selection
                    .ids
                    .iter()
                    .find_map(|id| document.text(*id).ok())
            })
            .map(TextStyle::from_text)
            .unwrap_or_else(|| TextStyle::from_text(&self.state.default_text))
    }

    pub fn text_style_mixed(&self, document: &DocumentModel) -> u32 {
        let mut selected_texts = self
            .state
            .selection
            .ids
            .iter()
            .filter_map(|id| document.text(*id).ok());
        let Some(first) = selected_texts.next() else {
            return 0;
        };
        let first = TextStyle::from_text(first);
        selected_texts.fold(0, |mut mixed, text| {
            let style = TextStyle::from_text(text);
            if style.color != first.color {
                mixed |= TEXT_STYLE_MIXED_COLOR;
            }
            if style.font_size != first.font_size {
                mixed |= TEXT_STYLE_MIXED_FONT_SIZE;
            }
            if style.font_family != first.font_family {
                mixed |= TEXT_STYLE_MIXED_FONT_FAMILY;
            }
            if style.fill != first.fill {
                mixed |= TEXT_STYLE_MIXED_FILL;
            }
            if style.fill_style != first.fill_style {
                mixed |= TEXT_STYLE_MIXED_FILL_STYLE;
            }
            if style.stroke != first.stroke {
                mixed |= TEXT_STYLE_MIXED_STROKE;
            }
            if style.stroke_width != first.stroke_width {
                mixed |= TEXT_STYLE_MIXED_STROKE_WIDTH;
            }
            if style.corner_radii != first.corner_radii {
                mixed |= TEXT_STYLE_MIXED_CORNER_RADII;
            }
            if style.horizontal_align != first.horizontal_align {
                mixed |= TEXT_STYLE_MIXED_HORIZONTAL_ALIGN;
            }
            if style.vertical_align != first.vertical_align {
                mixed |= TEXT_STYLE_MIXED_VERTICAL_ALIGN;
            }
            if style.opacity != first.opacity {
                mixed |= TEXT_STYLE_MIXED_OPACITY;
            }
            mixed
        })
    }

    pub fn serial_number_style(&self, document: &DocumentModel) -> SerialNumberStyle {
        self.state
            .selection
            .primary
            .and_then(|id| document.serial_number(id).ok())
            .or_else(|| {
                self.state
                    .selection
                    .ids
                    .iter()
                    .find_map(|id| document.serial_number(*id).ok())
            })
            .map(SerialNumberStyle::from_serial_number)
            .unwrap_or_else(|| {
                SerialNumberStyle::from_serial_number(&self.state.default_serial_number)
            })
    }

    pub fn serial_number_style_mixed(&self, document: &DocumentModel) -> u32 {
        let mut selected_serial_numbers = self
            .state
            .selection
            .ids
            .iter()
            .filter_map(|id| document.serial_number(*id).ok());
        let Some(first) = selected_serial_numbers.next() else {
            return 0;
        };
        let first = SerialNumberStyle::from_serial_number(first);
        selected_serial_numbers.fold(0, |mut mixed, serial| {
            let style = SerialNumberStyle::from_serial_number(serial);
            if style.number != first.number {
                mixed |= SERIAL_NUMBER_STYLE_MIXED_NUMBER;
            }
            if style.color != first.color {
                mixed |= SERIAL_NUMBER_STYLE_MIXED_COLOR;
            }
            if style.fill != first.fill {
                mixed |= SERIAL_NUMBER_STYLE_MIXED_FILL;
            }
            if style.fill_style != first.fill_style {
                mixed |= SERIAL_NUMBER_STYLE_MIXED_FILL_STYLE;
            }
            if style.font_size != first.font_size {
                mixed |= SERIAL_NUMBER_STYLE_MIXED_FONT_SIZE;
            }
            if style.font_family != first.font_family {
                mixed |= SERIAL_NUMBER_STYLE_MIXED_FONT_FAMILY;
            }
            if style.stroke_width != first.stroke_width {
                mixed |= SERIAL_NUMBER_STYLE_MIXED_STROKE_WIDTH;
            }
            if style.stroke_style != first.stroke_style {
                mixed |= SERIAL_NUMBER_STYLE_MIXED_STROKE_STYLE;
            }
            if style.opacity != first.opacity {
                mixed |= SERIAL_NUMBER_STYLE_MIXED_OPACITY;
            }
            mixed
        })
    }

    fn update_default_shape_styles(
        &mut self,
        document: &DocumentModel,
        rectangle_style: RectangleShapeStyle,
        arrow_style: ArrowStyle,
    ) {
        if self.state.default_rectangle_shape_style == rectangle_style
            && self.state.default_arrow_style == arrow_style
        {
            return;
        }

        self.state.default_rectangle_shape_style = rectangle_style;
        self.state.default_arrow_style = arrow_style;
        let bindables = self.bindable_elements(document, &[]);
        let arrow_context = self.arrow_engine_context(Modifiers::default());
        let preview_changed = match self.state.creation_preview.as_mut() {
            Some(ElementCreationPreview::Rectangle(preview)) => {
                let next = rectangle_with_style(preview, rectangle_style);
                if *preview != next {
                    *preview = next;
                    true
                } else {
                    false
                }
            }
            Some(ElementCreationPreview::Arrow(preview)) => {
                let next = arrow_with_style(
                    document.peek_next_element_id(),
                    preview,
                    arrow_style,
                    &bindables,
                    arrow_context,
                );
                if *preview != next {
                    *preview = next;
                    true
                } else {
                    false
                }
            }
            _ => false,
        };
        if preview_changed {
            self.bump_scene_state_revision();
        }
    }

    fn update_default_serial_number_style(&mut self, style: &SerialNumberStyle) {
        let next_default = serial_number_with_style(&self.state.default_serial_number, style);
        if self.state.default_serial_number == next_default {
            return;
        }

        self.state.default_serial_number = next_default;
        if let Some(ElementCreationPreview::SerialNumber(preview)) =
            self.state.creation_preview.as_mut()
        {
            *preview = serial_number_with_style(preview, style);
            self.bump_scene_state_revision();
        }
    }

    pub fn set_shape_style_patch(
        &mut self,
        document: &DocumentModel,
        patch: ShapeStylePatch,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        validate_shape_style_patch(patch)?;
        if patch.is_empty() {
            return Ok(None);
        }

        let previous_stroke_cursor_width = self.active_stroke_cursor_width();
        match patch.kind {
            ShapeKind::Rectangle => self.update_default_shape_styles(
                document,
                patch.apply_to_rectangle_shape(self.state.default_rectangle_shape_style),
                self.state.default_arrow_style,
            ),
            ShapeKind::Arrow => self.update_default_shape_styles(
                document,
                self.state.default_rectangle_shape_style,
                patch.apply_to_arrow(self.state.default_arrow_style),
            ),
            ShapeKind::Line => {
                self.state.default_line_style = patch.apply_to_line(self.state.default_line_style);
            }
            ShapeKind::FreeDraw => {
                self.state.default_free_draw_style =
                    patch.apply_to_line(self.state.default_free_draw_style);
            }
            ShapeKind::RectangleHighlight => {
                self.state.default_rectangle_highlight_style =
                    patch.apply_to_highlight(self.state.default_rectangle_highlight_style);
            }
            ShapeKind::PenHighlight => {
                self.state.default_pen_highlight_style =
                    patch.apply_to_line(self.state.default_pen_highlight_style);
            }
            ShapeKind::Spotlight => {}
        }
        if self.active_stroke_cursor_width() != previous_stroke_cursor_width {
            self.bump_overlay_state_revision();
        }

        let bindables = self.bindable_elements(document, &[]);
        let arrow_context = self.arrow_engine_context(Modifiers::default());
        if !self.state.selection.is_empty() {
            let history_undo_snapshot = self.capture_document_sync_snapshot(document);
            let mut transaction = Transaction::new("update element style properties");
            let mut next_selection_elements = Vec::new();
            let mut next_selection_arrows = Vec::new();
            for id in self.state.selection.ids.iter().copied() {
                if let Ok(current_rect) = document.rectangle(id) {
                    let matches_kind = (patch.kind == ShapeKind::Rectangle
                        && !current_rect.is_highlight())
                        || (patch.kind == ShapeKind::RectangleHighlight
                            && current_rect.is_highlight());
                    let updated_rect = if matches_kind {
                        rectangle_with_style(
                            current_rect,
                            patch.apply_to_rectangle_shape(RectangleShapeStyle::from_rectangle(
                                current_rect,
                            )),
                        )
                    } else {
                        *current_rect
                    };
                    next_selection_elements.push(SelectionRectState {
                        id,
                        rect: updated_rect,
                    });
                    if matches_kind && *current_rect != updated_rect {
                        transaction.update_rectangle(id, updated_rect);
                    }
                    continue;
                }
                if let Ok(current_free_draw) = document.free_draw(id) {
                    let mut updated = current_free_draw.clone();
                    if patch.kind == ShapeKind::FreeDraw {
                        let style =
                            patch.apply_to_line(ShapeStyle::from_free_draw(current_free_draw));
                        updated.fill = style.fill;
                        updated.fill_style = style.fill_style;
                        updated.stroke = style.stroke;
                        updated.stroke_width = style.stroke_width;
                        updated.stroke_style = style.stroke_style;
                        updated.opacity = style.opacity;
                    }
                    if updated != *current_free_draw {
                        transaction.update_free_draw(id, updated.clone());
                    }
                    next_selection_elements.push(SelectionRectState {
                        id,
                        rect: RectangleData {
                            rectangle_kind:
                                snow_draw_engine_document::RectangleElementKind::Rectangle,
                            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
                            center: Point::new(
                                updated.x + updated.width / 2.0,
                                updated.y + updated.height / 2.0,
                            ),
                            width: updated.width,
                            height: updated.height,
                            rotation: updated.rotation,
                            fill: updated.fill,
                            fill_style: updated.fill_style,
                            stroke: updated.stroke,
                            stroke_width: updated.stroke_width,
                            stroke_style: updated.stroke_style,
                            corner_radii: CornerRadii::default(),
                            opacity: updated.opacity,
                        },
                    });
                    continue;
                }
                let Ok(current_arrow) = document.arrow(id) else {
                    if let Some(rect) = document.element_rect_proxy(id) {
                        next_selection_elements.push(SelectionRectState { id, rect });
                    }
                    continue;
                };
                let updated_arrow = if patch.kind == ShapeKind::Arrow
                    && !current_arrow.is_line()
                    && !current_arrow.is_pen_highlight()
                {
                    arrow_with_style(
                        id,
                        current_arrow,
                        patch.apply_to_arrow(ArrowStyle::from_arrow(current_arrow)),
                        &bindables,
                        arrow_context,
                    )
                } else if patch.kind == ShapeKind::Line && current_arrow.is_line() {
                    let style = patch.apply_to_line(ShapeStyle::from_line(current_arrow));
                    let mut updated = current_arrow.clone();
                    updated.fill = style.fill;
                    updated.fill_style = style.fill_style;
                    updated.stroke = style.stroke;
                    updated.stroke_width = style.stroke_width;
                    updated.stroke_style = style.stroke_style;
                    updated.opacity = style.opacity;
                    updated.start_arrowhead = None;
                    updated.end_arrowhead = None;
                    updated.arrow_type = ArrowType::Curve;
                    updated
                } else if patch.kind == ShapeKind::PenHighlight && current_arrow.is_pen_highlight()
                {
                    let style = patch.apply_to_line(ShapeStyle::from_line(current_arrow));
                    let mut updated = current_arrow.clone();
                    updated.stroke = style.stroke;
                    updated.stroke_width = style.stroke_width;
                    updated.stroke_style = StrokeStyle::Solid;
                    updated.arrow_type = ArrowType::Straight;
                    updated
                } else {
                    current_arrow.clone()
                };
                next_selection_arrows.push(SelectionArrowState {
                    id,
                    arrow: updated_arrow.clone(),
                });
                if matches!(
                    patch.kind,
                    ShapeKind::Arrow
                        | ShapeKind::Line
                        | ShapeKind::FreeDraw
                        | ShapeKind::PenHighlight
                ) && *current_arrow != updated_arrow
                {
                    transaction.update_arrow(id, updated_arrow);
                }
            }
            if transaction.is_empty() {
                return Ok(None);
            }
            self.state.selection.elements = next_selection_elements;
            self.state.selection.arrows = next_selection_arrows;
            self.state.selection.bounds = selection_bounds_from_selection(
                &self.state.selection.elements,
                &self.state.selection.arrows,
            );
            return Ok(Some(EditorCommand::ApplyTransaction(
                ApplyTransactionCommand::with_history_undo_snapshot(
                    transaction,
                    history_undo_snapshot,
                ),
            )));
        }

        Ok(None)
    }

    pub fn set_rectangle_shape_style(
        &mut self,
        document: &DocumentModel,
        style: RectangleShapeStyle,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        validate_rectangle_shape_style(style)?;
        self.update_default_shape_styles(document, style, self.state.default_arrow_style);

        if !self.state.selection.is_empty() {
            let history_undo_snapshot = self.capture_document_sync_snapshot(document);
            let mut transaction = Transaction::new("update rectangle style");
            let mut next_selection_elements = Vec::new();
            let mut next_selection_arrows = Vec::new();

            for id in self.state.selection.ids.iter().copied() {
                if let Ok(current_rect) = document.rectangle(id) {
                    let updated_rect = rectangle_with_style(current_rect, style);
                    next_selection_elements.push(SelectionRectState {
                        id,
                        rect: updated_rect,
                    });
                    if *current_rect != updated_rect {
                        transaction.update_rectangle(id, updated_rect);
                    }
                    continue;
                }

                if let Ok(current_arrow) = document.arrow(id) {
                    next_selection_arrows.push(SelectionArrowState {
                        id,
                        arrow: current_arrow.clone(),
                    });
                    continue;
                }

                if let Some(rect) = document.element_rect_proxy(id) {
                    next_selection_elements.push(SelectionRectState { id, rect });
                }
            }

            if transaction.is_empty() {
                return Ok(None);
            }

            self.state.selection.elements = next_selection_elements;
            self.state.selection.arrows = next_selection_arrows;
            self.state.selection.bounds = selection_bounds_from_selection(
                &self.state.selection.elements,
                &self.state.selection.arrows,
            );
            return Ok(Some(EditorCommand::ApplyTransaction(
                ApplyTransactionCommand::with_history_undo_snapshot(
                    transaction,
                    history_undo_snapshot,
                ),
            )));
        }

        Ok(None)
    }

    pub fn set_text_style(
        &mut self,
        document: &DocumentModel,
        style: TextStyle,
        layouts: &[TextLayoutOverride],
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        validate_text_style(&style)?;
        let next_default = text_with_style_attributes(&self.state.default_text, &style);

        let selected_text_ids = self
            .state
            .selection
            .ids
            .iter()
            .copied()
            .filter(|id| document.text(*id).is_ok())
            .collect::<Vec<_>>();
        if !selected_text_ids.is_empty() {
            let history_undo_snapshot = self.capture_document_sync_snapshot(document);
            let mut transaction = Transaction::new("update text style");
            let mut next_selection_elements = Vec::new();
            let mut next_selection_arrows = Vec::new();

            for id in self.state.selection.ids.iter().copied() {
                if let Ok(current_text) = document.text(id) {
                    let updated_text = text_with_style(id, current_text, &style, layouts)?;
                    validate_text(&updated_text)?;
                    next_selection_elements.push(SelectionRectState {
                        id,
                        rect: RectangleData {
                            rectangle_kind:
                                snow_draw_engine_document::RectangleElementKind::Rectangle,
                            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
                            center: updated_text.center,
                            width: updated_text.width,
                            height: updated_text.height,
                            rotation: updated_text.rotation,
                            fill: updated_text.fill,
                            fill_style: updated_text.fill_style,
                            stroke: updated_text.stroke,
                            stroke_width: updated_text.stroke_width,
                            stroke_style: StrokeStyle::Solid,
                            corner_radii: updated_text.corner_radii,
                            opacity: updated_text.opacity,
                        },
                    });
                    if *current_text != updated_text {
                        transaction.update_text(id, updated_text);
                    }
                    continue;
                }

                if let Ok(current_arrow) = document.arrow(id) {
                    next_selection_arrows.push(SelectionArrowState {
                        id,
                        arrow: current_arrow.clone(),
                    });
                    continue;
                }

                if let Some(rect) = document.element_rect_proxy(id) {
                    next_selection_elements.push(SelectionRectState { id, rect });
                }
            }

            self.state.default_text = next_default;
            if transaction.is_empty() {
                return Ok(None);
            }
            self.state.selection.elements = next_selection_elements;
            self.state.selection.arrows = next_selection_arrows;
            self.state.selection.bounds = selection_bounds_from_selection(
                &self.state.selection.elements,
                &self.state.selection.arrows,
            );
            return Ok(Some(EditorCommand::ApplyTransaction(
                ApplyTransactionCommand::with_history_undo_snapshot(
                    transaction,
                    history_undo_snapshot,
                ),
            )));
        }

        self.state.default_text = next_default;
        Ok(None)
    }

    pub fn set_serial_number_style(
        &mut self,
        document: &DocumentModel,
        style: SerialNumberStyle,
    ) -> Result<Option<EditorCommand>, ErrorCode> {
        validate_serial_number_style(&style)?;

        let selected_serial_ids = self
            .state
            .selection
            .ids
            .iter()
            .copied()
            .filter(|id| document.serial_number(*id).is_ok())
            .collect::<Vec<_>>();
        if !selected_serial_ids.is_empty() {
            let history_undo_snapshot = self.capture_document_sync_snapshot(document);
            let mut transaction = Transaction::new("update serial number style");
            let mut next_selection_elements = Vec::new();
            let mut next_selection_arrows = Vec::new();

            for id in self.state.selection.ids.iter().copied() {
                if let Ok(current_serial) = document.serial_number(id) {
                    let updated_serial = serial_number_with_style(current_serial, &style);
                    validate_serial_number(&updated_serial)?;
                    next_selection_elements.push(SelectionRectState {
                        id,
                        rect: serial_number_rect_proxy(&updated_serial),
                    });
                    if *current_serial != updated_serial {
                        transaction.update_serial_number(id, updated_serial);
                    }
                    continue;
                }

                if let Ok(current_arrow) = document.arrow(id) {
                    next_selection_arrows.push(SelectionArrowState {
                        id,
                        arrow: current_arrow.clone(),
                    });
                    continue;
                }

                if let Some(rect) = document.element_rect_proxy(id) {
                    next_selection_elements.push(SelectionRectState { id, rect });
                }
            }

            self.update_default_serial_number_style(&style);
            if transaction.is_empty() {
                return Ok(None);
            }
            self.state.selection.elements = next_selection_elements;
            self.state.selection.arrows = next_selection_arrows;
            self.state.selection.bounds = selection_bounds_from_selection(
                &self.state.selection.elements,
                &self.state.selection.arrows,
            );
            return Ok(Some(EditorCommand::ApplyTransaction(
                ApplyTransactionCommand::with_history_undo_snapshot(
                    transaction,
                    history_undo_snapshot,
                ),
            )));
        }

        self.update_default_serial_number_style(&style);
        Ok(None)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::defaults::editor_style_defaults;
    use snow_draw_engine_core::Point;
    use snow_draw_engine_document::{ElementMeta, resolve_serial_number_style_diameter};

    fn assert_close(actual: f64, expected: f64) {
        assert!(
            (actual - expected).abs() <= f64::EPSILON,
            "expected {actual} to equal {expected}"
        );
    }

    #[test]
    fn rectangle_style_preserves_and_updates_fill_style() {
        let rect = RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: Default::default(),
            width: 120.0,
            height: 80.0,
            rotation: 0.0,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Line,
            stroke: ColorRgba8::default(),
            stroke_width: 2.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        };

        let mut style = RectangleShapeStyle::from_rectangle(&rect);
        assert_eq!(style.fill_style, FillStyle::Line);
        assert_eq!(style.stroke_style, StrokeStyle::Solid);

        style.fill_style = FillStyle::CrossLine;
        style.stroke_style = StrokeStyle::Dashed;
        let updated = rectangle_with_style(&rect, style);
        assert_eq!(updated.fill_style, FillStyle::CrossLine);
        assert_eq!(updated.stroke_style, StrokeStyle::Dashed);
        assert_eq!(updated.center, rect.center);
        assert_eq!(updated.width, rect.width);
        assert_eq!(updated.height, rect.height);
    }

    #[test]
    fn rectangle_style_patch_preserves_unrequested_properties() {
        let rectangle_style = RectangleShapeStyle {
            shape: snow_draw_engine_document::HighlightShape::Rectangle,
            fill: ColorRgba8 {
                r: 10,
                g: 20,
                b: 30,
                a: 40,
            },
            fill_style: FillStyle::CrossLine,
            stroke: ColorRgba8 {
                r: 50,
                g: 60,
                b: 70,
                a: 80,
            },
            stroke_width: 2.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::splat(6.0),
        };
        let arrow_style = ArrowStyle {
            stroke: ColorRgba8 {
                r: 90,
                g: 100,
                b: 110,
                a: 120,
            },
            stroke_width: 3.0,
            start_arrowhead: None,
            end_arrowhead: Some(Arrowhead::Arrow),
            stroke_style: StrokeStyle::Dotted,
            arrow_type: ArrowType::Curve,
        };
        let mut updated_style =
            ShapeStyle::from_rectangle_shape_style(rectangle_style).with_arrow_style(arrow_style);
        updated_style.stroke_width = 9.0;
        updated_style.stroke = ColorRgba8 {
            r: 200,
            g: 201,
            b: 202,
            a: 203,
        };

        let patch = ShapeStylePatch {
            kind: ShapeKind::Rectangle,
            style: updated_style,
            properties: SHAPE_STYLE_PROPERTY_STROKE_WIDTH,
        };

        let updated_rectangle = patch.apply_to_rectangle_shape(rectangle_style);
        assert_eq!(updated_rectangle.stroke_width, 9.0);
        assert_eq!(updated_rectangle.fill, rectangle_style.fill);
        assert_eq!(updated_rectangle.fill_style, rectangle_style.fill_style);
        assert_eq!(updated_rectangle.stroke, rectangle_style.stroke);
        assert_eq!(updated_rectangle.stroke_style, rectangle_style.stroke_style);
        assert_eq!(updated_rectangle.corner_radii, rectangle_style.corner_radii);

        let mut stroke_style_only = ShapeStyle::from_rectangle_shape_style(rectangle_style);
        stroke_style_only.stroke_style = StrokeStyle::Dotted;
        let stroke_style_patch = ShapeStylePatch {
            kind: ShapeKind::Rectangle,
            style: stroke_style_only,
            properties: SHAPE_STYLE_PROPERTY_STROKE_STYLE,
        };
        let stroke_style_updated = stroke_style_patch.apply_to_rectangle_shape(rectangle_style);
        assert_eq!(stroke_style_updated.stroke_style, StrokeStyle::Dotted);

        let updated_arrow = patch.apply_to_arrow(arrow_style);
        assert_eq!(updated_arrow.stroke_width, 9.0);
        assert_eq!(updated_arrow.stroke, arrow_style.stroke);
        assert_eq!(updated_arrow.start_arrowhead, arrow_style.start_arrowhead);
        assert_eq!(updated_arrow.end_arrowhead, arrow_style.end_arrowhead);
        assert_eq!(updated_arrow.stroke_style, arrow_style.stroke_style);
        assert_eq!(updated_arrow.arrow_type, arrow_style.arrow_type);
    }

    #[test]
    fn highlight_tools_support_only_their_public_properties() {
        assert_eq!(
            ShapeKind::RectangleHighlight.supported_properties(),
            SHAPE_STYLE_PROPERTY_FILL
                | SHAPE_STYLE_PROPERTY_STROKE_WIDTH
                | SHAPE_STYLE_PROPERTY_STROKE
        );
        assert_eq!(
            ShapeKind::PenHighlight.supported_properties(),
            SHAPE_STYLE_PROPERTY_STROKE | SHAPE_STYLE_PROPERTY_STROKE_WIDTH
        );

        let mut style = editor_style_defaults().rectangle_highlight;
        style.fill_style = FillStyle::CrossLine;
        let error = validate_shape_style_patch(ShapeStylePatch {
            kind: ShapeKind::RectangleHighlight,
            style,
            properties: SHAPE_STYLE_PROPERTY_FILL_STYLE,
        });
        assert_eq!(error, Err(ErrorCode::InvalidArgument));
    }

    #[test]
    fn rectangle_highlight_patch_updates_its_public_properties() {
        let mut document = DocumentModel::new();
        let first_id = document.allocate_element_id();
        let second_id = document.allocate_element_id();
        let base = RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: Point::default(),
            width: 80.0,
            height: 40.0,
            rotation: 0.0,
            fill: ColorRgba8 {
                r: 245,
                g: 34,
                b: 45,
                a: 255,
            },
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        };
        let first = base.into_highlight(snow_draw_engine_document::HighlightShape::Rectangle);
        let mut second = base.into_highlight(snow_draw_engine_document::HighlightShape::Rectangle);
        second.center.x = 120.0;
        let mut insert = Transaction::new("insert highlights");
        insert.insert_rectangle(first_id, ElementMeta::default(), first);
        insert.insert_rectangle(second_id, ElementMeta::default(), second);
        document.apply_transaction(insert).unwrap();

        let mut editor = Editor::new(Default::default()).unwrap();
        editor.set_selection_state_with_document(
            Some(&document),
            vec![first_id, second_id],
            Some(first_id),
        );
        assert_eq!(editor.shape_style_mixed(&document), 0);

        let mut style = editor.shape_style(&document);
        style.fill = ColorRgba8 {
            r: 255,
            g: 220,
            b: 0,
            a: 255,
        };
        style.stroke = ColorRgba8 {
            r: 12,
            g: 34,
            b: 56,
            a: 255,
        };
        style.stroke_width = 6.0;
        let command = editor
            .set_shape_style_patch(
                &document,
                ShapeStylePatch {
                    kind: ShapeKind::RectangleHighlight,
                    style,
                    properties: ShapeKind::RectangleHighlight.supported_properties(),
                },
            )
            .unwrap()
            .unwrap();
        let EditorCommand::ApplyTransaction(command) = command else {
            panic!("expected a highlight style transaction")
        };
        document.apply_transaction(command.transaction).unwrap();

        for id in [first_id, second_id] {
            let highlight = document.rectangle(id).unwrap();
            assert_eq!(highlight.fill, style.fill);
            assert_eq!(
                highlight.highlight_shape,
                snow_draw_engine_document::HighlightShape::Rectangle
            );
            assert_eq!(highlight.stroke, style.stroke);
            assert_eq!(highlight.stroke_width, style.stroke_width);
            assert_eq!(highlight.opacity, 1.0);
            assert_eq!(highlight.fill_style, FillStyle::Solid);
            assert_eq!(highlight.stroke_style, StrokeStyle::Solid);
            assert_eq!(highlight.corner_radii, CornerRadii::default());
        }
    }

    #[test]
    fn selected_element_style_patch_updates_creation_defaults() {
        let mut document = DocumentModel::new();
        let rectangle_id = document.peek_next_element_id();
        let rectangle = RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: Default::default(),
            width: 120.0,
            height: 80.0,
            rotation: 0.0,
            fill: ColorRgba8 {
                r: 10,
                g: 20,
                b: 30,
                a: 255,
            },
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8 {
                r: 40,
                g: 50,
                b: 60,
                a: 255,
            },
            stroke_width: 2.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        };
        let mut insert = Transaction::new("insert selected rectangle");
        insert.insert_rectangle(rectangle_id, ElementMeta::default(), rectangle);
        document.apply_transaction(insert).unwrap();

        let mut editor = Editor::new(Default::default()).unwrap();
        editor.select_element(&document, rectangle_id).unwrap();
        let original_default_fill = editor.state.default_rectangle_shape_style.fill;
        let original_default_arrow_stroke_width = editor.state.default_arrow_style.stroke_width;

        let mut style = editor.shape_style(&document);
        style.stroke_width = 9.0;
        style.fill = ColorRgba8 {
            r: 200,
            g: 201,
            b: 202,
            a: 255,
        };
        let command = editor
            .set_shape_style_patch(
                &document,
                ShapeStylePatch {
                    kind: ShapeKind::Rectangle,
                    style,
                    properties: SHAPE_STYLE_PROPERTY_STROKE_WIDTH,
                },
            )
            .unwrap();

        assert!(command.is_some());
        assert_eq!(editor.state.default_rectangle_shape_style.stroke_width, 9.0);
        assert_eq!(
            editor.state.default_arrow_style.stroke_width, original_default_arrow_stroke_width,
            "rectangle patches must not mutate arrow creation defaults"
        );
        assert_eq!(
            editor.state.default_rectangle_shape_style.fill, original_default_fill,
            "properties not edited in the toolbar must keep their creation defaults"
        );

        editor.clear_selection();
        assert_eq!(editor.rectangle_shape_style(&document).stroke_width, 9.0);
        assert_eq!(
            editor.arrow_style(&document).stroke_width,
            original_default_arrow_stroke_width
        );
    }

    #[test]
    fn selected_arrow_style_patch_preserves_unrequested_properties() {
        let mut document = DocumentModel::new();
        let arrow = ArrowData::from_global_points(
            &[
                snow_draw_engine_core::Point::new(10.0, 10.0),
                snow_draw_engine_core::Point::new(90.0, 50.0),
            ],
            ColorRgba8 {
                r: 10,
                g: 20,
                b: 30,
                a: 255,
            },
            6.0,
            StrokeStyle::Dotted,
            ArrowType::Curve,
            Some(Arrowhead::Bar),
            Some(Arrowhead::Arrow),
        )
        .expect("two points create an arrow");
        let arrow_id = document.peek_next_element_id();
        let mut insert = Transaction::new("insert selected arrow");
        insert.insert_arrow(arrow_id, ElementMeta::default(), arrow.clone());
        document.apply_transaction(insert).unwrap();

        let mut editor = Editor::new(Default::default()).unwrap();
        editor.select_element(&document, arrow_id).unwrap();
        let original_rectangle_default = editor.state.default_rectangle_shape_style;

        let mut style = editor.shape_style(&document);
        style.end_arrowhead = Some(Arrowhead::Diamond);
        let command = editor
            .set_shape_style_patch(
                &document,
                ShapeStylePatch {
                    kind: ShapeKind::Arrow,
                    style,
                    properties: SHAPE_STYLE_PROPERTY_END_ARROWHEAD,
                },
            )
            .unwrap();

        assert!(command.is_some());
        let updated = &editor.state.selection.arrows[0].arrow;
        assert_eq!(updated.end_arrowhead, Some(Arrowhead::Diamond));
        assert_eq!(updated.stroke, arrow.stroke);
        assert_eq!(updated.stroke_width, arrow.stroke_width);
        assert_eq!(updated.start_arrowhead, arrow.start_arrowhead);
        assert_eq!(updated.stroke_style, arrow.stroke_style);
        assert_eq!(updated.arrow_type, arrow.arrow_type);
        assert_eq!(
            editor.state.default_rectangle_shape_style, original_rectangle_default,
            "arrow patches must not mutate rectangle creation defaults"
        );
    }

    #[test]
    fn line_style_patch_is_canonical_and_does_not_mutate_arrow() {
        let mut document = DocumentModel::new();
        let arrow = ArrowData::from_global_points(
            &[
                snow_draw_engine_core::Point::new(0.0, 0.0),
                snow_draw_engine_core::Point::new(40.0, 0.0),
            ],
            ColorRgba8 {
                r: 200,
                g: 0,
                b: 0,
                a: 255,
            },
            3.0,
            StrokeStyle::Solid,
            ArrowType::Straight,
            None,
            Some(Arrowhead::Arrow),
        )
        .unwrap();
        let line = ArrowData::from_global_points(
            &[
                snow_draw_engine_core::Point::new(0.0, 20.0),
                snow_draw_engine_core::Point::new(40.0, 20.0),
            ],
            ColorRgba8 {
                r: 30,
                g: 30,
                b: 30,
                a: 255,
            },
            2.0,
            StrokeStyle::Solid,
            ArrowType::Curve,
            None,
            None,
        )
        .unwrap()
        .into_line(ColorRgba8::default(), FillStyle::Solid);
        let arrow_id = document.allocate_element_id();
        let line_id = document.allocate_element_id();
        let mut insert = Transaction::new("insert arrow and line");
        insert.insert_arrow(arrow_id, ElementMeta::default(), arrow.clone());
        insert.insert_arrow(line_id, ElementMeta::default(), line);
        document.apply_transaction(insert).unwrap();

        let mut editor = Editor::new(Default::default()).unwrap();
        editor.set_selection_state_with_document(
            Some(&document),
            vec![arrow_id, line_id],
            Some(line_id),
        );
        let original_arrow_default = editor.state.default_arrow_style;
        let mut style = editor.state.default_line_style;
        style.stroke_width = 9.0;
        style.fill = ColorRgba8 {
            r: 10,
            g: 20,
            b: 30,
            a: 128,
        };
        style.opacity = 0.4;
        style.arrow_type = ArrowType::Elbow;
        style.start_arrowhead = Some(Arrowhead::Diamond);
        style.end_arrowhead = Some(Arrowhead::Arrow);
        let command = editor
            .set_shape_style_patch(
                &document,
                ShapeStylePatch {
                    kind: ShapeKind::Line,
                    style,
                    properties: SHAPE_STYLE_PROPERTY_STROKE_WIDTH
                        | SHAPE_STYLE_PROPERTY_FILL
                        | SHAPE_STYLE_PROPERTY_OPACITY,
                },
            )
            .unwrap();

        assert!(command.is_some());
        let selected_arrow = editor
            .state
            .selection
            .arrows
            .iter()
            .find(|item| item.id == arrow_id)
            .unwrap();
        let selected_line = editor
            .state
            .selection
            .arrows
            .iter()
            .find(|item| item.id == line_id)
            .unwrap();
        assert_eq!(selected_arrow.arrow, arrow);
        assert_eq!(selected_line.arrow.stroke_width, 9.0);
        assert_eq!(selected_line.arrow.fill, style.fill);
        assert_eq!(selected_line.arrow.opacity, 0.4);
        assert_eq!(selected_line.arrow.arrow_type, ArrowType::Curve);
        assert_eq!(selected_line.arrow.start_arrowhead, None);
        assert_eq!(selected_line.arrow.end_arrowhead, None);
        assert_eq!(editor.state.default_arrow_style, original_arrow_default);
    }

    #[test]
    fn serial_number_style_font_size_decrease_shrinks_diameter() {
        let mut serial = SerialNumberData::default();
        serial.font_size = 42.0;
        serial.diameter = resolve_serial_number_style_diameter(serial.number, serial.font_size);

        let mut style = SerialNumberStyle::from_serial_number(&serial);
        style.font_size = 16.0;

        let updated = serial_number_with_style(&serial, &style);

        assert!(updated.diameter < serial.diameter);
        assert_close(
            updated.diameter,
            resolve_serial_number_style_diameter(updated.number, updated.font_size),
        );
    }

    #[test]
    fn serial_number_non_size_style_preserves_diameter() {
        let serial = SerialNumberData {
            font_size: 42.0,
            diameter: 123.0,
            ..SerialNumberData::default()
        };

        let mut style = SerialNumberStyle::from_serial_number(&serial);
        style.color = ColorRgba8 {
            r: 0,
            g: 128,
            b: 255,
            a: 255,
        };

        let updated = serial_number_with_style(&serial, &style);

        assert_eq!(updated.diameter, serial.diameter);
    }

    #[test]
    fn serial_number_style_rejects_font_size_below_minimum() {
        let mut style = SerialNumberStyle::from_serial_number(&SerialNumberData::default());
        style.font_size = MIN_SERIAL_NUMBER_FONT_SIZE - 0.1;

        assert_eq!(
            validate_serial_number_style(&style),
            Err(ErrorCode::InvalidArgument)
        );
    }

    #[test]
    fn filter_style_updates_only_filters_in_a_heterogeneous_selection() {
        let mut document = DocumentModel::new();
        let filter_id = document.allocate_element_id();
        let rectangle_id = document.allocate_element_id();
        let filter = FilterData::default();
        let second_filter_id = document.allocate_element_id();
        let second_filter = FilterData {
            opacity: 0.25,
            ..filter
        };
        let rectangle = RectangleData {
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: Point::default(),
            width: 10.0,
            height: 10.0,
            rotation: 0.0,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        };
        let mut insert = Transaction::new("insert");
        insert.insert_filter(filter_id, ElementMeta::default(), filter);
        insert.insert_rectangle(rectangle_id, ElementMeta::default(), rectangle);
        insert.insert_filter(second_filter_id, ElementMeta::default(), second_filter);
        document.apply_transaction(insert).unwrap();
        let mut editor = Editor::new(snow_draw_engine_core::EngineConfig::default()).unwrap();
        editor.set_selection_state(
            vec![rectangle_id, filter_id, second_filter_id],
            Some(rectangle_id),
        );

        assert_eq!(
            editor.style_toolbar_source(&document),
            StyleToolbarSource::SelectedFilter
        );
        assert_eq!(
            editor.filter_style_mixed(&document),
            FILTER_STYLE_PROPERTY_OPACITY
        );
        let command = editor
            .set_filter_style(
                &document,
                FilterStyle {
                    filter_type: snow_draw_engine_document::CanvasFilterType::Inversion,
                    strength: 2.0,
                    opacity: 0.75,
                    stroke_width: 2.0,
                },
                FILTER_STYLE_PROPERTY_TYPE
                    | FILTER_STYLE_PROPERTY_STRENGTH
                    | FILTER_STYLE_PROPERTY_OPACITY,
            )
            .unwrap()
            .unwrap();
        let EditorCommand::ApplyTransaction(command) = command else {
            panic!()
        };
        assert_eq!(command.transaction.operations().len(), 2);
        document.apply_transaction(command.transaction).unwrap();
        assert_eq!(
            document.filter(filter_id).unwrap().filter_type,
            snow_draw_engine_document::CanvasFilterType::Inversion
        );
        assert_eq!(document.filter(filter_id).unwrap().strength, 1.0);
        assert_eq!(document.filter(filter_id).unwrap().opacity, 0.75);
        assert_eq!(document.filter(second_filter_id).unwrap().opacity, 0.75);
        assert_eq!(*document.rectangle(rectangle_id).unwrap(), rectangle);
    }
}
