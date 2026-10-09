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

void persistedAngleDefaultsAreIndependent() {
    using namespace snow_shot::presentation;
    const auto initial = screenshotCanvasStyleDefaults();
    require(initial.angle.stroke == initial.arrow.stroke && initial.angle.strokeWidth == 2 &&
                initial.angle.unit == SnowCanvasAngleUnit::Degrees &&
                initial.angle.decimalPlaces == 0,
            "angle defaults use the drawing color and integer degree labels");
    require(screenshotCanvasToolStyleDefaults().angle == initial.angle,
            "empty configuration restores angle defaults");
    auto changed = initial.angle;
    changed.stroke = QColor(17, 89, 141, 201);
    changed.unit = SnowCanvasAngleUnit::Radians;
    changed.decimalPlaces = 3;
    require(persistScreenshotCanvasStyleEdit(SnowCanvasAngleStyleEdit{
                changed, static_cast<quint32>(SnowCanvasAngleStyleProperty::All)}),
            "persist angle creation defaults");
    auto widthOnly = initial.angle;
    widthOnly.strokeWidth = 6;
    require(persistScreenshotCanvasStyleEdit(SnowCanvasAngleStyleEdit{
                widthOnly, static_cast<quint32>(SnowCanvasAngleStyleProperty::StrokeWidth)}),
            "merge angle width against the latest stored defaults");
    changed.strokeWidth = 6;
    const auto restored = screenshotCanvasToolStyleDefaults();
    require(restored.angle == changed && restored.arrow == initial.arrow &&
                restored.distance == initial.distance,
            "angle property patches preserve independent arrow and distance defaults");
    require(persistScreenshotCanvasStyleEdit(SnowCanvasAngleStyleEdit{
                initial.angle, static_cast<quint32>(SnowCanvasAngleStyleProperty::All), false}) &&
                screenshotCanvasToolStyleDefaults().angle == changed,
            "selected angle edits never overwrite creation defaults");
    SnowCanvasWidget canvas;
    applyScreenshotCanvasToolStyles(canvas, restored);
    require(canvas.setCanvasTool(SnowCanvasTool::Angle) && canvas.canvasAngleStyle() == changed,
            "new editors restore angle creation defaults");
}

void toolConfigurationUpgradesDefaultsAndPreservesCustomLayouts() {
    using namespace snow_shot;
    const auto positions = presentation::toolbar_layout::defaultPositions();
    const QStringList stack{QStringLiteral("angle"), QStringLiteral("distance"),
                            QStringLiteral("line"), QStringLiteral("arrow")};
    require(positions.contains(stack), "angle joins the existing stack with the arrow trigger");
    const auto shortcuts = storage::DrawingShortcutSettings().allShortcuts();
    require(shortcuts.contains(QStringLiteral("angle")) &&
                shortcuts.value(QStringLiteral("angle")).isEmpty(),
            "the angle shortcut starts unassigned");
    require(storage::DrawingShortcutSettings().setShortcuts(QStringLiteral("angle"),
                                                            {QStringLiteral("Ctrl+Alt+F11")}),
            "angle shortcuts can be assigned");
    require(presentation::screenshotQuickSelectionDisabledTools({QStringLiteral("angle")})
                .contains(SnowCanvasTool::Angle),
            "angle participates in quick-selection exclusions");
    storage::ScreenshotToolbarSettings toolbar;
    constexpr auto kind = storage::ScreenshotToolbarLayoutKind::DrawingTools;
    storage::ScreenshotToolbarLayout previous;
    previous.positions = positions;
    for (auto& position : previous.positions)
        position.removeAll(QStringLiteral("angle"));
    require(presentation::toolbar_layout::normalizedLayout(previous).positions == positions,
            "the model upgrades previous drawing defaults in place");
    QJsonArray saved;
    for (const auto& position : previous.positions)
        saved.append(QJsonArray::fromStringList(position));
    require(storage::ApplicationStorage::instance().configuration().setValue(
                QStringLiteral("screenshot_toolbar/layout"),
                QJsonObject{{QStringLiteral("positions"), saved},
                            {QStringLiteral("hidden"), QJsonArray{}}}) &&
                toolbar.layout(kind).positions == positions,
            "persisted previous defaults acquire the angle in the existing stack");
    previous.hidden.append(QStringLiteral("angle"));
    require(toolbar.setLayout(kind, previous) &&
                toolbar.layout(kind).hidden.contains(QStringLiteral("angle")) &&
                toolbar.layout(kind).positions == previous.positions,
            "a deliberately hidden angle remains hidden");
    auto custom = previous;
    custom.hidden.clear();
    custom.positions.swapItemsAt(2, 3);
    const auto normalizedCustom = presentation::toolbar_layout::normalizedLayout(custom);
    require(normalizedCustom.positions.sliced(0, custom.positions.size()) == custom.positions &&
                normalizedCustom.positions.last() == QStringList{QStringLiteral("angle")} &&
                toolbar.setLayout(kind, custom) &&
                toolbar.layout(kind).positions == normalizedCustom.positions,
            "custom toolbar arrangements keep their stacks and append the new angle separately");
    auto legacy = positions;
    for (auto& position : legacy) {
        position.removeAll(QStringLiteral("angle"));
        position.removeAll(QStringLiteral("distance"));
    }
    require(presentation::toolbar_layout::normalizedLayout({legacy, {}}).positions == positions,
            "older defaults acquire both annotations in the existing arrow stack");
    require(toolbar.setLastDrawingTool(QStringLiteral("angle")) &&
                toolbar.lastDrawingTool() == QStringLiteral("angle"),
            "remembered drawing tool accepts the angle tool");
}

void angleMcpStyleValidationMatchesTheControls() {
    using namespace snow_shot::app::mcp;
    SnowCanvasRuntime runtime;
    SnowCanvasRuntimeEditor editor(runtime, SnowCanvasTool::Angle);
    require(editor.isValid(), "create a headless angle editor");
    const auto apply = [&](const QJsonObject& style) {
        return mcpStylePatch(editor, editor,
                             {{QStringLiteral("target"), QStringLiteral("angle")},
                              {QStringLiteral("style"), style}});
    };
    require(apply({{QStringLiteral("stroke_width"), 72},
                   {QStringLiteral("unit"), QStringLiteral("radians")},
                   {QStringLiteral("decimal_places"), 3}}),
            "MCP accepts all angle controls");
    const auto accepted = editor.canvasStyleToolbarState().angleStyle;
    require(accepted.strokeWidth == 72 && accepted.unit == SnowCanvasAngleUnit::Radians &&
                accepted.decimalPlaces == 3,
            "angle MCP styles reach creation defaults");
    for (const auto& invalid : {QJsonObject{{QStringLiteral("unit"), QStringLiteral("cm")}},
                                QJsonObject{{QStringLiteral("decimal_places"), 1.5}},
                                QJsonObject{{QStringLiteral("decimal_places"), 4}},
                                QJsonObject{{QStringLiteral("stroke_width"), 0}},
                                QJsonObject{{QStringLiteral("stroke_width"), 73}},
                                QJsonObject{{QStringLiteral("fill"), QJsonArray{0, 0, 0, 255}}},
                                QJsonObject{{QStringLiteral("factor"), 2}}}) {
        require(!apply(invalid) && editor.canvasStyleToolbarState().angleStyle == accepted,
                "unsupported angle styles reject atomically");
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid() && QDir().mkpath(directory.filePath(QStringLiteral("bin"))),
            "create isolated angle configuration");
    require(snow_shot::storage::ApplicationStorage::instance()
                .initialize({directory.filePath(QStringLiteral("bin")), directory.path(), 60000})
                .success,
            "initialize isolated angle storage");
    persistedAngleDefaultsAreIndependent();
    toolConfigurationUpgradesDefaultsAndPreservesCustomLayouts();
    angleMcpStyleValidationMatchesTheControls();
    snow_shot::storage::ApplicationStorage::instance().shutdown();
    return 0;
}
