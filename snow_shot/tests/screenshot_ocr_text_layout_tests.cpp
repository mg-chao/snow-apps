#include "snow_shot/presentation/screenshotocrtextlayout.h"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
int failures = 0;
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        ++failures;
    }
}

QPolygonF quad(const QRectF& rect) {
    return {rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()};
}

QImage paint(ScreenshotOcrTextLayout& layout, const QTransform& transform) {
    QImage image(1000, 180, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setTransform(transform);
    layout.paint(&painter);
    return image;
}

void sourceTextUsesFullHeightAndNaturalSpacing() {
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Readable text: Agyp");
    line.quad = quad(QRectF(20, 30, 800, 30));
    QFont font = QGuiApplication::font();
    font.setLetterSpacing(QFont::AbsoluteSpacing, 12);
    font.setWordSpacing(20);
    ScreenshotOcrTextLayout layout;
    QTransform transform;
    qreal size = 0;
    require(
        configureScreenshotOcrTextLayout(layout, line, {}, font, Qt::black, {}, &transform, &size),
        "source text must configure");
    const QRectF fitted = transform.mapRect(layout.boundingRect());
    require(std::abs(fitted.height() - 30) < 0.001 && std::abs(fitted.left() - 20) < 0.001,
            "source text must use the full estimated ink height and align to the left edge");
    require(fitted.width() < 600 && layout.sourceTextMetrics().spacingEm == 0,
            "source text must retain natural spacing instead of filling unused width");
    const QImage original = paint(layout, transform);
    line.quad = quad(QRectF(20, 30, 900, 30));
    qreal widerSize = 0;
    require(configureScreenshotOcrTextLayout(layout, line, {}, font, Qt::black, {}, &transform,
                                             &widerSize),
            "wider source text must configure");
    require(std::abs(size - widerSize) < 0.001 && paint(layout, transform) == original,
            "extra horizontal space must not change glyphs, tracking, size or left alignment");
    line.quad = quad(QRectF(20, 30, 60, 30));
    qreal narrowerSize = 0;
    require(configureScreenshotOcrTextLayout(layout, line, {}, font, Qt::black, {}, &transform,
                                             &narrowerSize),
            "narrow source text must configure");
    require(narrowerSize < size && transform.mapRect(layout.boundingRect()).width() <= 60.001,
            "source text must shrink uniformly only when its natural width overflows");
    const QPointF start = transform.map(QPointF());
    const QPointF x = transform.map(QPointF(1, 0)) - start;
    const QPointF y = transform.map(QPointF(0, 1)) - start;
    require(std::abs(std::hypot(x.x(), x.y()) - std::hypot(y.x(), y.y())) < 0.001,
            "width-constrained glyphs must retain their aspect ratio");
}

void sourceRowsKeepNaturalSpacingAndSelection() {
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Agyp Agyp Agyp Agyp Agyp Agyp Agyp Agyp");
    line.paragraph = true;
    line.quad = quad(QRectF(20, 20, 800, 80));
    line.sourceLineQuads = {quad(QRectF(20, 20, 800, 30)), quad(QRectF(20, 70, 800, 30))};
    ScreenshotOcrTextLayout layout;
    QTransform transform;
    const QFont font = QGuiApplication::font();
    require(configureScreenshotOcrTextLayout(layout, line, {}, font, Qt::black, {}, &transform) &&
                layout.usesSourceRows(),
            "short source rows must retain their slots without synthetic justification");
    require(layout.sourceTextMetrics().spacingEm == 0,
            "paragraph source rows must retain natural letter spacing");
    const auto fragments = layout.rangeFragments({0, static_cast<int>(line.text.size())});
    require(fragments.size() == 2, "selected source text must retain both visual rows");
    const auto baseline = paint(layout, transform);
    layout.setSelection({0, static_cast<int>(line.text.size())});
    layout.setSelection({});
    require(paint(layout, transform) == baseline, "selection must not alter source glyph layout");
}

void verticalSourceTextDoesNotStretchItsSpacing() {
    ScreenshotOcrLine line;
    line.text = QStringLiteral("\u7530\u7530");
    line.direction = ScreenshotOcrTextDirection::Vertical;
    line.quad = quad(QRectF(20, 20, 24, 120));
    ScreenshotOcrTextLayout layout;
    QTransform transform;
    const QFont font = QGuiApplication::font();
    qreal size = 0;
    require(
        configureScreenshotOcrTextLayout(layout, line, {}, font, Qt::black, {}, &transform, &size),
        "vertical source text must configure");
    const auto original = paint(layout, transform);
    const auto originalSize = size;
    line.quad = quad(QRectF(20, 20, 24, 150));
    require(configureScreenshotOcrTextLayout(layout, line, {}, font, Qt::black, {}, &transform,
                                             &size) &&
                std::abs(size - originalSize) < 0.001 && paint(layout, transform) == original,
            "unused column height must not stretch vertical glyph spacing or change font size");
}

void translationRetainsSourceSizeAndBaseline() {
    ScreenshotOcrLine source;
    source.text = QStringLiteral("Readable text with descenders Agyp");
    source.quad = quad(QRectF(20, 30, 800, 30));
    QVector<ScreenshotOcrLine> translation{source};
    translation[0].text = QStringLiteral("Agyp");
    const QFont font = QGuiApplication::font();
    setScreenshotOcrTranslationFontCaps(translation, {source}, {{0}}, font);
    ScreenshotOcrTextLayout layout;
    QTransform transform;
    qreal size = 0;
    require(configureScreenshotOcrTextLayout(layout, translation[0], {}, font, Qt::black, {},
                                             &transform, &size),
            "short translation must configure");
    const auto shortSize = size;
    translation[0].text = source.text;
    require(configureScreenshotOcrTextLayout(layout, translation[0], {}, font, Qt::black, {},
                                             &transform, &size) &&
                std::abs(size - shortSize) < 0.001,
            "short and fitting translations must preserve the source font cap");
    require(layout.sourceTextMetrics().spacingEm == 0,
            "translation must retain natural spacing after source fitting changes");
}
int coloredPixels(const QImage& image, const QColor& color, const QRect& area) {
    int count = 0;
    const QRect bounds = area.intersected(image.rect());
    for (int y = bounds.top(); y <= bounds.bottom(); ++y)
        for (int x = bounds.left(); x <= bounds.right(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() >= 180 && std::abs(pixel.red() - color.red()) <= 3 &&
                std::abs(pixel.green() - color.green()) <= 3 &&
                std::abs(pixel.blue() - color.blue()) <= 3)
                ++count;
        }
    return count;
}

void sourceColorsPaintWithoutChangingSelectionOrShaping() {
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Colored readable text");
    line.quad = quad(QRectF(20, 20, 700, 30));
    line.backgroundFillColor = Qt::white;
    line.sourceTextColor = QColor(20, 40, 190);
    ScreenshotOcrTextLayout layout;
    QTransform transform;
    const QFont font = QGuiApplication::font();
    require(configureScreenshotOcrTextLayout(layout, line, {}, font, Qt::black, {}, &transform),
            "colored source text must configure");
    auto image = paint(layout, transform);
    require(coloredPixels(image, line.sourceTextColor, image.rect()) > 30,
            "estimated ink must override the black/white contrast fallback");
    const auto bounds = layout.boundingRect();
    line.sourceTextColor = QColor(190, 30, 25);
    require(configureScreenshotOcrTextLayout(layout, line, {}, font, Qt::black, {}, &transform),
            "a color-only update must configure");
    image = paint(layout, transform);
    require(coloredPixels(image, line.sourceTextColor, image.rect()) > 30 &&
                layout.boundingRect() == bounds,
            "cached shaping must accept a new color without moving glyphs");
    layout.setSelection({0, static_cast<int>(line.text.size())});
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setTransform(transform);
        layout.paint(&painter, Qt::yellow, Qt::green);
    }
    require(coloredPixels(image, Qt::green, image.rect()) > 30 &&
                coloredPixels(image, line.sourceTextColor, image.rect()) == 0,
            "selection foreground must still override source ink");
    layout.setSelection({});
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setTransform(transform);
        layout.paint(&painter, Qt::yellow, Qt::green,
                     ScreenshotOcrTextLayout::PaintMode::SelectionOnly);
    }
    require(coloredPixels(image, line.sourceTextColor, image.rect()) == 0,
            "selection-only overlays must never reveal estimated text");
    line.sourceTextColor = QColor();
    line.backgroundFillColor = Qt::black;
    require(configureScreenshotOcrTextLayout(layout, line, {}, font, Qt::black, {}, &transform),
            "uncertain source text must configure");
    image = paint(layout, transform);
    require(coloredPixels(image, Qt::white, image.rect()) > 30,
            "uncertain text must retain the existing dark-background fallback");
}

void sourceRowsKeepTheirColorsAfterSorting() {
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Colored text Colored text Colored text Colored text");
    line.paragraph = true;
    line.quad = quad(QRectF(20, 20, 800, 80));
    line.sourceLineQuads = {quad(QRectF(20, 70, 800, 30)), quad(QRectF(20, 20, 800, 30))};
    line.sourceTextColor = Qt::blue;
    line.sourceLineTextColors = {Qt::red, Qt::blue};
    ScreenshotOcrTextLayout layout;
    QTransform transform;
    const QFont font = QGuiApplication::font();
    require(configureScreenshotOcrTextLayout(layout, line, {}, font, Qt::black, {}, &transform) &&
                layout.usesSourceRows(),
            "colored source rows must configure");
    auto image = paint(layout, transform);
    require(coloredPixels(image, Qt::blue, QRect(0, 0, 1000, 60)) > 30 &&
                coloredPixels(image, Qt::red, QRect(0, 60, 1000, 120)) > 30,
            "normalizing row order must preserve color/geometry associations");
    line.sourceLineTextColors = {Qt::green, Qt::red};
    require(configureScreenshotOcrTextLayout(layout, line, {}, font, Qt::black, {}, &transform),
            "source-row colors must update through the shaping cache");
    image = paint(layout, transform);
    require(coloredPixels(image, Qt::red, QRect(0, 0, 1000, 60)) > 30 &&
                coloredPixels(image, Qt::green, QRect(0, 60, 1000, 120)) > 30,
            "cached source rows must paint updated colors");
    line.fontSizeCapRatio = 0.8;
    require(configureScreenshotOcrTextLayout(layout, line, {}, font, Qt::black, {}, &transform),
            "translated paragraphs must configure with source color");
    image = paint(layout, transform);
    require(coloredPixels(image, Qt::blue, image.rect()) > 30 &&
                coloredPixels(image, Qt::red, image.rect()) == 0 &&
                coloredPixels(image, Qt::green, image.rect()) == 0,
            "reflowed translations must use the representative color, not stale row mappings");
}
} // namespace

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    sourceTextUsesFullHeightAndNaturalSpacing();
    sourceRowsKeepNaturalSpacingAndSelection();
    verticalSourceTextDoesNotStretchItsSpacing();
    translationRetainsSourceSizeAndBaseline();
    sourceColorsPaintWithoutChangingSelectionOrShaping();
    sourceRowsKeepTheirColorsAfterSorting();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
