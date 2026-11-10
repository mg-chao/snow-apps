#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrvisuals.h"

#include <QCoreApplication>
#include <QPainter>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
int failures = 0;
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        ++failures;
    }
}

QPolygonF quad(const QRectF& bounds) {
    return {bounds.topLeft(), bounds.topRight(), bounds.bottomRight(), bounds.bottomLeft()};
}

ScreenshotOcrPresentation presentation(const QRectF& bounds = QRectF(20, 20, 360, 24)) {
    ScreenshotOcrPresentation result;
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Colored source text");
    line.quad = quad(bounds);
    result.lines.push_back(line);
    return result;
}

QColor blend(QColor foreground, QColor background, int coverage) {
    const auto channel = [&](int f, int b) { return (f * coverage + b * (255 - coverage)) / 255; };
    return {channel(foreground.red(), background.red()),
            channel(foreground.green(), background.green()),
            channel(foreground.blue(), background.blue())};
}

void strokes(QPainter& painter, QColor foreground, QColor background, int start = 24, int end = 376,
             int thickness = 3) {
    for (int x = start; x < end; x += 11) {
        painter.fillRect(QRect(x - 1, 23, thickness + 2, 18), blend(foreground, background, 85));
        painter.fillRect(QRect(x, 23, thickness, 18), foreground);
    }
}

QImage fixture(QColor foreground, QColor background, int thickness = 3) {
    QImage image(400, 70, QImage::Format_ARGB32_Premultiplied);
    image.fill(background);
    QPainter painter(&image);
    strokes(painter, foreground, background, 24, 376, thickness);
    return image;
}

void expectColor(const QColor& actual, QColor expected, const char* message, int tolerance = 8) {
    const bool close = actual.isValid() && actual.alpha() == 255 &&
                       std::abs(actual.red() - expected.red()) <= tolerance &&
                       std::abs(actual.green() - expected.green()) <= tolerance &&
                       std::abs(actual.blue() - expected.blue()) <= tolerance;
    if (!close)
        std::fprintf(stderr, "actual=%s expected=%s\n", qPrintable(actual.name()),
                     qPrintable(expected.name()));
    require(close, message);
}

void solidColorsAndThinStrokes() {
    const std::array<QColor, 6> colors{QColor(26, 13, 171),   QColor(220, 35, 30),
                                       QColor(105, 105, 105), QColor(235, 235, 235),
                                       QColor(20, 190, 80),   QColor(180, 170, 160)};
    for (QColor background : {QColor(Qt::white), QColor(24, 28, 34), QColor(40, 122, 40)}) {
        for (QColor color : colors) {
            for (int thickness : {1, 3}) {
                auto result = presentation();
                const auto image = fixture(color, background, thickness);
                const auto key = image.cacheKey();
                prepareScreenshotOcrTextColors(result, image, image.rect());
                expectColor(result.lines[0].sourceTextColor, color,
                            "solid and thin text must retain ink color");
                require(image.cacheKey() == key,
                        "estimation must not detach or modify the source image");
            }
        }
    }
}

void dominantInkPrecedesContrast() {
    const QColor gray(115, 115, 115), red(220, 20, 20);
    auto image = fixture(gray, Qt::white);
    {
        QPainter painter(&image);
        painter.fillRect(QRect(20, 20, 88, 24), Qt::white);
        strokes(painter, red, Qt::white, 24, 106);
    }
    auto result = presentation();
    prepareScreenshotOcrTextColors(result, image, image.rect());
    expectColor(result.lines[0].sourceTextColor, gray,
                "a high-contrast minority must not replace dominant gray ink");
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        strokes(painter, red, Qt::white, 24, 280);
        strokes(painter, gray, Qt::white, 288, 376);
    }
    prepareScreenshotOcrTextColors(result, image, image.rect());
    expectColor(result.lines[0].sourceTextColor, red,
                "the dominant colored phrase must win without averaging colors");
}

void samplingGeometryAndFormats() {
    const QColor color(20, 80, 200);
    const auto original = fixture(color, Qt::white);
    for (const auto format :
         {QImage::Format_RGB32, QImage::Format_RGB888, QImage::Format_BGR888,
          QImage::Format_RGBA8888, QImage::Format_RGBX8888, QImage::Format_RGBA8888_Premultiplied,
          QImage::Format_ARGB32, QImage::Format_ARGB32_Premultiplied, QImage::Format_RGBA64}) {
        auto image = original.convertToFormat(format);
        image.setDevicePixelRatio(2);
        auto result = presentation(QRectF(110, 210, 180, 12));
        prepareScreenshotOcrTextColors(result, image, QRectF(100, 200, 200, 35));
        expectColor(result.lines[0].sourceTextColor, color,
                    "pixel mapping must honor canvas origin/scale, not image DPR");
    }
    for (int angle : {15, 90, 180}) {
        QImage image(500, 500, QImage::Format_RGB32);
        image.fill(Qt::white);
        QTransform transform;
        transform.translate(250, 250);
        transform.rotate(angle);
        transform.translate(-200, -35);
        {
            QPainter painter(&image);
            painter.setTransform(transform);
            strokes(painter, color, Qt::white);
        }
        auto result = presentation();
        result.lines[0].quad = transform.map(result.lines[0].quad);
        prepareScreenshotOcrTextColors(result, image, image.rect());
        expectColor(result.lines[0].sourceTextColor, color,
                    "rotated and vertical text must sample the original quad");
    }
    auto result = presentation(QRectF(-40, 20, 420, 24));
    prepareScreenshotOcrTextColors(result, original, original.rect());
    expectColor(result.lines[0].sourceTextColor, color,
                "partially clipped text must remain bounded and measurable");
}

void uncertainInputsAndInvalidation() {
    auto result = presentation();
    auto image = fixture(Qt::blue, Qt::white);
    prepareScreenshotOcrTextColors(result, image, image.rect());
    expectColor(result.lines[0].sourceTextColor, Qt::blue, "initial estimate must succeed");
    result.lines[0].text = QStringLiteral("Edited text reuses source ink");
    prepareScreenshotOcrFillColors(result, image, image.rect(), false);
    prepareScreenshotOcrTextColors(result, image, image.rect());
    expectColor(result.lines[0].sourceTextColor, Qt::blue,
                "editing and disabling fill must retain source color");
    result.lines[0].quad.translate(0, 35);
    prepareScreenshotOcrTextColors(result, image, image.rect());
    require(!result.lines[0].sourceTextColor.isValid(),
            "geometry changes must invalidate the color cache");
    result = presentation();
    for (auto format : {QImage::Format_ARGB32, QImage::Format_ARGB32_Premultiplied}) {
        image = QImage(400, 70, format);
        image.fill(QColor(0, 0, 220, 128));
        prepareScreenshotOcrTextColors(result, image, image.rect());
        require(!result.lines[0].sourceTextColor.isValid(),
                "unknown alpha compositing must not invent opaque ink");
    }
    image.fill(Qt::white);
    prepareScreenshotOcrTextColors(result, image, image.rect());
    require(!result.lines[0].sourceTextColor.isValid(),
            "blank regions must retain the renderer fallback");
    quint32 random = 91;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            random = random * 1664525U + 1013904223U;
            image.setPixel(x, y, 0xff000000U | (random & 0xffffffU));
        }
    prepareScreenshotOcrTextColors(result, image, image.rect());
    require(!result.lines[0].sourceTextColor.isValid(),
            "texture must not be confidently labeled as text ink");
    image = fixture(Qt::blue, Qt::white);
    prepareScreenshotOcrTextColors(result, image, image.rect());
    prepareScreenshotOcrTextColors(result, {}, image.rect());
    require(!result.lines[0].sourceTextColor.isValid(),
            "a missing source must clear stale estimates");
    result.lines[0].quad[0].setX(std::numeric_limits<qreal>::quiet_NaN());
    prepareScreenshotOcrTextColors(result, image, image.rect());
    require(!result.lines[0].sourceTextColor.isValid(), "nonfinite quads must be rejected");
    result = presentation(QRectF(-1e12, -1e12, 2e12, 2e12));
    prepareScreenshotOcrTextColors(result, image, image.rect());
    require(!result.lines[0].sourceTextColor.isValid(),
            "huge malformed geometry must not allocate image-sized buffers");
}

void sourceRowsAndCancellation() {
    QImage image(400, 130, QImage::Format_RGB32);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        strokes(painter, Qt::blue, Qt::white);
        painter.translate(0, 50);
        strokes(painter, Qt::red, Qt::white);
    }
    auto result = presentation(QRectF(20, 20, 360, 74));
    result.lines[0].paragraph = true;
    result.lines[0].sourceLineQuads = {quad(QRectF(20, 20, 360, 24)),
                                       quad(QRectF(20, 70, 360, 24))};
    std::atomic_bool cancelled{true};
    prepareScreenshotOcrTextColors(result, image, image.rect(), &cancelled);
    require(!result.lines[0].sourceTextColor.isValid(),
            "cancelled work must not publish estimates");
    cancelled = false;
    prepareScreenshotOcrTextColors(result, image, image.rect(), &cancelled);
    require(result.lines[0].sourceLineTextColors == QVector<QColor>{Qt::blue, Qt::red},
            "source rows must retain separate colors");
    std::swap(result.lines[0].sourceLineQuads[0], result.lines[0].sourceLineQuads[1]);
    prepareScreenshotOcrTextColors(result, image, image.rect());
    require(result.lines[0].sourceLineTextColors == QVector<QColor>{Qt::red, Qt::blue},
            "source-row order changes must invalidate cached color associations");
}

void backgroundNoiseAndSubpixelFringes() {
    const QColor ink(26, 13, 171);
    auto image = fixture(ink, Qt::white);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y) == QColor(Qt::white)) {
                const int value = 249 + (x * 7 + y * 3) % 7;
                image.setPixel(x, y, qRgb(value, value, value));
            }
        }
    for (int x = 24; x < 376; x += 11)
        for (int y = 23; y < 41; ++y) {
            image.setPixelColor(x - 1, y, QColor(120, 165, 235));
            image.setPixelColor(x + 3, y, QColor(235, 160, 145));
        }
    auto result = presentation();
    prepareScreenshotOcrTextColors(result, image, image.rect());
    expectColor(result.lines[0].sourceTextColor, ink,
                "background noise and colored edge fringes must not replace solid stroke evidence");
    auto sharedResult = result;
    const auto* originalLines = result.lines.constData();
    prepareScreenshotOcrTextColors(sharedResult, image, image.rect());
    require(sharedResult.lines.constData() == originalLines,
            "cache hits on shared presentations must not detach line storage");
    image = fixture(QColor(190, 40, 30), Qt::white);
    prepareScreenshotOcrTextColors(sharedResult, image, image.rect());
    expectColor(sharedResult.lines[0].sourceTextColor, QColor(190, 40, 30),
                "a different image must invalidate shared estimates");
    expectColor(result.lines[0].sourceTextColor, ink,
                "recomputing one presentation must not mutate another presentation's colors");
    prepareScreenshotOcrTextColors(sharedResult, image, QRectF(0, 100, 400, 70));
    require(!sharedResult.lines[0].sourceTextColor.isValid(),
            "canvas mapping changes must invalidate cached estimates");
}

void smallGrayTextWithLcdFringes() {
    QImage image(400, 70, QImage::Format_RGB32);
    image.fill(Qt::white);
    const QColor ink(105, 105, 105);
    // More fringe pixels than solid ink, with different channel coverage on either side.
    const std::array<QColor, 5> coverage{QColor(255, 240, 215), QColor(150, 175, 220), ink,
                                         QColor(215, 170, 145), QColor(210, 240, 255)};
    for (int x = 24; x < 376; x += 11)
        for (int y = 23; y < 41; ++y)
            for (int offset = -2; offset <= 2; ++offset)
                image.setPixelColor(x + offset, y, coverage[static_cast<std::size_t>(offset + 2)]);
    auto result = presentation();
    prepareScreenshotOcrTextColors(result, image, image.rect());
    expectColor(result.lines[0].sourceTextColor, ink,
                "small gray glyphs must survive a majority of colored LCD fringe pixels");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    solidColorsAndThinStrokes();
    dominantInkPrecedesContrast();
    samplingGeometryAndFormats();
    uncertainInputsAndInvalidation();
    sourceRowsAndCancellation();
    backgroundNoiseAndSubpixelFringes();
    smallGrayTextWithLcdFringes();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
