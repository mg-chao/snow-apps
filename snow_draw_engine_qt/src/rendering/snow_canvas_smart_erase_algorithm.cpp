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
#include <chrono>
#include <type_traits>

namespace snow_canvas_smart_erase {
namespace {
class StageTimer {
  public:
    explicit StageTimer(double* elapsed) : elapsed_(elapsed) {
        if (elapsed_)
            start_ = std::chrono::steady_clock::now();
    }
    ~StageTimer() {
        if (elapsed_)
            *elapsed_ +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_)
                    .count();
    }

  private:
    double* elapsed_;
    std::chrono::steady_clock::time_point start_{};
};
constexpr qsizetype kMaximumWorkingPixels = 16 * 1024 * 1024;

void checkCancelled(const std::atomic_bool& cancelled) {
    if (cancelled.load(std::memory_order_relaxed)) {
        throw std::runtime_error("cancelled");
    }
}

// A surface is accepted using surrounding observations, never pixels inside the hole.
// Robust fitting tolerates a few foreground/antialiasing pixels on the outer boundary.
cv::Mat3f surfaceFill(const cv::Mat3f& source, const cv::Mat1b& hole, const cv::Mat1b& known,
                      const std::atomic_bool& cancelled) {
    const auto bounds = cv::boundingRect(hole);
    cv::Mat1b ring;
    cv::dilate(hole, ring, cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(9, 9)));
    cv::bitwise_and(ring, known, ring);
    std::vector<cv::Point> boundary;
    cv::findNonZero(ring, boundary);
    if (boundary.size() < 12)
        return {};
    const auto basis = [&](cv::Point p) {
        return cv::Vec3d(1, static_cast<double>(p.x - bounds.x) / bounds.width,
                         static_cast<double>(p.y - bounds.y) / bounds.height);
    };
    const std::size_t stride = std::max<std::size_t>(1, boundary.size() / 4096);
    cv::Matx33d coefficients = cv::Matx33d::zeros();
    for (int iteration = 0; iteration < 5; ++iteration) {
        checkCancelled(cancelled);
        cv::Matx33d normal = cv::Matx33d::zeros(), rhs = cv::Matx33d::zeros();
        for (std::size_t i = 0; i < boundary.size(); i += stride) {
            const auto p = boundary[i];
            const auto b = basis(p);
            const cv::Vec3d value(source(p));
            const double residual = cv::norm(coefficients.t() * b - value);
            const double weight =
                iteration == 0 ? 1 : std::min(1.0, 0.008 / std::max(1e-8, residual));
            normal += weight * b * b.t();
            rhs += weight * b * value.t();
        }
        if (!cv::solve(normal, rhs, coefficients, cv::DECOMP_CHOLESKY))
            return {};
    }
    std::size_t inliers = 0, count = 0;
    double squaredError = 0;
    for (std::size_t i = 0; i < boundary.size(); i += stride) {
        const auto p = boundary[i];
        const double residual = cv::norm(coefficients.t() * basis(p) - cv::Vec3d(source(p)));
        ++count;
        if (residual < 4.0 / 255) {
            ++inliers;
            squaredError += residual * residual;
        }
    }
    if (inliers * 100 < count * 97 ||
        squaredError > static_cast<double>(inliers) * 6.0 / (255 * 255))
        return {};
    cv::Mat3f result = source.clone();
    for (int y = bounds.y; y < bounds.y + bounds.height; ++y) {
        checkCancelled(cancelled);
        for (int x = bounds.x; x < bounds.x + bounds.width; ++x) {
            if (hole(y, x))
                result(y, x) = cv::Vec3f(coefficients.t() * basis({x, y}));
        }
    }
    return result;
}

// Buffer coverage and donor eligibility are separate. Start locally and expand only
// if no usable patches remain (e.g. a stroke beside a source edge).
cv::Mat1b donorDomain(const cv::Mat1b& hole, const cv::Mat1b& known,
                      const std::atomic_bool& cancelled) {
    cv::Mat1f distance;
    cv::distanceTransform(~hole, distance, cv::DIST_L2, 5);
    const auto bounds = cv::boundingRect(hole);
    int reach = std::clamp(std::min(bounds.width, bounds.height) / 2, 16, 96);
    cv::Mat1b domain;
    for (;;) {
        checkCancelled(cancelled);
        cv::compare(distance, reach, domain, cv::CMP_LE);
        cv::bitwise_and(domain, known, domain);
        cv::Mat1b patches;
        cv::erode(domain, patches, cv::getStructuringElement(cv::MORPH_RECT, {7, 7}), {-1, -1}, 1,
                  cv::BORDER_CONSTANT, cv::Scalar(0));
        if (cv::countNonZero(patches) >= 64 || reach >= 512)
            return domain;
        reach = std::min(512, reach * 2);
    }
}

// Exact periodic backgrounds have a fast path, but matching a border is not enough.
// Verify a short translation throughout the known donor domain before using it.
cv::Mat3f periodicFill(const cv::Mat3f& source, const cv::Mat1b& hole, const cv::Mat1b& domain,
                       const std::atomic_bool& cancelled) {
    std::vector<cv::Point> points;
    cv::findNonZero(domain, points);
    for (const auto direction : {cv::Point(1, 0), cv::Point(0, 1)}) {
        for (int period = 2; period <= 64; ++period) {
            checkCancelled(cancelled);
            const auto offset = direction * period;
            int count = 0;
            double error = 0;
            for (std::size_t i = 0; i < points.size(); ++i) {
                if (i % 4096 == 0)
                    checkCancelled(cancelled);
                const auto p = points[i], q = p + offset;
                if (!cv::Rect({}, source.size()).contains(q) || !domain(q))
                    continue;
                const auto delta = source(p) - source(q);
                error += static_cast<double>(delta.dot(delta));
                ++count;
                // Early rejection against the maximum possible final sample count.
                if (error > static_cast<double>(points.size()) * 0.5 / (255 * 255))
                    break;
            }
            if (count < 64 || error > count * 0.5 / (255 * 255))
                continue;
            cv::Mat3f output = source.clone();
            bool complete = true;
            for (int y = 0; y < source.rows && complete; ++y) {
                checkCancelled(cancelled);
                for (int x = 0; x < source.cols; ++x) {
                    if (!hole(y, x))
                        continue;
                    bool found = false;
                    for (int d = 1; d * period < std::max(source.rows, source.cols) && !found;
                         ++d) {
                        for (const int sign : {-1, 1}) {
                            const cv::Point donor = cv::Point(x, y) + offset * (d * sign);
                            if (cv::Rect({}, source.size()).contains(donor) && domain(donor)) {
                                output(y, x) = source(donor);
                                found = true;
                                break;
                            }
                        }
                    }
                    if (!found) {
                        complete = false;
                        break;
                    }
                }
            }
            if (complete)
                return output;
        }
    }
    return {};
}

cv::Mat3f observedBackground(const cv::Mat3f& source, const cv::Mat1b& known, int radius,
                             const std::atomic_bool& cancelled, cv::Mat1f& weights) {
    known.convertTo(weights, CV_32F, 1.0 / 255);
    cv::Mat3f weighted(source.size(), cv::Vec3f(0, 0, 0));
    source.copyTo(weighted, known);
    const cv::Size kernel(2 * radius + 1, 2 * radius + 1);
    cv::GaussianBlur(weighted, weighted, kernel, radius / 2.0);
    cv::GaussianBlur(weights, weights, kernel, radius / 2.0);
    for (int y = 0; y < source.rows; ++y) {
        checkCancelled(cancelled);
        for (int x = 0; x < source.cols; ++x)
            weighted(y, x) /= std::max(weights(y, x), 1e-6F);
    }
    return weighted;
}

cv::Mat1f backgroundVariation(const cv::Mat3f& source, const cv::Mat3f& mean,
                              const cv::Mat1b& known, int radius, const std::atomic_bool& cancelled,
                              const cv::Mat1f& weights) {
    cv::Mat1f squared(source.size(), 0.0F);
    for (int y = 0; y < source.rows; ++y) {
        checkCancelled(cancelled);
        for (int x = 0; x < source.cols; ++x)
            if (known(y, x))
                squared(y, x) = source(y, x).dot(source(y, x));
    }
    const cv::Size kernel(2 * radius + 1, 2 * radius + 1);
    cv::GaussianBlur(squared, squared, kernel, radius / 2.0);
    for (int y = 0; y < source.rows; ++y) {
        checkCancelled(cancelled);
        for (int x = 0; x < source.cols; ++x)
            squared(y, x) = std::sqrt(std::max(
                0.0F, squared(y, x) / std::max(weights(y, x), 1e-6F) - mean(y, x).dot(mean(y, x))));
    }
    return squared;
}

struct BackgroundGuide {
    cv::Mat3f color;
    cv::Mat1f variation;
};

// Continue compatible observations across each hole along four directions. Choosing
// agreeing endpoints preserves bands/edges without assuming a horizontal layout.
BackgroundGuide backgroundGuide(const cv::Mat3f& observed, const cv::Mat1f& variation,
                                const cv::Mat1b& hole, const cv::Mat1b& known,
                                const std::atomic_bool& cancelled) {
    cv::Mat3f guide = observed.clone();
    cv::Mat1f texture = variation.clone();
    cv::Mat1f costs(observed.size(), std::numeric_limits<float>::max());
    const cv::Rect image({}, observed.size());
    for (const auto step : {cv::Point(1, 0), cv::Point(0, 1), cv::Point(1, 1), cv::Point(-1, 1)}) {
        for (int y = 0; y < observed.rows; ++y) {
            checkCancelled(cancelled);
            for (int x = 0; x < observed.cols; ++x) {
                if (image.contains(cv::Point(x, y) - step))
                    continue;
                cv::Point before(-1, -1);
                std::vector<cv::Point> pending;
                const auto flush = [&](cv::Point after) {
                    if (pending.empty())
                        return;
                    if (before.x < 0 && after.x < 0)
                        return;
                    const auto a = before.x >= 0 ? before : after;
                    const auto b = after.x >= 0 ? after : before;
                    const auto delta = observed(a) - observed(b);
                    const float difference = delta.dot(delta);
                    const float span = static_cast<float>(cv::norm(a - b));
                    const float score =
                        difference + 0.02F * span + (before.x < 0 || after.x < 0 ? 1000.0F : 0.0F);
                    for (const auto p : pending) {
                        if (score >= costs(p))
                            continue;
                        const float t = span > 0 ? static_cast<float>(cv::norm(p - a)) / span : 0;
                        guide(p) = difference < 900 ? observed(a) * (1 - t) + observed(b) * t
                                                    : observed(t < 0.5F ? a : b);
                        texture(p) = variation(a) * (1 - t) + variation(b) * t;
                        costs(p) = score;
                    }
                };
                for (cv::Point p(x, y); image.contains(p); p += step) {
                    if (known(p)) {
                        flush(p);
                        pending.clear();
                        before = p;
                    } else if (hole(p)) {
                        pending.push_back(p);
                    } else {
                        flush({-1, -1});
                        pending.clear();
                        before = {-1, -1};
                    }
                }
                flush({-1, -1});
            }
        }
    }
    return {guide, texture};
}

struct LevelResult {
    cv::Mat3f image;
    cv::Mat_<cv::Vec2i> matches;
};

struct MaskSpan {
    int y;
    int begin;
    int end;
    std::size_t offset;
};
struct PatchCoverage {
    unsigned char known = 0;
    unsigned char missing = 0;
    bool interior = false;
};
struct TargetPatch {
    int x;
    int y;
    cv::Vec3f color;
    float variation;
    float structureWeight;
    float normalization;
    bool interior;
};

// Original observations remain immutable. Only masked row spans participate in
// propagation and voting; their order matches a dense forward/reverse scan.
LevelResult fillLevel(const cv::Mat3f& source, const cv::Mat1b& hole, const cv::Mat1b& coverage,
                      const cv::Mat1b& domain, const BackgroundGuide& background,
                      const LevelResult& previous, const std::atomic_bool& cancelled,
                      const ReconstructionOptions& options, int passes,
                      LevelDiagnostics* diagnostics) {
    const auto& guide = background.color;
    const auto& variation = background.variation;
    cv::Mat1b donors;
    std::vector<cv::Point> candidates;
    int radius = 3;
    for (; radius >= 0; --radius) {
        cv::erode(domain, donors,
                  cv::getStructuringElement(cv::MORPH_RECT, {2 * radius + 1, 2 * radius + 1}),
                  {-1, -1}, 1, cv::BORDER_CONSTANT, cv::Scalar(0));
        cv::findNonZero(donors, candidates);
        if (!candidates.empty())
            break;
    }
    if (candidates.empty())
        throw std::runtime_error("no source patches");
    std::vector<MaskSpan> spans;
    std::vector<PatchCoverage> patches;
    for (int y = 0; y < hole.rows; ++y) {
        checkCancelled(cancelled);
        const auto* row = hole.ptr<unsigned char>(y);
        for (int x = 0; x < hole.cols;) {
            if (!row[x]) {
                ++x;
                continue;
            }
            const int begin = x;
            const auto offset = patches.size();
            for (; x < hole.cols && row[x]; ++x) {
                PatchCoverage patch;
                for (int dy = -radius; dy <= radius; ++dy) {
                    const int ty = y + dy;
                    if (ty < 0 || ty >= hole.rows)
                        continue;
                    const auto* covered = coverage.ptr<unsigned char>(ty);
                    const auto* masked = hole.ptr<unsigned char>(ty);
                    for (int dx = -radius; dx <= radius; ++dx) {
                        const int tx = x + dx;
                        if (tx < 0 || tx >= hole.cols || !covered[tx])
                            continue;
                        if (masked[tx])
                            ++patch.missing;
                        else
                            ++patch.known;
                    }
                }
                patch.interior = patch.known + patch.missing == (2 * radius + 1) * (2 * radius + 1);
                patches.push_back(patch);
            }
            spans.push_back({y, begin, x, offset});
        }
    }
    cv::Mat3f output = source.clone();
    if (!previous.image.empty()) {
        cv::Mat3f enlarged;
        cv::resize(previous.image, enlarged, source.size(), 0, 0, cv::INTER_LINEAR);
        enlarged.copyTo(output, hole);
    }
    cv::Mat1f distance;
    cv::Mat1i labels;
    cv::distanceTransform(~donors, distance, labels, cv::DIST_L2, 5, cv::DIST_LABEL_PIXEL);
    std::vector<cv::Point> nearest(candidates.size() + 1);
    for (const auto p : candidates)
        nearest.at(static_cast<std::size_t>(labels(p))) = p;
    cv::Mat_<cv::Vec2i> matches(source.size(), cv::Vec2i(-1, -1));
    cv::Mat1f costs(source.size(), std::numeric_limits<float>::max());
    // Counts are converted in place to penalties after each complete usage pass.
    cv::Mat1f reusePenalty(source.size(), 0.0F);
    const float expectedUsage =
        std::max(1.0F, static_cast<float>(patches.size()) / static_cast<float>(candidates.size()));
    std::mt19937 random(0x534e4f57U);
    if (previous.image.empty()) {
        for (const auto& span : spans) {
            checkCancelled(cancelled);
            const int y = span.y;
            for (int x = span.begin; x < span.end; ++x) {
                auto best = nearest.at(static_cast<std::size_t>(labels(y, x)));
                auto delta = guide(y, x) - guide(best);
                float textureDelta = variation(y, x) - variation(best);
                float error = delta.dot(delta) + 4 * textureDelta * textureDelta;
                for (int sample = 0; sample < 32; ++sample) {
                    const auto p = candidates[random() % candidates.size()];
                    delta = guide(y, x) - guide(p);
                    textureDelta = variation(y, x) - variation(p);
                    const float cost = delta.dot(delta) + 4 * textureDelta * textureDelta;
                    if (cost < error) {
                        best = p;
                        error = cost;
                    }
                }
                output(y, x) = source(best);
                matches(y, x) = {best.x, best.y};
            }
        }
    }
    float synthesizedWeight = 0.05F;
    const float scale = static_cast<float>(std::max(source.cols, source.rows));
    const auto targetPatch = [&](const MaskSpan& span, int x) {
        const auto& patch = patches[span.offset + static_cast<std::size_t>(x - span.begin)];
        const float texture = variation(span.y, x);
        return TargetPatch{x,
                           span.y,
                           guide(span.y, x),
                           texture,
                           8.0F / (4.0F + texture),
                           std::max(1.0F, patch.known + patch.missing * synthesizedWeight),
                           patch.interior};
    };
    const auto tryCandidate = [&](const TargetPatch& target, int sx, int sy) {
        if (sx < 0 || sy < 0 || sx >= source.cols || sy >= source.rows || !donors(sy, sx))
            return;
        const auto structure = target.color - guide(sy, sx);
        const float textureDelta = target.variation - variation(sy, sx);
        const float dx = static_cast<float>(sx - target.x), dy = static_cast<float>(sy - target.y);
        const float fixed = target.structureWeight * structure.dot(structure) +
                            4 * textureDelta * textureDelta + reusePenalty(sy, sx) +
                            8 * (dx * dx + dy * dy) / (scale * scale);
        const float best = costs(target.y, target.x);
        // Every omitted squared difference is nonnegative. Use the complete
        // denominator, with slack so rounding cannot prune a competitive patch.
        const float limit = options.earlyRejection ? best + 1e-5F * std::max(1.0F, best)
                                                   : std::numeric_limits<float>::infinity();
        if (fixed > limit)
            return;
        float sum = 0;
        const auto accumulate = [&](auto interior) {
            for (int py = -radius; py <= radius; ++py) {
                const int ty = target.y + py;
                if constexpr (!decltype(interior)::value) {
                    if (ty < 0 || ty >= source.rows)
                        continue;
                }
                const auto* targetRow = output.ptr<cv::Vec3f>(ty);
                const auto* sourceRow = source.ptr<cv::Vec3f>(sy + py);
                const auto* holeRow = hole.ptr<unsigned char>(ty);
                const auto* coverageRow = coverage.ptr<unsigned char>(ty);
                for (int px = -radius; px <= radius; ++px) {
                    const int tx = target.x + px;
                    if constexpr (!decltype(interior)::value) {
                        if (tx < 0 || tx >= source.cols || !coverageRow[tx])
                            continue;
                    }
                    const auto delta = targetRow[tx] - sourceRow[sx + px];
                    sum += delta.dot(delta) * (holeRow[tx] ? synthesizedWeight : 1.0F);
                }
                if (sum / target.normalization + fixed > limit)
                    return false;
            }
            return true;
        };
        if (!(target.interior ? accumulate(std::true_type{}) : accumulate(std::false_type{})))
            return;
        const float cost = sum / target.normalization + fixed;
        if (cost < best) {
            costs(target.y, target.x) = cost;
            matches(target.y, target.x) = {sx, sy};
        }
    };
    if (!previous.matches.empty()) {
        for (const auto& span : spans) {
            checkCancelled(cancelled);
            const int y = span.y;
            for (int x = span.begin; x < span.end; ++x) {
                const int px =
                    static_cast<int>(static_cast<qint64>(x) * previous.matches.cols / source.cols);
                const int py =
                    static_cast<int>(static_cast<qint64>(y) * previous.matches.rows / source.rows);
                const auto p = previous.matches(py, px);
                if (p[0] >= 0) {
                    const int sx = static_cast<int>(static_cast<qint64>(p[0]) * source.cols /
                                                    previous.matches.cols);
                    const int sy = static_cast<int>(static_cast<qint64>(p[1]) * source.rows /
                                                    previous.matches.rows);
                    tryCandidate(targetPatch(span, x), sx, sy);
                }
            }
        }
    }
    cv::Mat3f next = source.clone();
    for (int iteration = 0; iteration < passes; ++iteration) {
        {
            StageTimer searchTimer(diagnostics ? &diagnostics->searchMs : nullptr);
            synthesizedWeight =
                0.15F + 0.20F * static_cast<float>(iteration) / static_cast<float>(passes - 1);
            for (const auto p : candidates)
                reusePenalty(p) = 0;
            for (const auto& span : spans) {
                checkCancelled(cancelled);
                for (int x = span.begin; x < span.end; ++x) {
                    const auto p = matches(span.y, x);
                    if (p[0] >= 0)
                        reusePenalty(p[1], p[0]) += 1;
                }
            }
            for (const auto p : candidates)
                reusePenalty(p) = 25 * std::log1p(reusePenalty(p) / expectedUsage);
            const int step = iteration % 2 == 0 ? 1 : -1;
            for (std::size_t si = 0; si < spans.size(); ++si) {
                checkCancelled(cancelled);
                const auto& span = spans[step > 0 ? si : spans.size() - 1 - si];
                const int y = span.y;
                for (int xi = 0; xi < span.end - span.begin; ++xi) {
                    if (xi % 64 == 0)
                        checkCancelled(cancelled);
                    const int x = step > 0 ? span.begin + xi : span.end - 1 - xi;
                    costs(y, x) = std::numeric_limits<float>::max();
                    const auto target = targetPatch(span, x);
                    const auto old = matches(y, x);
                    tryCandidate(target, old[0], old[1]);
                    const auto local = nearest.at(static_cast<std::size_t>(labels(y, x)));
                    tryCandidate(target, local.x, local.y);
                    const auto seed = candidates[random() % candidates.size()];
                    tryCandidate(target, seed.x, seed.y);
                    if (x - step >= 0 && x - step < source.cols) {
                        const auto neighbor = matches(y, x - step);
                        if (neighbor[0] >= 0)
                            tryCandidate(target, neighbor[0] + step, neighbor[1]);
                    }
                    if (y - step >= 0 && y - step < source.rows) {
                        const auto neighbor = matches(y - step, x);
                        if (neighbor[0] >= 0)
                            tryCandidate(target, neighbor[0], neighbor[1] + step);
                    }
                    for (int window = std::max(source.cols, source.rows); window > 0; window /= 2) {
                        const auto center = matches(y, x);
                        const int width = window * 2 + 1;
                        const int sx = center[0] +
                                       static_cast<int>(random() % static_cast<unsigned>(width)) -
                                       window;
                        const int sy = center[1] +
                                       static_cast<int>(random() % static_cast<unsigned>(width)) -
                                       window;
                        tryCandidate(target, sx, sy);
                    }
                }
            }
        }
        StageTimer voteTimer(diagnostics ? &diagnostics->votingMs : nullptr);
        const auto vote = [&](const cv::Range& range) {
            for (int si = range.start; si < range.end; ++si) {
                checkCancelled(cancelled);
                const auto& span = spans[static_cast<std::size_t>(si)];
                const int y = span.y;
                for (int x = span.begin; x < span.end; ++x) {
                    cv::Vec3f sum(0, 0, 0);
                    float weight = 0;
                    const auto center = matches(y, x);
                    const auto anchor = source(center[1], center[0]);
                    const float tolerance = std::clamp(variation(y, x) * 0.25F, 1.5F, 10.0F);
                    for (int dy = -radius; dy <= radius; ++dy) {
                        for (int dx = -radius; dx <= radius; ++dx) {
                            const int nx = x + dx, ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= source.cols || ny >= source.rows ||
                                !hole(ny, nx))
                                continue;
                            const auto donor = matches(ny, nx);
                            if (donor[0] < 0)
                                continue;
                            const auto value = source(donor[1] - dy, donor[0] - dx);
                            const auto difference =
                                guide(donor[1] - dy, donor[0] - dx) - guide(y, x);
                            const auto detailDifference = value - anchor;
                            const float w = 1.0F / ((1.0F + costs(ny, nx)) *
                                                    (1.0F + difference.dot(difference) / 100.0F) *
                                                    (1.0F + detailDifference.dot(detailDifference) /
                                                                (tolerance * tolerance)));
                            sum += value * w;
                            weight += w;
                        }
                    }
                    // Both buffers retain the original known pixels permanently.
                    next(y, x) = weight > 0 ? sum / weight : output(y, x);
                }
            }
        };
        const cv::Range range(0, static_cast<int>(spans.size()));
        if (options.parallelVoting && patches.size() >= 32768)
            cv::parallel_for_(range, vote, 2);
        else
            vote(range);
        std::swap(output, next);
    }
    return {output, matches};
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

Result reconstructWithOptions(const SnowCanvasSceneItem& item,
                              const QList<SnowCanvasBaseImageSource>& sources,
                              const std::atomic_bool& cancelled,
                              const ReconstructionOptions& options,
                              ReconstructionDiagnostics* diagnostics) {
    if (options.coarsePasses < 2 || options.coarsePasses > 5 || options.intermediatePasses < 2 ||
        options.intermediatePasses > 5 || options.finePasses < 2 || options.finePasses > 5)
        return {};
    if (diagnostics)
        *diagnostics = {};
    auto preparationStart =
        diagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
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
                rgb(y, x) = cv::Vec3f(static_cast<float>(qRed(row[x])) / 255.0F,
                                      static_cast<float>(qGreen(row[x])) / 255.0F,
                                      static_cast<float>(qBlue(row[x])) / 255.0F);
            }
        }
        if (cv::countNonZero(hole) == 0)
            return {{}, {}, {}, true};
        cv::Mat1b known;
        cv::bitwise_and(coverage, ~hole, known);
        if (cv::countNonZero(known) == 0)
            return {};
        if (diagnostics) {
            diagnostics->workingSize = size;
            diagnostics->maskedPixels = cv::countNonZero(hole);
            diagnostics->preparationMs = std::chrono::duration<double, std::milli>(
                                             std::chrono::steady_clock::now() - preparationStart)
                                             .count();
        }
        auto fastStart = diagnostics ? std::chrono::steady_clock::now()
                                     : std::chrono::steady_clock::time_point{};
        cv::Mat3f surface = surfaceFill(rgb, hole, known, cancelled);
        if (diagnostics && !surface.empty())
            diagnostics->path = ReconstructionDiagnostics::Path::Surface;
        const auto domain = surface.empty() ? donorDomain(hole, known, cancelled) : cv::Mat1b();
        if (surface.empty()) {
            surface = periodicFill(rgb, hole, domain, cancelled);
            if (diagnostics && !surface.empty())
                diagnostics->path = ReconstructionDiagnostics::Path::Periodic;
        }
        if (diagnostics)
            diagnostics->fastPathsMs = std::chrono::duration<double, std::milli>(
                                           std::chrono::steady_clock::now() - fastStart)
                                           .count();
        if (!surface.empty()) {
            rgb = std::move(surface);
        } else {
            auto guideStart = diagnostics ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
            if (diagnostics)
                diagnostics->path = ReconstructionDiagnostics::Path::Patches;
            cv::Mat3f lab;
            cv::cvtColor(rgb, lab, cv::COLOR_RGB2Lab);
            rgb.release();
            const auto bounds = cv::boundingRect(hole);
            const int guideRadius = std::clamp(std::min(bounds.width, bounds.height) / 12, 4, 16);
            cv::Mat1f weights;
            const auto mean = observedBackground(lab, known, guideRadius, cancelled, weights);
            const auto variation =
                backgroundVariation(lab, mean, known, guideRadius, cancelled, weights);
            weights.release();
            const auto guide = backgroundGuide(mean, variation, hole, known, cancelled);
            std::vector<cv::Mat3f> images{lab};
            std::vector<BackgroundGuide> guides{guide};
            std::vector<cv::Mat1b> holes{hole}, coverages{coverage};
            std::vector<cv::Mat1b> domains{domain};
            while (std::max(images.back().cols, images.back().rows) > 64 &&
                   std::min(images.back().cols, images.back().rows) > 8) {
                checkCancelled(cancelled);
                cv::Mat3f reduced;
                cv::pyrDown(images.back(), reduced);
                cv::Mat1b reducedHole, reducedCoverage, reducedDomain;
                cv::Mat1b expandedHole;
                cv::dilate(holes.back(), expandedHole,
                           cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 5)));
                cv::resize(expandedHole, reducedHole, reduced.size(), 0, 0, cv::INTER_AREA);
                cv::threshold(reducedHole, reducedHole, 0, 255, cv::THRESH_BINARY);
                cv::Mat1b interiorCoverage, interiorDomain;
                const auto kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 5));
                cv::erode(coverages.back(), interiorCoverage, kernel, {-1, -1}, 1,
                          cv::BORDER_CONSTANT, cv::Scalar(0));
                cv::resize(interiorCoverage, reducedCoverage, reduced.size(), 0, 0, cv::INTER_AREA);
                cv::threshold(reducedCoverage, reducedCoverage, 254, 255, cv::THRESH_BINARY);
                cv::erode(domains.back(), interiorDomain, kernel, {-1, -1}, 1, cv::BORDER_CONSTANT,
                          cv::Scalar(0));
                cv::resize(interiorDomain, reducedDomain, reduced.size(), 0, 0, cv::INTER_AREA);
                cv::threshold(reducedDomain, reducedDomain, 254, 255, cv::THRESH_BINARY);
                cv::bitwise_and(reducedDomain, reducedCoverage & ~reducedHole, reducedDomain);
                cv::Mat1b patches;
                cv::erode(reducedDomain, patches, cv::getStructuringElement(cv::MORPH_RECT, {7, 7}),
                          {-1, -1}, 1, cv::BORDER_CONSTANT, cv::Scalar(0));
                if (cv::countNonZero(patches) < 16)
                    break;
                BackgroundGuide reducedGuide;
                cv::pyrDown(guides.back().color, reducedGuide.color);
                cv::pyrDown(guides.back().variation, reducedGuide.variation);
                images.push_back(reduced);
                guides.push_back(reducedGuide);
                holes.push_back(reducedHole);
                coverages.push_back(reducedCoverage);
                domains.push_back(reducedDomain);
            }
            if (diagnostics)
                diagnostics->guidePyramidMs = std::chrono::duration<double, std::milli>(
                                                  std::chrono::steady_clock::now() - guideStart)
                                                  .count();
            LevelResult filled;
            for (std::size_t i = images.size(); i-- > 0;) {
                const int passes = i == images.size() - 1 ? options.coarsePasses
                                   : i == 0               ? options.finePasses
                                                          : options.intermediatePasses;
                LevelDiagnostics* level = nullptr;
                if (diagnostics) {
                    diagnostics->levels.push_back({QSize(images[i].cols, images[i].rows),
                                                   cv::countNonZero(holes[i]), passes, 0, 0});
                    level = &diagnostics->levels.back();
                }
                filled = fillLevel(images[i], holes[i], coverages[i], domains[i], guides[i], filled,
                                   cancelled, options, passes, level);
            }
            cv::cvtColor(filled.image, rgb, cv::COLOR_Lab2RGB);
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
                const auto& color = rgb(y, x);
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
Result reconstruct(const SnowCanvasSceneItem& item, const QList<SnowCanvasBaseImageSource>& sources,
                   const std::atomic_bool& cancelled) {
    return reconstructWithOptions(item, sources, cancelled, {});
}
} // namespace snow_canvas_smart_erase
