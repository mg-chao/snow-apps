#include "screenshot_selection_presentation_fixture.h"
#include "snow_shot/presentation/screenshotoverlayinputhandler.h"

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
using selection_presentation_test::FrameBackend;

enum class Scenario {
    SteadySelection,
    ChangedSelection,
    Pointer,
    AnimationFrames,
    Retargeting,
    MagnifierVisible,
    MagnifierHidden,
    CombinedAnimation,
    ManualMarquee,
    ManualResize,
    ManualMarqueeMagnifier,
    BooleanAddTargets,
    BooleanSubtractTargets,
    BooleanAddPixelReuse,
    BooleanSubtractPixelReuse,
    BooleanAddAnimation,
    BooleanSubtractAnimation,
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
    Case{"combined_animation_pointer_magnifier_1", Scenario::CombinedAnimation, 1},
    Case{"combined_animation_pointer_magnifier_16", Scenario::CombinedAnimation, 16},
    Case{"manual_marquee_1", Scenario::ManualMarquee, 1},
    Case{"manual_marquee_16", Scenario::ManualMarquee, 16},
    Case{"manual_resize_1", Scenario::ManualResize, 1},
    Case{"manual_resize_16", Scenario::ManualResize, 16},
    Case{"manual_marquee_magnifier_1", Scenario::ManualMarqueeMagnifier, 1},
    Case{"manual_marquee_magnifier_16", Scenario::ManualMarqueeMagnifier, 16},
    Case{"boolean_add_targets_1", Scenario::BooleanAddTargets, 1},
    Case{"boolean_add_targets_16", Scenario::BooleanAddTargets, 16},
    Case{"boolean_subtract_targets_1", Scenario::BooleanSubtractTargets, 1},
    Case{"boolean_subtract_targets_16", Scenario::BooleanSubtractTargets, 16},
    Case{"boolean_add_pixel_reuse_16", Scenario::BooleanAddPixelReuse, 16},
    Case{"boolean_subtract_pixel_reuse_16", Scenario::BooleanSubtractPixelReuse, 16},
    Case{"boolean_add_animation_1", Scenario::BooleanAddAnimation, 1},
    Case{"boolean_subtract_animation_1", Scenario::BooleanSubtractAnimation, 1},
};

bool booleanOperation(Scenario scenario) {
    return scenario == Scenario::BooleanAddTargets ||
           scenario == Scenario::BooleanSubtractTargets ||
           scenario == Scenario::BooleanAddPixelReuse ||
           scenario == Scenario::BooleanSubtractPixelReuse ||
           scenario == Scenario::BooleanAddAnimation ||
           scenario == Scenario::BooleanSubtractAnimation;
}

bool booleanAnimation(Scenario scenario) {
    return scenario == Scenario::BooleanAddAnimation ||
           scenario == Scenario::BooleanSubtractAnimation;
}

ScreenshotRegionGeometry complexConfirmedRegion(const QSize& size) {
    // A deterministic freehand-like contour with concave edges and two holes.
    // Boolean marquee changes cross its edge, preventing the containment fast path.
    const QPointF center(size.width() * 0.48, size.height() * 0.46);
    QPainterPath contour;
    constexpr int vertices = 96;
    constexpr qreal pi = 3.14159265358979323846;
    for (int vertex = 0; vertex < vertices; ++vertex) {
        const qreal angle = vertex * 2.0 * pi / vertices;
        const qreal radius = vertex % 2 == 0 ? 1.0 : 0.82;
        const QPointF point = center + QPointF(std::cos(angle) * size.width() * 0.3 * radius,
                                               std::sin(angle) * size.height() * 0.32 * radius);
        if (vertex == 0)
            contour.moveTo(point);
        else
            contour.lineTo(point);
    }
    contour.closeSubpath();
    contour.setFillRule(Qt::OddEvenFill);
    contour.addEllipse(QRectF(center.x() - size.width() * 0.14, center.y() - size.height() * 0.1,
                              size.width() * 0.09, size.height() * 0.14));
    contour.addEllipse(QRectF(center.x() + size.width() * 0.02, center.y() - size.height() * 0.06,
                              size.width() * 0.08, size.height() * 0.12));
    return ScreenshotRegionGeometry::fromPath(contour, ScreenshotRegionType::Freehand);
}

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
                    int requestedFrameIntervalMs, FrameBackend frameBackend) {
    const bool animation = benchmarkCase.scenario == Scenario::AnimationFrames ||
                           benchmarkCase.scenario == Scenario::Retargeting ||
                           benchmarkCase.scenario == Scenario::CombinedAnimation ||
                           booleanAnimation(benchmarkCase.scenario);
    const int frameIntervalMs =
        requestedFrameIntervalMs > 0 ? requestedFrameIntervalMs : (animation ? 17 : 8);
    Fixture fixture(logicalSize, animation, !animation, frameBackend);
    const bool magnifier = benchmarkCase.scenario == Scenario::MagnifierVisible ||
                           benchmarkCase.scenario == Scenario::MagnifierHidden ||
                           benchmarkCase.scenario == Scenario::CombinedAnimation ||
                           benchmarkCase.scenario == Scenario::ManualMarqueeMagnifier;
    const bool manual = benchmarkCase.scenario == Scenario::ManualMarquee ||
                        benchmarkCase.scenario == Scenario::ManualResize ||
                        benchmarkCase.scenario == Scenario::ManualMarqueeMagnifier;
    if (benchmarkCase.toolbarHidden) {
        fixture.coordinator.setSelectionToolbarHidden(true);
        fixture.processEvents();
        fixture.resetCounters();
    }
    if (magnifier)
        fixture.enableColorPicker(benchmarkCase.scenario == Scenario::MagnifierHidden
                                      ? ScreenshotColorPickerDisplayMode::AlwaysHide
                                      : ScreenshotColorPickerDisplayMode::AlwaysShow);
    if (benchmarkCase.scenario == Scenario::Pointer ||
        benchmarkCase.scenario == Scenario::CombinedAnimation)
        fixture.enableGuides();
    const QRectF base = fixture.baseSelection();
    const QSize canvasSize = fixture.geometry.canvasBounds().size().toSize();
    const auto overlayPosition = [&fixture](const QPointF& canvasPosition) {
        return fixture.geometry.logicalPositionForCanvasPoint(fixture.displays.displayAt(0),
                                                              canvasPosition) -
               fixture.overlay.captureGeometry().topLeft();
    };
    std::unique_ptr<ScreenshotOverlayInputHandler> input;
    if (manual) {
        fixture.displays.startup->resumeLiveInput();
        ScreenshotOverlayInputActions actions;
        actions.updateOverlayState = [&fixture] { fixture.services->updateOverlayState(); };
#if !defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE)
        actions.requestSelectionDragPresentation = [&fixture] {
            fixture.services->requestSelectionDragPresentation();
        };
#endif
        actions.updateGuideLinesForOverlay = [&fixture](ScreenshotOverlayWindow* owner,
                                                        const QPointF& position) {
            fixture.services->updatePointerPresentation(owner, position);
        };
        if (magnifier) {
            actions.updateColorPickerForOverlay = [&fixture](ScreenshotOverlayWindow* owner,
                                                             const QPointF& position) {
                fixture.services->requestColorPickerPresentation(owner, position);
            };
            actions.updateColorPickerForSelectionDrag = [&fixture](const QPointF& position) {
#if defined(SNOW_SHOT_SELECTION_PRESENTATION_BASELINE)
                fixture.services->discardColorPickerPresentation();
                fixture.colorPickerController->updateForSelectionDrag(
                    position, fixture.services->colorPickerContext());
#else
                fixture.services->requestSelectionDragColorPickerPresentation(position);
#endif
            };
        }
        input =
            std::make_unique<ScreenshotOverlayInputHandler>(ScreenshotOverlayInputHandlerContext{
                fixture.captureState, fixture.interaction, fixture.selection,
                fixture.intelligentSelection, fixture.geometry, fixture.displays,
                std::move(actions)});
        if (benchmarkCase.scenario == Scenario::ManualResize) {
            fixture.interaction.confirmSelection();
            fixture.services->updateOverlayState();
            if (!input->beginSelectionResizeAtCanvasPosition(base.bottomRight()))
                throw std::runtime_error("manual resize benchmark requires a resize gesture");
        } else {
            fixture.selection.clearSelection();
            fixture.interaction.returnToSelectionMode(false);
            fixture.services->updateOverlayState();
            input->handleMousePress(&fixture.overlay, overlayPosition(base.topLeft()));
        }
        fixture.flushFrame();
        fixture.processEvents();
        fixture.resetCounters();
    } else if (booleanOperation(benchmarkCase.scenario)) {
        fixture.selection.setSelectionRegion(complexConfirmedRegion(canvasSize));
        const bool subtract = benchmarkCase.scenario == Scenario::BooleanSubtractTargets ||
                              benchmarkCase.scenario == Scenario::BooleanSubtractPixelReuse ||
                              benchmarkCase.scenario == Scenario::BooleanSubtractAnimation;
        fixture.selection.beginRegionOperation(
            subtract ? ScreenshotSelectionModel::RegionOperation::Subtract
                     : ScreenshotSelectionModel::RegionOperation::Add);
        fixture.requestSelection(base);
        fixture.flushFrame();
        fixture.processEvents();
        fixture.resetCounters();
    }
    const int totalFrames = iterations + warmup;
    std::vector<QRectF> targets;
    std::vector<QPointF> pointers;
    targets.reserve(static_cast<std::size_t>(totalFrames * benchmarkCase.requestsPerFrame));
    pointers.reserve(targets.capacity());
    for (int frame = 0; frame < totalFrames; ++frame) {
        for (int request = 0; request < benchmarkCase.requestsPerFrame; ++request) {
            const int sample = frame * benchmarkCase.requestsPerFrame + request;
            if (booleanOperation(benchmarkCase.scenario)) {
                const qreal x = canvasSize.width() * 0.59 + frame % 83;
                const qreal y = canvasSize.height() * 0.38 + (frame * 3) % 47;
                const bool pixelReuse =
                    benchmarkCase.scenario == Scenario::BooleanAddPixelReuse ||
                    benchmarkCase.scenario == Scenario::BooleanSubtractPixelReuse;
                const qreal delta = pixelReuse ? 0.25 + 0.01 * request : 2.0 * request;
                targets.emplace_back(std::floor(x) + delta, std::floor(y) + delta,
                                     std::floor(canvasSize.width() * 0.18),
                                     std::floor(canvasSize.height() * 0.22));
            } else {
                targets.push_back(base.translated(2.0 * (1 + sample % 121), (sample * 3) % 71));
            }
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
    qint64 schedulerWakeups = 0;
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
        const bool requestTarget = (benchmarkCase.scenario != Scenario::AnimationFrames &&
                                    benchmarkCase.scenario != Scenario::CombinedAnimation &&
                                    !booleanAnimation(benchmarkCase.scenario)) ||
                                   frame % framesPerTarget == 0;
        if (requestTarget || benchmarkCase.scenario == Scenario::CombinedAnimation) {
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
                case Scenario::CombinedAnimation:
                    if (requestTarget)
                        fixture.requestSelection(targets[index]);
                    fixture.requestMagnifier(pointers[index]);
                    break;
                case Scenario::ManualMarquee:
                case Scenario::ManualMarqueeMagnifier:
                    input->handleMouseMove(&fixture.overlay,
                                           overlayPosition(targets[index].bottomRight()));
                    break;
                case Scenario::ManualResize:
                    input->updateSelectionResizeAtCanvasPosition(targets[index].bottomRight());
                    break;
                case Scenario::BooleanAddTargets:
                case Scenario::BooleanSubtractTargets:
                case Scenario::BooleanAddPixelReuse:
                case Scenario::BooleanSubtractPixelReuse:
                case Scenario::BooleanAddAnimation:
                case Scenario::BooleanSubtractAnimation:
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
            schedulerWakeups += fixture.frameWakeObserver.wakeups;
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
    const qint64 inputSamples = static_cast<qint64>(iterations) * benchmarkCase.requestsPerFrame;
    const bool selectionIncluded = benchmarkCase.scenario != Scenario::Pointer &&
                                   benchmarkCase.scenario != Scenario::MagnifierVisible &&
                                   benchmarkCase.scenario != Scenario::MagnifierHidden;
    return {{"scenario", QString::fromLatin1(benchmarkCase.name)},
            {"iterations", iterations},
            {"requests_per_frame", benchmarkCase.requestsPerFrame},
            {"selection_toolbar_hidden", benchmarkCase.toolbarHidden},
            {"magnifier_included", magnifier},
            {"manual_input_handler_included", manual},
            {"boolean_region_operation_included", booleanOperation(benchmarkCase.scenario)},
            {"magnifier_display_mode", !magnifier ? "excluded"
                                       : benchmarkCase.scenario == Scenario::MagnifierHidden
                                           ? "always_hide"
                                           : "always_show"},
            {"request_count",
             benchmarkCase.scenario == Scenario::CombinedAnimation ? inputSamples : targetRequests},
            {"selection_request_count", selectionIncluded ? targetRequests : 0},
            {"pointer_request_count",
             magnifier || benchmarkCase.scenario == Scenario::Pointer ? inputSamples : 0},
            {"magnifier_request_count", magnifier ? inputSamples : 0},
            {"scheduler_wake_events", schedulerWakeups},
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
    parser.setApplicationDescription(
        QStringLiteral("Real screenshot selection presentation benchmark"));
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
    parser.addOption({QStringLiteral("scenario"),
                      QStringLiteral("Scenario name, optimizations, or all"),
                      QStringLiteral("name"), QStringLiteral("all")});
    parser.addOption({QStringLiteral("frame-backend"),
                      QStringLiteral("Presentation wakeup backend: automatic or qt_timer"),
                      QStringLiteral("backend"), QStringLiteral("automatic")});
    parser.addOption(
        {QStringLiteral("output"), QStringLiteral("JSON output path"), QStringLiteral("path")});
    parser.process(application);
    const QString backendName = parser.value(QStringLiteral("frame-backend"));
    if (backendName != QStringLiteral("automatic") && backendName != QStringLiteral("qt_timer"))
        throw std::runtime_error("frame-backend must be automatic or qt_timer");
    const auto frameBackend =
        backendName == QStringLiteral("qt_timer") ? FrameBackend::QtTimer : FrameBackend::Automatic;
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
        const bool optimizationCase = booleanOperation(benchmarkCase.scenario) ||
                                      benchmarkCase.scenario == Scenario::ManualMarquee ||
                                      benchmarkCase.scenario == Scenario::ManualResize ||
                                      benchmarkCase.scenario == Scenario::ManualMarqueeMagnifier;
        if (selected == QStringLiteral("all") || selected == QLatin1String(benchmarkCase.name) ||
            (selected == QStringLiteral("optimizations") && optimizationCase))
            results.append(runCase(benchmarkCase, logicalSize, iterations, warmup, frameIntervalMs,
                                   frameBackend));
    }
    if (results.isEmpty())
        throw std::runtime_error("unknown selection presentation benchmark scenario");
    const qreal dpr = QGuiApplication::primaryScreen()->devicePixelRatio();
    QJsonObject report{
        {"benchmark", "screenshot_selection_presentation"},
        {"platform", QGuiApplication::platformName()},
        {"scheduler_backend", backendName},
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
         "presentation and the real picker controller/window. The optional archived legacy "
         "baseline target updates hover magnifiers immediately and reports internal "
         "sampling/preview "
         "counters as null; the current target batches hover magnifiers with the presentation "
         "frame. "
         "Picker paint/move/owner-change event counts are comparable. Combined cases include "
         "animated "
         "selection, guides, and magnifier bursts. "
         "Manual cases deliver coordinates through the real input handler and include constrained "
         "drag geometry; manual magnifier cases also include the real drag anchor sampler. "
         "Boolean cases use a concave freehand contour with two holes; pixel reuse "
         "keeps sixteen subpixel requests inside one integer marquee per frame, and animation "
         "allows displayed marquees to evolve between target requests. "
         "Idle waits, selector workers, native mouse delivery, and input-to-photon latency "
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
