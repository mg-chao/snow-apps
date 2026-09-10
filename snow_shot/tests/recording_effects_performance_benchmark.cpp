#include "recording_effects_performance_benchmark.h"
#include "../src/presentation/recording/recordingeffectpreview.h"
#include "snow_shot/presentation/screenrecordingareawindow.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include <QLabel>
#include <QPaintEvent>
#include <QScreen>
#include <QTimer>
#include <QPointer>
#include <qt_windows.h>
#include <dwmapi.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool success, const char* error) {
    if (!success) {
        throw std::runtime_error(error);
    }
}
void processFor(int milliseconds) {
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(milliseconds);
    loop.exec();
}
double percentile(std::vector<double> values, double fraction) {
    require(!values.empty(), "benchmark did not receive canvas paints");
    std::sort(values.begin(), values.end());
    return values.at(static_cast<size_t>((values.size() - 1) * fraction));
}
void key(bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = 'A';
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    require(SendInput(1, &input, sizeof(INPUT)) == 1, "benchmark key injection failed");
}
struct RestoreInput {
    POINT point{};
    HWND foreground = GetForegroundWindow();
    RestoreInput() {
        GetCursorPos(&point);
    }
    ~RestoreInput() {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = 'A';
        input.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &input, sizeof(INPUT));
        SetCursorPos(point.x, point.y);
        if (foreground != nullptr) {
            SetForegroundWindow(foreground);
        }
    }
};
} // namespace

int runRecordingEffectsPerformanceBenchmark(RecordingEffectsBenchmarkApplication& app) {
    RestoreInput restore;
    try {
        const QRect screen = ScreenshotGeometryMapper::physicalRectForScreen(*app.primaryScreen());
        for (const QSize output : {QSize(1920, 1080), QSize(3840, 2160)}) {
            ScreenRecordingAreaWindow area;
            const QRect capture(screen.topLeft() + QPoint(20, 20), output);
            area.setPhysicalRegion(capture);
            area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
            area.show();
            area.activateWindow();
            area.canvas()->setFocus();
            RecordingEffectPreview preview(area);
            preview.configure(capture, output, Qt::transparent, Qt::transparent, true);
            preview.setEligible(true);
            processFor(250);
            auto* label =
                area.findChild<QLabel*>(QStringLiteral("screenRecordingMotionPreviewLabel"));
            require(label && label->isVisible(), "native benchmark observer failed to initialize");
            size_t paints = 0;
            quint64 maxDirty = 0;
            std::vector<double> paintTimes;
            QPointer<QEventLoop> pendingPaint;
            app.afterEvent = [&](QObject* receiver, QEvent* event, qint64 nanoseconds) {
                if (receiver == area.canvas() && event->type() == QEvent::Paint) {
                    ++paints;
                    paintTimes.push_back(nanoseconds / 1000000.0);
                    quint64 pixels = 0;
                    for (const QRect& rect : static_cast<QPaintEvent*>(event)->region()) {
                        pixels += static_cast<quint64>(rect.width()) * rect.height();
                    }
                    maxDirty = qMax(maxDirty, pixels);
                    if (pendingPaint) {
                        pendingPaint->quit();
                    }
                }
            };
            processFor(250);
            require(paints == 0,
                    "an idle preview or static readout caused periodic canvas repaints");
            std::vector<double> latency;
            for (int sample = 0; sample < 40; ++sample) {
                const size_t previous = paints;
                QElapsedTimer elapsed;
                elapsed.start();
                QEventLoop untilPaint;
                QTimer timeout;
                timeout.setSingleShot(true);
                QObject::connect(&timeout, &QTimer::timeout, &untilPaint, &QEventLoop::quit);
                pendingPaint = &untilPaint;
                key(true);
                timeout.start(2000);
                untilPaint.exec();
                pendingPaint = nullptr;
                require(paints > previous, "native input did not reach the canvas");
                static_cast<void>(DwmFlush());
                latency.push_back(elapsed.nsecsElapsed() / 1000000.0);
                key(false);
                // Finish the 120 ms keyboard row motion before measuring the next input.
                processFor(180);
            }
            preview.configure(capture, output, QColor(255, 40, 60, 180), QColor(40, 180, 255, 160),
                              true);
            processFor(250);
            paintTimes.clear();
            maxDirty = 0;
            const QRect inputBounds = capture.intersected(screen).adjusted(20, 20, -20, -20);
            for (int sample = 0; sample < 300; ++sample) {
                SetCursorPos(inputBounds.left() + (sample * 13) % inputBounds.width(),
                             inputBounds.top() + (sample * 7) % inputBounds.height());
                if (sample % 30 == 0) {
                    key(true);
                    key(false);
                    INPUT clicks[2]{};
                    clicks[0].type = clicks[1].type = INPUT_MOUSE;
                    clicks[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                    clicks[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
                    require(SendInput(2, clicks, sizeof(INPUT)) == 2,
                            "benchmark click injection failed");
                }
                processFor(8);
            }
            std::cout << output.width() << 'x' << output.height()
                      << " canvas=" << area.canvas()->width() << 'x' << area.canvas()->height()
                      << " idle_paints=0 Qt_paint_p50_ms=" << percentile(paintTimes, .5)
                      << " Qt_paint_p95_ms=" << percentile(paintTimes, .95)
                      << " input_to_compositor_p50_ms=" << percentile(latency, .5)
                      << " input_to_compositor_p95_ms=" << percentile(latency, .95)
                      << " max_dirty_logical_pixels=" << maxDirty << '\n';
            require(percentile(paintTimes, .95) < 4, "Qt preview painting exceeded 4 ms p95");
            app.afterEvent = {};
            preview.stopAndClear(true);
            area.hide();
        }
        return 0;
    } catch (const std::exception& error) {
        app.afterEvent = {};
        std::cerr << error.what() << '\n';
        return 1;
    }
}
