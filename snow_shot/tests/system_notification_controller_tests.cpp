#include "snow_shot/presentation/systemnotificationcontroller.h"
#include "snow_shot/presentation/systemnotificationfeedback.h"
#include "widgets/message.h"

#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include <QVector>
#include <QWidget>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>

namespace {
using snow_shot::platform::SystemNotificationAction;
using snow_shot::platform::SystemNotificationCompletion;
using snow_shot::platform::SystemNotificationRequest;
using snow_shot::platform::SystemNotificationResult;
using snow_shot::platform::SystemNotificationSeverity;
using snow_shot::presentation::presentSystemNotificationResult;
using snow_shot::presentation::SystemNotificationController;
using Status = SystemNotificationResult::Status;
using Type = adqt::widgets::AdMessage::Type;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void requestCompletionAndFeedback() {
    QVector<SystemNotificationCompletion> pending;
    SystemNotificationController controller(
        [&](const SystemNotificationRequest&, SystemNotificationCompletion completed) {
            pending.append(std::move(completed));
        });
    QWidget window;
    window.resize(800, 500);
    int activations = 0;
    QVector<SystemNotificationRequest> delivered;
    QVector<SystemNotificationResult> results;
    QObject::connect(
        &controller, &SystemNotificationController::deliveryFinished, &controller,
        [&](const SystemNotificationRequest& request, const SystemNotificationResult& result) {
            require(QThread::currentThread() == controller.thread(),
                    "delivery feedback must return to the controller's thread");
            delivered.append(request);
            results.append(result);
            auto* handle = presentSystemNotificationResult(request, result, [&] {
                ++activations;
                window.show();
                return &window;
            });
            if (result.accepted()) {
                require(handle == nullptr,
                        "accepted native delivery must not create fallback feedback");
            } else {
                require(handle && handle->isOpen() &&
                            handle->content() == request.title + QLatin1Char('\n') + request.body,
                        "rejected delivery must display the original message");
            }
        });
    controller.show({QStringLiteral("Export complete"), QStringLiteral("/first.mp4"),
                     SystemNotificationSeverity::Information,
                     SystemNotificationAction::OpenRecording, QStringLiteral("/first.mp4")});
    controller.show({QStringLiteral("Export complete"), QStringLiteral("/second.mp4"),
                     SystemNotificationSeverity::Information,
                     SystemNotificationAction::OpenRecording, QStringLiteral("/second.mp4")});
    controller.show({QStringLiteral("Pin failed"), QStringLiteral("Invalid image"),
                     SystemNotificationSeverity::Warning});
    require(pending.size() == 3, "each notification must have its own completion");
    // A backend can finish concurrent requests in any order and on any thread.
    std::thread worker([completed = pending.at(1)] { completed({}); });
    worker.join();
    QCoreApplication::processEvents();
    require(results.size() == 1 && results.first().accepted() && activations == 0 &&
                !window.isVisible(),
            "OS-accepted notifications must respect presentation settings without activating UI");
    pending.at(2)({Status::Denied, QStringLiteral("internal permission diagnostic")});
    pending.at(0)({Status::Failed, QStringLiteral("internal transport diagnostic")});
    require(results.size() == 1, "synchronous backend failures must defer window feedback");
    QCoreApplication::processEvents();
    require(results.size() == 3 && activations == 2 && window.isVisible(),
            "every rejected request must reach the shared visible fallback");
    require(delivered.at(0).filePath == QStringLiteral("/second.mp4") &&
                delivered.at(0).action == SystemNotificationAction::OpenRecording &&
                delivered.at(1).title == QStringLiteral("Pin failed") &&
                delivered.at(1).severity == SystemNotificationSeverity::Warning &&
                results.at(1).status == Status::Denied &&
                delivered.at(2).filePath == QStringLiteral("/first.mp4") &&
                results.at(2).status == Status::Failed,
            "out-of-order results must preserve each request's message, severity and action");
    adqt::widgets::AdMessageService::destroyAll(&window);
}

void unavailableAndShutdown() {
    SystemNotificationController unavailable({});
    int unavailableResults = 0;
    QObject::connect(
        &unavailable, &SystemNotificationController::deliveryFinished, &unavailable,
        [&](const SystemNotificationRequest& request, const SystemNotificationResult& result) {
            require(request.body == QStringLiteral("Export complete") &&
                        result.status == Status::Unavailable,
                    "a missing backend must report the original undelivered request");
            ++unavailableResults;
        });
    unavailable.show({{}, QStringLiteral("Export complete")});
    require(unavailableResults == 0, "unavailable delivery must also queue its result");
    QCoreApplication::processEvents();
    require(unavailableResults == 1, "unavailable delivery must produce one result");

    SystemNotificationCompletion lateCompletion;
    int shutdownResults = 0;
    auto controller = std::make_unique<SystemNotificationController>(
        [&](const SystemNotificationRequest&, SystemNotificationCompletion completed) {
            lateCompletion = std::move(completed);
        });
    QObject::connect(controller.get(), &SystemNotificationController::deliveryFinished,
                     &unavailable, [&](const auto&, const auto&) { ++shutdownResults; });
    controller->show({{}, QStringLiteral("Pending shutdown")});
    lateCompletion({Status::Denied, {}});
    controller.reset();
    QCoreApplication::processEvents();
    std::thread worker([completed = lateCompletion] { completed({Status::Failed, {}}); });
    worker.join();
    QCoreApplication::processEvents();
    require(shutdownResults == 0,
            "queued and late native completions must not use a destroyed controller");
}

void fallbackSeverityAndContent() {
    QWidget window;
    window.resize(800, 500);
    int activations = 0;
    const auto activate = [&]() -> QWidget* {
        ++activations;
        window.show();
        return &window;
    };
    const SystemNotificationResult rejected{Status::Unavailable,
                                            QStringLiteral("private backend detail")};
    struct Case {
        SystemNotificationSeverity severity;
        Type type;
        QString title;
        QString body;
        QString content;
    };
    const QVector<Case> cases{{SystemNotificationSeverity::Information, Type::Info,
                               QStringLiteral("Export complete"), QStringLiteral("/clip.mp4"),
                               QStringLiteral("Export complete\n/clip.mp4")},
                              {SystemNotificationSeverity::Warning,
                               Type::Warning,
                               {},
                               QStringLiteral("Pin failed"),
                               QStringLiteral("Pin failed")},
                              {SystemNotificationSeverity::Error,
                               Type::Error,
                               QStringLiteral("Capture failed"),
                               {},
                               QStringLiteral("Capture failed")}};
    for (const auto& item : cases) {
        auto* handle = presentSystemNotificationResult({item.title, item.body, item.severity},
                                                       rejected, activate);
        QCoreApplication::processEvents();
        require(handle && handle->isOpen() && handle->type() == item.type &&
                    handle->content() == item.content && handle->noticeWidget() &&
                    handle->noticeWidget()->isVisible(),
                "the fallback must render original content with the correct severity");
        adqt::widgets::AdMessageService::destroyAll(&window);
    }
    require(activations == cases.size(), "each failed delivery must activate its fallback once");
    require(presentSystemNotificationResult({}, rejected, {}) == nullptr &&
                presentSystemNotificationResult({}, rejected,
                                                []() -> QWidget* { return nullptr; }) == nullptr,
            "fallback must tolerate an unavailable application window");
}
} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    requestCompletionAndFeedback();
    unavailableAndShutdown();
    fallbackSeverityAndContent();
    return 0;
}
