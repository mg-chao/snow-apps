#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_shot/storage/persistedselectioncodec.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QPainterPathStroker>
#include <QPainter>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void apply(ScreenshotSelectionModel& model, ScreenshotSelectionModel::RegionOperation operation,
           const QRect& rectangle) {
    const QRegion before = model.selectionRegion();
    model.beginRegionOperation(operation);
    require(model.confirmedRegion() == before, "entry must preserve confirmed geometry");
    model.setSelectionRect(rectangle);
    const auto preview = model.selectionRegion();
    model.commitRegionOperation();
    require(model.selectionRegion() == preview, "committed geometry must equal preview");
}
void geometryAndTransactions() {
    using Operation = ScreenshotSelectionModel::RegionOperation;
    ScreenshotSelectionModel model;
    model.setSelectionRect(QRect(10, 10, 80, 60));
    apply(model, Operation::Add, QRect(70, 10, 50, 60));
    require(model.rectangular() && model.pixelSelection() == QRect(10, 10, 110, 60),
            "overlapping rectangles should restore rectangular resizing");
    apply(model, Operation::Add, QRect(120, 10, 10, 60));
    require(model.rectangular(), "adjacent rectangles must merge without seams");
    apply(model, Operation::Add, QRect(30, 20, 10, 10));
    require(model.rectangular(), "contained additions must be no-ops");
    apply(model, Operation::Subtract, QRect(40, 25, 30, 20));
    require(!model.rectangular() && !model.selectionRegion().contains(QPoint(50, 30)),
            "internal subtraction must leave a hole");
    const auto hole = model.selectionRegion();
    model.beginRegionOperation(Operation::Add);
    require(model.selectionRegion() == hole, "entry without a marquee must not change mask");
    model.setSelectionRect(QRect(40, 25, 30, 20));
    require(model.selectionRegion() == QRegion(QRect(10, 10, 120, 60)), "hole fill preview");
    model.cancelRegionOperation();
    require(model.selectionRegion() == hole, "cancel must restore original shape");
    model.beginMoveDrag(QPointF(10, 10));
    model.setDraggedSelectionRect(QRectF(30, 40, 120, 60), ScreenshotSelectionDragMode::All);
    require(model.selectionRegion() == hole.translated(20, 30), "move must preserve holes");
    require(!model.adjustFromToolbar(0, 0, 1, 0, QRectF(0, 0, 300, 300), 1),
            "toolbar must reject complex resizing");
    require(model.adjustFromToolbar(1, 0, 1, 0, QRectF(0, 0, 300, 300), 1),
            "toolbar translation must remain available");
    apply(model, Operation::Subtract, QRect(0, 0, 300, 300));
    require(model.selectionRegion().isEmpty() && !model.hasPixelSelection(),
            "complete subtraction");
    model.setSelectionStartEnd(QPointF(5, 6), QPointF(6, 6));
    require(model.pixelSelection() == QRect(5, 6, 2, 1), "manual pointer cells stay inclusive");
    apply(model, Operation::Add, QRect(50, 50, 12, 13));
    require(model.selectionRegion().rectCount() == 2, "disconnected region");
    apply(model, Operation::Subtract, QRect(100, 100, 5, 5));
    require(model.selectionRegion().rectCount() == 2, "outside subtraction is a no-op");
    ScreenshotSelectionParams preset;
    preset.selection = QRect(20, 20, 40, 40);
    require(model.applyParams(preset, QRect(0, 0, 300, 300)) && model.rectangular(),
            "preset must replace complex selection");
    apply(model, Operation::Subtract, QRect(30, 0, 10, 100));
    require(!model.rectangular() && !model.selectionRegion().contains(QPoint(35, 35)),
            "subtraction must split a region");
}
void outlinesAndEffects() {
    const QRegion shape = QRegion(QRect(0, 0, 100, 80)).subtracted(QRect(30, 20, 40, 40));
    const auto path = screenshotRegionPath(shape);
    require(path.contains(QPointF(10, 10)) && !path.contains(QPointF(50, 40)), "outline holes");
    QPainterPathStroker stroker;
    stroker.setWidth(1);
    require(!stroker.createStroke(path).contains(QPointF(10, 20)), "no scanline rectangle seams");
    const auto rounded = screenshotRegionPath(shape, 8);
    require(!rounded.contains(QPointF(0.5, 0.5)) && rounded.contains(QPointF(10, 10)),
            "rounded outer corners");
    require(!rounded.contains(QPointF(50, 40)), "rounding must preserve hole");
    QImage content(100, 80, QImage::Format_ARGB32_Premultiplied);
    content.fill(Qt::red);
    ScreenshotResultStyle style;
    style.region = shape;
    const auto output = ScreenshotResultCompositor::compose(content, style);
    require(output.pixelColor(50, 40).alpha() == 0, "cutout must not export screenshot pixels");
    require(output.pixelColor(10, 10) == QColor(Qt::red), "selected pixels unchanged");
    style.cornerRadius = 8;
    style.shadowWidth = 6;
    const auto effected = ScreenshotResultCompositor::compose(content, style);
    require(effected.size() == QSize(112, 92), "shadow padding");
    require(effected.pixelColor(56, 46).alpha() == 0, "hole center remains transparent");
    require(effected.pixelColor(37, 46).alpha() > 0, "shadow follows internal cutout edge");
    QImage annotated = effected;
    {
        QPainter painter(&annotated);
        painter.fillRect(QRect(42, 35, 20, 20), Qt::magenta);
    }
    auto bakedPath = screenshotRegionPath(shape, style.cornerRadius);
    bakedPath.translate(6, 6);
    ScreenshotResultCompositor::restoreBakedExterior(annotated, effected, bakedPath);
    require(annotated.pixelColor(56, 46).alpha() == 0 &&
                annotated.pixelColor(37, 46) == effected.pixelColor(37, 46),
            "pinned edits must preserve transparent cutouts and baked shadows");
    const auto repeated = ScreenshotResultCompositor::compose(content, style);
    require(repeated == effected, "cached effects must match initial composition");
    style.regionScale = 2;
    style.cornerRadius *= 2;
    style.shadowWidth *= 2;
    const auto scaled = ScreenshotResultCompositor::compose(content.scaled(200, 160), style);
    require(scaled.pixelColor(112, 92).alpha() == 0 && scaled.pixelColor(32, 32).red() == 255,
            "geometry must scale with backing resolution");
    const QRegion thin = QRegion(QRect(0, 0, 100, 2)).united(QRect(49, 0, 2, 80));
    const auto narrowPath = screenshotRegionPath(thin, 256);
    require(narrowPath.contains(QPointF(50, 40)), "large radius must preserve narrow bridge");
}
void persistenceAndHandlePolicy() {
    using namespace snow_shot::storage;
    PersistedSelection saved;
    saved.rectangle = QRect(0, 0, 100, 100);
    saved.shadowColor = Qt::black;
    saved.region = QRegion(saved.rectangle).subtracted(QRect(20, 20, 40, 40));
    const auto decoded = normalizePersistedSelection(persistedSelectionToJson(saved));
    require(decoded.valid && decoded.value == saved, "region codec round trip");
    auto legacy = persistedSelectionToJson(saved);
    legacy.remove(QStringLiteral("regions"));
    require(normalizePersistedSelection(legacy).valid &&
                !normalizePersistedSelection(legacy).value.region,
            "legacy rectangle compatibility");
    legacy.insert(QStringLiteral("regions"), QJsonArray{QStringLiteral("bad")});
    require(!normalizePersistedSelection(legacy).valid,
            "bad regions must not expose bounding rectangle");
    auto extreme = persistedSelectionToJson(saved);
    auto rectangle = [](int x) {
        return QJsonObject{{QStringLiteral("x"), x},
                           {QStringLiteral("y"), 0},
                           {QStringLiteral("width"), 100},
                           {QStringLiteral("height"), 100}};
    };
    extreme.insert(QStringLiteral("regions"),
                   QJsonArray{rectangle(-2000000000), rectangle(2000000000)});
    require(!normalizePersistedSelection(extreme).valid,
            "region spans must not overflow integer bounds");
    ScreenshotSelectionModel model;
    ScreenshotSelectionParams params;
    params.selection = saved.rectangle;
    params.region = saved.region;
    require(model.applyParams(params, QRect(0, 0, 50, 50)), "partial restore");
    require(model.selectionRegion() == saved.region->intersected(QRect(0, 0, 50, 50)),
            "clip restore");
    require(!model.applyParams(params, QRect(200, 200, 10, 10)) && !model.hasPixelSelection(),
            "fully unavailable restored shape clears selection");
    ScreenshotInteractionState interaction;
    interaction.beginCapture();
    require(!interaction.selectionHandlesVisible(), "manual entry has no handles");
    require(interaction.enterSelectionDrag(ScreenshotSelectionDragMode::Marquee), "start marquee");
    require(!interaction.selectionHandlesVisible(), "marquee has no handles");
    require(interaction.enterSelectionDrag(ScreenshotSelectionDragMode::All), "move marquee");
    require(!interaction.selectionHandlesVisible(), "temporary move has no handles");
    interaction.finishDrag();
    interaction.confirmSelection();
    require(interaction.selectionHandlesVisible(), "confirmed rectangle handles return");
}
} // namespace
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    geometryAndTransactions();
    outlinesAndEffects();
    persistenceAndHandlePolicy();
    std::cout << "Multi-region selection tests passed\n";
}
