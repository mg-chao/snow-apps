#include "snow_shot/presentation/directcaptureworkflow.h"

#include <QCoreApplication>

#include <cstdlib>
#include <iostream>

using namespace snow_shot::presentation;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

DirectCaptureFrame frame() {
    QImage image(7, 5, QImage::Format_RGB32);
    image.fill(qRgb(12, 34, 56));
    return {image, QRect(-1900, -200, 7, 5), QStringLiteral("target"), 1, {}};
}

struct Fixture {
    std::function<void(DirectCaptureFrame)> acquired;
    std::function<void(QString, QString)> saved;
    DirectCapturePorts::Completion copied;
    DirectCapturePorts::Completion recorded;
    QVector<DirectCaptureRequest> requests;
    QVector<DirectCaptureRequest> saveRequests;
    QVector<DirectCaptureRequest> copyRequests;
    QStringList events;
    QImage output;
    QString copiedPath;
    bool acceptAcquire = true;
    bool acceptSave = true;
    bool acceptCopy = true;
    bool acceptHistory = true;
    bool stopOnCaptureRequested = false;
    bool stopOnReport = false;
    int completed = 0;
    DirectCaptureWorkflow workflow{DirectCapturePorts{
        [this](const DirectCaptureRequest& request, auto done) {
            requests.push_back(request);
            events << "acquire";
            acquired = std::move(done);
            return acceptAcquire;
        },
        [this](const DirectCaptureRequest& request, const auto& result, auto done) {
            events << "save";
            saveRequests.push_back(request);
            output = result.image;
            saved = std::move(done);
            return acceptSave;
        },
        [this](const auto& request, const auto& result, const auto& path, auto done) {
            events << "copy";
            copyRequests.push_back(request);
            output = result.image;
            copiedPath = path;
            copied = std::move(done);
            return acceptCopy;
        },
        [this](const auto&, const auto& result, auto done) {
            events << "history";
            output = result.image;
            recorded = std::move(done);
            return acceptHistory;
        },
        [this](const QString&, bool warning) {
            events << (warning ? "warning" : "error");
            if (stopOnReport)
                workflow.shutdown();
        },
        [this]() {
            events << "shutter";
            if (stopOnCaptureRequested)
                workflow.shutdown();
        },
        [this]() { ++completed; },
    }};
};

void shutterPlaysImmediatelyForEveryRequest() {
    for (auto target : {DirectCaptureTarget::FocusedWindow, DirectCaptureTarget::CurrentMonitor}) {
        Fixture f;
        DirectCaptureRequest request;
        request.target = target;
        f.workflow.enqueue(request);
        require(f.events == QStringList({"shutter", "acquire"}),
                "shutter waited for image acquisition instead of acknowledging the request");
        f.workflow.enqueue(request);
        require(f.events == QStringList({"shutter", "acquire", "shutter"}),
                "queued capture delayed its shutter notification");
        f.acquired(frame());
        f.copied({});
        QCoreApplication::processEvents();
        f.acquired({});
        require(f.events.count("shutter") == 2 && f.workflow.pendingCount() == 0,
                "capture completion or failure repeated the shutter notification");
        f.workflow.shutdown();
        f.workflow.enqueue(request);
        require(f.events.count("shutter") == 2, "stopped workflow played a shutter sound");
    }
}

void shutterPreferenceAppliesToEachRequest() {
    for (auto target : {DirectCaptureTarget::FocusedWindow, DirectCaptureTarget::CurrentMonitor}) {
        Fixture f;
        DirectCaptureRequest request;
        request.target = target;
        request.shutterSoundNotification = false;
        f.workflow.enqueue(request);
        f.workflow.enqueue(request);
        require(f.events == QStringList({"acquire"}),
                "disabled shutter notification played for an active or queued capture");
        request.shutterSoundNotification = true;
        f.workflow.enqueue(request);
        require(f.events == QStringList({"acquire", "shutter"}),
                "reenabling shutter notification did not affect the next request immediately");
        for (int i = 0; i < 3; ++i) {
            f.acquired(frame());
            f.copied({});
            QCoreApplication::processEvents();
        }
        require(f.events.count("shutter") == 1 && f.events.count("copy") == 3 &&
                    f.workflow.pendingCount() == 0,
                "shutter preference changed capture output or repeated a queued notification");
    }
}

void outputsKeepRawPixelsAndProcessEveryRequest() {
    Fixture f;
    DirectCaptureRequest first;
    first.target = DirectCaptureTarget::FocusedWindow;
    first.window = 1234;
    first.autoSave = true;
    first.historyEnabled = true;
    first.encoding.quality = 35;
    first.encoding.compressionLevel = ScreenshotCompressionLevel::High;
    first.filenameFormat = QStringLiteral("first");
    f.workflow.enqueue(first);
    DirectCaptureRequest second;
    second.monitorName = QStringLiteral("second-monitor");
    f.workflow.enqueue(second);
    require(f.requests.size() == 1 && f.requests.front().window == 1234,
            "FIFO target snapshot lost");
    f.acquired(frame());
    require(f.output == frame().image, "capture pixels were styled or resampled");
    require(f.events.back() == "copy" && !f.saved,
            "automatic saving delayed clipboard publication");
    require(f.copiedPath.isEmpty(), "auto save incorrectly selected file clipboard mode");
    f.copied({});
    require(f.saveRequests.size() == 1 && !f.saveRequests.front().copyFile &&
                f.saveRequests.front().encoding.quality == 35 &&
                f.saveRequests.front().encoding.compressionLevel ==
                    ScreenshotCompressionLevel::High,
            "auto-save-only capture lost its image encoding settings");
    f.saved(QStringLiteral("saved.png"), {});
    require(f.workflow.pendingCount() == 2, "next capture began before history completion");
    f.recorded({});
    QCoreApplication::processEvents();
    require(f.requests.size() == 2 && f.requests.back().monitorName == second.monitorName,
            "mixed direct captures were dropped or reordered");
    f.acquired(frame());
    f.copied({});
    require(f.workflow.pendingCount() == 0 && f.completed == 2,
            "successful queue did not drain or release each export");
    require(f.events == QStringList({"shutter", "acquire", "shutter", "copy", "save", "history",
                                     "acquire", "copy"}),
            "output ordering changed");
}

void queuedCopiesRetainCaptureTargets() {
    for (bool autoSave : {false, true}) {
        Fixture f;
        const DirectCaptureTarget targets[] = {
            DirectCaptureTarget::FocusedWindow, DirectCaptureTarget::CurrentMonitor,
            DirectCaptureTarget::FocusedWindow, DirectCaptureTarget::CurrentMonitor};
        for (auto target : targets) {
            DirectCaptureRequest request;
            request.target = target;
            request.autoSave = autoSave;
            f.workflow.enqueue(request);
            request.target = DirectCaptureTarget::FocusedWindow;
        }
        for (auto target : targets) {
            QCoreApplication::processEvents();
            f.acquired(frame());
            require(f.copyRequests.back().target == target &&
                        f.copyRequests.back().autoSave == autoSave,
                    "clipboard copy lost the active request's capture target or save setting");
            require(f.copiedPath.isEmpty(), "automatic saving changed bitmap copy to file copy");
            f.copied({});
            if (autoSave)
                f.saved(QStringLiteral("saved.png"), {});
        }
        require(f.copyRequests.size() == 4 && f.workflow.pendingCount() == 0,
                "queued clipboard copies were dropped or left pending");
    }
}

void clipboardFailureStillAttemptsAutomaticSave() {
    for (bool accepted : {false, true}) {
        for (bool saveSucceeds : {false, true}) {
            Fixture f;
            f.acceptCopy = accepted;
            DirectCaptureRequest request;
            request.autoSave = true;
            request.historyEnabled = true;
            f.workflow.enqueue(request);
            f.workflow.enqueue({});
            f.acquired(frame());
            if (accepted)
                f.copied(QStringLiteral("clipboard failed"));
            require(f.events.back() == "save" && f.workflow.pendingCount() == 2,
                    "clipboard failure skipped automatic saving");
            f.saved(saveSucceeds ? QStringLiteral("saved.png") : QString(),
                    saveSucceeds ? QString() : QStringLiteral("save failed"));
            require(!f.events.contains("history") && f.completed == 1,
                    "failed clipboard copy published history or did not finish");
            QCoreApplication::processEvents();
            require(f.requests.size() == 2, "clipboard failure blocked the next capture");
            f.workflow.shutdown();
        }
    }
}

void failuresDoNotBlockLaterRequests() {
    for (bool file : {false, true}) {
        Fixture f;
        DirectCaptureRequest request;
        request.autoSave = true;
        request.copyFile = file;
        request.historyEnabled = true;
        f.workflow.enqueue(request);
        f.workflow.enqueue({});
        f.acquired(frame());
        if (!file) {
            require(f.events.back() == "copy" && !f.saved,
                    "image copy waited for automatic saving");
            f.copied({});
            f.saved({}, QStringLiteral("save failed"));
            require(f.events.contains("warning") && f.events.back() == "history",
                    "auto save failure discarded a successful copy's history");
            f.recorded({});
        } else {
            f.saved({}, QStringLiteral("save failed"));
            require(!f.events.contains("copy") && !f.events.contains("history"),
                    "failed file save published clipboard or history");
        }
        QCoreApplication::processEvents();
        require(f.requests.size() == 2, "failure stalled the FIFO");
        f.acquired({});
        require(f.workflow.pendingCount() == 0, "invalid capture did not finish");
    }
}

void fileCopyHistoryFailureAndLateCallbacks() {
    Fixture f;
    DirectCaptureRequest request;
    request.copyFile = true;
    request.historyEnabled = true;
    f.workflow.enqueue(request);
    auto oldAcquired = f.acquired;
    f.acquired(frame());
    f.saved(QStringLiteral("image.png"), {});
    require(f.copiedPath == QStringLiteral("image.png"), "file mode lost saved path");
    f.copied({});
    auto oldRecorded = f.recorded;
    f.recorded(QStringLiteral("history failed"));
    oldRecorded({});
    oldAcquired(frame());
    require(f.workflow.pendingCount() == 0 && f.events.count("copy") == 1,
            "duplicate callbacks repeated output");
    f.workflow.enqueue({});
    auto late = f.acquired;
    f.workflow.shutdown();
    late(frame());
    f.workflow.enqueue({});
    require(f.workflow.pendingCount() == 0 && f.events.count("copy") == 1,
            "shutdown allowed late output");
}

void rejectedOperationsFinishAndDestroyedReceiversIgnoreResults() {
    for (int stage = 0; stage < 4; ++stage) {
        Fixture f;
        f.acceptAcquire = stage != 0;
        f.acceptSave = stage != 1;
        f.acceptCopy = stage != 2;
        f.acceptHistory = stage != 3;
        DirectCaptureRequest request;
        request.copyFile = true;
        request.historyEnabled = true;
        f.workflow.enqueue(request);
        if (stage > 0)
            f.acquired(frame());
        if (stage > 1)
            f.saved(QStringLiteral("file.png"), {});
        if (stage > 2)
            f.copied({});
        require(f.workflow.pendingCount() == 0, "rejected operation left the request stuck");
    }
    std::function<void(DirectCaptureFrame)> late;
    {
        Fixture f;
        f.workflow.enqueue({});
        late = f.acquired;
    }
    late(frame());
}

void queuedRequestsRetainTargetsAndOutputSettings() {
    Fixture f;
    DirectCaptureRequest request;
    request.target = DirectCaptureTarget::FocusedWindow;
    request.window = 8765;
    request.requestedAt = QDateTime::fromMSecsSinceEpoch(123456).toUTC();
    request.autoSave = true;
    request.copyFile = true;
    request.historyEnabled = true;
    request.directories = {QStringLiteral("original-directory")};
    request.imageFormat = QStringLiteral("pdf");
    request.encoding.quality = 42;
    request.encoding.compressionLevel = ScreenshotCompressionLevel::High;
    request.pdf.pageSize = ScreenshotPdfPageSize::LandscapeA4;
    request.filenameFormat = QStringLiteral("original-filename");
    f.workflow.enqueue({});
    for (int i = 0; i < 12; ++i) {
        request.window = static_cast<quintptr>(8765 + i);
        f.workflow.enqueue(request);
    }
    request = {};
    require(f.requests.size() == 1, "queued requests acquired pixels before reaching the head");
    f.acquired(frame());
    f.copied({});
    for (int i = 0; i < 12; ++i) {
        QCoreApplication::processEvents();
        const auto& queued = f.requests.back();
        require(queued.target == DirectCaptureTarget::FocusedWindow &&
                    queued.window == static_cast<quintptr>(8765 + i) && queued.autoSave &&
                    queued.copyFile && queued.historyEnabled &&
                    queued.requestedAt.toMSecsSinceEpoch() == 123456 &&
                    queued.directories == QStringList{QStringLiteral("original-directory")} &&
                    queued.imageFormat == QStringLiteral("pdf") && queued.encoding.quality == 42 &&
                    queued.encoding.compressionLevel == ScreenshotCompressionLevel::High &&
                    queued.pdf.pageSize == ScreenshotPdfPageSize::LandscapeA4 &&
                    queued.pdf.quality == 100 &&
                    queued.filenameFormat == QStringLiteral("original-filename"),
                "queued target or settings changed after invocation");
        f.acquired(frame());
        f.saved(QStringLiteral("saved.png"), {});
        f.copied({});
        f.recorded({});
    }
    require(f.requests.size() == 13 && f.workflow.pendingCount() == 0,
            "burst requests were coalesced or dropped");
}

void shutdownDuringNotificationsStopsOutputs() {
    {
        Fixture f;
        f.stopOnCaptureRequested = true;
        f.workflow.enqueue({});
        require(f.workflow.pendingCount() == 0 && f.events == QStringList({"shutter"}),
                "request notification shutdown allowed acquisition or output");
    }
    for (int stage = 0; stage < 4; ++stage) {
        Fixture f;
        f.stopOnReport = true;
        DirectCaptureRequest request;
        request.autoSave = true;
        request.historyEnabled = true;
        f.workflow.enqueue(request);
        f.workflow.enqueue({});
        if (stage == 0)
            f.acquired({});
        else {
            f.acquired(frame());
            f.copied(stage == 1 ? QStringLiteral("copy failed") : QString());
            if (stage > 1)
                f.saved(stage == 2 ? QString() : QStringLiteral("saved.png"),
                        stage == 2 ? QStringLiteral("save failed") : QString());
            if (stage == 3)
                f.recorded(QStringLiteral("history failed"));
        }
        QCoreApplication::processEvents();
        require(f.workflow.pendingCount() == 0 && f.requests.size() == 1 &&
                    (stage != 1 || !f.events.contains("save")),
                "error notification shutdown allowed further output or acquisition");
    }
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    shutterPlaysImmediatelyForEveryRequest();
    shutterPreferenceAppliesToEachRequest();
    outputsKeepRawPixelsAndProcessEveryRequest();
    queuedCopiesRetainCaptureTargets();
    failuresDoNotBlockLaterRequests();
    clipboardFailureStillAttemptsAutomaticSave();
    fileCopyHistoryFailureAndLateCallbacks();
    rejectedOperationsFinishAndDestroyedReceiversIgnoreResults();
    queuedRequestsRetainTargetsAndOutputSettings();
    shutdownDuringNotificationsStopsOutputs();
    return 0;
}
