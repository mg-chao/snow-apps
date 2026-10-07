#include "snow_shot/app/mcp/mcpstylepatch.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/screenshottoolbarlayoutmodel.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include <QApplication>
#include <QDir>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void persistedDefaultsMergeDistanceProperties() {
    using namespace snow_shot::presentation;
    const auto initial = screenshotCanvasStyleDefaults();
    require(initial.distance.stroke == initial.arrow.stroke && initial.distance.strokeWidth == 2 &&
                initial.distance.factor == 1 &&
                initial.distance.unit == SnowCanvasDistanceUnit::Cm &&
                initial.distance.decimalPlaces == 0 && initial.distance.endpointScale == 1 &&
                initial.distance.endpointStyle == SnowCanvasArrowhead::Bar,
            "distance defaults match arrow appearance with centimeter labels and bar endpoints");
    require(screenshotCanvasToolStyleDefaults().distance == initial.distance,
            "empty configuration uses centimeter distance defaults");
    auto changed = initial.distance;
    changed.factor = 2.25;
    changed.unit = SnowCanvasDistanceUnit::Mm;
    changed.decimalPlaces = 3;
    changed.endpointStyle = SnowCanvasArrowhead::Diamond;
    require(persistScreenshotCanvasStyleEdit(
                SnowCanvasDistanceEdit{changed, SnowCanvasDistanceStyleAllProperties}),
            "persist the complete distance style");
    auto widthOnly = initial.distance;
    widthOnly.strokeWidth = 4;
    require(persistScreenshotCanvasStyleEdit(
                SnowCanvasDistanceEdit{widthOnly, SnowCanvasDistanceStylePropertyStrokeWidth}),
            "merge a later distance width edit against stored defaults");
    const auto restored = screenshotCanvasToolStyleDefaults();
    changed.strokeWidth = 4;
    require(restored.distance == changed && restored.arrow == initial.arrow,
            "property edits preserve other distance settings and independent arrow defaults");
    SnowCanvasWidget canvas;
    applyScreenshotCanvasToolStyles(canvas, restored);
    require(canvas.setCanvasTool(SnowCanvasTool::Distance) &&
                canvas.canvasDistanceStyle() == changed,
            "a new canvas restores distance creation defaults");
}

void distanceToolsParticipateInConfiguration() {
    using namespace snow_shot;
    const auto positions = presentation::toolbar_layout::defaultPositions();
    require(positions.contains(
                {QStringLiteral("distance"), QStringLiteral("line"), QStringLiteral("arrow")}),
            "distance joins the arrow and line stack without changing its arrow trigger");
    const auto defaults = storage::DrawingShortcutSettings().allShortcuts();
    require(defaults.contains(QStringLiteral("distance")) &&
                defaults.value(QStringLiteral("distance")).isEmpty(),
            "distance shortcuts start unassigned");
    require(storage::DrawingShortcutSettings().setShortcuts(QStringLiteral("distance"),
                                                            {QStringLiteral("Ctrl+Alt+F12")}),
            "distance supports user shortcuts");
    require(presentation::screenshotQuickSelectionDisabledTools({QStringLiteral("distance")})
                .contains(SnowCanvasTool::Distance),
            "distance participates in quick-selection exclusions");
    storage::ScreenshotToolbarSettings toolbar;
    constexpr auto kind = storage::ScreenshotToolbarLayoutKind::DrawingTools;
    auto previousDefaults = positions;
    QJsonArray savedPositions;
    for (auto& position : previousDefaults) {
        position.removeAll(QStringLiteral("distance"));
        savedPositions.append(QJsonArray::fromStringList(position));
    }
    require(storage::ApplicationStorage::instance().configuration().setValue(
                QStringLiteral("screenshot_toolbar/layout"),
                QJsonObject{{QStringLiteral("positions"), savedPositions},
                            {QStringLiteral("hidden"), QJsonArray{}}}) &&
                toolbar.layout(kind).positions == positions,
            "an unchanged saved default layout upgrades the arrow and line stack");
    QJsonArray defaultsWithHiddenTool;
    for (const auto& position : previousDefaults) {
        auto visible = position;
        visible.removeAll(QStringLiteral("eraser"));
        if (!visible.isEmpty())
            defaultsWithHiddenTool.append(QJsonArray::fromStringList(visible));
    }
    require(
        storage::ApplicationStorage::instance().configuration().setValue(
            QStringLiteral("screenshot_toolbar/layout"),
            QJsonObject{{QStringLiteral("positions"), defaultsWithHiddenTool},
                        {QStringLiteral("hidden"), QJsonArray{QStringLiteral("eraser")}}}) &&
            toolbar.layout(kind).positions.contains(
                {QStringLiteral("distance"), QStringLiteral("line"), QStringLiteral("arrow")}) &&
            toolbar.layout(kind).hidden == QStringList{QStringLiteral("eraser")},
        "a previous default layout upgrades its stack while preserving another hidden tool");
    auto customized = toolbar.layout(kind);
    for (auto& position : customized.positions)
        position.removeAll(QStringLiteral("distance"));
    customized.hidden.append(QStringLiteral("distance"));
    require(toolbar.setLayout(kind, customized) &&
                toolbar.layout(kind).hidden.contains(QStringLiteral("distance")),
            "custom layouts retain a deliberately hidden distance tool");
}

void sourcePixelCalibrationBelongsToAnnotations() {
    using namespace snow_shot::app::mcp;
    QImage raster(240, 120, QImage::Format_ARGB32_Premultiplied);
    raster.fill(Qt::white);
    const QRectF rect(10, 20, 120, 60);
    require(mcpDistancePixelScale({{raster, rect}}, rect) == QSizeF(2, 2),
            "pixel measurement uses the source raster mapping including any baked margins");
    QImage second(30, 20, QImage::Format_ARGB32_Premultiplied);
    require(mcpDistancePixelScale({{raster, rect}, {second, QRectF(200, 20, 10, 10)}},
                                  QRectF(10, 20, 200, 60)) == QSizeF(3, 2),
            "mixed-source calibration retains independent horizontal and vertical density");
    const QJsonObject distance{
        {QStringLiteral("type"), QStringLiteral("distance")},
        {QStringLiteral("points"), QJsonArray{QJsonArray{0, 0}, QJsonArray{3, 4}}}};
    const QJsonObject rectangle{{QStringLiteral("type"), QStringLiteral("rectangle")}};
    const auto batch = mcpDistanceAnnotationsWithPixelScale(
        {{QStringLiteral("operations"), QJsonArray{distance, rectangle}}}, QSizeF(2, 3));
    const auto operations = batch.value(QStringLiteral("operations")).toArray();
    require(operations[0].toObject().value(QStringLiteral("pixel_scale")) == QJsonArray{2, 3} &&
                !operations[1].toObject().contains(QStringLiteral("pixel_scale")),
            "only distance annotations receive the host's stable image calibration");
    require(mcpDistancePixelScale({}, rect) == QSizeF(1, 1),
            "an empty global canvas measures virtual canvas pixels");
}

void distanceMcpStyleValidationMatchesControls() {
    using namespace snow_shot::app::mcp;
    SnowCanvasRuntime runtime;
    SnowCanvasRuntimeEditor editor(runtime, SnowCanvasTool::Distance);
    require(editor.isValid(), "create a headless distance editor");
    const auto apply = [&](const QJsonObject& style) {
        return mcpStylePatch(editor, editor,
                             {{QStringLiteral("target"), QStringLiteral("distance")},
                              {QStringLiteral("style"), style}});
    };
    require(apply({{QStringLiteral("factor"), 1000},
                   {QStringLiteral("unit"), QStringLiteral("km")},
                   {QStringLiteral("decimal_places"), 3},
                   {QStringLiteral("endpoint_scale"), 3},
                   {QStringLiteral("endpoint_style"), QStringLiteral("crowfoot_one_or_many")}}),
            "MCP accepts the supported distance settings and existing endpoint styles");
    const auto accepted = editor.canvasStyleToolbarState().distanceStyle;
    require(accepted.factor == 1000 && accepted.unit == SnowCanvasDistanceUnit::Km &&
                accepted.decimalPlaces == 3 && accepted.endpointScale == 3 &&
                accepted.endpointStyle == SnowCanvasArrowhead::CrowfootOneOrMany,
            "MCP style patches reach distance creation defaults");
    require(apply({{QStringLiteral("unit"), QStringLiteral("mm")}}) &&
                editor.canvasStyleToolbarState().distanceStyle.unit == SnowCanvasDistanceUnit::Mm,
            "MCP accepts millimeter distance labels");
    const auto millimeters = editor.canvasStyleToolbarState().distanceStyle;
    for (const auto& invalid :
         {QJsonObject{{QStringLiteral("factor"), 0}},
          QJsonObject{{QStringLiteral("factor"), 1000.1}},
          QJsonObject{{QStringLiteral("decimal_places"), 1.5}},
          QJsonObject{{QStringLiteral("decimal_places"), 4}},
          QJsonObject{{QStringLiteral("endpoint_scale"), 0.4}},
          QJsonObject{{QStringLiteral("unit"), QStringLiteral("unsupported")}},
          QJsonObject{{QStringLiteral("stroke_width"), 0}},
          QJsonObject{{QStringLiteral("fill"), QJsonArray{0, 0, 0, 255}}}}) {
        require(!apply(invalid) && editor.canvasStyleToolbarState().distanceStyle == millimeters,
                "invalid distance settings reject atomically without changing defaults");
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid() && QDir().mkpath(directory.filePath(QStringLiteral("bin"))),
            "create isolated storage");
    require(snow_shot::storage::ApplicationStorage::instance()
                .initialize({directory.filePath(QStringLiteral("bin")), directory.path(), 60000})
                .success,
            "initialize isolated distance test configuration");
    persistedDefaultsMergeDistanceProperties();
    distanceToolsParticipateInConfiguration();
    sourcePixelCalibrationBelongsToAnnotations();
    distanceMcpStyleValidationMatchesControls();
    snow_shot::storage::ApplicationStorage::instance().shutdown();
    return 0;
}
