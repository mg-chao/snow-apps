#include "snow_shot/image/screenshotregionpoints.h"
#include <QLineF>
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "snow_shot/presentation/screenshotsmartselectiontransition.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_shot/presentation/screenshotselectionshadowrenderer.h"
#include "snow_shot/storage/persistedselectioncodec.h"

#include "snow_draw_engine_qt/snow_canvas_path_geometry.h"
#include <QGuiApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QPainterPathStroker>
#include <QPainter>
#include <QTimer>
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
    const auto before = model.selectionRegion();
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

void moveSnapshotEndsWithSelection() {
    const QRegion original = QRegion(QRect(10, 10, 20, 20)) + QRect(50, 10, 20, 20);
    for (const bool resetModel : {false, true}) {
        ScreenshotSelectionModel model;
        model.setSelectionRegion(original);
        model.beginMoveDrag(QPointF(10, 10));
        model.setDraggedSelectionRect(QRectF(15, 20, 60, 20), ScreenshotSelectionDragMode::All);
        require(model.selectionRegion() == original.translated(5, 10),
                "active move must retain its original complex geometry");

        if (resetModel)
            model.reset();
        else
            model.clearSelection();
        const QRect fresh(100, 100, 25, 25);
        model.setSelectionRect(fresh);
        model.setDraggedSelectionRect(QRectF(fresh), ScreenshotSelectionDragMode::All);
        require(model.selectionRegion() == QRegion(fresh),
                "ended selection must not reuse the previous move snapshot");
    }
}

void animatedMarqueeUsesDisplayedGeometry() {
    using Operation = ScreenshotSelectionModel::RegionOperation;
    const auto check = [](Operation operation, const QRect& confirmed, const QRect& first,
                          const QRect& target) {
        ScreenshotSelectionModel model;
        model.setSelectionRect(confirmed);
        model.beginRegionOperation(operation);
        require(model.selectionRegionForMarquee({}) == QRegion(confirmed),
                "empty animated marquee must preserve the confirmed region");
        model.setSelectionRect(first);
        ScreenshotRegionGeometry displayed;
        ScreenshotSmartSelectionTransition transition(
            [&](const QRectF& frame) { displayed = model.selectionRegionForMarquee(frame); });

        static_cast<void>(transition.update(QRectF(first), true));
        const QRegion firstRegion = operation == Operation::Add
                                        ? QRegion(confirmed).united(first)
                                        : QRegion(confirmed).subtracted(first);
        require(displayed == firstRegion, "first region preview must be immediate");

        model.setSelectionRect(target);
        static_cast<void>(transition.update(QRectF(target), true));
        const QRegion targetRegion = operation == Operation::Add
                                         ? QRegion(confirmed).united(target)
                                         : QRegion(confirmed).subtracted(target);
        require(transition.isRunning(), "changed region target must animate");
        require(displayed == firstRegion, "region preview must start at the previous frame");
        require(model.selectionRegion() == targetRegion,
                "animated region preview must not change the capture target");

        QEventLoop loop;
        QTimer::singleShot(ScreenshotSmartSelectionTransition::kDurationMs + 100, &loop,
                           &QEventLoop::quit);
        loop.exec();
        require(!transition.isRunning(), "region preview transition must finish");
        require(displayed == targetRegion, "region preview must finish at the target shape");
    };

    check(Operation::Add, QRect(10, 10, 40, 40), QRect(60, 10, 20, 30), QRect(110, 10, 30, 30));
    check(Operation::Subtract, QRect(0, 0, 100, 100), QRect(20, 0, 40, 100), QRect(60, 0, 40, 100));
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
    const QRegion separated = QRegion(QRect(0, 0, 32, 32)) + QRegion(QRect(160, 0, 32, 32));
    const auto separatedOutline = screenshotRegionPath(separated, 8);
    require(!separatedOutline.contains(QPointF(0.5, 0.5)) &&
                separatedOutline.contains(QPointF(16, 16)) &&
                separatedOutline.contains(QPointF(176, 16)) &&
                !separatedOutline.contains(QPointF(96, 16)),
            "isolated rounded regions preserve their separate contours");
    QImage separatedContent(192, 32, QImage::Format_ARGB32_Premultiplied);
    separatedContent.fill(Qt::red);
    ScreenshotResultStyle separatedStyle;
    separatedStyle.region = separated;
    separatedStyle.cornerRadius = 8;
    separatedStyle.shadowWidth = 4;
    const QImage separatedOutput =
        ScreenshotResultCompositor::compose(separatedContent, separatedStyle);
    require(separatedOutput.pixelColor(20, 20).alpha() == 255 &&
                separatedOutput.pixelColor(180, 20).alpha() == 255 &&
                separatedOutput.pixelColor(100, 20).alpha() == 0 &&
                separatedOutput.pixelColor(37, 20).alpha() > 0,
            "sparse export retains both regions and their local shadows");
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

void customGeometryTransactionsAndPersistence() {
    const QVector<QPointF> vertices{{10.25, 10.5}, {110.75, 20.25}, {65.5, 100.75}, {20.25, 70.5}};
    QPainterPath polygon;
    polygon.addPolygon(QPolygonF(vertices));
    polygon.closeSubpath();
    const auto polyline =
        ScreenshotRegionGeometry::fromPath(polygon, ScreenshotRegionType::Polyline);
    const auto curvePath = snowCanvasCatmullRomPath(vertices, true);
    const auto curve = ScreenshotRegionGeometry::fromPath(curvePath, ScreenshotRegionType::Curve);
    require(curve.path() == curvePath &&
                curve.path().elementAt(1).type == QPainterPath::CurveToElement,
            "curve regions must retain engine cubics exactly before Boolean operations");
    require(curve.custom() && curve.rectCount() > 1,
            "custom shapes must not acquire rectangle handles");
    const auto hole = curve.subtracted(QRect(40, 35, 20, 20));
    require(!hole.contains(QPointF(50, 45)) && hole.contains(QPointF(65, 65)),
            "curve subtraction preserves holes");
    const auto expanded = hole.united(QRect(150, 20, 20, 20));
    require(expanded.contains(QPointF(155, 25)) && !expanded.contains(QPointF(130, 25)),
            "disconnected vector components");
    require(expanded.translated(9, 13).contains(QPointF(164, 38)),
            "translate every vector operand");
    const auto decoded = ScreenshotRegionGeometry::fromJson(expanded.toJson());
    require(decoded && *decoded == expanded && decoded->path(2.0) == expanded.path(2.0),
            "vector operand codec round trip at another scale");
    auto corrupt = expanded.toJson();
    auto operands = corrupt.value(QStringLiteral("operands")).toArray();
    auto first = operands.first().toObject();
    first.insert(QStringLiteral("commands"), QJsonArray{QJsonArray{2, 4, 8}});
    operands[0] = first;
    corrupt.insert(QStringLiteral("operands"), operands);
    require(!ScreenshotRegionGeometry::fromJson(corrupt), "reject orphan cubic control points");
    corrupt = expanded.toJson();
    corrupt.insert(QStringLiteral("version"), 999);
    require(!ScreenshotRegionGeometry::fromJson(corrupt), "reject unsupported geometry versions");

    ScreenshotSelectionModel model;
    model.setSelectionRect(QRect(0, 0, 200, 200));
    static_cast<void>(model.setCornerRadius(20));
    model.beginRegionOperation(ScreenshotSelectionModel::RegionOperation::Add);
    model.setDraftRegion(polyline);
    model.commitDraftRegion();
    require(model.cornerRadiusApplicable(),
            "contained custom addition must not disable rectangle rounding");
    model.beginRegionOperation(ScreenshotSelectionModel::RegionOperation::Subtract);
    model.setDraftRegion(polyline);
    require(!model.cornerRadiusApplicable(), "custom contribution suppresses rounding in preview");
    model.cancelRegionOperation();
    require(model.cornerRadiusApplicable() && model.cornerRadius() == 20,
            "cancel restores radius eligibility and value");
    model.beginRegionOperation(ScreenshotSelectionModel::RegionOperation::Subtract);
    model.setDraftRegion(polyline);
    model.commitDraftRegion();
    require(!model.cornerRadiusApplicable() && model.cornerRadius() == 20,
            "commit retains radius preference without applying it");
    const auto params = model.params(QRect(0, 0, 300, 300));
    ScreenshotSelectionModel restored;
    require(restored.applyParams(params, QRect(0, 0, 300, 300)) &&
                restored.selectionRegion() == model.selectionRegion(),
            "custom selection model restoration");
    require(ScreenshotResultCompositor::normalizedStyle(model.resultStyle()).cornerRadius == 0,
            "compositor centrally suppresses custom rounding");
    model.setSelectionRect(QRect(0, 0, 200, 200));
    require(model.cornerRadiusApplicable(), "replacement rectangle restores radius eligibility");

    snow_shot::storage::PersistedSelection persisted;
    persisted.shadowColor = Qt::black;
    persisted.rectangle = expanded.boundingRect();
    persisted.region = expanded;
    const auto normalization = snow_shot::storage::normalizePersistedSelection(
        snow_shot::storage::persistedSelectionToJson(persisted));
    require(normalization.valid && normalization.value == persisted,
            "history/previous selection vector codec");
    QImage content(200, 150, QImage::Format_ARGB32_Premultiplied);
    content.fill(Qt::red);
    ScreenshotResultStyle style;
    style.region = expanded;
    style.cornerRadius = 40;
    const auto raster = ScreenshotResultCompositor::compose(content, style);
    require(raster.pixelColor(50, 45).alpha() == 0 && raster.pixelColor(155, 25).alpha() == 255,
            "export uses holes and components");
    bool antialiased = false;
    for (int y = 0; y < raster.height(); ++y)
        for (int x = 0; x < raster.width(); ++x) {
            const int alpha = raster.pixelColor(x, y).alpha();
            antialiased |= alpha > 0 && alpha < 255;
        }
    require(antialiased, "custom contour edges must be antialiased");
    QPainterPath sparseCurves;
    sparseCurves.addEllipse(QRectF(10, 10, 30, 30));
    sparseCurves.addEllipse(QRectF(160, 10, 30, 30));
    QPainterPath wholeBounds = sparseCurves;
    wholeBounds.moveTo(35, 25);
    wholeBounds.lineTo(165, 25);
    ScreenshotResultStyle sparseCurveStyle;
    sparseCurveStyle.shadowWidth = 8;
    sparseCurveStyle.region =
        ScreenshotRegionGeometry::fromPath(sparseCurves, ScreenshotRegionType::Curve);
    QImage sparseContent(200, 50, QImage::Format_ARGB32_Premultiplied);
    sparseContent.fill(Qt::blue);
    const QImage sparseResult =
        ScreenshotResultCompositor::compose(sparseContent, sparseCurveStyle);
    sparseCurveStyle.region =
        ScreenshotRegionGeometry::fromPath(wholeBounds, ScreenshotRegionType::Curve);
    const QImage wholeResult = ScreenshotResultCompositor::compose(sparseContent, sparseCurveStyle);
    require(sparseResult == wholeResult,
            "tiled sparse export must match whole-image mask and shadow pixels");
    const auto repeated = snowCanvasCatmullRomPath({{10, 10}, {10, 10}, {50, 10}, {30, 50}}, true);
    require(!repeated.isEmpty(), "curve bridge supports degenerate neighbor fallback");
}
void translatedGeometryRetainsContoursAndPersistence() {
    QPainterPath ellipse;
    ellipse.addEllipse(QRectF(0.25, 0.5, 160, 120));
    auto geometry = ScreenshotRegionGeometry::fromPath(ellipse, ScreenshotRegionType::Curve)
                        .subtracted(QRect(60, 30, 25, 80));
    const auto original = geometry.path();
    const auto moved = geometry.translated(123, -47);
    require(moved.boundingRect() == geometry.boundingRect().translated(123, -47),
            "translated geometry retains exact bounds");
    require(moved.contains(QPointF(143, -7)) == geometry.contains(QPointF(20, 40)),
            "translated hit testing uses local geometry");
    const auto restored = ScreenshotRegionGeometry::fromJson(moved.toJson());
    require(restored && *restored == moved, "translated operands round trip in canvas coordinates");
    for (const qreal scale : {1.0, 1.25, 2.0, 1.5, 1.0, 2.0, 3.0, 1.0}) {
        const auto fresh = ScreenshotRegionGeometry::fromJson(geometry.toJson());
        require(moved.path(scale) == fresh->path(scale).translated(123, -47),
                "scaled contours are independent of previous scale requests and translations");
    }
    require(geometry.path() == original, "render scale changes preserve the canonical contour");
    const auto localCut = geometry.subtracted(QRect(10, 10, 20, 20)).translated(123, -47);
    const auto movedCut = moved.subtracted(QRect(133, -37, 20, 20));
    require(localCut == movedCut, "editing a translated snapshot retains operand coordinates");
    require(geometry.path() == original && moved.translated(-123, 47) == geometry,
            "editing and translation never mutate shared snapshots");
}

void regionMaskMatchesPainterCoverage() {
    QPainterPath path;
    path.addEllipse(QRectF(1.25, 2.75, 63.5, 48.25));
    path.addEllipse(QRectF(16.5, 15.75, 20.25, 20.5));
    path.setFillRule(Qt::OddEvenFill);
    QImage content(70, 60, QImage::Format_ARGB32_Premultiplied);
    content.fill(QColor(170, 50, 200, 180));
    ScreenshotResultStyle style;
    style.region = ScreenshotRegionGeometry::fromPath(path, ScreenshotRegionType::Curve);
    QImage reference = content;
    QImage mask(content.size(), QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    {
        QPainter painter(&mask);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillPath(path, Qt::white);
    }
    {
        QPainter painter(&reference);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        painter.drawImage(QPoint(), mask);
    }
    require(ScreenshotResultCompositor::compose(content, style) == reference,
            "region mask preserves painter coverage and translucent content exactly");
    // A color or shadow-width change must not reuse the preceding effect.
    style.shadowWidth = 7;
    style.shadowColor = Qt::red;
    const auto red = ScreenshotResultCompositor::compose(content, style);
    style.shadowColor = Qt::blue;
    const auto blue = ScreenshotResultCompositor::compose(content, style);
    require(red != blue, "shadow color changes invalidate the effect independently of the mask");
    style.shadowWidth = 0;
    require(ScreenshotResultCompositor::compose(content, style) == reference,
            "disabling a cached shadow restores the original masked content");
}

void sessionCacheCleanupPreservesResults() {
    QPainterPath ellipse;
    ellipse.addEllipse(QRectF(0, 0, 100, 80));
    const auto geometry = ScreenshotRegionGeometry::fromPath(ellipse, ScreenshotRegionType::Curve)
                              .subtracted(QRect(30, 20, 20, 30));
    const auto snapshot = geometry.translated(2, 3);
    geometry.clearDerivedCache();
    const auto coldBytes = geometry.retainedBytesEstimate();
    const auto expected = snapshot.path(1.5);
    require(geometry.retainedBytesEstimate() > coldBytes, "fixture must retain a derived contour");
    snapshot.clearDerivedCache();
    require(geometry.retainedBytesEstimate() == coldBytes,
            "session cleanup releases derived contours shared with retained snapshots");
    require(snapshot.path(1.5) == expected, "cleaned geometry rebuilds the same contour");

    QImage content(100, 80, QImage::Format_ARGB32_Premultiplied);
    content.fill(Qt::white);
    ScreenshotResultStyle style;
    style.region = QRegion(content.rect()).subtracted(QRect(30, 20, 20, 30));
    style.cornerRadius = 5;
    style.shadowWidth = 7;
    const auto first = ScreenshotResultCompositor::compose(content, style);
    const auto warm = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(warm.regionCacheRetainedBytes > 0 && warm.regionPathCacheElements > 0,
            "fixture must populate both region composition and contour caches");
    ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
    ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
    const auto cleared = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(cleared.retainedBytes == 0 && cleared.retainedEntries == 0 &&
                cleared.regionCacheRetainedBytes == 0 && cleared.regionPathCacheElements == 0,
            "session cleanup is idempotent and releases every thread-local rendering cache");
    require(ScreenshotResultCompositor::compose(content, style) == first,
            "a subsequent session rebuilds identical export pixels");
    ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
}

void tiledShadowsMatchIndependentLocalCompositions(bool sparse) {
    QImage content(sparse ? 5000 : 1200, 1000, QImage::Format_ARGB32_Premultiplied);
    content.fill(QColor(70, 140, 210, 180));
    QPainterPath path;
    path.addEllipse(QRectF(1.25, 2.5, sparse ? 1095.5 : 1195.5, 990.25));
    if (sparse)
        path.addEllipse(QRectF(3900.25, 2.5, 1095.5, 990.25));
    path.addRect(QRectF(490.25, 490.5, 550, 200));
    path.setFillRule(Qt::OddEvenFill);
    for (const int width : {1, 7, 32}) {
        ScreenshotResultStyle style;
        style.region = ScreenshotRegionGeometry::fromPath(path, ScreenshotRegionType::Curve);
        style.shadowWidth = width;
        style.shadowColor = QColor(35, 20, 90, 190);
        const auto tiled = ScreenshotResultCompositor::compose(content, style, 1, 0.7);
        for (const int x : {16, 512, 1024, sparse ? 4412 : 1190}) {
            for (const int y : {16, 512, 990}) {
                const QPoint origin(x - width - 64, y - width - 64);
                auto localStyle = style;
                localStyle.region = ScreenshotRegionGeometry::fromPath(path.translated(-origin),
                                                                       ScreenshotRegionType::Curve);
                const auto local = ScreenshotResultCompositor::compose(
                    content.copy(QRect(origin, QSize(128, 128))), localStyle, 1, 0.7);
                for (int dy = -8; dy <= 8; ++dy)
                    for (int dx = -8; dx <= 8; ++dx) {
                        const auto a = tiled.pixelColor(x + dx, y + dy);
                        const auto b = local.pixelColor(64 + width + dx, 64 + width + dy);
                        // Compare premultiplied channels: unpremultiplication of
                        // a one-level alpha difference magnifies low-alpha RGB.
                        const auto pa = qPremultiply(a.rgba()), pb = qPremultiply(b.rgba());
                        require(
                            std::abs(qRed(pa) - qRed(pb)) <= 1 &&
                                std::abs(qGreen(pa) - qGreen(pb)) <= 1 &&
                                std::abs(qBlue(pa) - qBlue(pb)) <= 1 &&
                                std::abs(qAlpha(pa) - qAlpha(pb)) <= 1,
                            "tiled export matches independent full-support compositions at seams");
                    }
            }
        }
    }
}

void shapeCodecAndSamplingBoundaries() {
    QVector<QPointF> samples;
    for (int i = 0; i <= 1000; ++i)
        samples.append(QPointF(i * 0.1, std::sin(i * 0.01) * 20));
    const auto reduced = simplifyScreenshotRegionPoints(samples, 0.25);
    require(reduced.size() < samples.size() / 10 && reduced.first() == samples.first() &&
                reduced.last() == samples.last(),
            "Freehand removes redundant samples and retains endpoints");
    for (const auto& point : samples) {
        qreal distance = 1e9;
        for (int i = 1; i < reduced.size(); ++i) {
            const auto delta = reduced[i] - reduced[i - 1];
            const auto offset = point - reduced[i - 1];
            const auto length = QPointF::dotProduct(delta, delta);
            const auto t = std::clamp(QPointF::dotProduct(offset, delta) / length, 0.0, 1.0);
            distance = std::min(distance, QLineF(point, reduced[i - 1] + t * delta).length());
        }
        require(distance <= 0.250001, "Freehand simplification obeys subpixel error bound");
    }
    QPainterPath crossing;
    crossing.addPolygon(QPolygonF{{0, 0}, {100, 100}, {0, 100}, {100, 0}});
    crossing.closeSubpath();
    const auto region =
        ScreenshotRegionGeometry::fromPath(crossing, ScreenshotRegionType::Polyline);
    require(region.contains({50, 10}) && !region.contains({10, 50}),
            "self-intersections use odd-even filling");
    ScreenshotSelectionModel model;
    model.setDraftRegion(region);
    model.commitDraftRegion(QRect(25, 0, 50, 100));
    require(model.selectionRegion().boundingRect() == QRect(25, 0, 50, 100) &&
                !model.selectionRegion().contains({15, 5}),
            "commit clips to the available canvas");
    ScreenshotResultStyle style{20, 8, Qt::black};
    style.region = model.selectionRegion();
    style.regionScale = 1.75;
    const auto encoded = encodeScreenshotResultStyle(style);
    const auto decoded = decodeScreenshotResultStyle(encoded);
    require(decoded && decoded->region == style.region && decoded->regionScale == 1.75,
            "pinned style codec retains vector geometry and scale");
    require(!decodeScreenshotResultStyle(encoded.chopped(5)),
            "truncated pinned geometry rejects the record");
    QByteArray legacy;
    QDataStream legacyStream(&legacy, QIODevice::WriteOnly);
    legacyStream << style.cornerRadius << style.shadowWidth << style.shadowColor;
    const auto oldStyle = decodeScreenshotResultStyle(legacy);
    require(oldStyle && !oldStyle->region && oldStyle->cornerRadius == 20,
            "legacy pinned result styles remain readable");
    style.region.reset();
    require(decodeScreenshotResultStyle(encodeScreenshotResultStyle(style)).has_value(),
            "rectangle style extensions round trip without geometry");
}
} // namespace
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    sessionCacheCleanupPreservesResults();
    tiledShadowsMatchIndependentLocalCompositions(false);
    tiledShadowsMatchIndependentLocalCompositions(true);
    translatedGeometryRetainsContoursAndPersistence();
    regionMaskMatchesPainterCoverage();
    shapeCodecAndSamplingBoundaries();
    customGeometryTransactionsAndPersistence();
    geometryAndTransactions();
    moveSnapshotEndsWithSelection();
    animatedMarqueeUsesDisplayedGeometry();
    outlinesAndEffects();
    persistenceAndHandlePolicy();
    std::cout << "Multi-region selection tests passed\n";
}
