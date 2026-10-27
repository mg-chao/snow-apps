// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/screenshotairdropservice.h"
#include "../src/presentation/services/screenshotairdropcompletion.h"

#include <QApplication>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QThread>
#include <QWidget>

#include <cstdlib>
#include <iostream>
#include <thread>
#include <utility>

namespace {
using Service = ScreenshotAirDropService;
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void wait(const std::function<bool()>& condition) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::msleep(1);
    }
    require(condition(), "timed out waiting for asynchronous AirDrop");
}
QImage snapshot() {
    QImage image(3, 2, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    image.setPixelColor(1, 0, QColor(210, 80, 120, 128));
    image.setPixelColor(2, 1, QColor(45, 75, 190));
    image.setColorSpace(QColorSpace::SRgb);
    image.setDevicePixelRatio(2);
    return image;
}
void pngLifetimeAndTerminalOutcomes() {
    QWidget owner;
    for (const auto status :
         {Service::Status::Shared, Service::Status::Cancelled, Service::Status::Failed,
          Service::Status::Unavailable, Service::Status::HandedOff}) {
        QString path;
        Service::Completion native;
        int starts = 0;
        Service service([&](QWidget* window, QString file, Service::Completion completion) {
            require(window == &owner, "native sharing receives the source owner");
            require(QThread::currentThread() == qApp->thread(), "native UI starts on GUI thread");
            path = std::move(file);
            native = std::move(completion);
            ++starts;
        });
        int completions = 0;
        QImage source = snapshot();
        const QImage expected = source;
        require(service.shareImage(&owner, &owner, source, QStringLiteral("capture.png"),
                                   [&](Service::Result result) {
                                       require(result.status == status, "preserve native outcome");
                                       require(!service.busy(),
                                               "release busy state before completion");
                                       require(QThread::currentThread() == qApp->thread(),
                                               "completion returns to GUI thread");
                                       ++completions;
                                   }),
                "accept image sharing");
        source.fill(Qt::green);
        require(completions == 0 && service.busy(), "sharing is asynchronous and busy immediately");
        require(!service.shareImage(&owner, &owner, snapshot(), QStringLiteral("other.png"),
                                    [](auto) {}),
                "reject duplicate request");
        wait([&] { return bool(native); });
        QImage decoded(path);
        require(decoded.size() == expected.size() && decoded.colorSpace() == expected.colorSpace(),
                "PNG preserves source resolution and color space");
        require(decoded.pixelColor(0, 0).alpha() == 0 &&
                    decoded.pixelColor(1, 0) == expected.pixelColor(1, 0) &&
                    decoded.pixelColor(2, 1) == expected.pixelColor(2, 1),
                "PNG freezes the requested pixels and preserves transparency");
        require(QFileInfo(path).fileName() == QStringLiteral("capture.png"),
                "use the supplied export filename");
        const QString directory = QFileInfo(path).absolutePath();
        std::thread worker([completion = std::move(native), status]() mutable {
            completion({status, {}});
            completion({status, {}});
        });
        worker.join();
        wait([&] { return completions == 1 && !QFileInfo::exists(directory); });
        require(starts == 1, "only one native chooser opens");
    }
}
void destructionKeepsInFlightFileAlive() {
    QObject receiver;
    auto owner = std::make_unique<QWidget>();
    Service::Completion native;
    QString path;
    int completions = 0;
    auto service =
        std::make_unique<Service>([&](QWidget*, QString file, Service::Completion completion) {
            path = std::move(file);
            native = std::move(completion);
        });
    require(service->shareImage(&receiver, owner.get(), snapshot(), QStringLiteral("../safe"),
                                [&](Service::Result result) {
                                    require(result.status == Service::Status::Cancelled,
                                            "source destruction cancels caller state");
                                    ++completions;
                                }),
            "start sharing before owner destruction");
    wait([&] { return bool(native); });
    require(QFileInfo(path).fileName() == QStringLiteral("safe.png"),
            "filename cannot escape the private temporary directory");
    const QString directory = QFileInfo(path).absolutePath();
    owner.reset();
    require(completions == 1 && !service->busy(), "owner destruction releases caller once");
    service.reset();
    require(QFileInfo::exists(path),
            "native sharing retains its PNG after source/service destruction");
    auto completion = std::move(native);
    completion({Service::Status::Shared, {}});
    completion = {};
    wait([&] { return !QFileInfo::exists(directory); });
    require(completions == 1, "late native outcome cannot mutate a destroyed source");
}
void unavailableAndEarlyDestruction() {
    QWidget owner;
    int completions = 0;
    Service unavailable({});
    require(!unavailable.shareImage(&owner, &owner, {}, {}, [](auto) {}), "reject an empty image");
    require(unavailable.shareImage(
                &owner, &owner, snapshot(), {},
                [&](auto result) {
                    require(result.status == Service::Status::Unavailable &&
                                !result.error.isEmpty(),
                            "missing native backend has a useful unavailable result");
                    ++completions;
                }),
            "accept unavailable request asynchronously");
    wait([&] { return completions == 1; });
    int starts = 0;
    Service service([&](QWidget*, QString, auto) { ++starts; });
    auto receiver = std::make_unique<QObject>();
    require(
        service.shareImage(receiver.get(), &owner, snapshot(), {}, [&](auto) { ++completions; }),
        "accept request before receiver destruction");
    receiver.reset();
    require(!service.busy(), "destroying receiver cancels preparation");
    wait([&] { return service.findChildren<QObject*>().isEmpty(); });
    require(starts == 0 && completions == 1, "destroyed receiver gets no native UI or callback");
}
void screenshotCompletionPolicy() {
    for (const auto status :
         {Service::Status::Shared, Service::Status::Cancelled, Service::Status::Failed,
          Service::Status::Unavailable, Service::Status::HandedOff}) {
        int released = 0, closed = 0, restored = 0;
        auto completion = screenshotAirDropCompletion(
            std::make_shared<bool>(false),
            [&] {
                ++released;
                return true;
            },
            [&] { ++closed; }, [&](auto) { ++restored; });
        completion({status, {}});
        completion({status, {}});
        require(released == 1 && closed == (status == Service::Status::Shared ? 1 : 0) &&
                    restored == (status == Service::Status::Shared ? 0 : 1),
                "only a successful share closes the screenshot, once");
        auto stale = screenshotAirDropCompletion(
            std::make_shared<bool>(false), [] { return false; }, [&] { ++closed; },
            [&](auto) { ++restored; });
        stale({Service::Status::Shared, {}});
        require(closed + restored == 1,
                "stale screenshot completion cannot affect a newer capture");
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    pngLifetimeAndTerminalOutcomes();
    destructionKeepsInFlightFileAlive();
    unavailableAndEarlyDestruction();
    screenshotCompletionPolicy();
    return 0;
}
