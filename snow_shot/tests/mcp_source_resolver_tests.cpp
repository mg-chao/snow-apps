// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/app/mcp/mcpsourceresolver.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_shot/storage/capturehistoryrepository.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QSemaphore>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace snow_shot::app::mcp;
int runMcpSourceResolverTests(int argc, char** argv);

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
bool equalPixels(const QImage& left, const QImage& right) {
    if (left.size() != right.size())
        return false;
    const auto first = left.convertToFormat(QImage::Format_RGBA8888);
    const auto second = right.convertToFormat(QImage::Format_RGBA8888);
    for (int row = 0; row < first.height(); ++row)
        if (std::memcmp(first.constScanLine(row), second.constScanLine(row),
                        static_cast<std::size_t>(first.width()) * 4) != 0)
            return false;
    return true;
}
template <typename Predicate> void await(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(predicate(), "source transition completes within fixture deadline");
}
ScreenshotMcpRequest request(QString source, QString id, quint64 owner = 1) {
    ScreenshotMcpRequest result;
    result.connectionId = owner;
    result.requestId = std::move(id);
    result.params.insert(QStringLiteral("source"), std::move(source));
    result.params.insert(QStringLiteral("source_id"), QStringLiteral("pin"));
    return result;
}
snow_shot::storage::PinnedWindowRecord pin() {
    snow_shot::storage::PinnedWindowRecord result;
    result.id = QStringLiteral("pin");
    result.image = QImage(4, 3, QImage::Format_RGBA8888);
    result.image.fill(Qt::red);
    result.canvasSourceRect = QRectF(0, 0, 4, 3);
    result.contentCanvasRect = QRectF(10, 20, 3, 4);
    result.surfaceCanvasRect = QRectF(5, 15, 20, 20);
    result.imageTransform.rotate(90);
    result.canvasSession = QByteArrayLiteral("owned canvas session");
    result.originalText = QStringLiteral("original pin text");
    ScreenshotResultStyle style;
    style.borderType = ScreenshotSelectionBorderType::Border;
    style.borderWidth = 2;
    style.borderColor = Qt::blue;
    result.resultStyle = encodeScreenshotResultStyle(style);
    return result;
}
void captureTransitions() {
    int started = 0;
    int canceled = 0;
    int delivered = 0;
    QString finalAdmission;
    QList<McpSourceResolver::CaptureCompletion> callbacks;
    QList<qint64> reservations;
    McpSourceResolver::Budget retainedBudget;
    McpSourceResolver::Ports ports;
    ports.captureAllowed = [&] { return finalAdmission; };
    ports.capture = [&](const QJsonObject&, auto completion, auto budget) {
        ++started;
        callbacks.append(std::move(completion));
        retainedBudget = std::move(budget);
        return true;
    };
    ports.cancelCapture = [&] { ++canceled; };
    McpSourceResolver resolver(std::move(ports));
    auto completion = [&](auto, const QString&) { ++delivered; };
    auto budget = [&](qint64 bytes) {
        reservations.append(bytes);
        return true;
    };
    // A zero timer was admitted before suspension but must recheck at execution.
    resolver.resolve(request(QStringLiteral("capture"), QStringLiteral("delayed")), completion,
                     budget);
    resolver.setSuspended(true);
    await([&] { return delivered == 1; });
    require(started == 0, "suspension closes final delayed capture admission");
    resolver.resolve(request(QStringLiteral("text"), QStringLiteral("new")), completion, budget);
    require(delivered == 2 && resolver.workerCount() == 0,
            "suspension also rejects newly arriving source work");
    resolver.setSuspended(false);
    finalAdmission = QStringLiteral("permission_required");
    resolver.resolve(request(QStringLiteral("capture"), QStringLiteral("revoked")), completion,
                     budget);
    await([&] { return delivered == 3; });
    require(started == 0, "delayed capture checks fresh permission and busy facts");
    finalAdmission.clear();
    resolver.resolve(request(QStringLiteral("capture"), QStringLiteral("reused")), completion,
                     budget);
    await([&] { return started == 1; });
    require(retainedBudget(200) && retainedBudget(50) && retainedBudget(300) &&
                reservations == QList<qint64>{200, 200, 300},
            "source reservation never shrinks across native fallback allocations");
    require(resolver.cancel(1, QStringLiteral("reused")) && canceled == 1 && !retainedBudget(400),
            "cancel requests native stop and forbids later source allocations");
    resolver.resolve(request(QStringLiteral("capture"), QStringLiteral("reused")), completion,
                     budget);
    await([&] { return started == 2; });
    callbacks.at(0)({}, {}, {});
    // Reusing an ID must not let its old callback clear the newer capture lease.
    resolver.resolve(request(QStringLiteral("capture"), QStringLiteral("competing")), completion,
                     budget);
    await([&] { return delivered == 4; });
    require(started == 2, "old canceled result cannot retire newer capture with the same ID");
    resolver.disconnected(1);
    callbacks.at(1)({}, {}, {});
    require(canceled == 2 && delivered == 4 && !resolver.hasPendingSources(),
            "disconnect cancels owned capture and suppresses late delivery");
    resolver.resolve(request(QStringLiteral("capture"), QStringLiteral("timer")), completion,
                     budget);
    require(resolver.cancel(1, QStringLiteral("timer")), "delayed capture can be canceled");
    QCoreApplication::processEvents();
    require(started == 2, "canceled timer never reaches native capture");
    resolver.stopAndDrain();
    resolver.resolve(request(QStringLiteral("capture"), QStringLiteral("after-stop")), completion,
                     budget);
    require(delivered == 5 && started == 2, "terminal stop keeps source admission closed");
}
void sourceFidelity() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "isolated source repository");
    auto history = snow_shot::storage::makeCaptureHistoryRepository(temporary.path());
    snow_shot::storage::CaptureHistoryDraft draft;
    draft.id = QStringLiteral("fd97d4e3-311c-48de-bbfb-3b4f43b0fef6");
    draft.createdUtc = QDateTime::currentDateTimeUtc();
    draft.canvasBounds = QRect(10, 20, 4, 3);
    draft.selection.rectangle = draft.canvasBounds;
    draft.selection.shadowColor = Qt::black;
    draft.canvasHistory = QByteArrayLiteral("{\"history\":\"document\"}");
    QImage image(4, 3, QImage::Format_RGBA8888);
    image.fill(Qt::green);
    snow_shot::storage::CaptureHistoryDisplayDraft display;
    display.stableId = QStringLiteral("display");
    display.name = display.stableId;
    display.image = image;
    display.sourceCanvasRect = draft.canvasBounds;
    draft.displays.append(display);
    draft.resultImage = image;
    require(history->publish(std::move(draft)).get().storage.success,
            "publish isolated history source");
    int pinReads = 0;
    int snapshots = 0;
    bool useLivePin = false;
    const auto* gui = QThread::currentThread();
    McpSourceResolver::Ports ports;
    ports.history = history.get();
    ports.clipboard = [&](auto budget) -> std::optional<ScreenshotClipboardContentSnapshot> {
        require(QThread::currentThread() == gui, "clipboard snapshot stays on GUI thread");
        require(budget(4 * 3 * 8), "clipboard reserves before detached raster snapshot");
        ScreenshotClipboardContentSnapshot snapshot;
        snapshot.detachedImage = image;
        return snapshot;
    };
    ports.livePin = [&](const QString&, auto budget) -> std::optional<McpSourceResolver::LivePin> {
        require(QThread::currentThread() == gui, "live pin snapshot stays on GUI thread");
        if (!useLivePin)
            return std::nullopt;
        if (!budget(4 * 3 * 8 + 64 * 1024 * 1024))
            return std::nullopt;
        ++snapshots;
        return McpSourceResolver::LivePin{pin(), {}, QStringLiteral("arrow")};
    };
    ports.loadPin = [&](const QString&,
                        auto budget) -> std::optional<snow_shot::storage::PinnedWindowRecord> {
        require(QThread::currentThread() != gui, "persisted pin payload read uses worker");
        ++pinReads;
        if (!budget(4 * 3 * 8))
            return std::nullopt;
        return pin();
    };
    McpSourceResolver resolver(std::move(ports));
    const auto resolve = [&](ScreenshotMcpRequest input) {
        std::optional<McpSourceResolver::Source> output;
        resolver.resolve(
            input,
            [&](auto source, const QString& error) {
                require(QThread::currentThread() == gui, "completion returns on owning GUI thread");
                require(error.isEmpty(), "valid source resolves without error");
                output = std::move(source);
            },
            [](qint64 bytes) { return bytes >= 0; });
        await([&] { return output.has_value(); });
        return std::move(*output);
    };
    const auto clipboard = resolve(request(QStringLiteral("clipboard"), QStringLiteral("clip")));
    require(clipboard.image == image, "clipboard resolves detached GUI snapshot pixels");
    auto textRequest = request(QStringLiteral("text"), QStringLiteral("text"));
    textRequest.params.insert(QStringLiteral("text"), QStringLiteral("source text"));
    const auto text = resolve(textRequest);
    require(!text.image.isNull() && text.originalContent.text == u"source text",
            "text keeps original content and worker raster");
    auto htmlRequest = request(QStringLiteral("html"), QStringLiteral("html"));
    htmlRequest.params.insert(QStringLiteral("text"), QStringLiteral("source html"));
    htmlRequest.params.insert(QStringLiteral("html"), QStringLiteral("<b>source html</b>"));
    const auto html = resolve(htmlRequest);
    require(!html.image.isNull() && html.originalContent.html == u"<b>source html</b>",
            "HTML keeps formatted original content");
    const auto persisted = resolve(request(QStringLiteral("pinned"), QStringLiteral("pin")));
    require(pinReads == 1 && persisted.image.size() == QSize(3, 4) &&
                persisted.documentSession == QByteArrayLiteral("owned canvas session") &&
                persisted.canvasBounds == QRectF(5, 15, 20, 20) &&
                persisted.images.front().canvasRect == QRectF(10, 20, 3, 4) &&
                persisted.selection && persisted.selection->borderWidth == 2 &&
                persisted.originalContent.text == u"original pin text",
            "pin transform, independent annotation session, bounds and selection survive import");
    useLivePin = true;
    const auto live = resolve(request(QStringLiteral("pinned"), QStringLiteral("live")));
    require(snapshots == 1 && pinReads == 1 && live.tool == u"arrow",
            "live pin owns GUI edit snapshot instead of reloading stale persisted payload");
    bool liveRejected = false;
    resolver.resolve(
        request(QStringLiteral("pinned"), QStringLiteral("live-budget")),
        [&](auto, const QString& error) { liveRejected = error == u"resource_limit"; },
        [](qint64) { return false; });
    require(liveRejected && snapshots == 1 && pinReads == 1 && resolver.workerCount() == 0,
            "live pin reservation rejects before allocating snapshot or falling back to disk");
    auto historyRequest = request(QStringLiteral("history"), QStringLiteral("history"));
    historyRequest.params.insert(QStringLiteral("source_id"),
                                 QStringLiteral("fd97d4e3-311c-48de-bbfb-3b4f43b0fef6"));
    const auto historic = resolve(historyRequest);
    require(historic.documentHistory == QByteArrayLiteral("{\"history\":\"document\"}") &&
                historic.images.size() == 1 && equalPixels(historic.images.front().image, image) &&
                historic.images.front().canvasRect == QRectF(10, 20, 4, 3),
            "history keeps document bytes, original display pixels and source coordinates");
    bool rejected = false;
    useLivePin = false;
    resolver.resolve(
        request(QStringLiteral("pinned"), QStringLiteral("budget")),
        [&](auto, const QString& error) { rejected = error == u"resource_limit"; },
        [](qint64) { return false; });
    await([&] { return rejected; });
    require(resolver.workerCount() == 0, "resource rejection releases completed worker lane");
}
void cancellationOccupancy() {
    QSemaphore entered;
    QSemaphore release;
    int delivered = 0;
    std::atomic_int finished = 0;
    McpSourceResolver::Ports ports;
    ports.loadPin = [&](const QString&,
                        auto) -> std::optional<snow_shot::storage::PinnedWindowRecord> {
        entered.release();
        release.acquire();
        ++finished;
        return pin();
    };
    McpSourceResolver resolver(std::move(ports));
    auto completion = [&](auto, const QString&) { ++delivered; };
    auto budget = [](qint64 bytes) { return bytes >= 0; };
    resolver.resolve(request(QStringLiteral("pinned"), QStringLiteral("first")), completion,
                     budget);
    resolver.resolve(request(QStringLiteral("pinned"), QStringLiteral("second"), 2), completion,
                     budget);
    require(entered.tryAcquire(2, 5000), "both bounded source workers start");
    bool full = false;
    resolver.resolve(
        request(QStringLiteral("pinned"), QStringLiteral("third")),
        [&](auto, const QString& error) { full = error == u"queue_full"; }, budget);
    require(full && resolver.workerCount() == 2, "third source cannot exceed two occupied lanes");
    require(resolver.cancel(1, QStringLiteral("first")), "cancel admitted source");
    resolver.disconnected(2);
    require(resolver.workerCount() == 2 && resolver.hasPendingSources(),
            "cancel and disconnect retain actual worker occupancy until completion");
    std::thread releaser([&] { release.release(2); });
    resolver.stopAndDrain();
    releaser.join();
    QCoreApplication::processEvents();
    require(finished == 2 && delivered == 0 && resolver.workerCount() == 0,
            "stop drains actual work and retires all canceled queued completions");
    McpSourceResolver::CaptureCompletion late;
    McpSourceResolver::Ports capturePorts;
    capturePorts.capture = [&](const QJsonObject&, auto completionCallback, auto) {
        late = std::move(completionCallback);
        return true;
    };
    auto dying = std::make_unique<McpSourceResolver>(std::move(capturePorts));
    dying->resolve(request(QStringLiteral("capture"), QStringLiteral("destroy")), completion,
                   budget);
    await([&] { return static_cast<bool>(late); });
    dying.reset();
    late({}, {}, {});
    require(delivered == 0, "destroyed resolver suppresses native result callback safely");
    McpSourceResolver::Ports throwingPorts;
    throwingPorts.clipboard = [](auto) -> std::optional<ScreenshotClipboardContentSnapshot> {
        throw std::runtime_error("snapshot failure");
    };
    throwingPorts.loadPin = [](const QString&,
                               auto) -> std::optional<snow_shot::storage::PinnedWindowRecord> {
        throw std::runtime_error("payload failure");
    };
    McpSourceResolver throwing(std::move(throwingPorts));
    throwing.resolve(request(QStringLiteral("clipboard"), QStringLiteral("throw-snapshot")),
                     completion, budget);
    require(delivered == 1 && throwing.workerCount() == 0,
            "snapshot exception completes and releases pre-submission lane");
    throwing.resolve(request(QStringLiteral("pinned"), QStringLiteral("throw-worker")), completion,
                     budget);
    await([&] { return delivered == 2; });
    require(throwing.workerCount() == 0, "worker exception releases lane and completes once");
}
void snapshotCancellationReleasesCapacity() {
    for (const auto& source : {QStringLiteral("clipboard"), QStringLiteral("pinned")}) {
        for (const bool disconnect : {false, true}) {
            for (const bool throws : {false, true}) {
                QSemaphore entered;
                QSemaphore release;
                McpSourceResolver* resolver = nullptr;
                QString activeRequest;
                bool cancelSnapshot = true;
                int canceledDeliveries = 0;
                int completed = 0;
                const auto interrupt = [&] {
                    require(resolver->workerCount() == 1,
                            "GUI snapshot reserves a source slot before invoking its provider");
                    if (disconnect)
                        resolver->disconnected(1);
                    else
                        require(resolver->cancel(1, activeRequest),
                                "snapshot can reentrantly cancel its own request");
                    if (throws)
                        throw std::runtime_error("snapshot failed after cancellation");
                };
                McpSourceResolver::Ports ports;
                ports.clipboard = [&](auto) -> std::optional<ScreenshotClipboardContentSnapshot> {
                    interrupt();
                    return std::nullopt;
                };
                ports.livePin = [&](const QString&,
                                    auto) -> std::optional<McpSourceResolver::LivePin> {
                    if (cancelSnapshot)
                        interrupt();
                    return std::nullopt;
                };
                ports.loadPin = [&](const QString&,
                                    auto) -> std::optional<snow_shot::storage::PinnedWindowRecord> {
                    entered.release();
                    release.acquire();
                    return pin();
                };
                McpSourceResolver instance(std::move(ports));
                resolver = &instance;
                const auto budget = [](qint64 bytes) { return bytes >= 0; };
                for (int index = 0; index < 2; ++index) {
                    activeRequest = QString::number(index);
                    instance.resolve(
                        request(source, activeRequest),
                        [&](auto, const QString&) { ++canceledDeliveries; }, budget);
                    require(
                        instance.workerCount() == 0 && !instance.hasPendingSources() &&
                            canceledDeliveries == 0 && entered.available() == 0,
                        "snapshot cancellation releases its slot without submission or delivery");
                }
                cancelSnapshot = false;
                for (int index = 0; index < 2; ++index)
                    instance.resolve(
                        request(QStringLiteral("pinned"), QString::number(index), 2),
                        [&](auto result, const QString& error) {
                            require(error.isEmpty() && !result.image.isNull(),
                                    "replacement source succeeds after snapshot cancellation");
                            ++completed;
                        },
                        budget);
                require(entered.tryAcquire(2, 5000) && instance.workerCount() == 2,
                        "both source slots remain reusable after repeated snapshot cancellation");
                release.release(2);
                await([&] { return completed == 2; });
                require(instance.workerCount() == 0 && !instance.hasPendingSources(),
                        "replacement workers each release their occupied slot once");
            }
        }
    }
}

void snapshotLifecycleTransitions() {
    enum class Transition { Suspend, Stop, Destroy };
    for (const auto& source : {QStringLiteral("clipboard"), QStringLiteral("pinned")}) {
        for (const auto transition : {Transition::Suspend, Transition::Stop, Transition::Destroy}) {
            std::unique_ptr<McpSourceResolver> resolver;
            int delivered = 0;
            std::atomic_int payloadReads = 0;
            const auto interrupt = [&] {
                require(resolver->workerCount() == 1, "snapshot occupies one source slot");
                if (transition == Transition::Suspend)
                    resolver->setSuspended(true);
                else if (transition == Transition::Stop) {
                    resolver->stopAndDrain();
                    require(resolver->workerCount() == 0 && !resolver->hasPendingSources(),
                            "reentrant stop retires the slot before snapshot returns");
                } else
                    resolver.reset();
            };
            McpSourceResolver::Ports ports;
            ports.clipboard = [&](auto) -> std::optional<ScreenshotClipboardContentSnapshot> {
                interrupt();
                return std::nullopt;
            };
            ports.livePin = [&](const QString&, auto) -> std::optional<McpSourceResolver::LivePin> {
                interrupt();
                return std::nullopt;
            };
            ports.loadPin = [&](const QString&,
                                auto) -> std::optional<snow_shot::storage::PinnedWindowRecord> {
                ++payloadReads;
                return pin();
            };
            resolver = std::make_unique<McpSourceResolver>(std::move(ports));
            resolver->resolve(
                request(source, QStringLiteral("snapshot-transition")),
                [&](auto, const QString& error) {
                    require(resolver && resolver->workerCount() == 0 && error == u"busy",
                            "suspension releases its slot before notifying completion");
                    ++delivered;
                },
                [](qint64 bytes) { return bytes >= 0; });
            require(payloadReads == 0 && delivered == (transition == Transition::Suspend ? 1 : 0),
                    "snapshot lifecycle transitions prevent payload submission and late delivery");
            if (resolver)
                require(resolver->workerCount() == 0 && !resolver->hasPendingSources(),
                        "snapshot unwind cannot leak or double-release a retired source slot");
            else
                require(transition == Transition::Destroy,
                        "only explicit destruction removes the resolver");
        }
    }
}

void reentrantAdmission() {
    int captures = 0;
    int delivered = 0;
    auto completion = [&](auto, const QString&) { ++delivered; };
    auto budget = [](qint64 bytes) { return bytes >= 0; };
    std::unique_ptr<McpSourceResolver> resolver;
    McpSourceResolver::Ports ports;
    ports.captureAllowed = [&] {
        resolver->setSuspended(true);
        return QString();
    };
    ports.capture = [&](const QJsonObject&, auto, auto) {
        ++captures;
        return true;
    };
    resolver = std::make_unique<McpSourceResolver>(std::move(ports));
    resolver->resolve(request(QStringLiteral("capture"), QStringLiteral("reentrant-pause")),
                      completion, budget);
    await([&] { return delivered == 1; });
    require(captures == 0, "permission observer suspension vetoes final native commitment");
    resolver.reset();
    McpSourceResolver::Ports deletingPorts;
    deletingPorts.captureAllowed = [&] {
        resolver.reset();
        return QString();
    };
    deletingPorts.capture = [&](const QJsonObject&, auto, auto) {
        ++captures;
        return true;
    };
    resolver = std::make_unique<McpSourceResolver>(std::move(deletingPorts));
    resolver->resolve(request(QStringLiteral("capture"), QStringLiteral("reentrant-delete")),
                      completion, budget);
    await([&] { return !resolver; });
    require(captures == 0 && delivered == 1,
            "permission observer can destroy resolver without native launch or late delivery");
    McpSourceResolver::Ports deletingSnapshotPorts;
    deletingSnapshotPorts.clipboard =
        [&](auto) -> std::optional<ScreenshotClipboardContentSnapshot> {
        resolver.reset();
        return std::nullopt;
    };
    resolver = std::make_unique<McpSourceResolver>(std::move(deletingSnapshotPorts));
    resolver->resolve(request(QStringLiteral("clipboard"), QStringLiteral("snapshot-delete")),
                      completion, budget);
    require(!resolver && delivered == 1,
            "GUI snapshot callback can destroy resolver before worker submission");
    const auto createDeletingCancellation = [&] {
        McpSourceResolver::Ports cancellationPorts;
        cancellationPorts.capture = [&](const QJsonObject&, auto, auto) {
            ++captures;
            return true;
        };
        cancellationPorts.cancelCapture = [&] { resolver.reset(); };
        return std::make_unique<McpSourceResolver>(std::move(cancellationPorts));
    };
    resolver = createDeletingCancellation();
    resolver->resolve(request(QStringLiteral("capture"), QStringLiteral("disconnect-delete")),
                      completion, budget);
    await([&] { return captures == 1; });
    resolver->disconnected(1);
    require(!resolver && delivered == 1,
            "native cancellation can destroy resolver during owner disconnect");
    resolver = createDeletingCancellation();
    resolver->resolve(request(QStringLiteral("capture"), QStringLiteral("stop-delete")), completion,
                      budget);
    await([&] { return captures == 2; });
    resolver->stopAndDrain();
    require(!resolver && delivered == 1,
            "native cancellation can destroy resolver during terminal stop");
}
} // namespace

// Test-build-only entry before ordinary bootstrap or native startup maintenance.
int runMcpSourceResolverTests(int argc, char** argv) {
    QStandardPaths::setTestModeEnabled(true);
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    captureTransitions();
    sourceFidelity();
    cancellationOccupancy();
    snapshotCancellationReleasesCapacity();
    snapshotLifecycleTransitions();
    reentrantAdmission();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    std::cout << "MCP source resolver tests passed\n";
    return 0;
}
