#include "snow_canvas_smart_erase.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QPainter>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <future>
#include <iostream>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <psapi.h>
#endif

namespace {
using namespace snow_canvas_smart_erase;
struct Scenario {
    const char* name;
    QSize size;
    QSize hole;
    bool texture = false;
    bool pen = false;
    bool diagonal = false;
    bool edge = false;
    double scale = 1;
};
const std::vector<Scenario> scenarios{
    {"texture", {512, 320}, {96, 64}},
    {"4k-text", {3840, 2160}, {192, 64}},
    {"8k-text", {7680, 4320}, {192, 64}},
    {"large-region", {3840, 2160}, {384, 256}},
    {"long-pen", {7680, 4320}, {2400, 80}, false, true},
    {"nonrepeating-texture", {1024, 768}, {192, 96}, true},
    {"nonrepeat-medium", {1920, 1080}, {384, 256}, true},
    {"nonrepeat-large", {1920, 1080}, {768, 512}, true},
    {"nonrepeat-long-pen", {3840, 2160}, {2400, 80}, true, true},
    {"diagonal-pen", {1024, 768}, {600, 400}, true, true, true},
    {"source-edge", {640, 480}, {160, 96}, true, false, false, true},
    {"fractional-scale", {1280, 960}, {192, 96}, true, false, false, false, 1.25},
    {"double-scale", {2048, 1536}, {192, 96}, true, false, false, false, 2},
};
QImage fixture(QSize size, bool nonrepeating) {
    QImage image(size, QImage::Format_ARGB32);
    for (int y = 0; y < size.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            const int stripe = ((x / 16 + y / 16) % 2) * 24;
            if (nonrepeating) {
                const double wave =
                    12 * std::sin(x * 0.037 + y * 0.013) + 9 * std::sin(y * 0.071 + x * 0.009);
                row[x] = qRgb(qRound(110 + 60.0 * x / size.width() + wave),
                              qRound(135 + 45.0 * y / size.height() + wave), qRound(165 + wave));
            } else {
                row[x] = qRgb(160 + stripe, 175 + stripe, 195 + stripe);
            }
        }
    }
    return image;
}
quint64 peakWorkingBytes() {
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return static_cast<quint64>(counters.PeakWorkingSetSize);
#endif
    return 0;
}
cv::Mat3f rgb(const QImage& image) {
    cv::Mat3f result(image.height(), image.width());
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const auto c = image.pixel(x, y);
            result(y, x) =
                cv::Vec3f(static_cast<float>(qRed(c)) / 255, static_cast<float>(qGreen(c)) / 255,
                          static_cast<float>(qBlue(c)) / 255);
        }
    }
    return result;
}
bool compare(const QImage& actual, const QImage& expected, const QImage& mask, const char* name) {
    if (actual.size() != expected.size())
        return false;
    // Keep diagnostics local too: full 8K float images would dwarf the worker's
    // memory use. Sixteen pixels exceed the support of the sigma-2 blur below.
    const cv::Mat1b maskView(mask.height(), mask.width(), const_cast<uchar*>(mask.constBits()),
                             static_cast<std::size_t>(mask.bytesPerLine()));
    const auto bounds = cv::boundingRect(maskView);
    if (bounds.empty())
        return false;
    const QRect crop = QRect(bounds.x, bounds.y, bounds.width, bounds.height)
                           .adjusted(-16, -16, 16, 16)
                           .intersected(actual.rect());
    const QImage activeMask = mask.copy(crop);
    const cv::Mat3f a = rgb(actual.copy(crop)), b = rgb(expected.copy(crop));
    cv::Mat3f smoothA, smoothB, labA, labB;
    cv::GaussianBlur(a, smoothA, {}, 2);
    cv::GaussianBlur(b, smoothB, {}, 2);
    cv::cvtColor(smoothA, labA, cv::COLOR_RGB2Lab);
    cv::cvtColor(smoothB, labB, cv::COLOR_RGB2Lab);
    std::vector<double> differences;
    double sum = 0, energyA = 0, energyB = 0;
    for (int y = 0; y < activeMask.height(); ++y) {
        for (int x = 0; x < activeMask.width(); ++x) {
            if (!activeMask.constScanLine(y)[x])
                continue;
            const double delta = cv::norm(labA(y, x) - labB(y, x));
            differences.push_back(delta);
            sum += delta;
            const auto da = a(y, x) - smoothA(y, x), db = b(y, x) - smoothB(y, x);
            energyA += da.dot(da);
            energyB += db.dot(db);
        }
    }
    if (differences.empty())
        return false;
    std::sort(differences.begin(), differences.end());
    const double mean = sum / static_cast<double>(differences.size());
    const double p95 = differences[static_cast<std::size_t>(
                                       std::ceil(0.95 * static_cast<double>(differences.size()))) -
                                   1];
    const double ratio = energyB > 1e-8 ? std::sqrt(energyA / energyB) : 1;
    const bool passed = mean <= 1.5 && p95 <= 4 && ratio >= 0.85 && ratio <= 1.15;
    std::cerr << "quality," << name << ",mean_de," << mean << ",p95_de," << p95 << ",texture_ratio,"
              << ratio << ",passed," << passed << '\n';
    return passed;
}
struct Sample {
    Result result;
    ReconstructionDiagnostics diagnostics;
};
bool run(const Scenario& scenario, const QString& directory, const QString& reference, int warmup,
         int repeats, int jobs, const ReconstructionOptions& options) {
    auto source = fixture(scenario.size, scenario.texture);
    const QSizeF canvasSize(scenario.size.width() / scenario.scale,
                            scenario.size.height() / scenario.scale);
    SnowSceneDisplayItem raw{};
    raw.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
    raw.element_id = {1, 1};
    raw.filter.filter_type = 5;
    raw.filter.strength = 0.5;
    raw.center_x = scenario.edge ? scenario.hole.width() / 2.0 : canvasSize.width() / 2;
    raw.center_y = canvasSize.height() / 2;
    raw.width = scenario.hole.width();
    raw.height = scenario.hole.height();
    raw.opacity = 1;
    SnowCanvasSceneItem item(raw);
    if (scenario.pen) {
        item.is_free_draw = 1;
        item.stroke_width = scenario.diagonal ? 8 : 20;
        std::vector<SnowArrowPoint> points;
        for (int i = 0; i < scenario.hole.width(); i += 4)
            points.push_back(
                {raw.center_x - scenario.hole.width() / 2.0 + i,
                 raw.center_y + (scenario.diagonal
                                     ? (static_cast<double>(i) / scenario.hole.width() - 0.5) *
                                           scenario.hole.height()
                                     : std::sin(i / 50.0) * 30)});
        item.setArrowPoints(points.data(), static_cast<std::uint32_t>(points.size()));
    }
    const auto target = path(item);
    {
        QPainter painter(&source);
        painter.scale(scenario.scale, scenario.scale);
        painter.fillPath(target, QColor(25, 35, 45));
    }
    const QList<SnowCanvasBaseImageSource> sources{{source, QRectF(QPointF(0, 0), canvasSize), {}}};
    std::atomic_bool cancelled{false};
    const auto compute = [&] {
        Sample sample;
        sample.result =
            reconstructWithOptions(item, sources, cancelled, options, &sample.diagnostics);
        return sample;
    };
    std::vector<double> times;
    Sample last;
    for (int iteration = -warmup; iteration < repeats; ++iteration) {
        last = {}; // Do not retain the previous result while measuring the next job.
        QElapsedTimer timer;
        timer.start();
        if (jobs == 2) {
            auto other = std::async(std::launch::async, compute);
            last = compute();
            if (!other.get().result.success)
                return false;
        } else {
            last = compute();
        }
        const double elapsed = static_cast<double>(timer.nsecsElapsed()) / 1e6;
        if (!last.result.success)
            return false;
        if (iteration < 0)
            continue;
        times.push_back(elapsed);
        double search = 0, voting = 0;
        QStringList passCounts;
        for (const auto& level : last.diagnostics.levels) {
            passCounts.push_back(QString::number(level.passes));
            search += level.searchMs;
            voting += level.votingMs;
        }
        const auto& d = last.diagnostics;
        std::cout << "sample," << scenario.name << ',' << iteration << ',' << jobs << ',' << elapsed
                  << ',' << d.workingSize.width() << ',' << d.workingSize.height() << ','
                  << d.maskedPixels << ',' << static_cast<int>(d.path) << ',' << d.levels.size()
                  << ',' << passCounts.join(QLatin1Char('/')).toStdString() << ',' << search << ','
                  << voting << ',' << d.preparationMs << ',' << d.fastPathsMs << ','
                  << d.guidePyramidMs << ',' << peakWorkingBytes() << '\n'
                  << std::flush;
    }
    std::sort(times.begin(), times.end());
    std::cerr << "summary," << scenario.name << ",median_ms," << times[times.size() / 2]
              << ",p95_ms," << times[static_cast<std::size_t>(std::ceil(0.95 * repeats)) - 1]
              << ",peak_working_bytes," << peakWorkingBytes() << '\n';
    if (directory.isEmpty() && reference.isEmpty())
        return true;
    QImage output = source.copy();
    {
        QPainter painter(&output);
        painter.scale(scenario.scale, scenario.scale);
        painter.drawImage(last.result.canvasRect, last.result.filled);
    }
    QImage mask(source.size(), QImage::Format_Grayscale8);
    mask.fill(0);
    {
        QPainter painter(&mask);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(scenario.scale, scenario.scale);
        painter.fillPath(target, Qt::white);
    }
    bool ok = true;
    const QString name = QString::fromLatin1(scenario.name);
    if (!reference.isEmpty()) {
        const QImage expected(QDir(reference).filePath(name + QStringLiteral("-result.png")));
        ok = !expected.isNull() && compare(output, expected, mask, scenario.name);
    }
    if (!directory.isEmpty()) {
        const QRect inspect = QRectF(target.boundingRect().topLeft() * scenario.scale,
                                     target.boundingRect().size() * scenario.scale)
                                  .adjusted(-48, -48, 48, 48)
                                  .toAlignedRect()
                                  .intersected(source.rect());
        QImage preview(inspect.width() * 2, inspect.height(), QImage::Format_ARGB32);
        QPainter painter(&preview);
        painter.drawImage(QPoint(0, 0), source.copy(inspect));
        painter.drawImage(QPoint(inspect.width(), 0), output.copy(inspect));
        painter.end();
        ok = output.save(QDir(directory).filePath(name + QStringLiteral("-result.png"))) && ok;
        ok = preview.save(QDir(directory).filePath(name + QStringLiteral(".png"))) && ok;
    }
    return ok;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("output"),
                                 QStringLiteral("Optional PNG output directory."));
    parser.addOptions({
        {{QStringLiteral("scenario")},
         QStringLiteral("Scenario name, repeatable; default all."),
         QStringLiteral("name")},
        {{QStringLiteral("warmup")},
         QStringLiteral("Warmup iterations."),
         QStringLiteral("count"),
         QStringLiteral("1")},
        {{QStringLiteral("repeat")},
         QStringLiteral("Measured iterations."),
         QStringLiteral("count"),
         QStringLiteral("7")},
        {{QStringLiteral("jobs")},
         QStringLiteral("Concurrent jobs (1 or 2)."),
         QStringLiteral("count"),
         QStringLiteral("1")},
        {{QStringLiteral("schedule")},
         QStringLiteral("Pass schedule: 532, 533, 544, 555."),
         QStringLiteral("passes")},
        {{QStringLiteral("parallel-voting")}, QStringLiteral("Enable two-stripe voting.")},
        {{QStringLiteral("serial-voting")}, QStringLiteral("Disable two-stripe voting.")},
        {{QStringLiteral("reference")},
         QStringLiteral("Baseline result PNG directory for quality gates."),
         QStringLiteral("directory")},
    });
    parser.process(app);
    bool validWarmup = false, validRepeat = false, validJobs = false;
    const int warmup = parser.value(QStringLiteral("warmup")).toInt(&validWarmup);
    const int repeats = parser.value(QStringLiteral("repeat")).toInt(&validRepeat);
    const int jobs = parser.value(QStringLiteral("jobs")).toInt(&validJobs);
    if (!validWarmup || !validRepeat || !validJobs || warmup < 0 || repeats < 1 ||
        (jobs != 1 && jobs != 2) || parser.positionalArguments().size() > 1)
        return 2;
    ReconstructionOptions options;
    const QString schedule = parser.value(QStringLiteral("schedule"));
    if (!schedule.isEmpty()) {
        if (!QStringList{QStringLiteral("532"), QStringLiteral("533"), QStringLiteral("544"),
                         QStringLiteral("555")}
                 .contains(schedule))
            return 2;
        options.coarsePasses = schedule[0].digitValue();
        options.intermediatePasses = schedule[1].digitValue();
        options.finePasses = schedule[2].digitValue();
    }
    if (parser.isSet(QStringLiteral("parallel-voting")) &&
        parser.isSet(QStringLiteral("serial-voting")))
        return 2;
    if (parser.isSet(QStringLiteral("serial-voting")))
        options.parallelVoting = false;
    if (parser.isSet(QStringLiteral("parallel-voting")))
        options.parallelVoting = true;
    const QString output =
        parser.positionalArguments().isEmpty() ? QString() : parser.positionalArguments().first();
    if (!output.isEmpty() && !QDir().mkpath(output))
        return 1;
    QStringList requested = parser.values(QStringLiteral("scenario"));
    for (const auto& name : requested) {
        if (std::none_of(scenarios.begin(), scenarios.end(),
                         [&](const auto& s) { return name == QLatin1String(s.name); }))
            return 2;
    }
    std::cout
        << "record,scenario,iteration,jobs,elapsed_ms,working_width,working_height,masked_"
           "pixels,path,levels,passes,search_ms,vote_ms,preparation_ms,fast_paths_ms,guide_pyramid_"
           "ms,peak_working_bytes\n";
    bool ok = true;
    for (const auto& scenario : scenarios) {
        if (!requested.empty() && !requested.contains(QLatin1String(scenario.name)))
            continue;
        ok = run(scenario, output, parser.value(QStringLiteral("reference")), warmup, repeats, jobs,
                 options) &&
             ok;
    }
    return ok ? 0 : 1;
}
