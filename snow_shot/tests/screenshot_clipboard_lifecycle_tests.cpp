#include "snow_shot/presentation/screenshotclipboardservice.h"

#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QEventLoop>
#include <QMimeData>
#include <QPointer>
#include <QTimer>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void commitCompletionMarksFinishedAndReleasesInputs() {
    QObject receiver;
    for (bool cancel : {false, true}) {
        QEventLoop loop;
        auto* mime = new QMimeData;
        mime->setText(QStringLiteral("clipboard lifetime fixture"));
        const QPointer<QMimeData> input(mime);
        ScreenshotClipboardCommitHandle handle;
        bool completed = false;
        handle = ScreenshotClipboardService::commitMimeData(
            QApplication::clipboard(), &receiver, mime,
            [&](ScreenshotClipboardCommitResult result) {
                require(handle.isFinished(),
                        "a completion callback must observe a finished handle");
                require(cancel ? result.failure == ScreenshotClipboardCommitFailure::Cancelled
                               : result.succeeded(),
                        "commit must report its actual outcome");
                if (cancel)
                    require(input.isNull(), "cancelled input must be released before completion");
                completed = true;
                loop.quit();
            });
        require(handle.isValid() && !handle.isFinished(), "a queued commit must remain pending");
        if (cancel) {
            handle.cancel();
            require(handle.isCancellationRequested() && !handle.isFinished(),
                    "requesting cancellation must not prematurely report completion");
        }
        QTimer::singleShot(2000, &loop, &QEventLoop::quit);
        loop.exec();
        require(completed, "clipboard commit completion timed out");
        QApplication::clipboard()->clear();
    }
}

void abandonedReceiversReleaseInputsAndFinishHandles() {
    auto* receiver = new QObject;
    auto* mime = new QMimeData;
    mime->setText(QStringLiteral("abandoned commit"));
    const QPointer<QMimeData> input(mime);
    bool called = false;
    auto handle = ScreenshotClipboardService::commitMimeData(
        QApplication::clipboard(), receiver, mime,
        [&](ScreenshotClipboardCommitResult) { called = true; });
    delete receiver;
    require(handle.isFinished() && input.isNull(),
            "destroying the receiver must release the queued input and finish its handle");
    QCoreApplication::processEvents();
    require(!called, "destroyed receivers must not receive completion callbacks");
}

void failedCommitsFinishHandles() {
    QObject receiver;
    QEventLoop loop;
    ScreenshotClipboardCommitHandle handle;
    bool completed = false;
    handle = ScreenshotClipboardService::commit(
        QApplication::clipboard(), &receiver, {}, [&](ScreenshotClipboardCommitResult result) {
            require(handle.isFinished(), "failed commits must finish before their callback");
            require(result.failure == ScreenshotClipboardCommitFailure::InvalidPayload,
                    "an empty payload must be rejected");
            completed = true;
            loop.quit();
        });
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    require(completed, "invalid payload completion timed out");
}

void scopedCommitsReleaseTrackingAndCallbacksOnEveryOutcome() {
    ScreenshotClipboardCommitScope scope;
    QObject receiver;
    for (int cycle = 0; cycle < 32; ++cycle) {
        for (int outcome = 0; outcome < 3; ++outcome) {
            QEventLoop loop;
            bool completed = false;
            ScreenshotClipboardCommitHandle handle;
            auto resource = std::make_shared<int>(cycle);
            const std::weak_ptr<int> lifetime(resource);
            auto completion = [&, resource](ScreenshotClipboardCommitResult result) {
                require(scope.pendingCount() == 0 && handle.isFinished(),
                        "every terminal commit must leave its scope before completion");
                const auto expected = outcome == 1
                                          ? ScreenshotClipboardCommitFailure::InvalidPayload
                                      : outcome == 2 ? ScreenshotClipboardCommitFailure::Cancelled
                                                     : ScreenshotClipboardCommitFailure::None;
                require(result.failure == expected, "scoped commits must preserve their outcome");
                require(*resource == cycle, "the callback must retain its inputs while running");
                completed = true;
                loop.quit();
            };
            if (outcome == 1) {
                handle =
                    scope.commit(QApplication::clipboard(), &receiver, {}, std::move(completion));
            } else {
                auto* mime = new QMimeData;
                mime->setText(QStringLiteral("scoped clipboard fixture"));
                handle = scope.commitMimeData(QApplication::clipboard(), &receiver, mime,
                                              std::move(completion));
            }
            resource.reset();
            require(scope.pendingCount() == 1 && handle.isValid(),
                    "a scope must own every pending publication");
            if (outcome == 2) {
                handle.cancel();
            }
            QTimer::singleShot(2000, &loop, &QEventLoop::quit);
            loop.exec();
            require(
                completed && lifetime.expired(),
                "finished commits must release callbacks without waiting for deferred deletion");
            QApplication::clipboard()->clear();
        }
    }
}

void scopedAbandonedAndRejectedCommitsReleaseOwnership() {
    ScreenshotClipboardCommitScope scope;
    auto* receiver = new QObject;
    auto* mime = new QMimeData;
    mime->setText(QStringLiteral("scoped abandoned input"));
    const QPointer<QMimeData> input(mime);
    auto resource = std::make_shared<int>(1);
    const std::weak_ptr<int> lifetime(resource);
    bool called = false;
    const auto handle =
        scope.commitMimeData(QApplication::clipboard(), receiver, mime,
                             [&, resource](ScreenshotClipboardCommitResult) { called = true; });
    resource.reset();
    delete receiver;
    require(scope.pendingCount() == 0 && handle.isFinished() && input.isNull() &&
                lifetime.expired(),
            "abandoned receivers must retire all scoped input and callback ownership immediately");
    require(!called, "abandoned scoped commits must suppress callbacks");

    mime = new QMimeData;
    const QPointer<QMimeData> rejectedInput(mime);
    const auto rejected =
        scope.commitMimeData(QApplication::clipboard(), nullptr, mime,
                             [&](ScreenshotClipboardCommitResult) { called = true; });
    require(!rejected.isValid() && rejectedInput.isNull() && scope.pendingCount() == 0,
            "rejected publications must not enter the scope or retain their inputs");
}

void scopedCancellationRetiresOnlyTheCancelledBatch() {
    ScreenshotClipboardCommitScope scope;
    auto* oldReceiver = new QObject;
    auto* mime = new QMimeData;
    mime->setText(QStringLiteral("cancelled batch"));
    bool oldCalled = false;
    const auto old =
        scope.commitMimeData(QApplication::clipboard(), oldReceiver, mime,
                             [&](ScreenshotClipboardCommitResult) { oldCalled = true; });
    scope.cancelAll();
    scope.cancelAll();
    require(scope.pendingCount() == 0 && old.isCancellationRequested(),
            "batch cancellation must release tracking and request cancellation");

    QObject receiver;
    QEventLoop loop;
    bool completed = false;
    mime = new QMimeData;
    mime->setText(QStringLiteral("new batch"));
    const auto current = scope.commitMimeData(
        QApplication::clipboard(), &receiver, mime, [&](ScreenshotClipboardCommitResult result) {
            require(result.succeeded() && scope.pendingCount() == 0,
                    "a new batch must complete independently of its cancelled predecessor");
            completed = true;
            loop.quit();
        });
    delete oldReceiver;
    require(scope.pendingCount() == 1 && old.isFinished(),
            "finishing an old batch must not remove a current operation");
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    require(completed && current.isFinished() && !oldCalled &&
                QApplication::clipboard()->text() == QStringLiteral("new batch"),
            "scope reuse must publish only the new batch and suppress cancelled callbacks");
    QApplication::clipboard()->clear();
}

void scopeDestructionCancelsWithoutAffectingOtherScopes() {
    QObject receiver;
    auto scope = std::make_unique<ScreenshotClipboardCommitScope>();
    auto* mime = new QMimeData;
    mime->setText(QStringLiteral("destroyed owner"));
    const QPointer<QMimeData> input(mime);
    bool called = false;
    const auto abandoned =
        scope->commitMimeData(QApplication::clipboard(), &receiver, mime,
                              [&](ScreenshotClipboardCommitResult) { called = true; });
    scope.reset();
    require(abandoned.isCancellationRequested(), "scope destruction must cancel its pending work");

    ScreenshotClipboardCommitScope survivor;
    QEventLoop loop;
    bool completed = false;
    mime = new QMimeData;
    mime->setText(QStringLiteral("surviving owner"));
    const auto live = survivor.commitMimeData(
        QApplication::clipboard(), &receiver, mime, [&](ScreenshotClipboardCommitResult result) {
            require(result.succeeded(), "independent scopes must preserve their pending work");
            completed = true;
            loop.quit();
        });
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    require(completed && live.isFinished() && abandoned.isFinished() && input.isNull() && !called,
            "destroyed scopes must release input ownership and suppress pending callbacks");
    QApplication::clipboard()->clear();
}

void completionCanCancelOtherCommitsAndStartANewBatch() {
    ScreenshotClipboardCommitScope scope;
    QObject receiver;
    QEventLoop loop;
    bool firstCompleted = false;
    bool cancelledCalled = false;
    bool newCompleted = false;
    const auto publicationId = ScreenshotClipboardService::reservePublication();
    const auto first = scope.commit(
        QApplication::clipboard(), &receiver, {},
        [&](ScreenshotClipboardCommitResult result) {
            require(result.failure == ScreenshotClipboardCommitFailure::InvalidPayload &&
                        scope.pendingCount() == 1,
                    "a completed operation must leave other pending publications owned");
            firstCompleted = true;
            scope.cancelAll();
            require(scope.pendingCount() == 0,
                    "a callback must be able to cancel its owner's remaining batch");
            const auto next = scope.commit(
                QApplication::clipboard(), &receiver, {},
                [&](ScreenshotClipboardCommitResult nextResult) {
                    require(nextResult.failure ==
                                    ScreenshotClipboardCommitFailure::InvalidPayload &&
                                scope.pendingCount() == 0,
                            "callback-created publications must retire the new batch correctly");
                    newCompleted = true;
                    loop.quit();
                });
            require(next.isValid() && scope.pendingCount() == 1,
                    "callbacks must be able to reuse a cancelled scope");
        },
        publicationId);
    const auto cancelled = scope.commit(
        QApplication::clipboard(), &receiver, {},
        [&](ScreenshotClipboardCommitResult) { cancelledCalled = true; }, publicationId);
    require(scope.pendingCount() == 2, "a scope must own multiple simultaneous publications");
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    require(firstCompleted && newCompleted && first.isFinished() && cancelled.isFinished() &&
                cancelled.isCancellationRequested() && !cancelledCalled,
            "reentrant batch replacement must preserve completion and cancellation boundaries");
}

#ifndef Q_OS_WIN
void scopeDestructionDuringPublicationSuppressesCompletion() {
    QObject receiver;
    auto scope = std::make_unique<ScreenshotClipboardCommitScope>();
    QEventLoop loop;
    const auto connection =
        QObject::connect(QApplication::clipboard(), &QClipboard::dataChanged, &loop, [&] {
            scope.reset();
            loop.quit();
        });
    auto* mime = new QMimeData;
    mime->setText(QStringLiteral("publication in progress"));
    bool called = false;
    const auto handle =
        scope->commitMimeData(QApplication::clipboard(), &receiver, mime,
                              [&](ScreenshotClipboardCommitResult) { called = true; });
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(connection);
    require(!scope && handle.isFinished() && handle.isCancellationRequested() && !called,
            "scope destruction during publication must safely suppress completion");
    require(QApplication::clipboard()->text() == QStringLiteral("publication in progress"),
            "a publication already in progress must retain valid input ownership");
    QApplication::clipboard()->clear();
}

void receiverDestructionDuringPublicationKeepsActiveInputsAlive(bool drainDeferredDeletes) {
    auto* receiver = new QObject;
    const QPointer<QObject> guardedReceiver(receiver);
    QImage image(32, 24, QImage::Format_ARGB32);
    image.fill(Qt::red);
    QEventLoop loop;
    const auto connection =
        QObject::connect(QApplication::clipboard(), &QClipboard::dataChanged, &loop, [&] {
            delete receiver;
            receiver = nullptr;
            if (drainDeferredDeletes) {
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }
            loop.quit();
        });
    bool called = false;
    const auto handle = ScreenshotClipboardService::commit(
        QApplication::clipboard(), receiver, ScreenshotClipboardService::prepareImage(image),
        [&](ScreenshotClipboardCommitResult) { called = true; });
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(connection);
    require(guardedReceiver.isNull() && handle.isFinished() && !called,
            "receiver destruction during publication must retire its commit without callbacks");
    require(QApplication::clipboard()->image().pixelColor(0, 0) == QColor(Qt::red),
            "publication already in progress must keep its input alive until the call returns");
    QApplication::clipboard()->clear();
}
#endif
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    require(ScreenshotClipboardCommitHandle{}.isFinished(), "an empty handle has no pending work");
    commitCompletionMarksFinishedAndReleasesInputs();
    abandonedReceiversReleaseInputsAndFinishHandles();
    failedCommitsFinishHandles();
    scopedCommitsReleaseTrackingAndCallbacksOnEveryOutcome();
    scopedAbandonedAndRejectedCommitsReleaseOwnership();
    scopedCancellationRetiresOnlyTheCancelledBatch();
    scopeDestructionCancelsWithoutAffectingOtherScopes();
    completionCanCancelOtherCommitsAndStartANewBatch();
#ifndef Q_OS_WIN
    scopeDestructionDuringPublicationSuppressesCompletion();
    receiverDestructionDuringPublicationKeepsActiveInputsAlive(false);
    receiverDestructionDuringPublicationKeepsActiveInputsAlive(true);
#endif
    return EXIT_SUCCESS;
}
