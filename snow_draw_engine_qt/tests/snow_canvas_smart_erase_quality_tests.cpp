#include "snow_canvas_smart_erase.h"

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QPainter>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>

namespace {
int failures = 0;
snow_canvas_smart_erase::ReconstructionOptions testOptions;
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

SnowCanvasSceneItem rectangle(QRect bounds) {
    SnowSceneDisplayItem raw{};
    raw.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
    raw.filter.filter_type = 5;
    raw.opacity = 1;
    raw.center_x = bounds.x() + bounds.width() / 2.0;
    raw.center_y = bounds.y() + bounds.height() / 2.0;
    raw.width = bounds.width();
    raw.height = bounds.height();
    return SnowCanvasSceneItem(raw);
}

QImage erase(const QImage& source, const SnowCanvasSceneItem& item) {
    std::atomic_bool cancelled{false};
    const auto result = snow_canvas_smart_erase::reconstructWithOptions(
        item, {{source, QRectF(source.rect()), {}}}, cancelled, testOptions);
    require(result.success && !result.filled.isNull(), "quality fixture must reconstruct");
    QImage output = source.copy();
    QPainter painter(&output);
    painter.drawImage(result.canvasRect, result.filled);
    return output;
}

void smoothSurfaces() {
    for (const bool gradient : {false, true}) {
        QImage clean(160, 120, QImage::Format_ARGB32);
        for (int y = 0; y < clean.height(); ++y) {
            for (int x = 0; x < clean.width(); ++x) {
                clean.setPixel(x, y,
                               gradient ? qRgb(60 + x / 2, 80 + y / 2, 100 + (x + y) / 4)
                                        : qRgb(242, 245, 248));
            }
        }
        const QRect bounds(44, 28, 72, 64);
        QImage source = clean.copy();
        QPainter painter(&source);
        painter.fillRect(bounds, Qt::black);
        painter.end();
        const auto output = erase(source, rectangle(bounds));
        double error = 0;
        for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
            for (int x = bounds.left(); x <= bounds.right(); ++x) {
                const auto actual = output.pixelColor(x, y), expected = clean.pixelColor(x, y);
                error += std::abs(actual.red() - expected.red()) +
                         std::abs(actual.green() - expected.green()) +
                         std::abs(actual.blue() - expected.blue());
            }
        }
        require(error / (bounds.width() * bounds.height() * 3) < 1.5,
                "flat and affine backgrounds must be reconstructed without a copied gradient seam");
    }
}

QImage bandedBackground(bool vertical) {
    QImage image(192, 160, QImage::Format_ARGB32);
    image.fill(Qt::white);
    for (int y = 12; y < 130; ++y) {
        for (int x = 12; x < 180; ++x) {
            const int coordinate = vertical ? x : y;
            const QColor base = coordinate < 60   ? QColor(55, 80, 110)
                                : coordinate < 90 ? QColor(205, 65, 90)
                                                  : QColor(65, 150, 40);
            const unsigned hash =
                (static_cast<unsigned>(x) * 73856093U) ^ (static_cast<unsigned>(y) * 19349663U);
            const int texture = static_cast<int>((hash ^ (hash >> 13)) % 25U) - 12;
            image.setPixel(
                x, y, qRgb(base.red() + texture, base.green() + texture, base.blue() + texture));
        }
    }
    // A nearby, unrelated panel must not supply white, black, or yellow fill pixels.
    for (int y = 138; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            image.setPixel(x, y, (x / 7 + y / 5) % 2 ? qRgb(250, 240, 15) : qRgb(5, 5, 5));
    return image;
}

void localTextureAndStructure() {
    for (const bool vertical : {false, true}) {
        const QImage clean = bandedBackground(vertical);
        const QRect bounds(62, 26, 68, 94);
        const auto item = rectangle(bounds);
        QImage source = clean.copy();
        QPainter painter(&source);
        painter.fillRect(bounds, QColor(10, 0, 250));
        painter.end();
        const auto output = erase(source, item);
        require(output == erase(source, item), "texture synthesis must be deterministic");
        int wrong = 0, checked = 0;
        double variation = 0;
        bool outsidePreserved = true;
        for (int y = 0; y < source.height(); ++y) {
            for (int x = 0; x < source.width(); ++x) {
                if (!bounds.contains(x, y)) {
                    outsidePreserved = outsidePreserved && output.pixel(x, y) == source.pixel(x, y);
                    continue;
                }
                const int coordinate = vertical ? x : y;
                if (std::abs(coordinate - 60) < 4 || std::abs(coordinate - 90) < 4)
                    continue;
                const auto actual = output.pixelColor(x, y), expected = clean.pixelColor(x, y);
                const int error = std::abs(actual.red() - expected.red()) +
                                  std::abs(actual.green() - expected.green()) +
                                  std::abs(actual.blue() - expected.blue());
                wrong += error > 90 ? 1 : 0;
                ++checked;
                if (bounds.contains(x + 1, y) && (vertical ? coordinate + 1 : coordinate) != 60 &&
                    (vertical ? coordinate + 1 : coordinate) != 90)
                    variation += std::abs(actual.red() - output.pixelColor(x + 1, y).red());
            }
        }
        require(wrong < checked / 50,
                "large holes must continue local background bands without unrelated panel colors");
        require(outsidePreserved, "reconstruction must preserve every pixel outside the mask");
        require(variation / checked > 1.0, "textured backgrounds must retain fine variation");
        painter.begin(&source);
        painter.fillRect(bounds, QColor(250, 250, 0));
        painter.end();
        require(output == erase(source, item),
                "masked object colors must never influence synthesis");
    }
}

void diagonalStructure() {
    QImage clean(144, 144, QImage::Format_ARGB32);
    for (int y = 0; y < clean.height(); ++y) {
        for (int x = 0; x < clean.width(); ++x) {
            const int noise = ((x * 17 + y * 31) ^ (x * y)) % 17 - 8;
            clean.setPixel(x, y,
                           x + y < 144 ? qRgb(65 + noise, 100 + noise, 170 + noise)
                                       : qRgb(170 + noise, 150 + noise, 50 + noise));
        }
    }
    const QRect bounds(42, 40, 60, 64);
    QImage source = clean.copy();
    QPainter painter(&source);
    painter.fillRect(bounds, Qt::black);
    painter.end();
    const auto output = erase(source, rectangle(bounds));
    int wrong = 0, count = 0;
    for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
        for (int x = bounds.left(); x <= bounds.right(); ++x) {
            if (std::abs(x + y - 144) < 8)
                continue;
            const auto actual = output.pixelColor(x, y), expected = clean.pixelColor(x, y);
            wrong += std::abs(actual.red() - expected.red()) > 40 ? 1 : 0;
            ++count;
        }
    }
    require(wrong < count / 50,
            "background guidance must continue diagonal edges as well as bands");
}

void verifiedRepetitionAndSourceEdges() {
    QImage clean(160, 112, QImage::Format_ARGB32);
    for (int y = 0; y < clean.height(); ++y)
        for (int x = 0; x < clean.width(); ++x) {
            const int stripe = ((x / 8 + y / 8) % 2) * 24;
            clean.setPixel(x, y, qRgb(140 + stripe, 160 + stripe, 180 + stripe));
        }
    const QRect bounds(48, 32, 64, 48);
    QImage source = clean.copy();
    QPainter painter(&source);
    painter.fillRect(bounds, Qt::black);
    painter.end();
    require(erase(source, rectangle(bounds)) == clean,
            "verified periodic backgrounds must preserve their exact phase and colors");

    clean = QImage(32, 40, QImage::Format_ARGB32);
    clean.fill(QColor(70, 130, 190));
    source = clean.copy();
    const QRect corner(0, 0, 27, 35);
    painter.begin(&source);
    painter.fillRect(corner, Qt::black);
    painter.end();
    require(erase(source, rectangle(corner)) == clean,
            "surface reconstruction must work when only two sides of the hole have source pixels");
}

void textureDiversity() {
    QImage source(128, 112, QImage::Format_ARGB32);
    unsigned random = 0x1845a233U;
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            const int value = static_cast<int>(random % 65U) - 32;
            source.setPixel(x, y, qRgb(120 + value, 150 + value, 90 + value));
        }
    }
    const QRect bounds(44, 36, 40, 40);
    QPainter painter(&source);
    painter.fillRect(bounds, Qt::black);
    painter.end();
    const auto output = erase(source, rectangle(bounds));
    // A single translated donor must not explain most of a nonperiodic texture fill.
    int maximumMatches = 0;
    for (int sy = 0; sy + bounds.height() <= source.height(); ++sy) {
        for (int sx = 0; sx + bounds.width() <= source.width(); ++sx) {
            if (QRect(sx, sy, bounds.width(), bounds.height()).intersects(bounds))
                continue;
            int matches = 0;
            for (int y = 0; y < bounds.height(); y += 4) {
                for (int x = 0; x < bounds.width(); x += 4) {
                    matches += std::abs(output.pixelColor(bounds.x() + x, bounds.y() + y).red() -
                                        source.pixelColor(sx + x, sy + y).red()) < 4
                                   ? 1
                                   : 0;
                }
            }
            maximumMatches = std::max(maximumMatches, matches);
        }
    }
    require(maximumMatches < 60, "nonperiodic texture must not be a single translated rectangle");
    double sum = 0, squared = 0;
    for (int y = bounds.top(); y <= bounds.bottom(); ++y)
        for (int x = bounds.left(); x <= bounds.right(); ++x) {
            const int value = output.pixelColor(x, y).red();
            sum += value;
            squared += value * value;
        }
    const double count = bounds.width() * bounds.height();
    require(squared / count - std::pow(sum / count, 2) > 40,
            "patch voting must retain texture instead of averaging it into a smooth surface");
}

void optimizedSearchAndVoting() {
    using namespace snow_canvas_smart_erase;
    for (int scenario = 0; scenario < 4; ++scenario) {
        QImage source(320, 260, QImage::Format_ARGB32);
        for (int y = 0; y < source.height(); ++y) {
            for (int x = 0; x < source.width(); ++x) {
                const unsigned hash =
                    static_cast<unsigned>(x) * 73856093U ^ static_cast<unsigned>(y) * 19349663U;
                const int noise = static_cast<int>((hash ^ (hash >> 13)) % 25U) - 12;
                source.setPixel(
                    x, y,
                    x + y < 280
                        ? qRgba(65 + noise, 100 + noise, 170 + noise, scenario == 3 ? 170 : 255)
                        : qRgba(170 + noise, 150 + noise, 50 + noise, scenario == 3 ? 170 : 255));
            }
        }
        const QImage clean = source;
        const QRectF canvas = scenario == 3 ? QRectF(-13.25, 7.5, 256, 208) : QRectF(source.rect());
        auto item = rectangle(scenario == 1 ? QRect(0, 50, 80, 120) : QRect(40, 40, 240, 180));
        if (scenario == 2) {
            item.is_free_draw = 1;
            item.stroke_width = 8;
            const SnowArrowPoint points[]{{25, 25}, {155, 205}, {290, 45}};
            item.setArrowPoints(points, 3);
        }
        if (scenario == 3) {
            item.center_x = canvas.center().x();
            item.center_y = canvas.center().y();
            item.width = 130;
            item.height = 95;
            item.rotation = 0.35;
        }
        {
            QPainter painter(&source);
            painter.scale(source.width() / canvas.width(), source.height() / canvas.height());
            painter.translate(-canvas.x(), -canvas.y());
            painter.fillPath(path(item), Qt::black);
        }
        const QList<SnowCanvasBaseImageSource> sources{{source, canvas, {}}};
        std::atomic_bool cancelled{false};
        auto options = testOptions;
        options.earlyRejection = false;
        options.parallelVoting = false;
        const auto exhaustive = reconstructWithOptions(item, sources, cancelled, options);
        options.earlyRejection = true;
        ReconstructionDiagnostics diagnostics;
        const auto bounded =
            reconstructWithOptions(item, sources, cancelled, options, &diagnostics);
        require(exhaustive.success && bounded.success && !bounded.filled.isNull(),
                "edge, sparse, large and fractional fixtures must reconstruct");
        require(diagnostics.path == ReconstructionDiagnostics::Path::Patches,
                "optimization fixtures must exercise general patch search");
        require(exhaustive.filled == bounded.filled,
                "early rejection must preserve exhaustive candidate scoring results");
        options.parallelVoting = true;
        const auto parallel = reconstructWithOptions(item, sources, cancelled, options);
        require(parallel.success && parallel.filled == bounded.filled,
                "two-stripe voting must be byte-identical to serial voting");
        if (bounded.filled.isNull())
            continue;
        QImage mask(bounded.filled.size(), QImage::Format_Grayscale8);
        mask.fill(0);
        {
            QPainter painter(&mask);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.scale(mask.width() / bounded.canvasRect.width(),
                          mask.height() / bounded.canvasRect.height());
            painter.translate(-bounded.canvasRect.x(), -bounded.canvasRect.y());
            painter.fillPath(path(item), Qt::white);
        }
        bool knownPreserved = true, alphaPreserved = true;
        for (int y = 0; y < mask.height(); ++y) {
            for (int x = 0; x < mask.width(); ++x) {
                const auto before = bounded.original.pixel(x, y),
                           after = bounded.filled.pixel(x, y);
                if (!mask.constScanLine(y)[x])
                    knownPreserved = knownPreserved && before == after;
                alphaPreserved = alphaPreserved && qAlpha(before) == qAlpha(after);
            }
        }
        if (scenario <= 1) {
            int wrong = 0, checked = 0;
            for (int y = 0; y < mask.height(); ++y) {
                for (int x = 0; x < mask.width(); ++x) {
                    if (!mask.constScanLine(y)[x])
                        continue;
                    const int sx = qRound(bounded.canvasRect.x()) + x;
                    const int sy = qRound(bounded.canvasRect.y()) + y;
                    if (std::abs(sx + sy - 280) < 12)
                        continue;
                    wrong +=
                        std::abs(qRed(bounded.filled.pixel(x, y)) - qRed(clean.pixel(sx, sy))) > 40
                            ? 1
                            : 0;
                    ++checked;
                }
            }
            require(wrong < checked / 50,
                    "large and source-edge holes must retain the diagonal background boundary");
        }
        require(knownPreserved && alphaPreserved,
                "reused voting buffers must preserve every known pixel and alpha value");
        require(!diagnostics.levels.empty(), "patch diagnostics must expose actual levels");
        for (std::size_t i = 0; i < diagnostics.levels.size(); ++i) {
            const int expected = i == 0                               ? options.coarsePasses
                                 : i + 1 == diagnostics.levels.size() ? options.finePasses
                                                                      : options.intermediatePasses;
            require(diagnostics.levels[i].passes == expected,
                    "coarsest budget takes precedence for single-level reconstruction");
        }
        cancelled = true;
        require(!reconstructWithOptions(item, sources, cancelled, options).success,
                "cancelled optimized reconstruction must not publish a result");
    }
    // A small nonperiodic source cannot form another pyramid level. Its only
    // level must keep the coarsest budget even when fine passes are reduced.
    auto small = bandedBackground(false).copy(20, 20, 48, 40);
    const auto smallItem = rectangle(QRect(20, 16, 8, 8));
    {
        QPainter painter(&small);
        painter.fillPath(path(smallItem), Qt::black);
    }
    std::atomic_bool cancelled{false};
    ReconstructionDiagnostics diagnostics;
    const auto result = reconstructWithOptions(smallItem, {{small, QRectF(small.rect()), {}}},
                                               cancelled, testOptions, &diagnostics);
    require(result.success && diagnostics.path == ReconstructionDiagnostics::Path::Patches &&
                diagnostics.levels.size() == 1,
            "small textured source must exercise single-level patch reconstruction");
    if (diagnostics.levels.size() == 1)
        require(diagnostics.levels.front().passes == testOptions.coarsePasses,
                "single-level patch reconstruction must retain the coarsest pass budget");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addOption({QStringLiteral("schedule"), QStringLiteral("Internal pass schedule."),
                      QStringLiteral("passes")});
    parser.process(app);
    const QString schedule = parser.value(QStringLiteral("schedule"));
    if (!schedule.isEmpty()) {
        if (!QStringList{QStringLiteral("532"), QStringLiteral("533"), QStringLiteral("544"),
                         QStringLiteral("555")}
                 .contains(schedule))
            return 2;
        testOptions.coarsePasses = schedule[0].digitValue();
        testOptions.intermediatePasses = schedule[1].digitValue();
        testOptions.finePasses = schedule[2].digitValue();
    }
    smoothSurfaces();
    localTextureAndStructure();
    diagonalStructure();
    verifiedRepetitionAndSourceEdges();
    textureDiversity();
    optimizedSearchAndVoting();
    std::cout << "Smart Erase quality failures: " << failures << '\n';
    return failures ? 1 : 0;
}
