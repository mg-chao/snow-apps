#include "screenshot_selection_presentation_fixture.h"

#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QThread>

#include <algorithm>
#include <array>
#include <numeric>
#include <vector>

namespace {
using selection_presentation_test::Fixture;

enum class Scenario {
    SteadySelection,
    ChangedSelection,
    Pointer,
    AnimationFrames,
    Retargeting,
    MagnifierVisible,
    MagnifierHidden,
};

struct Case {
    const char* name;
    Scenario scenario;
    int requestsPerFrame;
    bool toolbarHidden = false;
};

constexpr std::array kCases{
    Case{"steady_target_1", Scenario::SteadySelection, 1},
    Case{"steady_target_16", Scenario::SteadySelection, 16},
    Case{"changed_targets_1", Scenario::ChangedSelection, 1},
    Case{"changed_targets_16", Scenario::ChangedSelection, 16},
    Case{"pointer_burst_1", Scenario::Pointer, 1},
    Case{"pointer_burst_16", Scenario::Pointer, 16},
    Case{"animation_frames_1", Scenario::AnimationFrames, 1},
    Case{"animation_frames_16", Scenario::AnimationFrames, 16},
    Case{"animation_retargeting_1", Scenario::Retargeting, 1},
    Case{"animation_retargeting_16", Scenario::Retargeting, 16},
    Case{"changed_targets_hidden_1", Scenario::ChangedSelection, 1, true},
    Case{"changed_targets_hidden_16", Scenario::ChangedSelection, 16, true},
    Case{"animation_frames_hidden_1", Scenario::AnimationFrames, 1, true},
    Case{"magnifier_visible_1", Scenario::MagnifierVisible, 1},
    Case{"magnifier_visible_16", Scenario::MagnifierVisible, 16},
    Case{"magnifier_hidden_1", Scenario::MagnifierHidden, 1},
    Case{"magnifier_hidden_16", Scenario::MagnifierHidden, 16},
};

QJsonObject distribution(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const std::size_t size = values.size();
    if (size == 0)
        return {};
    const auto percentile = [&](double fraction) {
        const double index = static_cast<double>(size - 1) * fraction;
        const auto lower = static_cast<std::size_t>(index);
        const std::size_t upper = std::min(lower + 1, size - 1);
        return values[lower] +
               (values[upper] - values[lower]) * (index - static_cast<double>(lower));
    };
    return {
        {"median", percentile(0.5)},
        {"p95", percentile(0.95)},
        {"mean", std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(size)},
        {"min", values.front()},
        {"max", values.back()}};
}

QJsonObject runCase(const Case& benchmarkCase, const QSize& logicalSize, int iterations, int warmup,
                    int requestedFrameIntervalMs) {
    const bool animation = benchmarkCase.scenario == Scenario::AnimationFrames ||
                           benchmarkCase.scenario == Scenario::Retargeting;
    const int frameIntervalMs =
        requestedFrameIntervalMs > 0 ? requestedFrameIntervalMs : (animation ? 17 : 8);
    Fixture fixture(logicalSize, animation, !animation);
    const bool magnifier = benchmarkCase.scenario == Scenario::MagnifierVisible ||
                           benchmarkCase.scenario == Scenario::MagnifierHidden;
    if (benchmarkCase.toolbarHidden) {
        fixture.coordinator.setSelectionToolbarHidden(true);
        fixture.processEvents();
        fixture.resetCounters();
    }
    if (magnifier)
        fixture.enableColorPicker(benchmarkCase.scenario == Scenario::MagnifierHidden
                                      ? ScreenshotColorPickerDisplayMode::AlwaysHide
                                      : ScreenshotColorPickerDisplayMode::AlwaysShow);
    if (benchmarkCase.scenario == Scenario::Pointer)
        fixture.enableGuides();
    const QRectF base = fixture.baseSelection();
    const int totalFrames = iterations + warmup;
    std::vector<QRectF> targets;
    std::vector<QPointF> pointers;
    targets.reserve(static_cast<std::size_t>(totalFrames * benchmarkCase.requestsPerFrame));
    pointers.reserve(targets.capacity());
    for (int frame = 0; frame < totalFrames; ++frame) {
        for (int request = 0; request < benchmarkCase.requestsPerFrame; ++request) {
            const int sample = frame * benchmarkCase.requestsPerFrame + request;
            targets.push_back(base.translated(2.0 * (1 + sample % 121), (sample * 3) % 71));
            if (magnifier) {
                // Keep the sample band below the selection and its interactive toolbar.
                pointers.emplace_back(logicalSize.width() * 0.65 +
                                          sample % std::max(1, logicalSize.width() / 4),
                                      logicalSize.height() * 0.6 +
                                          (sample * 3) % std::max(1, logicalSize.height() / 5));
            } else {
                pointers.emplace_back(40.0 + sample % (logicalSize.width() - 80),
                                      40.0 + (sample * 3) % (logicalSize.height() - 80));
            }
        }
    }

    std::vector<double> submissionMs;
    std::vector<double> frameCommitMs;
    std::vector<double> preparationMs;
    std::vector<double> eventProcessingMs;
    std::vector<double> totalMs;
    submissionMs.reserve(static_cast<std::size_t>(iterations));
    frameCommitMs.reserve(static_cast<std::size_t>(iterations));
    preparationMs.reserve(static_cast<std::size_t>(iterations));
    eventProcessingMs.reserve(static_cast<std::size_t>(iterations));
    totalMs.reserve(static_cast<std::size_t>(iterations));
    qint64 semanticNotifications = 0;
    qint64 hintTranslationRequests = 0;
    qint64 canvasPaints = 0;
    qint64 uiPaints = 0;
    qint64 canvasDamagePixels = 0;
    qint64 displayedGeometryChanges = 0;
    qint64 targetRequests = 0;
    qint64 colorPickerPaints = 0;
    qint64 colorPickerMoveEvents = 0;
    qint64 colorPickerOwnerChangeEvents = 0;
#if !defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE)
    qint64 colorPickerSamples = 0;
    qint64 colorPickerPreviews = 0;
    qint64 colorPickerPositionMoves = 0;
    qint64 colorPickerOwnerChanges = 0;
#endif
    const int framesPerTarget =
        (ScreenshotSmartSelectionTransition::kDurationMs + frameIntervalMs - 1) / frameIntervalMs +
        2;
    QRectF previousDisplayed = fixture.displayedSelection();
    QElapsedTimer cadence;
    cadence.start();
    qint64 measurementStartedNs = 0;
    for (int frame = 0; frame < totalFrames; ++frame) {
        if (frame == warmup)
            measurementStartedNs = cadence.nsecsElapsed();
        fixture.resetCounters();
        QElapsedTimer preparation;
        preparation.start();
        const bool requestTarget =
            benchmarkCase.scenario != Scenario::AnimationFrames || frame % framesPerTarget == 0;
        if (requestTarget) {
            for (int request = 0; request < benchmarkCase.requestsPerFrame; ++request) {
                const auto index =
                    static_cast<std::size_t>(frame * benchmarkCase.requestsPerFrame + request);
                switch (benchmarkCase.scenario) {
                case Scenario::SteadySelection:
                    fixture.requestSelection(base);
                    break;
                case Scenario::Pointer:
                    fixture.requestPointer(pointers[index]);
                    break;
                case Scenario::MagnifierVisible:
                case Scenario::MagnifierHidden:
                    fixture.requestMagnifier(pointers[index]);
                    break;
                case Scenario::ChangedSelection:
                case Scenario::AnimationFrames:
                case Scenario::Retargeting:
                    fixture.requestSelection(targets[index]);
                    break;
                }
            }
        }
        const qint64 submissionNs = preparation.nsecsElapsed();
        if (!animation) {
            fixture.advanceClock(frameIntervalMs);
            fixture.flushFrame();
        }
        // Scheduled animation commits occur during Qt event processing. Controlled
        // frames expose their explicit clock advance and flush as a separate phase.
        const qint64 preparationNs = animation ? submissionNs : preparation.nsecsElapsed();
        const double submissionMilliseconds = static_cast<double>(submissionNs) / 1000000.0;
        const double frameCommitMilliseconds =
            static_cast<double>(preparationNs - submissionNs) / 1000000.0;
        const double preparationMilliseconds = static_cast<double>(preparationNs) / 1000000.0;

        qint64 frameGeometryChanges = 0;
        const auto observeGeometry = [&]() {
            const QRectF displayed = fixture.displayedSelection();
            if (displayed != previousDisplayed) {
                ++frameGeometryChanges;
                previousDisplayed = displayed;
            }
        };
        observeGeometry();
        QElapsedTimer events;
        qint64 eventProcessingNs = 0;
        const auto processReadyEvents = [&]() {
            events.start();
            fixture.processEvents();
            eventProcessingNs += events.nsecsElapsed();
            observeGeometry();
        };
        if (animation) {
            // Keep Qt timers and paints running throughout each input interval. Sleeping for
            // the full interval would cap both animation drivers at the input sampling rate.
            // Fixed deadlines keep the supplied input cadence independent of painting cost.
            const qint64 deadlineNs = static_cast<qint64>(frame + 1) * frameIntervalMs * 1000000;
            while (true) {
                processReadyEvents();
                const qint64 remainingNs = deadlineNs - cadence.nsecsElapsed();
                if (remainingNs <= 0)
                    break;
                const qint64 sleepNs = std::min<qint64>(remainingNs, 1000000);
                QThread::usleep(static_cast<unsigned long>((sleepNs + 999) / 1000));
            }
        } else {
            processReadyEvents();
        }
        const double eventMilliseconds = static_cast<double>(eventProcessingNs) / 1000000.0;
        if (frame >= warmup) {
            submissionMs.push_back(submissionMilliseconds);
            frameCommitMs.push_back(frameCommitMilliseconds);
            preparationMs.push_back(preparationMilliseconds);
            eventProcessingMs.push_back(eventMilliseconds);
            totalMs.push_back(preparationMilliseconds + eventMilliseconds);
            semanticNotifications += fixture.stateNotifications;
            hintTranslationRequests += fixture.hintTranslations.requests;
            canvasPaints += fixture.paintObserver.canvasPaints;
            uiPaints += fixture.paintObserver.uiPaints;
            canvasDamagePixels += fixture.paintObserver.canvasDamagePixels;
            displayedGeometryChanges += frameGeometryChanges;
            colorPickerPaints += fixture.paintObserver.colorPickerPaints;
            colorPickerMoveEvents += fixture.paintObserver.colorPickerMoves;
            colorPickerOwnerChangeEvents += fixture.paintObserver.colorPickerOwnerChanges;
#if !defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE)
            if (auto* picker = fixture.coordinator.colorPicker()) {
                const auto counters = picker->workCounters();
                colorPickerSamples += static_cast<qint64>(counters.samples);
                colorPickerPreviews += static_cast<qint64>(counters.previews);
                colorPickerPositionMoves += static_cast<qint64>(counters.moves);
                colorPickerOwnerChanges += static_cast<qint64>(counters.ownerChanges);
            }
#endif
            if (requestTarget)
                targetRequests += benchmarkCase.requestsPerFrame;
        }
    }
    const double viewportPixels =
        static_cast<double>(fixture.overlay.canvas()->width()) * fixture.overlay.canvas()->height();
    const double elapsedMilliseconds =
        static_cast<double>(cadence.nsecsElapsed() - measurementStartedNs) / 1000000.0;
    const double workMilliseconds = std::accumulate(totalMs.begin(), totalMs.end(), 0.0);
    const QJsonValue millisecondsPerPaint =
        canvasPaints > 0 ? QJsonValue(workMilliseconds / static_cast<double>(canvasPaints))
                         : QJsonValue(QJsonValue::Null);
    const QJsonValue millisecondsPerGeometryChange =
        displayedGeometryChanges > 0
            ? QJsonValue(workMilliseconds / static_cast<double>(displayedGeometryChanges))
            : QJsonValue(QJsonValue::Null);
    return {{"scenario", QString::fromLatin1(benchmarkCase.name)},
            {"iterations", iterations},
            {"requests_per_frame", benchmarkCase.requestsPerFrame},
            {"selection_toolbar_hidden", benchmarkCase.toolbarHidden},
            {"magnifier_included", magnifier},
            {"magnifier_display_mode", !magnifier ? "excluded"
                                       : benchmarkCase.scenario == Scenario::MagnifierHidden
                                           ? "always_hide"
                                           : "always_show"},
            {"request_count", targetRequests},
            {"clock", animation ? "real_monotonic" : "controlled_monotonic"},
            {"frame_commit", animation ? "scheduled_timer" : "explicit_frame"},
            {"input_interval_ms", frameIntervalMs},
            {"maximum_requested_animation_idle_wait_ms", animation ? 1 : 0},
            {"elapsed_ms", elapsedMilliseconds},
            {"submission_ms", distribution(std::move(submissionMs))},
            {"frame_commit_ms", distribution(std::move(frameCommitMs))},
            {"preparation_ms", distribution(std::move(preparationMs))},
            {"event_processing_ms", distribution(std::move(eventProcessingMs))},
            {"total_ms", distribution(std::move(totalMs))},
            {"semantic_notifications", semanticNotifications},
            {"hint_translation_requests", hintTranslationRequests},
            {"canvas_paints", canvasPaints},
            {"ui_paints", uiPaints},
            {"magnifier_paints", colorPickerPaints},
            {"magnifier_move_events", colorPickerMoveEvents},
            {"magnifier_owner_change_events", colorPickerOwnerChangeEvents},
#if !defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE)
            {"magnifier_samples", colorPickerSamples},
            {"magnifier_preview_rebuilds", colorPickerPreviews},
            {"magnifier_position_moves", colorPickerPositionMoves},
            {"magnifier_owner_changes", colorPickerOwnerChanges},
#else
            {"magnifier_samples", QJsonValue(QJsonValue::Null)},
            {"magnifier_preview_rebuilds", QJsonValue(QJsonValue::Null)},
            {"magnifier_position_moves", QJsonValue(QJsonValue::Null)},
            {"magnifier_owner_changes", QJsonValue(QJsonValue::Null)},
#endif
            {"displayed_geometry_changes", displayedGeometryChanges},
            {"work_ms_per_canvas_paint", millisecondsPerPaint},
            {"work_ms_per_geometry_change", millisecondsPerGeometryChange},
            {"mean_canvas_damage_ratio",
             static_cast<double>(canvasDamagePixels) / (viewportPixels * iterations)}};
}

int positiveInteger(const QCommandLineParser& parser, const QString& name) {
    bool valid = false;
    const int value = parser.value(name).toInt(&valid);
    if (!valid || value <= 0)
        throw std::runtime_error("benchmark counts and dimensions must be positive integers");
    return value;
}
} // namespace

int main(int argc, char* argv[]) {
    bool native = false;
    for (int argument = 1; argument < argc; ++argument)
        native = native || std::strcmp(argv[argument], "--native") == 0;
    if (!native)
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Real smart selection presentation benchmark"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("native"), QStringLiteral("Use the native Qt platform")});
    parser.addOption(
        {QStringLiteral("offscreen"), QStringLiteral("Use the offscreen Qt platform")});
    parser.addOption({QStringLiteral("iterations"), QStringLiteral("Measured frames"),
                      QStringLiteral("count"), QStringLiteral("240")});
    parser.addOption({QStringLiteral("warmup"), QStringLiteral("Warmup frames"),
                      QStringLiteral("count"), QStringLiteral("30")});
    parser.addOption({QStringLiteral("width"), QStringLiteral("Logical overlay width"),
                      QStringLiteral("pixels"), QStringLiteral("1800")});
    parser.addOption({QStringLiteral("height"), QStringLiteral("Logical overlay height"),
                      QStringLiteral("pixels"), QStringLiteral("975")});
    parser.addOption({QStringLiteral("frame-interval-ms"),
                      QStringLiteral("Sample interval; default 17ms animation, 8ms other cases"),
                      QStringLiteral("milliseconds")});
    parser.addOption({QStringLiteral("scenario"), QStringLiteral("Scenario name or all"),
                      QStringLiteral("name"), QStringLiteral("all")});
    parser.addOption(
        {QStringLiteral("output"), QStringLiteral("JSON output path"), QStringLiteral("path")});
    parser.process(application);
    const int iterations = positiveInteger(parser, QStringLiteral("iterations"));
    const int warmup = positiveInteger(parser, QStringLiteral("warmup"));
    const QSize logicalSize(positiveInteger(parser, QStringLiteral("width")),
                            positiveInteger(parser, QStringLiteral("height")));
    if (logicalSize.width() <= 80 || logicalSize.height() <= 80)
        throw std::runtime_error("benchmark overlay dimensions must exceed 80 pixels");
    const int frameIntervalMs = parser.isSet(QStringLiteral("frame-interval-ms"))
                                    ? positiveInteger(parser, QStringLiteral("frame-interval-ms"))
                                    : 0;
    selection_presentation_test::IsolatedStorage storage;
    QJsonArray results;
    const QString selected = parser.value(QStringLiteral("scenario"));
    for (const Case& benchmarkCase : kCases) {
        if (selected == QStringLiteral("all") || selected == QLatin1String(benchmarkCase.name))
            results.append(
                runCase(benchmarkCase, logicalSize, iterations, warmup, frameIntervalMs));
    }
    if (results.isEmpty())
        throw std::runtime_error("unknown selection presentation benchmark scenario");
    const qreal dpr = QGuiApplication::primaryScreen()->devicePixelRatio();
    QJsonObject report{
        {"benchmark", "screenshot_selection_presentation"},
        {"platform", QGuiApplication::platformName()},
#if defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE)
        {"implementation", "baseline"},
#else
        {"implementation", "optimized"},
#endif
        {"device_pixel_ratio", dpr},
        {"warmup", warmup},
        {"logical_width", logicalSize.width()},
        {"logical_height", logicalSize.height()},
        {"physical_width", static_cast<int>(std::ceil(logicalSize.width() * dpr))},
        {"physical_height", static_cast<int>(std::ceil(logicalSize.height() * dpr))},
        {"screen_refresh_hz", QGuiApplication::primaryScreen()->refreshRate()},
        {"animation_sample_interval_ms", frameIntervalMs > 0 ? frameIntervalMs : 17},
        {"measurement",
         "Request submission and explicit frame commit are measured separately; preparation "
         "includes both phases. Scheduled animation frame_commit_ms is zero, with timer commits "
         "included in Qt event processing. "
         "Controlled cases commit once per supplied frame; animation uses each implementation's "
         "event-driven timer with event pumping and requested idle waits of at most 1ms. "
         "Event-processing work and counters cover the entire input interval. Compare animation "
         "costs with observed paint and geometry counts. Magnifier cases include pointer "
         "presentation and the real picker controller/window: baseline updates each input sample "
         "immediately, optimized commits the latest sample with the presentation frame. Picker "
         "paint/move/owner-change event counts are comparable; internal sampling/preview counters "
         "are unavailable for the baseline and reported as null. Other cases exclude magnifier "
         "work. Idle waits, selector workers, native mouse delivery, and input-to-photon latency "
         "are excluded from work timings."},
        {"results", results}};
    const QByteArray json = QJsonDocument(report).toJson(QJsonDocument::Indented);
    if (parser.isSet(QStringLiteral("output"))) {
        QFile file(parser.value(QStringLiteral("output")));
        if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size())
            throw std::runtime_error("unable to save selection presentation benchmark JSON");
    }
    QTextStream(stdout) << json;
    return 0;
}
