#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrvisuals.h"

#include <QCoreApplication>
#include <QPainter>

#include <cstdlib>
#include <algorithm>
#include <cstdio>
#include <limits>

namespace {
int failures = 0;
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        ++failures;
    }
}

ScreenshotOcrLine box(qreal x, qreal y, qreal width, qreal height) {
    ScreenshotOcrLine line;
    line.quad = {{x, y}, {x + width, y}, {x + width, y + height}, {x, y + height}};
    return line;
}

void isolatedContaminationCannotDetermineTheBackground() {
    QImage source(100, 100, QImage::Format_RGB32);
    const QColor background(32, 48, 64);
    source.fill(background);
    // Sparse border/text contamination at the former eight sample positions must not outweigh
    // the surrounding flat panel. Changing a handful of pixels must not change the inferred panel.
    for (const QPoint point : {QPoint(18, 18), QPoint(40, 18), QPoint(62, 18), QPoint(62, 40),
                               QPoint(62, 62), QPoint(40, 62), QPoint(18, 62), QPoint(18, 40)}) {
        source.setPixelColor(point, Qt::red);
    }
    ScreenshotOcrPresentation presentation;
    presentation.lines = {box(20, 20, 40, 40)};
    prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
    require(presentation.lines[0].backgroundFillColor == background,
            "isolated perimeter contamination must not select the foreground color");
}

void splitBackgroundChoosesDominantPaletteColor() {
    QImage source(160, 100, QImage::Format_RGB32);
    const QColor blue(20, 80, 210), dark(30, 40, 50);
    ScreenshotOcrPresentation presentation;
    presentation.lines = {box(20, 20, 120, 60)};
    for (const int majorityWidth : {84, 36}) {
        source.fill(Qt::white);
        QPainter painter(&source);
        painter.fillRect(20, 20, 120, 60, dark);
        painter.fillRect(20, 20, majorityWidth, 60, blue);
        painter.end();
        prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
        require(presentation.lines[0].backgroundFillColor == (majorityWidth > 60 ? blue : dark),
                "interior 70/30 dominance must win over unrelated surrounding white");
    }
    source.fill(blue);
    {
        QPainter painter(&source);
        painter.fillRect(80, 20, 60, 60, dark);
    }
    prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
    require(presentation.lines[0].backgroundFillColor == blue,
            "perimeter support must resolve an equal interior split between existing colors");
    source.fill(Qt::white);
    {
        QPainter painter(&source);
        painter.fillRect(20, 20, 60, 60, blue);
        painter.fillRect(80, 20, 60, 60, dark);
    }
    prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
    const QColor tied = presentation.lines[0].backgroundFillColor;
    require(tied == blue || tied == dark,
            "equal blocks must select a palette color, never a blend");
    std::reverse(presentation.lines[0].quad.begin(), presentation.lines[0].quad.end());
    prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
    require(presentation.lines[0].backgroundFillColor == tied,
            "equal-color tie breaking must not depend on quad winding");
}

void backgroundFillNeverBlurs() {
    QImage source(160, 100, QImage::Format_RGB32);
    source.fill(Qt::white);
    {
        QPainter painter(&source);
        painter.fillRect(80, 0, 80, 100, Qt::black);
    }
    ScreenshotOcrPresentation presentation;
    presentation.lines = {box(20, 20, 120, 60)};
    prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
    const QColor color = presentation.lines[0].backgroundFillColor;
    require(color.isValid() && color.alpha() == 255,
            "Background Fill must always select opaque color");
    const QRegion region = screenshotOcrFilterRegion(presentation, source.rect(), source.size());
    QRect crop;
    QImage rendered =
        renderScreenshotOcrFilteredImage(source, source.rect(), presentation, Qt::red, 1.0, &crop);
    bool solid = true;
    for (int y = 0; y < rendered.height(); ++y) {
        for (int x = 0; x < rendered.width(); ++x) {
            if (region.contains(QPoint(x, y) + crop.topLeft())) {
                solid = solid && rendered.pixelColor(x, y) == color;
            }
        }
    }
    require(solid, "split-background rendering must contain only the selected solid, without tint");
    // Render policy must survive missing color evidence rather than treating invalid QColor as
    // Blur.
    presentation.lines[0].backgroundFillColor = QColor();
    rendered =
        renderScreenshotOcrFilteredImage(source, source.rect(), presentation, Qt::red, 1.0, &crop);
    require(rendered.pixelColor(QPoint(40, 40) - crop.topLeft()) == QColor(Qt::red) &&
                rendered.pixelColor(QPoint(100, 40) - crop.topLeft()) == QColor(Qt::red),
            "missing prepared color in Background Fill must use opaque fallback, never blur");
    prepareScreenshotOcrFillColors(presentation, source, source.rect(), false);
    rendered =
        renderScreenshotOcrFilteredImage(source, source.rect(), presentation, Qt::red, 1.0, &crop);
    require(rendered.pixelColor(QPoint(40, 40) - crop.topLeft()) !=
                rendered.pixelColor(QPoint(100, 40) - crop.topLeft()),
            "explicit Blur must retain its separate blur and tint behavior");
}

void mixedRegionsRetainTheBlurFallback() {
    QImage source(180, 80, QImage::Format_RGB32);
    source.fill(Qt::black);
    ScreenshotOcrPresentation presentation;
    presentation.lines = {box(10, 20, 40, 40), box(120, 20, 40, 40)};
    presentation.lines[0].backgroundFillColor = Qt::red;
    QRect crop;
    const QImage result = renderScreenshotOcrFilteredImage(source, source.rect(), presentation,
                                                           Qt::white, 1.0, &crop);
    require(!result.isNull(), "mixed background rendering must return an image");
    require(result.pixelColor(QPoint(30, 40) - crop.topLeft()) == QColor(Qt::red),
            "a confident region must keep its exact opaque fill");
    require(result.pixelColor(QPoint(140, 40) - crop.topLeft()).red() >= 127,
            "a solid region must not suppress the blur and tint on an uncertain region");
    require(source.pixelColor(30, 40) == QColor(Qt::black), "source must remain immutable");
}

void flatPanelsSurviveTextNoiseAndGeometry() {
    for (const QColor background :
         {QColor(24, 32, 48), QColor(240, 232, 224), QColor(15, 70, 220)}) {
        for (const auto format : {QImage::Format_RGB32, QImage::Format_ARGB32_Premultiplied,
                                  QImage::Format_RGBA8888, QImage::Format_RGB888}) {
            QImage source(240, 160, format);
            source.fill(background);
            ScreenshotOcrPresentation presentation;
            presentation.lines = {box(30, 40, 180, 12), box(30, 80, 12, 60)};
            {
                QPainter painter(&source);
                for (int x = 35; x < 205; x += 8) {
                    painter.fillRect(x, 40, 2, 12, Qt::black);
                }
            }
            prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
            require(presentation.lines[0].backgroundFillColor == background &&
                        presentation.lines[1].backgroundFillColor == background,
                    "thin horizontal/vertical boxes and opaque image formats must infer the panel");
            QTransform rotation;
            rotation.translate(120, 80).rotate(27).translate(-120, -80);
            presentation.lines = {box(50, 60, 140, 20)};
            presentation.lines[0].quad = rotation.map(presentation.lines[0].quad);
            source.fill(background);
            prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
            require(presentation.lines[0].backgroundFillColor == background,
                    "rotated regions must sample normal to their edges");
            std::reverse(presentation.lines[0].quad.begin(), presentation.lines[0].quad.end());
            prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
            require(presentation.lines[0].backgroundFillColor == background,
                    "reversing polygon winding must not reverse the sampling band");
        }
    }
    QImage noisy(240, 160, QImage::Format_RGB32);
    for (int y = 0; y < noisy.height(); ++y) {
        for (int x = 0; x < noisy.width(); ++x) {
            const int noise = ((x * 17 + y * 29 + x * y) % 7) - 3;
            noisy.setPixel(x, y, qRgb(128 + noise, 144 + noise, 160 + noise));
        }
    }
    ScreenshotOcrPresentation presentation;
    presentation.lines = {box(30, 30, 180, 100)};
    prepareScreenshotOcrFillColors(presentation, noisy, noisy.rect(), true);
    const QColor actual = presentation.lines[0].backgroundFillColor;
    require(actual.isValid() && std::abs(actual.red() - 128) <= 1 &&
                std::abs(actual.green() - 144) <= 1 && std::abs(actual.blue() - 160) <= 1,
            "small compression noise straddling histogram bins must converge to the panel color");
}

void complexBackgroundsStillChooseSolidColors() {
    QImage source(160, 100, QImage::Format_RGB32);
    ScreenshotOcrPresentation presentation;
    presentation.lines = {box(20, 20, 120, 60)};
    source.fill(Qt::white);
    {
        QPainter painter(&source);
        painter.fillRect(20, 20, 120, 60, QColor(20, 80, 210));
    }
    prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
    require(presentation.lines[0].backgroundFillColor == QColor(20, 80, 210),
            "a colored label must select its interior background instead of its white surround");
    source.fill(Qt::white);
    {
        QPainter painter(&source);
        painter.fillRect(50, 20, 60, 60, QColor(20, 80, 210));
    }
    prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
    require(presentation.lines[0].backgroundFillColor == QColor(Qt::white) ||
                presentation.lines[0].backgroundFillColor == QColor(20, 80, 210),
            "a nested split panel must choose one existing background color");
    source.fill(Qt::white);
    for (int y = 20; y < 80; ++y) {
        for (int x = 20; x < 140; ++x) {
            source.setPixel(x, y, (x + y) % 2 == 0 ? qRgb(30, 30, 30) : qRgb(230, 230, 230));
        }
    }
    prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
    require(
        presentation.lines[0].backgroundFillColor == QColor(30, 30, 30) ||
            presentation.lines[0].backgroundFillColor == QColor(230, 230, 230),
        "textured interior samples must supply a solid palette color despite flat white surround");
    for (int mode = 0; mode < 2; ++mode) {
        for (int y = 0; y < source.height(); ++y) {
            for (int x = 0; x < source.width(); ++x) {
                const int value =
                    mode == 0 ? x * 255 / source.width() : ((x + y) % 2 == 0 ? 30 : 230);
                source.setPixel(x, y, qRgb(value, value, value));
            }
        }
        prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
        require(presentation.lines[0].backgroundFillColor.isValid() &&
                    presentation.lines[0].backgroundFillColor.alpha() == 255,
                "gradients and textures still require opaque colors in Background Fill");
    }
    source = QImage(160, 100, QImage::Format_ARGB32_Premultiplied);
    source.fill(QColor(100, 140, 200, 128));
    prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
    require(presentation.lines[0].backgroundFillColor.rgb() == source.pixelColor(40, 40).rgb(),
            "partially transparent input must use unpremultiplied RGB with opaque output");
    source.fill(Qt::transparent);
    prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
    require(presentation.lines[0].backgroundFillColor == QColor(Qt::white),
            "fully transparent input must use a deterministic opaque fallback");
    source.fill(QColor(40, 60, 80));
    presentation.lines = {box(20, 20, 0.2, 0.2)};
    prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
    require(presentation.lines[0].backgroundFillColor == QColor(40, 60, 80),
            "tiny positive-area regions must retain an opaque source color");
    source.fill(Qt::white);
    presentation.lines = {box(200, 200, 10, 10), box(20, 20, 0, 20), box(20, 20, 0, 0),
                          box(20, 20, 30, 30), box(20, 20, 30, 30)};
    presentation.lines[3].quad[0].setX(std::numeric_limits<qreal>::quiet_NaN());
    presentation.lines[4].quad[0].setY(std::numeric_limits<qreal>::infinity());
    prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
    for (const auto& line : presentation.lines) {
        require(
            !line.backgroundFillColor.isValid(),
            "disjoint, degenerate, tiny, and nonfinite geometry must not create false evidence");
    }
    require(screenshotOcrFilterRegion(presentation, source.rect(), source.size())
                    .intersected(source.rect()) ==
                screenshotOcrFilterRegion(presentation, source.rect(), source.size()),
            "invalid geometry must remain safe when constructing render regions");
    presentation.lines = {box(-10000000, -10000000, 20000000, 20000000)};
    presentation.lines[0].quad[0].setX(-9000000);
    prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
    require(screenshotOcrFilterRegion(presentation, source.rect(), source.size()).boundingRect() ==
                source.rect(),
            "huge slanted geometry must be clipped before allocating raster regions");
}

void clippingScalingAndModeChanges() {
    QImage source(200, 120, QImage::Format_RGB32);
    const QColor background(48, 80, 112);
    source.fill(background);
    ScreenshotOcrPresentation presentation;
    for (const auto& line : {box(-4, -4, 208, 128), box(-10, 20, 100, 60), box(20, -10, 120, 80),
                             box(120, 60, 100, 80)}) {
        presentation.lines = {line};
        prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
        require(presentation.lines[0].backgroundFillColor == background,
                "clipped quads must use available perimeter or strong interior evidence");
    }
    presentation.lines = {box(20, 20, 120, 60)};
    for (QPointF& point : presentation.lines[0].quad) {
        point = QPointF(point.x() / 1.5 - 80, point.y() / 2.0 + 25);
    }
    const QRectF canvas(-80, 25, 200 / 1.5, 60);
    prepareScreenshotOcrFillColors(presentation, source, canvas, true);
    require(presentation.lines[0].backgroundFillColor == background,
            "translated canvas and fractional nonuniform scaling must preserve source color");
    prepareScreenshotOcrFillColors(presentation, source, canvas, false);
    require(!presentation.lines[0].backgroundFillColor.isValid(),
            "switching to blur must clear all cached solid colors");
    presentation.lines[0].backgroundFillColor = Qt::red;
    prepareScreenshotOcrFillColors(presentation, {}, canvas, true);
    require(!presentation.lines[0].backgroundFillColor.isValid(),
            "missing source clears stale fill");
    prepareScreenshotOcrFillColors(presentation, source, {}, true);
    require(!presentation.lines[0].backgroundFillColor.isValid(), "invalid canvas stays uncertain");
}

void neighborsAndOverlaps() {
    QImage source(240, 120, QImage::Format_RGB32);
    const QColor background(220, 230, 240);
    source.fill(background);
    ScreenshotOcrPresentation presentation;
    presentation.lines = {box(40, 35, 160, 16), box(40, 54, 160, 16), box(40, 73, 160, 16)};
    {
        QPainter painter(&source);
        for (int y : {35, 54, 73}) {
            for (int x = 42; x < 198; x += 6) {
                painter.fillRect(x, y, 2, 16, Qt::black);
            }
        }
    }
    prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
    for (const auto& line : presentation.lines) {
        require(line.backgroundFillColor == background,
                "nearby OCR text must be excluded from perimeter evidence");
    }
    presentation.lines = {box(20, 20, 100, 60), box(70, 30, 100, 60)};
    prepareScreenshotOcrFillColors(presentation, source, source.rect(), false);
    presentation.lines[0].backgroundFillColor = Qt::red;
    const QImage original = source.copy();
    QRect crop;
    const QImage mixed = renderScreenshotOcrFilteredImage(source, source.rect(), presentation,
                                                          Qt::white, 1.5, &crop);
    require(mixed.pixelColor(QPoint(90, 50) - crop.topLeft()) == QColor(Qt::red),
            "solid fill must win an overlap with a later uncertain region");
    ScreenshotOcrPresentation fallback;
    fallback.lines = {presentation.lines[1]};
    QRect fallbackCrop;
    const QImage reference = renderScreenshotOcrFilteredImage(source, source.rect(), fallback,
                                                              Qt::white, 1.5, &fallbackCrop);
    const QRegion region = screenshotOcrFilterRegion(presentation, source.rect(), source.size());
    ScreenshotOcrPresentation solid;
    solid.lines = {presentation.lines[0]};
    const QRegion solidRegion = screenshotOcrFilterRegion(solid, source.rect(), source.size());
    bool pixelsMatch = true;
    for (int y = 0; y < mixed.height(); ++y) {
        for (int x = 0; x < mixed.width(); ++x) {
            const QPoint point = QPoint(x, y) + crop.topLeft();
            const QColor expected = solidRegion.contains(point) ? QColor(Qt::red)
                                    : region.contains(point)
                                        ? reference.pixelColor(point - fallbackCrop.topLeft())
                                        : source.pixelColor(point);
            pixelsMatch = pixelsMatch && mixed.pixelColor(x, y) == expected;
        }
    }
    require(pixelsMatch, "mixed crop must match standalone blur and preserve every exterior pixel");
    require(source == original, "all background passes must leave the source byte-identical");
    presentation.lines[1].backgroundFillColor = Qt::blue;
    const QImage solids = renderScreenshotOcrFilteredImage(source, source.rect(), presentation,
                                                           Qt::white, 1.0, &crop);
    require(solids.pixelColor(QPoint(90, 50) - crop.topLeft()) == QColor(Qt::blue),
            "two solid fills must preserve presentation line order");
}

void periodicTextCannotAliasTheInteriorSampler() {
    QImage source(180, 60, QImage::Format_RGB32);
    source.fill(Qt::white);
    QPainter painter(&source);
    for (int x = 27; x < 148; x += 16) {
        painter.fillRect(x, 20, 3, 16, Qt::black);
    }
    painter.end();
    ScreenshotOcrPresentation presentation;
    presentation.lines = {box(20, 20, 128, 16)};
    prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
    require(
        presentation.lines[0].backgroundFillColor == QColor(Qt::white),
        "periodic narrow glyphs must not alias every interior sample and become the background");
}

void disjointSolidRegionsCannotExpandIntoTheImage() {
    QImage source(160, 100, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::white);
    ScreenshotOcrPresentation presentation;
    presentation.lines = {box(160.1, 20, 20, 40)};
    ScreenshotOcrLine rotated;
    rotated.quad = {{-10, 0}, {0, -10}, {4.8, -5.2}, {-5.2, 4.8}};
    presentation.lines.push_back(rotated);
    prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
    require(screenshotOcrFilterRegion(presentation, source.rect(), source.size()).isEmpty(),
            "solid regions must intersect the image before expansion, including rotated bounds");
    require(renderScreenshotOcrFilteredImage(source, source.rect(), presentation, Qt::red) ==
                source,
            "a disjoint solid region must not paint an expanded fallback into the image");
}

void spatialExclusionIsOrderIndependent() {
    QImage source(400, 400, QImage::Format_RGB32);
    ScreenshotOcrPresentation presentation;
    QPainter painter(&source);
    for (int i = 0; i < 100; ++i) {
        const int column = i % 10;
        const int row = i / 10;
        const QColor background(30 + column * 20, 35 + row * 20, 80);
        painter.fillRect(column * 40, row * 40, 40, 40, background);
        auto line = box(column * 40 + 6, row * 40 + 14, 28, 12);
        line.quad[1].ry() += 5;
        line.quad[2].ry() += 5;
        line.text = QString::number(i);
        presentation.lines.push_back(line);
    }
    painter.end();
    for (int order = 0; order < 2; ++order) {
        prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
        for (const auto& line : presentation.lines) {
            const int id = line.text.toInt();
            require(line.backgroundFillColor ==
                        QColor(30 + (id % 10) * 20, 35 + (id / 10) * 20, 80),
                    "spatial pruning must preserve each panel color regardless of OCR line order");
        }
        std::reverse(presentation.lines.begin(), presentation.lines.end());
    }
    source.fill(QColor(40, 60, 80));
    presentation.lines.fill(presentation.lines.front(), 100);
    prepareScreenshotOcrFillColors(presentation, source, source.rect(), true);
    for (const auto& line : presentation.lines) {
        require(line.backgroundFillColor == QColor(40, 60, 80),
                "identical overlapping bounds must not break spatial-index partitioning");
    }
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    isolatedContaminationCannotDetermineTheBackground();
    splitBackgroundChoosesDominantPaletteColor();
    backgroundFillNeverBlurs();
    mixedRegionsRetainTheBlurFallback();
    flatPanelsSurviveTextNoiseAndGeometry();
    complexBackgroundsStillChooseSolidColors();
    clippingScalingAndModeChanges();
    neighborsAndOverlaps();
    spatialExclusionIsOrderIndependent();
    periodicTextCannotAliasTheInteriorSampler();
    disjointSolidRegionsCannotExpandIntoTheImage();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
