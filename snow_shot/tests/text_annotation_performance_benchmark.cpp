// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshottextannotationgeometry.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include <QApplication>
#include <QAbstractTextDocumentLayout>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextLayout>
#include <QPainter>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(EXIT_FAILURE);
    }
}

template <typename Work> double medianMilliseconds(Work work) {
    std::vector<double> samples;
    for (int i = 0; i < 5; ++i) {
        QElapsedTimer timer;
        timer.start();
        work();
        samples.push_back(timer.nsecsElapsed() / 1e6);
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

void longSelectionGeometry() {
    for (const int characters : {24000, 96000}) {
        QTextDocument document;
        QString text = QStringLiteral("The quick brown fox jumps over the lazy dog. ")
                           .repeated(characters / 44 + 1);
        text.truncate(characters);
        document.setPlainText(text);
        document.setTextWidth(640);
        static_cast<void>(document.documentLayout()->documentSize());
        const double selection = medianMilliseconds([&] {
            require(
                !screenshotFormattedTextSelectionBounds(document, {{0, 0, characters}}).isEmpty(),
                "measure complete wrapped selection geometry");
        });
        const double lateRange = medianMilliseconds([&] {
            require(!screenshotFormattedTextSelectionBounds(document,
                                                            {{0, characters - 10, characters}})
                         .isEmpty(),
                    "measure a small selection near the end of a long wrapped paragraph");
        });
        std::printf("geometry characters=%d rows=%d select_all_ms=%.3f late_range_ms=%.3f\n",
                    characters, document.begin().layout()->lineCount(), selection, lateRange);
    }
}

void retainedStrokeGeometry() {
    QTextDocument document;
    document.setPlainText(
        QStringLiteral("The quick brown fox jumps over the lazy dog. Selected text.\n")
            .repeated(500));
    document.setTextWidth(640);
    ScreenshotTextAnnotationDocument annotations;
    annotations.bindSource(ScreenshotTextAnnotationSource::Formatted,
                           screenshotFormattedTextAnnotationFingerprint(document));
    const QVector<ScreenshotTextSelectionRange> ranges{{0, 0, document.characterCount() - 1}};
    ScreenshotTextAnnotationStyle style;
    annotations.toggle(ranges, ScreenshotTextAnnotationTool::WavyUnderline, style);
    ScreenshotTextAnnotationRenderer renderer;
    auto snapshot = renderer.formattedSnapshot(annotations, document);
    const auto initial = renderer.diagnostics();
    const double recolour = medianMilliseconds([&] {
        style.color = style.color == QColor(Qt::red) ? QColor(Qt::blue) : QColor(Qt::red);
        annotations.updateStyle(ranges, ScreenshotTextAnnotationTool::WavyUnderline, style,
                                ScreenshotTextAnnotationStylePropertyColor);
        snapshot = renderer.formattedSnapshot(annotations, document);
    });
    const auto final = renderer.diagnostics();
    require(initial.strokePreparations == final.strokePreparations &&
                initial.fragmentBuilds == final.fragmentBuilds,
            "colour previews must not build stroke or fragment geometry");
    std::printf(
        "retained_geometry rows=500 description_MiB=%.2f recolour_ms=%.3f "
        "extra_stroke_preparations=%llu\n",
        final.geometryBytes / 1048576.0, recolour,
        static_cast<unsigned long long>(final.strokePreparations - initial.strokePreparations));
}

void incrementalOcrGeometry() {
    for (const int rows : {100, 500, 2000}) {
        ScreenshotOcrPresentation source;
        source.selection = QRect(0, 0, 640, 24 * (rows + 1));
        QVector<ScreenshotTextSelectionRange> selected;
        for (int i = 0; i <= rows; ++i) {
            ScreenshotOcrLine line;
            line.text =
                QStringLiteral("The quick brown fox jumps over the lazy dog. Row %1").arg(i);
            line.quad = QPolygonF{QPointF(0, i * 24), QPointF(620, i * 24),
                                  QPointF(620, i * 24 + 22), QPointF(0, i * 24 + 22)};
            source.lines.append(line);
            if (i < rows)
                selected.append({i, 0, static_cast<int>(line.text.size())});
        }
        source.prepareForRendering();
        ScreenshotTextAnnotationDocument annotations;
        annotations.bindSource(ScreenshotTextAnnotationSource::Ocr,
                               screenshotOcrTextAnnotationFingerprint(source));
        ScreenshotTextAnnotationStyle style;
        annotations.toggle(selected, ScreenshotTextAnnotationTool::WavyUnderline, style);
        for (bool transformed : {false, true}) {
            const auto mapping = transformed ? QTransform::fromScale(.5, .5) : QTransform();
            ScreenshotTextAnnotationRenderer renderer;
            auto snapshot = renderer.ocrSnapshot(annotations, source, mapping);
            const auto initial = renderer.diagnostics();
            const double edit = medianMilliseconds([&] {
                annotations.toggle({{rows, 0, 10}}, ScreenshotTextAnnotationTool::WavyUnderline,
                                   style);
                auto next = renderer.ocrSnapshot(annotations, source, mapping);
                require(!screenshotTextAnnotationDamage(snapshot, next).isEmpty(),
                        "include canvas damage calculation in incremental edit timing");
                snapshot = std::move(next);
            });
            const auto final = renderer.diagnostics();
            require(final.fragmentBuilds - initial.fragmentBuilds == 3 &&
                        snapshot.items.size() == rows + 1,
                    "five incremental toggles prepare only the three newly added ranges");
            std::printf(
                "incremental_ocr rows=%d transformed=%d wavy_edit_and_damage_ms=%.3f "
                "extra_fragment_builds=%llu\n",
                rows, int(transformed), edit,
                static_cast<unsigned long long>(final.fragmentBuilds - initial.fragmentBuilds));
            annotations.toggle({{rows, 0, 10}}, ScreenshotTextAnnotationTool::WavyUnderline, style);
        }
    }
}

void clippedAnnotationPainting() {
    for (const int rows : {500, 2000}) {
        QTextDocument text;
        text.setPlainText(
            QStringLiteral("The quick brown fox jumps over the lazy dog. Selected text.\n")
                .repeated(rows));
        text.setTextWidth(640);
        ScreenshotTextAnnotationDocument annotations;
        annotations.bindSource(ScreenshotTextAnnotationSource::Formatted,
                               screenshotFormattedTextAnnotationFingerprint(text));
        annotations.toggle({{0, 0, text.characterCount() - 1}},
                           ScreenshotTextAnnotationTool::WavyUnderline, {});
        const auto snapshot = screenshotFormattedTextAnnotationSnapshot(annotations, text);
        auto expanded = snapshot;
        for (auto& item : expanded.items) {
            item.path = item.coverage();
            item.stroke.reset();
        }
        ScreenshotTextAnnotationPaintCache cache;
        QImage output(800, 400, QImage::Format_ARGB32_Premultiplied);
        const auto paint = [&](bool cull) {
            output.fill(Qt::white);
            QPainter painter(&output);
            painter.setClipRect(QRect(0, 0, 400, 80));
            if (cull) {
                paintScreenshotTextAnnotations(painter, snapshot, {}, &cache);
            } else {
                // Reference the former submission of every path, keeping the same raster clip.
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setPen(Qt::NoPen);
                for (const auto& item : expanded.items) {
                    painter.setBrush(item.color);
                    painter.drawPath(item.path);
                }
            }
        };
        const double uncull = medianMilliseconds([&] { paint(false); });
        const QImage reference = output.copy();
        const double culled = medianMilliseconds([&] { paint(true); });
        require(output == reference, "culling clipped wavy ink preserves exact output pixels");
        std::printf("clipped_paint rows=%d all_paths_ms=%.3f culled_ms=%.3f\n", rows, uncull,
                    culled);
    }
}

void coldSnapshotsAndVisibleCoverage() {
    for (int rows : {500, 2000}) {
        QTextDocument text;
        text.setPlainText(
            QStringLiteral("The quick brown fox jumps over the lazy dog. Selected text.\n")
                .repeated(rows));
        text.setTextWidth(640);
        static_cast<void>(text.documentLayout()->documentSize());
        for (auto rendering :
             {SnowCanvasStrokeRenderingStyle::Architect, SnowCanvasStrokeRenderingStyle::Artist,
              SnowCanvasStrokeRenderingStyle::Cartoonist}) {
            ScreenshotTextAnnotationDocument annotations;
            annotations.bindSource(ScreenshotTextAnnotationSource::Formatted,
                                   screenshotFormattedTextAnnotationFingerprint(text));
            ScreenshotTextAnnotationStyle style;
            style.renderingStyle = rendering;
            annotations.toggle({{0, 0, text.characterCount() - 1}},
                               ScreenshotTextAnnotationTool::WavyUnderline, style);
            ScreenshotTextAnnotationRenderSnapshot snapshot;
            ScreenshotTextAnnotationRenderer::Diagnostics descriptions;
            const auto preparation = medianMilliseconds([&] {
                ScreenshotTextAnnotationRenderer renderer;
                snapshot = renderer.formattedSnapshot(annotations, text);
                descriptions = renderer.diagnostics();
            });
            ScreenshotTextAnnotationPaintCache cache;
            QImage output(800, 400, QImage::Format_ARGB32_Premultiplied);
            const auto paint = [&] {
                output.fill(Qt::white);
                QPainter painter(&output);
                painter.setClipRect(output.rect());
                paintScreenshotTextAnnotations(painter, snapshot, {}, &cache);
            };
            const auto firstPaint = medianMilliseconds([&] {
                cache.clear();
                paint();
            });
            const auto beforeRepaint = cache.diagnostics();
            const auto repaint = medianMilliseconds(paint);
            require(beforeRepaint.coverageBuilds == cache.diagnostics().coverageBuilds,
                    "the visible-row cache avoids stroke expansion on repaint");
            std::printf("cold rows=%d style=%d prepare_ms=%.3f first_viewport_ms=%.3f "
                        "repaint_ms=%.3f descriptions_MiB=%.3f visible_cache_MiB=%.3f\n",
                        rows, int(rendering), preparation, firstPaint, repaint,
                        descriptions.geometryBytes / 1048576., beforeRepaint.bytes / 1048576.);
        }
    }
}

void viewportExport() {
    QImage image(7680, 4320, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(180, 180, 180));
    const QRectF full(QPointF(), QSizeF(image.size()));
    ScreenshotPinnedViewportExportSource source{{}, image, full, image.size(), {}, {}, 1};
    source.visibleCanvasRect = QRectF(0, 0, 512, 512);
    QPainterPath path;
    path.addRect(QRectF(40, 40, 180, 20));
    source.textAnnotations.items.append({path, QColor(255, 0, 0, 160), true});
    source.textAnnotations.bounds = path.boundingRect();
    QImage projected;
    QImage baked;
    const double projection = medianMilliseconds([&] {
        projected = renderScreenshotPinnedViewport(source);
        require(!projected.isNull(), "render vector annotations into the requested viewport");
    });
    const double fullCopy = medianMilliseconds([&] {
        auto previous = source;
        previous.backgroundImage =
            renderScreenshotTextAnnotations(image, full, source.textAnnotations);
        previous.textAnnotations = {};
        baked = renderScreenshotPinnedViewport(previous);
        require(!baked.isNull(), "measure the former full-source annotation composition");
    });
    require(projected == baked, "native-scale crop composition preserves exact pixels");
    SnowCanvasRuntime runtime;
    require(
        !runtime
             .applyAnnotationTransaction(
                 R"({"version":1,"operations":[{"type":"rectangle","bounds":[15,15,10,10],"style":{"fill":[0,0,255,255],"stroke":[0,0,0,0]}}]})")
             .isEmpty(),
        "prepare ordinary canvas drawing for the unannotated export benchmark");
    source.documentSession = runtime.serializeDocumentSession();
    source.textAnnotations = {};
    source.visibleCanvasRect = QRectF(0, 0, 3840, 2160);
    const double unannotated = medianMilliseconds([&] {
        const auto output = renderScreenshotPinnedViewport(source);
        require(output.size() == QSize(3840, 2160), "render an unannotated cropped document");
    });
    source.visibleCanvasRect.reset();
    source.textAnnotations.items.append({path, QColor(255, 0, 0, 160), true});
    source.textAnnotations.bounds = path.boundingRect();
    QImage directMixed, preparedMixed;
    const double mixedDirect =
        medianMilliseconds([&] { directMixed = renderScreenshotPinnedViewport(source); });
    const double mixedPrepared = medianMilliseconds([&] {
        auto previous = source;
        previous.backgroundImage =
            renderScreenshotTextAnnotations(image, full, source.textAnnotations);
        previous.textAnnotations = {};
        preparedMixed = renderScreenshotPinnedViewport(previous);
    });
    require(!directMixed.isNull() && directMixed == preparedMixed,
            "direct mixed drawing/text exports preserve the prepared backdrop's exact pixels");
    std::printf("mixed_export source=7680x4320 prepared_ms=%.3f direct_ms=%.3f "
                "avoided_background_MiB=%.2f\n",
                mixedPrepared, mixedDirect, image.sizeInBytes() / 1048576.0);
    std::printf("export source=7680x4320 crop=512x512 full_copy_ms=%.3f projected_ms=%.3f "
                "avoided_source_MiB=%.2f no_text_4k_document_ms=%.3f\n",
                fullCopy, projection, image.sizeInBytes() / 1048576.0, unannotated);
}
} // namespace

int runTextAnnotationPerformanceBenchmark(int argc, char** argv) {
    QApplication application(argc, argv);
#ifdef Q_OS_WIN
    require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) >= 0,
            "load text annotation benchmark font");
    QFont font(QStringLiteral("Segoe UI"));
#else
    QFont font = QApplication::font();
#endif
    font.setPixelSize(18);
    QApplication::setFont(font);
    longSelectionGeometry();
    retainedStrokeGeometry();
    incrementalOcrGeometry();
    clippedAnnotationPainting();
    coldSnapshotsAndVisibleCoverage();
    viewportExport();
    return EXIT_SUCCESS;
}
