#include "snow_shot/app/mcp/mcpstylepatch.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/screenshottoolbarlayoutmodel.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include <QApplication>
#include <QDir>
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
void integration() {
    using namespace snow_shot;
    using namespace presentation;
    const auto defaults = screenshotCanvasToolStyleDefaults();
    auto style = defaults.bracket;
    style.stroke = QColor(20, 70, 190, 160);
    style.strokeWidth = 6;
    require(persistScreenshotCanvasStyleEdit(SnowCanvasBracketStyleEdit{style, 3}),
            "persist independent bracket defaults");
    const auto restored = screenshotCanvasToolStyleDefaults();
    require(restored.bracket == style && restored.angle == defaults.angle &&
                restored.arrow == defaults.arrow,
            "other tool defaults are unchanged");
    const QStringList stack{"bracket", "angle", "distance", "line", "arrow"};
    const auto positions = toolbar_layout::defaultPositions();
    require(positions.contains(stack), "bracket shares the arrow stack");
    const auto shown =
        toolbar_layout::stackPresentation(stack, [](const QString&) { return true; });
    require(shown.popoverItemIds == QStringList{"arrow", "line", "distance", "angle", "bracket"},
            "bracket is immediately right of angle");
    storage::ScreenshotToolbarLayout previous{positions, {}};
    for (auto& position : previous.positions)
        position.removeAll("bracket");
    require(toolbar_layout::normalizedLayout(previous).positions == positions,
            "previous default layout is upgraded");
    auto custom = previous;
    custom.positions.swapItemsAt(2, 3);
    const auto upgraded = toolbar_layout::normalizedLayout(custom);
    require(upgraded.positions.sliced(0, custom.positions.size()) == custom.positions &&
                upgraded.positions.last() == QStringList{"bracket"},
            "custom stacks keep their arrangement");
    storage::ScreenshotToolbarSettings toolbar;
    constexpr auto kind = storage::ScreenshotToolbarLayoutKind::DrawingTools;
    require(toolbar.setLayout(kind, previous) && toolbar.layout(kind).positions == positions,
            "stored default layouts upgrade in the configuration normalizer");
    require(toolbar.setLayout(kind, custom) && toolbar.layout(kind).positions == upgraded.positions,
            "stored custom layouts append bracket and preserve stacks");
    previous.hidden.append("bracket");
    require(toolbar_layout::normalizedLayout(previous).hidden.contains("bracket"),
            "hidden bracket stays hidden");
    require(toolbar.setLayout(kind, previous) && toolbar.layout(kind).hidden.contains("bracket"),
            "configuration preserves explicit hiding");
    require(toolbar.setLastDrawingTool("bracket") && toolbar.lastDrawingTool() == "bracket",
            "bracket can be remembered across hosts");
    const auto shortcuts = storage::DrawingShortcutSettings().allShortcuts();
    require(shortcuts.contains("bracket") && shortcuts.value("bracket").isEmpty(),
            "bracket shortcut is initially unassigned");
    require(storage::DrawingShortcutSettings().setShortcuts(QStringLiteral("bracket"),
                                                            {QStringLiteral("Ctrl+Alt+F10")}),
            "bracket accepts an assigned shortcut");
    require(screenshotQuickSelectionDisabledTools({"bracket"}).contains(SnowCanvasTool::Bracket),
            "quick selection policy supports bracket");
    SnowCanvasRuntime runtime;
    SnowCanvasRuntimeEditor editor(runtime, SnowCanvasTool::Bracket);
    require(editor.isValid(), "headless bracket editor");
    const auto patch = [&](const QJsonObject& value) {
        return app::mcp::mcpStylePatch(editor, editor, {{"target", "bracket"}, {"style", value}});
    };
    require(patch({{"stroke_width", 72}}) &&
                editor.canvasStyleToolbarState().bracketStyle.strokeWidth == 72,
            "MCP style reaches bracket defaults");
    for (const auto& invalid :
         {QJsonObject{{"stroke_width", 0}}, QJsonObject{{"stroke_width", 73}},
          QJsonObject{{"unit", "degrees"}}, QJsonObject{{"fill", QJsonArray{0, 0, 0, 255}}}})
        require(!patch(invalid), "invalid bracket properties reject atomically");
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir dir;
    require(dir.isValid() && QDir().mkpath(dir.filePath("bin")), "temporary settings directory");
    require(snow_shot::storage::ApplicationStorage::instance()
                .initialize({dir.filePath("bin"), dir.path(), 60000})
                .success,
            "initialize settings");
    integration();
    snow_shot::storage::ApplicationStorage::instance().shutdown();
    return 0;
}
