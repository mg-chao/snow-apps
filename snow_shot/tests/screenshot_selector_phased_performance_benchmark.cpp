#include "snow_shot/presentation/screenshotselectorcoordinator.h"
#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <Windows.h>
#include <cstdio>
#include <functional>

// Runs the real Qt coordinator, queued bridge, and selection model. The legacy
// build uses preserved sources, rather than emulating the old scheduling policy.
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const QStringList args = app.arguments();
    const int backendIndex = args.indexOf(QStringLiteral("--backend"));
    qputenv("SNOW_SHOT_SELECTOR_BACKEND",
            backendIndex >= 0 ? args.value(backendIndex + 1).toLatin1() : QByteArray("uia"));
    QVector<QPoint> points;
    for (int i = 1; i + 2 < args.size(); ++i) {
        if (args[i] == QStringLiteral("--point")) {
            points.push_back(QPoint(args[i + 1].toInt(), args[i + 2].toInt()));
            i += 2;
        }
    }
    HWND native = nullptr;
    if (points.isEmpty()) {
        native = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"STATIC",
                                 L"UIA benchmark fixture", WS_POPUP | WS_VISIBLE, 60, 60, 500, 320,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!native)
            return 2;
        for (int x : {20, 260})
            CreateWindowExW(0, L"BUTTON", L"Control", WS_CHILD | WS_VISIBLE, x, 40, 200, 60, native,
                            nullptr, GetModuleHandleW(nullptr), nullptr);
        points = {QPoint(100, 120), QPoint(350, 120)};
    }
    ScreenshotSelectorCoordinator coordinator;
    ScreenshotIntelligentSelectionModel model;
    model.beginCaptureSession(true);
    QElapsedTimer clock;
    clock.start();
    QString scenario;
    int round = 0, sample = 0, received = 0;
    qint64 activeAt = 0, pendingAt = 0, lastDisplay = 0;
    bool active = false, pending = false, stopping = false;
    QTimer moves;
    moves.setTimerType(Qt::PreciseTimer);
    moves.setInterval(1);
    std::function<void()> beginRound;
    std::function<void()> nextStage;
    const auto submit = [&](const QPoint& point) {
        const qint64 now = clock.nsecsElapsed();
        if (active) {
            pending = true;
            pendingAt = now;
        } else {
            active = true;
            activeAt = now;
        }
        if (!coordinator.requestHitTest(point, ScreenshotSelectorHitTestMode::WindowSubElement))
            app.exit(3);
    };
    const auto print = [&](const char* phase, const QVector<QRectF>& rects, bool changed) {
        const qint64 now = clock.nsecsElapsed();
        std::printf("%s,%d,%d,%s,%.6f,%.6f,%lld,%d\n", scenario.toUtf8().constData(), round,
                    received, phase, static_cast<double>(now - activeAt) / 1e6,
                    lastDisplay ? static_cast<double>(now - lastDisplay) / 1e6 : 0.0,
                    static_cast<long long>(rects.size()), changed ? 1 : 0);
        lastDisplay = now;
    };
    nextStage = [&]() {
        lastDisplay = 0;
        coordinator.resetHitTestState();
        active = pending = false;
        if (scenario == QStringLiteral("cold"))
            scenario = QStringLiteral("warm");
        else if (scenario == QStringLiteral("warm"))
            scenario = QStringLiteral("new_branch");
        else if (scenario == QStringLiteral("new_branch")) {
            scenario = QStringLiteral("rapid");
            sample = 0;
            moves.start();
            return;
        } else if (scenario == QStringLiteral("rapid")) {
            scenario = QStringLiteral("stationary");
            submit(points.first());
            QTimer::singleShot(1700, &app, [&]() {
                scenario = QStringLiteral("after_refinement");
                lastDisplay = 0;
                sample = 0;
                moves.start();
            });
            return;
        } else {
            ++round;
            if (round == 10) {
                app.quit();
                return;
            }
            beginRound();
            return;
        }
        submit(scenario == QStringLiteral("new_branch") ? points.last() : points.first());
    };
    QObject::connect(&moves, &QTimer::timeout, &app, [&]() {
        if (++sample > 200) {
            moves.stop();
            stopping = true;
            if (!active) {
                stopping = false;
                QTimer::singleShot(0, &app, nextStage);
            }
            return;
        }
        submit(points.at(sample % points.size()));
    });
#ifdef SNOW_UIA_LEGACY_BENCHMARK
    QObject::connect(
        &coordinator, &ScreenshotSelectorCoordinator::hitTestFinished, &app,
#else
    QObject::connect(
        &coordinator, &ScreenshotSelectorCoordinator::initialResultReady, &app,
#endif
        [&](bool ok, const QVector<QRectF>& rects) {
            if (!ok) {
                app.exit(4);
                return;
            }
            ++received;
            const bool changed =
                model.applyCanvasHitPath(rects, QRectF(-100000, -100000, 200000, 200000), 1);
            print("initial", rects, changed);
            active = false;
            if (pending) {
                active = true;
                activeAt = pendingAt;
                pending = false;
            }
#ifdef SNOW_UIA_LEGACY_BENCHMARK
            coordinator.startNextHitTest();
#endif
            if (scenario == QStringLiteral("cold") || scenario == QStringLiteral("warm") ||
                scenario == QStringLiteral("new_branch") || stopping) {
                if (!active) {
                    stopping = false;
                    QTimer::singleShot(0, &app, nextStage);
                }
            }
        });
#ifndef SNOW_UIA_LEGACY_BENCHMARK
    QObject::connect(&coordinator, &ScreenshotSelectorCoordinator::refinementReady, &app,
                     [&](const QVector<QRectF>& rects) {
                         const bool changed = model.applyCanvasRefinementPath(
                             rects, QRectF(-100000, -100000, 200000, 200000), 1);
                         print("refinement", rects, changed);
                     });
#endif
    QObject::connect(&coordinator, &ScreenshotSelectorCoordinator::refreshFinished, &app,
                     [&](bool ok) {
                         if (!ok) {
                             app.exit(5);
                             return;
                         }
                         submit(points.first());
                     });
    beginRound = [&]() {
        scenario = QStringLiteral("cold");
        active = pending = false;
        model.beginCaptureSession(true);
        if (!coordinator.startRefresh({}))
            app.exit(6);
    };
    std::puts("scenario,round,sample,phase,movement_to_result_ms,display_gap_ms,depth,applied");
    QTimer::singleShot(0, &app, beginRound);
    QTimer::singleShot(90000, &app, [&]() { app.exit(7); });
    const int result = app.exec();
    coordinator.releaseCache();
    if (native)
        DestroyWindow(native);
    return result;
}
