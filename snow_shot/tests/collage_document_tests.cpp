// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/collagedocument.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "../src/presentation/collage/collageresourcebudget_p.h"

#include <QCoreApplication>
#include <QColorSpace>
#include <QPainter>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace snow_shot::presentation;

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

QImage fixture(QSize size, QColor color = Qt::red) {
    QImage image(size, QImage::Format_RGBA8888);
    image.fill(color);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    return image;
}

QImage asymmetricFixture() {
    QImage image = fixture({3, 2});
    const std::array<QColor, 6> colors = {Qt::red,    Qt::green,   Qt::blue,
                                          Qt::yellow, Qt::magenta, Qt::cyan};
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3; ++x)
            image.setPixelColor(x, y, colors[size_t(y) * 3 + size_t(x)]);
    return image;
}

ImageStitchingSettings customSettings(int rows, int columns) {
    ImageStitchingSettings settings;
    settings.direction = ImageStitchingDirection::Custom;
    settings.rows = rows;
    settings.columns = columns;
    settings.customGridInitialized = true;
    return settings;
}

void geometry() {
    ImageStitchingDocument document;
    require(!document.layout().isValid() && document.selectedId() == 0,
            "empty document has no layout or selection");
    require(document.settings() == ImageStitchingSettings{},
            "initial settings match specification");
    const auto a = document.append(fixture({4, 2}), QStringLiteral("a"));
    document.append(fixture({2, 4}), QStringLiteral("b"));
    document.append(fixture({3, 3}), QStringLiteral("c"));
    require(document.selectedId() == a, "first import selects first image");
    auto layout = document.layout();
    require(layout.size == QSize(4, 14) && layout.rects[1] == QRect(0, 2, 4, 8),
            "uniform vertical geometry references first width");

    ImageStitchingSettings settings;
    settings.sizing = ImageStitchingSizing::Original;
    settings.border = 2;
    settings.spacing = 3;
    document.setSettings(settings);
    layout = document.layout();
    require(layout.size == QSize(8, 19), "border bands and adjacent spacing appear once");
    require(layout.rects ==
                QVector<QRect>{QRect(2, 2, 4, 2), QRect(3, 7, 2, 4), QRect(2, 14, 3, 3)},
            "original vertical images center with deterministic odd-pixel placement");

    settings.direction = ImageStitchingDirection::Horizontal;
    document.setSettings(settings);
    layout = document.layout();
    require(layout.size == QSize(19, 8) && layout.rects[0] == QRect(2, 3, 4, 2) &&
                layout.rects[1] == QRect(9, 2, 2, 4),
            "original horizontal images center vertically");

    settings.sizing = ImageStitchingSizing::Uniform;
    document.setSettings(settings);
    require(document.layout().size == QSize(17, 6) &&
                document.layout().rects[1].size() == QSize(1, 2),
            "uniform horizontal images reference first height");

    settings.border = std::numeric_limits<int>::max();
    document.setSettings(settings);
    require(!document.layout().isValid() && !document.layout().error.isEmpty(),
            "integer overflow and output limit produce an actionable layout error");
    settings.border = -1;
    document.setSettings(settings);
    require(document.settings().border == std::numeric_limits<int>::max(),
            "negative settings do not replace valid settings");

    ImageStitchingDocument single;
    QImage dpiImage = fixture({4, 2});
    dpiImage.setDevicePixelRatio(2);
    single.append(dpiImage, QStringLiteral("dpi"));
    ImageStitchingSettings singleSettings;
    singleSettings.spacing = std::numeric_limits<int>::max();
    single.setSettings(singleSettings);
    require(
        single.layout().size == QSize(4, 2) &&
            materializeImageStitchingRows(prepareImageStitchingRows(single.items(), singleSettings))
                    .size() == QSize(4, 2),
        "output pixels are DPI independent and single-image spacing cannot overflow");
}

void customGridGeometry() {
    ImageStitchingDocument document;
    const auto first = document.append(fixture({4, 2}), QStringLiteral("first"));
    document.append(fixture({2, 4}, Qt::blue), QStringLiteral("second"));
    document.append(fixture({3, 3}, Qt::green), QStringLiteral("third"));
    auto settings = customSettings(2, 2);
    settings.border = 2;
    settings.spacing = 3;
    document.setSettings(settings);
    auto layout = document.layout();
    require(
        layout.size == QSize(15, 11) &&
            layout.rects ==
                QVector<QRect>{QRect(2, 2, 4, 2), QRect(10, 2, 1, 2), QRect(3, 7, 2, 2)},
        "uniform custom cells use the first image dimensions and center aspect-preserving fits");

    settings.sizing = ImageStitchingSizing::Original;
    document.setSettings(settings);
    layout = document.layout();
    require(layout.size == QSize(15, 15) &&
                layout.rects ==
                    QVector<QRect>{QRect(2, 3, 4, 2), QRect(10, 2, 2, 4), QRect(2, 9, 3, 3)},
            "original custom cells use global maximum dimensions without resizing their images");

    settings.sizing = ImageStitchingSizing::Uniform;
    settings.border = 0;
    settings.spacing = 0;
    document.setSettings(settings);
    require(document.rotate(first, true) && document.layout().size == QSize(4, 8) &&
                document.layout().rects[2] == QRect(0, 5, 2, 2),
            "rotating the uniform reference immediately updates custom cell dimensions");
    require(document.swapItems(first, document.items()[2].id) &&
                document.layout().size == QSize(6, 6),
            "custom uniform cells follow the first image after reordering");

    ImageStitchingDocument single;
    single.append(fixture({4, 2}), QStringLiteral("single"));
    single.setSettings(customSettings(3, 4));
    require(single.layout().size == QSize(16, 6) &&
                single.layout().rects == QVector<QRect>{QRect(0, 0, 4, 2)},
            "requested rows and columns retain background cells even with only one image");

    settings = customSettings(1, 1);
    settings.spacing = std::numeric_limits<int>::max();
    single.setSettings(settings);
    require(single.layout().size == QSize(4, 2),
            "single-cell custom layouts do not include unused spacing");

    const QVector<ImageStitchingItem> items = document.items();
    const auto requested = customSettings(2, 1);
    const auto expanded = calculateImageStitchingLayout(items, requested);
    require(expanded.size == QSize(6, 6) && requested.rows == 2 && requested.columns == 1 &&
                expanded.rects[2].top() == 3,
            "pure custom layout calculation expands effective columns without altering settings");
}

void customGridInitializationAndGrowth() {
    for (const auto count : {0, 1, 2, 3, 4, 5, 10, 17}) {
        ImageStitchingDocument document;
        for (int index = 0; index < count; ++index)
            document.append(fixture({1, 1}), QString::number(index));
        QVector<QStringList> adjusted;
        QObject::connect(&document, &ImageStitchingDocument::customGridAdjusted,
                         [&adjusted](const QStringList& fields) { adjusted.append(fields); });
        auto settings = document.settings();
        settings.direction = ImageStitchingDirection::Custom;
        document.setSettings(settings);
        const auto initialized = document.settings();
        const int columns = std::max(1, static_cast<int>(std::ceil(std::sqrt(double(count)))));
        const int rows = std::max(1, count / columns + (count % columns != 0 ? 1 : 0));
        require(initialized.customGridInitialized && initialized.rows == rows &&
                    initialized.columns == columns && adjusted.size() == 1 &&
                    adjusted[0] == QStringList{QStringLiteral("rows"), QStringLiteral("columns"),
                                               QStringLiteral("custom_grid_initialized")},
                "first custom selection initializes a near-square grid and announces its snapshot");
        document.setSettings(initialized);
        settings = initialized;
        settings.direction = ImageStitchingDirection::Horizontal;
        document.setSettings(settings);
        settings.direction = ImageStitchingDirection::Custom;
        document.setSettings(settings);
        require(document.settings() == initialized && adjusted.size() == 1,
                "an initialized custom grid survives switching layout direction without "
                "reinitializing");
    }

    ImageStitchingDocument document;
    QVector<QStringList> adjusted;
    QObject::connect(&document, &ImageStitchingDocument::customGridAdjusted,
                     [&adjusted](const QStringList& fields) { adjusted.append(fields); });
    document.setSettings(customSettings(2, 2));
    for (int index = 0; index < 4; ++index)
        document.append(fixture({2, 2}), QString::number(index));
    require(document.settings().rows == 2 && document.settings().columns == 2 && adjusted.isEmpty(),
            "images fitting an initialized custom grid leave dimensions unchanged");
    document.append(fixture({2, 2}), QStringLiteral("overflow"));
    require(document.settings().rows == 2 && document.settings().columns == 3 &&
                adjusted == QVector<QStringList>{QStringList{QStringLiteral("columns")}},
            "import overflow increases only columns and announces only that persisted field");
    document.append(fixture({2, 2}), QStringLiteral("capacity"));
    require(document.settings().columns == 3 && adjusted.size() == 1,
            "importing an image at exact expanded capacity does not grow the grid again");
    document.append(fixture({2, 2}), QStringLiteral("second overflow"));
    require(document.settings().rows == 2 && document.settings().columns == 4 &&
                adjusted.size() == 2,
            "repeated import overflow preserves rows and expands columns as needed");

    auto settings = document.settings();
    settings.rows = 1;
    document.setSettings(settings);
    require(document.settings().rows == 1 && document.settings().columns == 7 &&
                adjusted.size() == 3,
            "reducing rows expands columns enough to retain all images");
    settings = document.settings();
    settings.columns = 1;
    document.setSettings(settings);
    require(document.settings().columns == 7 && adjusted.size() == 3,
            "a normalized no-op column edit retains actual settings without redundant adjustment "
            "events");

    settings = document.settings();
    settings.rows = 0;
    document.setSettings(settings);
    require(document.settings().rows == 1, "zero rows do not replace valid custom settings");
    settings.rows = 1;
    settings.columns = -1;
    document.setSettings(settings);
    require(document.settings().columns == 7,
            "negative columns do not replace valid custom settings");
    require(document.remove(document.items().front().id) && document.settings().columns == 7 &&
                document.replace(document.items().front().id, fixture({3, 4}),
                                 QStringLiteral("replacement")) &&
                document.settings().columns == 7 && adjusted.size() == 3,
            "removing and replacing images do not shrink or grow the remembered grid");
    document.clear();
    require(document.settings().rows == 1 && document.settings().columns == 7 &&
                adjusted.size() == 3,
            "clearing images keeps the initialized grid and its expanded columns");

    require(!document.append({}, QStringLiteral("failed")) && adjusted.size() == 3,
            "failed imports do not modify custom dimensions or announce persistence changes");
}

void customGridPixelsAndLimits() {
    ImageStitchingDocument document;
    document.append(fixture({2, 2}), QStringLiteral("red"));
    document.append(fixture({2, 2}, Qt::blue), QStringLiteral("blue"));
    document.append(fixture({2, 2}, Qt::green), QStringLiteral("green"));
    auto settings = customSettings(2, 2);
    settings.border = 1;
    settings.spacing = 1;
    settings.background = QColor(20, 40, 60, 80);
    document.setSettings(settings);
    auto output = materializeImageStitchingRows(
        prepareImageStitchingRows(document.items(), document.settings()));
    require(output.size() == QSize(7, 7) && output.pixelColor(1, 1) == Qt::red &&
                output.pixelColor(4, 1) == Qt::blue && output.pixelColor(1, 4) == Qt::green &&
                output.pixelColor(4, 4) == settings.background &&
                output.pixelColor(0, 0) == settings.background &&
                output.pixelColor(3, 2) == settings.background,
            "custom export fills cells row-major and preserves background in empty cells, borders "
            "and gaps");
    document.append(fixture({2, 2}, Qt::yellow), QStringLiteral("yellow"));
    document.append(fixture({2, 2}, Qt::cyan), QStringLiteral("cyan"));
    const auto rows = prepareImageStitchingRows(document.items(), document.settings());
    output = materializeImageStitchingRows(rows);
    require(output.size() == QSize(10, 7) && output.pixelColor(7, 1) == Qt::green &&
                output.pixelColor(1, 4) == Qt::yellow && output.pixelColor(4, 4) == Qt::cyan &&
                output.pixelColor(7, 4) == settings.background,
            "column growth reflows row-major pixels while preserving the specified row count");
    const qsizetype stride = qsizetype(output.width()) * 4 + 5;
    QByteArray buffer(stride * 2, '\x5a');
    require(rows.readRows(3, 2, stride, reinterpret_cast<uchar*>(buffer.data()), buffer.size()) &&
                std::memcmp(buffer.constData(), output.constScanLine(3),
                            size_t(output.width()) * 4) == 0 &&
                std::memcmp(buffer.constData() + stride, output.constScanLine(4),
                            size_t(output.width()) * 4) == 0 &&
                buffer[output.width() * 4] == '\x5a',
            "partial grid row reads cross gaps and images correctly with padded strides");

    ImageStitchingDocument fitted;
    fitted.append(fixture({4, 2}, Qt::green), QStringLiteral("reference"));
    QImage portrait = fixture({2, 4}, QColor(255, 0, 0, 128));
    portrait.setPixelColor(0, 0, QColor(0, 0, 255, 128));
    fitted.append(portrait, QStringLiteral("portrait"));
    settings = customSettings(1, 2);
    settings.background = Qt::transparent;
    fitted.setSettings(settings);
    output =
        materializeImageStitchingRows(prepareImageStitchingRows(fitted.items(), fitted.settings()));
    require(output.size() == QSize(8, 2) && output.pixelColor(4, 0).alpha() == 0 &&
                output.pixelColor(6, 0).alpha() == 0 && output.pixelColor(5, 1).red() >= 254 &&
                output.pixelColor(5, 1).alpha() == 128,
            "fitting portrait images centers letterboxing and preserves their scaled alpha");

    ImageStitchingDocument small;
    small.append(fixture({1, 1}), QStringLiteral("small"));
    const auto rejects = [&small](ImageStitchingSettings candidate) {
        const auto layout = calculateImageStitchingLayout(small.items(), candidate);
        return !layout.isValid() && !layout.error.isEmpty();
    };
    settings = customSettings(std::numeric_limits<int>::max(), std::numeric_limits<int>::max());
    settings.border = std::numeric_limits<int>::max();
    settings.spacing = std::numeric_limits<int>::max();
    require(rejects(settings),
            "maximum grid dimensions reject arithmetic and output overflow safely");
    settings = customSettings(1, std::numeric_limits<int>::max());
    require(rejects(settings), "custom output scanline overflow fails before raster allocation");
    settings = customSettings(5, std::numeric_limits<int>::max() / 4);
    require(rejects(settings), "empty grid capacity still obeys the 8 GiB output raster limit");
    settings = customSettings(1, 1);
    settings.rows = 0;
    require(rejects(settings),
            "pure custom layout calculation rejects nonpositive grid dimensions");
}

void referenceAndManagement() {
    ImageStitchingDocument document;
    const auto a = document.append(fixture({4, 2}), QStringLiteral("a"));
    const auto b = document.append(fixture({2, 4}), QStringLiteral("b"));
    require(document.layout().size == QSize(4, 10), "initial reference is first item");
    require(document.rotate(a, true) && document.layout().size == QSize(2, 8),
            "rotating first item changes uniform reference immediately");
    require(document.swapItems(a, b) && document.items()[0].id == b &&
                document.items()[1].id == a && document.selectedId() == a,
            "swap preserves identity and selection");
    require(!document.swapItems(a, 9999) && !document.swapItems(a, a),
            "invalid and self swaps do not change ordering");
    document.resetTransform(a);
    require(document.layout().size == QSize(2, 5), "reset applies only to selected item");
    document.rotate(b, true);
    require(document.replace(b, fixture({6, 3}, Qt::blue), QStringLiteral("replacement")),
            "replacement succeeds");
    require(document.items()[0].id == b && document.items()[0].transform.isIdentity() &&
                document.layout().size == QSize(6, 6),
            "replacement preserves position, resets transform, and changes first reference");
    const auto oldKey = document.items()[0].image.cacheKey();
    require(!document.replace(b, {}, QStringLiteral("failed")) &&
                document.items()[0].image.cacheKey() == oldKey,
            "failed replacement keeps original item intact");
    require(document.remove(b) && document.layout().size == QSize(4, 2),
            "deleting first item changes reference");
    require(document.remove(a) && !document.layout().isValid() && document.selectedId() == 0,
            "deleting final image produces empty state");
    require(document.append({}, QStringLiteral("failed")) == 0 && !document.lastError().isEmpty(),
            "failed import is reported without adding an item");
    const auto duplicate = fixture({2, 2});
    const auto first = document.append(duplicate, QStringLiteral("duplicate"));
    const auto second = document.append(duplicate, QStringLiteral("duplicate"));
    require(first != second && document.items().size() == 2,
            "repeated source images remain independent collection items");
    document.clear();
    require(document.items().isEmpty() && document.selectedId() == 0,
            "clear resets collection and selection");
}

void transformations() {
    ImageStitchingDocument document;
    const QImage source = asymmetricFixture();
    const auto id = document.append(source, QStringLiteral("source"));
    const auto sourceKey = source.cacheKey();
    auto output = [&] {
        ImageStitchingSettings settings;
        settings.sizing = ImageStitchingSizing::Original;
        return materializeImageStitchingRows(prepareImageStitchingRows(document.items(), settings));
    };
    document.rotate(id, true);
    QImage rotated = output();
    require(rotated.size() == QSize(2, 3) && rotated.pixelColor(1, 0) == Qt::red &&
                rotated.pixelColor(0, 0) == Qt::yellow && rotated.pixelColor(1, 2) == Qt::blue,
            "clockwise quarter-turn is exact");
    document.flip(id, true);
    QImage flipped = output();
    require(flipped.pixelColor(0, 0) == Qt::red && flipped.pixelColor(1, 0) == Qt::yellow,
            "horizontal flip uses current visual orientation");
    document.flip(id, false);
    flipped = output();
    require(flipped.pixelColor(0, 2) == Qt::red && flipped.pixelColor(1, 2) == Qt::yellow,
            "vertical flip uses current visual orientation");
    document.resetTransform(id);
    require(output() == source, "reset restores source pixel order");
    document.rotate(id, false);
    rotated = output();
    require(rotated.pixelColor(0, 0) == Qt::blue && rotated.pixelColor(1, 0) == Qt::cyan,
            "counterclockwise quarter-turn is exact");
    document.rotate(id, true);
    require(output() == source && document.items()[0].image.cacheKey() == sourceKey,
            "inverse rotations restore transform without altering original pixels");
    require(transformedPreview(document.items()[0]).size() == source.size(),
            "small previews reflect current transform");
    ImageStitchingItem large;
    large.image = fixture({400, 300});
    large.transform = QTransform(0, 1, -1, 0, 0, 0);
    require(transformedPreview(large).size() == QSize(192, 256),
            "missing previews derive bounded images before transforming");
}

void transformedRowsPreserveNativePixelsAndBoundMemory() {
    const std::array<QTransform, 8> transforms{QTransform{},
                                               QTransform(0, 1, -1, 0, 0, 0),
                                               QTransform(-1, 0, 0, -1, 0, 0),
                                               QTransform(0, -1, 1, 0, 0, 0),
                                               QTransform(-1, 0, 0, 1, 0, 0),
                                               QTransform(1, 0, 0, -1, 0, 0),
                                               QTransform(0, 1, 1, 0, 0, 0),
                                               QTransform(0, -1, -1, 0, 0, 0)};
    QImage colors(257, 129, QImage::Format_RGBA8888);
    for (int y = 0; y < colors.height(); ++y)
        for (int x = 0; x < colors.width(); ++x)
            colors.setPixelColor(x, y,
                                 QColor(x % 256, y % 256, (x * 7 + y * 11) % 256,
                                        (x + y) % 4 == 0   ? 0
                                        : (x + y) % 4 == 1 ? 127
                                                           : 255));
    colors.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    ImageStitchingSettings settings;
    settings.sizing = ImageStitchingSizing::Original;
    settings.background = Qt::transparent;
    for (const auto format :
         {QImage::Format_RGBA8888, QImage::Format_ARGB32_Premultiplied, QImage::Format_RGB32,
          QImage::Format_RGB888, QImage::Format_RGBA64_Premultiplied, QImage::Format_Grayscale16,
          QImage::Format_Indexed8, QImage::Format_Mono}) {
        const auto source = colors.convertToFormat(format);
        for (const auto& transform : transforms) {
            const auto native = snowCanvasTransformImage(source, transform);
            QVector<ImageStitchingItem> expectedItems{{1, 1, native, {}, {}, {}, {}, {}}};
            QVector<ImageStitchingItem> actualItems{{1, 1, source, {}, transform, {}, {}, {}}};
            const auto expected =
                materializeImageStitchingRows(prepareImageStitchingRows(expectedItems, settings));
            auto rows = prepareImageStitchingRows(actualItems, settings);
            require(rows.isValid() && materializeImageStitchingRows(rows) == expected,
                    "bounded transforms preserve native orientation, alpha and ICC pixels");
            const qsizetype stride = qsizetype(rows.size.width()) * 4 + 12;
            QByteArray storage(stride * 3, char(0x5a));
            for (const int first : {0, 63, 64, rows.size.height() - 3, 17}) {
                require(rows.readRows(first, 3, stride, reinterpret_cast<uchar*>(storage.data()),
                                      storage.size()),
                        "transformed rows support padded and nonsequential reads");
                for (int row = 0; row < 3; ++row) {
                    require(std::memcmp(storage.constData() + row * stride,
                                        expected.constScanLine(first + row),
                                        size_t(expected.width()) * 4) == 0,
                            "random transformed row pixels match native reference");
                    for (qsizetype x = expected.width() * 4; x < stride; ++x)
                        require(storage[row * stride + x] == char(0x5a),
                                "transformed row reads leave destination padding untouched");
                }
            }
        }
    }
    auto highDpiSource = colors;
    highDpiSource.setDevicePixelRatio(2);
    const auto highDpiKey = highDpiSource.cacheKey();
    for (const auto& transform : transforms) {
        const auto native = snowCanvasTransformImage(highDpiSource, transform);
        const QVector<ImageStitchingItem> expectedItems{{1, 1, native, {}, {}, {}, {}, {}}};
        const QVector<ImageStitchingItem> actualItems{
            {1, 1, highDpiSource, {}, transform, {}, {}, {}}};
        const auto expected =
            materializeImageStitchingRows(prepareImageStitchingRows(expectedItems, settings));
        const auto actual =
            materializeImageStitchingRows(prepareImageStitchingRows(actualItems, settings));
        require(!actual.isNull() && actual == expected && native.devicePixelRatio() == 2 &&
                    highDpiSource.cacheKey() == highDpiKey,
                "high-DPI transforms preserve native fallback pixels and source metadata");
    }
    const auto baseline = imageStitchingResourceUsage().reservedBytes;
    {
        ImageStitchingDocument document;
        for (int i = 0; i < 12; ++i) {
            const auto id = document.append(fixture({1024, 1024}), QString::number(i));
            require(id != 0 && document.rotate(id, true), "large transform fixture admission");
        }
        const auto owned = imageStitchingResourceUsage().reservedBytes;
        auto rows = prepareImageStitchingRows(document.items(), settings);
        require(rows.isValid() &&
                    imageStitchingResourceUsage().reservedBytes - owned < 32 * 1024 * 1024,
                "export retains bounded transform caches rather than all rotated rasters");
        document.clear();
        QByteArray row(rows.size.width() * 4, Qt::Uninitialized);
        require(rows.readRows(rows.size.height() - 1, 1, row.size(),
                              reinterpret_cast<uchar*>(row.data()), row.size()),
                "bounded transform readers retain originals after document clear");
    }
    require(imageStitchingResourceUsage().reservedBytes == baseline,
            "bounded transform resources retire with the final reader");
}

void regionReadsAndLazyResize(bool manualPlacement = false) {
    ImageStitchingDocument document;
    const auto first = document.append(asymmetricFixture(), QStringLiteral("first"));
    document.append(fixture({7, 5}, QColor(20, 100, 200, 128)), QStringLiteral("resized"));
    document.append(fixture({4, 9}, QColor(100, 20, 60, 0)), QStringLiteral("transparent"));
    require(document.rotate(first, true), "region rotation fixture");
    if (manualPlacement) {
        document.setStaticPlacement(first, {{1, -1}, 3});
        document.setStaticPlacement(document.items()[1].id, {{-2, 1}, .75});
    }
    for (const auto direction :
         {ImageStitchingDirection::Vertical, ImageStitchingDirection::Horizontal,
          ImageStitchingDirection::Custom}) {
        auto settings = customSettings(2, 2);
        settings.direction = direction;
        settings.border = 2;
        settings.spacing = 3;
        for (const auto background :
             {QColor(Qt::transparent), QColor(70, 30, 90, 128), QColor(Qt::white)}) {
            settings.background = background;
            auto rows = prepareImageStitchingRows(document.items(), settings);
            const auto full = materializeImageStitchingRows(rows);
            require(!full.isNull() && rows.readRegion &&
                        rows.isKnownOpaque == (background.alpha() == 255),
                    "collage regions expose a structurally valid opacity guarantee");
            const std::array<QRect, 4> regions{QRect(0, 0, 1, 1),
                                               QRect(1, 1, full.width() - 2, full.height() - 2),
                                               QRect(full.width() - 2, full.height() - 2, 2, 2),
                                               QRect(full.width() / 2, 0, 1, full.height())};
            for (const auto& region : regions) {
                const qsizetype stride = region.width() * 4 + 7;
                QByteArray storage(stride * region.height(), char(0x5a));
                require(rows.readRegion(region, stride, reinterpret_cast<uchar*>(storage.data()),
                                        storage.size()),
                        "native region read succeeds across borders, gaps and scaled items");
                for (int y = 0; y < region.height(); ++y) {
                    require(std::memcmp(storage.constData() + y * stride,
                                        full.constScanLine(region.top() + y) + region.left() * 4,
                                        size_t(region.width()) * 4) == 0,
                            "native region matches full composition without alpha changes");
                    for (qsizetype x = region.width() * 4; x < stride; ++x)
                        require(storage[y * stride + x] == char(0x5a),
                                "native regions preserve destination padding");
                }
            }
            uchar storage[4]{};
            require(!rows.readRegion({}, 0, storage, 0) &&
                        !rows.readRegion(QRect(-1, 0, 1, 1), 4, storage, 4) &&
                        !rows.readRegion(QRect(full.width(), 0, 1, 1), 4, storage, 4) &&
                        !rows.readRegion(QRect(0, 0, 1, 1), 3, storage, 4),
                    "invalid native regions fail before arithmetic or pixel access");
            const auto& image = document.items().front().image;
            const auto unchanged = image.copy();
            require(!rows.readRegion(QRect(0, 0, 1, 1), 4, const_cast<uchar*>(image.constBits()),
                                     image.sizeInBytes()) &&
                        image == unchanged,
                    "aliased native regions fail without modifying immutable originals");
            auto* wrapped =
                reinterpret_cast<uchar*>(std::numeric_limits<std::uintptr_t>::max() - 1);
            require(!rows.readRegion(QRect(0, 0, 1, 1), 4, wrapped, 4),
                    "wrapped destination addresses fail before background fills");
        }
    }
    if (manualPlacement)
        return;
    {
        ImageStitchingSettings settings;
        settings.sizing = ImageStitchingSizing::Original;
        settings.background = QColor(0, 0, 255, 128);
        auto oldRows = prepareImageStitchingRows(document.items(), settings);
        const auto oldPixels = materializeImageStitchingRows(oldRows);
        require(document.replace(first, fixture({3, 2}, QColor(255, 0, 0, 128)),
                                 QStringLiteral("replacement")),
                "replace an original whose opacity was cached");
        const auto replaced =
            materializeImageStitchingRows(prepareImageStitchingRows(document.items(), settings));
        const auto topLeft =
            calculateImageStitchingLayout(document.items(), settings).rects.front().topLeft();
        require(replaced.pixelColor(topLeft) == QColor(170, 0, 85, 192) &&
                    materializeImageStitchingRows(oldRows) == oldPixels,
                "replacement invalidates opacity while older export snapshots remain unchanged");
    }
    const auto baseline = imageStitchingResourceUsage().reservedBytes;
    {
        ImageStitchingDocument large;
        for (int index = 0; index < 12; ++index)
            require(large.append(fixture(index == 0 ? QSize(1024, 1024) : QSize(2048, 1024)),
                                 QString::number(index)) != 0,
                    "large uniform resize fixture admission");
        const auto owned = imageStitchingResourceUsage().reservedBytes;
        auto rows = prepareImageStitchingRows(large.items(), ImageStitchingSettings{});
        require(rows.isValid() &&
                    imageStitchingResourceUsage().reservedBytes - owned < 32 * 1024 * 1024,
                "uniform export retains plans and tiles rather than every resized raster");
        large.clear();
        QByteArray storage(4, Qt::Uninitialized);
        require(rows.readRegion(QRect(0, rows.size.height() - 1, 1, 1), 4,
                                reinterpret_cast<uchar*>(storage.data()), storage.size()),
                "lazy resized rows survive document clear");
    }
    require(imageStitchingResourceUsage().reservedBytes == baseline,
            "lazy resize plans, tiles and reservations retire together");
}

void alphaAndRowReads() {
    ImageStitchingDocument document;
    QImage image = fixture({2, 2}, QColor(255, 0, 0, 128));
    image.setPixelColor(1, 1, Qt::transparent);
    document.append(image, QStringLiteral("alpha"));
    ImageStitchingSettings settings;
    settings.border = 1;
    settings.background = QColor(0, 0, 255, 128);
    document.setSettings(settings);
    QString error;
    const auto rows = prepareImageStitchingRows(document.items(), settings, {}, &error);
    require(rows.isValid() && rows.backingImage.isNull() && error.isEmpty(),
            "export returns an immutable row source without materializing full output");
    const QImage output = materializeImageStitchingRows(rows);
    require(output.size() == QSize(4, 4) && output.pixelColor(0, 0) == settings.background,
            "outer band retains semitransparent background");
    const QColor color = output.pixelColor(1, 1);
    require(color.red() == 170 && color.blue() == 85 && color.alpha() == 192,
            "straight alpha composition retains semitransparency");
    require(output.pixelColor(2, 2) == settings.background,
            "fully transparent source reveals background");

    const qsizetype stride = output.width() * 4 + 7;
    QByteArray buffer(stride * output.height(), '\x5a');
    require(rows.readRows(1, 2, stride, reinterpret_cast<uchar*>(buffer.data()), buffer.size()),
            "arbitrary multi-row reads accept padded strides");
    require(std::memcmp(buffer.constData(), output.constScanLine(1), size_t(output.width()) * 4) ==
                    0 &&
                std::memcmp(buffer.constData() + stride, output.constScanLine(2),
                            size_t(output.width()) * 4) == 0 &&
                buffer[output.width() * 4] == '\x5a',
            "partial reads match whole output and leave padding untouched");
    require(
        !rows.readRows(-1, 1, stride, reinterpret_cast<uchar*>(buffer.data()), buffer.size()) &&
            !rows.readRows(3, 2, stride, reinterpret_cast<uchar*>(buffer.data()), buffer.size()) &&
            !rows.readRows(0, 2, stride, reinterpret_cast<uchar*>(buffer.data()), 1),
        "invalid destination ranges are rejected safely");
    require(materializeImageStitchingRows(rows, 1).isNull(),
            "test materializer obeys explicit allocation limit");

    settings.background = Qt::transparent;
    const QImage transparent =
        materializeImageStitchingRows(prepareImageStitchingRows(document.items(), settings));
    require(transparent.pixelColor(1, 1) == image.pixelColor(0, 0) &&
                transparent.pixelColor(0, 0).alpha() == 0,
            "transparent backgrounds preserve source alpha");
}

void snapshotsAndResources() {
    const qint64 baseline = imageStitchingResourceUsage().reservedBytes;
    QVector<ImageStitchingItem> snapshot;
    {
        ImageStitchingDocument first;
        ImageStitchingDocument second;
        first.append(fixture({20, 10}), QStringLiteral("first"));
        second.append(fixture({10, 20}), QStringLiteral("second"));
        auto settings = first.settings();
        settings.direction = ImageStitchingDirection::Horizontal;
        first.setSettings(settings);
        first.rotate(first.items()[0].id, true);
        require(second.settings().direction == ImageStitchingDirection::Vertical &&
                    second.items()[0].transform.isIdentity(),
                "document editing and settings remain independent");
        snapshot = first.items();
    }
    require(imageStitchingResourceUsage().reservedBytes > baseline,
            "snapshots retain source memory admission after window destruction");
    auto rows = prepareImageStitchingRows(snapshot, ImageStitchingSettings{});
    snapshot.clear();
    require(rows.isValid() && imageStitchingResourceUsage().reservedBytes > baseline &&
                !materializeImageStitchingRows(rows).isNull(),
            "prepared rows keep snapshot source storage and reservations alive");
    rows = {};
    require(imageStitchingResourceUsage().reservedBytes == baseline,
            "last reader retirement releases all resource reservations");

    QString error;
    auto reservation = reserveImageStitchingImport(1, &error);
    require(reservation && reservation->resize(2, &error), "import reservation can grow");
    require(reservation->resize(qint64{513} * 1024 * 1024, &error),
            "import rasters are not charged to the smaller painting scratch pool");
    require(!reservation->resize(imageStitchingResourceUsage().limitBytes + 1, &error) &&
                !error.isEmpty(),
            "imports still enforce the shared memory budget");
    reservation.reset();
    require(imageStitchingResourceUsage().reservedBytes == baseline,
            "rejected reservation growth retains and later releases prior admission");

    ImageStitchingDocument document;
    document.append(fixture({2, 2}), QStringLiteral("cancel"));
    std::atomic_bool cancel{true};
    require(!prepareImageStitchingRows(
                 document.items(), document.settings(), [&] { return cancel.load(); }, &error)
                    .isValid() &&
                !error.isEmpty(),
            "already cancelled preparation returns clear failure");
    cancel.store(false);
    auto cancellable = prepareImageStitchingRows(
        document.items(), document.settings(), [&] { return cancel.load(); }, &error);
    cancel.store(true);
    require(materializeImageStitchingRows(cancellable).isNull(),
            "row production honors cancellation");
    require(!reserveImageStitchingClipboard({2000000, 1000000}, &error) && !error.isEmpty(),
            "clipboard resource limits fail before native allocations");

    const auto small = estimateImageStitchingClipboard({1, 2});
    require(small.isValid(),
            "small clipboard estimates are independent of current available memory");
#ifdef Q_OS_WIN
    require(small.dibBytes == 8 && small.rowScratchBytes == 8 &&
                small.bytes == qint64{64} * 1024 * 1024 + 32,
            "clipboard estimate includes padded DIB stride, both PNG representations, and scratch");
    const auto wide = estimateImageStitchingClipboard({384000, 2160});
    require(wide.isValid() && wide.rowScratchBytes == qint64{384000} * 4 * 64 &&
                wide.dibBytes == qint64{384000} * 3 * 2160 &&
                wide.bytes ==
                    qint64{384000} * 2160 * 11 + wide.rowScratchBytes + qint64{64} * 1024 * 1024,
            "horizontal stress admission includes the complete 64-row native scratch buffer");
    require(!estimateImageStitchingClipboard({449350, 3840}).isValid(),
            "mixed horizontal stress output is rejected before overflowing native DIB capacity");
#else
    require(small.dibBytes == 0 && small.rowScratchBytes == 0 &&
                small.bytes == qint64{64} * 1024 * 1024 + 8,
            "non-Windows clipboard estimates cover PNG preparation without a native DIB");
#endif
    require(!estimateImageStitchingClipboard({}).isValid() &&
                !estimateImageStitchingClipboard({-1, 2}).isValid() &&
                !estimateImageStitchingClipboard({std::numeric_limits<int>::max(), 1}).isValid(),
            "clipboard dimension and stride bounds reject invalid sizes without allocating");
}

void capturedImagesAdmitMemoryBeforePaintingAndOwnTheirReservation() {
    const qint64 baseline = imageStitchingResourceUsage().reservedBytes;
    int captures = 0;
    const auto capture = [&] {
        ++captures;
        return fixture({3, 2});
    };
    QString error;
    require(captureImageStitchingImage({65536, 32769}, capture, &error).isNull() && captures == 0 &&
                !error.isEmpty(),
            "oversized captures fail before calling the rasterizer");
    auto occupied = image_stitching_detail::reserveWorking(qint64{512} * 1024 * 1024);
    require(occupied != nullptr, "occupy the shared painting budget without allocating pixels");
    require(captureImageStitchingImage({3, 2}, capture, &error).isNull() && captures == 0 &&
                !error.isEmpty(),
            "memory-limited captures fail before calling the rasterizer");
    occupied.reset();
    require(imageStitchingResourceUsage().reservedBytes == baseline,
            "rejected captures leave no reservations behind");

    const uchar* capturedPixels = nullptr;
    QImage captured = captureImageStitchingImage(
        {3, 2},
        [&] {
            QImage pixels = fixture({3, 2}, QColor(50, 100, 150, 96));
            pixels.setDevicePixelRatio(2);
            pixels.setDotsPerMeterX(1234);
            pixels.setDotsPerMeterY(5678);
            pixels.setOffset({7, 9});
            pixels.setText(QStringLiteral("description"), QStringLiteral("captured image"));
            capturedPixels = pixels.constBits();
            return pixels;
        },
        &error);
    require(!captured.isNull() && error.isEmpty() && captured.constBits() == capturedPixels &&
                captured.colorSpace() == QColorSpace(QColorSpace::SRgb) &&
                captured.devicePixelRatio() == 2 && captured.dotsPerMeterX() == 1234 &&
                captured.dotsPerMeterY() == 5678 && captured.offset() == QPoint(7, 9) &&
                captured.text(QStringLiteral("description")) == QStringLiteral("captured image") &&
                captured.pixelColor(1, 1) == QColor(50, 100, 150, 96) &&
                imageStitchingResourceUsage().reservedBytes == baseline + captured.sizeInBytes(),
            "captured pixels own their admission without copying or losing image metadata");
    QImage queuedCopy = captured;
    captured = {};
    require(imageStitchingResourceUsage().reservedBytes == baseline + queuedCopy.sizeInBytes(),
            "a queued image copy keeps its raster accounted for");
    occupied = image_stitching_detail::reserveWorking(qint64{512} * 1024 * 1024);
    require(occupied != nullptr, "retained captures do not occupy the temporary painting budget");
    occupied.reset();
    queuedCopy = {};
    require(imageStitchingResourceUsage().reservedBytes == baseline,
            "the last image reader releases the captured raster reservation");
    require(captureImageStitchingImage(
                {3, 2}, [] { return QImage{}; }, &error)
                    .isNull() &&
                !error.isEmpty() && imageStitchingResourceUsage().reservedBytes == baseline,
            "failed rasterization releases both retained and working reservations");
    require(imageStitchingImageImportBytes({8000, 8000}, &error) == 512'000'000 &&
                error.isEmpty() && imageStitchingImageImportBytes({}, &error) == 0 &&
                !error.isEmpty(),
            "capture and import share inclusive pixel limits and reject invalid dimensions");
    require(imageStitchingImageImportBytes({2048, 32769}, &error) == qint64{2048} * 32769 * 8 &&
                error.isEmpty(),
            "scrolling imports above 64 million pixels reserve both raster and preparation bytes");
    require(imageStitchingImageImportBytes({65536, 32768}, &error) ==
                    qint64{16} * 1024 * 1024 * 1024 &&
                error.isEmpty() && imageStitchingImageImportBytes({65536, 32769}, &error) == 0 &&
                !error.isEmpty(),
            "image imports use the inclusive 8 GiB raster ceiling");
    require(imageStitchingImageImportBytes({std::numeric_limits<int>::max(), 1}, &error) == 0 &&
                !error.isEmpty(),
            "imports reject dimensions that cannot be represented by the image bridge");
}

void staticPlacementGeometryAndLifecycle() {
    for (const auto direction :
         {ImageStitchingDirection::Vertical, ImageStitchingDirection::Horizontal,
          ImageStitchingDirection::Custom}) {
        for (const auto sizing : {ImageStitchingSizing::Original, ImageStitchingSizing::Uniform}) {
            ImageStitchingDocument document;
            const auto first = document.append(fixture({80, 40}), QStringLiteral("first"));
            const auto second =
                document.append(fixture({40, 80}, Qt::blue), QStringLiteral("second"));
            auto settings = customSettings(1, 2);
            settings.direction = direction;
            settings.sizing = sizing;
            settings.border = 4;
            settings.spacing = 6;
            document.setSettings(settings);
            const auto automatic = document.layout();
            const auto revision = document.contentRevision();
            const auto original = document.items()[0].image.cacheKey();
            int changes = 0;
            QObject::connect(&document, &ImageStitchingDocument::staticPlacementChanged, &document,
                             [&](quint64) { ++changes; });
            const ImageStitchingStaticPlacement placement{{3, -2}, .5};
            require(document.setStaticPlacement(first, placement), "static placement is admitted");
            const auto placed = document.layout();
            require(placed.size == automatic.size && placed.rects == automatic.rects &&
                        placed.clipRects == automatic.clipRects &&
                        placed.imageRects[0].size() == QSize(40, 20) &&
                        QRectF(placed.imageRects[0]).center() ==
                            QRectF(placed.clipRects[0]).center() + QPointF(3, -2) &&
                        placed.imageRects[1] == automatic.imageRects[1] && changes == 1 &&
                        document.contentRevision() == revision &&
                        document.items()[0].image.cacheKey() == original,
                    "manual edits affect only placed bounds, never slots, neighbors or source "
                    "revisions");
            require(!document.setStaticPlacement(first, placement) && changes == 1,
                    "unchanged placement emits no notification");
            document.setOutputType(ImageStitchingOutputType::PdfDocument);
            auto pdf = document.pdfSettings();
            pdf.marginTopMm += 1;
            document.setPdfSettings(pdf);
            document.setOutputType(ImageStitchingOutputType::StaticImage);
            settings.background = Qt::transparent;
            document.setSettings(settings);
            require(document.items()[0].staticPlacement == placement,
                    "mode, PDF geometry and background changes preserve static placement");
            document.rotate(first, true);
            require(document.layout().imageRects[0].size() == QSize(20, 40) &&
                        document.items()[0].staticPlacement == placement,
                    "rotation preserves manual source scale and center offset");
            document.flip(first, true);
            require(document.items()[0].staticPlacement == placement, "flip preserves placement");
            document.resetTransform(first);
            require(document.items()[0].staticPlacement == ImageStitchingStaticPlacement{} &&
                        document.layout().imageRects == automatic.imageRects,
                    "reset restores orientation and automatic placement");
            document.setStaticPlacement(first, placement);
            document.resetTransform(first);
            require(document.items()[0].staticPlacement == ImageStitchingStaticPlacement{},
                    "reset clears placement even when orientation is already identity");
            document.setStaticPlacement(first, placement);
            document.swapItems(first, second);
            require(document.items()[1].staticPlacement == ImageStitchingStaticPlacement{},
                    "swapping clears the affected placement");
            document.setStaticPlacement(first, placement);
            settings.spacing += 1;
            document.setSettings(settings);
            require(document.items()[1].staticPlacement == ImageStitchingStaticPlacement{},
                    "layout geometry edits restore automatic placements");
            document.setStaticPlacement(first, placement);
            document.replace(first, fixture({80, 40}), QStringLiteral("replacement"));
            require(document.items()[1].staticPlacement == ImageStitchingStaticPlacement{},
                    "replacement clears placement");
            for (const qreal invalid : {qreal(0), qreal(-1), std::numeric_limits<qreal>::infinity(),
                                        std::numeric_limits<qreal>::quiet_NaN(), qreal(1e20)}) {
                require(!document.setStaticPlacement(first, {{}, invalid}),
                        "invalid and unrepresentable scales are rejected");
            }
            require(!document.setStaticPlacement(first, {{1e20, 0}, 1}) &&
                        !document.setStaticPlacement(999, placement),
                    "invalid offsets and unknown IDs are rejected");
        }
    }
}

void staticPlacementPixelsAndRegions() {
    const auto baseline = imageStitchingResourceUsage().reservedBytes;
    {
        ImageStitchingDocument document;
        const auto first =
            document.append(fixture({40, 20}, QColor(255, 0, 0, 128)), QStringLiteral("alpha"));
        document.append(fixture({40, 20}, Qt::blue), QStringLiteral("neighbor"));
        const auto output = [&] {
            return materializeImageStitchingRows(
                prepareImageStitchingRows(document.items(), document.settings()));
        };
        document.setStaticPlacement(first, {{}, .5});
        auto pixels = output();
        require(pixels.size() == QSize(40, 40) && pixels.pixelColor(0, 0) == Qt::white &&
                    pixels.pixelColor(20, 10) == QColor(255, 127, 127) &&
                    pixels.pixelColor(20, 30) == Qt::blue,
                "shrink exposes background and composites source alpha exactly once");
        document.setStaticPlacement(first, {{10, 0}, 2});
        pixels = output();
        require(pixels.pixelColor(0, 0) == QColor(255, 127, 127) &&
                    pixels.pixelColor(39, 19) == QColor(255, 127, 127) &&
                    pixels.pixelColor(20, 20) == Qt::blue,
                "enlarged images are clipped before the neighboring slot");
        auto settings = document.settings();
        settings.background = Qt::transparent;
        document.setSettings(settings);
        document.setStaticPlacement(first, {{20, 0}, 1});
        const auto rows = prepareImageStitchingRows(document.items(), document.settings());
        pixels = materializeImageStitchingRows(rows);
        require(pixels.pixelColor(19, 10).alpha() == 0 &&
                    pixels.pixelColor(20, 10) == QColor(255, 0, 0, 128),
                "translation crops the source and leaves transparent whitespace");
        const QRect region(16, 7, 12, 21);
        QImage partial(region.size(), QImage::Format_RGBA8888);
        partial.setColorSpace(pixels.colorSpace());
        require(
            rows.readRegion(region, partial.bytesPerLine(), partial.bits(),
                            partial.sizeInBytes()) &&
                partial == pixels.copy(region),
            "partial regions crossing cropped content, whitespace and neighbors match full export");
        document.setStaticPlacement(first, {{100, 100}, 1});
        pixels = output();
        require(pixels.pixelColor(20, 10).alpha() == 0 && pixels.pixelColor(20, 30) == Qt::blue,
                "an image wholly outside its slot exports only the remaining composition");
        require(materializeImageStitchingRows(rows).pixelColor(20, 10) == QColor(255, 0, 0, 128),
                "prepared output owns an immutable placement snapshot");

        // Asymmetric colors catch source-offset errors in both opaque and blending paths.
        for (const auto background : {QColor(Qt::white), QColor(Qt::transparent)}) {
            ImageStitchingDocument colors;
            colors.append(asymmetricFixture(), QStringLiteral("colors"));
            auto config = colors.settings();
            config.background = background;
            colors.setSettings(config);
            colors.setStaticPlacement(colors.selectedId(), {{1, 0}, 1});
            const auto result =
                materializeImageStitchingRows(prepareImageStitchingRows(colors.items(), config));
            require(result.pixelColor(1, 0) == Qt::red && result.pixelColor(2, 0) == Qt::green &&
                        result.pixelColor(1, 1) == Qt::yellow &&
                        result.pixelColor(2, 1) == Qt::magenta,
                    "cropped row readers retain the correct source origin");
        }
    }
    require(imageStitchingResourceUsage().reservedBytes == baseline,
            "placement row resources retire with their readers");
}

void enlargedPlacementUsesBoundedRowsAndPreview() {
    const auto baseline = imageStitchingResourceUsage().reservedBytes;
    {
        ImageStitchingDocument document;
        const auto id = document.append(fixture({3000, 1800}, Qt::red), QStringLiteral("large"));
        require(document.setStaticPlacement(id, {{100, -20}, 4}), "large placement is admitted");
        const auto owned = imageStitchingResourceUsage().reservedBytes;
        auto rows = prepareImageStitchingRows(document.items(), document.settings());
        require(rows.isValid() &&
                    imageStitchingResourceUsage().reservedBytes - owned < 32 * 1024 * 1024,
                "an enlarged virtual image uses bounded plans and tiles instead of a full scaled "
                "raster");
        uchar pixel[4]{};
        require(rows.readRegion(QRect(0, 0, 1, 1), 4, pixel, 4) && pixel[0] == 255 &&
                    pixel[3] == 255 && rows.readRegion(QRect(2999, 1799, 1, 1), 4, pixel, 4) &&
                    pixel[0] == 255,
                "cropped lazy rows cover both output edges without materializing overflow");
        require(!rows.previewImage.isNull() && rows.previewImage.pixelColor(0, 0) == Qt::red,
                "large export preview hints apply enlarged placement and clipping");
        document.setStaticPlacement(id, {{}, .5});
        auto smaller = prepareImageStitchingRows(document.items(), document.settings());
        require(!smaller.previewImage.isNull() &&
                    smaller.previewImage.pixelColor(0, 0) == Qt::white &&
                    smaller.previewImage.pixelColor(smaller.previewImage.width() / 2,
                                                    smaller.previewImage.height() / 2) == Qt::red,
                "large export preview hints show the same shrink whitespace as native rows");
        bool cancelled = false;
        auto cancellable = prepareImageStitchingRows(document.items(), document.settings(),
                                                     [&] { return cancelled; });
        cancelled = true;
        require(!cancellable.readRegion(QRect(0, 0, 1, 1), 4, pixel, 4),
                "placed row readers honor cancellation before accessing pixels");
    }
    require(imageStitchingResourceUsage().reservedBytes == baseline,
            "all placed preview and export reservations retire");
}

void scalingAndLargeAxes() {
    ImageStitchingDocument document;
    document.append(fixture({2, 2}, Qt::blue), QStringLiteral("reference"));
    document.append(fixture({4, 2}, QColor(255, 0, 0, 128)), QStringLiteral("scaled"));
    ImageStitchingSettings settings;
    settings.background = Qt::transparent;
    const auto rows = prepareImageStitchingRows(document.items(), settings);
    const QImage output = materializeImageStitchingRows(rows);
    require(output.size() == QSize(2, 3) && output.pixelColor(0, 2).red() >= 254 &&
                output.pixelColor(0, 2).alpha() == 128,
            "uniform resizing preserves contents and alpha without cropping");

    QVector<ImageStitchingItem> items;
    for (int index = 0; index < 40; ++index)
        items.append({quint64(index + 1),
                      1,
                      fixture({1000, 1}, Qt::green),
                      {},
                      {},
                      QString::number(index),
                      {},
                      {}});
    settings.direction = ImageStitchingDirection::Horizontal;
    settings.sizing = ImageStitchingSizing::Original;
    const auto wide = prepareImageStitchingRows(items, settings);
    const QImage result = materializeImageStitchingRows(wide);
    require(result.size() == QSize(40000, 1) && result.pixelColor(39999, 0) == Qt::green,
            "row composition remains correct beyond raster painter coordinate limits");

    QVector<ImageStitchingItem> thinItems;
    thinItems.reserve(140000);
    const QImage thinImage = fixture({1000, 1});
    for (int index = 0; index < 140000; ++index)
        thinItems.append({quint64(index + 1), 1, thinImage, {}, {}, {}, {}, {}});
    const auto thinLayout = calculateImageStitchingLayout(thinItems, settings);
    require(thinLayout.isValid() && thinLayout.size == QSize(140000000, 1),
            "very wide single-row geometry fits the output image limit");
    QString error;
    int cancellationChecks = 0;
    const qint64 baseline = imageStitchingResourceUsage().reservedBytes;
    const auto rejected = prepareImageStitchingRows(
        thinItems, settings,
        [&] {
            ++cancellationChecks;
            return false;
        },
        &error);
    require(!rejected.isValid() && !error.isEmpty() && cancellationChecks == 1 &&
                imageStitchingResourceUsage().reservedBytes == baseline,
            "complete output row scratch is rejected before preparing items or reading pixels");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        geometry();
        customGridGeometry();
        customGridInitializationAndGrowth();
        customGridPixelsAndLimits();
        referenceAndManagement();
        transformations();
        transformedRowsPreserveNativePixelsAndBoundMemory();
        regionReadsAndLazyResize();
        regionReadsAndLazyResize(true);
        alphaAndRowReads();
        snapshotsAndResources();
        capturedImagesAdmitMemoryBeforePaintingAndOwnTheirReservation();
        staticPlacementGeometryAndLifecycle();
        staticPlacementPixelsAndRegions();
        enlargedPlacementUsesBoundedRowsAndPreview();
        scalingAndLargeAxes();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Collage document tests passed\n";
    return 0;
}
