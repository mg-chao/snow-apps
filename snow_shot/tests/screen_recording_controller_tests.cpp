#include "snow_shot/presentation/screenrecordingcontroller.h"
#include "snow_shot/presentation/screenrecordingtoolbarwindow.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_capture.h"

#include <QApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <atomic>
#include <cstdlib>
#include <iostream>

struct SnowCaptureRecordingSessionImpl {};
namespace {
SnowCaptureRecordingSession session;
int starts = 0;
std::atomic<int> exports = 0;
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
ScreenshotToolPalette* palette() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* toolbar = qobject_cast<ScreenRecordingToolbarWindow*>(widget);
            toolbar != nullptr && toolbar->isVisible()) {
            return toolbar->palette();
        }
    }
    require(false, "recording toolbar must exist");
    return nullptr;
}
} // namespace

extern "C" {
SnowCaptureRecordingSession*
snow_capture_recording_session_create(const SnowCaptureRecordingConfig*) {
    return &session;
}
void snow_capture_recording_session_destroy(SnowCaptureRecordingSession*) {}
uint8_t snow_capture_recording_session_start(SnowCaptureRecordingSession*) {
    ++starts;
    return 1;
}
uint8_t snow_capture_recording_session_pause(SnowCaptureRecordingSession*) {
    return 1;
}
uint8_t snow_capture_recording_session_resume(SnowCaptureRecordingSession*) {
    return 1;
}
uint8_t snow_capture_recording_session_stop_and_export(SnowCaptureRecordingSession*,
                                                       const SnowCaptureRecordingExportConfig*) {
    ++exports;
    return 1;
}
const char* snow_capture_last_error_message() {
    return "test backend";
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary storage must exist");
    using namespace snow_shot::storage;
    require(ApplicationStorage::instance()
                .initialize({temporary.filePath("bin"), temporary.filePath("settings"), 0})
                .success,
            "isolated storage must initialize");
    require(RecordingSettings().setVideoSaveDirectory(temporary.path()),
            "test output directory must be set");
    require(RecordingSettings().setHideToolbarInRecording(false),
            "capture exclusion must be disabled for fake backend");
    {
        ScreenRecordingController controller;
        controller.startRecording();
        controller.open(QRect(40, 40, 320, 240));
        QCoreApplication::processEvents();
        require(starts == 0, "a start requested while closed must not affect a later window");
        controller.startRecording();
        palette()->recordingCloseRequested();
        controller.open(QRect(80, 80, 320, 240));
        QCoreApplication::processEvents();
        require(starts == 0 && !controller.isRecording(),
                "closing a queued start must not start a later recording window");
        for (int iteration = 0; iteration < 8; ++iteration) {
            controller.startRecording();
            palette()->recordingCloseRequested();
            controller.open(QRect(80, 80, 320, 240));
        }
        int toolbarCount = 0;
        for (auto* widget : QApplication::topLevelWidgets()) {
            toolbarCount += qobject_cast<ScreenRecordingToolbarWindow*>(widget) != nullptr;
        }
        require(toolbarCount == 1, "reopening must reuse the controller's existing toolbar");
        controller.startRecording();
        controller.open(QRect(120, 80, 320, 240));
        QCoreApplication::processEvents();
        require(starts == 0, "replacing the selected region must invalidate pending starts");
        controller.startRecording();
        palette()->recordingCloseRequested();
        controller.open(QRect(80, 80, 320, 240));
        controller.startRecording();
        controller.startRecording();
        QCoreApplication::processEvents();
        require(starts == 1 && controller.isRecording(), "a fresh request must start exactly once");
        palette()->recordingPauseRequested();
        require(controller.isRecording(),
                "paused recording must remain stoppable by the global toggle");
        controller.stopRecordingAndCopyVideo();
        controller.stopRecordingAndCopyVideo();
        controller.startRecording();
        // Do not pump the UI export-completion timer: this test must not alter
        // the desktop clipboard. Destruction joins the fake export worker.
        QElapsedTimer elapsed;
        elapsed.start();
        while (exports.load() == 0 && elapsed.elapsed() < 2000) {
            QThread::msleep(1);
        }
        require(exports.load() == 1 && starts == 1,
                "repeated shortcut calls during export must not duplicate stop or restart");
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    {
        ScreenRecordingController controller;
        controller.open(QRect(40, 40, 320, 240));
        controller.startRecording();
    }
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(starts == 1, "destroying the controller must cancel a queued recording start");
    ApplicationStorage::instance().shutdown();
    return 0;
}
