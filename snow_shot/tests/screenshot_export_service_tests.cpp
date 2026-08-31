#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/screenshotexportservice.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"

#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QEventLoop>
#include <QImage>
#include <QMouseEvent>
#include <QObject>
#include <QTimer>

#include <cstdlib>
#include <iostream>
#include <utility>

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
    ExportFixture()
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
        display.screen = QGuiApplication::primaryScreen();
        display.active = true;
        m_displays.appendDisplay(std::move(display));
        m_geometry.rebuild(m_displays);

        m_service = std::make_unique<ScreenshotExportService>(ScreenshotExportServiceContext{
            m_displays,
            m_runtime,
            m_geometry,
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

    SnowCanvasRuntime& runtime() {
        return m_runtime;
    }

  private:
    ScreenshotDisplaySession m_displays;
    SnowCanvasRuntime m_runtime;
    ScreenshotGeometryMapper m_geometry;
    std::unique_ptr<ScreenshotExportService> m_service;
};

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

QImage waitForPinnedResult(ScreenshotExportService& service,
                           const ScreenshotPinnedSelectionRequest& request,
                           std::optional<ScreenshotPinnedSelectionRequest>* deliveredRequest = nullptr,
                           bool* deliveredSuccess = nullptr) {
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
    const bool scheduled = service.schedulePinnedSelection(
        request, &receiver,
        [&receiver, &image, &loop, deliveredRequest, deliveredSuccess](
            ScreenshotPinnedSelectionRequest delivered,
            ScreenshotPinnedSelectionResultHandle result) mutable {
            if (deliveredRequest != nullptr) {
                *deliveredRequest = delivered;
            }
            const bool subscribed = result.subscribe(
                &receiver, [&image, &loop, deliveredSuccess](bool success, QImage value) mutable {
                    if (deliveredSuccess != nullptr) {
                        *deliveredSuccess = success;
                    }
                    image = std::move(value);
                    loop.quit();
                });
            require(subscribed, "pinned result handle could not be subscribed");
        });
    require(scheduled, "pinned export was not scheduled");
    timeout.start();
    loop.exec();
    timeout.stop();
    require(!timedOut, "pinned image materialization timed out");
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

void pinnedSelectionMaterializesCompositedImage() {
    ExportFixture fixture;
    require(fixture.isValid(), "export fixture could not initialize the canvas runtime");

    const QRect selection(12, 8, 37, 29);
    const ScreenshotResultStyle style{5, 4, QColor(0, 0, 0, 160)};
    SnowCanvasWidget canvas(fixture.runtime());
    canvas.resize(fixture.displaySnapshot().size());
    canvas.show();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(canvas.setViewportCamera(40.0, 30.0, 1.0),
            "pinned export canvas camera setup failed");
    require(canvas.setCanvasTool(SnowCanvasTool::Shape),
            "pinned export canvas should activate the shape tool");
    SnowCanvasShapeStyle shapeStyle;
    shapeStyle.stroke = QColor(240, 24, 24);
    shapeStyle.strokeWidth = 4.0;
    require(canvas.setCanvasShapeStylePatch(
                shapeStyle, SnowCanvasShapeStylePropertyStrokeColor |
                               SnowCanvasShapeStylePropertyStrokeWidth,
                SnowCanvasShapeKind::Rectangle),
            "pinned export canvas should configure a detectable rectangle stroke");
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(15.0, 10.0),
                      canvas.mapToGlobal(QPoint(15, 10)), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, QPointF(68.0, 48.0), canvas.mapToGlobal(QPoint(68, 48)),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(68.0, 48.0),
                        canvas.mapToGlobal(QPoint(68, 48)), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(&canvas, &press);
    QCoreApplication::sendEvent(&canvas, &move);
    QCoreApplication::sendEvent(&canvas, &release);
    require(canvas.canvasHistoryState().canUndo,
            "pinned export canvas should commit a drawing before pinning");

    const std::optional<ScreenshotPinnedSelectionRequest> prepared =
        fixture.service().preparePinnedSelection(selection, style);
    require(prepared.has_value() && prepared->isPrepared(),
            "pinned selection should be prepared before its image is rendered");

    std::optional<ScreenshotPinnedSelectionRequest> materialized;
    bool pinnedSuccess = false;
    const QImage pinnedImage = waitForPinnedResult(fixture.service(), *prepared, &materialized,
                                                   &pinnedSuccess);
    require(materialized.has_value() && materialized->isPrepared(),
            "pinned selection callback did not receive a valid prepared request");
    require(pinnedSuccess && !pinnedImage.isNull(),
            "pinned selection result handle did not publish a rendered image");

    const QImage expected = waitForResult(
        [&](QObject* receiver, auto callback) {
            return fixture.service().requestSelectionResult(selection, style, receiver,
                                                             std::move(callback));
        },
        [](QImage image) { return image; });
    require(hasSamePixels(pinnedImage, expected),
            "pinned selection image differed from the clipboard/result render");
    require(materialized->resultStyle.cornerRadius == style.cornerRadius &&
                materialized->resultStyle.shadowWidth == style.shadowWidth,
            "pinned selection request lost its result style metadata");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    directSourceKeepsFullWgcFrameForResultAndClipboard();
    styledClipboardResultRetainsDibV5();
    pinnedSelectionMaterializesCompositedImage();
    std::cout << "All screenshot export service tests passed\n";
    return EXIT_SUCCESS;
}
