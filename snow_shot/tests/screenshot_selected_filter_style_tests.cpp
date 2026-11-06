#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshotstylebinding.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "widgets/button.h"
#include "widgets/select.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QScopeGuard>
#include <cstdlib>
#include <iostream>

namespace {
using namespace snow_shot::presentation;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << std::endl;
        std::exit(1);
    }
}

void mouse(SnowCanvasWidget& canvas, QEvent::Type type, QPointF point,
           Qt::MouseButton button = Qt::LeftButton,
           Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent event(type, point, canvas.mapToGlobal(point.toPoint()),
                      type == QEvent::MouseMove ? Qt::NoButton : button,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : button, modifiers);
    QApplication::sendEvent(&canvas, &event);
}

void drag(SnowCanvasWidget& canvas, QPointF from, QPointF to) {
    mouse(canvas, QEvent::MouseButtonPress, from);
    mouse(canvas, QEvent::MouseMove, to);
    mouse(canvas, QEvent::MouseButtonRelease, to);
}

QJsonObject lastElementData(const SnowCanvasRuntime& runtime, const QString& kind) {
    return QJsonDocument::fromJson(runtime.serializeDocumentSession())
        .object()
        .value(QStringLiteral("document"))
        .toObject()
        .value(QStringLiteral("slots"))
        .toArray()
        .last()
        .toObject()
        .value(QStringLiteral("data"))
        .toObject()
        .value(kind)
        .toObject();
}

void selectedFiltersRememberOnlyTheirCreationFamily(bool pen) {
    const auto original = screenshotCanvasToolStyleDefaults();
    const auto restore =
        qScopeGuard([&] { static_cast<void>(persistScreenshotCanvasToolStyles(original)); });
    auto expected = original;
    expected.rectangleFilter = {SnowCanvasFilterType::Mosaic, 0.5, 0.8, 4};
    expected.penFilter = {SnowCanvasFilterType::GaussianBlur, 0.5, 0.6, 12};
    require(persistScreenshotCanvasToolStyles(expected), "save distinct filter family defaults");

    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    ScreenshotToolPalette::Options options;
    options.showFilterTool = true;
    options.styleDefaults = expected;
    ScreenshotToolPalette palette(options);
    ScreenshotStyleBinding binding(palette, canvas, &palette);
    QObject::connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, &palette,
                     [&] { palette.setStyleToolbarState(canvas.canvasStyleToolbarState()); });
    applyScreenshotCanvasToolStyles(canvas, expected);
    canvas.resize(400, 300);
    canvas.show();
    require(canvas.setViewportCamera(200, 150, 1), "prepare filter fixture viewport");
    const auto tool = pen ? SnowCanvasTool::PenFilter : SnowCanvasTool::RectangleFilter;
    require(canvas.setCanvasTool(tool), "activate filter creation fixture");
    drag(canvas, {80, 100}, {180, 160});
    palette.setActiveTool(pen ? ScreenshotToolPalette::Tool::BrushFilter
                              : ScreenshotToolPalette::Tool::RectangleFilter);
    mouse(canvas, QEvent::MouseButtonPress, {130, 130}, Qt::RightButton);
    mouse(canvas, QEvent::MouseButtonRelease, {130, 130}, Qt::RightButton);
    const auto selectedSource = pen ? SnowCanvasStyleToolbarSource::SelectedPenFilter
                                    : SnowCanvasStyleToolbarSource::SelectedRectangleFilter;
    require(canvas.canvasStyleToolbarState().source == selectedSource,
            "filter quick selection exposes its corresponding style toolbar");
    palette.setStyleToolbarState(canvas.canvasStyleToolbarState());
    palette.show();
    QApplication::processEvents();

    auto& edited = pen ? expected.penFilter : expected.rectangleFilter;
    const auto verify = [&] {
        const auto state = canvas.canvasStyleToolbarState();
        require(state.source == selectedSource && state.filterStyle.type == edited.type &&
                    state.filterStyle.opacity == edited.opacity &&
                    (!pen || state.filterStyle.strokeWidth == edited.strokeWidth),
                "selected filter receives the requested appearance patch");
        require(palette.creationStyleDefaults() == expected,
                "selected filter edits remember only the corresponding palette defaults");
        require(screenshotCanvasToolStyleDefaults() == expected,
                "selected filter edits persist only the corresponding filter family");
    };

    if (pen) {
        auto* width = palette.findChild<adqt::widgets::AdButton*>(
            QStringLiteral("screenshotPenFilterStrokeWidth54"));
        require(width != nullptr, "selected pen filter exposes the stroke width preset");
        width->click();
        edited.strokeWidth = 54;
        verify();
    }
    auto* type = palette.findChild<adqt::widgets::AdSelect*>(
        pen ? QStringLiteral("screenshotPenFilterTypeSelect")
            : QStringLiteral("screenshotFilterTypeSelect"));
    require(type != nullptr, "selected filter exposes its type editor");
    type->setCurrentData(static_cast<int>(SnowCanvasFilterType::Inversion),
                         adqt::widgets::AdSelect::DefaultValueRole);
    edited.type = SnowCanvasFilterType::Inversion;
    verify();

    require(canvas.setCanvasTool(SnowCanvasTool::Select),
            "switch to Select for direct appearance-edit context resolution");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    mouse(canvas, QEvent::MouseButtonPress, {130, 130});
    mouse(canvas, QEvent::MouseButtonRelease, {130, 130});
    auto style = canvas.canvasStyleToolbarState().filterStyle;
    style.opacity = 0.35;
    require(canvas.commitStyleEdit(
                SnowCanvasFilterEdit{style, SnowCanvasFilterStylePropertyOpacity, !pen}),
            "selected filter opacity patches resolve their family from current selection");
    edited.opacity = style.opacity;
    verify();

    require(canvas.resetEditingState() && canvas.setCanvasTool(tool),
            "leave filter selection and inspect subsequent creation defaults");
    require(canvas.canvasStyleToolbarState().filterStyle == edited,
            "subsequent filter creation uses every selected appearance change");
    drag(canvas, {250, 200}, {340, 250});
    const auto created =
        lastElementData(runtime, pen ? QStringLiteral("PenFilter") : QStringLiteral("Filter"));
    require(created.value(QStringLiteral("filter_type")).toString() ==
                    QStringLiteral("Inversion") &&
                created.value(QStringLiteral("opacity")).toDouble() == edited.opacity &&
                (!pen ||
                 created.value(QStringLiteral("stroke_width")).toDouble() == edited.strokeWidth),
            "new filter elements inherit the edited type, opacity and pen stroke width");
    require(canvas.resetEditingState() && canvas.setCanvasTool(pen ? SnowCanvasTool::RectangleFilter
                                                                   : SnowCanvasTool::PenFilter),
            "inspect the other filter creation family");
    require(canvas.canvasStyleToolbarState().filterStyle ==
                (pen ? expected.rectangleFilter : expected.penFilter),
            "selected filter patches preserve the other family's session defaults");
}

void mixedFilterSelectionRemembersEveryAffectedFamily(bool penPrimary) {
    const auto original = screenshotCanvasToolStyleDefaults();
    const auto restore =
        qScopeGuard([&] { static_cast<void>(persistScreenshotCanvasToolStyles(original)); });
    auto expected = original;
    expected.rectangleFilter = {SnowCanvasFilterType::Mosaic, 0.5, 0.8, 4};
    expected.penFilter = {SnowCanvasFilterType::GaussianBlur, 0.5, 0.6, 12};
    require(persistScreenshotCanvasToolStyles(expected), "save mixed filter fixture defaults");
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    ScreenshotToolPalette::Options options;
    options.showFilterTool = true;
    options.styleDefaults = expected;
    ScreenshotToolPalette palette(options);
    ScreenshotStyleBinding binding(palette, canvas, &palette);
    QObject::connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, &palette,
                     [&] { palette.setStyleToolbarState(canvas.canvasStyleToolbarState()); });
    applyScreenshotCanvasToolStyles(canvas, expected);
    canvas.resize(400, 300);
    canvas.show();
    require(canvas.setViewportCamera(200, 150, 1) &&
                canvas.setCanvasTool(SnowCanvasTool::RectangleFilter),
            "prepare mixed filter rectangle fixture");
    drag(canvas, {70, 70}, {130, 130});
    require(canvas.setCanvasTool(SnowCanvasTool::PenFilter), "prepare mixed pen filter fixture");
    drag(canvas, {250, 80}, {330, 140});
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "select both filter families");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    const QPointF first = penPrimary ? QPointF(100, 100) : QPointF(290, 110);
    const QPointF second = penPrimary ? QPointF(290, 110) : QPointF(100, 100);
    mouse(canvas, QEvent::MouseButtonPress, first);
    mouse(canvas, QEvent::MouseButtonRelease, first);
    mouse(canvas, QEvent::MouseButtonPress, second, Qt::LeftButton, Qt::ShiftModifier);
    mouse(canvas, QEvent::MouseButtonRelease, second, Qt::LeftButton, Qt::ShiftModifier);
    const auto selected = canvas.canvasStyleToolbarState();
    require(selected.selectedElementCount == 2 &&
                selected.source == (penPrimary
                                        ? SnowCanvasStyleToolbarSource::SelectedPenFilter
                                        : SnowCanvasStyleToolbarSource::SelectedRectangleFilter) &&
                (selected.filterStyleMixed & SnowCanvasFilterStyleMixedContainsBothFamilies) != 0,
            "mixed filter selection reports both creation families to the toolbar");
    palette.setStyleToolbarState(selected);
    palette.show();
    QApplication::processEvents();

    const auto verify = [&] {
        require(palette.creationStyleDefaults() == expected &&
                    screenshotCanvasToolStyleDefaults() == expected,
                "mixed filter edits remember and persist only properties of affected families");
    };
    auto style = canvas.canvasStyleToolbarState().filterStyle;
    style.strokeWidth = 54;
    require(canvas.commitStyleEdit(
                SnowCanvasFilterEdit{style, SnowCanvasFilterStylePropertyStrokeWidth, !penPrimary}),
            "mixed selected filter stroke edits resolve pen width regardless of the primary");
    expected.penFilter.strokeWidth = 54;
    verify();
    style = canvas.canvasStyleToolbarState().filterStyle;
    style.type = SnowCanvasFilterType::Inversion;
    require(canvas.commitStyleEdit(
                SnowCanvasFilterEdit{style, SnowCanvasFilterStylePropertyType, !penPrimary}),
            "mixed selected filter type edits resolve every selected family");
    expected.rectangleFilter.type = SnowCanvasFilterType::Inversion;
    expected.penFilter.type = SnowCanvasFilterType::Inversion;
    verify();
    style = canvas.canvasStyleToolbarState().filterStyle;
    style.opacity = 0.35;
    require(
        canvas.commitStyleEdit(SnowCanvasFilterEdit{style, SnowCanvasFilterStylePropertyOpacity}),
        "mixed selected filter opacity edits resolve every selected family");
    expected.rectangleFilter.opacity = style.opacity;
    expected.penFilter.opacity = style.opacity;
    verify();

    require(canvas.resetEditingState() && canvas.setCanvasTool(SnowCanvasTool::RectangleFilter) &&
                canvas.canvasStyleToolbarState().filterStyle == expected.rectangleFilter,
            "mixed edits update rectangle defaults without changing its unused pen width");
    drag(canvas, {70, 210}, {130, 270});
    const auto rectangle = lastElementData(runtime, QStringLiteral("Filter"));
    require(rectangle.value(QStringLiteral("filter_type")).toString() ==
                    QStringLiteral("Inversion") &&
                rectangle.value(QStringLiteral("opacity")).toDouble() == 0.35,
            "new rectangle filters inherit mixed selection appearance edits");
    require(canvas.resetEditingState() && canvas.setCanvasTool(SnowCanvasTool::PenFilter) &&
                canvas.canvasStyleToolbarState().filterStyle == expected.penFilter,
            "mixed edits update pen filter session defaults");
    drag(canvas, {250, 210}, {330, 270});
    const auto pen = lastElementData(runtime, QStringLiteral("PenFilter"));
    require(pen.value(QStringLiteral("filter_type")).toString() == QStringLiteral("Inversion") &&
                pen.value(QStringLiteral("opacity")).toDouble() == 0.35 &&
                pen.value(QStringLiteral("stroke_width")).toDouble() == 54,
            "new pen filters inherit mixed selection style and their own stroke width");
}
} // namespace

void runScreenshotSelectedFilterStyleTests() {
    selectedFiltersRememberOnlyTheirCreationFamily(true);
    selectedFiltersRememberOnlyTheirCreationFamily(false);
    mixedFilterSelectionRemembersEveryAffectedFamily(true);
    mixedFilterSelectionRemembersEveryAffectedFamily(false);
}
