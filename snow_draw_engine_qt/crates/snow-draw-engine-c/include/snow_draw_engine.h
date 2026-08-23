#pragma once

#include <stdint.h>
#include <stddef.h>

#define SNOW_ARROW_PATH_UTF8_CAPACITY 4096
#define SNOW_ARROW_PATH_COMMAND_CAPACITY 128
#define SNOW_ARROWHEAD_PRIMITIVE_CAPACITY 16
#define SNOW_ARROWHEAD_PRIMITIVE_POINT_CAPACITY 8
#define SNOW_FONT_FAMILY_UTF8_CAPACITY 128

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnowRuntimeImpl SnowRuntimeImpl;
typedef SnowRuntimeImpl* SnowRuntime;
typedef SnowRuntimeImpl SnowEngineImpl;
typedef SnowEngineImpl* SnowEngine;

typedef struct SnowViewportImpl SnowViewportImpl;
typedef SnowViewportImpl* SnowViewport;
typedef struct SnowPatchHandleImpl SnowPatchHandleImpl;
typedef SnowPatchHandleImpl* SnowPatchHandle;
typedef struct SnowChangedViewportListImpl SnowChangedViewportListImpl;
typedef SnowChangedViewportListImpl* SnowChangedViewportList;
typedef struct SnowRuntimeConfig SnowRuntimeConfig;

typedef enum SnowError {
    SNOW_OK = 0,
    SNOW_ERROR_INVALID_ARGUMENT = 1,
    SNOW_ERROR_NOT_FOUND = 2,
    SNOW_ERROR_INVALID_STATE = 3,
    SNOW_ERROR_BUFFER_TOO_SMALL = 4,
    SNOW_ERROR_STALE_REVISION = 5,
    SNOW_ERROR_UNSUPPORTED = 6,
    SNOW_ERROR_INTERNAL = 7
} SnowError;

SnowError snow_runtime_serialize_document_session(SnowRuntime runtime, uint8_t* buffer,
                                                  size_t buffer_capacity, size_t* out_size);
SnowError snow_runtime_create_from_document_session(const uint8_t* bytes, size_t size,
                                                    SnowRuntime* out_runtime);
SnowError snow_runtime_create_from_document_session_with_config(const uint8_t* bytes, size_t size,
                                                                const SnowRuntimeConfig* config,
                                                                SnowRuntime* out_runtime);
SnowError snow_runtime_serialize_document_history(SnowRuntime runtime, uint8_t* buffer,
                                                  size_t buffer_capacity, size_t* out_size);
SnowError snow_runtime_create_from_document_history(const uint8_t* bytes, size_t size,
                                                    SnowRuntime* out_runtime);
SnowError snow_runtime_create_from_document_history_with_config(const uint8_t* bytes, size_t size,
                                                                const SnowRuntimeConfig* config,
                                                                SnowRuntime* out_runtime);

typedef enum SnowZoomFocus {
    SNOW_ZOOM_FOCUS_POINTER = 0,
    SNOW_ZOOM_FOCUS_CENTER = 1
} SnowZoomFocus;

typedef enum SnowActiveTool {
    SNOW_ACTIVE_TOOL_SELECT = 0,
    SNOW_ACTIVE_TOOL_SHAPE = 1,
    SNOW_ACTIVE_TOOL_ARROW = 2,
    SNOW_ACTIVE_TOOL_TEXT = 3,
    SNOW_ACTIVE_TOOL_SERIAL_NUMBER = 4,
    SNOW_ACTIVE_TOOL_LINE = 5,
    SNOW_ACTIVE_TOOL_FREE_DRAW = 6,
    SNOW_ACTIVE_TOOL_RECTANGLE_HIGHLIGHT = 7,
    SNOW_ACTIVE_TOOL_ERASER = 8,
    SNOW_ACTIVE_TOOL_RECTANGLE_FILTER = 9,
    SNOW_ACTIVE_TOOL_WATERMARK = 10,
    SNOW_ACTIVE_TOOL_PEN_HIGHLIGHT = 11,
    SNOW_ACTIVE_TOOL_PEN_FILTER = 12,
    SNOW_ACTIVE_TOOL_SPOTLIGHT = 13
} SnowActiveTool;
#define SNOW_ACTIVE_TOOL_FILTER SNOW_ACTIVE_TOOL_RECTANGLE_FILTER
#define SNOW_ACTIVE_TOOL_HIGHLIGHT SNOW_ACTIVE_TOOL_RECTANGLE_HIGHLIGHT

typedef enum SnowStyleToolbarSource {
    SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_RECTANGLE = 0,
    SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_RECTANGLE = 1,
    SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_ARROW = 2,
    SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_ARROW = 3,
    SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_TEXT = 4,
    SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_TEXT = 5,
    SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_SERIAL_NUMBER = 6,
    SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_SERIAL_NUMBER = 7,
    SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_LINE = 8,
    SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_LINE = 9,
    SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_FREE_DRAW = 10,
    SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_FREE_DRAW = 11,
    SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_RECTANGLE_HIGHLIGHT = 12,
    SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_RECTANGLE_HIGHLIGHT = 13,
    SNOW_STYLE_TOOLBAR_SOURCE_ERASER = 14,
    SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_RECTANGLE_FILTER = 15,
    SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_RECTANGLE_FILTER = 16,
    SNOW_STYLE_TOOLBAR_SOURCE_WATERMARK = 17,
    SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_PEN_HIGHLIGHT = 18,
    SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_PEN_HIGHLIGHT = 19,
    SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_PEN_FILTER = 20,
    SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_PEN_FILTER = 21,
    SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_SPOTLIGHT = 22,
    SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_SPOTLIGHT = 23
} SnowStyleToolbarSource;
#define SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_FILTER SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_RECTANGLE_FILTER
#define SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_FILTER                                                  \
    SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_RECTANGLE_FILTER
#define SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_HIGHLIGHT                                                \
    SNOW_STYLE_TOOLBAR_SOURCE_DEFAULT_RECTANGLE_HIGHLIGHT
#define SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_HIGHLIGHT                                               \
    SNOW_STYLE_TOOLBAR_SOURCE_SELECTED_RECTANGLE_HIGHLIGHT

typedef enum SnowFilterType {
    SNOW_FILTER_TYPE_MOSAIC = 0,
    SNOW_FILTER_TYPE_GAUSSIAN_BLUR = 1,
    SNOW_FILTER_TYPE_GRAYSCALE = 2,
    SNOW_FILTER_TYPE_INVERSION = 3
} SnowFilterType;

typedef struct SnowFilterStyle {
    SnowFilterType filter_type;
    double strength;
    double opacity;
    double stroke_width;
} SnowFilterStyle;

#define SNOW_TEXT_STYLE_MIXED_COLOR (1u << 0)
#define SNOW_TEXT_STYLE_MIXED_FONT_SIZE (1u << 1)
#define SNOW_TEXT_STYLE_MIXED_FONT_FAMILY (1u << 2)
#define SNOW_TEXT_STYLE_MIXED_FILL (1u << 3)
#define SNOW_TEXT_STYLE_MIXED_FILL_STYLE (1u << 4)
#define SNOW_TEXT_STYLE_MIXED_STROKE (1u << 5)
#define SNOW_TEXT_STYLE_MIXED_STROKE_WIDTH (1u << 6)
#define SNOW_TEXT_STYLE_MIXED_CORNER_RADII (1u << 7)
#define SNOW_TEXT_STYLE_MIXED_HORIZONTAL_ALIGN (1u << 8)
#define SNOW_TEXT_STYLE_MIXED_VERTICAL_ALIGN (1u << 9)
#define SNOW_TEXT_STYLE_MIXED_OPACITY (1u << 10)

#define SNOW_SERIAL_NUMBER_STYLE_MIXED_NUMBER (1u << 0)
#define SNOW_SERIAL_NUMBER_STYLE_MIXED_COLOR (1u << 1)
#define SNOW_SERIAL_NUMBER_STYLE_MIXED_FILL (1u << 2)
#define SNOW_SERIAL_NUMBER_STYLE_MIXED_FILL_STYLE (1u << 3)
#define SNOW_SERIAL_NUMBER_STYLE_MIXED_FONT_SIZE (1u << 4)
#define SNOW_SERIAL_NUMBER_STYLE_MIXED_FONT_FAMILY (1u << 5)
#define SNOW_SERIAL_NUMBER_STYLE_MIXED_STROKE_WIDTH (1u << 6)
#define SNOW_SERIAL_NUMBER_STYLE_MIXED_STROKE_STYLE (1u << 7)
#define SNOW_SERIAL_NUMBER_STYLE_MIXED_OPACITY (1u << 8)

typedef enum SnowPointerEventType {
    SNOW_POINTER_EVENT_DOWN = 0,
    SNOW_POINTER_EVENT_MOVE = 1,
    SNOW_POINTER_EVENT_UP = 2,
    SNOW_POINTER_EVENT_CANCEL = 3,
    SNOW_POINTER_EVENT_ENTER = 4,
    SNOW_POINTER_EVENT_LEAVE = 5,
    SNOW_POINTER_EVENT_DOUBLE_CLICK = 6
} SnowPointerEventType;

typedef enum SnowPointerDevice {
    SNOW_POINTER_DEVICE_MOUSE = 0,
    SNOW_POINTER_DEVICE_TOUCH = 1,
    SNOW_POINTER_DEVICE_PEN = 2,
    SNOW_POINTER_DEVICE_UNKNOWN = 3
} SnowPointerDevice;

typedef enum SnowPointerButton {
    SNOW_POINTER_BUTTON_NONE = 0,
    SNOW_POINTER_BUTTON_PRIMARY = 1,
    SNOW_POINTER_BUTTON_SECONDARY = 2,
    SNOW_POINTER_BUTTON_MIDDLE = 3
} SnowPointerButton;

typedef enum SnowInputEventKind {
    SNOW_INPUT_EVENT_POINTER = 0,
    SNOW_INPUT_EVENT_WHEEL = 1,
    SNOW_INPUT_EVENT_KEY = 2,
    SNOW_INPUT_EVENT_FOCUS_LOST = 3
} SnowInputEventKind;

typedef enum SnowWheelDeltaKind {
    SNOW_WHEEL_DELTA_PIXEL = 0,
    SNOW_WHEEL_DELTA_ANGLE = 1
} SnowWheelDeltaKind;

typedef enum SnowKeyEventType { SNOW_KEY_EVENT_DOWN = 0, SNOW_KEY_EVENT_UP = 1 } SnowKeyEventType;

typedef enum SnowKeyCode {
    SNOW_KEY_CODE_UNKNOWN = 0,
    SNOW_KEY_CODE_SPACE = 1,
    SNOW_KEY_CODE_ESCAPE = 2,
    SNOW_KEY_CODE_ARROW_UP = 3,
    SNOW_KEY_CODE_ARROW_DOWN = 4,
    SNOW_KEY_CODE_ARROW_LEFT = 5,
    SNOW_KEY_CODE_ARROW_RIGHT = 6,
    SNOW_KEY_CODE_CHARACTER = 7,
    SNOW_KEY_CODE_BACKSPACE = 8,
    SNOW_KEY_CODE_DELETE = 9
} SnowKeyCode;

typedef enum SnowPointerCaptureCommandKind {
    SNOW_POINTER_CAPTURE_NO_CHANGE = 0,
    SNOW_POINTER_CAPTURE_CAPTURE = 1,
    SNOW_POINTER_CAPTURE_RELEASE = 2
} SnowPointerCaptureCommandKind;

typedef enum SnowCursorCommandKind {
    SNOW_CURSOR_NO_CHANGE = 0,
    SNOW_CURSOR_SET = 1
} SnowCursorCommandKind;

typedef enum SnowCursorStyle {
    SNOW_CURSOR_STYLE_DEFAULT = 0,
    SNOW_CURSOR_STYLE_CROSSHAIR = 1,
    SNOW_CURSOR_STYLE_GRAB = 2,
    SNOW_CURSOR_STYLE_GRABBING = 3,
    SNOW_CURSOR_STYLE_MOVE = 4,
    SNOW_CURSOR_STYLE_RESIZE_HORIZONTAL = 5,
    SNOW_CURSOR_STYLE_RESIZE_VERTICAL = 6,
    SNOW_CURSOR_STYLE_RESIZE_NWSE = 7,
    SNOW_CURSOR_STYLE_RESIZE_NESW = 8,
    SNOW_CURSOR_STYLE_NOT_ALLOWED = 9,
    SNOW_CURSOR_STYLE_TEXT = 10,
    SNOW_CURSOR_STYLE_CORNER_RADIUS = 11,
    SNOW_CURSOR_STYLE_HIDDEN = 12
} SnowCursorStyle;

typedef enum SnowSceneDisplayItemKind {
    SNOW_SCENE_DISPLAY_ITEM_UNKNOWN = 0,
    SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT = 1,
    SNOW_SCENE_DISPLAY_ITEM_STROKE = 2,
    SNOW_SCENE_DISPLAY_ITEM_TEXT = 3,
    SNOW_SCENE_DISPLAY_ITEM_IMAGE = 4,
    SNOW_SCENE_DISPLAY_ITEM_ARROW = 5,
    SNOW_SCENE_DISPLAY_ITEM_SERIAL_NUMBER = 6,
    SNOW_SCENE_DISPLAY_ITEM_SERIAL_NUMBER_CONNECTOR = 7,
    SNOW_SCENE_DISPLAY_ITEM_FILTER = 8
} SnowSceneDisplayItemKind;

typedef enum SnowBlendMode {
    SNOW_BLEND_MODE_NORMAL = 0,
    SNOW_BLEND_MODE_MULTIPLY = 1
} SnowBlendMode;

typedef enum SnowOverlayDisplayItemKind {
    SNOW_OVERLAY_DISPLAY_ITEM_DRAW_RECT = 1,
    SNOW_OVERLAY_DISPLAY_ITEM_SNAP_GUIDE = 2,
    SNOW_OVERLAY_DISPLAY_ITEM_FOCUS_CONNECTION = 3,
    SNOW_OVERLAY_DISPLAY_ITEM_PEN_FILTER_CONTOUR = 4
} SnowOverlayDisplayItemKind;

typedef enum SnowArrowhead {
    SNOW_ARROWHEAD_NONE = 0,
    SNOW_ARROWHEAD_ARROW = 1,
    SNOW_ARROWHEAD_BAR = 2,
    SNOW_ARROWHEAD_DOT = 3,
    SNOW_ARROWHEAD_CIRCLE = 4,
    SNOW_ARROWHEAD_CIRCLE_OUTLINE = 5,
    SNOW_ARROWHEAD_TRIANGLE = 6,
    SNOW_ARROWHEAD_TRIANGLE_OUTLINE = 7,
    SNOW_ARROWHEAD_DIAMOND = 8,
    SNOW_ARROWHEAD_DIAMOND_OUTLINE = 9,
    SNOW_ARROWHEAD_CROWFOOT_ONE = 10,
    SNOW_ARROWHEAD_CROWFOOT_MANY = 11,
    SNOW_ARROWHEAD_CROWFOOT_ONE_OR_MANY = 12,
    SNOW_ARROWHEAD_SQUARE = 13,
    SNOW_ARROWHEAD_INVERTED_TRIANGLE = 14
} SnowArrowhead;

typedef enum SnowArrowStrokeStyle {
    SNOW_ARROW_STROKE_STYLE_SOLID = 0,
    SNOW_ARROW_STROKE_STYLE_DASHED = 1,
    SNOW_ARROW_STROKE_STYLE_DOTTED = 2
} SnowArrowStrokeStyle;

typedef enum SnowArrowType {
    SNOW_ARROW_TYPE_STRAIGHT = 0,
    SNOW_ARROW_TYPE_CURVE = 1,
    SNOW_ARROW_TYPE_ELBOW = 2
} SnowArrowType;

typedef enum SnowArrowheadPrimitiveKind {
    SNOW_ARROWHEAD_PRIMITIVE_NONE = 0,
    SNOW_ARROWHEAD_PRIMITIVE_LINE = 1,
    SNOW_ARROWHEAD_PRIMITIVE_POLYGON = 2,
    SNOW_ARROWHEAD_PRIMITIVE_CIRCLE = 3
} SnowArrowheadPrimitiveKind;

typedef enum SnowArrowheadFillMode {
    SNOW_ARROWHEAD_FILL_STROKE = 0,
    SNOW_ARROWHEAD_FILL_BACKGROUND = 1
} SnowArrowheadFillMode;

typedef enum SnowArrowheadDashMode {
    SNOW_ARROWHEAD_DASH_INHERIT = 0,
    SNOW_ARROWHEAD_DASH_SOLID = 1,
    SNOW_ARROWHEAD_DASH_DOTTED_CAP = 2
} SnowArrowheadDashMode;

typedef enum SnowArrowPathCommandKind {
    SNOW_ARROW_PATH_COMMAND_NONE = 0,
    SNOW_ARROW_PATH_COMMAND_MOVE_TO = 1,
    SNOW_ARROW_PATH_COMMAND_LINE_TO = 2,
    SNOW_ARROW_PATH_COMMAND_QUAD_TO = 3,
    SNOW_ARROW_PATH_COMMAND_CUBIC_TO = 4
} SnowArrowPathCommandKind;

typedef enum SnowTextHorizontalAlign {
    SNOW_TEXT_HORIZONTAL_ALIGN_LEFT = 0,
    SNOW_TEXT_HORIZONTAL_ALIGN_CENTER = 1,
    SNOW_TEXT_HORIZONTAL_ALIGN_RIGHT = 2
} SnowTextHorizontalAlign;

typedef enum SnowTextVerticalAlign {
    SNOW_TEXT_VERTICAL_ALIGN_TOP = 0,
    SNOW_TEXT_VERTICAL_ALIGN_CENTER = 1,
    SNOW_TEXT_VERTICAL_ALIGN_BOTTOM = 2
} SnowTextVerticalAlign;

typedef enum SnowFillStyle {
    SNOW_FILL_STYLE_LINE = 0,
    SNOW_FILL_STYLE_CROSS_LINE = 1,
    SNOW_FILL_STYLE_SOLID = 2
} SnowFillStyle;

typedef enum SnowStrokeStyle {
    SNOW_STROKE_STYLE_SOLID = 0,
    SNOW_STROKE_STYLE_DASHED = 1,
    SNOW_STROKE_STYLE_DOTTED = 2
} SnowStrokeStyle;

typedef enum SnowDisplayRectShape {
    SNOW_DISPLAY_RECT_SHAPE_RECTANGLE = 0,
    SNOW_DISPLAY_RECT_SHAPE_ELLIPSE = 1,
    SNOW_DISPLAY_RECT_SHAPE_DIAMOND = 2
} SnowDisplayRectShape;

typedef enum SnowOverlayRectKind {
    SNOW_OVERLAY_RECT_UNSPECIFIED = 0,
    SNOW_OVERLAY_RECT_SELECTION_MARQUEE = 1,
    SNOW_OVERLAY_RECT_SELECTION_CANDIDATE_FRAME = 2,
    SNOW_OVERLAY_RECT_SELECTION_FRAME = 3,
    SNOW_OVERLAY_RECT_SELECTION_MULTI_FRAME = 4,
    SNOW_OVERLAY_RECT_SELECTION_RESIZE_HANDLE = 5,
    SNOW_OVERLAY_RECT_SELECTION_ROTATION_HANDLE = 6,
    SNOW_OVERLAY_RECT_SELECTION_CORNER_RADIUS_HANDLE = 7,
    SNOW_OVERLAY_RECT_ARROW_ENDPOINT_HANDLE = 8,
    SNOW_OVERLAY_RECT_ARROW_FOCUS_HANDLE = 9,
    SNOW_OVERLAY_RECT_ARROW_SEGMENT_HANDLE = 10,
    SNOW_OVERLAY_RECT_TEXT_ACTUAL_FRAME = 11,
    SNOW_OVERLAY_RECT_TEXT_HOVER_UNDERLINE = 12,
    SNOW_OVERLAY_RECT_ERASER_CURSOR = 13
} SnowOverlayRectKind;

typedef enum SnowSnapGuideKind {
    SNOW_SNAP_GUIDE_POINT = 0,
    SNOW_SNAP_GUIDE_GAP = 1
} SnowSnapGuideKind;

typedef enum SnowSnapGuideAxis {
    SNOW_SNAP_GUIDE_HORIZONTAL = 0,
    SNOW_SNAP_GUIDE_VERTICAL = 1
} SnowSnapGuideAxis;

typedef struct SnowColorRgba8 {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} SnowColorRgba8;

typedef struct SnowCornerRadii {
    double top_left;
    double top_right;
    double bottom_right;
    double bottom_left;
} SnowCornerRadii;

typedef struct SnowWatermarkConfig {
    SnowColorRgba8 color;
    uint32_t text_utf8_len;
    char text_utf8[256];
    double font_size;
    uint32_t font_family_utf8_len;
    char font_family_utf8[128];
    double angle;
    double gap;
    double opacity;
} SnowWatermarkConfig;

typedef struct SnowSpotlightConfig {
    SnowColorRgba8 color;
    double opacity;
} SnowSpotlightConfig;

typedef enum SnowHighlightShape {
    SNOW_HIGHLIGHT_SHAPE_RECTANGLE = 0,
    SNOW_HIGHLIGHT_SHAPE_ELLIPSE = 1
} SnowHighlightShape;

typedef enum SnowRectangleShape {
    SNOW_RECTANGLE_SHAPE_RECTANGLE = 0,
    SNOW_RECTANGLE_SHAPE_ELLIPSE = 1,
    SNOW_RECTANGLE_SHAPE_DIAMOND = 2
} SnowRectangleShape;

typedef struct SnowShapeStyle {
    SnowColorRgba8 fill;
    SnowColorRgba8 stroke;
    double stroke_width;
    SnowCornerRadii corner_radii;
    SnowArrowhead start_arrowhead;
    SnowArrowhead end_arrowhead;
    SnowArrowStrokeStyle stroke_style;
    SnowArrowType arrow_type;
    SnowFillStyle fill_style;
    double opacity;
    SnowHighlightShape highlight_shape;
    SnowRectangleShape shape;
} SnowShapeStyle;

typedef enum SnowShapeKind {
    SNOW_SHAPE_KIND_RECTANGLE = 0,
    SNOW_SHAPE_KIND_ARROW = 1,
    SNOW_SHAPE_KIND_LINE = 2,
    SNOW_SHAPE_KIND_FREE_DRAW = 3,
    SNOW_SHAPE_KIND_RECTANGLE_HIGHLIGHT = 4,
    SNOW_SHAPE_KIND_PEN_HIGHLIGHT = 5,
    SNOW_SHAPE_KIND_SPOTLIGHT = 6
} SnowShapeKind;
#define SNOW_SHAPE_KIND_HIGHLIGHT SNOW_SHAPE_KIND_RECTANGLE_HIGHLIGHT

#define SNOW_SHAPE_STYLE_PROPERTY_FILL (1u << 0)
#define SNOW_SHAPE_STYLE_PROPERTY_FILL_STYLE (1u << 1)
#define SNOW_SHAPE_STYLE_PROPERTY_STROKE (1u << 2)
#define SNOW_SHAPE_STYLE_PROPERTY_STROKE_WIDTH (1u << 3)
#define SNOW_SHAPE_STYLE_PROPERTY_CORNER_RADII (1u << 4)
#define SNOW_SHAPE_STYLE_PROPERTY_START_ARROWHEAD (1u << 5)
#define SNOW_SHAPE_STYLE_PROPERTY_END_ARROWHEAD (1u << 6)
#define SNOW_SHAPE_STYLE_PROPERTY_STROKE_STYLE (1u << 7)
#define SNOW_SHAPE_STYLE_PROPERTY_ARROW_TYPE (1u << 8)
#define SNOW_SHAPE_STYLE_PROPERTY_OPACITY (1u << 9)
#define SNOW_SHAPE_STYLE_PROPERTY_HIGHLIGHT_SHAPE (1u << 10)
#define SNOW_SHAPE_STYLE_PROPERTY_SHAPE (1u << 11)
#define SNOW_SHAPE_STYLE_PROPERTY_LINE                                                             \
    (SNOW_SHAPE_STYLE_PROPERTY_FILL | SNOW_SHAPE_STYLE_PROPERTY_FILL_STYLE |                       \
     SNOW_SHAPE_STYLE_PROPERTY_STROKE | SNOW_SHAPE_STYLE_PROPERTY_STROKE_WIDTH |                   \
     SNOW_SHAPE_STYLE_PROPERTY_STROKE_STYLE | SNOW_SHAPE_STYLE_PROPERTY_OPACITY)

typedef struct SnowRectangleShapeStyle {
    SnowColorRgba8 fill;
    SnowFillStyle fill_style;
    SnowColorRgba8 stroke;
    double stroke_width;
    SnowStrokeStyle stroke_style;
    SnowCornerRadii corner_radii;
} SnowRectangleShapeStyle;

typedef struct SnowArrowStyle {
    SnowColorRgba8 stroke;
    double stroke_width;
    SnowArrowhead start_arrowhead;
    SnowArrowhead end_arrowhead;
    SnowArrowStrokeStyle stroke_style;
    SnowArrowType arrow_type;
    uint8_t reserved0[4];
} SnowArrowStyle;

typedef struct SnowTextStyle {
    SnowColorRgba8 color;
    double font_size;
    SnowColorRgba8 fill;
    SnowFillStyle fill_style;
    SnowColorRgba8 stroke;
    double stroke_width;
    SnowCornerRadii corner_radii;
    SnowTextHorizontalAlign horizontal_align;
    SnowTextVerticalAlign vertical_align;
    double opacity;
    uint8_t reserved0[4];
    uint32_t font_family_utf8_len;
    uint8_t font_family_truncated;
    uint8_t reserved1[3];
    char font_family_utf8[SNOW_FONT_FAMILY_UTF8_CAPACITY];
} SnowTextStyle;

typedef struct SnowSerialNumberStyle {
    int64_t number;
    SnowColorRgba8 color;
    SnowColorRgba8 fill;
    SnowFillStyle fill_style;
    double font_size;
    double stroke_width;
    SnowStrokeStyle stroke_style;
    double opacity;
    uint8_t reserved0[4];
    uint32_t font_family_utf8_len;
    uint8_t font_family_truncated;
    uint8_t reserved1[3];
    char font_family_utf8[SNOW_FONT_FAMILY_UTF8_CAPACITY];
} SnowSerialNumberStyle;

typedef struct SnowStyleToolbarState {
    SnowStyleToolbarSource source;
    uint32_t reserved0;
    SnowShapeStyle shape_style;
    SnowTextStyle text_style;
    SnowSerialNumberStyle serial_number_style;
    uint32_t text_style_mixed;
    uint32_t serial_number_style_mixed;
    uint32_t shape_style_mixed;
    SnowFilterStyle filter_style;
    uint32_t filter_style_mixed;
} SnowStyleToolbarState;

typedef struct SnowStyleDefaults {
    SnowShapeStyle rectangle;
    SnowShapeStyle arrow;
    SnowShapeStyle line;
    SnowShapeStyle free_draw;
    SnowShapeStyle rectangle_highlight;
    SnowShapeStyle pen_highlight;
    SnowFilterStyle rectangle_filter;
    SnowFilterStyle pen_filter;
    SnowTextStyle text;
    SnowSerialNumberStyle serial_number;
    SnowWatermarkConfig watermark;
    SnowSpotlightConfig spotlight;
} SnowStyleDefaults;

struct SnowRuntimeConfig {
    const SnowStyleDefaults* style_defaults;
};

typedef struct SnowSerialNumberToolbarState {
    uint8_t visible;
    uint8_t can_decrease;
    uint8_t can_increase;
    uint8_t can_create_text;
    uint8_t reserved0[4];
    double left;
    double top;
    double width;
    double height;
} SnowSerialNumberToolbarState;

typedef struct SnowHistoryState {
    uint8_t can_undo;
    uint8_t can_redo;
    uint8_t reserved0[6];
} SnowHistoryState;

typedef struct SnowModifiers {
    uint8_t ctrl;
    uint8_t shift;
    uint8_t alt;
    uint8_t meta;
} SnowModifiers;

typedef struct SnowSnapConfig {
    uint8_t enabled;
    uint8_t enable_point_snaps;
    uint8_t enable_gap_snaps;
    uint8_t show_guides;
    uint8_t show_gap_size;
    uint8_t reserved0[3];
    double distance;
    SnowColorRgba8 line_color;
    double line_width;
    double marker_size;
    double gap_dash_length;
    double gap_dash_gap;
} SnowSnapConfig;

typedef struct SnowGridConfig {
    uint8_t enabled;
    uint8_t reserved0[7];
    double size;
} SnowGridConfig;

typedef struct SnowEngineConfig {
    double min_zoom;
    double max_zoom;
    SnowZoomFocus zoom_focus;
    double wheel_zoom_sensitivity;
    SnowColorRgba8 clear_color;
    SnowSnapConfig snap;
    SnowGridConfig grid;
    uint8_t enable_pointer_capture;
    uint8_t reserved[7];
} SnowEngineConfig;

#define SNOW_TEXT_UTF8_CAPACITY 1024

typedef struct SnowElementId {
    uint32_t index;
    uint32_t generation;
} SnowElementId;

typedef struct SnowTextLayoutSize {
    /* Exact host-renderer measured text width. */
    double width;
    /* Exact host-renderer measured text height. */
    double height;
} SnowTextLayoutSize;

typedef struct SnowTextLayoutOverride {
    /* Text element that this exact host-renderer layout applies to. */
    SnowElementId id;
    SnowTextLayoutSize size;
} SnowTextLayoutOverride;

typedef struct SnowTextCommitDraft {
    /* Existing text element to edit when has_existing_element is nonzero. Ignored for new text. */
    SnowElementId element_id;
    /* Nonzero edits element_id; zero creates a new text element. */
    uint8_t has_existing_element;
    /* Nonzero keeps auto-resize enabled after commit. */
    uint8_t auto_resize;
    /* Nonzero updates the editor default text style from style when text is non-empty. */
    uint8_t update_default_style;
    uint8_t reserved0[5];
    /* Final host-measured text center in canvas coordinates. */
    double center_x;
    double center_y;
    /* UTF-8 draft contents. May be null only when text_utf8_len is zero. */
    const char* text_utf8;
    uint32_t text_utf8_len;
    uint32_t reserved1;
    /* Exact host-renderer layout for the committed text. */
    SnowTextLayoutSize measured_layout;
    /* Full draft style to persist for the committed text. */
    SnowTextStyle style;
} SnowTextCommitDraft;

typedef struct SnowActiveTextDraftPresentation {
    SnowElementId element_id;
    uint8_t has_existing_element;
    uint8_t auto_resize;
    uint8_t reserved0[6];
    double center_x;
    double center_y;
    double width;
    double height;
    double rotation;
    const char* text_utf8;
    uint32_t text_utf8_len;
    uint32_t reserved1;
    SnowTextStyle style;
} SnowActiveTextDraftPresentation;

typedef struct SnowTextElementInfo {
    SnowElementId id;
    double center_x;
    double center_y;
    double width;
    double height;
    double rotation;
    double font_size;
    uint32_t text_utf8_len;
    uint8_t text_truncated;
    uint8_t auto_resize;
    /* Nonzero asks the host to measure natural unwrapped width; zero asks for
       wrapped height using width. */
    uint8_t measure_natural_width;
    uint8_t reserved0[1];
    char text_utf8[SNOW_TEXT_UTF8_CAPACITY];
    uint32_t font_family_utf8_len;
    uint8_t font_family_truncated;
    uint8_t reserved1[3];
    char font_family_utf8[SNOW_FONT_FAMILY_UTF8_CAPACITY];
} SnowTextElementInfo;

typedef struct SnowPointerEvent {
    uint32_t pointer_id;
    SnowPointerEventType event_type;
    SnowPointerDevice device;
    double position_x;
    double position_y;
    SnowPointerButton button;
    uint8_t buttons;
    uint8_t reserved0[3];
    SnowModifiers modifiers;
} SnowPointerEvent;

typedef struct SnowWheelEvent {
    double position_x;
    double position_y;
    double delta_x;
    double delta_y;
    SnowWheelDeltaKind delta_kind;
    SnowModifiers modifiers;
} SnowWheelEvent;

typedef struct SnowKeyEvent {
    SnowKeyEventType event_type;
    SnowKeyCode key_code;
    uint32_t codepoint;
    SnowModifiers modifiers;
    uint8_t repeat;
    uint8_t reserved0[3];
} SnowKeyEvent;

typedef struct SnowInputEvent {
    SnowInputEventKind kind;
    SnowPointerEvent pointer;
    SnowWheelEvent wheel;
    SnowKeyEvent key;
} SnowInputEvent;

typedef struct SnowInteractionOutput {
    uint8_t consumed;
    uint8_t reserved0[3];
    SnowPointerCaptureCommandKind capture_kind;
    uint32_t capture_pointer_id;
    SnowCursorCommandKind cursor_kind;
    SnowCursorStyle cursor_style;
} SnowInteractionOutput;

typedef struct SnowPatchCursor {
    uint64_t scene_revision;
    uint64_t decoration_revision;
    uint64_t overlay_revision;
} SnowPatchCursor;

typedef struct SnowPatchOp {
    uint32_t start;
    uint32_t delete_count;
    uint32_t insert_offset;
    uint32_t insert_count;
} SnowPatchOp;

typedef struct SnowPenFilterGeometryPatch {
    SnowElementId element_id;
    uint64_t expected_geometry_revision;
    uint64_t resulting_geometry_revision;
    uint32_t retain_prefix_count;
    uint32_t append_offset;
    uint32_t append_count;
    double old_changed_min_x;
    double old_changed_min_y;
    double old_changed_max_x;
    double old_changed_max_y;
    double new_changed_min_x;
    double new_changed_min_y;
    double new_changed_max_x;
    double new_changed_max_y;
    uint8_t full_reset;
    uint8_t element_removed;
    uint8_t reserved0[6];
} SnowPenFilterGeometryPatch;

typedef struct SnowPathGeometryPatch {
    SnowElementId element_id;
    uint64_t expected_geometry_revision;
    uint64_t resulting_geometry_revision;
    uint32_t range_offset;
    uint32_t range_count;
    double old_changed_min_x;
    double old_changed_min_y;
    double old_changed_max_x;
    double old_changed_max_y;
    double new_changed_min_x;
    double new_changed_min_y;
    double new_changed_max_x;
    double new_changed_max_y;
    uint8_t closed;
    uint8_t full_reset;
    uint8_t element_removed;
    uint8_t reserved0[5];
} SnowPathGeometryPatch;

typedef struct SnowPathChunkRange {
    uint32_t start;
    uint32_t delete_count;
    uint32_t insert_chunk_offset;
    uint32_t insert_chunk_count;
} SnowPathChunkRange;

typedef struct SnowPathChunk {
    uint64_t stable_id;
    uint32_t command_start;
    uint32_t command_offset;
    uint32_t command_count;
    uint32_t reserved0;
    double start_x;
    double start_y;
    double min_x;
    double min_y;
    double max_x;
    double max_y;
    double cumulative_start_length;
} SnowPathChunk;

#define SNOW_WATERMARK_TEXT_CAPACITY 256
#define SNOW_WATERMARK_FONT_FAMILY_CAPACITY 128

typedef struct SnowPatchInfo {
    uint64_t scene_base_revision;
    uint64_t scene_revision;
    uint64_t decoration_base_revision;
    uint64_t decoration_revision;
    uint64_t overlay_base_revision;
    uint64_t overlay_revision;
    uint8_t scene_reset;
    uint8_t decoration_reset;
    uint8_t overlay_reset;
    uint8_t reserved0[5];
    uint32_t scene_op_count;
    uint32_t overlay_op_count;
    uint32_t spotlight_op_count;
    uint32_t scene_item_count;
    uint32_t overlay_item_count;
    uint32_t spotlight_item_count;
    uint32_t scene_dirty_rect_count;
    uint32_t decoration_dirty_rect_count;
    uint32_t overlay_dirty_rect_count;
    uint32_t surface_width;
    uint32_t surface_height;
    double camera_center_x;
    double camera_center_y;
    double camera_zoom;
    SnowColorRgba8 clear_color;
    SnowColorRgba8 watermark_color;
    uint16_t watermark_text_len;
    uint16_t watermark_font_family_len;
    uint8_t watermark_text[SNOW_WATERMARK_TEXT_CAPACITY];
    double watermark_font_size;
    uint8_t watermark_font_family[SNOW_WATERMARK_FONT_FAMILY_CAPACITY];
    double watermark_angle;
    double watermark_gap;
    double watermark_opacity;
    SnowColorRgba8 spotlight_color;
    double spotlight_opacity;
    uint8_t spotlight_active;
    uint8_t reserved1[7];
} SnowPatchInfo;

typedef struct SnowDirtyRect {
    double min_x;
    double min_y;
    double max_x;
    double max_y;
} SnowDirtyRect;

typedef struct SnowSpotlightCutout {
    double center_x;
    double center_y;
    double width;
    double height;
    double rotation;
} SnowSpotlightCutout;

#define SNOW_ARROW_POINT_CAPACITY 64

typedef struct SnowArrowPoint {
    double x;
    double y;
} SnowArrowPoint;

typedef struct SnowArrowheadPrimitive {
    SnowArrowheadPrimitiveKind kind;
    SnowArrowheadFillMode fill_mode;
    SnowArrowheadDashMode dash_mode;
    uint32_t point_count;
    SnowArrowPoint points[SNOW_ARROWHEAD_PRIMITIVE_POINT_CAPACITY];
    SnowArrowPoint center;
    double diameter;
} SnowArrowheadPrimitive;

typedef struct SnowArrowPathCommand {
    SnowArrowPathCommandKind kind;
    uint32_t reserved0;
    SnowArrowPoint point;
    SnowArrowPoint control1;
    SnowArrowPoint control2;
} SnowArrowPathCommand;

/*
 * Compact borrowed display view. Pointer fields are valid only while the
 * SnowPatchHandle that returned this item remains alive. A pointer may be
 * null only when its corresponding count or UTF-8 length is zero.
 */
typedef struct SnowFilterRenderSpec {
    uint32_t filter_type;
    uint32_t reserved0;
    double strength;
    double mosaic_block_size;
    double blur_sigma;
    double sampling_radius;
} SnowFilterRenderSpec;

SnowFilterRenderSpec snow_filter_render_spec_resolve(uint32_t filter_type, double strength);

typedef struct SnowSceneDisplayItem {
    SnowSceneDisplayItemKind kind;
    SnowBlendMode blend_mode;
    SnowElementId element_id;
    double center_x;
    double center_y;
    double width;
    double height;
    double rotation;
    SnowColorRgba8 fill;
    SnowColorRgba8 stroke;
    SnowColorRgba8 text_color;
    double stroke_width;
    SnowCornerRadii corner_radii;
    uint32_t arrow_point_count;
    SnowArrowType arrow_type;
    uint8_t is_free_draw;
    uint8_t reserved1[2];
    SnowArrowhead arrow_start_head;
    SnowArrowhead arrow_end_head;
    SnowArrowStrokeStyle arrow_stroke_style;
    uint32_t bound_text_element_index;
    const SnowArrowPoint* arrow_points;
    uint32_t arrow_path_command_count;
    uint32_t arrowhead_primitive_count;
    const SnowArrowPathCommand* arrow_path_commands;
    const SnowArrowheadPrimitive* arrowhead_primitives;
    double font_size;
    double opacity;
    int64_t serial_number;
    uint32_t text_utf8_len;
    SnowTextHorizontalAlign text_horizontal_align;
    SnowTextVerticalAlign text_vertical_align;
    SnowFillStyle fill_style;
    SnowStrokeStyle stroke_style;
    uint8_t has_bound_text_element;
    uint8_t rect_shape;
    uint8_t reserved2[2];
    uint32_t bound_text_element_generation;
    SnowFilterRenderSpec filter;
    const char* text_utf8;
    uint32_t font_family_utf8_len;
    const char* font_family_utf8;
} SnowSceneDisplayItem;

/* Pointer fields follow the same SnowPatchHandle lifetime as scene items. */
typedef struct SnowOverlayDisplayItem {
    SnowOverlayDisplayItemKind kind;
    SnowOverlayRectKind rect_kind;
    SnowSnapGuideKind snap_guide_kind;
    SnowSnapGuideAxis snap_guide_axis;
    uint8_t snap_marker_count;
    uint8_t snap_has_label;
    uint16_t reserved0;
    double center_x;
    double center_y;
    double width;
    double height;
    double rotation;
    SnowColorRgba8 fill;
    SnowColorRgba8 stroke;
    double stroke_width;
    SnowCornerRadii corner_radii;
    uint32_t arrow_point_count;
    SnowArrowType arrow_type;
    uint8_t reserved1[3];
    SnowArrowhead arrow_start_head;
    SnowArrowhead arrow_end_head;
    SnowArrowStrokeStyle arrow_stroke_style;
    SnowFillStyle fill_style;
    const SnowArrowPoint* arrow_points;
    uint32_t arrow_path_command_count;
    uint32_t arrowhead_primitive_count;
    const SnowArrowPathCommand* arrow_path_commands;
    const SnowArrowheadPrimitive* arrowhead_primitives;
    double snap_start_x;
    double snap_start_y;
    double snap_end_x;
    double snap_end_y;
    double snap_marker0_x;
    double snap_marker0_y;
    double snap_marker1_x;
    double snap_marker1_y;
    double snap_label;
    SnowColorRgba8 snap_color;
    double snap_line_width;
    double snap_marker_size;
    double snap_gap_dash_length;
    double snap_gap_dash_gap;
} SnowOverlayDisplayItem;

SnowError snow_runtime_create(SnowRuntime* out_runtime);

SnowError snow_runtime_create_with_config(const SnowRuntimeConfig* config,
                                          SnowRuntime* out_runtime);

SnowError snow_runtime_style_defaults_default(SnowStyleDefaults* out_defaults);

SnowError snow_runtime_clone_document_session(SnowRuntime source, SnowRuntime* out_runtime);

SnowError snow_runtime_clone_document_session_with_config(SnowRuntime source,
                                                          const SnowRuntimeConfig* config,
                                                          SnowRuntime* out_runtime);

void snow_runtime_destroy(SnowRuntime runtime);

SnowError snow_engine_create(SnowEngine* out_engine);

void snow_engine_destroy(SnowEngine engine);

SnowError snow_runtime_set_quick_selection_disabled_tools(SnowRuntime runtime, uint64_t tools);

SnowError snow_runtime_set_quick_selection_disabled_tools_ex(
    SnowRuntime runtime, uint64_t tools, SnowChangedViewportList* out_changed_viewports);

SnowError snow_viewport_create(SnowRuntime runtime, const SnowEngineConfig* config,
                               SnowViewport* out_viewport);

void snow_viewport_destroy(SnowRuntime runtime, SnowViewport viewport);

SnowError snow_viewport_set_surface_size(SnowRuntime runtime, SnowViewport viewport, uint32_t width,
                                         uint32_t height);

SnowError snow_viewport_set_camera(SnowRuntime runtime, SnowViewport viewport, double center_x,
                                   double center_y, double zoom);

SnowError snow_viewport_get_id(SnowViewport viewport, uint64_t* out_id);

SnowError snow_viewport_set_active_tool(SnowRuntime runtime, SnowViewport viewport,
                                        SnowActiveTool tool);

SnowError snow_viewport_set_active_tool_ex(SnowRuntime runtime, SnowViewport viewport,
                                           SnowActiveTool tool,
                                           SnowChangedViewportList* out_changed_viewports);

SnowError snow_viewport_get_active_tool(SnowRuntime runtime, SnowViewport viewport,
                                        SnowActiveTool* out_tool);

SnowError snow_viewport_get_snap_config(SnowRuntime runtime, SnowViewport viewport,
                                        SnowSnapConfig* out_config);

SnowError snow_viewport_set_snap_config(SnowRuntime runtime, SnowViewport viewport,
                                        const SnowSnapConfig* config);

SnowError snow_viewport_set_snap_config_ex(SnowRuntime runtime, SnowViewport viewport,
                                           const SnowSnapConfig* config,
                                           SnowChangedViewportList* out_changed_viewports);

SnowError snow_viewport_get_grid_config(SnowRuntime runtime, SnowViewport viewport,
                                        SnowGridConfig* out_config);

SnowError snow_viewport_set_grid_config(SnowRuntime runtime, SnowViewport viewport,
                                        const SnowGridConfig* config);

SnowError snow_viewport_set_grid_config_ex(SnowRuntime runtime, SnowViewport viewport,
                                           const SnowGridConfig* config,
                                           SnowChangedViewportList* out_changed_viewports);

SnowError snow_runtime_get_history_state(SnowRuntime runtime, SnowHistoryState* out_state);

SnowError
snow_runtime_clear_document_preserving_viewports(SnowRuntime runtime,
                                                 SnowChangedViewportList* out_changed_viewports);

SnowError snow_runtime_restore_document_history_preserving_editor_styles(
    SnowRuntime runtime, const uint8_t* bytes, size_t size,
    SnowChangedViewportList* out_changed_viewports);

SnowError snow_runtime_undo(SnowRuntime runtime);

SnowError snow_runtime_undo_ex(SnowRuntime runtime, SnowChangedViewportList* out_changed_viewports);

SnowError snow_runtime_redo(SnowRuntime runtime);

SnowError snow_runtime_redo_ex(SnowRuntime runtime, SnowChangedViewportList* out_changed_viewports);

SnowError snow_engine_get_history_state(SnowEngine engine, SnowHistoryState* out_state);

SnowError snow_engine_undo(SnowEngine engine);

SnowError snow_engine_undo_ex(SnowEngine engine, SnowChangedViewportList* out_changed_viewports);

SnowError snow_engine_redo(SnowEngine engine);

SnowError snow_engine_redo_ex(SnowEngine engine, SnowChangedViewportList* out_changed_viewports);

SnowError snow_viewport_get_style_toolbar_state(SnowRuntime runtime, SnowViewport viewport,
                                                SnowStyleToolbarState* out_state);

SnowError snow_viewport_get_serial_number_toolbar_state(SnowRuntime runtime, SnowViewport viewport,
                                                        SnowSerialNumberToolbarState* out_state);

SnowError snow_viewport_set_shape_style_patch_ex(SnowRuntime runtime, SnowViewport viewport,
                                                 const SnowShapeStyle* style, uint32_t properties,
                                                 SnowShapeKind kind,
                                                 SnowChangedViewportList* out_changed_viewports);
SnowError snow_viewport_set_filter_style_ex(SnowRuntime runtime, SnowViewport viewport,
                                            const SnowFilterStyle* style, uint32_t properties,
                                            SnowChangedViewportList* out_changed_viewports);
SnowError snow_viewport_get_watermark_config(SnowRuntime runtime, SnowViewport viewport,
                                             SnowWatermarkConfig* out_config);
SnowError snow_viewport_set_watermark_config(SnowRuntime runtime, SnowViewport viewport,
                                             const SnowWatermarkConfig* config);
SnowError snow_viewport_set_watermark_config_ex(SnowRuntime runtime, SnowViewport viewport,
                                                const SnowWatermarkConfig* config,
                                                SnowChangedViewportList* out_changed_viewports);

SnowError snow_viewport_get_rectangle_shape_style(SnowRuntime runtime, SnowViewport viewport,
                                                  SnowRectangleShapeStyle* out_style);

SnowError snow_viewport_set_rectangle_shape_style(SnowRuntime runtime, SnowViewport viewport,
                                                  const SnowRectangleShapeStyle* style);

SnowError
snow_viewport_set_rectangle_shape_style_ex(SnowRuntime runtime, SnowViewport viewport,
                                           const SnowRectangleShapeStyle* style,
                                           SnowChangedViewportList* out_changed_viewports);

SnowError snow_viewport_get_arrow_style(SnowRuntime runtime, SnowViewport viewport,
                                        SnowArrowStyle* out_style);

SnowError snow_viewport_get_text_style(SnowRuntime runtime, SnowViewport viewport,
                                       SnowTextStyle* out_style);

SnowError snow_viewport_set_text_style(SnowRuntime runtime, SnowViewport viewport,
                                       const SnowTextStyle* style,
                                       const SnowTextLayoutOverride* layouts,
                                       uint32_t layout_count);

SnowError snow_viewport_set_text_style_ex(SnowRuntime runtime, SnowViewport viewport,
                                          const SnowTextStyle* style,
                                          const SnowTextLayoutOverride* layouts,
                                          uint32_t layout_count,
                                          SnowChangedViewportList* out_changed_viewports);

SnowError snow_viewport_get_serial_number_style(SnowRuntime runtime, SnowViewport viewport,
                                                SnowSerialNumberStyle* out_style);

SnowError snow_viewport_set_serial_number_style(SnowRuntime runtime, SnowViewport viewport,
                                                const SnowSerialNumberStyle* style);

SnowError snow_viewport_set_serial_number_style_ex(SnowRuntime runtime, SnowViewport viewport,
                                                   const SnowSerialNumberStyle* style,
                                                   SnowChangedViewportList* out_changed_viewports);

SnowError snow_viewport_create_text(SnowRuntime runtime, SnowViewport viewport, double center_x,
                                    double center_y, const char* text_utf8, uint32_t text_utf8_len,
                                    double measured_width, double measured_height);

SnowError snow_viewport_create_text_ex(SnowRuntime runtime, SnowViewport viewport, double center_x,
                                       double center_y, const char* text_utf8,
                                       uint32_t text_utf8_len, double measured_width,
                                       double measured_height,
                                       SnowChangedViewportList* out_changed_viewports);

SnowError snow_viewport_hit_text(SnowRuntime runtime, SnowViewport viewport, double canvas_x,
                                 double canvas_y, SnowElementId* out_id, uint8_t* out_hit);

SnowError snow_viewport_is_element_selected(SnowRuntime runtime, SnowViewport viewport,
                                            SnowElementId id, uint8_t* out_selected);

SnowError snow_viewport_selected_text_count(SnowRuntime runtime, SnowViewport viewport,
                                            uint32_t* out_count);

SnowError snow_viewport_get_selected_text_elements(SnowRuntime runtime, SnowViewport viewport,
                                                   SnowTextElementInfo* out_items,
                                                   uint32_t capacity, uint32_t* out_count);

SnowError snow_viewport_get_active_text_resize_measurement(SnowRuntime runtime,
                                                           SnowViewport viewport,
                                                           SnowTextElementInfo* out_info,
                                                           uint8_t* out_active);

SnowError snow_viewport_apply_active_text_resize_measurement_ex(
    SnowRuntime runtime, SnowViewport viewport, const SnowTextLayoutSize* layout,
    SnowChangedViewportList* out_changed_viewports);

SnowError
snow_viewport_set_active_text_draft_presentation_ex(SnowRuntime runtime, SnowViewport viewport,
                                                    const SnowActiveTextDraftPresentation* draft,
                                                    SnowChangedViewportList* out_changed_viewports);

SnowError snow_viewport_clear_active_text_draft_presentation_ex(
    SnowRuntime runtime, SnowViewport viewport, SnowChangedViewportList* out_changed_viewports);

SnowError snow_viewport_get_active_text_draft_presentation(SnowRuntime runtime,
                                                           SnowViewport viewport,
                                                           SnowTextElementInfo* out_info,
                                                           SnowTextStyle* out_style,
                                                           uint8_t* out_active);

SnowError snow_viewport_is_text_bound_to_serial_number(SnowRuntime runtime, SnowViewport viewport,
                                                       SnowElementId id, uint8_t* out_bound);

SnowError snow_runtime_get_text_element(SnowRuntime runtime, SnowElementId id,
                                        SnowTextElementInfo* out_info);

/* Legacy unstyled commit path. This preserves the historical behavior of using
   the editor/default text style and should not be treated as a full draft-style
   commit. New text editors should prefer snow_viewport_commit_text_draft_payload_ex. */
SnowError snow_viewport_commit_text_ex(SnowRuntime runtime, SnowViewport viewport,
                                       SnowElementId existing_id, uint8_t has_existing_id,
                                       double center_x, double center_y, const char* text_utf8,
                                       uint32_t text_utf8_len, double measured_width,
                                       double measured_height,
                                       SnowChangedViewportList* out_changed_viewports);

/* Typed draft commit path. Persists text content, exact host layout, auto-resize
   state, and full SnowTextStyle from SnowTextCommitDraft. */
SnowError
snow_viewport_commit_text_draft_payload_ex(SnowRuntime runtime, SnowViewport viewport,
                                           const SnowTextCommitDraft* draft,
                                           SnowChangedViewportList* out_changed_viewports);

/* Convenience wrapper for snow_viewport_commit_text_draft_payload_ex. Unlike
   snow_viewport_commit_text_ex, this is a styled draft commit. */
SnowError snow_viewport_commit_text_draft_ex(
    SnowRuntime runtime, SnowViewport viewport, SnowElementId existing_id, uint8_t has_existing_id,
    double center_x, double center_y, const char* text_utf8, uint32_t text_utf8_len,
    double measured_width, double measured_height, const SnowTextStyle* style, uint8_t auto_resize,
    uint8_t update_default_style, SnowChangedViewportList* out_changed_viewports);

SnowError snow_viewport_select_element_ex(SnowRuntime runtime, SnowViewport viewport,
                                          SnowElementId id,
                                          SnowChangedViewportList* out_changed_viewports);

SnowError snow_viewport_reset_editing_state_ex(SnowRuntime runtime, SnowViewport viewport,
                                               SnowChangedViewportList* out_changed_viewports);

SnowError snow_viewport_delete_selected_ex(SnowRuntime runtime, SnowViewport viewport,
                                           SnowChangedViewportList* out_changed_viewports);

SnowError snow_viewport_duplicate_selected_ex(SnowRuntime runtime, SnowViewport viewport,
                                              double offset_x, double offset_y,
                                              SnowChangedViewportList* out_changed_viewports);

SnowError snow_viewport_reorder_selected_ex(SnowRuntime runtime, SnowViewport viewport,
                                            uint32_t action,
                                            SnowChangedViewportList* out_changed_viewports);

SnowError snow_viewport_set_selected_opacity_ex(SnowRuntime runtime, SnowViewport viewport,
                                                double opacity,
                                                SnowChangedViewportList* out_changed_viewports);

SnowError
snow_viewport_adjust_selected_serial_numbers_ex(SnowRuntime runtime, SnowViewport viewport,
                                                int64_t delta,
                                                SnowChangedViewportList* out_changed_viewports);

SnowError
snow_viewport_create_serial_number_text_ex(SnowRuntime runtime, SnowViewport viewport,
                                           double measured_width, double measured_height,
                                           SnowElementId* out_text_id, uint8_t* out_has_text_id,
                                           SnowChangedViewportList* out_changed_viewports);

SnowError snow_viewport_process_input(SnowRuntime runtime, SnowViewport viewport,
                                      const SnowInputEvent* event,
                                      SnowInteractionOutput* out_output);

SnowError snow_viewport_process_input_ex(SnowRuntime runtime, SnowViewport viewport,
                                         const SnowInputEvent* event,
                                         SnowInteractionOutput* out_output,
                                         SnowChangedViewportList* out_changed_viewports);

/* Processes an ordered, non-empty batch of pointer-move events and refreshes
   viewport presentation once after the final event. */
SnowError snow_viewport_process_pointer_move_batch_ex(
    SnowRuntime runtime, SnowViewport viewport, const SnowInputEvent* events, uint32_t event_count,
    SnowInteractionOutput* out_output, SnowChangedViewportList* out_changed_viewports);

void snow_changed_viewports_destroy(SnowChangedViewportList changed_viewports);

uint32_t snow_changed_viewports_count(SnowChangedViewportList changed_viewports);

SnowError snow_changed_viewports_get(SnowChangedViewportList changed_viewports, uint32_t index,
                                     uint64_t* out_viewport_id);

SnowError snow_viewport_acquire_patch(SnowRuntime runtime, SnowViewport viewport,
                                      const SnowPatchCursor* cursor, SnowPatchHandle* out_patch);

void snow_patch_destroy(SnowPatchHandle patch);

SnowError snow_patch_get_info(SnowPatchHandle patch, SnowPatchInfo* out_info);

SnowError snow_patch_get_scene_ops(SnowPatchHandle patch, const SnowPatchOp** out_ops,
                                   uint32_t* out_count);

SnowError snow_patch_get_pen_filter_geometry_ops(SnowPatchHandle patch,
                                                 const SnowPenFilterGeometryPatch** out_ops,
                                                 uint32_t* out_count);

SnowError snow_patch_get_pen_filter_geometry_points(SnowPatchHandle patch,
                                                    const SnowArrowPoint** out_points,
                                                    uint32_t* out_count);

SnowError snow_patch_get_path_geometry_ops(SnowPatchHandle patch,
                                           const SnowPathGeometryPatch** out_ops,
                                           uint32_t* out_count);

SnowError snow_patch_get_path_geometry_ranges(SnowPatchHandle patch,
                                              const SnowPathChunkRange** out_ranges,
                                              uint32_t* out_count);

SnowError snow_patch_get_path_geometry_chunks(SnowPatchHandle patch,
                                              const SnowPathChunk** out_chunks,
                                              uint32_t* out_count);

SnowError snow_patch_get_path_geometry_commands(SnowPatchHandle patch,
                                                const SnowArrowPathCommand** out_commands,
                                                uint32_t* out_count);

SnowError snow_patch_get_overlay_ops(SnowPatchHandle patch, const SnowPatchOp** out_ops,
                                     uint32_t* out_count);

SnowError snow_patch_get_spotlight_ops(SnowPatchHandle patch, const SnowPatchOp** out_ops,
                                       uint32_t* out_count);

/* Returned items and all nested pointer fields borrow from `patch`. */
SnowError snow_patch_get_scene_items(SnowPatchHandle patch, const SnowSceneDisplayItem** out_items,
                                     uint32_t* out_count);

/* Returned items and all nested pointer fields borrow from `patch`. */
SnowError snow_patch_get_overlay_items(SnowPatchHandle patch,
                                       const SnowOverlayDisplayItem** out_items,
                                       uint32_t* out_count);

/* Returned cutouts borrow from `patch`. */
SnowError snow_patch_get_spotlight_cutouts(SnowPatchHandle patch,
                                           const SnowSpotlightCutout** out_cutouts,
                                           uint32_t* out_count);

SnowError snow_patch_get_scene_dirty_rects(SnowPatchHandle patch, const SnowDirtyRect** out_rects,
                                           uint32_t* out_count);

SnowError snow_patch_get_overlay_dirty_rects(SnowPatchHandle patch, const SnowDirtyRect** out_rects,
                                             uint32_t* out_count);
SnowError snow_viewport_get_spotlight_config(SnowRuntime runtime, SnowViewport viewport,
                                             SnowSpotlightConfig* out_config);
SnowError snow_viewport_set_spotlight_config(SnowRuntime runtime, SnowViewport viewport,
                                             const SnowSpotlightConfig* config);
SnowError snow_viewport_set_spotlight_config_ex(SnowRuntime runtime, SnowViewport viewport,
                                                const SnowSpotlightConfig* config,
                                                SnowChangedViewportList* out_changed_viewports);

SnowError snow_patch_get_decoration_dirty_rects(SnowPatchHandle patch,
                                                const SnowDirtyRect** out_rects,
                                                uint32_t* out_count);

#ifdef __cplusplus
}
#endif
