#include "snow_canvas_smart_erase.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <QPainter>
#include <QPainterPathStroker>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

namespace snow_canvas_smart_erase {
namespace {
constexpr qsizetype kMaximumWorkingPixels = 16 * 1024 * 1024;

void checkCancelled(const std::atomic_bool& cancelled) {
    if (cancelled.load(std::memory_order_relaxed)) {
        throw std::runtime_error("cancelled");
    }
}

// Exact/repeated screenshot backgrounds benefit from a coherent exemplar before
// pixel-wise PatchMatch. Accept it only when the known boundary agrees closely.
cv::Mat3f coherentFill(const cv::Mat3f& source, const cv::Mat1b& hole, const cv::Mat1b& coverage,
                       const std::atomic_bool& cancelled) {
    const cv::Rect bounds = cv::boundingRect(hole);
    cv::Mat1b known;
    cv::bitwise_and(coverage, ~hole, known);
    cv::Mat1b invalid;
    cv::compare(known, 255, invalid, cv::CMP_NE);
    cv::Mat1i integral;
    cv::integral(invalid / 255, integral, CV_32S);
    cv::Mat1b ring;
    cv::dilate(hole, ring, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(7, 7)));
    cv::bitwise_and(ring, known, ring);
    std::vector<cv::Point> boundary;
    cv::findNonZero(ring, boundary);
    if (boundary.size() < 8)
        return {};
    std::vector<cv::Point> samples;
    const std::size_t stride = std::max<std::size_t>(1, boundary.size() / 512);
    for (std::size_t i = 0; i < boundary.size(); i += stride)
        samples.push_back(boundary[i]);
    constexpr float maximumMeanError = 3.0F / (255.0F * 255.0F);
    float bestError = maximumMeanError;
    cv::Point best;
    bool found = false;
    const int step = std::max(1, std::min(bounds.width, bounds.height) / 16);
    for (int sy = 0; sy + bounds.height <= source.rows; sy += step) {
        checkCancelled(cancelled);
        for (int sx = 0; sx + bounds.width <= source.cols; sx += step) {
            const int right = sx + bounds.width, bottom = sy + bounds.height;
            if (integral(bottom, right) - integral(sy, right) - integral(bottom, sx) +
                    integral(sy, sx) !=
                0)
                continue;
            const cv::Point offset(sx - bounds.x, sy - bounds.y);
            float error = 0;
            bool valid = true;
            for (const auto point : samples) {
                const auto donor = point + offset;
                if (donor.x < 0 || donor.y < 0 || donor.x >= source.cols ||
                    donor.y >= source.rows || !known(donor)) {
                    valid = false;
                    break;
                }
                const auto difference = source(point) - source(donor);
                error += difference.dot(difference);
                if (error > bestError * static_cast<float>(samples.size())) {
                    valid = false;
                    break;
                }
            }
            if (valid) {
                error /= static_cast<float>(samples.size());
                if (!found || error < bestError) {
                    bestError = error;
                    best = offset;
                    found = true;
                }
                if (bestError == 0)
                    break;
            }
        }
        if (found && bestError == 0)
            break;
    }
    if (!found)
        return {};
    cv::Mat3f result = source.clone();
    for (int y = bounds.y; y < bounds.y + bounds.height; ++y) {
        checkCancelled(cancelled);
        for (int x = bounds.x; x < bounds.x + bounds.width; ++x) {
            if (hole(y, x))
                result(y, x) = source(y + best.y, x + best.x);
        }
    }
    return result;
}

// PatchMatch searches only patches made entirely from original, unmasked pixels.
// Synthesized target pixels contribute to matching but can never become donors.
cv::Mat3f fillLevel(const cv::Mat3f& source, const cv::Mat1b& hole, const cv::Mat1b& coverage,
                    const cv::Mat3f& previous, const std::atomic_bool& cancelled) {
    cv::Mat1b known;
    cv::bitwise_and(coverage, ~hole, known);
    cv::Mat1b donors;
    std::vector<cv::Point> candidates;
    int radius = 3;
    for (; radius >= 0; --radius) {
        cv::erode(
            known, donors,
            cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2 * radius + 1, 2 * radius + 1)),
            cv::Point(-1, -1), 1, cv::BORDER_CONSTANT, cv::Scalar(0));
        cv::findNonZero(donors, candidates);
        if (!candidates.empty()) {
            break;
        }
    }
    if (candidates.empty()) {
        throw std::runtime_error("no source patches");
    }
    cv::Mat3f output = source.clone();
    if (!previous.empty()) {
        cv::Mat3f enlarged;
        cv::resize(previous, enlarged, source.size(), 0, 0, cv::INTER_LINEAR);
        enlarged.copyTo(output, hole);
    } else {
        // Nearest known initialization avoids black target pixels biasing the coarse search.
        cv::Mat1f distance;
        cv::Mat1i labels;
        cv::distanceTransform(~known, distance, labels, cv::DIST_L2, 5, cv::DIST_LABEL_PIXEL);
        std::vector<cv::Vec3f> colors(1);
        for (int y = 0; y < source.rows; ++y) {
            checkCancelled(cancelled);
            for (int x = 0; x < source.cols; ++x) {
                if (known(y, x)) {
                    const auto label = static_cast<std::size_t>(labels(y, x));
                    if (colors.size() <= label)
                        colors.resize(label + 1);
                    colors[label] = source(y, x);
                }
            }
        }
        for (int y = 0; y < source.rows; ++y) {
            for (int x = 0; x < source.cols; ++x) {
                if (hole(y, x))
                    output(y, x) = colors.at(static_cast<std::size_t>(labels(y, x)));
            }
        }
    }
    cv::Mat_<cv::Vec2i> matches(source.size(), cv::Vec2i(-1, -1));
    cv::Mat1f costs(source.size(), std::numeric_limits<float>::max());
    std::mt19937 random(0x534e4f57U);
    const auto patchCost = [&](int x, int y, int sx, int sy, float limit) {
        float sum = 0;
        int count = 0;
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const int tx = x + dx, ty = y + dy;
                if (tx < 0 || ty < 0 || tx >= source.cols || ty >= source.rows || !coverage(ty, tx))
                    continue;
                const cv::Vec3f difference = output(ty, tx) - source(sy + dy, sx + dx);
                sum += difference.dot(difference);
                ++count;
            }
        }
        (void)limit;
        return count ? sum / static_cast<float>(count) : std::numeric_limits<float>::max();
    };
    const auto tryCandidate = [&](int x, int y, int sx, int sy) {
        if (sx < 0 || sy < 0 || sx >= source.cols || sy >= source.rows || !donors(sy, sx))
            return;
        const float cost = patchCost(x, y, sx, sy, costs(y, x));
        if (cost < costs(y, x)) {
            costs(y, x) = cost;
            matches(y, x) = cv::Vec2i(sx, sy);
        }
    };
    for (int iteration = 0; iteration < 4; ++iteration) {
        costs.setTo(std::numeric_limits<float>::max());
        const int step = iteration % 2 == 0 ? 1 : -1;
        for (int yi = 0; yi < source.rows; ++yi) {
            checkCancelled(cancelled);
            const int y = step > 0 ? yi : source.rows - 1 - yi;
            for (int xi = 0; xi < source.cols; ++xi) {
                if (xi % 64 == 0)
                    checkCancelled(cancelled);
                const int x = step > 0 ? xi : source.cols - 1 - xi;
                if (!hole(y, x))
                    continue;
                const auto old = matches(y, x);
                tryCandidate(x, y, old[0], old[1]);
                const auto seed = candidates[random() % candidates.size()];
                tryCandidate(x, y, seed.x, seed.y);
                if (x - step >= 0 && x - step < source.cols) {
                    const auto neighbor = matches(y, x - step);
                    if (neighbor[0] >= 0)
                        tryCandidate(x, y, neighbor[0] + step, neighbor[1]);
                }
                if (y - step >= 0 && y - step < source.rows) {
                    const auto neighbor = matches(y - step, x);
                    if (neighbor[0] >= 0)
                        tryCandidate(x, y, neighbor[0], neighbor[1] + step);
                }
                const auto center = matches(y, x);
                for (int window = std::max(source.cols, source.rows); window > 0; window /= 2) {
                    const int span = window * 2 + 1;
                    const int sx = center[0] +
                                   static_cast<int>(random() % static_cast<unsigned>(span)) -
                                   window;
                    const int sy = center[1] +
                                   static_cast<int>(random() % static_cast<unsigned>(span)) -
                                   window;
                    tryCandidate(x, y, sx, sy);
                }
            }
        }
        // Overlapping patch votes reduce seams; leave every known pixel exactly unchanged.
        cv::Mat3f next = output.clone();
        for (int y = 0; y < source.rows; ++y) {
            checkCancelled(cancelled);
            for (int x = 0; x < source.cols; ++x) {
                if (!hole(y, x))
                    continue;
                cv::Vec3f sum(0, 0, 0);
                float weight = 0;
                for (int dy = -radius; dy <= radius; ++dy) {
                    for (int dx = -radius; dx <= radius; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= source.cols || ny >= source.rows ||
                            !hole(ny, nx))
                            continue;
                        const auto donor = matches(ny, nx);
                        if (donor[0] < 0)
                            continue;
                        const float w = 1.0F / (1.0F + costs(ny, nx));
                        sum += source(donor[1] - dy, donor[0] - dx) * w;
                        weight += w;
                    }
                }
                if (weight > 0)
                    next(y, x) = sum / weight;
            }
        }
        output = std::move(next);
    }
    return output;
}
} // namespace

QPainterPath path(const SnowCanvasSceneItem& item) {
    QPainterPath result;
    if (item.is_free_draw != 0) {
        if (!item.arrow_points || item.arrow_point_count == 0)
            return result;
        result.moveTo(item.arrow_points[0].x, item.arrow_points[0].y);
        for (std::uint32_t i = 1; i < item.arrow_point_count; ++i)
            result.lineTo(item.arrow_points[i].x, item.arrow_points[i].y);
        QPainterPathStroker stroker;
        stroker.setWidth(item.stroke_width);
        stroker.setCapStyle(Qt::RoundCap);
        stroker.setJoinStyle(Qt::RoundJoin);
        QPainterPath stroke = stroker.createStroke(result);
        if (stroke.isEmpty())
            stroke.addEllipse(QPointF(item.arrow_points[0].x, item.arrow_points[0].y),
                              item.stroke_width / 2, item.stroke_width / 2);
        return stroke;
    }
    result.addRect(-item.width / 2, -item.height / 2, item.width, item.height);
    QTransform transform;
    transform.translate(item.center_x, item.center_y);
    transform.rotateRadians(item.rotation);
    return transform.map(result);
}

Result reconstruct(const SnowCanvasSceneItem& item, const QList<SnowCanvasBaseImageSource>& sources,
                   const std::atomic_bool& cancelled) {
    try {
        const QPainterPath shape = path(item);
        QRectF available;
        qreal scale = 1;
        for (const auto& source : sources) {
            const QRectF coverage = source.coverage.isEmpty()
                                        ? source.canvasRect
                                        : source.coverage.intersected(source.canvasRect);
            available = available.isNull() ? coverage : available.united(coverage);
            if (source.canvasRect.intersects(shape.boundingRect())) {
                scale = std::max({scale, source.image.width() / source.canvasRect.width(),
                                  source.image.height() / source.canvasRect.height()});
            }
        }
        const QRectF target = shape.boundingRect().intersected(available);
        if (target.isEmpty())
            return {{}, {}, {}, true};
        // Bound donor context for long sparse strokes without reducing source resolution.
        const qreal padding =
            std::min(512.0 / scale, std::max({64.0 / scale, target.width(), target.height()}));
        const QRectF roi =
            target.adjusted(-padding, -padding, padding, padding).intersected(available);
        const double pixelWidth = std::ceil(roi.width() * scale);
        const double pixelHeight = std::ceil(roi.height() * scale);
        if (!std::isfinite(pixelWidth) || !std::isfinite(pixelHeight) || pixelWidth <= 0 ||
            pixelHeight <= 0 || pixelWidth > kMaximumWorkingPixels ||
            pixelHeight > kMaximumWorkingPixels || pixelWidth * pixelHeight > kMaximumWorkingPixels)
            return {};
        const QSize size(static_cast<int>(pixelWidth), static_cast<int>(pixelHeight));
        if (size.width() <= 0 || size.height() <= 0 ||
            static_cast<qint64>(size.width()) * size.height() > kMaximumWorkingPixels)
            return {};
        checkCancelled(cancelled);
        QImage original(size, QImage::Format_ARGB32);
        if (original.isNull())
            return {};
        original.fill(Qt::transparent);
        QTransform mapping;
        mapping.scale(size.width() / roi.width(), size.height() / roi.height());
        mapping.translate(-roi.x(), -roi.y());
        {
            QPainter painter(&original);
            painter.setWorldTransform(mapping);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            for (const auto& source : sources) {
                checkCancelled(cancelled);
                painter.save();
                if (!source.coverage.isEmpty())
                    painter.setClipRect(source.coverage);
                painter.drawImage(source.canvasRect, source.image);
                painter.restore();
            }
        }
        QImage mask(size, QImage::Format_Grayscale8);
        if (mask.isNull())
            return {};
        mask.fill(0);
        {
            QPainter painter(&mask);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setWorldTransform(mapping);
            painter.fillPath(shape, Qt::white);
        }
        cv::Mat1b hole(size.height(), size.width()), coverage(size.height(), size.width());
        cv::Mat3f rgb(size.height(), size.width());
        for (int y = 0; y < size.height(); ++y) {
            checkCancelled(cancelled);
            const auto* row = reinterpret_cast<const QRgb*>(original.constScanLine(y));
            const auto* maskRow = mask.constScanLine(y);
            for (int x = 0; x < size.width(); ++x) {
                coverage(y, x) = qAlpha(row[x]) > 0 ? 255 : 0;
                hole(y, x) = maskRow[x] > 0 && coverage(y, x) ? 255 : 0;
                rgb(y, x) = cv::Vec3f(qRed(row[x]) / 255.0F, qGreen(row[x]) / 255.0F,
                                      qBlue(row[x]) / 255.0F);
            }
        }
        if (cv::countNonZero(hole) == 0)
            return {{}, {}, {}, true};
        cv::Mat3f coherent = coherentFill(rgb, hole, coverage, cancelled);
        if (!coherent.empty()) {
            rgb = std::move(coherent);
        } else {
            cv::Mat3f lab;
            cv::cvtColor(rgb, lab, cv::COLOR_RGB2Lab);
            rgb.release();
            std::vector<cv::Mat3f> images{lab};
            std::vector<cv::Mat1b> holes{hole}, coverages{coverage};
            while (std::max(images.back().cols, images.back().rows) > 64 &&
                   std::min(images.back().cols, images.back().rows) > 8) {
                checkCancelled(cancelled);
                cv::Mat3f reduced;
                cv::pyrDown(images.back(), reduced);
                cv::Mat1b reducedHole, reducedCoverage;
                cv::Mat1b expandedHole;
                cv::dilate(holes.back(), expandedHole,
                           cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 5)));
                cv::resize(expandedHole, reducedHole, reduced.size(), 0, 0, cv::INTER_AREA);
                cv::threshold(reducedHole, reducedHole, 0, 255, cv::THRESH_BINARY);
                cv::resize(coverages.back(), reducedCoverage, reduced.size(), 0, 0, cv::INTER_AREA);
                cv::threshold(reducedCoverage, reducedCoverage, 254, 255, cv::THRESH_BINARY);
                if (cv::countNonZero(reducedCoverage & ~reducedHole) == 0)
                    break;
                images.push_back(reduced);
                holes.push_back(reducedHole);
                coverages.push_back(reducedCoverage);
            }
            cv::Mat3f filled;
            for (std::size_t i = images.size(); i-- > 0;) {
                filled = fillLevel(images[i], holes[i], coverages[i], filled, cancelled);
            }
            cv::cvtColor(filled, rgb, cv::COLOR_Lab2RGB);
        }
        QImage output = original.copy();
        if (output.isNull())
            return {};
        for (int y = 0; y < size.height(); ++y) {
            checkCancelled(cancelled);
            auto* row = reinterpret_cast<QRgb*>(output.scanLine(y));
            for (int x = 0; x < size.width(); ++x) {
                if (!hole(y, x))
                    continue;
                const auto color = rgb(y, x);
                row[x] = qRgba(std::clamp(qRound(color[0] * 255), 0, 255),
                               std::clamp(qRound(color[1] * 255), 0, 255),
                               std::clamp(qRound(color[2] * 255), 0, 255), qAlpha(row[x]));
            }
        }
        // Cache only the affected bounds; surrounding donor pixels are job-local.
        const QRect crop = mapping.mapRect(target).toAlignedRect().intersected(original.rect());
        const QRectF canvasCrop = mapping.inverted().mapRect(QRectF(crop));
        return {original.copy(crop), output.copy(crop), canvasCrop, true};
    } catch (const std::exception&) {
        return {};
    }
}
} // namespace snow_canvas_smart_erase
