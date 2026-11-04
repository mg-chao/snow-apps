#include "snow_shot/presentation/screenshottextannotationrenderer.h"
#include "snow_shot/presentation/screenshottextannotationgeometry.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include <QApplication>
#include <QFontDatabase>
#include <QPainter>
#include <QTextDocument>
#include <QAbstractTextDocumentLayout>
#include <algorithm>
#include <future>
#include <cstdio>
#include <cstdlib>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

void highlightingUsesMultiplyAndLeavesSourceUntouched() {
    QImage original(40, 30, QImage::Format_ARGB32_Premultiplied);
    original.fill(QColor(200, 160, 120));
    original.setPixelColor(15, 12, Qt::black);
    QPainterPath path;
    path.addRect(QRectF(110, 210, 20, 10));
    ScreenshotTextAnnotationRenderSnapshot snapshot;
    const QColor color(240, 80, 20, 140);
    snapshot.items.append({path, color, true});
    snapshot.bounds = path.boundingRect();
    const auto result =
        renderScreenshotTextAnnotations(original, QRectF(100, 200, 40, 30), snapshot);
    require(!result.isNull(), "render an immutable highlight snapshot");
    QImage expected = original.copy();
    QPainter painter(&expected);
    painter.setCompositionMode(QPainter::CompositionMode_Multiply);
    painter.fillRect(QRect(10, 10, 20, 10), color);
    painter.end();
    require(result == expected && result.pixelColor(15, 12) == QColor(Qt::black),
            "text highlighting matches canvas multiply blending without covering black glyphs");
    require(original.pixelColor(12, 12) == QColor(200, 160, 120),
            "rendering keeps the shared original raster pristine");
}

void wrappedTextProducesSeparateMarksAndScalesOrientation() {
    QTextDocument text;
    QFont font = QApplication::font();
    font.setPixelSize(18);
    text.setDefaultFont(font);
    text.setPlainText(QStringLiteral("First selected row\nSecond selected row"));
    text.setTextWidth(260);
    ScreenshotTextAnnotationDocument annotations;
    annotations.bindSource(ScreenshotTextAnnotationSource::Formatted,
                           screenshotFormattedTextAnnotationFingerprint(text));
    const QVector<ScreenshotTextSelectionRange> selected{{0, 0, text.characterCount() - 1}};
    ScreenshotTextAnnotationStyle style;
    style.color = QColor(20, 90, 230, 180);
    style.width = 2;
    require(annotations.toggle(selected, ScreenshotTextAnnotationTool::Underline, style),
            "apply a multiline underline");
    const auto underline = screenshotFormattedTextAnnotationSnapshot(annotations, text);
    const auto geometry = screenshotFormattedTextAnnotationFragments(text, selected);
    if (geometry.size() != 2 || underline.items.size() != 2) {
        std::fprintf(stderr, "Multiline geometry: %lld fragments, %lld marks\n",
                     static_cast<long long>(geometry.size()),
                     static_cast<long long>(underline.items.size()));
        for (const auto& fragment : geometry)
            std::fprintf(stderr, "Range %d..%d: rect %.3f %.3f %.3f %.3f\n", fragment.range.start,
                         fragment.range.end, fragment.rect.x(), fragment.rect.y(),
                         fragment.rect.width(), fragment.rect.height());
    }
    require(geometry.size() == 2 && underline.items.size() == 2,
            "a multiline selection produces separate underline paths for its visual rows");
    for (qsizetype i = 0; i < geometry.size(); ++i) {
        const auto bottom = geometry[i].localToSource.mapRect(geometry[i].rect).bottom();
        require(underline.items[i].coverage().boundingRect().top() >= bottom - 0.01,
                "underlines remain beneath glyph descenders");
    }
    QTransform rotation;
    rotation.translate(200, 0);
    rotation.rotate(90);
    const auto rotated = screenshotFormattedTextAnnotationSnapshot(annotations, text, rotation);
    require(rotated.items.size() == underline.items.size(), "rotation preserves visual row count");
    for (qsizetype i = 0; i < underline.items.size(); ++i)
        require(rotated.items[i].coverage() == rotation.map(underline.items[i].coverage()),
                "text annotation orientation follows the source transform");
    const QVector<ScreenshotTextSelectionRange> partial{{0, 1, 6}};
    require(annotations.toggle(partial, ScreenshotTextAnnotationTool::Highlight, style),
            "highlight a subrange within an existing underline");
    const auto partialHighlight = screenshotFormattedTextAnnotationSnapshot(annotations, text);
    qsizetype lineIndex = 0;
    for (const auto& item : partialHighlight.items) {
        if (!item.highlight) {
            require(lineIndex < underline.items.size() &&
                        item.coverage() == underline.items[lineIndex++].coverage(),
                    "highlight boundaries preserve existing line geometry and pattern phase");
        }
    }
    require(lineIndex == 2 &&
                annotations.toggle(partial, ScreenshotTextAnnotationTool::Highlight, style),
            "removing the partial highlight retains both complete underline rows");
    require(annotations.toggle(selected, ScreenshotTextAnnotationTool::Highlight, style),
            "highlight coexists with underline");
    const auto combined = screenshotFormattedTextAnnotationSnapshot(annotations, text);
    require(combined.items.size() == 4 &&
                text.toPlainText() == QStringLiteral("First selected row\nSecond selected row"),
            "highlighting and line marks never modify source text or its copied formatting");
    style.renderingStyle = SnowCanvasStrokeRenderingStyle::Artist;
    style.strokeStyle = SnowCanvasStrokeStyle::Dashed;
    require(annotations.toggle(selected, ScreenshotTextAnnotationTool::WavyUnderline, style),
            "a wavy underline replaces the existing line style");
    const auto waves = screenshotFormattedTextAnnotationSnapshot(annotations, text);
    const auto repeat = screenshotFormattedTextAnnotationSnapshot(annotations, text);
    require(waves.items.size() == 4 && waves.items[2].coverage() == repeat.items[2].coverage(),
            "hand-drawn dashed wavy marks remain deterministic across snapshots");
}

void ocrLineThicknessUsesSourceUnits() {
    ScreenshotOcrPresentation source;
    source.selection = QRect(0, 0, 300, 180);
    ScreenshotOcrLine line;
    line.text = QStringLiteral("OCR source units");
    line.quad = QPolygonF{QPointF(20, 20), QPointF(260, 20), QPointF(260, 100), QPointF(20, 100)};
    source.lines.append(line);
    source.prepareForRendering();
    ScreenshotTextAnnotationDocument annotations;
    annotations.bindSource(ScreenshotTextAnnotationSource::Ocr,
                           screenshotOcrTextAnnotationFingerprint(source));
    ScreenshotTextAnnotationStyle style;
    style.width = 4;
    require(annotations.toggle({{0, 0, static_cast<int>(line.text.size())}},
                               ScreenshotTextAnnotationTool::Underline, style),
            "apply a source-width underline to a large OCR text quad");
    const auto marks = screenshotOcrTextAnnotationSnapshot(annotations, source);
    require(marks.items.size() == 1 &&
                qAbs(marks.items.front().coverage().boundingRect().height() - 4) < 0.01,
            "OCR font fitting does not scale the remembered line width");
}

void largeAnnotationsRetainOnlyActiveGeometry() {
    auto document = std::make_shared<QTextDocument>();
    QFont font = QApplication::font();
    font.setPixelSize(18);
    document->setDefaultFont(font);
    document->setPlainText(
        QStringLiteral("The quick brown fox jumps over the lazy dog. Selected text.\n")
            .repeated(500));
    document->setTextWidth(640);
    ScreenshotTextAnnotationDocument annotations;
    annotations.bindSource(ScreenshotTextAnnotationSource::Formatted,
                           screenshotFormattedTextAnnotationFingerprint(*document));
    const QVector<ScreenshotTextSelectionRange> selected{{0, 0, document->characterCount() - 1}};
    ScreenshotTextAnnotationStyle style;
    style.color = Qt::red;
    annotations.toggle(selected, ScreenshotTextAnnotationTool::WavyUnderline, style);
    ScreenshotTextAnnotationRenderer renderer;
    const auto original = renderer.formattedSnapshot(annotations, *document);
    const auto initial = renderer.diagnostics();
    require(initial.geometryBytes < 256 * 1024 && initial.strokes == original.items.size(),
            "large annotations retain compact descriptions instead of expanded coverage");
    style.color = Qt::blue;
    annotations.updateStyle(selected, ScreenshotTextAnnotationTool::WavyUnderline, style,
                            ScreenshotTextAnnotationStylePropertyColor);
    const auto recoloured = renderer.formattedSnapshot(annotations, *document);
    auto diagnostics = renderer.diagnostics();
    require(diagnostics.fragmentBuilds == initial.fragmentBuilds &&
                diagnostics.strokePreparations == initial.strokePreparations,
            "recolouring a large document must not reshape text or expand strokes");
    require(recoloured.items.size() == original.items.size(), "recolouring preserves every row");
    for (qsizetype i = 0; i < original.items.size(); ++i)
        require(recoloured.items[i].coverage() == original.items[i].coverage() &&
                    recoloured.items[i].color == QColor(Qt::blue) &&
                    original.items[i].color == QColor(Qt::red),
                "recolouring shares immutable ink without modifying an earlier export snapshot");
    QTransform transform;
    transform.translate(73, -29);
    transform.rotate(35);
    transform.scale(1.75, 1.25);
    const auto moved = renderer.formattedSnapshot(annotations, *document, transform);
    diagnostics = renderer.diagnostics();
    require(diagnostics.fragmentBuilds == initial.fragmentBuilds &&
                diagnostics.strokePreparations == initial.strokePreparations,
            "camera transforms reuse source-space geometry independently of cache size");
    for (qsizetype i = 0; i < original.items.size(); ++i)
        require(moved.items[i].coverage() == transform.map(original.items[i].coverage()),
                "retained geometry follows rotation and nonuniform scaling");
    style.color = Qt::transparent;
    annotations.updateStyle(selected, ScreenshotTextAnnotationTool::WavyUnderline, style,
                            ScreenshotTextAnnotationStylePropertyColor);
    require(renderer.formattedSnapshot(annotations, *document).empty(),
            "making a stroke transparent removes its ink and bounds from new snapshots");
    style.color = Qt::blue;
    annotations.updateStyle(selected, ScreenshotTextAnnotationTool::WavyUnderline, style,
                            ScreenshotTextAnnotationStylePropertyColor);
    const auto visibleAgain = renderer.formattedSnapshot(annotations, *document);
    require(visibleAgain.items.size() == original.items.size() &&
                renderer.diagnostics().strokePreparations == initial.strokePreparations,
            "restoring opacity reuses active geometry after an invisible snapshot");
    style.width *= 2;
    annotations.updateStyle(selected, ScreenshotTextAnnotationTool::WavyUnderline, style,
                            ScreenshotTextAnnotationStylePropertyWidth);
    const auto wider = renderer.formattedSnapshot(annotations, *document);
    diagnostics = renderer.diagnostics();
    require(diagnostics.fragmentBuilds == initial.fragmentBuilds &&
                diagnostics.strokePreparations == initial.strokePreparations + initial.strokes &&
                diagnostics.strokes == initial.strokes &&
                wider.items.front().coverage() != original.items.front().coverage(),
            "width changes replace active strokes without retaining obsolete widths or reshaping");
    const auto beforeLayoutChange = renderer.diagnostics();
    document->setTextWidth(320);
    const auto wrapped = renderer.formattedSnapshot(annotations, *document);
    require(renderer.diagnostics().fragmentBuilds > beforeLayoutChange.fragmentBuilds &&
                wrapped.items.size() > wider.items.size(),
            "document reflow invalidates retained fragment geometry");
    const std::weak_ptr<QTextDocument> weakDocument = document;
    document.reset();
    require(weakDocument.expired(), "retained geometry must not keep its source document alive");
    renderer.clear();
    require(renderer.diagnostics().strokes == 0 && renderer.diagnostics().fragmentSets == 0 &&
                !original.empty() && !wrapped.empty(),
            "releasing renderer state leaves outstanding immutable export snapshots usable");
}

void ocrGeometryFollowsSourceAndFontChanges() {
    ScreenshotOcrPresentation source;
    source.selection = QRect(0, 0, 400, 100);
    source.lines.append(
        {QStringLiteral("Original OCR geometry"), 1,
         QPolygonF{QPointF(10, 10), QPointF(390, 10), QPointF(390, 80), QPointF(10, 80)}});
    source.prepareForRendering();
    ScreenshotTextAnnotationDocument annotations;
    annotations.bindSource(ScreenshotTextAnnotationSource::Ocr,
                           screenshotOcrTextAnnotationFingerprint(source));
    const QVector<ScreenshotTextSelectionRange> selection{{0, 0, 8}};
    ScreenshotTextAnnotationStyle style;
    annotations.toggle(selection, ScreenshotTextAnnotationTool::Underline, style);
    ScreenshotTextAnnotationRenderer renderer;
    const auto original = renderer.ocrSnapshot(annotations, source);
    const auto initial = renderer.diagnostics();
    style.color = Qt::green;
    annotations.updateStyle(selection, ScreenshotTextAnnotationTool::Underline, style);
    const auto recoloured = renderer.ocrSnapshot(annotations, source);
    require(renderer.diagnostics().fragmentBuilds == initial.fragmentBuilds &&
                renderer.diagnostics().strokePreparations == initial.strokePreparations &&
                original.items.front().coverage() == recoloured.items.front().coverage(),
            "OCR colour changes reuse retained source-space ink");
    QFont bold = QApplication::font();
    bold.setBold(true);
    static_cast<void>(renderer.ocrSnapshot(annotations, source, {}, bold));
    require(renderer.diagnostics().fragmentBuilds > initial.fragmentBuilds,
            "OCR font changes must rebuild layout geometry");
    const auto beforeSourceChange = renderer.diagnostics();
    source.lines[0].quad.translate(0, 20);
    annotations.bindSource(ScreenshotTextAnnotationSource::Ocr,
                           screenshotOcrTextAnnotationFingerprint(source));
    annotations.toggle(selection, ScreenshotTextAnnotationTool::Underline, style);
    const auto shifted = renderer.ocrSnapshot(annotations, source);
    require(renderer.diagnostics().fragmentBuilds > beforeSourceChange.fragmentBuilds &&
                shifted.items.front().coverage().boundingRect().top() >
                    original.items.front().coverage().boundingRect().top(),
            "source rebinding replaces all retained geometry");
    annotations.clear();
    require(renderer.ocrSnapshot(annotations, source).empty() &&
                renderer.diagnostics().strokes == 0 && renderer.diagnostics().fragmentSets == 0,
            "removing annotations releases derived geometry immediately");
}

void incrementalEditsReuseUnchangedRanges() {
    ScreenshotOcrPresentation source;
    QVector<ScreenshotTextSelectionRange> selected;
    for (int i = 0; i < 65; ++i) {
        ScreenshotOcrLine line;
        line.text = QStringLiteral("Selected text on source row %1").arg(i);
        line.quad = QPolygonF{QPointF(0, i * 24), QPointF(400, i * 24), QPointF(400, i * 24 + 22),
                              QPointF(0, i * 24 + 22)};
        source.lines.append(line);
        if (i < 64)
            selected.append({i, 0, 8});
    }
    source.prepareForRendering();
    ScreenshotTextAnnotationDocument annotations;
    annotations.bindSource(ScreenshotTextAnnotationSource::Ocr,
                           screenshotOcrTextAnnotationFingerprint(source));
    ScreenshotTextAnnotationStyle style;
    annotations.toggle(selected, ScreenshotTextAnnotationTool::Underline, style);
    ScreenshotTextAnnotationRenderer renderer;
    const auto original = renderer.ocrSnapshot(annotations, source);
    const auto initial = renderer.diagnostics();
    const QVector<ScreenshotTextSelectionRange> extra{{64, 0, 8}};
    annotations.toggle(extra, ScreenshotTextAnnotationTool::Underline, style);
    const auto added = renderer.ocrSnapshot(annotations, source);
    const auto afterAdd = renderer.diagnostics();
    require(added.items.size() == original.items.size() + 1 &&
                afterAdd.fragmentBuilds == initial.fragmentBuilds + 1 &&
                afterAdd.strokePreparations == initial.strokePreparations + 1,
            "adding one OCR range builds only its fragments and stroke");
    for (qsizetype i = 0; i < original.items.size(); ++i)
        require(added.items[i].coverage() == original.items[i].coverage(),
                "an incremental annotation preserves every unchanged row");
    annotations.toggle(extra, ScreenshotTextAnnotationTool::Underline, style);
    const auto removed = renderer.ocrSnapshot(annotations, source);
    const auto afterRemove = renderer.diagnostics();
    require(removed.items.size() == original.items.size() &&
                afterRemove.fragmentBuilds == afterAdd.fragmentBuilds &&
                afterRemove.strokePreparations == afterAdd.strokePreparations &&
                afterRemove.fragmentSets == initial.fragmentSets &&
                afterRemove.strokes == initial.strokes,
            "removing an OCR range releases its geometry without rebuilding survivors");
    annotations.toggle(selected, ScreenshotTextAnnotationTool::Highlight, style);
    const auto both = renderer.ocrSnapshot(annotations, source);
    require(both.items.size() == original.items.size() * 2 &&
                renderer.diagnostics().fragmentBuilds == afterRemove.fragmentBuilds,
            "highlight and line channels share fragments for identical ranges");

    // Splitting a range must regenerate the changed pieces without changing other rows.
    style.color = Qt::blue;
    annotations.updateStyle({{0, 2, 6}}, ScreenshotTextAnnotationTool::Underline, style,
                            ScreenshotTextAnnotationStylePropertyColor);
    const auto split = renderer.ocrSnapshot(annotations, source);
    const auto fresh = screenshotOcrTextAnnotationSnapshot(annotations, source);
    require(renderer.diagnostics().fragmentBuilds == afterRemove.fragmentBuilds + 3 &&
                split.items.size() == fresh.items.size(),
            "partial recolouring builds only the three changed intervals");
    for (qsizetype i = 0; i < fresh.items.size(); ++i)
        require(split.items[i].coverage() == fresh.items[i].coverage() &&
                    split.items[i].color == fresh.items[i].color &&
                    split.items[i].highlight == fresh.items[i].highlight,
                "incremental geometry matches a fresh snapshot after splitting a range");

    QTextDocument text;
    text.setPlainText(QStringLiteral("First paragraph\nSecond paragraph\nThird paragraph"));
    text.setTextWidth(200);
    annotations.bindSource(ScreenshotTextAnnotationSource::Formatted,
                           screenshotFormattedTextAnnotationFingerprint(text));
    annotations.toggle({{0, 0, 5}}, ScreenshotTextAnnotationTool::Underline, style);
    renderer.formattedSnapshot(annotations, text);
    const auto formattedInitial = renderer.diagnostics();
    annotations.toggle({{0, 16, 22}}, ScreenshotTextAnnotationTool::Underline, style);
    const auto formatted = renderer.formattedSnapshot(annotations, text);
    require(formatted.items.size() == 2 &&
                renderer.diagnostics().fragmentBuilds == formattedInitial.fragmentBuilds + 1,
            "formatted text also retains fragments for unchanged selection intervals");
}

void clippedPaintingPreservesTransformedPixels() {
    QTextDocument text;
    text.setPlainText(QStringLiteral("Selected text with curved underline\n").repeated(30));
    text.setTextWidth(280);
    ScreenshotTextAnnotationDocument annotations;
    annotations.bindSource(ScreenshotTextAnnotationSource::Formatted,
                           screenshotFormattedTextAnnotationFingerprint(text));
    ScreenshotTextAnnotationStyle style;
    style.color = QColor(255, 100, 20, 150);
    const QVector<ScreenshotTextSelectionRange> selected{{0, 0, text.characterCount() - 1}};
    annotations.toggle(selected, ScreenshotTextAnnotationTool::Highlight, style);
    style.color = QColor(30, 90, 230, 180);
    annotations.toggle(selected, ScreenshotTextAnnotationTool::WavyUnderline, style);
    auto snapshot = screenshotFormattedTextAnnotationSnapshot(annotations, text);
    QPainterPath edge;
    edge.addEllipse(QRectF(94.6, 15.2, 0.8, 36.3));
    snapshot.items.append({edge, Qt::red, false});
    QTransform rotated;
    rotated.translate(210.25, 10.75);
    rotated.rotate(83);
    rotated.scale(.7, 1.3);
    const QVector<QTransform> transforms{{},
                                         QTransform::fromScale(.37, .81),
                                         QTransform(-.8, .1, .2, 1.1, 220.5, -15.2),
                                         rotated,
                                         QTransform(1, .1, .0002, .2, 1, .0001, -12, 3, 1)};
    for (qreal dpr : {1.0, 1.5, 2.0}) {
        for (const auto& transform : transforms) {
            for (int clipping : {0, 1, 2}) {
                const auto render = [&](bool optimized) {
                    QImage image(QSize(320, 180) * dpr, QImage::Format_ARGB32_Premultiplied);
                    image.setDevicePixelRatio(dpr);
                    image.fill(QColor(220, 230, 240));
                    QPainter painter(&image);
                    painter.translate(3.25, -2.5);
                    if (clipping == 1)
                        painter.setClipRegion(QRegion(QRect(10, 15, 85, 37)) |
                                              QRegion(QRect(160, 80, 70, 20)));
                    else if (clipping == 2)
                        painter.setClipRect(QRectF());
                    if (optimized) {
                        paintScreenshotTextAnnotations(painter, snapshot, transform);
                    } else {
                        painter.setRenderHint(QPainter::Antialiasing);
                        painter.setTransform(transform, true);
                        painter.setPen(Qt::NoPen);
                        for (bool highlight : {true, false}) {
                            painter.setCompositionMode(highlight
                                                           ? QPainter::CompositionMode_Multiply
                                                           : QPainter::CompositionMode_SourceOver);
                            for (const auto& item : snapshot.items) {
                                if (item.highlight == highlight) {
                                    painter.setBrush(item.color);
                                    painter.drawPath(item.coverage());
                                }
                            }
                        }
                    }
                    painter.end();
                    return image;
                };
                require(render(true) == render(false),
                        "clipped annotation painting preserves blending and antialiased pixels "
                        "under DPR, rotation, reflection, shear and perspective");
            }
        }
    }
}

void damageExcludesUnchangedInk() {
    ScreenshotTextAnnotationRenderSnapshot original;
    for (int i = 0; i < 4; ++i) {
        QPainterPath path;
        path.addRect(QRectF(10, i * 100, 200, 20));
        original.items.append({path, Qt::red, false});
        original.bounds = original.bounds.united(path.boundingRect());
    }
    require(screenshotTextAnnotationDamage(original, original).isEmpty(),
            "identical annotation snapshots do not damage the canvas");
    auto changed = original;
    changed.items[1].color = Qt::blue;
    require(screenshotTextAnnotationDamage(original, changed) ==
                original.items[1].coverage().boundingRect(),
            "recolouring one row does not repaint distant unchanged annotations");
    changed = original;
    changed.items.removeAt(1);
    require(screenshotTextAnnotationDamage(original, changed) ==
                    original.items[1].coverage().boundingRect() &&
                screenshotTextAnnotationDamage(changed, original) ==
                    original.items[1].coverage().boundingRect(),
            "inserting or removing a row preserves the unchanged prefix and suffix");
    changed = original;
    changed.items[1].path.translate(15, 7);
    const auto bounds = original.items[1].coverage().boundingRect().united(
        changed.items[1].coverage().boundingRect());
    require(screenshotTextAnnotationDamage(original, changed) == bounds &&
                screenshotTextAnnotationDamage(original, {}) == original.bounds &&
                screenshotTextAnnotationDamage({}, original) == original.bounds,
            "damage includes both moved ink and every item when showing or clearing annotations");
}

void deferredCoverageIsBoundedAndSafeToExport() {
    QTextDocument text;
    text.setPlainText(QStringLiteral("A selected row with a wavy underline\n").repeated(500));
    text.setTextWidth(400);
    ScreenshotTextAnnotationDocument annotations;
    annotations.bindSource(ScreenshotTextAnnotationSource::Formatted,
                           screenshotFormattedTextAnnotationFingerprint(text));
    const QVector<ScreenshotTextSelectionRange> selected{{0, 0, text.characterCount() - 1}};
    ScreenshotTextAnnotationStyle style;
    style.renderingStyle = SnowCanvasStrokeRenderingStyle::Artist;
    style.color = QColor(40, 100, 230, 170);
    annotations.toggle(selected, ScreenshotTextAnnotationTool::WavyUnderline, style);
    ScreenshotTextAnnotationRenderer renderer;
    const auto original = renderer.formattedSnapshot(annotations, text);
    require(renderer.diagnostics().geometryBytes < 256 * 1024 &&
                std::all_of(
                    original.items.cbegin(), original.items.cend(),
                    [](const auto& item) { return item.path.isEmpty() && item.stroke; }),
            "snapshot preparation never expands ordinary stroke coverage");
    ScreenshotTextAnnotationPaintCache cache;
    const auto paint = [&](const auto& snapshot, ScreenshotTextAnnotationPaintCache* retained) {
        QImage image(420, 90, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setClipRect(image.rect());
        paintScreenshotTextAnnotations(painter, snapshot, {}, retained);
        return image;
    };
    const auto first = paint(original, &cache);
    const auto initial = cache.diagnostics();
    require(initial.coverageBuilds > 0 && initial.coverageBuilds < 12,
            "only viewport-intersecting rows expand their stroke descriptions");
    require(first == paint(original, &cache) &&
                cache.diagnostics().coverageBuilds == initial.coverageBuilds,
            "an unchanged viewport reuses its coverage");
    auto mappedItem = original.items.front();
    mappedItem.sourceToCanvas = QTransform(.5, .1, .2, .8, 10, 20);
    require(cache.coverage(mappedItem) == mappedItem.coverage() &&
                cache.coverage(original.items.front()) == original.items.front().coverage() &&
                cache.diagnostics().coverageBuilds == initial.coverageBuilds,
            "camera changes remap retained coverage without regenerating source ink");
    QImage cancelledImage(420, 90, QImage::Format_ARGB32_Premultiplied);
    cancelledImage.fill(Qt::white);
    const QImage untouched = cancelledImage;
    QPainter cancelledPainter(&cancelledImage);
    cancelledPainter.setClipRect(cancelledImage.rect());
    cancelledPainter.setPen(Qt::green);
    ScreenshotTextAnnotationPaintCache cancelledCache;
    require(!paintScreenshotTextAnnotations(cancelledPainter, original, {}, &cancelledCache,
                                            [] { return true; }) &&
                cancelledCache.diagnostics().coverageBuilds == 0 &&
                cancelledPainter.pen().color() == QColor(Qt::green),
            "cancellation stops before coverage generation and restores the painter state");
    cancelledPainter.end();
    require(cancelledImage == untouched, "cancelled coverage leaves output pixels untouched");
    style.color = Qt::red;
    annotations.updateStyle(selected, ScreenshotTextAnnotationTool::WavyUnderline, style);
    const auto recoloured = renderer.formattedSnapshot(annotations, text);
    require(paint(recoloured, &cache) != first &&
                cache.diagnostics().coverageBuilds == initial.coverageBuilds,
            "recolouring visible ink does not regenerate coverage");
    renderer.clear();
    // Independent exports exercise the immutable description on worker threads while the
    // canvas continues to paint the same snapshot through its own thread-confined cache.
    auto worker = std::async(std::launch::async, [&] { return paint(original, nullptr); });
    require(paint(original, &cache) == worker.get(),
            "snapshots remain usable on independent threads after releasing renderer state");
    ScreenshotTextAnnotationPaintCache smallCache(64 * 1024);
    for (const auto& item : original.items) {
        require(smallCache.coverage(item) == item.coverage(),
                "cache eviction and oversized entries preserve exact coverage");
        require(smallCache.diagnostics().bytes <= 64 * 1024,
                "scrolling a long document never grows the coverage cache beyond its budget");
    }
}

void deferredBoundsCoverEveryStrokeStyle() {
    ScreenshotOcrPresentation source;
    source.selection = QRect(0, 0, 500, 250);
    source.lines.append(
        {QStringLiteral("Bounds of selected ink"), 1,
         QPolygonF{QPointF(15, 20), QPointF(390, 35), QPointF(385, 100), QPointF(10, 85)}});
    source.prepareForRendering();
    const QVector<QTransform> transforms{{},
                                         QTransform(-.7, .2, .3, 1.4, 400, 10),
                                         QTransform::fromScale(.25, 2),
                                         QTransform(1, .1, .0002, .2, 1, .0001, 10, 3, 1)};
    for (int rendering = 0; rendering < 3; ++rendering) {
        for (int pattern = 0; pattern < 3; ++pattern) {
            for (auto tool : {ScreenshotTextAnnotationTool::Underline,
                              ScreenshotTextAnnotationTool::WavyUnderline,
                              ScreenshotTextAnnotationTool::Strikethrough}) {
                for (qreal width : {.5, 2., 15.}) {
                    ScreenshotTextAnnotationDocument annotations;
                    annotations.bindSource(ScreenshotTextAnnotationSource::Ocr, {});
                    ScreenshotTextAnnotationStyle style;
                    style.width = width;
                    style.renderingStyle = static_cast<SnowCanvasStrokeRenderingStyle>(rendering);
                    style.strokeStyle = static_cast<SnowCanvasStrokeStyle>(pattern);
                    annotations.toggle({{0, 0, 20}}, tool, style);
                    for (const auto& transform : transforms) {
                        const auto snapshot =
                            screenshotOcrTextAnnotationSnapshot(annotations, source, transform);
                        require(!snapshot.empty(), "prepare transformed stroke bounds");
                        for (const auto& item : snapshot.items)
                            require(item.bounds()
                                        .adjusted(-.001, -.001, .001, .001)
                                        .contains(item.coverage().boundingRect()),
                                    "deferred bounds contain exact ink under every stroke style");
                    }
                }
            }
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
#ifdef Q_OS_WIN
    require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) >= 0,
            "load the Windows text-annotation renderer fixture font");
    QApplication::setFont(QFont(QStringLiteral("Segoe UI")));
#endif
    highlightingUsesMultiplyAndLeavesSourceUntouched();
    wrappedTextProducesSeparateMarksAndScalesOrientation();
    ocrLineThicknessUsesSourceUnits();
    largeAnnotationsRetainOnlyActiveGeometry();
    ocrGeometryFollowsSourceAndFontChanges();
    incrementalEditsReuseUnchangedRanges();
    clippedPaintingPreservesTransformedPixels();
    damageExcludesUnchangedInk();
    deferredCoverageIsBoundedAndSafeToExport();
    deferredBoundsCoverEveryStrokeStyle();
    return 0;
}
