mod engine;
mod history;
mod persistence;
#[cfg(test)]
mod persistence_tests;
mod session;

pub use engine::{
    Engine, EngineConfig as RuntimeConfig, InputUpdate, MutationResult, StyleDefaults,
    TextElementInfo, ViewportConfig, ViewportId,
};
pub use snow_draw_engine_core::arrow::{
    ArrowPathCommand, ArrowType, Arrowhead, StrokeRenderingStyle, StrokeStyle,
};
pub use snow_draw_engine_core::*;
pub use snow_draw_engine_display::*;
pub use snow_draw_engine_document::{
    AngleAnnotation, AngleType, AngleUnit, CanvasFilterType, DistanceAnnotation, DistanceUnit,
    ElementId, FillStyle, HighlightShape, InkBox, SerialNumberType, SpotlightConfig, TextData,
    TextHorizontalAlign, TextLayoutSize, TextVerticalAlign, WatermarkConfig,
    WatermarkTemplateApplicationTime, normalize_font_family,
};
pub use snow_draw_engine_editor::{
    ANGLE_STYLE_MIXED_DECIMAL_PLACES, ANGLE_STYLE_MIXED_STROKE, ANGLE_STYLE_MIXED_STROKE_WIDTH,
    ANGLE_STYLE_MIXED_TYPE, ANGLE_STYLE_MIXED_UNIT, ANGLE_STYLE_PROPERTY_ALL,
    ANGLE_STYLE_PROPERTY_DECIMAL_PLACES, ANGLE_STYLE_PROPERTY_STROKE,
    ANGLE_STYLE_PROPERTY_STROKE_WIDTH, ANGLE_STYLE_PROPERTY_TYPE, ANGLE_STYLE_PROPERTY_UNIT,
    ActiveTextDraftPresentation, ActiveTextDraftTarget, ActiveTool, AngleStyle,
    ApplyTransactionCommand, ArrowStyle, BRACKET_STYLE_PROPERTY_ALL, BRACKET_STYLE_PROPERTY_STROKE,
    BRACKET_STYLE_PROPERTY_STROKE_WIDTH, BRUSH_ERASER_STYLE_PROPERTY_STROKE_WIDTH, BracketStyle,
    BrushEraserStyle, DISTANCE_STYLE_MIXED_DECIMAL_PLACES, DISTANCE_STYLE_MIXED_ENDPOINT_RATIO,
    DISTANCE_STYLE_MIXED_ENDPOINT_STYLE, DISTANCE_STYLE_MIXED_FACTOR, DISTANCE_STYLE_MIXED_STROKE,
    DISTANCE_STYLE_MIXED_STROKE_WIDTH, DISTANCE_STYLE_MIXED_UNIT, DISTANCE_STYLE_PROPERTY_ALL,
    DISTANCE_STYLE_PROPERTY_DECIMAL_PLACES, DISTANCE_STYLE_PROPERTY_ENDPOINT_RATIO,
    DISTANCE_STYLE_PROPERTY_ENDPOINT_STYLE, DISTANCE_STYLE_PROPERTY_FACTOR,
    DISTANCE_STYLE_PROPERTY_STROKE, DISTANCE_STYLE_PROPERTY_STROKE_WIDTH,
    DISTANCE_STYLE_PROPERTY_UNIT, DistanceStyle, DocumentSyncSnapshot, EditorCommand,
    EditorSession, EditorSessionSnapshot, EditorStyleDefaults, EditorUpdate, EditorViewState,
    EditorViewportState, FILTER_STYLE_PROPERTY_ALL, FilterStyle, HistoryState,
    MAGNIFIER_STYLE_PROPERTY_ALL, MAGNIFIER_STYLE_PROPERTY_ARROW_RATIO,
    MAGNIFIER_STYLE_PROPERTY_CONNECTION_TYPE, MAGNIFIER_STYLE_PROPERTY_CORNER_RADII,
    MAGNIFIER_STYLE_PROPERTY_SOURCE_OUTLINE, MagnifierStyle, RectangleShapeStyle, SelectionBounds,
    SelectionRectState, SerialNumberStyle, SerialNumberToolbarState, ShapeKind, ShapeStyle,
    ShapeStylePatch, SnapGuideTargets, StyleToolbarSource, StyleToolbarState,
    TEXT_STYLE_ALL_PROPERTIES, TextCommitTarget, TextDraftCommit, TextLayoutOverride, TextStyle,
    validate_angle_style, validate_magnifier_style,
};
pub use snow_draw_engine_interaction::*;

pub type Runtime = Engine;

pub use snow_draw_engine_editor::{
    DRAW_TEMPLATE_SCHEMA_VERSION, SpeechBubbleTextConstraints, SpeechBubbleTextLayoutRequest,
    SpeechBubbleTextMeasurement, speech_bubble_text_constraints,
};
