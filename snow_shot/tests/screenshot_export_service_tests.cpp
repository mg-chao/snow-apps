#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/screenshotexportservice.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_shot/presentation/screenshotselectionpin.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snowimageqtcodec.h"

#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QBuffer>
#include <QEventLoop>
#include <QImage>
#include <QMouseEvent>
#include <QObject>
#include <QTimer>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QColorSpace>

#include <cstdlib>
#include <iostream>
#include <utility>

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
    if (actual.size() != expected.size())
        return false;
    // Compare the same straight-alpha storage representation. Color-space
    // metadata is checked independently by the color regression tests.
    const auto actualPixels = actual.convertToFormat(QImage::Format_ARGB32);
    const auto expectedPixels = expected.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < actual.height(); ++y) {
        for (int x = 0; x < actual.width(); ++x) {
            if (actualPixels.pixel(x, y) != expectedPixels.pixel(x, y))
                return false;
        }
    }
    return true;
}

class ExportFixture final {
  public:
    explicit ExportFixture(bool points = false, QSize pixels = QSize(80, 60), qreal dpr = 0.0)
        : m_runtime(
              SnowCanvasRuntimeConfig{snow_shot::presentation::screenshotCanvasStyleDefaults()}) {
        CapturedDisplayModel display;
        display.stableId = QStringLiteral("display-history-source");
        display.name = QStringLiteral("Display history source");
        display.physicalRect = QRect(QPoint(), pixels);
        display.canvasRect = display.physicalRect;
        display.imageSourceCanvasRect = display.canvasRect;
        display.logicalRect = display.physicalRect;
        display.image = patternedImage(display.physicalRect.size(), 3);
        display.screen = QGuiApplication::primaryScreen();
        display.active = true;
        if (dpr > 0.0) {
            display.logicalRect.setSize(
                QSize(qRound(pixels.width() / dpr), qRound(pixels.height() / dpr)));
            display.logicalToPhysicalScale = dpr;
            display.geometryResolved = true;
        }
        if (points) {
            display.canvasUsesPoints = true;
            display.capturedLogicalRect = QRect(0, 0, 40, 30);
            display.imageSourceCanvasRect = display.capturedLogicalRect;
            display.backingScale = 2;
        }
        m_displays.appendDisplay(std::move(display));
        m_geometry.rebuild(m_displays);
        // Synthetic point displays have no NSScreen identity. Bind their test surface explicitly.
        m_displays.displayAt(0).screen = QGuiApplication::primaryScreen();

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

QImage
waitForPinnedResult(ScreenshotExportService& service,
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
        [&receiver, &image, &loop, deliveredRequest,
         deliveredSuccess](ScreenshotPinnedSelectionRequest delivered,
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

void styledClipboardResultRetainsPngTransparency() {
    ExportFixture fixture;
    require(fixture.isValid(), "styled export fixture could not initialize the canvas runtime");

    const QRect visibleSelection(12, 8, 37, 29);
    const ScreenshotResultStyle style{8, 0, QColor(0, 0, 0, 180)};

    const QImage resultImage = waitForResult(
        [&](QObject* receiver, auto callback) {
            return fixture.service().requestSelectionClipboard(visibleSelection, style, receiver,
                                                               std::move(callback));
        },
        [](ScreenshotSelectionClipboardResult result) {
            require(result.isValid(), "styled clipboard export did not produce a valid payload");
            require(result.payload.isValid() && !result.payload.pngBytes().isEmpty(),
                    "clipboard export must prepare PNG and its bitmap fallback");
            return std::move(result.image);
        });
    require(!resultImage.isNull(), "styled clipboard export produced no image");
    require(resultImage.pixelColor(0, 0).alpha() == 0,
            "styled clipboard export did not retain rounded-corner transparency");
}

void screenshotExportsRetainSrgb() {
    const QColorSpace srgb(QColorSpace::SRgb);
    QImage source(40, 30, QImage::Format_ARGB32_Premultiplied);
    source.fill(QColor(200, 100, 50));
    source.setColorSpace(srgb);
    ScreenshotResultStyle compound;
    compound.region = QRegion(source.rect()).subtracted(QRect(25, 20, 10, 5));
    for (const auto& style :
         {ScreenshotResultStyle{}, ScreenshotResultStyle{4, 3, Qt::black}, compound}) {
        const auto result = ScreenshotResultCompositor::compose(source, style);
        require(result.colorSpace() == srgb, "styled screenshot lost its sRGB working space");
        const auto layout = ScreenshotResultCompositor::layoutForContent(source.size(), style);
        require(result.pixelColor(layout.contentRect.center()) == QColor(200, 100, 50),
                "sRGB composition changed captured pixel values");
        const auto png = snow_shot::image_codec::encodePng(result, 1);
        const auto decoded = QImage::fromData(png, "PNG");
        require(decoded.colorSpace() == srgb, "PNG round-trip lost its sRGB profile");
        require(hasSamePixels(decoded, result), "PNG round-trip changed screenshot pixels");
    }
    QImage p3 = source;
    p3.setColorSpace(QColorSpace::DisplayP3);
    const auto expected = p3.convertedToColorSpace(srgb, QImage::Format_ARGB32_Premultiplied);
    const auto normalized = ScreenshotResultCompositor::normalizeImage(p3);
    require(normalized.colorSpace() == srgb && hasSamePixels(normalized, expected) &&
                normalized.pixelColor(0, 0) != p3.pixelColor(0, 0),
            "profiled pin must convert its pixels into the raster working space");

    QByteArray encodedP3;
    QBuffer fixtureBuffer(&encodedP3);
    require(fixtureBuffer.open(QIODevice::WriteOnly) && p3.save(&fixtureBuffer, "PNG"),
            "Display P3 import fixture must encode");
    const auto imported =
        snow_shot::image_codec::decode(encodedP3, snow::image::Format::png, "image/png");
    const auto importedResult = ScreenshotResultCompositor::compose(imported, {});
    require(importedResult.colorSpace() == srgb && hasSamePixels(importedResult, expected),
            "imported pin must convert the embedded profile before composition");
    const auto importedExport =
        QImage::fromData(snow_shot::image_codec::encodePng(imported), "PNG");
    require(importedExport.colorSpace() == srgb && hasSamePixels(importedExport, expected),
            "imported pin export must retain the correctly converted sRGB colors");

    QImage largeSource(1200, 1000, QImage::Format_ARGB32_Premultiplied);
    largeSource.setColorSpace(srgb);
    largeSource.fill(QColor(200, 100, 50));
    ScreenshotResultStyle sparse;
    sparse.region = QRegion(QRect(0, 0, 20, 20)).united(QRect(1180, 980, 20, 20));
    sparse.shadowWidth = 3;
    ScreenshotResultStyle tiled;
    tiled.region = QRegion(largeSource.rect()).subtracted(QRect(100, 100, 20, 20));
    tiled.shadowWidth = 3;
    for (const auto& style : {sparse, tiled}) {
        const auto result = ScreenshotResultCompositor::compose(largeSource, style, 1.0, 0.5);
        require(result.colorSpace() == srgb && result.pixelColor(13, 13).alpha() == 128,
                "sparse and tiled region exports must retain sRGB through opacity composition");
    }

    ExportFixture fixture(true);
    const auto exported = waitForResult(
        [&](QObject* receiver, auto callback) {
            return fixture.service().requestSelectionResult(QRect(0, 0, 40, 30), {}, receiver,
                                                            std::move(callback));
        },
        [](QImage image) { return image; });
    require(exported.colorSpace() == srgb && hasSamePixels(exported, fixture.displaySnapshot()),
            "canvas export must retain sRGB without changing Retina capture pixels");
}

void selectionClipboardSnapshotsExportSettings() {
    QTemporaryDir directory;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(directory.isValid() &&
                storage.initialize({directory.filePath(QStringLiteral("bin")), directory.path(), 0})
                    .success,
            "clipboard encoding settings fixture could not initialize");
    const auto cleanup = qScopeGuard([&] { storage.shutdown(); });
    const snow_shot::storage::ScreenshotSettings settings;
    ExportFixture fixture;
    require(fixture.isValid(), "clipboard encoding fixture could not initialize the canvas");
    const QRect selection(12, 8, 37, 29);
    for (auto compression : {ScreenshotCompressionLevel::Low, ScreenshotCompressionLevel::Medium,
                             ScreenshotCompressionLevel::High}) {
        require(settings.setCompressionLevel(
                    ScreenshotImageFileService::compressionLevelKey(compression)),
                "clipboard compression setup failed");
        require(settings.setImageQuality(35), "clipboard image quality setup failed");
        const int level = ScreenshotImageFileService::encodeOptions(ScreenshotImageFileFormat::Png,
                                                                    {35, compression})
                              .compression_level;
        const QImage result = waitForResult(
            [&](QObject* receiver, auto callback) {
                const bool scheduled = fixture.service().requestSelectionClipboard(
                    selection, {}, receiver, std::move(callback));
                // The worker must use the value captured by the request, even when settings
                // change before its callback is delivered.
                require(
                    settings.setCompressionLevel(ScreenshotImageFileService::compressionLevelKey(
                        compression == ScreenshotCompressionLevel::High
                            ? ScreenshotCompressionLevel::Low
                            : ScreenshotCompressionLevel::High)),
                    "clipboard settings mutation failed");
                return scheduled;
            },
            [&](ScreenshotSelectionClipboardResult value) {
                require(value.isValid() &&
                            value.payload.pngBytes() ==
                                snow_shot::image_codec::encodePng(
                                    snow_shot::image_codec::srgbRowSource(value.image), level),
                        "selection clipboard ignored its export settings snapshot");
                return std::move(value.image);
            });
        require(hasSamePixels(result, fixture.displaySnapshot().copy(selection)),
                "selection clipboard encoding changed the captured pixels");
    }
}

void selectionClipboardPreservesEffects() {
    ExportFixture fixture;
    require(fixture.isValid(), "clipboard export fixture could not initialize the canvas runtime");
    const QRect selection(12, 8, 37, 29);
    for (int radius : {0, 8}) {
        for (int shadow : {0, 6}) {
            const ScreenshotResultStyle style{radius, shadow, QColor(0, 0, 0, 180)};
            const QImage image = waitForResult(
                [&](QObject* receiver, auto callback) {
                    return fixture.service().requestSelectionClipboard(selection, style, receiver,
                                                                       std::move(callback));
                },
                [&](ScreenshotSelectionClipboardResult result) {
                    require(result.isValid(), "selection clipboard export has no payload");
                    require(result.payload.isValid() && !result.payload.pngBytes().isEmpty(),
                            "clipboard export must prepare PNG and its bitmap fallback");
                    const auto appearance =
                        decodeScreenshotClipboardAppearance(result.payload.appearanceBytes());
                    require(
                        appearance && appearance->rasterSize == result.image.size() &&
                            appearance->borderAppearance ==
                                screenshotSelectionBorderAppearance(selection.size(), style) &&
                            appearance->checkerboardEnabled ==
                                screenshotSelectionNeedsCheckerboard(appearance->borderAppearance),
                        "clipboard appearance differs from a direct selection pin");
                    const auto direct = fixture.service().preparePinnedSelection(selection, style);
                    const auto placement =
                        decodeScreenshotClipboardPlacement(result.payload.placementBytes());
                    require(direct && placement &&
                                placement->windowRect == direct->geometry.nativeGeometry &&
                                placement->rasterSize == result.image.size(),
                            "plain or styled clipboard geometry differs from direct pinning");
                    return std::move(result.image);
                });
            require(image.size() == selection.size() + QSize(shadow * 2, shadow * 2),
                    "clipboard export changed result dimensions");
            if (radius != 0 || shadow != 0) {
                require(image.pixelColor(0, 0).alpha() < 255,
                        "styled clipboard export lost transparency");
            } else {
                require(hasSamePixels(image, fixture.displaySnapshot().copy(selection)),
                        "plain clipboard export changed capture pixels");
            }
        }
    }
}

void fractionalDpiExportsPreserveCapturePixels() {
    for (const auto& [pixels, dpr] :
         {std::pair{QSize(2560, 1440), 1.5}, std::pair{QSize(2560, 1600), 1.5},
          std::pair{QSize(3840, 2160), 2.25}}) {
        ExportFixture fixture(false, pixels, dpr);
        require(fixture.isValid(), "fractional-DPI export fixture could not initialize");
        // Check the complete capture and a crop touching the last physical row/column.
        for (const QRect selection : {QRect(QPoint(), pixels),
                                      QRect(pixels.width() - 101, pixels.height() - 79, 101, 79)}) {
            const QImage expected = fixture.displaySnapshot().copy(selection);
            const QImage result = waitForResult(
                [&](QObject* receiver, auto callback) {
                    return fixture.service().requestSelectionClipboard(selection, {}, receiver,
                                                                       std::move(callback));
                },
                [&](ScreenshotSelectionClipboardResult value) {
                    require(value.isValid(), "fractional-DPI clipboard export failed");
                    require(hasSamePixels(QImage::fromData(value.payload.pngBytes()), expected),
                            "fractional-DPI PNG must preserve every captured pixel and dimension");
                    return std::move(value.image);
                });
            require(hasSamePixels(result, expected),
                    "fractional-DPI export must not stretch or crop the captured image");
        }
    }
}

void pinnedSelectionPreservesFiltersAfterUndoRedo() {
    for (const auto tool : {SnowCanvasTool::RectangleFilter, SnowCanvasTool::PenFilter}) {
        for (const auto type : {SnowCanvasFilterType::Mosaic, SnowCanvasFilterType::GaussianBlur,
                                SnowCanvasFilterType::Grayscale, SnowCanvasFilterType::Inversion,
                                SnowCanvasFilterType::Emboss, SnowCanvasFilterType::SmartErase,
                                SnowCanvasFilterType::Brightness}) {
            ExportFixture fixture;
            require(fixture.isValid(), "filter export fixture could not initialize");
            SnowCanvasWidget canvas(fixture.runtime());
            canvas.resize(fixture.displaySnapshot().size());
            canvas.show();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            require(canvas.setViewportCamera(40.0, 30.0, 1.0), "filter camera setup failed");
            require(canvas.setCanvasTool(tool), "filter tool setup failed");
            SnowCanvasFilterStyle filter;
            filter.type = type;
            filter.strength = 0.7;
            require(canvas.setCanvasFilterStyle(filter, SnowCanvasFilterStylePropertyType |
                                                            SnowCanvasFilterStylePropertyStrength),
                    "filter style setup failed");
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(15, 10),
                              canvas.mapToGlobal(QPoint(15, 10)), Qt::LeftButton, Qt::LeftButton,
                              Qt::NoModifier);
            QMouseEvent move(QEvent::MouseMove, QPointF(65, 45), canvas.mapToGlobal(QPoint(65, 45)),
                             Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(65, 45),
                                canvas.mapToGlobal(QPoint(65, 45)), Qt::LeftButton, Qt::NoButton,
                                Qt::NoModifier);
            QCoreApplication::sendEvent(&canvas, &press);
            QCoreApplication::sendEvent(&canvas, &move);
            QCoreApplication::sendEvent(&canvas, &release);
            require(canvas.canvasHistoryState().canUndo, "filter drawing did not commit");
            require(canvas.undo() && canvas.redo(), "filter undo/redo failed");

            const QRect selection(10, 5, 60, 45);
            const QList<CanvasExportSource> sources{
                {fixture.displaySnapshot(), QRectF(0, 0, 80, 60)}};
            const QImage expected =
                fixture.runtime().renderToImage(selection, selection.size(), sources);
            require(!expected.isNull(), "filter reference image is unavailable");
            const auto request = fixture.service().preparePinnedSelection(selection, {});
            require(request.has_value(), "filter pin request was not prepared");
            bool success = false;
            const QImage pinned =
                waitForPinnedResult(fixture.service(), *request, nullptr, &success);
            require(success && hasSamePixels(pinned, expected),
                    "filter pin worker did not preserve the live document pixels");
        }
    }
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
    require(canvas.setViewportCamera(40.0, 30.0, 1.0), "pinned export canvas camera setup failed");
    require(canvas.setCanvasTool(SnowCanvasTool::Shape),
            "pinned export canvas should activate the shape tool");
    SnowCanvasShapeStyle shapeStyle;
    shapeStyle.stroke = QColor(240, 24, 24);
    shapeStyle.strokeWidth = 4.0;
    require(canvas.setCanvasShapeStylePatch(shapeStyle,
                                            SnowCanvasShapeStylePropertyStrokeColor |
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

    const QImage annotatedCopy = waitForResult(
        [&](QObject* receiver, auto callback) {
            return fixture.service().requestSelectionClipboard(selection, {}, receiver,
                                                               std::move(callback));
        },
        [&](ScreenshotSelectionClipboardResult result) {
            require(result.isValid(), "annotated selection did not prepare a clipboard payload");
            require(result.payload.isValid() && !result.payload.pngBytes().isEmpty(),
                    "clipboard export must prepare PNG and its bitmap fallback");
            const auto direct = fixture.service().preparePinnedSelection(selection, {});
            const auto placement =
                decodeScreenshotClipboardPlacement(result.payload.placementBytes());
            require(direct && placement && placement->windowRect == direct->geometry.nativeGeometry,
                    "annotation must not alter clipboard placement");
            return std::move(result.image);
        });
    require(annotatedCopy.size() == selection.size() &&
                !hasSamePixels(annotatedCopy, fixture.displaySnapshot().copy(selection)),
            "clipboard fixture did not export its annotation");

    const std::optional<ScreenshotPinnedSelectionRequest> prepared =
        fixture.service().preparePinnedSelection(selection, style);
    require(prepared.has_value() && prepared->isPrepared(),
            "pinned selection should be prepared before its image is rendered");

    std::optional<ScreenshotPinnedSelectionRequest> materialized;
    bool pinnedSuccess = false;
    const QImage pinnedImage =
        waitForPinnedResult(fixture.service(), *prepared, &materialized, &pinnedSuccess);
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
void pointSelectionRetainsBackingPixelsAndScalesEffects() {
    ExportFixture fixture(true);
    const QRect selection(0, 0, 40, 30);
    const auto copied = waitForResult(
        [&](QObject* receiver, auto callback) {
            return fixture.service().requestSelectionClipboard(selection, {}, receiver,
                                                               std::move(callback));
        },
        [](ScreenshotSelectionClipboardResult result) {
            require(QImage::fromData(result.payload.pngBytes()).size() == QSize(80, 60),
                    "PNG lost backing resolution");
            return result.image;
        });
    require(hasSamePixels(copied, fixture.displaySnapshot()),
            "point-space clipboard resampled native Retina pixels");
    for (const QRect bounds : {QRect(0, 0, 40, 30), QRect(3, 5, 21, 13)}) {
        for (const int padding : {0, 3}) {
            const ScreenshotResultStyle style{0, padding, Qt::black};
            const auto request = fixture.service().preparePinnedSelection(bounds, style);
            require(request.has_value(), "point-space pin preparation failed");
            const auto layout = ScreenshotResultCompositor::layoutForContent(bounds.size(), style);
            const int inset = layout.effectInsets.left();
            require(
                request->geometry.nativeGeometry == bounds.adjusted(-inset, -inset, inset, inset) &&
                    request->initialWindowSize == layout.outputRect.size(),
                "pin bounds and 100 percent size must remain logical, including shadow padding");
            const QImage result = waitForPinnedResult(fixture.service(), *request);
            const QImage content = fixture.displaySnapshot().copy(
                bounds.x() * 2, bounds.y() * 2, bounds.width() * 2, bounds.height() * 2);
            require(hasSamePixels(result, ScreenshotResultCompositor::compose(
                                              content, {0, padding * 2, Qt::black})),
                    "pin export must preserve Retina pixels and scale the shadow exactly once");
        }
    }
    SnowCanvasWidget canvas(fixture.runtime());
    canvas.resize(40, 30);
    canvas.show();
    QCoreApplication::processEvents();
    require(canvas.setViewportCamera(20, 15, 1), "point annotation viewport");
    require(canvas.setCanvasTool(SnowCanvasTool::Shape), "point annotation tool");
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(3, 3), canvas.mapToGlobal(QPoint(3, 3)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, QPointF(12, 12), canvas.mapToGlobal(QPoint(12, 12)),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(12, 12),
                        canvas.mapToGlobal(QPoint(12, 12)), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(&canvas, &press);
    QCoreApplication::sendEvent(&canvas, &move);
    QCoreApplication::sendEvent(&canvas, &release);
    const auto annotated = waitForResult(
        [&](QObject* receiver, auto callback) {
            return fixture.service().requestSelectionResult(selection, {}, receiver,
                                                            std::move(callback));
        },
        [](QImage image) { return image; });
    require(annotated.size() == copied.size() && !hasSamePixels(annotated, copied),
            "annotation not rendered");
    require(hasSamePixels(annotated.copy(60, 40, 16, 16), copied.copy(60, 40, 16, 16)),
            "annotations downsampled the screenshot background");
    const auto styled = waitForResult(
        [&](QObject* receiver, auto callback) {
            return fixture.service().requestSelectionResult(selection, {3, 2, Qt::black}, receiver,
                                                            std::move(callback));
        },
        [](QImage image) { return image; });
    require(
        hasSamePixels(styled, ScreenshotResultCompositor::compose(annotated, {6, 4, Qt::black})),
        "effects did not follow output scale");
}
} // namespace

void compoundExportsSnapshotTheirGeometry() {
    for (bool points : {false, true}) {
        ExportFixture fixture(points);
        require(fixture.isValid(), "compound export fixture");
        const QRect selection(0, 0, 40, 30);
        ScreenshotResultStyle style;
        const QRegion shape = QRegion(selection).subtracted(QRect(10, 10, 10, 10));
        style.region = shape;
        const auto result = waitForResult(
            [&](QObject* receiver, auto callback) {
                const bool scheduled = fixture.service().requestSelectionResult(
                    selection, style, receiver, std::move(callback));
                style.region = QRegion(selection);
                return scheduled;
            },
            [](QImage image) { return image; });
        const int scale = points ? 2 : 1;
        require(result.size() == selection.size() * scale &&
                    result.pixelColor(15 * scale, 15 * scale).alpha() == 0 &&
                    result.pixelColor(5 * scale, 5 * scale).alpha() == 255,
                "asynchronous export must retain its shape snapshot at backing scale");
        style.region = shape;
        const auto clipboardPlacement =
            fixture.service().prepareClipboardPlacement(selection, style);
        const auto clipboard = waitForResult(
            [&](QObject* receiver, auto callback) {
                const bool scheduled = fixture.service().requestSelectionClipboard(
                    selection, style, receiver, std::move(callback));
                style.shadowWidth = 8;
                style.region = QRegion(selection);
                return scheduled;
            },
            [&](ScreenshotSelectionClipboardResult value) {
                const auto metadata =
                    decodeScreenshotClipboardPlacement(value.payload.placementBytes());
                require(metadata && clipboardPlacement &&
                            metadata->windowRect == clipboardPlacement->windowRect,
                        "asynchronous clipboard export observes later geometry edits");
                return value.image;
            });
        require(hasSamePixels(clipboard, result), "clipboard must use the same compound mask");
        style.shadowWidth = 0;
        style.region = shape;
        const auto request = fixture.service().preparePinnedSelection(selection, style);
        require(request && request->resultStyle.region == style.region,
                "pin request carries region snapshot");
        require(hasSamePixels(waitForPinnedResult(fixture.service(), *request), result),
                "pin output must use the same compound mask");
    }
}

void exportWorkerReleasesSharedDerivedContours() {
    ExportFixture fixture;
    QPainterPath ellipse;
    ellipse.addEllipse(QRectF(0, 0, 40, 30));
    ScreenshotResultStyle style;
    style.region = ScreenshotRegionGeometry::fromPath(ellipse, ScreenshotRegionType::Curve)
                       .subtracted(QRect(10, 10, 10, 10));
    style.shadowWidth = 3;
    style.region->clearDerivedCache();
    const auto coldBytes = style.region->retainedBytesEstimate();
    for (const bool clipboard : {false, true}) {
        require(!style.region->path(1.5).isEmpty(), "warm export contour");
        require(style.region->retainedBytesEstimate() > coldBytes, "warm contour has storage");
        const auto result =
            clipboard ? waitForResult(
                            [&](QObject* receiver, auto callback) {
                                return fixture.service().requestSelectionClipboard(
                                    QRect(0, 0, 40, 30), style, receiver, std::move(callback));
                            },
                            [](ScreenshotSelectionClipboardResult value) { return value.image; })
                      : waitForResult(
                            [&](QObject* receiver, auto callback) {
                                return fixture.service().requestSelectionResult(
                                    QRect(0, 0, 40, 30), style, receiver, std::move(callback));
                            },
                            [](QImage value) { return value; });
        require(!result.isNull(), "export survives cache cleanup");
        require(style.region->retainedBytesEstimate() == coldBytes,
                "worker releases shared derived contours before publishing its result");
    }
}

void exportWorkerReleasesDocumentSnapshotsBeforeCompletion() {
    ExportFixture fixture;
    const QRect selection(0, 0, 40, 30);
    const auto expected = fixture.displaySnapshot().copy(selection);
    const auto placement = fixture.service().prepareClipboardPlacement(selection, {});
    require(placement.has_value(), "prepare snapshot lifetime clipboard placement");
    for (int mode = 0; mode < 3; ++mode) {
        auto snapshot = fixture.runtime().smartEraseSnapshot();
        const std::weak_ptr<const SnowCanvasSmartEraseSnapshot::Data> lifetime = snapshot.data;
        fixture.runtime().restoreSmartEraseSnapshot(snapshot);
        snapshot = {};
        const auto result = waitForResult(
            [&](QObject* receiver, auto callback) {
                bool scheduled = false;
                if (mode == 0) {
                    scheduled = fixture.service().requestSelectionResult(selection, {}, receiver,
                                                                         std::move(callback));
                } else if (mode == 1) {
                    scheduled = fixture.service().requestSelectionClipboard(
                        selection, {}, receiver,
                        [placement,
                         callback = std::move(callback)](ScreenshotSelectionClipboardResult value) {
                            const auto metadata =
                                decodeScreenshotClipboardPlacement(value.payload.placementBytes());
                            require(metadata && metadata->windowRect == placement->windowRect &&
                                        metadata->rasterSize == value.image.size(),
                                    "releasing export snapshots must preserve clipboard placement");
                            callback(std::move(value.image));
                        });
                } else {
                    const auto request = fixture.service().preparePinnedSelection(selection, {});
                    require(request.has_value(), "prepare snapshot lifetime pin request");
                    scheduled = fixture.service().schedulePinnedSelection(
                        *request, receiver,
                        [receiver, callback = std::move(callback)](
                            ScreenshotPinnedSelectionRequest,
                            ScreenshotPinnedSelectionResultHandle value) {
                            require(value.subscribe(
                                        receiver,
                                        [callback](bool success, QImage image) {
                                            require(success,
                                                    "snapshot lifetime pin export must succeed");
                                            callback(std::move(image));
                                        }),
                                    "subscribe snapshot lifetime pin result");
                        });
                }
                require(fixture.runtime().clearDocumentPreservingViewports(),
                        "retire the capture document while its export is queued");
                return scheduled;
            },
            [&](QImage image) {
                require(lifetime.expired(),
                        "completed exports must release worker and request snapshot ownership");
                return image;
            });
        require(hasSamePixels(result, expected), "released export state must leave output intact");
    }
}

void clipboardPlacementMatchesDirectPin() {
    for (const bool points : {false, true}) {
        ExportFixture fixture(points);
        for (const int shadow : {0, 4}) {
            ScreenshotResultStyle style;
            style.shadowWidth = shadow;
            style.cornerRadius = 6;
            QPainterPath ellipse;
            ellipse.addEllipse(QRectF(0, 0, 20, 15));
            style.region = ScreenshotRegionGeometry::fromPath(ellipse, ScreenshotRegionType::Curve);
            const QRect selection(10, 10, 20, 15);
            const auto direct = fixture.service().preparePinnedSelection(selection, style);
            require(direct.has_value(), "direct placement must be prepared");
            const auto snapshot = fixture.service().prepareClipboardPlacement(selection, style);
            require(snapshot && snapshot->windowRect == direct->geometry.nativeGeometry &&
                        snapshot->placement.windowSize == direct->initialWindowSize,
                    "clipboard snapshot differs from direct pin geometry");
            bool received = false;
            const auto result = waitForResult(
                [&](QObject* receiver, auto callback) {
                    return fixture.service().requestSelectionClipboard(selection, style, receiver,
                                                                       std::move(callback));
                },
                [&](ScreenshotSelectionClipboardResult value) {
                    const auto metadata =
                        decodeScreenshotClipboardPlacement(value.payload.placementBytes());
                    require(metadata && metadata->windowRect == direct->geometry.nativeGeometry &&
                                metadata->rasterSize == value.image.size(),
                            "composited clipboard raster loses the direct pin's platform geometry");
                    const auto appearance =
                        decodeScreenshotClipboardAppearance(value.payload.appearanceBytes());
                    require(appearance && appearance->rasterSize == value.image.size() &&
                                appearance->borderAppearance ==
                                    screenshotSelectionBorderAppearance(selection.size(), style),
                            "scaled clipboard raster loses its reference outline");
                    received = true;
                    return value.image;
                });
            require(received && !result.isNull(), "clipboard placement export must complete");
        }
    }
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--fractional-dpi"))) {
        fractionalDpiExportsPreserveCapturePixels();
        return EXIT_SUCCESS;
    }
    screenshotExportsRetainSrgb();
    clipboardPlacementMatchesDirectPin();
    selectionClipboardSnapshotsExportSettings();
    fractionalDpiExportsPreserveCapturePixels();
    exportWorkerReleasesSharedDerivedContours();
    exportWorkerReleasesDocumentSnapshotsBeforeCompletion();
    compoundExportsSnapshotTheirGeometry();
    pointSelectionRetainsBackingPixelsAndScalesEffects();
    styledClipboardResultRetainsPngTransparency();
    selectionClipboardPreservesEffects();
    pinnedSelectionMaterializesCompositedImage();
    pinnedSelectionPreservesFiltersAfterUndoRedo();
    std::cout << "All screenshot export service tests passed\n";
    return EXIT_SUCCESS;
}
