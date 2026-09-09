#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snowimageqtcodec.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/diagnostics/diagnostics.h"
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>

#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QThread>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <windows.h>
#endif

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void processUntil(const std::function<bool()>& predicate) {
    for (int iteration = 0; iteration < 1000 && !predicate(); ++iteration) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(predicate(), "timed out waiting for export artifact completion");
}

QImage testImage() {
    QImage image(QSize(37, 19), QImage::Format_RGBA8888);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixelColor(x, y, QColor((x * 9) % 256, (y * 17) % 256, (x + y) % 256, 255));
        }
    }
    return image;
}

ScreenshotImageRowSource rowSourceFor(const QImage& source, std::function<bool()> cancellation) {
    const QImage image = source.convertToFormat(QImage::Format_RGBA8888);
    const qsizetype rowBytes = static_cast<qsizetype>(image.width()) * 4;
    ScreenshotImageRowSource rows;
    rows.size = image.size();
    rows.cancellationRequested = std::move(cancellation);
    rows.readRows = [image, rowBytes](int firstRow, int rowCount, qsizetype stride,
                                      uchar* destination, qsizetype destinationSize) {
        if (firstRow < 0 || rowCount <= 0 || rowCount > image.height() - firstRow ||
            stride < rowBytes || destination == nullptr ||
            destinationSize < stride * (rowCount - 1) + rowBytes) {
            return false;
        }
        for (int row = 0; row < rowCount; ++row) {
            std::memcpy(destination + static_cast<qsizetype>(row) * stride,
                        image.constScanLine(firstRow + row), static_cast<std::size_t>(rowBytes));
        }
        return true;
    };
    return rows;
}

void clipboardAndSaveShareCanonicalEncoding() {
    const QImage image = testImage();
    for (bool rowBacked : {false, true}) {
        std::atomic_int materializations = 0;
        std::atomic_int rowFactories = 0;
        ScreenshotExportSource::RowSourceFactory factory;
        if (rowBacked) {
            factory = [&rowFactories, image](std::function<bool()> cancellation) {
                ++rowFactories;
                return rowSourceFor(image, std::move(cancellation));
            };
        }
        ScreenshotExportArtifact artifact(ScreenshotExportSource::fromProducer(
            [&materializations, image](const ScreenshotExportCancellation&) {
                ++materializations;
                return image;
            },
            factory));
        QTemporaryDir directory;
        require(directory.isValid(), "temporary save directory unavailable");
        QObject receiver;
        int callbacks = 0;
        QByteArray canonical;
        QByteArray clipboard;
        QString path;
        require(artifact.requestCanonicalPng(&receiver,
                                             [&](ScreenshotExportEncodingResult result) {
                                                 require(result.succeeded(),
                                                         "canonical PNG failed");
                                                 canonical = result.image.bytes();
                                                 ++callbacks;
                                             }),
                "canonical request rejected");
        require(artifact.requestClipboard(&receiver,
                                          [&](ScreenshotExportClipboardResult result) {
                                              require(result.succeeded(),
                                                      "clipboard preparation failed");
                                              clipboard = result.payload.pngBytes();
                                              ++callbacks;
                                          }),
                "clipboard request rejected");
        require(artifact.requestAutomaticSave(
                    &receiver, {directory.path()}, ScreenshotImageFileFormat::Png,
                    QStringLiteral("shared"),
                    [&](ScreenshotExportTaskResult result) {
                        require(result.succeeded(), "automatic PNG save failed");
                        path = result.savedPath;
                        ++callbacks;
                    }),
                "save request rejected");
        processUntil([&] { return callbacks == 3; });
        require(canonical.constData() == clipboard.constData(),
                "clipboard did not reuse the canonical PNG buffer");
        QFile saved(path);
        require(saved.open(QIODevice::ReadOnly) && saved.readAll() == canonical,
                "saved PNG differs from clipboard/history encoding");
        require(materializations == (rowBacked ? 0 : 1),
                "image source was materialized more than once");
        require(rowFactories == (rowBacked ? 1 : 0),
                "PNG consumers did not reuse the cached row source");
        require(artifact.requestClipboard(&receiver,
                                          [&](ScreenshotExportClipboardResult result) {
                                              require(result.succeeded() &&
                                                          result.payload.pngBytes().constData() ==
                                                              canonical.constData(),
                                                      "cached clipboard request re-encoded PNG");
                                              ++callbacks;
                                          }),
                "cached clipboard request rejected");
        processUntil([&] { return callbacks == 4; });
        require(rowFactories == (rowBacked ? 1 : 0),
                "cached clipboard request recreated the row source");
    }
}

void nonPngSaveReadsPixelsWithoutEncodingPng() {
    const QImage image = testImage();
    for (bool rowBacked : {false, true}) {
        std::atomic_int imageCalls = 0;
        std::atomic_int rowCalls = 0;
        ScreenshotExportSource::RowSourceFactory factory;
        if (rowBacked) {
            factory = [&rowCalls, image](std::function<bool()> cancellation) {
                ++rowCalls;
                return rowSourceFor(image, std::move(cancellation));
            };
        }
        ScreenshotExportArtifact artifact(ScreenshotExportSource::fromProducer(
            [&imageCalls, image](const ScreenshotExportCancellation&) {
                ++imageCalls;
                return image;
            },
            factory));
        QTemporaryDir directory;
        QObject receiver;
        int callbacks = 0;
        require(artifact.requestAutomaticSave(
                    &receiver, {directory.path()}, ScreenshotImageFileFormat::Webp,
                    QStringLiteral("pixels"),
                    [&](ScreenshotExportTaskResult result) {
                        require(result.succeeded() && QFile::exists(result.savedPath),
                                "non-PNG save failed");
                        ++callbacks;
                    }),
                "non-PNG save request rejected");
        processUntil([&] { return callbacks == 1; });
        require(rowCalls == (rowBacked ? 1 : 0) && imageCalls == (rowBacked ? 0 : 1),
                "non-PNG save performed an extra encoding or materialization");
        require(artifact.requestAutomaticSave(
                    &receiver, {directory.path()}, ScreenshotImageFileFormat::Png,
                    QStringLiteral("bad/name"),
                    [&](ScreenshotExportTaskResult result) {
                        require(!result.succeeded() && !result.error.isEmpty(),
                                "invalid save filename did not report failure");
                        ++callbacks;
                    }),
                "invalid save request must complete with an error");
        processUntil([&] { return callbacks == 2; });
    }
}

void imageRequestsShareOneAsyncLoad() {
    int loadCount = 0;
    std::function<void(QImage)> finishLoad;
    ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImageLoader(
        [&loadCount, &finishLoad](QObject*, std::function<void(QImage)> callback) {
            ++loadCount;
            finishLoad = std::move(callback);
            return true;
        }));
    QObject firstReceiver;
    QObject secondReceiver;
    int callbacks = 0;
    require(artifact.requestImage(&firstReceiver,
                                  [&callbacks](ScreenshotExportImageResult result) {
                                      require(result.succeeded(),
                                              "first shared image request failed");
                                      ++callbacks;
                                  }) &&
                artifact.requestImage(&secondReceiver,
                                      [&callbacks](ScreenshotExportImageResult result) {
                                          require(result.succeeded(),
                                                  "second shared image request failed");
                                          ++callbacks;
                                      }),
            "shared image requests were rejected");
    require(loadCount == 1 && finishLoad, "concurrent image requests started duplicate loads");
    finishLoad(testImage());
    require(callbacks == 2, "shared image result did not fan out to both subscribers");
}

void canonicalEncodingCoalescesAndPreservesBufferIdentity() {
    const QImage image = testImage();
    std::atomic_int rowFactoryCount = 0;
    std::atomic_int imageProducerCount = 0;
    ScreenshotExportArtifact artifact(ScreenshotExportSource::fromProducer(
        [&imageProducerCount, image](const ScreenshotExportCancellation&) {
            ++imageProducerCount;
            return image;
        },
        [&rowFactoryCount, image](std::function<bool()> cancellation) {
            ++rowFactoryCount;
            return rowSourceFor(image, std::move(cancellation));
        }));
    QObject receiver;
    int callbacks = 0;
    const QByteArray* firstBuffer = nullptr;
    const QByteArray* secondBuffer = nullptr;
    require(artifact.requestCanonicalPng(
                &receiver,
                [&callbacks, &firstBuffer](ScreenshotExportEncodingResult result) {
                    require(result.succeeded(), "first canonical encoding failed");
                    firstBuffer = result.image.sharedBytes().get();
                    ++callbacks;
                }) &&
                artifact.requestCanonicalPng(
                    &receiver,
                    [&callbacks, &secondBuffer](ScreenshotExportEncodingResult result) {
                        require(result.succeeded(), "second canonical encoding failed");
                        secondBuffer = result.image.sharedBytes().get();
                        ++callbacks;
                    }),
            "canonical encoding requests were rejected");
    processUntil([&callbacks]() { return callbacks == 2; });
    require(rowFactoryCount == 1, "exact canonical encoding requests were not coalesced");
    require(imageProducerCount == 0,
            "row-backed canonical encoding materialized the image unnecessarily");
    require(firstBuffer != nullptr && firstBuffer == secondBuffer,
            "canonical encoding subscribers did not receive the same immutable buffer");
}

void encodingFailureFansOutOnce() {
    std::atomic_int rowFactoryCount = 0;
    ScreenshotExportArtifact artifact(ScreenshotExportSource::fromProducer(
        {}, [&rowFactoryCount](std::function<bool()> cancellation) {
            ++rowFactoryCount;
            ScreenshotImageRowSource rows;
            rows.size = QSize(16, 8);
            rows.cancellationRequested = std::move(cancellation);
            rows.readRows = [](int, int, qsizetype, uchar*, qsizetype) { return false; };
            return rows;
        }));
    QObject receiver;
    int callbacks = 0;
    const auto completion = [&callbacks](ScreenshotExportEncodingResult result) {
        require(!result.succeeded() && !result.error.isEmpty(),
                "failed encoding was reported as successful");
        ++callbacks;
    };
    require(artifact.requestCanonicalPng(&receiver, completion) &&
                artifact.requestCanonicalPng(&receiver, completion),
            "failed canonical encoding requests were rejected prematurely");
    require(artifact.requestClipboard(&receiver,
                                      [&](ScreenshotExportClipboardResult result) {
                                          require(
                                              !result.succeeded(),
                                              "failed PNG encoding produced a clipboard payload");
                                          ++callbacks;
                                      }),
            "clipboard failure request rejected");
    processUntil([&callbacks]() { return callbacks == 3; });
    require(rowFactoryCount == 1, "encoding failure was recomputed for each subscriber");
}

void rowRequestsCoalesceAndReuseBackingImage() {
    const QImage image = testImage();
    std::atomic_int imageCalls = 0;
    ScreenshotExportArtifact artifact(ScreenshotExportSource::fromProducer(
        [&imageCalls, image](const ScreenshotExportCancellation&) {
            ++imageCalls;
            return image;
        }));
    QObject firstReceiver;
    QObject secondReceiver;
    int callbacks = 0;
    const uchar* firstPixels = nullptr;
    const uchar* secondPixels = nullptr;
    require(artifact.requestRowSource(&firstReceiver,
                                      [&](ScreenshotImageRowSource source, QString error) {
                                          require(error.isEmpty() && source.isValid() &&
                                                      !source.backingImage.isNull(),
                                                  "first cached row request failed");
                                          firstPixels = source.backingImage.constBits();
                                          ++callbacks;
                                      }) &&
                artifact.requestRowSource(&secondReceiver,
                                          [&](ScreenshotImageRowSource source, QString error) {
                                              require(error.isEmpty() && source.isValid() &&
                                                          !source.backingImage.isNull(),
                                                      "second cached row request failed");
                                              secondPixels = source.backingImage.constBits();
                                              ++callbacks;
                                          }),
            "concurrent row requests were rejected");
    processUntil([&] { return callbacks == 2; });
    require(imageCalls == 1 && firstPixels != nullptr && firstPixels == secondPixels,
            "row requests did not share one converted immutable backing image");

    require(artifact.requestRowSource(
                &firstReceiver,
                [&](ScreenshotImageRowSource source, QString error) {
                    require(error.isEmpty() && source.backingImage.constBits() == firstPixels,
                            "ready row request did not reuse the cached source");
                    ++callbacks;
                }),
            "ready row request was rejected");
    require(callbacks == 3 && imageCalls == 1, "ready row result was recomputed");
}

void canonicalPngAdoptionHandlesPendingAndFailedEncoding() {
    const QImage image = testImage();
    auto prepared = snow_shot::storage::PreparedPngImage::fromBytes(
        image.size(), snow_shot::image_codec::encodePng(image));
    require(prepared.has_value(), "adoption PNG fixture is invalid");

    std::atomic_bool entered = false;
    std::atomic_bool release = false;
    std::atomic_int factories = 0;
    ScreenshotExportArtifact pending(ScreenshotExportSource::fromProducer(
        {}, [&image, &entered, &release, &factories](std::function<bool()> cancellation) {
            ++factories;
            ScreenshotImageRowSource rows = rowSourceFor(image, cancellation);
            const auto read = rows.readRows;
            rows.readRows = [read, &entered, &release, cancellation = std::move(cancellation)](
                                int first, int count, qsizetype stride, uchar* destination,
                                qsizetype capacity) {
                entered.store(true, std::memory_order_release);
                while (!release.load(std::memory_order_acquire) &&
                       !(cancellation && cancellation())) {
                    QThread::msleep(1);
                }
                return !(cancellation && cancellation()) &&
                       read(first, count, stride, destination, capacity);
            };
            return rows;
        }));
    require(!pending.adoptCanonicalPng(*prepared),
            "canonical PNG was adopted before the row source was known");
    QObject receiver;
    int rowCallbacks = 0;
    require(pending.requestRowSource(&receiver,
                                     [&](ScreenshotImageRowSource source, QString error) {
                                         require(source.isValid() && error.isEmpty(),
                                                 "pending adoption row source failed");
                                         ++rowCallbacks;
                                     }),
            "pending adoption row request rejected");
    processUntil([&] { return rowCallbacks == 1; });

    int encodingCallbacks = 0;
    const QByteArray* adoptedBytes = prepared->sharedBytes().get();
    require(pending.requestCanonicalPng(
                &receiver,
                [&](ScreenshotExportEncodingResult result) {
                    require(result.succeeded() && result.image.sharedBytes().get() == adoptedBytes,
                            "pending PNG subscriber did not receive the adopted buffer");
                    ++encodingCallbacks;
                }),
            "pending canonical encoding request rejected");
    processUntil([&] { return entered.load(std::memory_order_acquire); });
    require(pending.adoptCanonicalPng(*prepared), "pending canonical PNG adoption failed");
    release.store(true, std::memory_order_release);
    processUntil([&] { return encodingCallbacks == 1; });
    require(factories == 1, "pending adoption recreated the row source");

    auto alternate = snow_shot::storage::PreparedPngImage::fromBytes(
        image.size(), snow_shot::image_codec::encodePng(image.flipped(Qt::Horizontal)));
    require(alternate.has_value() && pending.adoptCanonicalPng(*alternate),
            "ready canonical PNG adoption was rejected");
    require(pending.requestCanonicalPng(
                &receiver,
                [&](ScreenshotExportEncodingResult result) {
                    require(result.image.sharedBytes().get() == adoptedBytes,
                            "ready canonical PNG buffer was replaced by adoption");
                    ++encodingCallbacks;
                }),
            "ready canonical request rejected");
    require(encodingCallbacks == 2, "ready canonical result was not delivered immediately");

    ScreenshotExportArtifact failed(
        ScreenshotExportSource::fromProducer({}, [image](std::function<bool()> cancellation) {
            ScreenshotImageRowSource rows = rowSourceFor(image, std::move(cancellation));
            rows.readRows = [](int, int, qsizetype, uchar*, qsizetype) { return false; };
            return rows;
        }));
    rowCallbacks = 0;
    require(failed.requestRowSource(&receiver,
                                    [&](ScreenshotImageRowSource source, QString error) {
                                        require(source.isValid() && error.isEmpty(),
                                                "failed fixture row source failed");
                                        ++rowCallbacks;
                                    }),
            "failed fixture row request rejected");
    processUntil([&] { return rowCallbacks == 1; });
    int failures = 0;
    require(failed.requestCanonicalPng(&receiver,
                                       [&](ScreenshotExportEncodingResult result) {
                                           require(!result.succeeded(),
                                                   "broken source unexpectedly encoded");
                                           ++failures;
                                       }),
            "failed canonical request rejected");
    processUntil([&] { return failures == 1; });
    require(failed.adoptCanonicalPng(*prepared), "failed canonical phase did not recover");
    require(failed.requestCanonicalPng(
                &receiver,
                [&](ScreenshotExportEncodingResult result) {
                    require(result.succeeded() && result.image.sharedBytes().get() == adoptedBytes,
                            "recovered canonical result is incorrect");
                    ++failures;
                }),
            "recovered canonical request rejected");
    require(failures == 2, "recovered canonical result was not delivered immediately");
    failed.cancel();
    require(!failed.adoptCanonicalPng(*prepared),
            "cancelled artifact accepted canonical PNG adoption");
}

void rowRequestFailureAndCancellationFanOut() {
    std::atomic_bool failureEntered = false;
    std::atomic_bool releaseFailure = false;
    std::atomic_int factories = 0;
    ScreenshotExportArtifact failed(ScreenshotExportSource::fromProducer(
        {}, [&failureEntered, &releaseFailure, &factories](std::function<bool()>) {
            ++factories;
            failureEntered.store(true, std::memory_order_release);
            while (!releaseFailure.load(std::memory_order_acquire))
                QThread::msleep(1);
            return ScreenshotImageRowSource{};
        }));
    QObject firstReceiver;
    QObject secondReceiver;
    int failures = 0;
    const auto failedCallback = [&failures](ScreenshotImageRowSource source, QString error) {
        require(!source.isValid() && !error.isEmpty(),
                "failed row-source request produced an invalid result contract");
        ++failures;
    };
    require(failed.requestRowSource(&firstReceiver, failedCallback) &&
                failed.requestRowSource(&secondReceiver, failedCallback),
            "failed row-source subscribers were rejected");
    processUntil([&] { return failureEntered.load(std::memory_order_acquire); });
    releaseFailure.store(true, std::memory_order_release);
    processUntil([&] { return failures == 2; });
    require(factories == 1, "failed row-source subscribers did not share one factory call");

    std::atomic_bool cancellationEntered = false;
    std::atomic_bool cancellationObserved = false;
    ScreenshotExportArtifact cancelled(ScreenshotExportSource::fromProducer(
        {}, [&cancellationEntered, &cancellationObserved](std::function<bool()> cancellation) {
            cancellationEntered.store(true, std::memory_order_release);
            while (!(cancellation && cancellation()))
                QThread::msleep(1);
            cancellationObserved.store(true, std::memory_order_release);
            return ScreenshotImageRowSource{};
        }));
    int cancelledCallbacks = 0;
    require(cancelled.requestRowSource(&firstReceiver,
                                       [&cancelledCallbacks](ScreenshotImageRowSource, QString) {
                                           ++cancelledCallbacks;
                                       }) &&
                cancelled.requestRowSource(
                    &secondReceiver, [&cancelledCallbacks](ScreenshotImageRowSource,
                                                           QString) { ++cancelledCallbacks; }),
            "cancelled row-source subscribers were rejected");
    processUntil([&] { return cancellationEntered.load(std::memory_order_acquire); });
    cancelled.cancel();
    processUntil([&] { return cancellationObserved.load(std::memory_order_acquire); });
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    require(cancelledCallbacks == 0 && cancelled.isCancelled(),
            "cancelled row-source request delivered pending subscribers");
}

void cancellationSuppressesPendingCallbacks() {
    std::function<void(QImage)> finishLoad;
    ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImageLoader(
        [&finishLoad](QObject*, std::function<void(QImage)> callback) {
            finishLoad = std::move(callback);
            return true;
        }));
    QObject receiver;
    int callbacks = 0;
    require(artifact.requestImage(&receiver,
                                  [&callbacks](ScreenshotExportImageResult) { ++callbacks; }),
            "cancellation image request was rejected");
    artifact.cancel();
    require(static_cast<bool>(finishLoad),
            "cancellation fixture did not retain its loader callback");
    finishLoad(testImage());
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    require(callbacks == 0 && artifact.isCancelled(),
            "cancelled artifact delivered a pending callback");
}

void pinnedViewportSourceRendersExpectedPixels() {
    SnowCanvasRuntime runtime;
    const QByteArray session = runtime.serializeDocumentSession();
    require(!session.isEmpty(), "runtime session could not be serialized");
    QImage background(QSize(64, 48), QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(12, 24, 36, 255));
    ScreenshotPinnedViewportExportSource source{
        session, std::move(background), QRectF(0.0, 0.0, 64.0, 48.0), QSize(64, 48), {},
    };
    ScreenshotExportArtifact artifact(
        ScreenshotExportSource::fromPinnedViewport(std::move(source)));
    QObject receiver;
    QImage rendered;
    require(artifact.requestImage(&receiver,
                                  [&rendered](ScreenshotExportImageResult result) {
                                      require(result.succeeded(),
                                              "pinned viewport artifact render failed");
                                      rendered = std::move(result.image);
                                  }),
            "pinned viewport artifact render was not scheduled");
    processUntil([&rendered]() { return !rendered.isNull(); });
    require(rendered.size() == QSize(64, 48) &&
                rendered.pixelColor(rendered.width() / 2, rendered.height() / 2) ==
                    QColor(12, 24, 36, 255),
            "pinned viewport artifact did not return the rendered pixels");
}

void quickSaveUsesOnlyConfiguredOutput() {
    QTemporaryDir directory;
    require(directory.isValid(), "quick save fixture unavailable");
    const QString executable = directory.filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "quick save fixture bin unavailable");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({executable, directory.path(), 60000}).success,
            "quick save isolated settings unavailable");
    const snow_shot::storage::ScreenshotSettings settings;
    const QString output = directory.filePath(QStringLiteral("new/nested"));
    require(settings.setImageSaveDirectory(output) &&
                settings.setAutoSaveFilenameFormat(QStringLiteral("Quick_output")) &&
                settings.setLastManualSaveDirectory(directory.path()) &&
                settings.setLastManualSaveFormat(QStringLiteral("jpeg")),
            "quick save settings setup failed");
    QObject receiver;
    const QImage image = testImage();
    const auto save = [&](ScreenshotExportArtifact& artifact) {
        std::optional<ScreenshotExportTaskResult> result;
        require(
            artifact.requestQuickSave(
                &receiver, [&](ScreenshotExportTaskResult saved) { result = std::move(saved); }),
            "quick save request was rejected");
        processUntil([&] { return result.has_value(); });
        return *result;
    };
    for (const QString& format :
         {QStringLiteral("png"), QStringLiteral("jpeg"), QStringLiteral("bmp"),
          QStringLiteral("webp"), QStringLiteral("jxl"), QStringLiteral("avif")}) {
        require(settings.setImageFormat(format), "quick save format setup failed");
        ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImage(image));
        const auto first = save(artifact);
        require(first.succeeded() && QFileInfo(first.savedPath).absolutePath() == output &&
                    QFileInfo(first.savedPath).baseName() == QStringLiteral("Quick_output") &&
                    !first.savedPath.contains(QLatin1Char('{')) &&
                    QFileInfo(first.savedPath).suffix() ==
                        ScreenshotImageFileService::extension(
                            ScreenshotImageFileService::formatForKey(format)),
                "quick save must create the configured directory and use format/name settings");
        const auto second = save(artifact);
        require(second.succeeded() && first.savedPath != second.savedPath &&
                    QFileInfo(second.savedPath).baseName().endsWith(QStringLiteral("_1")) &&
                    QFileInfo::exists(first.savedPath),
                "quick save must preserve existing files on collision");
        if (format == QStringLiteral("png")) {
            require(QImage(first.savedPath).convertToFormat(QImage::Format_RGBA8888) == image,
                    "quick save must preserve composed source pixels");
        }
    }
    require(settings.lastManualSaveDirectory() == directory.path() &&
                settings.lastManualSaveFormat() == QStringLiteral("jpeg"),
            "quick save must not update manual-save settings");

    require(settings.setImageFormat(QStringLiteral("png")), "streaming format setup failed");
    ScreenshotExportArtifact rows(
        ScreenshotExportSource::fromProducer({}, [image](std::function<bool()> cancellation) {
            return rowSourceFor(image, std::move(cancellation));
        }));
    const auto rowSave = save(rows);
    require(rowSave.succeeded() &&
                QImage(rowSave.savedPath).convertToFormat(QImage::Format_RGBA8888) == image,
            "row-backed quick-save must export the complete source without losing pixels");
    // Fill the bounded queue while retaining already encoded pixels so rejection tests the save
    // job.
    processUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; });
    auto release = std::make_shared<std::atomic_bool>(false);
    std::vector<ScreenshotExportJobHandle> blockers;
    for (int i = 0; i < 64; ++i) {
        auto job = ScreenshotExportCoordinator::shared().submit(
            &receiver, ScreenshotExportCoordinator::Priority::Background,
            [release](const ScreenshotExportCancellation& cancellation) {
                while (!release->load() && !cancellation.isCancellationRequested())
                    QThread::msleep(1);
                return ScreenshotExportTaskResult{};
            },
            [](ScreenshotExportTaskResult) {});
        if (!job.isValid())
            break;
        blockers.push_back(job);
    }
    std::optional<ScreenshotExportTaskResult> rejected;
    const bool accepted = rows.requestQuickSave(
        &receiver, [&](ScreenshotExportTaskResult result) { rejected = std::move(result); });
    release->store(true);
    processUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; });
    require(accepted && rejected && !rejected->succeeded() &&
                rejected->failureStage == ScreenshotExportFailureStage::Queue,
            "a full export queue must report a save failure");
    require(save(rows).succeeded(), "a rejected quick-save must allow retry");
    QFile blocker(directory.filePath(QStringLiteral("blocked")));
    require(blocker.open(QIODevice::WriteOnly) && blocker.write("unchanged") == 9,
            "quick save failure fixture unavailable");
    blocker.close();
    for (const QString& invalid : {QString(), blocker.fileName() + QStringLiteral("/child")}) {
        require(settings.setImageSaveDirectory(invalid), "invalid directory setup failed");
        ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImage(image));
        const auto failed = save(artifact);
        require(!failed.succeeded() && failed.savedPath.isEmpty() && !failed.error.isEmpty(),
                "an unusable configured directory must fail without saving to a fallback");
    }
    ScreenshotExportArtifact cancelled(ScreenshotExportSource::fromImage(image));
    int callbacks = 0;
    require(cancelled.requestQuickSave(&receiver, [&](ScreenshotExportTaskResult) { ++callbacks; }),
            "empty-directory request was rejected");
    cancelled.cancel();
    QCoreApplication::processEvents();
    require(callbacks == 0 &&
                !cancelled.requestQuickSave(&receiver, [](ScreenshotExportTaskResult) {}),
            "cancelled quick saves must reject new requests and suppress pending callbacks");
    storage.shutdown();
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        QTemporaryDir logs;
        snow_shot::diagnostics::DiagnosticsOptions logging;
        logging.directories = {logs.path()};
        logging.enableCrashCapture = false;
        logging.mirrorToConsole = false;
        auto& diagnostics = snow_shot::diagnostics::DiagnosticsService::instance();
        require(diagnostics.initialize(logging), "export diagnostics must initialize");
        {
            QObject receiver;
            ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImageLoader(
                [](QObject*, std::function<void(QImage)> callback) {
                    callback({});
                    return true;
                }));
            bool finished = false;
            require(artifact.requestImage(&receiver,
                                          [&](ScreenshotExportImageResult result) {
                                              require(!result.succeeded(),
                                                      "empty export must report failure");
                                              finished = true;
                                          }),
                    "failed image load must be accepted asynchronously");
            processUntil([&] { return finished; });
            require(diagnostics.flush(), "export diagnostics must flush");
            QFile file(diagnostics.status().currentFile);
            require(file.open(QIODevice::ReadOnly), "export log must be readable");
            bool found = false;
            for (const auto& line : file.readAll().split('\n')) {
                const auto record = QJsonDocument::fromJson(line).object();
                if (record.value(QStringLiteral("event")) !=
                    QStringLiteral("export.image_finished"))
                    continue;
                const auto fields = record.value(QStringLiteral("fields")).toObject();
                found = fields.value(QStringLiteral("operation")) == artifact.diagnosticId() &&
                        fields.value(QStringLiteral("outcome")) == QStringLiteral("failed") &&
                        !record.value(QStringLiteral("message")).toString().isEmpty();
            }
            require(found, "export failure must preserve its cause and correlation identifier");
            ScreenshotExportArtifact cancelled(ScreenshotExportSource::fromImageLoader(
                [](QObject*, std::function<void(QImage)>) { return true; }));
            require(cancelled.requestImage(
                        &receiver,
                        [](ScreenshotExportImageResult) {
                            require(false, "cancelled exports must not deliver callbacks");
                        }),
                    "pending cancellation fixture must accept image loading");
            cancelled.cancel();
            cancelled.cancel();
            require(diagnostics.flush(), "cancellation diagnostics must flush");
            file.seek(0);
            int cancellations = 0;
            for (const auto& line : file.readAll().split('\n')) {
                const auto record = QJsonDocument::fromJson(line).object();
                if (record.value(QStringLiteral("event")) == QStringLiteral("export.cancelled") &&
                    record.value(QStringLiteral("fields"))
                            .toObject()
                            .value(QStringLiteral("operation")) == cancelled.diagnosticId())
                    ++cancellations;
            }
            require(cancellations == 1, "a pending export must log cancellation exactly once");
        }
        diagnostics.shutdown();
        if (application.arguments().contains(QStringLiteral("--quick-save-only"))) {
            quickSaveUsesOnlyConfiguredOutput();
            return EXIT_SUCCESS;
        }
        quickSaveUsesOnlyConfiguredOutput();
        clipboardAndSaveShareCanonicalEncoding();
        nonPngSaveReadsPixelsWithoutEncodingPng();
        imageRequestsShareOneAsyncLoad();
        canonicalEncodingCoalescesAndPreservesBufferIdentity();
        encodingFailureFansOutOnce();
        rowRequestsCoalesceAndReuseBackingImage();
        canonicalPngAdoptionHandlesPendingAndFailedEncoding();
        rowRequestFailureAndCancellationFanOut();
        cancellationSuppressesPendingCallbacks();
        pinnedViewportSourceRendersExpectedPixels();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
