#include "snow_canvas_state.h"
#include "snow_canvas_runtime_access.h"
#include "snow_canvas_type_conversions.h"
#include "snow_canvas_viewport.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_types.h"

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void filterStyleParticipatesInToolbarStateDiffs() {
    snow_canvas_state::Snapshot previous;
    snow_canvas_state::Snapshot next = previous;
    next.styleToolbarState.filter_style.strength = 0.73;

    const snow_canvas_state::Changes styleChanges =
        snow_canvas_state::diffSnapshots(previous, next);
    require(styleChanges.styleToolbarChanged,
            "filter style changes should invalidate the toolbar state");

    previous = next;
    next.styleToolbarState.filter_style_mixed = 1;
    const snow_canvas_state::Changes mixedChanges =
        snow_canvas_state::diffSnapshots(previous, next);
    require(mixedChanges.styleToolbarChanged,
            "mixed filter properties should invalidate the toolbar state");

    require(!snow_canvas_state::diffSnapshots(next, next).any(),
            "identical snapshots should not emit state synchronization");
}

template <typename T> void requireEqualPair(const T& value, const char* message) {
    // The independent copy is the behavior under test here.
    const T copy = value; // NOLINT(performance-unnecessary-copy-initialization)
    require(value == copy && !(value != copy), message);
}

template <typename T> void requireUnequalPair(const T& lhs, const T& rhs, const char* message) {
    require(lhs != rhs && !(lhs == rhs), message);
}

void publicCanvasDtosUseExactCompleteEquality() {
    SnowCanvasCornerRadii radii{1.0, 2.0, 3.0, 4.0};
    requireEqualPair(radii, "identical corner radii should compare equal");
    for (int field = 0; field < 4; ++field) {
        SnowCanvasCornerRadii changed = radii;
        double* values[] = {&changed.topLeft, &changed.topRight, &changed.bottomRight,
                            &changed.bottomLeft};
        *values[field] += 0.25;
        requireUnequalPair(radii, changed, "every corner radius should participate");
    }

    SnowCanvasFilterStyle filter;
    requireEqualPair(filter, "identical filters should compare equal");
    auto changedFilter = filter;
    changedFilter.type = SnowCanvasFilterType::GaussianBlur;
    requireUnequalPair(filter, changedFilter, "filter type should participate");
    changedFilter = filter;
    changedFilter.strength += 0.1;
    requireUnequalPair(filter, changedFilter, "filter strength should participate");
    changedFilter = filter;
    changedFilter.opacity -= 0.1;
    requireUnequalPair(filter, changedFilter, "filter opacity should participate");

    SnowCanvasShapeStyle shape;
    shape.fill = QColor(1, 2, 3, 4);
    shape.stroke = QColor(5, 6, 7, 8);
    requireEqualPair(shape, "identical shape styles should compare equal");
#define REQUIRE_SHAPE_CHANGE(field, value)                                                         \
    do {                                                                                           \
        auto changed = shape;                                                                      \
        changed.field = value;                                                                     \
        requireUnequalPair(shape, changed, "shape field should participate");                      \
    } while (false)
    REQUIRE_SHAPE_CHANGE(fill, QColor(9, 2, 3, 4));
    REQUIRE_SHAPE_CHANGE(fillStyle, SnowCanvasFillStyle::CrossLine);
    REQUIRE_SHAPE_CHANGE(stroke, QColor(9, 6, 7, 8));
    REQUIRE_SHAPE_CHANGE(strokeWidth, 2.0);
    REQUIRE_SHAPE_CHANGE(cornerRadii, (SnowCanvasCornerRadii{1, 2, 3, 4}));
    REQUIRE_SHAPE_CHANGE(startArrowhead, SnowCanvasArrowhead::Bar);
    REQUIRE_SHAPE_CHANGE(endArrowhead, SnowCanvasArrowhead::Arrow);
    REQUIRE_SHAPE_CHANGE(strokeStyle, SnowCanvasArrowStrokeStyle::Dashed);
    REQUIRE_SHAPE_CHANGE(arrowType, SnowCanvasArrowType::Curve);
    REQUIRE_SHAPE_CHANGE(opacity, 0.5);
    REQUIRE_SHAPE_CHANGE(highlightShape, SnowCanvasHighlightShape::Ellipse);
    REQUIRE_SHAPE_CHANGE(shape, SnowCanvasRectangleShape::Diamond);
#undef REQUIRE_SHAPE_CHANGE

    SnowCanvasRectangleShapeStyle rectangle;
    requireEqualPair(rectangle, "identical rectangle styles should compare equal");
#define REQUIRE_RECTANGLE_CHANGE(field, value)                                                     \
    do {                                                                                           \
        auto changed = rectangle;                                                                  \
        changed.field = value;                                                                     \
        requireUnequalPair(rectangle, changed, "rectangle field should participate");              \
    } while (false)
    REQUIRE_RECTANGLE_CHANGE(fill, QColor(Qt::red));
    REQUIRE_RECTANGLE_CHANGE(fillStyle, SnowCanvasFillStyle::Line);
    REQUIRE_RECTANGLE_CHANGE(stroke, QColor(Qt::blue));
    REQUIRE_RECTANGLE_CHANGE(strokeWidth, 3.0);
    REQUIRE_RECTANGLE_CHANGE(strokeStyle, SnowCanvasStrokeStyle::Dotted);
    REQUIRE_RECTANGLE_CHANGE(cornerRadii, (SnowCanvasCornerRadii{2, 3, 4, 5}));
#undef REQUIRE_RECTANGLE_CHANGE

    SnowCanvasArrowStyle arrow;
    requireEqualPair(arrow, "identical arrow styles should compare equal");
#define REQUIRE_ARROW_CHANGE(field, value)                                                         \
    do {                                                                                           \
        auto changed = arrow;                                                                      \
        changed.field = value;                                                                     \
        requireUnequalPair(arrow, changed, "arrow field should participate");                      \
    } while (false)
    REQUIRE_ARROW_CHANGE(stroke, QColor(Qt::green));
    REQUIRE_ARROW_CHANGE(strokeWidth, 4.0);
    REQUIRE_ARROW_CHANGE(startArrowhead, SnowCanvasArrowhead::Dot);
    REQUIRE_ARROW_CHANGE(endArrowhead, SnowCanvasArrowhead::Triangle);
    REQUIRE_ARROW_CHANGE(strokeStyle, SnowCanvasArrowStrokeStyle::Dotted);
    REQUIRE_ARROW_CHANGE(arrowType, SnowCanvasArrowType::Elbow);
#undef REQUIRE_ARROW_CHANGE

    SnowCanvasTextStyle text;
    requireEqualPair(text, "identical text styles should compare equal");
#define REQUIRE_TEXT_CHANGE(field, value)                                                          \
    do {                                                                                           \
        auto changed = text;                                                                       \
        changed.field = value;                                                                     \
        requireUnequalPair(text, changed, "text field should participate");                        \
    } while (false)
    REQUIRE_TEXT_CHANGE(color, QColor(Qt::blue));
    REQUIRE_TEXT_CHANGE(fontSize, 31.0);
    REQUIRE_TEXT_CHANGE(fontFamily, QStringLiteral("Exact Font"));
    REQUIRE_TEXT_CHANGE(fill, QColor(Qt::yellow));
    REQUIRE_TEXT_CHANGE(fillStyle, SnowCanvasFillStyle::Line);
    REQUIRE_TEXT_CHANGE(stroke, QColor(Qt::black));
    REQUIRE_TEXT_CHANGE(strokeWidth, 1.0);
    REQUIRE_TEXT_CHANGE(cornerRadii, (SnowCanvasCornerRadii{1, 2, 3, 4}));
    REQUIRE_TEXT_CHANGE(horizontalAlign, SnowCanvasTextHorizontalAlign::Right);
    REQUIRE_TEXT_CHANGE(verticalAlign, SnowCanvasTextVerticalAlign::Bottom);
    REQUIRE_TEXT_CHANGE(opacity, 0.75);
#undef REQUIRE_TEXT_CHANGE

    SnowCanvasSerialNumberStyle serial;
    requireEqualPair(serial, "identical serial styles should compare equal");
#define REQUIRE_SERIAL_CHANGE(field, value)                                                        \
    do {                                                                                           \
        auto changed = serial;                                                                     \
        changed.field = value;                                                                     \
        requireUnequalPair(serial, changed, "serial field should participate");                    \
    } while (false)
    REQUIRE_SERIAL_CHANGE(number, 2);
    REQUIRE_SERIAL_CHANGE(color, QColor(Qt::cyan));
    REQUIRE_SERIAL_CHANGE(fill, QColor(Qt::magenta));
    REQUIRE_SERIAL_CHANGE(fillStyle, SnowCanvasFillStyle::CrossLine);
    REQUIRE_SERIAL_CHANGE(fontSize, 25.0);
    REQUIRE_SERIAL_CHANGE(fontFamily, QStringLiteral("Exact Serial Font"));
    REQUIRE_SERIAL_CHANGE(strokeWidth, 3.0);
    REQUIRE_SERIAL_CHANGE(strokeStyle, SnowCanvasStrokeStyle::Dashed);
    REQUIRE_SERIAL_CHANGE(opacity, 0.5);
#undef REQUIRE_SERIAL_CHANGE

    SnowCanvasWatermarkConfig watermark;
    requireEqualPair(watermark, "identical watermarks should compare equal");
#define REQUIRE_WATERMARK_CHANGE(field, value)                                                     \
    do {                                                                                           \
        auto changed = watermark;                                                                  \
        changed.field = value;                                                                     \
        requireUnequalPair(watermark, changed, "watermark field should participate");              \
    } while (false)
    REQUIRE_WATERMARK_CHANGE(color, QColor(Qt::white));
    REQUIRE_WATERMARK_CHANGE(text, QStringLiteral("watermark"));
    REQUIRE_WATERMARK_CHANGE(fontSize, 17.0);
    REQUIRE_WATERMARK_CHANGE(fontFamily, QStringLiteral("Exact Watermark Font"));
    REQUIRE_WATERMARK_CHANGE(angle, 31.0);
    REQUIRE_WATERMARK_CHANGE(gap, 57.0);
    REQUIRE_WATERMARK_CHANGE(opacity, 0.17);
#undef REQUIRE_WATERMARK_CHANGE

    SnowCanvasStyleToolbarState toolbar;
    requireEqualPair(toolbar, "identical toolbar states should compare equal");
#define REQUIRE_TOOLBAR_CHANGE(field, value)                                                       \
    do {                                                                                           \
        auto changed = toolbar;                                                                    \
        changed.field = value;                                                                     \
        requireUnequalPair(toolbar, changed, "toolbar field should participate");                  \
    } while (false)
    REQUIRE_TOOLBAR_CHANGE(source, SnowCanvasStyleToolbarSource::SelectedRectangle);
    auto changedToolbar = toolbar;
    changedToolbar.shapeStyle.arrowType = SnowCanvasArrowType::Curve;
    requireUnequalPair(toolbar, changedToolbar, "inactive shape fields should participate");
    changedToolbar = toolbar;
    changedToolbar.textStyle.fontFamily = QStringLiteral("text");
    requireUnequalPair(toolbar, changedToolbar, "text style should participate");
    changedToolbar = toolbar;
    changedToolbar.serialNumberStyle.number = 9;
    requireUnequalPair(toolbar, changedToolbar, "serial style should participate");
    REQUIRE_TOOLBAR_CHANGE(textStyleMixed, 1u);
    REQUIRE_TOOLBAR_CHANGE(serialNumberStyleMixed, 2u);
    REQUIRE_TOOLBAR_CHANGE(shapeStyleMixed, 4u);
    changedToolbar = toolbar;
    changedToolbar.filterStyle.strength = 0.25;
    requireUnequalPair(toolbar, changedToolbar, "filter style should participate");
    REQUIRE_TOOLBAR_CHANGE(filterStyleMixed, 8u);
#undef REQUIRE_TOOLBAR_CHANGE

    SnowCanvasSerialNumberToolbarState serialToolbar;
    requireEqualPair(serialToolbar, "identical serial-number toolbar states should compare equal");
#define REQUIRE_SERIAL_TOOLBAR_CHANGE(field, value)                                                \
    do {                                                                                           \
        auto changed = serialToolbar;                                                              \
        changed.field = value;                                                                     \
        requireUnequalPair(serialToolbar, changed,                                                 \
                           "serial-number toolbar field should participate");                      \
    } while (false)
    REQUIRE_SERIAL_TOOLBAR_CHANGE(visible, true);
    REQUIRE_SERIAL_TOOLBAR_CHANGE(geometry, QRectF(1, 2, 3, 4));
    REQUIRE_SERIAL_TOOLBAR_CHANGE(canDecrease, true);
    REQUIRE_SERIAL_TOOLBAR_CHANGE(canIncrease, true);
    REQUIRE_SERIAL_TOOLBAR_CHANGE(canCreateText, true);
#undef REQUIRE_SERIAL_TOOLBAR_CHANGE
}

SnowCanvasStyleDefaults customStyleDefaults() {
    SnowCanvasStyleDefaults defaults;
    const QColor transparent(1, 2, 3, 0);
    const QColor ink(17, 34, 51, 128);
    SnowCanvasShapeStyle* shapes[] = {
        &defaults.rectangle,          &defaults.arrow,        &defaults.line, &defaults.freeDraw,
        &defaults.rectangleHighlight, &defaults.penHighlight,
    };
    double width = 3.0;
    for (SnowCanvasShapeStyle* shape : shapes) {
        shape->fill = transparent;
        shape->stroke = ink;
        shape->strokeWidth = width;
        shape->opacity = 0.75;
        width += 1.0;
    }
    defaults.rectangleFilter = {
        SnowCanvasFilterType::GaussianBlur,
        0.41,
        0.51,
        9.0,
    };
    defaults.penFilter = {
        SnowCanvasFilterType::Inversion,
        0.42,
        0.52,
        10.0,
    };
    defaults.text.color = ink;
    defaults.text.fill = transparent;
    defaults.text.stroke = QColor(68, 85, 102, 255);
    defaults.text.fontSize = 31.0;
    defaults.text.fontFamily = QStringLiteral("Qt Text Font");
    defaults.serialNumber.color = ink;
    defaults.serialNumber.fill = transparent;
    defaults.serialNumber.fontSize = 25.0;
    defaults.serialNumber.fontFamily = QStringLiteral("Qt Serial Font");
    defaults.watermark.color = ink;
    defaults.watermark.text = QStringLiteral("Qt watermark");
    defaults.watermark.fontFamily = QStringLiteral("Qt Watermark Font");
    defaults.watermark.opacity = 0.24;
    defaults.spotlight.color = ink;
    defaults.spotlight.opacity = 0.62;
    return defaults;
}

SnowViewport createViewport(SnowCanvasRuntime& runtime) {
    SnowViewport viewport = nullptr;
    const SnowEngineConfig config = snow_canvas_viewport::defaultEngineConfig();
    require(snow_viewport_create(snow_canvas_runtime::Access::handle(runtime), &config,
                                 &viewport) == SNOW_OK &&
                viewport != nullptr,
            "runtime viewport creation should succeed");
    return viewport;
}

SnowStyleToolbarState toolbarState(SnowCanvasRuntime& runtime, SnowViewport viewport,
                                   SnowActiveTool tool) {
    SnowRuntime handle = snow_canvas_runtime::Access::handle(runtime);
    require(snow_viewport_set_active_tool(handle, viewport, tool) == SNOW_OK,
            "configured tool selection should succeed");
    SnowStyleToolbarState state{};
    require(snow_viewport_get_style_toolbar_state(handle, viewport, &state) == SNOW_OK,
            "configured toolbar state query should succeed");
    return state;
}

void configuredRuntimeProfileFollowsRestoreAndResetLifecycle() {
    const SnowCanvasStyleDefaults defaults = customStyleDefaults();
    SnowCanvasRuntimeConfig config;
    config.styleDefaults = defaults;
    SnowCanvasRuntime runtime(config);
    require(runtime.isValid(), "a complete configured runtime should be valid");

    SnowViewport viewport = createViewport(runtime);
    require(toolbarState(runtime, viewport, SNOW_ACTIVE_TOOL_SHAPE).shape_style.stroke_width ==
                defaults.rectangle.strokeWidth,
            "rectangle should expose its configured creation style");
    require(toolbarState(runtime, viewport, SNOW_ACTIVE_TOOL_ARROW).shape_style.stroke_width ==
                defaults.arrow.strokeWidth,
            "arrow should expose its configured creation style");
    require(toolbarState(runtime, viewport, SNOW_ACTIVE_TOOL_LINE).shape_style.stroke_width ==
                defaults.line.strokeWidth,
            "line should expose its configured creation style");
    require(toolbarState(runtime, viewport, SNOW_ACTIVE_TOOL_FREE_DRAW).shape_style.stroke_width ==
                defaults.freeDraw.strokeWidth,
            "free draw should expose its configured creation style");
    require(toolbarState(runtime, viewport, SNOW_ACTIVE_TOOL_RECTANGLE_HIGHLIGHT)
                    .shape_style.stroke_width == defaults.rectangleHighlight.strokeWidth,
            "rectangle highlight should expose its configured creation style");
    require(
        toolbarState(runtime, viewport, SNOW_ACTIVE_TOOL_PEN_HIGHLIGHT).shape_style.stroke_width ==
            defaults.penHighlight.strokeWidth,
        "pen highlight should expose its configured creation style");
    require(toolbarState(runtime, viewport, SNOW_ACTIVE_TOOL_RECTANGLE_FILTER)
                    .filter_style.stroke_width == defaults.rectangleFilter.strokeWidth,
            "rectangle filter should expose its configured creation style");
    require(
        toolbarState(runtime, viewport, SNOW_ACTIVE_TOOL_PEN_FILTER).filter_style.stroke_width ==
            defaults.penFilter.strokeWidth,
        "pen filter should expose its configured creation style");
    require(toolbarState(runtime, viewport, SNOW_ACTIVE_TOOL_TEXT).text_style.font_size ==
                defaults.text.fontSize,
            "text should expose its configured creation style");
    require(toolbarState(runtime, viewport, SNOW_ACTIVE_TOOL_SERIAL_NUMBER)
                    .serial_number_style.font_size == defaults.serialNumber.fontSize,
            "sequence numbers should expose their configured creation style");

    SnowShapeStyle edited = toolbarState(runtime, viewport, SNOW_ACTIVE_TOOL_SHAPE).shape_style;
    edited.stroke_width = 41.0;
    SnowChangedViewportList changed = nullptr;
    require(snow_viewport_set_shape_style_patch_ex(snow_canvas_runtime::Access::handle(runtime),
                                                   viewport, &edited,
                                                   SNOW_SHAPE_STYLE_PROPERTY_STROKE_WIDTH,
                                                   SNOW_SHAPE_KIND_RECTANGLE, &changed) == SNOW_OK,
            "editing the configured rectangle style should succeed");
    snow_changed_viewports_destroy(changed);

    const QByteArray fullSession = runtime.serializeDocumentSession();
    const QByteArray documentHistory = runtime.serializeDocumentHistory();
    require(!fullSession.isEmpty() && !documentHistory.isEmpty(),
            "configured runtime serialization should succeed");

    require(runtime.restoreDocumentSession(fullSession),
            "full configured session restore should succeed");
    viewport = createViewport(runtime);
    require(toolbarState(runtime, viewport, SNOW_ACTIVE_TOOL_SHAPE).shape_style.stroke_width ==
                41.0,
            "full session restore should retain current editor styles");

    require(runtime.restoreDocumentHistory(documentHistory),
            "configured document-history restore should succeed");
    viewport = createViewport(runtime);
    require(toolbarState(runtime, viewport, SNOW_ACTIVE_TOOL_SHAPE).shape_style.stroke_width ==
                defaults.rectangle.strokeWidth,
            "history-only restore should use the target runtime profile");

    edited = toolbarState(runtime, viewport, SNOW_ACTIVE_TOOL_SHAPE).shape_style;
    edited.stroke_width = 42.0;
    changed = nullptr;
    require(snow_viewport_set_shape_style_patch_ex(snow_canvas_runtime::Access::handle(runtime),
                                                   viewport, &edited,
                                                   SNOW_SHAPE_STYLE_PROPERTY_STROKE_WIDTH,
                                                   SNOW_SHAPE_KIND_RECTANGLE, &changed) == SNOW_OK,
            "style mutation before clear should succeed");
    snow_changed_viewports_destroy(changed);
    require(runtime.clearDocumentPreservingViewports(),
            "configured clear should preserve attached viewports");
    require(toolbarState(runtime, viewport, SNOW_ACTIVE_TOOL_SHAPE).shape_style.stroke_width ==
                42.0,
            "configured clear should preserve the current creation style on the retained viewport");

    require(runtime.reset(), "configured runtime reset should succeed");
    viewport = createViewport(runtime);
    require(toolbarState(runtime, viewport, SNOW_ACTIVE_TOOL_SHAPE).shape_style.stroke_width ==
                defaults.rectangle.strokeWidth,
            "configured runtime reset should restore the immutable profile");

    SnowCanvasStyleDefaults invalid = defaults;
    invalid.text.fill = QColor();
    SnowCanvasRuntimeConfig invalidConfig;
    invalidConfig.styleDefaults = invalid;
    SnowCanvasRuntime rejected(invalidConfig);
    require(!rejected.isValid(), "an invalid configured profile should be rejected atomically");

    invalid = defaults;
    // Runtime configuration must reject values that arrive outside the C++ enum domain.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    invalid.arrow.arrowType = static_cast<SnowCanvasArrowType>(99);
    invalidConfig.styleDefaults = invalid;
    SnowCanvasRuntime rejectedEnum(invalidConfig);
    require(!rejectedEnum.isValid(),
            "an invalid configured enum should be rejected before C ABI conversion");
}

void legacyRuntimeUsesGenericEngineDefaults() {
    SnowStyleDefaults expected{};
    require(snow_runtime_style_defaults_default(&expected) == SNOW_OK,
            "generic C defaults should be available");
    SnowCanvasRuntime runtime;
    require(runtime.isValid(), "legacy runtime creation should remain valid");
    SnowViewport viewport = createViewport(runtime);
    const SnowStyleToolbarState state = toolbarState(runtime, viewport, SNOW_ACTIVE_TOOL_SHAPE);
    require(state.shape_style.stroke.r == expected.rectangle.stroke.r &&
                state.shape_style.stroke.g == expected.rectangle.stroke.g &&
                state.shape_style.stroke.b == expected.rectangle.stroke.b &&
                state.shape_style.stroke_width == expected.rectangle.stroke_width,
            "legacy runtime creation should retain generic engine defaults");
}
} // namespace

int main() {
    filterStyleParticipatesInToolbarStateDiffs();
    publicCanvasDtosUseExactCompleteEquality();
    configuredRuntimeProfileFollowsRestoreAndResetLifecycle();
    legacyRuntimeUsesGenericEngineDefaults();
    return 0;
}
