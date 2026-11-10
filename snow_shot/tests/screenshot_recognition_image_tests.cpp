#include "snow_shot/presentation/screenshotrecognitionimage.h"
#include "snow_shot/presentation/screenshotocrtextlayout.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "snow_shot/presentation/screenshotselectionshadowrenderer.h"
#include "../../test-support/virtualmemory.h"
#include <QGuiApplication>
#include <QFontDatabase>
#include <QPainter>
#include <QThread>
#include <future>
#include <iostream>
#include <stdexcept>
#include <limits>
#include <algorithm>
#include <cmath>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
ScreenshotRecognitionImageSnapshot fixture() {
    ScreenshotRecognitionImageSnapshot snapshot;
    snapshot.image = QImage(320, 180, QImage::Format_ARGB32_Premultiplied);
    snapshot.image.fill(QColor(230, 240, 250));
    snapshot.canvasRect = QRectF(640, 360, 320, 180);
    snapshot.font = QGuiApplication::font();
    snapshot.textColor = Qt::black;
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Translated result");
    line.quad = {{660, 380}, {920, 380}, {920, 420}, {660, 420}};
    snapshot.lines.push_back(line);
    return snapshot;
}

qreal fittedFontSize(const ScreenshotOcrLine& line, const QTransform& output = {}) {
    ScreenshotOcrTextLayout layout;
    QTransform transform;
    qreal size = 0;
    require(configureScreenshotOcrTextLayout(layout, line, output, QGuiApplication::font(),
                                             Qt::black, {}, &transform, &size),
            "font-size fixture must produce a valid layout");
    require(std::abs(size - layout.fontPixelSize() * std::hypot(transform.m11(), transform.m12())) <
                0.0001,
            "reported font size must include the actual glyph and quad transforms");
    return size;
}

void translationsNeverExceedTheirSourceFontSize() {
    for (const bool vertical : {false, true}) {
        for (const qreal angle : {0.0, 27.0}) {
            auto original = fixture().lines[0];
            original.text = vertical ? QString(18, QChar(0x7530)) : QString(32, QLatin1Char('M'));
            if (vertical) {
                original.direction = ScreenshotOcrTextDirection::Vertical;
                original.quad = {{20, 20}, {60, 20}, {60, 140}, {20, 140}};
            }
            original.quad = QTransform().rotate(angle).map(original.quad);
            const qreal baseline = fittedFontSize(original);
            QVector<ScreenshotOcrLine> translated{original};
            setScreenshotOcrTranslationFontCaps(translated, {original}, {{0}},
                                                QGuiApplication::font());
            require(translated[0].fontSizeCapRatio && !original.fontSizeCapRatio,
                    "only the translation owns a source font cap");
            for (const QString& text : {QStringLiteral("M"), QString(QChar(0x7530)), original.text,
                                        original.text.repeated(4)}) {
                translated[0].text = text;
                for (const qreal scale : {0.5, 1.0, 1.25, 2.0}) {
                    const auto output = QTransform::fromScale(scale, scale);
                    require(fittedFontSize(translated[0], output) <= baseline * scale + 0.0001,
                            "translation size must remain capped after zoom, rotation and shaping");
                }
            }
            translated[0].text = QStringLiteral("M");
            auto uncapped = translated[0];
            uncapped.fontSizeCapRatio.reset();
            require(fittedFontSize(uncapped) > baseline * 1.5,
                    "short-text regression must exercise enlargement in the uncapped fitter");
            require(std::abs(fittedFontSize(translated[0]) - baseline) < 0.0001,
                    "a short translation can retain the source baseline without filling its box");
            translated[0].text = original.text.repeated(4);
            require(fittedFontSize(translated[0]) < baseline,
                    "long translations may shrink below the cap");
        }
    }
}

void mergedFontCapsUseTheMedianOriginalBaseline() {
    QVector<ScreenshotOcrLine> source;
    QVector<qreal> sizes;
    for (const int count : {20, 40, 30, 50}) {
        auto line = fixture().lines[0];
        line.text = QString(count, QLatin1Char('M'));
        line.quad.translate(0, static_cast<qreal>(source.size()) * 48);
        sizes.push_back(fittedFontSize(line));
        source.push_back(line);
    }
    for (const int count : {3, 4}) {
        auto line = fixture().lines[0];
        line.text = QStringLiteral("M");
        line.paragraph = true;
        line.quad = {{660, 380}, {920, 380}, {920, 380 + count * 48.0}, {660, 380 + count * 48.0}};
        QVector<int> indices;
        for (int index = 0; index < count; ++index) {
            line.sourceLineQuads.push_back(source[index].quad);
            indices.push_back(index);
        }
        QVector<ScreenshotOcrLine> translation{line};
        setScreenshotOcrTranslationFontCaps(translation, source, {indices},
                                            QGuiApplication::font());
        auto sorted = sizes.mid(0, count);
        std::sort(sorted.begin(), sorted.end());
        const qreal median = count == 3 ? sorted[1] : (sorted[1] + sorted[2]) / 2;
        require(translation[0].fontSizeCapRatio &&
                    std::abs(*translation[0].fontSizeCapRatio * count * 48 - median) < 0.0001 &&
                    std::abs(fittedFontSize(translation[0]) - median) < 0.0001,
                "a sparse paragraph uses the median original fitted size, including even counts");
        const auto cap = translation[0].fontSizeCapRatio;
        auto changedSource = source;
        changedSource[0].text = QStringLiteral("different source");
        setScreenshotOcrTranslationFontCaps(translation, changedSource, {indices},
                                            QFont(QStringLiteral("Arial")));
        require(translation[0].fontSizeCapRatio == cap,
                "an established translation baseline is immutable");
    }
}

void sourceRowFontCapsPreserveSlotsAndInvalidateLayouts() {
    ScreenshotOcrLine line;
    line.text = QString(31, QChar(0x7530));
    line.paragraph = true;
    line.quad = {{0, 0}, {300, 0}, {300, 96}, {0, 96}};
    for (const QRectF& row :
         {QRectF(0, 0, 300, 24), QRectF(0, 36, 300, 24), QRectF(0, 72, 180, 24)}) {
        line.sourceLineQuads.push_back(
            {row.topLeft(), row.topRight(), row.bottomRight(), row.bottomLeft()});
    }
    ScreenshotOcrTextLayout layout;
    QTransform transform;
    qreal originalSize = 0;
    require(configureScreenshotOcrTextLayout(layout, line, {}, QGuiApplication::font(), Qt::black,
                                             {}, &transform, &originalSize) &&
                layout.usesSourceRows(),
            "source-row fixture must use fixed original slots");
    const QTransform originalTransform = transform;
    for (const qreal factor : {0.9, 0.8}) {
        line.fontSizeCapRatio = originalSize * factor / 96;
        qreal capped = 0;
        require(configureScreenshotOcrTextLayout(layout, line, {}, QGuiApplication::font(),
                                                 Qt::black, {}, &transform, &capped) &&
                    layout.usesSourceRows(),
                "moderate source-row caps retain the original row layout");
        require(capped <= originalSize * factor + 0.0001 && transform == originalTransform,
                "cap changes reshape glyphs without shrinking or relocating the row slots");
    }
    line.text = QStringLiteral("M");
    qreal capped = 0;
    require(configureScreenshotOcrTextLayout(layout, line, {}, QGuiApplication::font(), Qt::black,
                                             {}, &transform, &capped) &&
                !layout.usesSourceRows() && capped <= originalSize * 0.8 + 0.0001,
            "sparse translations remain capped when source rows fall back to paragraph fitting");
}

void streamingTextKeepsItsFirstGlyphInPlace() {
    for (const bool paragraph : {false, true}) {
        ScreenshotOcrLine line;
        line.paragraph = paragraph;
        const qreal height = paragraph ? 96 : 24;
        line.quad = {{0, 0}, {180, 0}, {180, height}, {0, height}};
        line.fontSizeCapRatio = 16 / height;
        const qreal left = paragraph ? 20 : 0;
        if (paragraph) {
            line.sourceLineQuads = {{{left, 0}, {180, 0}, {180, 24}, {left, 24}},
                                    {{0, 36}, {180, 36}, {180, 60}, {0, 60}},
                                    {{0, 72}, {150, 72}, {150, 96}, {0, 96}}};
        }
        ScreenshotOcrTextLayout layout;
        QTransform transform;
        const auto configure = [&] {
            require(configureScreenshotOcrTextLayout(layout, line, {}, QGuiApplication::font(),
                                                     Qt::black, {}, &transform),
                    "configure the live translation layout");
        };
        const auto characterRect = [&](int index) {
            const auto fragments = layout.rangeFragments({index, 1});
            require(fragments.size() == 1, "one Latin character occupies one selection fragment");
            const auto& fragment = fragments.first();
            return (fragment.localToSource * transform).mapRect(fragment.rect);
        };
        line.text = QStringLiteral("Hi");
        configure();
        const QRectF finalFirst = characterRect(0);
        line.translationStreaming = true;
        configure();
        const QRectF first = characterRect(0);
        require(std::abs(first.left() - left) < 2 && first.top() < 12 && finalFirst != first,
                "partial text starts at the left of the first source row");
        line.text += QStringLiteral(" there");
        configure();
        require(QLineF(first.topLeft(), characterRect(0).topLeft()).length() < 0.5 &&
                    characterRect(1).right() < first.right() + 15,
                "new letters keep the prefix anchored and naturally spaced");
        if (paragraph) {
            line.text += QStringLiteral(" friend. The rest of the text wraps onto the next line.");
            configure();
            require(QLineF(first.topLeft(), characterRect(0).topLeft()).length() < 0.5 &&
                        characterRect(static_cast<int>(line.text.size()) - 1).top() >
                            first.bottom(),
                    "streaming wraps below the first row without rebalancing the prefix");
        }
        line.text = QStringLiteral("Hi");
        configure();
        line.translationStreaming = false;
        configure();
        require(characterRect(0) == finalFirst,
                "completion with identical text restores the original fitted layout");
    }
}

void streamingVerticalTextGrowsDownTheColumn() {
    ScreenshotOcrLine line;
    line.quad = {{0, 0}, {24, 0}, {24, 180}, {0, 180}};
    line.direction = ScreenshotOcrTextDirection::Vertical;
    line.fontSizeCapRatio = 0.6;
    ScreenshotOcrTextLayout layout;
    const auto bounds = [&] {
        QTransform transform;
        require(configureScreenshotOcrTextLayout(layout, line, {}, QGuiApplication::font(),
                                                 Qt::black, {}, &transform),
                "configure the vertical translation layout");
        return transform.mapRect(layout.boundingRect());
    };
    line.translationStreaming = true;
    line.text = QString(QChar(0x7530));
    const auto first = bounds();
    line.text += line.text;
    const auto second = bounds();
    require(std::abs(first.top()) < 0.01 && std::abs(second.top()) < 0.01 &&
                std::abs(second.height() - 2 * first.height()) < 0.01,
            "vertical streaming adds a natural cell below the first one");
    line.translationStreaming = false;
    require(bounds().height() > second.height(), "the final column resumes fitted spacing");
}

void streamingTypeMatchesTheRenderedOcr() {
    struct Sample {
        qreal fontSize;
        QPointF start;
        qreal advance;
    };
    ScreenshotOcrTextLayout layout;
    const auto measure = [&](const ScreenshotOcrLine& line, qreal zoom) {
        QTransform transform;
        qreal size = 0;
        require(configureScreenshotOcrTextLayout(layout, line, QTransform::fromScale(zoom, zoom),
                                                 QGuiApplication::font(), Qt::black, {}, &transform,
                                                 &size),
                "fit the typography sample");
        const auto origin = [&](int index) {
            const auto fragments = layout.rangeFragments({index, 1});
            require(fragments.size() == 1, "measure a single shaped character");
            const auto& cell = fragments.first();
            return (cell.localToSource * transform).mapRect(cell.rect).topLeft();
        };
        return Sample{size, origin(0), QLineF(origin(1), origin(2)).length()};
    };
    for (const bool vertical : {false, true}) {
        ScreenshotOcrLine original;
        original.text = QString(6, vertical ? QChar(0x7530) : QChar(u'H'));
        original.quad = vertical ? QPolygonF{{0, 0}, {24, 0}, {24, 300}, {0, 300}}
                                 : QPolygonF{{0, 0}, {300, 0}, {300, 24}, {0, 24}};
        original.direction = vertical ? ScreenshotOcrTextDirection::Vertical
                                      : ScreenshotOcrTextDirection::Horizontal;
        QVector<ScreenshotOcrLine> translated{original};
        setScreenshotOcrTranslationFontCaps(translated, {original}, {{0}}, QGuiApplication::font());
        auto& line = translated[0];
        for (const qreal zoom : {1.0, 2.0}) {
            const auto source = measure(original, zoom);
            require(source.advance > source.fontSize * 1.4, "fixture has expanded OCR spacing");
            line.text = original.text.left(3);
            line.translationStreaming = false;
            const auto final = measure(line, zoom);
            line.translationStreaming = true;
            const auto prefix = measure(line, zoom);
            require(std::abs(prefix.fontSize - source.fontSize) < zoom &&
                        std::abs(prefix.advance - source.advance) < zoom,
                    "partial translation uses the rendered OCR font size and character advance");
            line.text += original.text.left(2);
            const auto more = measure(line, zoom);
            require(QLineF(more.start, prefix.start).length() < 0.5 &&
                        std::abs(more.advance - prefix.advance) < 0.5 &&
                        std::abs(more.fontSize - prefix.fontSize) < 0.01,
                    "another delta retains the source typography and starting edge");
            line.text = original.text.left(3);
            line.translationStreaming = false;
            const auto completed = measure(line, zoom);
            require(completed.start == final.start && completed.advance == final.advance &&
                        completed.fontSize == final.fontSize,
                    "completion restores the fitted layout of the same translation");
        }
    }

    QVector<ScreenshotOcrLine> sourceRows;
    ScreenshotOcrLine paragraph;
    paragraph.paragraph = true;
    paragraph.quad = {{0, 0}, {300, 0}, {300, 96}, {0, 96}};
    for (const int width : {300, 240, 180}) {
        ScreenshotOcrLine row;
        row.text = QStringLiteral("HHHHHH");
        row.quad = {{0, 0}, {qreal(width), 0}, {qreal(width), 24}, {0, 24}};
        row.quad.translate(0, static_cast<qreal>(sourceRows.size()) * 36);
        sourceRows.append(row);
        paragraph.sourceLineQuads.append(row.quad);
    }
    QVector<ScreenshotOcrLine> translated{paragraph};
    setScreenshotOcrTranslationFontCaps(translated, sourceRows, {{0, 1, 2}},
                                        QGuiApplication::font());
    translated[0].translationStreaming = true;
    translated[0].text = QStringLiteral("HHH");
    const auto preview = measure(translated[0], 1);
    const auto middleRow = measure(sourceRows[1], 1);
    require(std::abs(preview.advance - middleRow.advance) < 1 &&
                std::abs(preview.fontSize - middleRow.fontSize) < 1,
            "a merged paragraph streams at the median source size and spacing");
}

void cappedSnapshotsMatchDirectPaintingAtEveryResolution() {
    auto snapshot = fixture();
    snapshot.lines[0].text = QString(32, QLatin1Char('M'));
    const auto source = snapshot.lines;
    setScreenshotOcrTranslationFontCaps(snapshot.lines, source, {{0}}, snapshot.font);
    snapshot.lines[0].text = QStringLiteral("M");
    for (const qreal scale : {0.5, 1.25, 2.0}) {
        auto scaled = snapshot;
        scaled.image = snapshot.image.scaled(qRound(320 * scale), qRound(180 * scale));
        scaled.image.setDevicePixelRatio(1.75);
        QImage expected = scaled.image.copy();
        expected.setDevicePixelRatio(1);
        QPainter painter(&expected);
        ScreenshotOcrTextLayout layout;
        QTransform textTransform;
        const QTransform output(scale, 0, 0, scale, -640 * scale, -360 * scale);
        require(configureScreenshotOcrTextLayout(layout, scaled.lines[0], output, scaled.font,
                                                 scaled.textColor, {}, &textTransform),
                "capped export fixture must fit");
        painter.setTransform(textTransform);
        layout.paint(&painter);
        painter.end();
        const auto rendered = std::async(std::launch::async, [scaled] {
                                  return renderScreenshotRecognitionImage(scaled);
                              }).get();
        require(rendered == expected,
                "worker export must preserve the cap and match direct painting");
        auto uncapped = scaled;
        uncapped.lines[0].fontSizeCapRatio.reset();
        require(rendered != renderScreenshotRecognitionImage(uncapped),
                "export must not silently lose the font cap from its snapshot");
    }
}
void rendersNativeCoordinatesAndPreservesSource() {
    auto snapshot = fixture();
    snapshot.image.setDevicePixelRatio(2.0);
    const QImage source = snapshot.image;
    const QImage rendered = renderScreenshotRecognitionImage(snapshot);
    require(rendered.size() == source.size() && rendered.devicePixelRatio() == 1.0,
            "export retains physical image resolution regardless of display DPR");
    require(snapshot.image == source, "worker render must not mutate source image");
    require(rendered.pixelColor(0, 0) == source.pixelColor(0, 0),
            "non-text image pixels are preserved");
    int textPixels = 0;
    for (int y = 0; y < rendered.height(); ++y) {
        for (int x = 0; x < rendered.width(); ++x) {
            if (rendered.pixelColor(x, y) != source.pixelColor(x, y)) {
                require(QRect(20, 20, 260, 40).contains(x, y), "text stays in its output quad");
                ++textPixels;
            }
        }
    }
    require(textPixels > 100, "recognized text is rasterized");
    snapshot.lines.clear();
    require(renderScreenshotRecognitionImage(snapshot).pixelColor(70, 30) ==
                source.pixelColor(70, 30),
            "pending and empty recognition preserve the currently displayed source");
}
void displayedBackgroundAndEffectsAreAppliedOnce() {
    auto snapshot = fixture();
    snapshot.lines.clear();
    snapshot.filteredImage = QImage(40, 20, QImage::Format_ARGB32_Premultiplied);
    snapshot.filteredImage.fill(Qt::green);
    snapshot.filteredCanvasRect = QRectF(680, 380, 40, 20);
    QImage image = renderScreenshotRecognitionImage(snapshot);
    require(image.pixelColor(45, 25) == QColor(Qt::green) &&
                image.pixelColor(39, 25) == snapshot.image.pixelColor(39, 25),
            "displayed filtered patch uses canvas coordinates without covering other image pixels");
    snapshot.resultStyle = {16, 8, Qt::black};
    image = renderScreenshotRecognitionImage(snapshot);
    require(image.size() == QSize(336, 196) && image.pixelColor(53, 33) == QColor(Qt::green),
            "shadow padding is applied exactly once after recognition composition");
    require(image.pixelColor(8, 8).alpha() < 255, "rounded result corners preserve transparency");
}
void effectsScaleOnceForEverySelectionShape() {
    for (const auto type :
         {ScreenshotSelectionBorderType::Shadow, ScreenshotSelectionBorderType::Border}) {
        for (const qreal scale : {0.5, 1.25, 2.0}) {
            for (const bool shaped : {false, true}) {
                auto snapshot = fixture();
                snapshot.lines.clear();
                snapshot.image = snapshot.image.scaled(qRound(320 * scale), qRound(180 * scale));
                snapshot.resultStyle = {16, 48, Qt::black};
                snapshot.resultStyle.borderType = type;
                snapshot.resultStyle.borderWidth = 48;
                if (shaped)
                    snapshot.resultStyle.region =
                        QRegion(0, 0, 320, 180).subtracted(QRect(100, 40, 120, 100));
                const auto rendered = renderScreenshotRecognitionImage(snapshot);
                const int padding = qRound(48 * scale);
                require(
                    rendered.size() == snapshot.image.size() + QSize(2 * padding, 2 * padding),
                    "recognition effects scale from canvas units once for every selection shape");
                require(rendered == ScreenshotResultCompositor::compose(
                                        snapshot.image, ScreenshotResultCompositor::scaledStyle(
                                                            snapshot.resultStyle, scale)),
                        "recognition and ordinary exports must use the same output style");
            }
        }
    }
}

void compoundSelectionClipsRecognizedText() {
    auto snapshot = fixture();
    snapshot.resultStyle.region = QRegion(0, 0, 320, 180).subtracted(QRegion(80, 0, 80, 100));
    const auto frozen = snapshot;
    snapshot.resultStyle.region = QRegion(0, 0, 320, 180);
    const QImage rendered = renderScreenshotRecognitionImage(frozen);
    require(rendered.pixelColor(100, 30).alpha() == 0 &&
                rendered.pixelColor(20, 100).alpha() == 255,
            "recognition exports clip source and recognized text with frozen geometry");
    auto scaled = frozen;
    scaled.image = scaled.image.scaled(640, 360);
    const QImage large = renderScreenshotRecognitionImage(scaled);
    require(large.pixelColor(200, 60).alpha() == 0 && large.pixelColor(40, 200).alpha() == 255,
            "recognition shape follows image backing scale");
}

void compactionFollowsRecognitionRendering() {
    for (auto type :
         {ScreenshotSelectionBorderType::Shadow, ScreenshotSelectionBorderType::Border}) {
        for (int shadow : {0, 3, 6}) {
            for (qreal scale : {1.0, 1.5, 2.0}) {
                auto snapshot = fixture();
                snapshot.image = snapshot.image.scaled(qRound(320 * scale), qRound(180 * scale));
                snapshot.resultStyle = {
                    6, shadow, Qt::black,
                    ScreenshotRegionGeometry(
                        QRegion(0, 0, 100, 180).united(QRegion(220, 0, 100, 180)))};
                snapshot.resultStyle.borderType = type;
                snapshot.resultStyle.borderWidth = shadow;
                if (type == ScreenshotSelectionBorderType::Border)
                    snapshot.resultStyle.shadowWidth = 11;
                const auto reference = renderScreenshotRecognitionImage(snapshot);
                snapshot.layoutCompaction = {true, 4};
                const auto originalQuad = snapshot.lines.first().quad;
                const auto actual = renderScreenshotRecognitionImage(snapshot);
                const int padding = qRound(shadow * scale);
                const QSize footprint(qRound(100 * scale) + 2 * padding,
                                      qRound(180 * scale) + 2 * padding);
                const int spacing = qRound(4 * scale);
                QImage expected(2 * footprint.width() + spacing, footprint.height(),
                                QImage::Format_ARGB32_Premultiplied);
                expected.fill(Qt::transparent);
                QPainter painter(&expected);
                painter.drawImage(QPoint(), reference, QRect(QPoint(), footprint));
                painter.drawImage(QPoint(footprint.width() + spacing, 0), reference,
                                  QRect(QPoint(qRound(220 * scale), 0), footprint));
                painter.end();
                require(actual == expected && snapshot.lines.first().quad == originalQuad,
                        "recognition compaction preserves text, active effect pixels and spacing");
            }
        }
    }
}

void completedRenderingReleasesCompositorCaches() {
    auto snapshot = fixture();
    snapshot.resultStyle = {
        6, 3, Qt::black,
        ScreenshotRegionGeometry(QRegion(0, 0, 100, 180).united(QRegion(220, 0, 100, 180)))};
    snapshot.layoutCompaction = {true, 4};
    const auto verify = [snapshot] {
        const auto rendered = renderScreenshotRecognitionImage(snapshot);
        const auto caches = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
        require(!rendered.isNull() && caches.retainedBytes == 0 &&
                    caches.regionCacheRetainedBytes == 0 && caches.regionPathCacheElements == 0 &&
                    !screenshotLayoutCompactionCache.plan,
                "completed recognition rendering releases worker compositor caches");
        require(renderScreenshotRecognitionImage(snapshot, [] { return true; }).isNull(),
                "canceled recognition rendering publishes no image");
        require(!screenshotLayoutCompactionCache.plan,
                "canceled rendering leaves no worker layout cache");
    };
    std::async(std::launch::async, verify).get();
}

void retainedBackgroundUsesOriginalCapture() {
    for (bool compact : {false, true}) {
        auto snapshot = fixture();
        snapshot.resultStyle = {
            12, 6, Qt::black,
            ScreenshotRegionGeometry(QRegion(0, 0, 100, 180) + QRect(220, 0, 100, 180))};
        snapshot.resultStyle.backgroundOpacityPercent = 50;
        snapshot.layoutCompaction = {compact, 8};
        QImage capture(360, 220, QImage::Format_ARGB32_Premultiplied);
        capture.fill(QColor(80, 140, 220));
        snapshot.backgroundSources = {{capture, snapshot.canvasRect.adjusted(-20, -20, 20, 20)}};
        snapshot.filteredImage = snapshot.image;
        snapshot.filteredImage.fill(Qt::green);
        snapshot.filteredCanvasRect = snapshot.canvasRect;
        const auto result = renderScreenshotRecognitionImage(snapshot);
        require(!result.isNull(), "recognition result with retained background renders");
        const int gapX = compact ? 116 : 166;
        const auto gap = result.pixelColor(gapX, 150);
        require(
            gap.blue() > gap.green() && gap.alpha() > 0 && gap.alpha() < 200,
            "recognition export gaps use capture pixels, not recognized text or filtered patches");
        require(result.pixelColor(50, 150) == QColor(Qt::green),
                "recognition result inside selected regions keeps full opacity");
        require(result.pixelColor(0, 0).alpha() > 0,
                "recognition exports retain captured pixels beyond the selection bounds");
    }
}

void layoutModesAndTransformsRenderOnWorkers() {
    for (bool vertical : {false, true}) {
        for (bool paragraph : {false, true}) {
            auto snapshot = fixture();
            snapshot.lines[0].direction = vertical ? ScreenshotOcrTextDirection::Vertical
                                                   : ScreenshotOcrTextDirection::Horizontal;
            snapshot.lines[0].paragraph = paragraph;
            snapshot.lines[0].text =
                QString::fromUtf8("繁體文字 Á العربية 👩‍💻");
            snapshot.lines[0].quad = {{675, 375}, {820, 390}, {805, 525}, {660, 510}};
            const QImage expected = renderScreenshotRecognitionImage(snapshot);
            auto result = std::async(std::launch::async, [snapshot]() {
                return renderScreenshotRecognitionImage(snapshot);
            });
            require(
                !expected.isNull() && result.get() == expected,
                "Unicode, vertical and paragraph layout are deterministic on independent workers");
            if (expected == snapshot.image)
                std::cerr << "empty layout vertical=" << vertical << " paragraph=" << paragraph
                          << '\n';
            require(expected != snapshot.image, "transformed Unicode text produces visible pixels");
        }
    }
}
void sourceRowsSurviveImageExportOnWorkers() {
    auto snapshot = fixture();
    snapshot.image = QImage(360, 160, QImage::Format_ARGB32_Premultiplied);
    snapshot.image.fill(Qt::white);
    snapshot.canvasRect = QRectF(-180, -80, 360, 160);
    auto& line = snapshot.lines[0];
    line.text = QString(31, QChar(0x7530));
    line.paragraph = true;
    line.quad = QPolygonF(QRectF(-150, -50, 300, 96));
    line.quad.removeLast();
    for (const QRectF& row :
         {QRectF(-150, -50, 300, 24), QRectF(-150, -14, 300, 24), QRectF(-150, 22, 180, 24)}) {
        QPolygonF quad(row);
        quad.removeLast();
        line.sourceLineQuads.push_back(quad);
    }
    const QImage expected = renderScreenshotRecognitionImage(snapshot);
    auto worker = std::async(std::launch::async,
                             [snapshot]() { return renderScreenshotRecognitionImage(snapshot); });
    require(worker.get() == expected, "source row export is deterministic on a worker thread");
    const auto inkBounds = [&](const QRect& region) {
        QRect bounds;
        for (int y = region.top(); y <= region.bottom(); ++y) {
            for (int x = region.left(); x <= region.right(); ++x) {
                if (expected.pixelColor(x, y).red() < 80)
                    bounds = bounds.united(QRect(x, y, 1, 1));
            }
        }
        return bounds;
    };
    for (const QRect& row :
         {QRect(30, 30, 300, 24), QRect(30, 66, 300, 24), QRect(30, 102, 180, 24)}) {
        const QRect ink = inkBounds(row);
        require(ink.width() >= row.width() * 0.9 && ink.height() >= row.height() * 0.65,
                "image export preserves the full source rows and shorter final row");
    }
    require(inkBounds(QRect(30, 56, 300, 8)).isEmpty() &&
                inkBounds(QRect(30, 92, 300, 8)).isEmpty() &&
                inkBounds(QRect(212, 102, 118, 24)).isEmpty(),
            "image export preserves source line gaps and the shorter final row boundary");
    line.sourceLineQuads.clear();
    const QImage fallback = renderScreenshotRecognitionImage(snapshot);
    line.sourceLineQuads = {line.quad, line.quad};
    require(renderScreenshotRecognitionImage(snapshot) == fallback,
            "image export falls back to paragraph layout for overlapping source rows");
}
void largeWorkerOutputRetainsManagedOwnership() {
    auto snapshot = fixture();
    snapshot.image = snowCanvasAllocateImage(QSize(1025, 513), QImage::Format_ARGB32_Premultiplied);
    require(!snapshot.image.isNull(), "large recognition fixture must allocate");
    snapshot.image.fill(Qt::white);
    snapshot.image.setDevicePixelRatio(2.0);
    const auto* original = snapshot.image.constBits();
    auto worker = std::async(std::launch::async,
                             [snapshot] { return renderScreenshotRecognitionImage(snapshot); });
    QImage rendered = worker.get();
    require(!rendered.isNull() && rendered.constBits() != original &&
                snapshot.image.constBits() == original && snapshot.image.devicePixelRatio() == 2.0,
            "recognition must detach into owned output without altering its capture lease");
    const auto* middle = rendered.constBits() + rendered.sizeInBytes() / 2;
    require(snow::test_support::virtualMemoryMapped(middle),
            "recognition output must own live mapped storage");
    rendered = {};
    require(!snow::test_support::virtualMemoryMapped(middle),
            "completed worker output must release its final pixel pages");
}

void cancellationAndInvalidInputNeverPublishPartialImages() {
    auto snapshot = fixture();
    int polls = 0;
    require(renderScreenshotRecognitionImage(snapshot, [&]() { return ++polls >= 2; }).isNull(),
            "mid-render cancellation discards partial output");
    snapshot.canvasRect = {};
    require(renderScreenshotRecognitionImage(snapshot).isNull(),
            "invalid geometry fails rendering");
    snapshot = fixture();
    snapshot.lines[0].quad[0].setX(std::numeric_limits<qreal>::quiet_NaN());
    require(renderScreenshotRecognitionImage(snapshot) == snapshot.image,
            "malformed text geometry cannot corrupt the exported image");
}
} // namespace
int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
#if defined(Q_OS_WIN)
    if (QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) < 0 ||
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc")) < 0)
        return 3;
    QGuiApplication::setFont(QFont(QStringLiteral("Segoe UI")));
#endif

    try {
        translationsNeverExceedTheirSourceFontSize();
        mergedFontCapsUseTheMedianOriginalBaseline();
        sourceRowFontCapsPreserveSlotsAndInvalidateLayouts();
        streamingTextKeepsItsFirstGlyphInPlace();
        streamingVerticalTextGrowsDownTheColumn();
        streamingTypeMatchesTheRenderedOcr();
        cappedSnapshotsMatchDirectPaintingAtEveryResolution();
        rendersNativeCoordinatesAndPreservesSource();
        displayedBackgroundAndEffectsAreAppliedOnce();
        effectsScaleOnceForEverySelectionShape();
        compoundSelectionClipsRecognizedText();
        compactionFollowsRecognitionRendering();
        completedRenderingReleasesCompositorCaches();
        retainedBackgroundUsesOriginalCapture();
        layoutModesAndTransformsRenderOnWorkers();
        sourceRowsSurviveImageExportOnWorkers();
        cancellationAndInvalidInputNeverPublishPartialImages();
        largeWorkerOutputRetainsManagedOwnership();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
