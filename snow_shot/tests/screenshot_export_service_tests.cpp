#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotasyncactivitytracker.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/screenshotexportservice.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"

#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include <QApplication>
#include <QEventLoop>
#include <QImage>
#include <QObject>
#include <QTimer>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

struct ScreenshotClipboardPayloadTestAccess {
#if defined(Q_OS_WIN) || defined(_WIN32)
    static bool hasDib(const ScreenshotClipboardPayload& payload) {
        return payload.m_nativeHandle != nullptr &&
               payload.m_formatMode == ScreenshotClipboardFormatMode::CompatibleDib;
    }

    static bool hasDibV5(const ScreenshotClipboardPayload& payload) {
        return payload.m_nativeHandle != nullptr &&
               payload.m_formatMode == ScreenshotClipboardFormatMode::DibV5;
    }
#endif
};

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

QImage patternedImage(const QSize& size, int seed) {
    QImage image(size, QImage::Format_ARGB32);
    for (int y = 0; y < size.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            row[x] = qRgba((x * 17 + seed) % 256, (y * 29 + seed * 3) % 256,
                           (x * 7 + y * 11 + seed * 5) % 256, 255);
        }
    }
    return image;
}

bool hasSamePixels(const QImage& actual, const QImage& expected) {
    return actual.size() == expected.size() && actual.convertToFormat(QImage::Format_ARGB32) ==
                                                   expected.convertToFormat(QImage::Format_ARGB32);
}

class ExportFixture final {
  public:
    explicit ExportFixture(std::function<void()> becameIdle = {})
        : m_runtime(
              SnowCanvasRuntimeConfig{snow_shot::presentation::screenshotCanvasStyleDefaults()}) {
        CapturedDisplayModel display;
        display.stableId = QStringLiteral("display-history-source");
        display.name = QStringLiteral("Display history source");
        display.physicalRect = QRect(0, 0, 80, 60);
        display.canvasRect = display.physicalRect;
        display.imageSourceCanvasRect = display.canvasRect;
        display.logicalRect = display.physicalRect;
        display.image = patternedImage(display.physicalRect.size(), 3);
        display.active = true;
        m_displays.appendDisplay(std::move(display));

        m_service = std::make_unique<ScreenshotExportService>(ScreenshotExportServiceContext{
            m_displays,
            m_runtime,
            m_geometry,
            std::move(becameIdle),
        });
    }

    [[nodiscard]] bool isValid() const {
        return m_runtime.isValid() && m_service != nullptr;
    }

    ScreenshotExportService& service() {
        return *m_service;
    }

    [[nodiscard]] QImage displaySnapshot() const {
        return m_displays.displayAt(0).image;
    }

  private:
    ScreenshotDisplaySession m_displays;
    SnowCanvasRuntime m_runtime;
    ScreenshotGeometryMapper m_geometry;
    std::unique_ptr<ScreenshotExportService> m_service;
};

template <typename Predicate>
bool waitForCondition(Predicate predicate, int timeoutMilliseconds = 5000) {
    if (predicate()) {
        return true;
    }

    QEventLoop loop;
    QTimer pollTimer;
    pollTimer.setInterval(1);
    QObject::connect(&pollTimer, &QTimer::timeout, &loop, [&]() {
        if (predicate()) {
            loop.quit();
        }
    });
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(timeoutMilliseconds);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    pollTimer.start();
    timeout.start();
    loop.exec();
    return predicate();
}

template <typename ScheduleRequest, typename ResultImage>
QImage waitForResult(ScheduleRequest scheduleRequest, ResultImage resultImage) {
    QObject receiver;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(5000);

    QImage image;
    bool timedOut = false;
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        loop.quit();
    });

    const bool scheduled = scheduleRequest(&receiver, [&](auto result) {
        image = resultImage(std::move(result));
        loop.quit();
    });
    require(scheduled, "selection export was not scheduled");
    timeout.start();
    loop.exec();
    timeout.stop();
    require(!timedOut, "selection export timed out");
    return image;
}

void directSourceKeepsFullWgcFrameForResultAndClipboard() {
    ExportFixture fixture;
    require(fixture.isValid(), "export fixture could not initialize the canvas runtime");

    const QRect visibleSelection(12, 8, 37, 29);
    const ScreenshotResultStyle style;
    const QImage directSource = patternedImage(QSize(137, 91), 11);
    const QImage expected = ScreenshotResultCompositor::compose(directSource, style);
    const QImage displayBefore = fixture.displaySnapshot().copy();

    fixture.service().setNextSelectionSourceImage(directSource);
    const QImage result = waitForResult(
        [&](QObject* receiver, auto callback) {
            return fixture.service().requestSelectionResult(visibleSelection, style, receiver,
                                                            std::move(callback));
        },
        [](QImage image) { return image; });

    require(result.size() == directSource.size(),
            "result export cropped the direct WGC source to the display selection");
    require(hasSamePixels(result, expected), "result export changed the direct WGC source pixels");
    require(fixture.displaySnapshot() == displayBefore,
            "result export modified the display snapshot retained for history");

    fixture.service().setNextSelectionSourceImage(directSource);
    const QImage clipboardResult = waitForResult(
        [&](QObject* receiver, auto callback) {
            return fixture.service().requestSelectionClipboard(visibleSelection, style, receiver,
                                                               std::move(callback));
        },
        [](ScreenshotSelectionClipboardResult result) {
            require(result.isValid(), "clipboard export did not produce a valid payload");
#if defined(Q_OS_WIN) || defined(_WIN32)
            require(ScreenshotClipboardPayloadTestAccess::hasDib(result.payload),
                    "plain clipboard export did not prepare CF_DIB");
            require(!ScreenshotClipboardPayloadTestAccess::hasDibV5(result.payload),
                    "plain clipboard export unexpectedly prepared CF_DIBV5");
#endif
            return std::move(result.image);
        });

    require(clipboardResult.size() == directSource.size(),
            "clipboard export cropped the direct WGC source to the display selection");
    require(hasSamePixels(clipboardResult, expected),
            "clipboard export changed the direct WGC source pixels");
    require(fixture.displaySnapshot() == displayBefore,
            "clipboard export modified the display snapshot retained for history");
}

void styledClipboardResultRetainsDibV5() {
    ExportFixture fixture;
    require(fixture.isValid(), "styled export fixture could not initialize the canvas runtime");

    const QRect visibleSelection(12, 8, 37, 29);
    const ScreenshotResultStyle style{8, 0, QColor(0, 0, 0, 180)};
    const QImage directSource = patternedImage(QSize(64, 48), 17);
    fixture.service().setNextSelectionSourceImage(directSource);

    const QImage resultImage = waitForResult(
        [&](QObject* receiver, auto callback) {
            return fixture.service().requestSelectionClipboard(visibleSelection, style, receiver,
                                                               std::move(callback));
        },
        [](ScreenshotSelectionClipboardResult result) {
            require(result.isValid(), "styled clipboard export did not produce a valid payload");
#if defined(Q_OS_WIN) || defined(_WIN32)
            require(!ScreenshotClipboardPayloadTestAccess::hasDib(result.payload),
                    "styled clipboard export unexpectedly prepared CF_DIB");
            require(ScreenshotClipboardPayloadTestAccess::hasDibV5(result.payload),
                    "styled clipboard export did not preserve CF_DIBV5");
#endif
            return std::move(result.image);
        });
    require(!resultImage.isNull(), "styled clipboard export produced no image");
    require(resultImage.pixelColor(0, 0).alpha() == 0,
            "styled clipboard export did not retain rounded-corner transparency");
}

void pendingRequestsReachIdleAfterTerminalPayloadDisposal() {
    const int baselineActivity =
        ScreenshotAsyncActivityTracker::shared().activeActivityCount();
    int idleNotifications = 0;
    bool idleObservedDisposedCallbacks = false;
    std::vector<std::weak_ptr<int>> callbackLifetimes;
    ExportFixture fixture([&]() {
        ++idleNotifications;
        idleObservedDisposedCallbacks =
            std::all_of(callbackLifetimes.cbegin(), callbackLifetimes.cend(),
                        [](const std::weak_ptr<int>& lifetime) { return lifetime.expired(); });
    });
    require(fixture.isValid(), "pending-state fixture could not initialize the canvas runtime");

    QObject receiver;
    int callbacks = 0;
    bool callbacksObservedPending = true;
    bool callbacksObservedActivity = true;
    for (int requestIndex = 0; requestIndex < 2; ++requestIndex) {
        auto callbackLifetime = std::make_shared<int>(requestIndex);
        callbackLifetimes.emplace_back(callbackLifetime);
        fixture.service().setNextSelectionSourceImage(
            patternedImage(QSize(96 + requestIndex, 64 + requestIndex), 31 + requestIndex));
        const bool scheduled = fixture.service().requestSelectionResult(
            QRect(5, 7, 32, 24), ScreenshotResultStyle{}, &receiver,
            [&, callbackLifetime](QImage image) {
                require(!image.isNull(), "pending-state export produced no image");
                callbacksObservedPending =
                    callbacksObservedPending && fixture.service().hasPendingRequests();
                callbacksObservedActivity =
                    callbacksObservedActivity &&
                    ScreenshotAsyncActivityTracker::shared().activeActivityCount() >
                        baselineActivity;
                ++callbacks;
            });
        require(scheduled, "pending-state export was not scheduled");
        callbackLifetime.reset();
    }

    require(fixture.service().hasPendingRequests(),
            "accepted exports were not reported as pending");
    require(ScreenshotAsyncActivityTracker::shared().activeActivityCount() ==
                baselineActivity + 2,
            "accepted exports did not retain independent activity leases");
    require(waitForCondition([&]() {
                return callbacks == 2 && !fixture.service().hasPendingRequests();
            }),
            "accepted exports did not reach their terminal callbacks");
    require(callbacksObservedPending,
            "pending state ended before a terminal callback returned");
    require(callbacksObservedActivity,
            "activity tracking ended before a terminal callback returned");
    require(waitForCondition([&]() { return idleNotifications == 1; }),
            "the final request did not publish one idle transition");
    require(idleNotifications == 1, "the service published duplicate idle transitions");
    require(idleObservedDisposedCallbacks,
            "idle was published before terminal callback captures were disposed");
    require(ScreenshotAsyncActivityTracker::shared().activeActivityCount() == baselineActivity,
            "export activity leases remained live after terminal disposal");
}

void destructionDrainsPendingRequestsWithoutPublishingCallbacks() {
    const int baselineActivity =
        ScreenshotAsyncActivityTracker::shared().activeActivityCount();
    bool callbackInvoked = false;
    bool idleNotificationInvoked = false;
    std::weak_ptr<int> callbackLifetime;
    QObject receiver;
    {
        ExportFixture fixture([&idleNotificationInvoked]() { idleNotificationInvoked = true; });
        require(fixture.isValid(),
                "teardown fixture could not initialize the canvas runtime");
        auto lifetime = std::make_shared<int>(1);
        callbackLifetime = lifetime;
        fixture.service().setNextSelectionSourceImage(patternedImage(QSize(640, 480), 47));
        const bool scheduled = fixture.service().requestSelectionResult(
            QRect(0, 0, 640, 480), ScreenshotResultStyle{}, &receiver,
            [lifetime, &callbackInvoked](QImage) { callbackInvoked = true; });
        require(scheduled && fixture.service().hasPendingRequests(),
                "teardown export was not accepted as pending");
        require(ScreenshotAsyncActivityTracker::shared().activeActivityCount() ==
                    baselineActivity + 1,
                "teardown export did not acquire an activity lease");
        lifetime.reset();
    }

    require(!callbackInvoked, "service teardown delivered a queued result callback");
    require(!idleNotificationInvoked,
            "service teardown published an idle callback after notifications were disabled");
    require(callbackLifetime.expired(),
            "service teardown retained a terminal callback capture");
    require(ScreenshotAsyncActivityTracker::shared().activeActivityCount() == baselineActivity,
            "service teardown retained pending async activity");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    directSourceKeepsFullWgcFrameForResultAndClipboard();
    styledClipboardResultRetainsDibV5();
    pendingRequestsReachIdleAfterTerminalPayloadDisposal();
    destructionDrainsPendingRequestsWithoutPublishingCallbacks();
    std::cout << "All screenshot export service tests passed\n";
    return EXIT_SUCCESS;
}
