#include "snow_shot/presentation/screenshotprintservice.h"
#include "../src/presentation/services/nativeprintbackend.h"
#include "../src/presentation/services/screenshotprintinteractionguard.h"
#include "../src/presentation/services/screenshotprintcompletion.h"
#include "print_diagnostics_test_support.h"

#include <QApplication>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLineEdit>
#include <QSet>
#include <QThread>
#include <QWidget>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

namespace {
using Service = ScreenshotPrintService;
// Stub the platform factory so the shared service can be tested without native UI.
std::vector<bool> sharedBackendSelections;
std::vector<bool> sharedBackendStarts;
Service::Status sharedBackendStatus = Service::Status::Cancelled;
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void flush() {
    for (int i = 0; i < 5; ++i)
        QCoreApplication::processEvents();
}
void processUntil(const std::function<bool()>& condition) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(condition(), "timed out waiting for asynchronous print preparation");
}
QImage image() {
    QImage result(2, 1, QImage::Format_ARGB32);
    result.setPixelColor(0, 0, Qt::transparent);
    result.setPixelColor(1, 0, QColor(255, 0, 0, 128));
    result.setDevicePixelRatio(2);
    return result;
}
void immutableWhiteSnapshotAndDuplicateCompletion() {
    Service::Completion nativeCompletion;
    QImage received;
    int starts = 0;
    Service service([&](QWidget*, QImage snapshot, Service::Completion completion) {
        ++starts;
        received = std::move(snapshot);
        nativeCompletion = std::move(completion);
    });
    QWidget owner;
    QImage source = image();
    int completions = 0;
    require(service.printImage(&owner, &owner, source,
                               [&](Service::Result result) {
                                   require(result.status == Service::Status::Submitted,
                                           "submission status must be preserved");
                                   require(QThread::currentThread() == qApp->thread(),
                                           "completion must return to GUI thread");
                                   ++completions;
                               }),
            "first print must be accepted");
    source.fill(Qt::black);
    require(!service.printImage(&owner, &owner, source, [](auto) {}),
            "duplicate preparation or native interaction must be rejected");
    processUntil([&] { return starts == 1; });
    require(starts == 1 && received.size() == QSize(2, 1) && received.devicePixelRatio() == 1 &&
                received.pixelColor(0, 0) == QColor(Qt::white) &&
                received.pixelColor(1, 0) == QColor(255, 127, 127),
            "snapshot must be immutable, fully sized, and composited onto white");
    require(received.colorSpace() == QColorSpace(QColorSpace::SRgb), "native pixels must be sRGB");
    std::thread worker([&] { nativeCompletion({Service::Status::Submitted, {}}); });
    worker.join();
    nativeCompletion({Service::Status::Failed, QStringLiteral("late")});
    flush();
    require(completions == 1 && !service.busy(), "late native completions must be ignored");
}
void fallbackOnlyWhenUnavailable() {
    for (auto status :
         {Service::Status::Cancelled, Service::Status::Failed, Service::Status::Submitted,
          Service::Status::HandedOff, Service::Status::Unavailable}) {
        int legacyStarts = 0;
        Service::Result final;
        Service::Completion modernCompletion;
        Service service(
            [&](QWidget*, QImage, Service::Completion completion) {
                modernCompletion = completion;
                completion({status, {}});
            },
            [&](QWidget*, QImage, Service::Completion completion) {
                ++legacyStarts;
                completion({Service::Status::Submitted, {}});
            });
        QWidget owner;
        require(service.printImage(&owner, &owner, image(), [&](auto result) { final = result; }),
                "print must start");
        processUntil([&] { return !service.busy(); });
        require(legacyStarts == (status == Service::Status::Unavailable ? 1 : 0),
                "cancellation and submission failures must never open another dialog");
        require(final.status ==
                    (status == Service::Status::Unavailable ? Service::Status::Submitted : status),
                "fallback must preserve the terminal backend outcome");
        modernCompletion({Service::Status::Unavailable, {}});
        flush();
        require(legacyStarts <= 1, "late primary callbacks must not restart fallback");
    }
    Service unavailable({}, {});
    QWidget owner;
    bool failed = false;
    require(unavailable.printImage(&owner, &owner, image(),
                                   [&](auto result) {
                                       failed = result.status == Service::Status::Failed &&
                                                !result.error.isEmpty();
                                   }),
            "unavailable backend request must be accepted");
    processUntil([&] { return failed; });
    require(failed && !unavailable.busy(), "unavailable backends must report a translated failure");
}
void recoveryRetainsSnapshotAndIgnoresLateModernCallbacks() {
    for (auto legacyStatus : {Service::Status::HandedOff, Service::Status::Cancelled,
                              Service::Status::Failed, Service::Status::Unavailable}) {
        Service::Completion modern;
        Service::Completion legacy;
        QImage prepared;
        int legacyStarts = 0;
        int completions = 0;
        Service::Result final;
        QWidget owner;
        Service service(
            [&](QWidget*, QImage snapshot, auto completion) {
                prepared = snapshot;
                modern = std::move(completion);
            },
            [&](QWidget* nativeOwner, QImage snapshot, auto completion) {
                require(nativeOwner == &owner && snapshot == prepared,
                        "legacy recovery must retain the originating owner and prepared snapshot");
                ++legacyStarts;
                legacy = std::move(completion);
            });
        require(service.printImage(&owner, &owner, image(),
                                   [&](auto result) {
                                       final = std::move(result);
                                       ++completions;
                                   }),
                "recoverable print request must start");
        processUntil([&] { return bool(modern); });
        modern({Service::Status::Unavailable, QStringLiteral("critical preview failure")});
        processUntil([&] { return bool(legacy); });
        modern({Service::Status::Unavailable, {}});
        modern({Service::Status::Submitted, {}});
        flush();
        require(service.busy() && legacyStarts == 1 && completions == 0 &&
                    !service.printImage(&owner, &owner, image(), [](auto) {}),
                "recovery must stay pending and ignore late modern outcomes without a second UI");
        legacy({legacyStatus, QStringLiteral("legacy outcome")});
        processUntil([&] { return completions == 1; });
        legacy({Service::Status::Submitted, {}});
        flush();
        require(!service.busy() && completions == 1 && legacyStarts == 1 &&
                    final.status == (legacyStatus == Service::Status::Unavailable
                                         ? Service::Status::Failed
                                         : legacyStatus),
                "legacy recovery must complete once and never retry another failed backend");
    }
}
void destroyedOwnerDuringRecoveryCancelsOnce() {
    Service::Completion modern;
    Service::Completion legacy;
    Service service([&](QWidget*, QImage, auto completion) { modern = std::move(completion); },
                    [&](QWidget*, QImage, auto completion) { legacy = std::move(completion); });
    QObject receiver;
    auto* owner = new QWidget;
    int completions = 0;
    require(service.printImage(&receiver, owner, image(),
                               [&](auto result) {
                                   require(result.status == Service::Status::Cancelled,
                                           "destroying the owner during recovery must cancel");
                                   ++completions;
                               }),
            "recoverable print request must start");
    processUntil([&] { return bool(modern); });
    modern({Service::Status::Unavailable, {}});
    processUntil([&] { return bool(legacy); });
    delete owner;
    modern({Service::Status::Unavailable, {}});
    legacy({Service::Status::Submitted, {}});
    flush();
    require(completions == 1 && !service.busy(),
            "destroyed recovery targets must release the request and ignore both backends");
}
void destroyedTargetsAndDelayedCallbacks() {
    int starts = 0;
    Service::Completion nativeCompletion;
    Service service([&](QWidget*, QImage, Service::Completion completion) {
        ++starts;
        nativeCompletion = std::move(completion);
    });
    QObject receiver;
    auto* owner = new QWidget;
    int completions = 0;
    require(service.printImage(&receiver, owner, image(),
                               [&](auto result) {
                                   require(result.status == Service::Status::Cancelled,
                                           "destroyed owner must cancel");
                                   ++completions;
                               }),
            "print must start");
    delete owner;
    flush();
    require(starts == 0 && completions == 1 && !service.busy(),
            "destroyed owners must prevent queued native UI and release pending state");
    owner = new QWidget;
    auto* temporaryReceiver = new QObject;
    require(service.printImage(temporaryReceiver, owner, image(), [&](auto) { ++completions; }),
            "second print must start");
    processUntil([&] { return starts == 1; });
    delete temporaryReceiver;
    nativeCompletion({Service::Status::Submitted, {}});
    flush();
    require(starts == 1 && completions == 1 && !service.busy(),
            "destroyed receivers must suppress delayed callbacks");
    delete owner;
}
void screenshotSubmissionAndStaleCapturePolicy() {
    for (bool current : {true, false}) {
        for (auto status : {Service::Status::Submitted, Service::Status::Cancelled,
                            Service::Status::Failed, Service::Status::HandedOff}) {
            int released = 0;
            int closed = 0;
            int restored = 0;
            auto completed = std::make_shared<bool>(false);
            auto captureCompletion = screenshotPrintCompletion(
                completed,
                [&] {
                    ++released;
                    return current;
                },
                [&] { ++closed; },
                [&](auto result) {
                    require(result.status == status, "restore must receive the native outcome");
                    ++restored;
                });
            Service::Completion native;
            Service service([&](QWidget*, QImage, auto completion) { native = completion; });
            QWidget owner;
            require(service.printImage(&owner, &owner, image(), captureCompletion),
                    "capture print must start");
            processUntil([&] { return bool(native); });
            native({status, {}});
            flush();
            captureCompletion({Service::Status::Submitted, {}});
            require(released == 1 && *completed &&
                        closed == (current && status == Service::Status::Submitted ? 1 : 0) &&
                        restored == (current && status != Service::Status::Submitted ? 1 : 0),
                    "only current submitted captures may close; stale results must only release "
                    "interaction");
        }
    }
}

void onePagePlacementAndInteraction() {
    require(Service::fittedRect(QSize(200, 100), QRectF(10, 20, 100, 200)) ==
                QRectF(10, 95, 100, 50),
            "portrait printable bounds must center the aspect fit");
    require(Service::fittedRect(QSize(100, 200), QRectF(5, 10, 200, 100)) ==
                QRectF(80, 10, 50, 100),
            "changed paper orientation must recalculate placement");
    require(Service::fittedRect({}, QRectF(0, 0, 100, 100)).isEmpty() &&
                Service::fittedRect(QSize(2, 1),
                                    QRectF(0, 0, std::numeric_limits<double>::infinity(), 100))
                    .isEmpty(),
            "invalid pages must not produce a placement");
    QWidget owner;
    QLineEdit editor(&owner);
    ScreenshotPrintInteractionGuard guard({&owner});
    QKeyEvent event(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
    QCoreApplication::sendEvent(&editor, &event);
    require(editor.text().isEmpty(), "native interaction must freeze capture edits");
    guard.release();
    QCoreApplication::sendEvent(&editor, &event);
    require(editor.text() == QStringLiteral("a"), "completion must restore capture input");
}

void printingPreservesOwnerAndWindowFlags() {
    for (auto status : {Service::Status::Submitted, Service::Status::Cancelled,
                        Service::Status::Failed, Service::Status::HandedOff}) {
        QWidget owner;
        owner.setWindowFlag(Qt::WindowStaysOnTopHint);
        QWidget toolbar(&owner, Qt::Tool | Qt::WindowStaysOnTopHint);
        QWidget ordinary;
        const auto ownerFlags = owner.windowFlags();
        const auto toolbarFlags = toolbar.windowFlags();
        const auto ordinaryFlags = ordinary.windowFlags();
        const auto ownerHandle = owner.winId();
        const auto toolbarHandle = toolbar.winId();
        const auto unchanged = [&] {
            require(owner.windowFlags() == ownerFlags && toolbar.windowFlags() == toolbarFlags &&
                        ordinary.windowFlags() == ordinaryFlags &&
                        owner.internalWinId() == ownerHandle &&
                        toolbar.internalWinId() == toolbarHandle,
                    "printing must preserve window flags and existing native handles");
        };
        Service::Completion nativeCompletion;
        Service service([&](QWidget* nativeOwner, QImage, Service::Completion completion) {
            require(nativeOwner == &owner, "native printing must receive the originating window");
            unchanged();
            nativeCompletion = std::move(completion);
        });
        bool completed = false;
        require(service.printImage(&owner, &owner, image(),
                                   [&](auto result) {
                                       require(result.status == status,
                                               "printing must preserve the native outcome");
                                       unchanged();
                                       completed = true;
                                   }),
                "topmost owner print must start");
        processUntil([&] { return bool(nativeCompletion); });
        require(nativeCompletion && service.busy(), "native print must remain pending");
        unchanged();
        nativeCompletion({status, {}});
        processUntil([&] { return completed; });
        require(completed && !service.busy(), "native completion must release pending state");
        unchanged();
    }
}

void sharedServiceUsesNativeBackendWithLegacyFallback() {
    auto& service = Service::shared();
    require(sharedBackendSelections == std::vector<bool>{false, true} ||
                sharedBackendSelections == std::vector<bool>{true, false},
            "shared service must select the native backend and configure a legacy fallback");
    QWidget owner;
    for (auto status : {Service::Status::Cancelled, Service::Status::Failed,
                        Service::Status::Submitted, Service::Status::HandedOff}) {
        sharedBackendStatus = status;
        const auto starts = sharedBackendStarts.size();
        int completions = 0;
        require(service.printImage(&owner, &owner, image(),
                                   [&](auto result) {
                                       require(result.status == status,
                                               "shared service must preserve the dialog outcome");
                                       ++completions;
                                   }),
                "shared service must accept and reopen printing");
        processUntil([&] { return completions == 1; });
        require(sharedBackendStarts.size() == starts + 1 && !sharedBackendStarts.back() &&
                    completions == 1 && !service.busy(),
                "shared service must use the native backend once and release each request");
    }
    sharedBackendStatus = Service::Status::Unavailable;
    const auto starts = sharedBackendStarts.size();
    int completions = 0;
    require(service.printImage(&owner, &owner, image(),
                               [&](auto result) {
                                   require(result.status == Service::Status::HandedOff,
                                           "shared service must preserve the fallback outcome");
                                   ++completions;
                               }),
            "shared service must accept printing when the native backend is unavailable");
    processUntil([&] { return completions == 1; });
    require(sharedBackendStarts.size() == starts + 2 && !sharedBackendStarts[starts] &&
                sharedBackendStarts[starts + 1] && completions == 1 && !service.busy(),
            "shared service must try the native backend before starting the legacy fallback once");
    require(&Service::shared() == &service && sharedBackendSelections.size() == 2,
            "shared service must retain its backend selection across requests");
}

void persistedPrintDiagnostics() {
    print_tests::LogSession logs;
    QWidget owner;
    Service::Completion native;
    Service service(
        [&](QWidget*, QImage, auto completion) { native = std::move(completion); },
        [](QWidget*, QImage, auto completion) { completion({Service::Status::HandedOff, {}}); });
    require(!service.printImage(nullptr, &owner, image(), [](auto) {}) &&
                !service.printImage(&owner, nullptr, image(), [](auto) {}) &&
                !service.printImage(&owner, &owner, {}, [](auto) {}) &&
                !service.printImage(&owner, &owner, image(), {}),
            "invalid print requests must still be rejected");
    for (auto status :
         {Service::Status::Submitted, Service::Status::Cancelled, Service::Status::Failed,
          Service::Status::HandedOff, Service::Status::Unavailable}) {
        native = {};
        require(service.printImage(&owner, &owner, image(), [](auto) {}), "logged request starts");
        require(!service.printImage(&owner, &owner, image(), [](auto) {}),
                "busy request is rejected");
        processUntil([&] { return bool(native); });
        native({status, QStringLiteral("private screenshot text and C:/private/snapshot.png")});
        processUntil([&] { return !service.busy(); });
        native({Service::Status::Failed, QStringLiteral("late")});
        flush();
    }
    Service unavailable({}, {});
    require(unavailable.printImage(&owner, &owner, image(), [](auto) {}),
            "unavailable request starts");
    processUntil([&] { return !unavailable.busy(); });
    QObject receiver;
    auto* temporaryOwner = new QWidget;
    require(service.printImage(&receiver, temporaryOwner, image(), [](auto) {}),
            "abandoned request starts");
    delete temporaryOwner;
    flush();
    auto* temporaryReceiver = new QObject;
    native = {};
    require(service.printImage(temporaryReceiver, &owner, image(), [](auto) {}),
            "request with a temporary receiver starts");
    processUntil([&] { return bool(native); });
    delete temporaryReceiver;
    native({Service::Status::Submitted, {}});
    flush();

    const auto records = logs.records();
    QSet<QString> accepted;
    QSet<QString> prepared;
    QSet<QString> completed;
    QSet<QString> statuses;
    QSet<QString> rejections;
    QString fallbackOperation;
    bool legacyStarted = false;
    bool ownerDestroyed = false;
    bool receiverDestroyed = false;
    for (const auto& record : records) {
        const auto event = record.value(QStringLiteral("event")).toString();
        const auto fields = record.value(QStringLiteral("fields")).toObject();
        const auto operation = fields.value(QStringLiteral("operation")).toString();
        const auto bytes = QJsonDocument(record).toJson();
        require(!bytes.contains("private screenshot") && !bytes.contains("private/snapshot") &&
                    !bytes.contains("late"),
                "print logs must exclude content and ignored callbacks");
        if (event == QStringLiteral("print.rejected")) {
            rejections.insert(fields.value(QStringLiteral("reason")).toString());
            continue;
        }
        require(!operation.isEmpty() &&
                    fields.value(QStringLiteral("duration_ms")).toDouble(-1) >= 0,
                "request events must retain correlation and duration through export");
        if (event == QStringLiteral("print.accepted")) {
            require(!accepted.contains(operation), "request IDs must be unique");
            accepted.insert(operation);
            require(fields.value(QStringLiteral("width")).toInt() == 2 &&
                        fields.value(QStringLiteral("height")).toInt() == 1 &&
                        fields.value(QStringLiteral("dpr")).toDouble() == 2,
                    "source image dimensions and DPR must survive log export");
        } else {
            require(accepted.contains(operation), "request progress must follow acceptance");
        }
        if (event == QStringLiteral("print.prepared"))
            prepared.insert(operation);
        if (event == QStringLiteral("print.backend_started")) {
            require(prepared.contains(operation), "native printing must follow image preparation");
            if (fields.value(QStringLiteral("backend")) == QStringLiteral("legacy")) {
                legacyStarted = true;
                require(operation == fallbackOperation, "fallback must retain the request ID");
            }
        }
        if (event == QStringLiteral("print.fallback")) {
            fallbackOperation = operation;
            require(record.value(QStringLiteral("level")) == QStringLiteral("WARN"),
                    "fallback must preserve a warning even if the legacy backend succeeds");
        }
        if (event == QStringLiteral("print.completed")) {
            require(!completed.contains(operation),
                    "each accepted request must log completion once");
            completed.insert(operation);
            const auto status = fields.value(QStringLiteral("status")).toString();
            statuses.insert(status);
            require(record.value(QStringLiteral("level")) == (status == QStringLiteral("failed")
                                                                  ? QStringLiteral("WARN")
                                                                  : QStringLiteral("INFO")),
                    "cancellation and handoff are normal outcomes while failures are warnings");
            ownerDestroyed |=
                fields.value(QStringLiteral("reason")) == QStringLiteral("owner_destroyed");
            receiverDestroyed |=
                fields.value(QStringLiteral("reason")) == QStringLiteral("receiver_destroyed");
        }
    }
    require(accepted.size() == 8 && completed == accepted,
            "all eight accepted print requests must log completion exactly once");
    require(legacyStarted, "fallback logs must identify the legacy backend");
    require(ownerDestroyed, "destroyed owners must log the correct cancellation reason");
    require(receiverDestroyed, "destroyed receivers must log the correct cancellation reason");
    require(statuses == QSet<QString>{QStringLiteral("submitted"), QStringLiteral("cancelled"),
                                      QStringLiteral("failed"), QStringLiteral("handed_off")},
            "logs must distinguish all terminal print outcomes");
    require(rejections ==
                QSet<QString>{QStringLiteral("missing_receiver"), QStringLiteral("missing_owner"),
                              QStringLiteral("empty_image"), QStringLiteral("missing_completion"),
                              QStringLiteral("busy")},
            "rejection reasons must be persisted");
}
} // namespace

ScreenshotPrintService::Backend screenshotNativePrintBackend(bool legacy) {
    sharedBackendSelections.push_back(legacy);
    return [legacy](QWidget*, QImage, Service::Completion completion) {
        sharedBackendStarts.push_back(legacy);
        completion({legacy && sharedBackendStatus == Service::Status::Unavailable
                        ? Service::Status::HandedOff
                        : sharedBackendStatus,
                    {}});
    };
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    immutableWhiteSnapshotAndDuplicateCompletion();
    fallbackOnlyWhenUnavailable();
    recoveryRetainsSnapshotAndIgnoresLateModernCallbacks();
    destroyedOwnerDuringRecoveryCancelsOnce();
    destroyedTargetsAndDelayedCallbacks();
    screenshotSubmissionAndStaleCapturePolicy();
    onePagePlacementAndInteraction();
    printingPreservesOwnerAndWindowFlags();
    sharedServiceUsesNativeBackendWithLegacyFallback();
    persistedPrintDiagnostics();
    return 0;
}
