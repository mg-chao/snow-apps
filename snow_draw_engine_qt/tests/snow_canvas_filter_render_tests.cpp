#include "snow_canvas_display_item.h"
#include "snow_canvas_export.h"
#include "snow_canvas_filter_render.h"
#include "snow_canvas_filter_tile_cache.h"
#include "snow_canvas_pen_mask_atlas.h"
#include "snow_canvas_render_geometry.h"
#include "snow_canvas_renderer.h"
#include "snow_draw_engine_qt/snow_canvas_custom_renderer.h"
#include "snow_draw_engine_qt/snow_canvas_region_filter.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void publicRegionFilterApiRestrictsEffectsToTheRequestedRegion() {
    QImage source(QSize(24, 16), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            source.setPixelColor(x, y, QColor((x * 17 + y * 3) % 256,
                                              (x * 5 + y * 19) % 256,
                                              (x * 11 + y * 7) % 256, 255));
        }
    }

    const QRegion region(QRect(7, 4, 8, 6));
    QImage destination = source;
    SnowCanvasRegionFilterParameters parameters;
    parameters.type = SnowCanvasFilterType::GaussianBlur;
    parameters.logicalSigma = 3.0;
    require(applySnowCanvasRegionFilter(source, destination, region, parameters),
            "the public region-filter API should accept premultiplied images");
    require(destination.pixelColor(10, 6) != source.pixelColor(10, 6),
            "the public region-filter API should modify pixels inside the region");
    for (int y = 0; y < destination.height(); ++y) {
        for (int x = 0; x < destination.width(); ++x) {
            if (region.contains(QPoint(x, y))) {
                continue;
            }
            require(destination.pixel(x, y) == source.pixel(x, y),
                    "the public region-filter API must preserve pixels outside the region");
        }
    }

    QImage invalidFormat(source.size(), QImage::Format_RGBA8888);
    invalidFormat.fill(Qt::black);
    require(!applySnowCanvasRegionFilter(invalidFormat, destination, region, parameters),
            "the public region-filter API should reject unsupported source formats");
    QImage invalidSize(source.size() - QSize(1, 0), QImage::Format_ARGB32_Premultiplied);
    invalidSize.fill(Qt::black);
    require(!applySnowCanvasRegionFilter(source, invalidSize, region, parameters),
            "the public region-filter API should reject mismatched image sizes");
}

void regionFilterSupportPixelsMatchesGaussianPlan() {
    SnowCanvasRegionFilterParameters parameters;
    parameters.type = SnowCanvasFilterType::GaussianBlur;
    int previousSupport = -1;
    for (const double logicalSigma : {1.0, 4.0, 8.0, 16.0, 32.0}) {
        for (const qreal devicePixelRatio : {1.0, 1.5, 2.0}) {
            parameters.logicalSigma = logicalSigma;
            parameters.devicePixelRatio = devicePixelRatio;
            const int support = snowCanvasRegionFilterSupportPixels(parameters);
            snow_canvas_filter_render::Parameters internal;
            internal.type = 1;
            internal.logicalSigma = logicalSigma;
            internal.devicePixelRatio = devicePixelRatio;
            require(support ==
                        snow_canvas_filter_render::gaussianBlurPlan(internal).physicalSupportRadius,
                    "the public support-radius helper must match the Gaussian plan");
            require(support >= previousSupport,
                    "the blur support radius must not shrink as sigma or dpr grows");
            previousSupport = support;
        }
    }
    parameters.logicalSigma = 8.0;
    parameters.devicePixelRatio = 1.0;
    require(snowCanvasRegionFilterSupportPixels(parameters) > 0,
            "a sigma-8 blur must report a positive support radius");
}

QImage noisyPatternImage(const QSize& size) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            image.setPixel(x, y, qRgba((x * 37 + y * 11) % 256, (x * 7 + y * 53) % 256,
                                       (x * 97 + y * 29) % 256, 255));
        }
    }
    return image;
}

void croppedRegionFilterMatchesFullFrameRender() {
    const QImage source = noisyPatternImage(QSize(120, 90));
    SnowCanvasRegionFilterParameters parameters;
    parameters.type = SnowCanvasFilterType::GaussianBlur;
    parameters.logicalSigma = 8.0;
    parameters.devicePixelRatio = 1.0;
    const int support = snowCanvasRegionFilterSupportPixels(parameters);

    // Two disjoint regions that would form independent clusters in the OCR
    // pipeline; together they exercise per-region calls on a shared crop.
    const QRegion region =
        QRegion(QRect(14, 10, 26, 14)) + QRegion(QRect(70, 52, 22, 12));

    QImage fullDestination = source;
    require(applySnowCanvasRegionFilter(source, fullDestination, region, parameters),
            "the full-frame reference render should succeed");

    const QRect crop =
        region.boundingRect().adjusted(-support, -support, support, support)
            .intersected(source.rect());
    QImage cropSource = source.copy(crop);
    QImage cropDestination = cropSource;
    SnowCanvasRegionFilterParameters cropped = parameters;
    // Anchoring the reduced sampling grid to the absolute origin keeps the
    // cropped render pixel-identical to the full-frame render.
    cropped.gridOriginInImage = QPointF(-crop.left(), -crop.top());
    SnowCanvasRegionFilterScratch scratch;
    for (const QRect& rect : region) {
        require(applySnowCanvasRegionFilter(cropSource, cropDestination,
                                            QRegion(rect.translated(-crop.topLeft())), cropped,
                                            &scratch),
                "each cropped cluster render should succeed");
    }
    // A second render through the same scratch must keep producing correct
    // pixels (scratch reuse must not corrupt state).
    QImage cropDestinationSecond = cropSource;
    for (const QRect& rect : region) {
        require(applySnowCanvasRegionFilter(cropSource, cropDestinationSecond,
                                            QRegion(rect.translated(-crop.topLeft())), cropped,
                                            &scratch),
                "a reused scratch should keep accepting renders");
    }

    for (int y = 0; y < crop.height(); ++y) {
        for (int x = 0; x < crop.width(); ++x) {
            const QPoint cropPosition(x, y);
            const QPoint imagePosition = cropPosition + crop.topLeft();
            require(cropDestination.pixel(cropPosition) == fullDestination.pixel(imagePosition),
                    "the cropped render must match the full-frame render pixel for pixel");
            require(cropDestinationSecond.pixel(cropPosition) ==
                        fullDestination.pixel(imagePosition),
                    "a reused scratch must reproduce the same pixels");
            if (!region.contains(imagePosition)) {
                require(cropDestination.pixel(cropPosition) == cropSource.pixel(cropPosition),
                        "the cropped render must preserve pixels outside the regions");
            }
        }
    }
}

void inversionPreservesPremultipliedAlpha() {
    QImage image(1, 1, QImage::Format_ARGB32_Premultiplied);
    image.setPixel(0, 0, qRgba(20, 40, 60, 100));
    snow_canvas_filter_render::Parameters parameters;
    parameters.type = 3;

    snow_canvas_filter_render::apply(image, parameters);

    const QRgb pixel = image.pixel(0, 0);
    require(qAlpha(pixel) == 100, "inversion must preserve alpha");
    require(qRed(pixel) == 80 && qGreen(pixel) == 60 && qBlue(pixel) == 40,
            "inversion must operate in premultiplied color space");
}

void grayscalePreservesPremultipliedAlpha() {
    QImage image(1, 1, QImage::Format_ARGB32_Premultiplied);
    image.setPixel(0, 0, qRgba(20, 40, 60, 100));
    snow_canvas_filter_render::Parameters parameters;
    parameters.type = 2;

    snow_canvas_filter_render::apply(image, parameters);

    const QRgb pixel = image.pixel(0, 0);
    require(qAlpha(pixel) == 100, "grayscale must preserve alpha");
    require(qRed(pixel) == qGreen(pixel) && qGreen(pixel) == qBlue(pixel),
            "grayscale must produce equal premultiplied color channels");
    require(qRed(pixel) <= qAlpha(pixel),
            "grayscale must preserve the premultiplied alpha invariant");
}

void colorEffectStrengthHasExactEndpointsAndInterpolation() {
    const QRgb original = qRgba(20, 40, 60, 100);
    const auto blend = [](int first, int second, int mix) {
        return (first * (255 - mix) + second * mix + 127) / 255;
    };
    for (std::uint32_t type : {2u, 3u}) {
        const int alpha = qAlpha(original);
        const int luminance =
            qMin(alpha,
                 (qRed(original) * 54 + qGreen(original) * 183 + qBlue(original) * 19 + 128) >> 8);
        const QRgb full = type == 2 ? qRgba(luminance, luminance, luminance, alpha)
                                    : qRgba(alpha - qRed(original), alpha - qGreen(original),
                                            alpha - qBlue(original), alpha);
        for (const auto& [strength, expectedMix] :
             {std::pair<double, int>{0.0, 0}, std::pair<double, int>{0.5, 128},
              std::pair<double, int>{1.0, 255}}) {
            QImage image(1, 1, QImage::Format_ARGB32_Premultiplied);
            image.setPixel(0, 0, original);
            snow_canvas_filter_render::Parameters parameters;
            parameters.type = type;
            parameters.strength = strength;
            snow_canvas_filter_render::apply(image, parameters);
            const QRgb expected = expectedMix == 0 ? original
                                  : expectedMix == 255
                                      ? full
                                      : qRgba(blend(qRed(original), qRed(full), expectedMix),
                                              blend(qGreen(original), qGreen(full), expectedMix),
                                              blend(qBlue(original), qBlue(full), expectedMix),
                                              blend(qAlpha(original), qAlpha(full), expectedMix));
            require(image.pixel(0, 0) == expected,
                    "color-effect strength must use exact 8-bit interpolation");
        }
        for (const auto& [strength, expected] :
             {std::pair<double, QRgb>{std::numeric_limits<double>::quiet_NaN(), full},
              std::pair<double, QRgb>{std::numeric_limits<double>::infinity(), full},
              std::pair<double, QRgb>{-std::numeric_limits<double>::infinity(), original}}) {
            QImage image(1, 1, QImage::Format_ARGB32_Premultiplied);
            image.setPixel(0, 0, original);
            snow_canvas_filter_render::Parameters parameters;
            parameters.type = type;
            parameters.strength = strength;
            snow_canvas_filter_render::apply(image, parameters);
            require(image.pixel(0, 0) == expected,
                    "non-finite color-effect strengths must normalize consistently");
        }
    }
}

void partialFilterRenderUsesABoundedSurface() {
    QImage image(QSize(1000, 1000), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    SnowSceneDisplayItem below{};
    below.kind = SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT;
    below.width = 100.0;
    below.height = 100.0;
    below.fill = SnowColorRgba8{255, 0, 0, 255};
    below.fill_style = SNOW_FILL_STYLE_SOLID;
    below.opacity = 1.0;
    SnowSceneDisplayItem filter{};
    filter.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
    filter.width = 40.0;
    filter.height = 40.0;
    filter.filter = snow_filter_render_spec_resolve(3, 1.0);
    filter.opacity = 1.0;
    const SnowCanvasSceneItem items[] = {
        SnowCanvasSceneItem(below),
        SnowCanvasSceneItem(filter),
    };
    SceneDisplayInfo displayInfo{};
    displayInfo.surface_width = image.width();
    displayInfo.surface_height = image.height();
    displayInfo.camera_zoom = 1.0;
    const QRegion exposed(QRect(490, 490, 20, 20));

    QPainter painter(&image);
    painter.setClipRegion(exposed);
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &painter, &displayInfo, items, 2, exposed, nullptr, 0});
    painter.end();

    const auto diagnostics = snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
    require(image.pixelColor(500, 500) == QColor(0, 255, 255),
            "the bounded compositor must preserve filter output");
    require(diagnostics.usedFilterPath, "an intersecting filter must use the filtered path");
    require(diagnostics.workingSurfacePixelCount == 20u * 20u,
            "the working surface must follow the exposed bounds for a local color filter");
    require(diagnostics.peakEffectPixelCount <= diagnostics.workingSurfacePixelCount,
            "an effect crop must not exceed its bounded working surface");
    require(diagnostics.maskPixelCount == 0 && diagnostics.opaqueRectDispatchCount == 1,
            "an aligned opaque color filter must bypass Alpha8 mask construction");

    filter.filter = snow_filter_render_spec_resolve(3, 0.5);
    const SnowCanvasSceneItem ignoredStrengthItems[] = {
        SnowCanvasSceneItem(below),
        SnowCanvasSceneItem(filter),
    };
    image.fill(Qt::transparent);
    QPainter halfPainter(&image);
    halfPainter.setClipRegion(exposed);
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &halfPainter, &displayInfo, ignoredStrengthItems, 2, exposed, nullptr, 0});
    halfPainter.end();
    const auto halfDiagnostics = snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
    require(image.pixelColor(500, 500) == QColor(0, 255, 255) &&
                halfDiagnostics.opaqueRectDispatchCount == 1 &&
                halfDiagnostics.constantOpacityRectDispatchCount == 0 &&
                halfDiagnostics.maskPixelCount == 0,
            "scene color-filter strength must be ignored on the mask-free path");

    filter.filter = snow_filter_render_spec_resolve(3, 0.0);
    const SnowCanvasSceneItem zeroStrengthItems[] = {
        SnowCanvasSceneItem(below),
        SnowCanvasSceneItem(filter),
    };
    image.fill(Qt::transparent);
    QPainter zeroPainter(&image);
    zeroPainter.setClipRegion(exposed);
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &zeroPainter, &displayInfo, zeroStrengthItems, 2, exposed, nullptr, 0});
    zeroPainter.end();
    const auto zeroDiagnostics = snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
    require(image.pixelColor(500, 500) == QColor(0, 255, 255) && zeroDiagnostics.usedFilterPath,
            "a zero-strength color filter must still render its full effect");

    filter.filter = snow_filter_render_spec_resolve(3, 1.0);
    filter.rotation = 0.2;
    const SnowCanvasSceneItem rotatedItems[] = {
        SnowCanvasSceneItem(below),
        SnowCanvasSceneItem(filter),
    };
    image.fill(Qt::transparent);
    QPainter rotatedPainter(&image);
    rotatedPainter.setClipRegion(exposed);
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &rotatedPainter, &displayInfo, rotatedItems, 2, exposed, nullptr, 0});
    rotatedPainter.end();
    const auto rotatedDiagnostics = snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
    require(rotatedDiagnostics.maskPixelCount > 0 &&
                rotatedDiagnostics.opaqueRectDispatchCount == 0 &&
                rotatedDiagnostics.constantOpacityRectDispatchCount == 0,
            "a rotated color filter must retain antialiased Alpha8 coverage");
}

void uniformGaussianBlurPreservesColor() {
    QImage image(96, 64, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(30, 90, 150, 210));
    snow_canvas_filter_render::Parameters parameters;
    parameters.type = 1;
    parameters.logicalSigma = 8.0;

    snow_canvas_filter_render::apply(image, parameters);

    const QColor pixel = image.pixelColor(48, 32);
    require(std::abs(pixel.red() - 30) <= 1 && std::abs(pixel.green() - 90) <= 1 &&
                std::abs(pixel.blue() - 150) <= 1 && pixel.alpha() == 210,
            "clamped Gaussian edges must preserve a uniform premultiplied image within rounding");
}

QImage referenceGaussian(const QImage& source, double sigma) {
    const int radius = qCeil(3.0 * sigma + 1.0);
    std::vector<double> kernel(static_cast<std::size_t>(radius * 2 + 1));
    double weightSum = 0.0;
    std::size_t kernelIndex = 0;
    for (int offset = -radius; offset <= radius; ++offset) {
        const double weight = std::exp(-(offset * offset) / (2.0 * sigma * sigma));
        kernel[kernelIndex] = weight;
        weightSum += weight;
        ++kernelIndex;
    }
    for (double& weight : kernel) {
        weight /= weightSum;
    }
    std::vector<double> horizontal(static_cast<std::size_t>(source.width()) * source.height() * 4u);
    for (int y = 0; y < source.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(source.constScanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            double values[4]{};
            kernelIndex = 0;
            for (int offset = -radius; offset <= radius; ++offset) {
                const QRgb pixel = line[qBound(0, x + offset, source.width() - 1)];
                const double weight = kernel[kernelIndex];
                values[0] += qRed(pixel) * weight;
                values[1] += qGreen(pixel) * weight;
                values[2] += qBlue(pixel) * weight;
                values[3] += qAlpha(pixel) * weight;
                ++kernelIndex;
            }
            const std::size_t index = (static_cast<std::size_t>(y) * source.width() + x) * 4u;
            std::copy(values, values + 4, horizontal.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }
    QImage result(source.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < source.height(); ++y) {
        auto* output = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            double values[4]{};
            kernelIndex = 0;
            for (int offset = -radius; offset <= radius; ++offset) {
                const int sampleY = qBound(0, y + offset, source.height() - 1);
                const std::size_t index =
                    (static_cast<std::size_t>(sampleY) * source.width() + x) * 4u;
                const double weight = kernel[kernelIndex];
                for (int channel = 0; channel < 4; ++channel) {
                    values[channel] += horizontal[index + channel] * weight;
                }
                ++kernelIndex;
            }
            const int alpha = qBound(0, qRound(values[3]), 255);
            output[x] = qRgba(qMin(alpha, qBound(0, qRound(values[0]), 255)),
                              qMin(alpha, qBound(0, qRound(values[1]), 255)),
                              qMin(alpha, qBound(0, qRound(values[2]), 255)), alpha);
        }
    }
    return result;
}

double premultipliedSsim(const QImage& first, const QImage& second) {
    const std::size_t count = static_cast<std::size_t>(first.width()) * first.height() * 4u;
    double firstMean = 0.0;
    double secondMean = 0.0;
    const auto channel = [](QRgb pixel, int index) {
        switch (index) {
        case 0:
            return qRed(pixel);
        case 1:
            return qGreen(pixel);
        case 2:
            return qBlue(pixel);
        default:
            return qAlpha(pixel);
        }
    };
    for (int y = 0; y < first.height(); ++y) {
        const auto* a = reinterpret_cast<const QRgb*>(first.constScanLine(y));
        const auto* b = reinterpret_cast<const QRgb*>(second.constScanLine(y));
        for (int x = 0; x < first.width(); ++x) {
            for (int c = 0; c < 4; ++c) {
                firstMean += channel(a[x], c);
                secondMean += channel(b[x], c);
            }
        }
    }
    firstMean /= static_cast<double>(count);
    secondMean /= static_cast<double>(count);
    double firstVariance = 0.0;
    double secondVariance = 0.0;
    double covariance = 0.0;
    for (int y = 0; y < first.height(); ++y) {
        const auto* a = reinterpret_cast<const QRgb*>(first.constScanLine(y));
        const auto* b = reinterpret_cast<const QRgb*>(second.constScanLine(y));
        for (int x = 0; x < first.width(); ++x) {
            for (int c = 0; c < 4; ++c) {
                const double da = channel(a[x], c) - firstMean;
                const double db = channel(b[x], c) - secondMean;
                firstVariance += da * da;
                secondVariance += db * db;
                covariance += da * db;
            }
        }
    }
    const double denominator = static_cast<double>(qMax<std::size_t>(1, count - 1));
    firstVariance /= denominator;
    secondVariance /= denominator;
    covariance /= denominator;
    constexpr double c1 = 6.5025;
    constexpr double c2 = 58.5225;
    return ((2.0 * firstMean * secondMean + c1) * (2.0 * covariance + c2)) /
           ((firstMean * firstMean + secondMean * secondMean + c1) *
            (firstVariance + secondVariance + c2));
}

void approximateGaussianMeetsReferenceQualityFloor() {
    struct Fixture {
        QImage image;
        double sigma;
    };
    std::vector<Fixture> fixtures;
    const auto make = [](int seed) {
        QImage image(96, 64, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < image.height(); ++y) {
            auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const int alpha = 64 + ((x * 7 + y * 13 + seed) % 192);
                line[x] = qRgba((x * 3 + seed) % (alpha + 1), (y * 5 + seed) % (alpha + 1),
                                (x + y * 2 + seed) % (alpha + 1), alpha);
            }
        }
        return image;
    };
    fixtures.push_back({make(3), 0.5});
    QImage text = make(11);
    {
        QPainter painter(&text);
        painter.setPen(Qt::white);
        painter.setFont(QFont("Arial", 9));
        painter.drawText(QRect(2, 2, 92, 60), Qt::AlignCenter, "Small text 1px");
    }
    fixtures.push_back({text, 1.0});
    QImage lines(96, 64, QImage::Format_ARGB32_Premultiplied);
    lines.fill(Qt::transparent);
    {
        QPainter painter(&lines);
        painter.setPen(QPen(Qt::white, 1));
        painter.drawLine(48, 0, 48, 63);
        painter.drawLine(0, 32, 95, 32);
    }
    fixtures.push_back({lines, 2.0});
    QImage checker(96, 64, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 96; ++x)
            checker.setPixel(x, y, ((x / 2 + y / 2) & 1) ? qRgb(255, 255, 255) : qRgb(0, 0, 0));
    fixtures.push_back({checker, 4.0});
    QImage impulse(96, 64, QImage::Format_ARGB32_Premultiplied);
    impulse.fill(Qt::transparent);
    impulse.setPixel(48, 32, qRgba(255, 64, 32, 255));
    fixtures.push_back({impulse, 8.0});
    QImage edge(96, 64, QImage::Format_ARGB32_Premultiplied);
    edge.fill(Qt::black);
    {
        QPainter painter(&edge);
        painter.fillRect(48, 0, 48, 64, QColor(20, 220, 80));
    }
    fixtures.push_back({edge, 18.5});
    fixtures.push_back({make(29), 64.0});
    fixtures.push_back({make(41), 128.0});

    double corpusSsim = 0.0;
    for (const Fixture& fixture : fixtures) {
        const QImage reference = referenceGaussian(fixture.image, fixture.sigma);
        QImage approximate = fixture.image;
        snow_canvas_filter_render::Parameters parameters;
        parameters.type = 1;
        parameters.logicalSigma = fixture.sigma;
        snow_canvas_filter_render::apply(approximate, parameters, nullptr,
                                         snow_canvas_filter_render::ExecutionOptions{true, true});
        const double ssim = premultipliedSsim(reference, approximate);
        require(ssim >= 0.85, "each Gaussian reference fixture must reach SSIM 0.85");
        corpusSsim += ssim;
        for (int y = 0; y < approximate.height(); ++y) {
            const auto* line = reinterpret_cast<const QRgb*>(approximate.constScanLine(y));
            for (int x = 0; x < approximate.width(); ++x) {
                require(qRed(line[x]) <= qAlpha(line[x]) && qGreen(line[x]) <= qAlpha(line[x]) &&
                            qBlue(line[x]) <= qAlpha(line[x]),
                        "Gaussian reference fixtures must remain premultiplied");
            }
        }
    }
    require(corpusSsim / static_cast<double>(fixtures.size()) >= 0.90,
            "the Gaussian reference corpus must reach mean SSIM 0.90");
}

void mosaicIntensityDoesNotAffectTransparency() {
    QImage source(4, 1, QImage::Format_ARGB32_Premultiplied);
    source.setPixel(0, 0, qRgba(0, 0, 0, 255));
    source.setPixel(1, 0, qRgba(64, 64, 64, 255));
    source.setPixel(2, 0, qRgba(128, 128, 128, 255));
    source.setPixel(3, 0, qRgba(255, 255, 255, 255));

    snow_canvas_filter_render::Parameters parameters;
    parameters.type = 0;
    parameters.logicalBlockSize = 4.0;

    QImage lowIntensity = source;
    parameters.strength = 0.0;
    snow_canvas_filter_render::apply(lowIntensity, parameters);

    QImage highIntensity = source;
    parameters.strength = 1.0;
    snow_canvas_filter_render::apply(highIntensity, parameters);

    require(lowIntensity == highIntensity,
            "mosaic intensity must not alter source-to-mosaic transparency");
    for (int x = 0; x < lowIntensity.width(); ++x) {
        require(lowIntensity.pixel(x, 0) == source.pixel(2, 0),
                "mosaic must replace every cell with its fully opaque sampled color");
    }
}

void mosaicRenderingIsIndependentOfFilterBounds() {
    const QSize imageSize(128, 128);
    QImage background(imageSize, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < background.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(background.scanLine(y));
        for (int x = 0; x < background.width(); ++x) {
            line[x] = qRgba(x * 2, y * 2, (x + y) / 2, 255);
        }
    }

    SnowSceneDisplayItem filter{};
    filter.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
    filter.width = 32.0;
    filter.height = 32.0;
    filter.filter = snow_filter_render_spec_resolve(0, 0.5);
    filter.opacity = 1.0;
    SceneDisplayInfo displayInfo{};
    displayInfo.surface_width = imageSize.width();
    displayInfo.surface_height = imageSize.height();
    displayInfo.camera_zoom = 1.0;

    QImage smallFilter(imageSize, QImage::Format_ARGB32_Premultiplied);
    smallFilter.fill(Qt::transparent);
    QPainter smallPainter(&smallFilter);
    const SnowCanvasSceneItem smallItem(filter);
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &smallPainter,
        &displayInfo,
        &smallItem,
        1,
        QRegion(smallFilter.rect()),
        nullptr,
        0,
        &background,
    });
    smallPainter.end();

    filter.width = 96.0;
    filter.height = 96.0;
    QImage largeFilter(imageSize, QImage::Format_ARGB32_Premultiplied);
    largeFilter.fill(Qt::transparent);
    QPainter largePainter(&largeFilter);
    const SnowCanvasSceneItem largeItem(filter);
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &largePainter,
        &displayInfo,
        &largeItem,
        1,
        QRegion(largeFilter.rect()),
        nullptr,
        0,
        &background,
    });
    largePainter.end();

    for (int y = 52; y < 76; ++y) {
        for (int x = 52; x < 76; ++x) {
            require(smallFilter.pixel(x, y) == largeFilter.pixel(x, y),
                    "mosaic pixels inside equal filter coverage must not depend on filter bounds");
        }
    }
}

void adaptiveGaussianReductionAndOpaqueMaskStayValid() {
    const QSize size(513, 257);
    QImage source(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < source.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            const int alpha = 96 + ((x * 7 + y * 11) & 0x9f);
            line[x] = qRgba((x * 5 + y * 3) % (alpha + 1), (x * 2 + y * 9) % (alpha + 1),
                            (x * 13 + y) % (alpha + 1), alpha);
        }
    }

    struct ReductionCase {
        double sigma;
        qreal devicePixelRatio;
        int expectedFactor;
    };
    for (const ReductionCase reduction : {
             ReductionCase{0.5, 1.0, 1},
             ReductionCase{1.99, 1.0, 1},
             ReductionCase{2.0, 1.0, 2},
             ReductionCase{3.99, 1.0, 2},
             ReductionCase{4.0, 1.0, 4},
             ReductionCase{4.1, 1.0, 4},
             ReductionCase{7.99, 1.0, 4},
             ReductionCase{8.0, 1.0, 8},
             ReductionCase{15.99, 1.0, 8},
             ReductionCase{16.0, 1.0, 16},
             ReductionCase{18.5, 1.0, 16},
             ReductionCase{18.5, 2.0, 32},
             ReductionCase{32.0, 1.0, 32},
             ReductionCase{128.0, 1.0, 64},
         }) {
        QImage scalar = source;
        QImage pooled = source;
        snow_canvas_filter_render::Parameters parameters;
        parameters.type = 1;
        parameters.logicalSigma = reduction.sigma;
        parameters.logicalSamplingRadius = reduction.sigma * 3.0 + 1.0;
        parameters.devicePixelRatio = reduction.devicePixelRatio;
        snow_canvas_filter_render::RenderWorkspace scalarWorkspace(0);
        snow_canvas_filter_render::RenderWorkspace pooledWorkspace(0);
        snow_canvas_filter_render::apply(scalar, parameters, &scalarWorkspace,
                                         snow_canvas_filter_render::ExecutionOptions{true, true});
        snow_canvas_filter_render::apply(pooled, parameters, &pooledWorkspace,
                                         snow_canvas_filter_render::ExecutionOptions{false, false});
        require(scalar == pooled,
                "adaptive Gaussian reduction must be deterministic across execution modes");
        require(pooledWorkspace.diagnostics().adaptiveBlurFactor == reduction.expectedFactor,
                "Gaussian reduction must select the expected aggressive power-of-two factor");
        for (int y = 0; y < pooled.height(); ++y) {
            const auto* line = reinterpret_cast<const QRgb*>(pooled.constScanLine(y));
            for (int x = 0; x < pooled.width(); ++x) {
                require(qRed(line[x]) <= qAlpha(line[x]) && qGreen(line[x]) <= qAlpha(line[x]) &&
                            qBlue(line[x]) <= qAlpha(line[x]),
                        "aggressively reduced Gaussian output must remain premultiplied");
            }
        }
    }

    snow_canvas_filter_render::Parameters lowSigma;
    lowSigma.type = 1;
    lowSigma.logicalSigma = 0.5;
    lowSigma.logicalSamplingRadius = 2.5;
    QImage expected = source;
    snow_canvas_filter_render::apply(expected, lowSigma);
    QImage actual(size, QImage::Format_ARGB32_Premultiplied);
    actual.fill(Qt::transparent);
    QImage opaqueMask(size, QImage::Format_Alpha8);
    opaqueMask.fill(255);
    require(
        snow_canvas_filter_render::applyMasked(source, actual, opaqueMask, source.rect(), lowSigma),
        "factor-one opaque Gaussian mask must render successfully");
    require(actual == expected,
            "factor-one opaque Gaussian mask must match direct Gaussian output");
}

QRgb referenceBlend(QRgb current, QRgb target, int mix) {
    const auto blend = [mix](int first, int second) {
        return (first * (255 - mix) + second * mix + 127) / 255;
    };
    const int alpha = blend(qAlpha(current), qAlpha(target));
    return qRgba(qMin(alpha, blend(qRed(current), qRed(target))),
                 qMin(alpha, blend(qGreen(current), qGreen(target))),
                 qMin(alpha, blend(qBlue(current), qBlue(target))), alpha);
}

void maskedMosaicMatchesReferenceAcrossCoverageAndOrigins() {
    const QSize size(37, 23);
    QImage source(size, QImage::Format_ARGB32_Premultiplied);
    QImage destination(size, QImage::Format_ARGB32_Premultiplied);
    QImage mask(size, QImage::Format_Alpha8);
    std::mt19937 generator(0x7612u);
    for (int y = 0; y < size.height(); ++y) {
        auto* sourceLine = reinterpret_cast<QRgb*>(source.scanLine(y));
        auto* destinationLine = reinterpret_cast<QRgb*>(destination.scanLine(y));
        auto* maskLine = mask.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            const int sourceAlpha = static_cast<int>(generator() & 0xffu);
            const int destinationAlpha = static_cast<int>(generator() & 0xffu);
            sourceLine[x] = qRgba(static_cast<int>(generator() % (sourceAlpha + 1)),
                                  static_cast<int>(generator() % (sourceAlpha + 1)),
                                  static_cast<int>(generator() % (sourceAlpha + 1)), sourceAlpha);
            destinationLine[x] =
                qRgba(static_cast<int>(generator() % (destinationAlpha + 1)),
                      static_cast<int>(generator() % (destinationAlpha + 1)),
                      static_cast<int>(generator() % (destinationAlpha + 1)), destinationAlpha);
            const int pattern = (x + y * 3) % 7;
            maskLine[x] =
                static_cast<uchar>(pattern == 0 ? 0 : (pattern <= 3 ? 255 : pattern * 31));
        }
    }
    const QRect affected(2, 1, size.width() - 5, size.height() - 3);
    for (int physicalBlock : {1, 2, 7, 12}) {
        for (qreal dpr : {1.0, 1.25, 2.0}) {
            for (const QPointF origin :
                 {QPointF(0.0, 0.0), QPointF(-3.0, 5.0), QPointF(2.4, -4.6)}) {
                snow_canvas_filter_render::Parameters parameters;
                parameters.type = 0;
                parameters.logicalBlockSize = physicalBlock / dpr;
                parameters.devicePixelRatio = dpr;
                parameters.gridOriginInImage = origin;
                QImage expected = destination;
                const int block =
                    qMax(1, qRound(parameters.logicalBlockSize * parameters.devicePixelRatio));
                const int originX = qRound(origin.x());
                const int originY = qRound(origin.y());
                const int firstX =
                    originX +
                    static_cast<int>(std::floor(-originX / static_cast<double>(block))) * block;
                const int firstY =
                    originY +
                    static_cast<int>(std::floor(-originY / static_cast<double>(block))) * block;
                for (int y = affected.top(); y <= affected.bottom(); ++y) {
                    auto* expectedLine = reinterpret_cast<QRgb*>(expected.scanLine(y));
                    const auto* maskLine = mask.constScanLine(y);
                    const int row = (y - firstY) / block;
                    const int sampleY =
                        qBound(0, firstY + row * block + block / 2, size.height() - 1);
                    for (int x = affected.left(); x <= affected.right(); ++x) {
                        const int column = (x - firstX) / block;
                        const int sampleX =
                            qBound(0, firstX + column * block + block / 2, size.width() - 1);
                        expectedLine[x] = referenceBlend(
                            expectedLine[x], source.pixel(sampleX, sampleY), maskLine[x]);
                    }
                }
                QImage actual = destination;
                require(snow_canvas_filter_render::applyMasked(source, actual, mask, affected,
                                                               parameters),
                        "masked mosaic must accept valid premultiplied buffers");
                require(actual == expected,
                        "optimized masked mosaic must match the scalar reference");
            }
        }
    }
}

void mosaicPlanningCropsDirtyOutputAndBatchesEquivalentBlocks() {
    const QSize size(192, 128);
    QImage background(size, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(40, 90, 160));
    SceneDisplayInfo displayInfo{};
    displayInfo.surface_width = size.width();
    displayInfo.surface_height = size.height();
    displayInfo.camera_zoom = 1.0;

    SnowSceneDisplayItem largeFilter{};
    largeFilter.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
    largeFilter.width = 160.0;
    largeFilter.height = 120.0;
    largeFilter.filter = snow_filter_render_spec_resolve(0, 1.0);
    largeFilter.opacity = 0.85;
    const SnowCanvasSceneItem largeItem(largeFilter);
    const QRegion dirty(QRect(64, 40, 64, 64));
    QImage dirtyOutput(size, QImage::Format_ARGB32_Premultiplied);
    dirtyOutput.fill(Qt::transparent);
    QPainter dirtyPainter(&dirtyOutput);
    dirtyPainter.setClipRegion(dirty);
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &dirtyPainter,
        &displayInfo,
        &largeItem,
        1,
        dirty,
        nullptr,
        0,
        &background,
    });
    dirtyPainter.end();
    const auto dirtyDiagnostics = snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
    require(dirtyDiagnostics.maskPixelCount == 64u * 64u,
            "a dirty mosaic pass must filter only its required destination pixels");
    require(dirtyDiagnostics.peakEffectPixelCount == 64u * 64u,
            "mosaic effect diagnostics must report the actual destination size");
    require(dirtyDiagnostics.workingSurfacePixelCount <= 78u * 78u,
            "the mosaic source halo must use the half-block dependency radius");

    SnowSceneDisplayItem first = largeFilter;
    first.width = 96.0;
    first.height = 96.0;
    first.filter = snow_filter_render_spec_resolve(0, 0.50);
    SnowSceneDisplayItem second = first;
    second.center_x = 4.0;
    second.filter = snow_filter_render_spec_resolve(0, 0.52);
    const SnowCanvasSceneItem groupedItems[] = {
        SnowCanvasSceneItem(first),
        SnowCanvasSceneItem(second),
    };
    QImage groupedOutput(size, QImage::Format_ARGB32_Premultiplied);
    groupedOutput.fill(Qt::transparent);
    QPainter groupedPainter(&groupedOutput);
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &groupedPainter,
        &displayInfo,
        groupedItems,
        2,
        QRegion(groupedOutput.rect()),
        nullptr,
        0,
        &background,
    });
    groupedPainter.end();
    const auto groupedDiagnostics = snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
    require(groupedDiagnostics.originalFilterCount == 2 &&
                groupedDiagnostics.effectDispatchCount == 1 &&
                groupedDiagnostics.batchedFilterCount == 1,
            "mosaics with equal physical blocks must share one effect dispatch");
}

void filteredBackgroundIsCompositedOnce() {
    QImage image(QSize(80, 80), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QImage background(image.size(), QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(200, 20, 40, 128));
    SnowSceneDisplayItem filter{};
    filter.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
    filter.width = 40.0;
    filter.height = 40.0;
    filter.filter = snow_filter_render_spec_resolve(3, 1.0);
    filter.opacity = 1.0;
    const SnowCanvasSceneItem item(filter);
    SceneDisplayInfo displayInfo{};
    displayInfo.surface_width = image.width();
    displayInfo.surface_height = image.height();
    displayInfo.camera_zoom = 1.0;

    QPainter painter(&image);
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &painter,
        &displayInfo,
        &item,
        1,
        QRegion(image.rect()),
        nullptr,
        0,
        &background,
    });
    painter.end();

    require(image.pixelColor(40, 40).alpha() == 128,
            "a semitransparent filtered background must be composited exactly once");
}

void emptySceneStillRendersBackgroundOnce() {
    QImage image(QSize(24, 18), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QImage background(image.size(), QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(30, 110, 210, 128));
    SceneDisplayInfo displayInfo{};
    displayInfo.surface_width = image.width();
    displayInfo.surface_height = image.height();
    displayInfo.camera_zoom = 1.0;

    QPainter painter(&image);
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &painter,
        &displayInfo,
        nullptr,
        0,
        QRegion(image.rect()),
        nullptr,
        0,
        &background,
    });
    painter.end();

    const QColor pixel = image.pixelColor(12, 9);
    require(pixel.alpha() == 128 && std::abs(pixel.red() - 30) <= 1 &&
                std::abs(pixel.green() - 110) <= 1 && std::abs(pixel.blue() - 210) <= 1,
            "an empty scene must composite its supplied background exactly once");
}

void penFilterUsesRawRoundStrokeMaskForEveryEffect() {
    const QSize imageSize(96, 96);
    QImage background(imageSize, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < background.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(background.scanLine(y));
        for (int x = 0; x < background.width(); ++x) {
            const int checker = ((x / 3 + y / 3) & 1) != 0 ? 96 : 0;
            line[x] = qRgb((x * 5 + checker + 17) & 255, (y * 7 + checker + 31) & 255,
                           (x * 3 + y * 2 + checker + 47) & 255);
        }
    }

    const SnowArrowPoint points[] = {
        {-20.0, 0.0},
        {0.0, 0.0},
        {0.0, 20.0},
    };
    SceneDisplayInfo displayInfo{};
    displayInfo.surface_width = imageSize.width();
    displayInfo.surface_height = imageSize.height();
    displayInfo.camera_zoom = 1.0;

    const auto render = [&](std::uint32_t type, double strokeWidth) {
        SnowSceneDisplayItem filter{};
        filter.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
        filter.is_free_draw = 1;
        filter.arrow_points = points;
        filter.arrow_point_count = static_cast<std::uint32_t>(std::size(points));
        filter.stroke_width = strokeWidth;
        filter.filter = snow_filter_render_spec_resolve(type, 0.75);
        filter.opacity = 1.0;
        const SnowCanvasSceneItem item(filter);

        QImage output(imageSize, QImage::Format_ARGB32_Premultiplied);
        output.fill(Qt::transparent);
        QPainter painter(&output);
        snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
            &painter,
            &displayInfo,
            &item,
            1,
            QRegion(output.rect()),
            nullptr,
            0,
            &background,
        });
        painter.end();
        return output;
    };

    for (const std::uint32_t type : {0u, 1u, 2u, 3u}) {
        const QImage output = render(type, 12.0);
        require(output.pixel(38, 48) != background.pixel(38, 48),
                "every filter effect must be applied through a pen stroke");
        require(output.pixel(8, 8) == background.pixel(8, 8),
                "pen filtering must leave pixels outside the stroke unchanged");
    }

    const QImage narrow = render(3, 4.0);
    const QImage wide = render(3, 12.0);
    require(narrow.pixel(38, 52) == background.pixel(38, 52) &&
                wide.pixel(38, 52) != background.pixel(38, 52),
            "pen filter width must expand the mask around the raw centerline");
    require(wide.pixel(24, 48) != background.pixel(24, 48),
            "pen filter endpoints must use round caps");
    require(wide.pixel(52, 44) != background.pixel(52, 44),
            "pen filter corners must use round joins");

    const auto diagnostics = snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
    require(
        diagnostics.maskPixelCount > 0 && diagnostics.opaqueRectDispatchCount == 0,
        "pen filters must use the general path-mask branch rather than the rectangle fast path");
}

void penFilterGeometryCacheRebuildsOnlyTheFinalChunk() {
    std::vector<SnowArrowPoint> points;
    for (int index = 0; index <= 130; ++index) {
        points.push_back(SnowArrowPoint{
            static_cast<double>(index),
            static_cast<double>((index % 9) * 3),
        });
    }
    SnowSceneDisplayItem filter{};
    filter.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
    filter.element_id = SnowElementId{91, 4};
    filter.is_free_draw = 1;
    filter.arrow_points = points.data();
    filter.arrow_point_count = static_cast<std::uint32_t>(points.size());
    filter.stroke_width = 18.0;
    filter.opacity = 1.0;
    filter.filter = snow_filter_render_spec_resolve(3, 1.0);
    SnowCanvasSceneItem item(filter);

    std::size_t builds = 0;
    std::size_t reuses = 0;
    item.takePenFilterGeometryDiagnostics(&builds, &reuses);
    require(builds == 5 && reuses == 0,
            "130 Pen Filter segments must build five 32-segment geometry chunks");

    for (int index = 131; index <= 138; ++index) {
        points.push_back(SnowArrowPoint{
            static_cast<double>(index),
            static_cast<double>((index % 9) * 3),
        });
    }
    require(item.applyPenFilterGeometryPatch(0, 1, 131, points.data() + 131, 8, false),
            "the incremental Pen Filter geometry patch must apply");
    builds = 0;
    reuses = 0;
    item.takePenFilterGeometryDiagnostics(&builds, &reuses);
    require(builds == 1 && reuses == 5,
            "an append must preserve every existing geometry chunk and build only the new tail");
    require(!item.applyPenFilterGeometryPatch(0, 2, 139, nullptr, 0, false),
            "a stale expected geometry revision must be rejected");

    std::vector<std::uint32_t> queried;
    std::size_t candidates = 0;
    item.queryPenSegmentChunks(QRectF(120.0, -20.0, 30.0, 60.0), &queried, &candidates);
    require(!queried.empty() && candidates >= queried.size(),
            "the canvas-space chunk index must find the appended tail");
    builds = 0;
    reuses = 0;
    item.takePenFilterGeometryDiagnostics(&builds, &reuses);
    require(builds == 0, "camera translation and zoom must not rerun the canvas-space stroker");

    SnowCanvasSceneItem copy(item);
    copy.queryPenSegmentChunks(QRectF(120.0, -20.0, 30.0, 60.0), &queried);
    require(!queried.empty(), "copying an item must preserve owned points and indexed geometry");
}

void retainedPenFilterMaskSkipsRasterAndScanOnReuse() {
    QImage background(QSize(160, 120), QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(30, 90, 150));
    QImage output(background.size(), QImage::Format_ARGB32_Premultiplied);
    const SnowArrowPoint points[] = {{-60.0, 0.0}, {0.0, 24.0}, {60.0, 0.0}};
    SnowSceneDisplayItem filter{};
    filter.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
    filter.element_id = SnowElementId{501, 7};
    filter.is_free_draw = 1;
    filter.arrow_points = points;
    filter.arrow_point_count = static_cast<std::uint32_t>(std::size(points));
    filter.stroke_width = 20.0;
    filter.opacity = 1.0;
    filter.filter = snow_filter_render_spec_resolve(3, 1.0);
    const SnowCanvasSceneItem item(filter);
    SceneDisplayInfo info{};
    info.surface_width = output.width();
    info.surface_height = output.height();
    info.camera_zoom = 1.0;
    snow_canvas_filter_render::RenderWorkspace workspace;
    snow_canvas_pen_mask::PenMaskAtlas penMaskAtlas;
    int cacheNamespace = 0;

    const auto render = [&]() {
        output.fill(Qt::transparent);
        QPainter painter(&output);
        snow_canvas_renderer::SceneRenderRequest request;
        request.painter = &painter;
        request.displayInfo = &info;
        request.sceneItems = &item;
        request.sceneItemCount = 1;
        request.exposedRegion = QRegion(output.rect());
        request.backgroundImage = &background;
        request.workspace = &workspace;
        request.cacheNamespace = &cacheNamespace;
        request.penMaskAtlas = &penMaskAtlas;
        snow_canvas_renderer::renderSceneItems(request);
        painter.end();
        return snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
    };

    const auto cold = render();
    const auto warm = render();
    require(cold.penAtlasMisses > 0 && cold.penAtlasHits == 0,
            "the first Pen Filter render must populate the tiled mask atlas");
    require(warm.penAtlasHits > 0 && warm.penRasterizedTileCount == 0 &&
                warm.maskScanNanoseconds == 0,
            "an unchanged Pen Filter mask must bypass tile rasterization and sparse scanning");
    require(penMaskAtlas.retainedBytes() <= penMaskAtlas.byteBudget(),
            "the retained Pen Filter atlas must remain within its 16 MiB budget");
}

void penMaskRasterizersAndTailInvalidationStayDeterministic() {
    const std::vector<std::vector<SnowArrowPoint>> paths{
        {{-70.0, 0.0}, {70.0, 0.0}},
        {{0.0, -70.0}, {0.0, 70.0}},
        {{-70.0, -70.0}, {0.0, 0.0}, {70.0, 70.0}},
        {{-70.0, 0.0}, {0.0, 50.0}, {12.0, -50.0}, {70.0, 0.0}},
        {{-70.0, 0.0}, {70.0, 0.0}, {-70.0, 20.0}, {70.0, 20.0}},
        {{0.0, 0.0}, {0.0, 0.0}},
    };
    SceneDisplayInfo info{};
    info.surface_width = 160.0;
    info.surface_height = 160.0;
    info.camera_zoom = 1.0;
    std::uint32_t id = 1;
    for (qreal dpr : {1.0, 1.25, 2.0}) {
        for (double width : {0.5, 18.0, 80.0}) {
            for (const auto& points : paths) {
                SnowSceneDisplayItem raw{};
                raw.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
                raw.element_id = SnowElementId{id++, 1};
                raw.is_free_draw = 1;
                raw.arrow_points = points.data();
                raw.arrow_point_count = static_cast<std::uint32_t>(points.size());
                raw.stroke_width = width;
                raw.opacity = 0.63;
                SnowCanvasSceneItem item(raw);
                snow_canvas_pen_mask::PenMaskAtlas scalarAtlas(0);
                snow_canvas_pen_mask::PenMaskAtlas simdAtlas(0);
                scalarAtlas.beginFrame(&scalarAtlas, info, dpr);
                simdAtlas.beginFrame(&simdAtlas, info, dpr);
                snow_canvas_filter_render::ExecutionOptions scalarOptions;
                scalarOptions.forceScalar = true;
                const int centerTile = static_cast<int>(std::floor(80.0 * dpr / 64.0));
                const auto scalar =
                    scalarAtlas.tile(item, centerTile, centerTile, info, dpr, scalarOptions);
                const auto simd = simdAtlas.tile(item, centerTile, centerTile, info, dpr);
                require(scalar && simd && scalar->alpha == simd->alpha,
                        "scalar and AVX2 Pen Filter tiles must have identical quantized alpha");
                require(scalar->coveredPixelCount == simd->coveredPixelCount,
                        "scalar and AVX2 Pen Filter tile metadata must match");
            }
        }
    }

    const SnowArrowPoint longStroke[] = {{-100.0, 0.0}, {100.0, 0.0}};
    SnowSceneDisplayItem raw{};
    raw.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
    raw.element_id = SnowElementId{900, 4};
    raw.is_free_draw = 1;
    raw.arrow_points = longStroke;
    raw.arrow_point_count = 2;
    raw.stroke_width = 18.0;
    raw.opacity = 1.0;
    SnowCanvasSceneItem item(raw);
    info.surface_width = 256.0;
    info.surface_height = 128.0;
    snow_canvas_pen_mask::PenMaskAtlas atlas;
    atlas.beginFrame(&atlas, info, 1.0);
    require(atlas.tile(item, 0, 1, info, 1.0) && atlas.tile(item, 3, 1, info, 1.0),
            "the atlas must rasterize both ends of a long stroke");
    (void)atlas.takeDiagnostics();
    AppliedPenFilterGeometryDelta delta;
    delta.elementId = raw.element_id;
    delta.oldChangedCanvasBounds = QRectF(40.0, -20.0, 70.0, 40.0);
    delta.newChangedCanvasBounds = delta.oldChangedCanvasBounds;
    atlas.invalidate(delta);
    require(atlas.tile(item, 0, 1, info, 1.0) && atlas.tile(item, 3, 1, info, 1.0),
            "the atlas must rerasterize an invalidated endpoint tile");
    const auto diagnostics = atlas.takeDiagnostics();
    require(diagnostics.hits >= 1 && diagnostics.misses >= 1,
            "changed-tail invalidation must preserve unaffected tile hits");
    atlas.removeElement(raw.element_id);
    require(atlas.entryCount() == 0 && atlas.retainedBytes() == 0,
            "deleting a Pen Filter must release all retained tiles");

    require(atlas.tile(item, 0, 1, info, 1.0) != nullptr,
            "the atlas must repopulate after element deletion");
    atlas.clear();
    require(atlas.entryCount() == 0 && atlas.retainedBytes() == 0,
            "clearing a Pen Filter atlas must release all retained tiles");
    info.camera_center_x = 0.375;
    info.camera_center_y = -0.625;
    info.camera_zoom = 1.4;
    atlas.beginFrame(&atlas, info, 1.25);
    require(atlas.entryCount() == 0,
            "fractional camera, zoom, or DPR changes must clear the atlas namespace");

    snow_canvas_pen_mask::PenMaskAtlas boundedAtlas(5000);
    info.camera_center_x = 0.0;
    info.camera_center_y = 0.0;
    info.camera_zoom = 1.0;
    boundedAtlas.beginFrame(&boundedAtlas, info, 1.0);
    for (int tileX = 0; tileX < 4; ++tileX) {
        (void)boundedAtlas.tile(item, tileX, 1, info, 1.0);
    }
    require(boundedAtlas.retainedBytes() <= boundedAtlas.byteBudget(),
            "Pen Filter tile LRU accounting must remain within its configured budget");
}

void sparseAndForcedDensePenFiltersMatch() {
    const QSize size(512, 512);
    QImage background(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(background.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            line[x] = qRgb((x * 11 + y * 3) & 255, (x * 5 + 19) & 255, (y * 7 + x * 2) & 255);
        }
    }
    const SnowArrowPoint points[] = {
        {-210.0, -210.0},
        {210.0, -210.0},
        {210.0, 210.0},
    };
    SceneDisplayInfo info{};
    info.surface_width = size.width();
    info.surface_height = size.height();
    info.camera_zoom = 1.0;

    for (std::uint32_t type : {0u, 1u, 2u, 3u}) {
        SnowSceneDisplayItem filter{};
        filter.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
        filter.element_id = SnowElementId{100 + type, 1};
        filter.is_free_draw = 1;
        filter.arrow_points = points;
        filter.arrow_point_count = static_cast<std::uint32_t>(std::size(points));
        filter.stroke_width = 20.0;
        filter.opacity = 0.73;
        filter.filter = snow_filter_render_spec_resolve(type, 0.7);
        const SnowCanvasSceneItem item(filter);

        const auto render = [&](bool forceDense) {
            QImage output(size, QImage::Format_ARGB32_Premultiplied);
            output.fill(Qt::transparent);
            snow_canvas_filter_render::RenderWorkspace workspace;
            QPainter painter(&output);
            snow_canvas_renderer::SceneRenderRequest request;
            request.painter = &painter;
            request.displayInfo = &info;
            request.sceneItems = &item;
            request.sceneItemCount = 1;
            request.exposedRegion = QRegion(output.rect());
            request.backgroundImage = &background;
            request.workspace = &workspace;
            request.execution.forceDenseMask = forceDense;
            snow_canvas_renderer::renderSceneItems(request);
            painter.end();
            return std::pair{
                output,
                snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread(),
            };
        };

        const auto [sparse, sparseDiagnostics] = render(false);
        const auto [dense, denseDiagnostics] = render(true);
        require(sparse == dense,
                "adaptive sparse and forced-dense Pen Filter output must be pixel-identical");
        require(sparseDiagnostics.sparseDispatchCount == 1 &&
                    sparseDiagnostics.maskCoveredPixelCount <
                        sparseDiagnostics.maskBoundingPixelCount / 2,
                "a large low-coverage Pen Filter mask must select sparse dispatch");
        require(denseDiagnostics.denseDispatchCount == 1 &&
                    denseDiagnostics.sparseDispatchCount == 0,
                "the test-only dense override must bypass adaptive sparse dispatch");
    }
}

class FilteredBackdropRenderer final : public SnowCanvasCustomRenderer {
  public:
    void renderBeforeCanvas(QPainter& painter, const SnowCanvasRenderContext& context) override {
        ++calls;
        painter.fillRect(context.viewportRect, QColor(255, 0, 0));
        painter.setOpacity(0.0);
    }

    int calls = 0;
};

void customBackdropIsRenderedOnceBelowFilters() {
    QImage image(QSize(80, 80), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    SnowSceneDisplayItem filter{};
    filter.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
    filter.width = 40.0;
    filter.height = 40.0;
    filter.filter = snow_filter_render_spec_resolve(3, 1.0);
    filter.opacity = 1.0;
    const SnowCanvasSceneItem item(filter);
    SceneDisplayInfo displayInfo{};
    displayInfo.surface_width = image.width();
    displayInfo.surface_height = image.height();
    displayInfo.camera_zoom = 1.0;
    FilteredBackdropRenderer renderer;
    const SnowCanvasRenderContext context{
        image.rect(),
        QRegion(image.rect()),
        QTransform(),
        1.0,
    };

    QPainter painter(&image);
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &painter,
        &displayInfo,
        &item,
        1,
        QRegion(image.rect()),
        nullptr,
        0,
        nullptr,
        &renderer,
        &context,
    });
    painter.end();

    require(renderer.calls == 1,
            "the before-canvas renderer must run exactly once for a filtered frame");
    require(image.pixelColor(40, 40) == QColor(0, 255, 255),
            "custom before-canvas output must participate in backdrop filtering");
}

void exportWithoutRuntimeStillRendersSources() {
    QImage source(QSize(16, 16), QImage::Format_ARGB32_Premultiplied);
    source.fill(QColor(40, 120, 200, 128));
    const QImage output = snow_canvas_export::renderToImage(
        nullptr, QRectF(0.0, 0.0, 16.0, 16.0), QSize(16, 16),
        {CanvasExportSource{source, QRectF(0.0, 0.0, 16.0, 16.0)}});

    const QColor pixel = output.pixelColor(8, 8);
    require(pixel.alpha() == 128 && std::abs(pixel.red() - 40) <= 1 &&
                std::abs(pixel.green() - 120) <= 1 && std::abs(pixel.blue() - 200) <= 1,
            "export sources must remain visible when no runtime scene is available");
}

void plainExportUsesDirectSourceFastPath() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    QImage source(QSize(23, 17), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < source.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            row[x] = qRgba((x * 11) & 0xff, (y * 17) & 0xff, ((x + y) * 7) & 0xff, 255);
        }
    }

    snow_canvas_export::resetDiagnosticsForCurrentThread();
    const QImage output = runtime.renderToImage(
        QRectF(0.0, 0.0, 23.0, 17.0), source.size(),
        {CanvasExportSource{source, QRectF(0.0, 0.0, 23.0, 17.0)}});
    const auto directDiagnostics = snow_canvas_export::diagnosticsForCurrentThread();
    require(output == source, "plain export fast path changed source pixels");
    require(directDiagnostics.directSourceFastPathCount == 1 &&
                directDiagnostics.fullCompositorPathCount == 0 &&
                directDiagnostics.unsynchronizedFallbackCount == 0,
            "plain authoritative scene did not use the direct-source export path");

    SnowCanvasWatermarkConfig watermark = canvas.canvasWatermarkConfig();
    watermark.text = QStringLiteral("export-path-test");
    watermark.color = QColor(0, 0, 0, 255);
    watermark.opacity = 0.5;
    require(canvas.setCanvasWatermarkConfig(watermark),
            "failed to configure watermark for export path test");
    snow_canvas_export::resetDiagnosticsForCurrentThread();
    const QImage watermarked = runtime.renderToImage(
        QRectF(0.0, 0.0, 23.0, 17.0), source.size(),
        {CanvasExportSource{source, QRectF(0.0, 0.0, 23.0, 17.0)}});
    const auto compositorDiagnostics = snow_canvas_export::diagnosticsForCurrentThread();
    require(!watermarked.isNull(), "effectful export produced a null image");
    require(compositorDiagnostics.directSourceFastPathCount == 0 &&
                compositorDiagnostics.fullCompositorPathCount == 1 &&
                compositorDiagnostics.unsynchronizedFallbackCount == 0,
            "visible watermark did not force the full export compositor");
}

void scalarAvx2AndThreadingProduceIdenticalPixels() {
    std::mt19937 generator(0x5a17u);
    for (const QSize size : {QSize(1, 1), QSize(17, 9), QSize(257, 193), QSize(641, 411)}) {
        QImage source(size, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < source.height(); ++y) {
            auto* line = reinterpret_cast<QRgb*>(source.scanLine(y));
            for (int x = 0; x < source.width(); ++x) {
                const int alpha = static_cast<int>(generator() & 255u);
                line[x] = qRgba(static_cast<int>(generator() % (alpha + 1)),
                                static_cast<int>(generator() % (alpha + 1)),
                                static_cast<int>(generator() % (alpha + 1)), alpha);
            }
        }
        for (std::uint32_t type : {0u, 1u, 2u, 3u}) {
            QImage scalar = source;
            QImage optimized = source;
            snow_canvas_filter_render::Parameters parameters;
            parameters.type = type;
            parameters.logicalBlockSize = 7.0;
            parameters.logicalSigma = 18.5;
            parameters.logicalSamplingRadius = type == 0 ? 7.0 : 56.5;
            snow_canvas_filter_render::apply(
                scalar, parameters, nullptr,
                snow_canvas_filter_render::ExecutionOptions{true, true});
            snow_canvas_filter_render::apply(
                optimized, parameters, nullptr,
                snow_canvas_filter_render::ExecutionOptions{false, false});
            if (scalar != optimized) {
                std::cerr << "backend mismatch type=" << type << " size=" << size.width() << 'x'
                          << size.height() << '\n';
                for (int y = 0; y < size.height(); ++y) {
                    for (int x = 0; x < size.width(); ++x) {
                        if (scalar.pixel(x, y) != optimized.pixel(x, y)) {
                            std::cerr << "first mismatch at " << x << ',' << y
                                      << " scalar=" << scalar.pixel(x, y)
                                      << " optimized=" << optimized.pixel(x, y) << '\n';
                            y = size.height();
                            break;
                        }
                    }
                }
                require(false,
                        "scalar/SIMD and single/pool execution must produce identical pixels");
            }
            for (int y = 0; y < optimized.height(); ++y) {
                const auto* line = reinterpret_cast<const QRgb*>(optimized.constScanLine(y));
                for (int x = 0; x < optimized.width(); ++x) {
                    require(qRed(line[x]) <= qAlpha(line[x]) &&
                                qGreen(line[x]) <= qAlpha(line[x]) &&
                                qBlue(line[x]) <= qAlpha(line[x]),
                            "every filter backend must preserve premultiplied alpha");
                }
            }
        }
    }
}

void maskedKernelsMatchAcrossBackendsAndRespectBlurMemoryBound() {
    const QSize size(641, 411);
    QImage source(size, QImage::Format_ARGB32_Premultiplied);
    QImage destination(size, QImage::Format_ARGB32_Premultiplied);
    QImage mask(size, QImage::Format_Alpha8);
    std::mt19937 generator(0x51a7u);
    for (int y = 0; y < size.height(); ++y) {
        auto* sourceLine = reinterpret_cast<QRgb*>(source.scanLine(y));
        auto* destinationLine = reinterpret_cast<QRgb*>(destination.scanLine(y));
        auto* maskLine = mask.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            const int sourceAlpha = static_cast<int>(generator() & 0xffu);
            const int destinationAlpha = static_cast<int>(generator() & 0xffu);
            sourceLine[x] = qRgba(static_cast<int>(generator() % (sourceAlpha + 1)),
                                  static_cast<int>(generator() % (sourceAlpha + 1)),
                                  static_cast<int>(generator() % (sourceAlpha + 1)), sourceAlpha);
            destinationLine[x] =
                qRgba(static_cast<int>(generator() % (destinationAlpha + 1)),
                      static_cast<int>(generator() % (destinationAlpha + 1)),
                      static_cast<int>(generator() % (destinationAlpha + 1)), destinationAlpha);
            maskLine[x] = static_cast<uchar>(generator() & 0xffu);
        }
    }
    const QRect affected(3, 2, size.width() - 8, size.height() - 7);
    const QImage croppedMask = mask.copy(affected);
    for (std::uint32_t type : {2u, 3u}) {
        for (double strength : {0.0, 0.5, 1.0}) {
            snow_canvas_filter_render::Parameters parameters;
            parameters.type = type;
            parameters.strength = strength;
            QImage scalar = destination;
            QImage optimized = destination;
            require(snow_canvas_filter_render::applyMasked(
                        source, scalar, mask, affected, parameters, nullptr,
                        snow_canvas_filter_render::ExecutionOptions{true, true}),
                    "the scalar masked color kernel must accept detached premultiplied buffers");
            require(snow_canvas_filter_render::applyMasked(
                        source, optimized, croppedMask, affected.topLeft(), affected, parameters,
                        nullptr, snow_canvas_filter_render::ExecutionOptions{false, false}),
                    "the selected masked color kernel must accept cropped mask origins");
            require(scalar == optimized,
                    "masked scalar and AVX2 color kernels must produce identical pixels");
        }
    }

    snow_canvas_filter_render::Parameters blur;
    blur.type = 1;
    blur.logicalSigma = 18.5;
    blur.logicalSamplingRadius = 56.5;
    const QRect blurMask(170, 110, 300, 190);
    QImage opaqueMask(size, QImage::Format_Alpha8);
    opaqueMask.fill(0);
    for (int y = blurMask.top(); y <= blurMask.bottom(); ++y) {
        std::fill(opaqueMask.scanLine(y) + blurMask.left(),
                  opaqueMask.scanLine(y) + blurMask.right() + 1, static_cast<uchar>(255));
    }
    QImage scalarBlur = destination;
    QImage pooledBlur = destination;
    snow_canvas_filter_render::RenderWorkspace scalarWorkspace(0);
    snow_canvas_filter_render::RenderWorkspace pooledWorkspace(0);
    require(snow_canvas_filter_render::applyMasked(
                source, scalarBlur, opaqueMask, blurMask, blur, &scalarWorkspace,
                snow_canvas_filter_render::ExecutionOptions{true, true}),
            "masked Gaussian rendering must use direct region downsampling");
    require(snow_canvas_filter_render::applyMasked(
                source, pooledBlur, opaqueMask, blurMask, blur, &pooledWorkspace,
                snow_canvas_filter_render::ExecutionOptions{false, false}),
            "pooled masked Gaussian rendering must use direct region downsampling");
    require(scalarBlur == pooledBlur,
            "masked single-thread and pooled Gaussian rendering must be deterministic");
    require(pooledBlur.pixel(0, 0) == destination.pixel(0, 0),
            "masked Gaussian rendering must not write outside the alpha mask");
    const int support = snow_canvas_filter_render::samplingRadiusPixels(blur);
    const QRect sampled =
        blurMask.adjusted(-support, -support, support, support).intersected(source.rect());
    const int factor = pooledWorkspace.diagnostics().adaptiveBlurFactor;
    const QSize reduced((sampled.width() + factor - 1) / factor,
                        (sampled.height() + factor - 1) / factor);
    const std::size_t twoReducedBuffers = static_cast<std::size_t>(reduced.width()) *
                                          static_cast<std::size_t>(reduced.height()) *
                                          sizeof(QRgb) * 2u;
    require(pooledWorkspace.diagnostics().allocatedBytes <= twoReducedBuffers,
            "masked Gaussian transient ARGB memory must not exceed two reduced buffers");
    for (int y = blurMask.top(); y <= blurMask.bottom(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(pooledBlur.constScanLine(y));
        for (int x = blurMask.left(); x <= blurMask.right(); ++x) {
            require(qRed(line[x]) <= qAlpha(line[x]) && qGreen(line[x]) <= qAlpha(line[x]) &&
                        qBlue(line[x]) <= qAlpha(line[x]),
                    "masked Gaussian upsampling must preserve premultiplied alpha");
        }
    }
}

class PatternBackdropRenderer final : public SnowCanvasCustomRenderer {
  public:
    explicit PatternBackdropRenderer(const QImage& image) : m_image(image) {}

    void renderBeforeCanvas(QPainter& painter, const SnowCanvasRenderContext& context) override {
        painter.drawImage(QRectF(context.viewportRect), m_image);
    }

  private:
    const QImage& m_image;
};

void movedFiltersMatchAFullRenderAfterDirtyRectangleUpdate() {
    const QSize logicalSize(257, 173);
    for (qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
        const QSize physicalSize(qCeil(logicalSize.width() * dpr),
                                 qCeil(logicalSize.height() * dpr));
        QImage background(physicalSize, QImage::Format_ARGB32_Premultiplied);
        background.setDevicePixelRatio(dpr);
        for (int y = 0; y < physicalSize.height(); ++y) {
            auto* line = reinterpret_cast<QRgb*>(background.scanLine(y));
            for (int x = 0; x < physicalSize.width(); ++x) {
                line[x] = qRgb((x * 29 + y * 3) & 0xff, (x * 5 + y * 47) & 0xff,
                               (x * 17 + y * 11) & 0xff);
            }
        }
        SceneDisplayInfo displayInfo{};
        displayInfo.surface_width = logicalSize.width();
        displayInfo.surface_height = logicalSize.height();
        displayInfo.camera_zoom = 1.0;
        PatternBackdropRenderer backdropRenderer(background);

        const auto render = [&](QImage& output, const SnowCanvasSceneItem& item,
                                const QRegion& exposed, bool customBackground) {
            QPainter painter(&output);
            painter.setClipRegion(exposed);
            SnowCanvasRenderContext context{QRect(QPoint(), logicalSize), exposed, QTransform(),
                                            dpr};
            snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
                &painter, &displayInfo, &item, 1, exposed, nullptr, 0,
                customBackground ? nullptr : &background,
                customBackground ? &backdropRenderer : nullptr,
                customBackground ? &context : nullptr});
            painter.end();
        };

        for (bool customBackground : {false, true}) {
            for (std::uint32_t type : {0u, 1u}) {
                SnowSceneDisplayItem filter{};
                filter.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
                filter.center_x = -38.0;
                filter.center_y = -21.0;
                filter.width = 91.0;
                filter.height = 67.0;
                filter.filter = snow_filter_render_spec_resolve(type, 0.75);
                filter.opacity = 1.0;
                const SnowCanvasSceneItem oldItem(filter);

                QImage dirtyResult(physicalSize, QImage::Format_ARGB32_Premultiplied);
                dirtyResult.setDevicePixelRatio(dpr);
                dirtyResult.fill(Qt::transparent);
                render(dirtyResult, oldItem, QRegion(QRect(QPoint(), logicalSize)),
                       customBackground);

                const auto oldBounds = snow_canvas_render_geometry::alignedRectForBounds(
                    snow_canvas_render_geometry::sceneItemBounds(displayInfo, oldItem));
                filter.center_x += 73.0;
                filter.center_y += 19.0;
                const SnowCanvasSceneItem movedItem(filter);
                const auto newBounds = snow_canvas_render_geometry::alignedRectForBounds(
                    snow_canvas_render_geometry::sceneItemBounds(displayInfo, movedItem));
                const QRegion dirty = QRegion(oldBounds) + QRegion(newBounds);
                render(dirtyResult, movedItem, dirty, customBackground);

                QImage fullResult(physicalSize, QImage::Format_ARGB32_Premultiplied);
                fullResult.setDevicePixelRatio(dpr);
                fullResult.fill(Qt::transparent);
                render(fullResult, movedItem, QRegion(QRect(QPoint(), logicalSize)),
                       customBackground);
                require(dirtyResult == fullResult,
                        type == 0
                            ? "moving a mosaic must make dirty repaint match a full render"
                            : "moving a Gaussian blur must make dirty repaint match a full render");
            }
        }
    }
}

void croppedCoverageWorkspaceAndFailurePathsStayValid() {
    const QSize size(257, 129);
    QImage source(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < source.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            const int alpha = 80 + ((x * 9 + y * 5) % 176);
            line[x] = qRgba((x * 3) % (alpha + 1), (y * 7) % (alpha + 1), (x + y * 2) % (alpha + 1),
                            alpha);
        }
    }
    const QRect coverage(71, 37, 91, 53);
    QImage fullMask(size, QImage::Format_Alpha8);
    fullMask.fill(0);
    QImage croppedMask(coverage.size(), QImage::Format_Alpha8);
    for (int y = 0; y < coverage.height(); ++y) {
        for (int x = 0; x < coverage.width(); ++x) {
            const uchar alpha = static_cast<uchar>((x * 11 + y * 17) & 255);
            fullMask.scanLine(coverage.top() + y)[coverage.left() + x] = alpha;
            croppedMask.scanLine(y)[x] = alpha;
        }
    }
    snow_canvas_filter_render::Parameters blur;
    blur.type = 1;
    blur.logicalSigma = 9.0;
    QImage fullResult = source;
    QImage croppedResult = source;
    require(snow_canvas_filter_render::applyMasked(source, fullResult, fullMask, coverage, blur),
            "the compatibility full-mask overload must remain available");
    require(snow_canvas_filter_render::applyMasked(source, croppedResult, croppedMask,
                                                   coverage.topLeft(), coverage, blur),
            "cropped masks with an independent physical origin must render");
    require(fullResult == croppedResult,
            "cropped and full-size alpha masks must produce identical output");

    QImage opaqueMask(coverage.size(), QImage::Format_Alpha8);
    opaqueMask.fill(255);
    QImage maskedOpaque = source;
    QImage directOpaque = source;
    require(snow_canvas_filter_render::applyMasked(source, maskedOpaque, opaqueMask,
                                                   coverage.topLeft(), coverage, blur),
            "an opaque cropped mask must render");
    require(snow_canvas_filter_render::applyRect(source, directOpaque, coverage, 1.0, blur),
            "opaque rectangular coverage must use the mask-free path");
    require(maskedOpaque == directOpaque,
            "mask-free opaque reconstruction must match an opaque Alpha8 mask");

    QImage partialMask(coverage.size(), QImage::Format_Alpha8);
    partialMask.fill(117);
    QImage maskedPartial = source;
    QImage directPartial = source;
    require(snow_canvas_filter_render::applyMasked(source, maskedPartial, partialMask,
                                                   coverage.topLeft(), coverage, blur) &&
                snow_canvas_filter_render::applyRect(source, directPartial, coverage, 117.0 / 255.0,
                                                     blur),
            "constant-opacity rectangular coverage must render without a mask");
    require(maskedPartial == directPartial,
            "constant-opacity reconstruction must match a constant Alpha8 mask");

    const QRect secondCoverage(9, 7, 31, 23);
    QImage regionMask(size, QImage::Format_Alpha8);
    regionMask.fill(0);
    for (const QRect& rect : QRegion(coverage) + QRegion(secondCoverage)) {
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            std::fill(regionMask.scanLine(y) + rect.left(),
                      regionMask.scanLine(y) + rect.right() + 1, static_cast<uchar>(255));
        }
    }
    QImage maskedRegion = source;
    QImage directRegion = source;
    const QRegion region = QRegion(coverage) + QRegion(secondCoverage);
    require(snow_canvas_filter_render::applyMasked(source, maskedRegion, regionMask,
                                                   region.boundingRect(), blur) &&
                snow_canvas_filter_render::applyRegion(source, directRegion, region, blur),
            "disjoint opaque rectangles must share one mask-free filtered source");
    require(maskedRegion == directRegion,
            "opaque-region reconstruction must match its Alpha8 union mask");

    for (std::uint32_t type : {2u, 3u}) {
        snow_canvas_filter_render::Parameters color;
        color.type = type;
        color.strength = 0.5;
        QImage maskedColor = source;
        QImage directColor = source;
        require(snow_canvas_filter_render::applyMasked(
                    source, maskedColor, partialMask, coverage.topLeft(), coverage, color, nullptr,
                    snow_canvas_filter_render::ExecutionOptions{true, true}) &&
                    snow_canvas_filter_render::applyRect(source, directColor, coverage,
                                                         117.0 / 255.0, color),
                "constant-opacity color coverage must render without a mask");
        require(maskedColor == directColor,
                "direct color coverage must match the exact scalar Alpha8 reference");

        color.strength = 1.0;
        maskedColor = source;
        directColor = source;
        require(snow_canvas_filter_render::applyMasked(
                    source, maskedColor, regionMask, region.boundingRect(), color, nullptr,
                    snow_canvas_filter_render::ExecutionOptions{true, true}) &&
                    snow_canvas_filter_render::applyRegion(source, directColor, region, color),
                "opaque color regions must render without a mask");
        require(maskedColor == directColor,
                "direct color regions must match the exact scalar Alpha8 reference");
    }

    snow_canvas_filter_render::RenderWorkspace workspace;
    QImage cold = source;
    snow_canvas_filter_render::apply(cold, blur, &workspace);
    require(workspace.diagnostics().allocatedBytes > 0,
            "a cold Gaussian workspace must report its allocations");
    workspace.finishFrame();
    require(workspace.retainedBytes() <= 128u * 1024u * 1024u,
            "the Gaussian image pool must respect the 128 MiB canvas limit");
    workspace.resetDiagnostics();
    QImage warm = source;
    snow_canvas_filter_render::apply(warm, blur, &workspace);
    require(workspace.diagnostics().allocatedBytes == 0,
            "a stable Gaussian frame must allocate zero workspace bytes");
    require(warm == cold, "pooled workspace reuse must not change pixels");
    if (snow_canvas_filter_render::selectedSimdBackend() ==
        snow_canvas_filter_render::SimdBackend::Avx2) {
        require(workspace.diagnostics().backend == snow_canvas_filter_render::SimdBackend::Avx2 &&
                    workspace.diagnostics().gaussianAvx2Executions > 0,
                "Gaussian may report AVX2 only after an AVX2 Gaussian stage executes");
        require(workspace.diagnostics().gaussianDownsampleAvx2Executions > 0 &&
                    workspace.diagnostics().gaussianReconstructionAvx2Executions > 0,
                "Gaussian diagnostics must report downsample and reconstruction SIMD separately");
    }
    workspace.finishFrame();

    snow_canvas_filter_render::RenderWorkspace failingWorkspace;
    failingWorkspace.setAllocationFailureForTests(true);
    QImage preserved = source;
    require(!snow_canvas_filter_render::applyMasked(source, preserved, opaqueMask,
                                                    coverage.topLeft(), coverage, blur,
                                                    &failingWorkspace),
            "workspace allocation failure must be a recoverable masked dispatch failure");
    require(preserved == source,
            "allocation failure must preserve the unfiltered destination scene");
}

void renderWorkspaceClearReleasesCanvasScratch() {
    snow_canvas_filter_render::RenderWorkspace workspace;
    (void)workspace.argbScratchA(QSize(320, 240));
    (void)workspace.mosaicSampleScratch(4096);
    workspace.finishFrame();
    require(workspace.retainedBytes() > 0,
            "a populated render workspace must retain scratch memory before clearing");

    workspace.clear();
    require(workspace.retainedBytes() == 0,
            "clearing a render workspace must release all retained scratch memory");

    workspace.resetDiagnostics();
    (void)workspace.argbScratchA(QSize(320, 240));
    require(workspace.diagnostics().allocatedBytes > 0,
            "rendering after a workspace clear must allocate cold scratch storage");
}

void stableRendererFramesReuseOpaqueGaussianWorkspace() {
    const QSize size(320, 200);
    QImage background = QImage(size, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(30, 80, 140));
    SnowSceneDisplayItem filter{};
    filter.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
    filter.width = 220.0;
    filter.height = 120.0;
    filter.filter = snow_filter_render_spec_resolve(1, 0.7);
    filter.opacity = 1.0;
    const SnowCanvasSceneItem item(filter);
    SceneDisplayInfo displayInfo{};
    displayInfo.surface_width = size.width();
    displayInfo.surface_height = size.height();
    displayInfo.camera_zoom = 1.0;
    const QRegion exposed(size.width() / 2 - 80, size.height() / 2 - 50, 160, 100);
    snow_canvas_filter_render::RenderWorkspace workspace;
    const auto paint = [&] {
        QImage output(size, QImage::Format_ARGB32_Premultiplied);
        output.fill(Qt::transparent);
        snow_canvas_renderer::FilterRenderDiagnostics diagnostics;
        QPainter painter(&output);
        snow_canvas_renderer::renderSceneItems(
            snow_canvas_renderer::SceneRenderRequest{&painter,
                                                     &displayInfo,
                                                     &item,
                                                     1,
                                                     exposed,
                                                     nullptr,
                                                     0,
                                                     &background,
                                                     nullptr,
                                                     nullptr,
                                                     nullptr,
                                                     &workspace,
                                                     {},
                                                     &diagnostics});
        painter.end();
        return diagnostics;
    };
    const auto cold = paint();
    require(cold.allocatedBytes > 0,
            "a cold renderer frame must report pooled scene and Gaussian allocations");
    const auto warm = paint();
    require(warm.allocatedBytes == 0,
            "a stable renderer frame must allocate zero filter workspace bytes");
    require(warm.opaqueRectDispatchCount == 1 && warm.maskPixelCount == 0,
            "an opaque axis-aligned Gaussian must reconstruct without an Alpha8 mask");
    require(warm.retainedWorkspaceBytes <= 128u * 1024u * 1024u,
            "renderer workspace retention must remain under the per-canvas cap");
}

void mosaicReplayRendersEveryFreeDrawChunk() {
    const QSize size(180, 120);
    QImage background(size, QImage::Format_ARGB32_Premultiplied);
    background.fill(Qt::white);

    constexpr int commandCount = 130;
    std::vector<SnowArrowPoint> points(commandCount);
    std::vector<SnowArrowPathCommand> commands(commandCount);
    for (int index = 0; index < commandCount; ++index) {
        points[static_cast<std::size_t>(index)] = SnowArrowPoint{
            -70.0 + 140.0 * index / (commandCount - 1),
            0.0,
        };
        commands[static_cast<std::size_t>(index)].kind =
            index == 0 ? SNOW_ARROW_PATH_COMMAND_MOVE_TO : SNOW_ARROW_PATH_COMMAND_LINE_TO;
        commands[static_cast<std::size_t>(index)].point = points[static_cast<std::size_t>(index)];
    }

    SnowSceneDisplayItem freeDraw{};
    freeDraw.kind = SNOW_SCENE_DISPLAY_ITEM_ARROW;
    freeDraw.stroke = SnowColorRgba8{0, 0, 0, 255};
    freeDraw.stroke_width = 6.0;
    freeDraw.arrow_points = points.data();
    freeDraw.arrow_point_count = commandCount;
    freeDraw.arrow_path_commands = commands.data();
    freeDraw.arrow_path_command_count = commandCount;
    freeDraw.arrow_type = SNOW_ARROW_TYPE_STRAIGHT;
    freeDraw.arrow_stroke_style = SNOW_ARROW_STROKE_STYLE_SOLID;
    freeDraw.is_free_draw = 1;
    freeDraw.opacity = 1.0;

    SnowSceneDisplayItem mosaic{};
    mosaic.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
    mosaic.width = size.width();
    mosaic.height = size.height();
    mosaic.filter = snow_filter_render_spec_resolve(0, 0.75);
    mosaic.opacity = 1.0;

    const SnowCanvasSceneItem filteredItems[] = {
        SnowCanvasSceneItem(mosaic),
        SnowCanvasSceneItem(freeDraw),
    };
    SceneDisplayInfo displayInfo{};
    displayInfo.surface_width = size.width();
    displayInfo.surface_height = size.height();
    displayInfo.camera_zoom = 1.0;
    const QRegion exposed(QRect(QPoint(), size));
    QImage filtered(size, QImage::Format_ARGB32_Premultiplied);
    filtered.fill(Qt::transparent);
    QPainter painter(&filtered);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRegion(exposed);
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &painter,
        &displayInfo,
        filteredItems,
        2,
        exposed,
        nullptr,
        0,
        &background,
    });
    painter.end();

    const auto diagnostics = snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
    require(diagnostics.usedFilterPath,
            "the free-draw regression must exercise the filter compositor");
    for (int x : {30, 80, 120, 150, 159}) {
        require(filtered.pixelColor(x, 60) == Qt::black,
                "mosaic replay must render every chunk of a long free-draw path");
    }
}

void adjacentLayerBatchesEffectsAndKeepsSparseComponents() {
    QImage image(400, 200, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    SnowSceneDisplayItem below{};
    below.kind = SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT;
    below.width = 400.0;
    below.height = 200.0;
    below.fill = SnowColorRgba8{120, 30, 10, 255};
    below.fill_style = SNOW_FILL_STYLE_SOLID;
    below.opacity = 1.0;
    SnowSceneDisplayItem inversion{};
    inversion.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
    inversion.width = 400.0;
    inversion.height = 200.0;
    inversion.filter = snow_filter_render_spec_resolve(3, 0.4);
    inversion.opacity = 0.6;
    SnowSceneDisplayItem strongerInversion = inversion;
    strongerInversion.filter = snow_filter_render_spec_resolve(3, 0.8);
    SnowSceneDisplayItem grayscale = inversion;
    grayscale.filter = snow_filter_render_spec_resolve(2, 0.4);
    const SnowCanvasSceneItem items[] = {
        SnowCanvasSceneItem(below),
        SnowCanvasSceneItem(inversion),
        SnowCanvasSceneItem(strongerInversion),
        SnowCanvasSceneItem(grayscale),
    };
    SceneDisplayInfo info{};
    info.surface_width = image.width();
    info.surface_height = image.height();
    info.camera_zoom = 1.0;
    const QRegion sparse = QRegion(QRect(5, 5, 20, 20)) + QRegion(QRect(370, 170, 20, 20));
    QPainter painter(&image);
    painter.setClipRegion(sparse);
    snow_canvas_renderer::renderSceneItems(
        snow_canvas_renderer::SceneRenderRequest{&painter, &info, items, 4, sparse});
    painter.end();
    const auto diagnostics = snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
    require(diagnostics.surfaceComponentCount == 2,
            "distant exposed rectangles must remain separate working components");
    require(diagnostics.filterLayerCount == 2 && diagnostics.originalFilterCount == 6 &&
                diagnostics.effectDispatchCount == 4 && diagnostics.batchedFilterCount == 2,
            "each sparse component must batch equal effects within one adjacent layer");
}

void distantSameEffectFiltersUseIndependentSpatialGroups() {
    QImage background(900, 240, QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(35, 90, 145));
    QImage output(background.size(), QImage::Format_ARGB32_Premultiplied);
    output.fill(Qt::transparent);
    SnowSceneDisplayItem left{};
    left.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
    left.element_id = SnowElementId{301, 1};
    left.center_x = -330.0;
    left.width = 120.0;
    left.height = 120.0;
    left.filter = snow_filter_render_spec_resolve(1, 0.4);
    left.opacity = 1.0;
    SnowSceneDisplayItem right = left;
    right.element_id = SnowElementId{302, 1};
    right.center_x = 330.0;
    const SnowCanvasSceneItem items[] = {
        SnowCanvasSceneItem(left),
        SnowCanvasSceneItem(right),
    };
    SceneDisplayInfo info{};
    info.surface_width = output.width();
    info.surface_height = output.height();
    info.camera_zoom = 1.0;
    QPainter painter(&output);
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &painter,
        &info,
        items,
        2,
        QRegion(output.rect()),
        nullptr,
        0,
        &background,
    });
    painter.end();
    const auto diagnostics = snow_canvas_renderer::filterRenderDiagnosticsForCurrentThread();
    require(diagnostics.originalFilterCount == 2 && diagnostics.spatialEffectGroupCount == 2 &&
                diagnostics.effectDispatchCount == 2,
            "distant same-effect filters must dispatch from two local spatial unions");
}

void filterSourceCacheKeepsOverlappingZBoundariesSeparate() {
    snow_canvas_filter_tile_cache::clear();
    int namespaceToken = 0;
    const QImage first(QSize(64, 64), QImage::Format_ARGB32_Premultiplied);
    const QImage second(QSize(64, 64), QImage::Format_ARGB32_Premultiplied);
    const QRect physicalRect(0, 0, 64, 64);
    const auto key = [&](std::uint64_t dependency, std::uint64_t node) {
        return snow_canvas_filter_tile_cache::Key{
            &namespaceToken,
            QPoint(0, 0),
            physicalRect,
            QSize(64, 64),
            0x3ff0000000000000ULL,
            17,
            dependency,
            node,
        };
    };
    require(snow_canvas_filter_tile_cache::store(key(11, 1), first, physicalRect),
            "the first filter boundary must be retained");
    require(snow_canvas_filter_tile_cache::store(key(29, 2), second, physicalRect),
            "the second overlapping filter boundary must be retained independently");
    require(snow_canvas_filter_tile_cache::find(key(11, 1)) != nullptr &&
                snow_canvas_filter_tile_cache::find(key(29, 2)) != nullptr,
            "overlapping filter boundaries must coexist for one tile coordinate");
    snow_canvas_filter_tile_cache::invalidateRegion(&namespaceToken, QRect(0, 0, 8, 8), 1.0,
                                                    11);
    require(snow_canvas_filter_tile_cache::find(key(11, 1)) == nullptr &&
                snow_canvas_filter_tile_cache::find(key(29, 2)) != nullptr,
            "dependency invalidation must remove only the affected boundary");
    snow_canvas_filter_tile_cache::clear();
}

void penFilterHoverDrawsAPathContour() {
    QImage image(QSize(120, 120), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    const SnowArrowPoint points[] = {
        SnowArrowPoint{-30.0, 0.0},
        SnowArrowPoint{30.0, 0.0},
    };
    SnowArrowPathCommand commands[2]{};
    commands[0].kind = SNOW_ARROW_PATH_COMMAND_MOVE_TO;
    commands[0].point = SnowArrowPoint{-30.0, 0.0};
    commands[1].kind = SNOW_ARROW_PATH_COMMAND_LINE_TO;
    commands[1].point = SnowArrowPoint{30.0, 0.0};
    SnowOverlayDisplayItem contour{};
    contour.kind = SNOW_OVERLAY_DISPLAY_ITEM_PEN_FILTER_CONTOUR;
    contour.stroke = SnowColorRgba8{0x40, 0x96, 0xff, 0xff};
    contour.stroke_width = 20.0;
    contour.arrow_point_count = 2;
    contour.arrow_points = points;
    contour.arrow_path_command_count = 2;
    contour.arrow_path_commands = commands;

    OverlayDisplayInfo info{};
    info.item_count = 1;
    info.surface_width = image.width();
    info.surface_height = image.height();
    info.camera_zoom = 1.0;
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    snow_canvas_renderer::renderOverlayItems(painter, info, &contour, 1, QRegion(image.rect()));
    painter.end();

    const auto hasSelectionBlueNear = [&image](int centerX, int centerY) {
        for (int y = centerY - 1; y <= centerY + 1; ++y) {
            for (int x = centerX - 1; x <= centerX + 1; ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.alpha() > 0 && pixel.blue() > pixel.red()) {
                    return true;
                }
            }
        }
        return false;
    };
    require(hasSelectionBlueNear(60, 50) && hasSelectionBlueNear(60, 70),
            "pen-filter hover feedback must trace both sides of the filtered stroke");
    require(image.pixelColor(60, 60).alpha() == 0,
            "pen-filter hover feedback must leave the filtered stroke interior unobscured");
    require(image.pixelColor(30, 40).alpha() == 0,
            "pen-filter hover feedback must not draw the old bounding box");
}

} // namespace

namespace {
class ExposedPatternBackdropRenderer final : public SnowCanvasCustomRenderer {
  public:
    explicit ExposedPatternBackdropRenderer(const QImage& image) : m_image(image) {}

    void renderBeforeCanvas(QPainter& painter, const SnowCanvasRenderContext& context) override {
        painter.save();
        painter.setClipRegion(context.exposedRegion, Qt::IntersectClip);
        painter.drawImage(QRectF(context.viewportRect), m_image);
        painter.restore();
    }

  private:
    const QImage& m_image;
};

void tiledRenderMatchesFullRender() {
    const QSize surfaceSize(1024, 1024);
    QImage background(surfaceSize, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < background.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(background.scanLine(y));
        for (int x = 0; x < background.width(); ++x) {
            line[x] = qRgba((x * 7) % 256, (y * 13) % 256, ((x + y) * 3) % 256, 255);
        }
    }

    const auto runCase = [&](int filterType, double strength, const char* label) {
        SnowSceneDisplayItem filter{};
        filter.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
        filter.center_x = 512.0;
        filter.center_y = 512.0;
        filter.width = 900.0;
        filter.height = 900.0;
        filter.filter = snow_filter_render_spec_resolve(filterType, strength);
        filter.opacity = 1.0;
        const SnowCanvasSceneItem items[] = {SnowCanvasSceneItem(filter)};
        SceneDisplayInfo displayInfo{};
        displayInfo.surface_width = surfaceSize.width();
        displayInfo.surface_height = surfaceSize.height();
        displayInfo.camera_zoom = 1.0;

        QImage full(surfaceSize, QImage::Format_ARGB32_Premultiplied);
        full.fill(Qt::transparent);
        QPainter fullPainter(&full);
        const QRegion all(QRect(QPoint(0, 0), surfaceSize));
        ExposedPatternBackdropRenderer backdrop(background);
        const SnowCanvasRenderContext context{
            QRect(QPoint(), surfaceSize),
            all,
            QTransform(),
            1.0,
        };
        snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
            &fullPainter, &displayInfo, items, 1, all, nullptr, 0, nullptr, &backdrop, &context});
        fullPainter.end();

        int renderToken = 0;
        snow_canvas_filter_tile_cache::clear();
        QImage tiled(surfaceSize, QImage::Format_ARGB32_Premultiplied);
        tiled.fill(Qt::transparent);
        QPainter tiledPainter(&tiled);
        snow_canvas_renderer::renderSceneItemsTiled(snow_canvas_renderer::SceneRenderRequest{
            &tiledPainter, &displayInfo, items, 1, all, nullptr, 0, nullptr, &backdrop, &context,
            nullptr, nullptr, {}, nullptr, &renderToken, nullptr, false, 0, QPoint(), true});
        tiledPainter.end();

        std::size_t mismatched = 0;
        int maxDelta = 0;
        std::vector<int> columnsWithDiff(surfaceSize.width(), 0);
        for (int y = 0; y < surfaceSize.height(); ++y) {
            const auto* fullLine = reinterpret_cast<const QRgb*>(full.constScanLine(y));
            const auto* tiledLine = reinterpret_cast<const QRgb*>(tiled.constScanLine(y));
            for (int x = 0; x < surfaceSize.width(); ++x) {
                const int delta = std::max({std::abs(qRed(fullLine[x]) - qRed(tiledLine[x])),
                                            std::abs(qGreen(fullLine[x]) - qGreen(tiledLine[x])),
                                            std::abs(qBlue(fullLine[x]) - qBlue(tiledLine[x]))});
                if (delta > 2) {
                    ++mismatched;
                    ++columnsWithDiff[x];
                }
                maxDelta = std::max(maxDelta, delta);
            }
        }
        if (mismatched != 0) {
            std::cerr << label << ": tiled output differs from full output at " << mismatched
                      << " pixels (maximum channel delta " << maxDelta << "); boundary columns:";
            for (int x : {255, 256, 257, 511, 512, 513, 767, 768, 769}) {
                std::cerr << " x" << x << "=" << columnsWithDiff[x];
            }
            std::cerr << '\n';
        }
        require(mismatched == 0,
                "tiled spatial filters must sample the same backdrop pixels as a full render");
    };

    runCase(0, 0.7, "mosaic strength 0.7");
    runCase(0, 0.2, "mosaic strength 0.2");
    runCase(1, 0.7, "blur strength 0.7");
    runCase(1, 0.2, "blur strength 0.2");
    runCase(2, 0.5, "grayscale");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    publicRegionFilterApiRestrictsEffectsToTheRequestedRegion();
    regionFilterSupportPixelsMatchesGaussianPlan();
    croppedRegionFilterMatchesFullFrameRender();
    tiledRenderMatchesFullRender();
    inversionPreservesPremultipliedAlpha();
    grayscalePreservesPremultipliedAlpha();
    colorEffectStrengthHasExactEndpointsAndInterpolation();
    partialFilterRenderUsesABoundedSurface();
    uniformGaussianBlurPreservesColor();
    approximateGaussianMeetsReferenceQualityFloor();
    adaptiveGaussianReductionAndOpaqueMaskStayValid();
    mosaicIntensityDoesNotAffectTransparency();
    mosaicRenderingIsIndependentOfFilterBounds();
    maskedMosaicMatchesReferenceAcrossCoverageAndOrigins();
    mosaicPlanningCropsDirtyOutputAndBatchesEquivalentBlocks();
    movedFiltersMatchAFullRenderAfterDirtyRectangleUpdate();
    filteredBackgroundIsCompositedOnce();
    emptySceneStillRendersBackgroundOnce();
    penFilterUsesRawRoundStrokeMaskForEveryEffect();
    penFilterGeometryCacheRebuildsOnlyTheFinalChunk();
    retainedPenFilterMaskSkipsRasterAndScanOnReuse();
    penMaskRasterizersAndTailInvalidationStayDeterministic();
    sparseAndForcedDensePenFiltersMatch();
    customBackdropIsRenderedOnceBelowFilters();
    exportWithoutRuntimeStillRendersSources();
    plainExportUsesDirectSourceFastPath();
    scalarAvx2AndThreadingProduceIdenticalPixels();
    maskedKernelsMatchAcrossBackendsAndRespectBlurMemoryBound();
    croppedCoverageWorkspaceAndFailurePathsStayValid();
    renderWorkspaceClearReleasesCanvasScratch();
    stableRendererFramesReuseOpaqueGaussianWorkspace();
    mosaicReplayRendersEveryFreeDrawChunk();
    adjacentLayerBatchesEffectsAndKeepsSparseComponents();
    distantSameEffectFiltersUseIndependentSpatialGroups();
    filterSourceCacheKeepsOverlappingZBoundariesSeparate();
    penFilterHoverDrawsAPathContour();
    return 0;
}
