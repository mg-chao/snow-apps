// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "snow_shot/presentation/imagesplitdocument.h"
#include <algorithm>
#include <tuple>
#include <vector>

namespace image_split_test {
// Deliberately simple whole-image oracle: no tiles, strip batching, direct format readers,
// compact-run construction or cached projections. Only for modest test/benchmark fixtures.
inline snow_shot::presentation::ImageSplitLayout
referenceSmart(const QImage& source, const snow_shot::presentation::ImageSplitSettings& settings) {
    using namespace snow_shot::presentation;
    auto result = calculateImageSplitLayout(source.size(), settings);
    if (!result.isValid() || settings.type != ImageSplitType::Smart ||
        settings.direction == ImageSplitDirection::Grid)
        return result;
    const QImage image = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const bool horizontal = settings.direction == ImageSplitDirection::Horizontal;
    const int dimension = horizontal ? image.height() : image.width();
    const int length = horizontal ? image.width() : image.height();
    std::vector<quint64> variation(static_cast<size_t>(dimension));
    for (int axis = 0; axis < dimension; ++axis) {
        QRgb previous = 0;
        for (int tangent = 0; tangent < length; ++tangent) {
            const auto pixel =
                image.pixel(horizontal ? tangent : axis, horizontal ? axis : tangent);
            if (tangent)
                variation[static_cast<size_t>(axis)] +=
                    static_cast<quint64>(std::max({std::abs(qRed(pixel) - qRed(previous)),
                                                   std::abs(qGreen(pixel) - qGreen(previous)),
                                                   std::abs(qBlue(pixel) - qBlue(previous)),
                                                   std::abs(qAlpha(pixel) - qAlpha(previous))}));
            previous = pixel;
        }
    }
    const auto value = [&](int axis) {
        return variation[static_cast<size_t>(std::clamp(axis, 0, dimension - 1))];
    };
    auto& lines = horizontal ? result.horizontalLines : result.verticalLines;
    const auto original = lines;
    for (qsizetype index = 0; index < original.size(); ++index) {
        const int nominal = original.at(index);
        int low = std::max(1, nominal - 21);
        int high = std::min(dimension - 1, nominal + 21);
        if (index)
            low = std::max(low, (original.at(index - 1) + nominal) / 2 + 1);
        if (index + 1 < original.size())
            high = std::min(high, (nominal + original.at(index + 1)) / 2);
        const auto score = [&](int b) {
            return std::tuple{std::min(value(b - 1), value(b)),
                              value(b - 2) + value(b - 1) + value(b) + value(b + 1),
                              std::abs(b - nominal), b};
        };
        int best = nominal;
        for (int candidate = low; candidate <= high; ++candidate)
            if (score(candidate) < score(best))
                best = candidate;
        lines[index] = best;
    }
    return result;
}
} // namespace image_split_test
