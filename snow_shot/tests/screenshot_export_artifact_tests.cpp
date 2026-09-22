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
#include <QScopeGuard>

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

void clipboardReusesFastEncodingWithoutChangingFileCompression() {
    const QImage image = testImage();
    const QByteArray fastPng =
        snow_shot::image_codec::encodePng(snow_shot::image_codec::srgbRowSource(image), 0);
    for (auto compression : {ScreenshotCompressionLevel::Low, ScreenshotCompressionLevel::Medium,
                             ScreenshotCompressionLevel::High}) {
        ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImage(image), compression);
        QObject receiver;
        QByteArray clipboard;
        require(artifact.requestClipboard(
                    &receiver,
                    [&](ScreenshotExportClipboardResult result) {
                        require(result.succeeded() && result.payload.pngBytes() == fastPng,
                                "clipboard encoding depends on the file compression setting");
                        clipboard = result.payload.pngBytes();
                    }),
                "clipboard request rejected");
        processUntil([&] { return !clipboard.isEmpty(); });
        QByteArray cached;
        require(artifact.requestPng(
                    &receiver, ScreenshotCompressionLevel::Low,
                    [&](ScreenshotExportEncodingResult result) { cached = result.image.bytes(); }),
                "fast PNG request rejected");
        require(cached.constData() == clipboard.constData(),
                "clipboard did not retain its encoded PNG for subsequent outputs");
        QByteArray canonical;
        require(artifact.requestCanonicalPng(&receiver,
                                             [&](ScreenshotExportEncodingResult result) {
                                                 require(result.succeeded(),
                                                         "canonical encoding failed");
                                                 canonical = result.image.bytes();
                                             }),
                "canonical request rejected after clipboard preparation");
        processUntil([&] { return !canonical.isEmpty(); });
        const int level =
            ScreenshotImageFileService::encodeOptions(
                ScreenshotImageFileFormat::Png, ScreenshotImageEncodingOptions{100, compression})
                .compression_level;
        require(canonical == snow_shot::image_codec::encodePng(
                                 snow_shot::image_codec::srgbRowSource(image), level),
                "clipboard changed the compression requested by file/history output");
        require((canonical.constData() == clipboard.constData()) == (level == 0),
                "matching PNG outputs did not share the same buffer");
    }
}

void pendingHighCompressionDoesNotBecomeAClipboardDependency() {
    const QImage image = testImage();
    auto firstRead = std::make_shared<std::atomic_bool>(true);
    auto entered = std::make_shared<std::atomic_bool>(false);
    auto released = std::make_shared<std::atomic_bool>(false);
    const auto release = qScopeGuard([released] { released->store(true); });
    auto rows = rowSourceFor(image, {});
    rows.readRows = [read = rows.readRows, firstRead, entered, released](
                        int first, int count, qsizetype stride, uchar* target, qsizetype capacity) {
        if (firstRead->exchange(false)) {
            entered->store(true);
            while (!released->load())
                QThread::msleep(1);
        }
        return read(first, count, stride, target, capacity);
    };
    ScreenshotExportArtifact artifact(ScreenshotExportSource::fromProducer(
                                          {},
                                          [rows](std::function<bool()> cancellation) mutable {
                                              rows.cancellationRequested = std::move(cancellation);
                                              return rows;
                                          }),
                                      ScreenshotCompressionLevel::High);
    QObject receiver;
    bool encoded = false;
    require(artifact.requestCanonicalPng(&receiver,
                                         [&](ScreenshotExportEncodingResult result) {
                                             require(result.succeeded(),
                                                     "high-compression fixture failed");
                                             encoded = true;
                                         }),
            "high-compression fixture rejected");
    processUntil([&] { return entered->load(); });
    bool copied = false;
    require(artifact.requestClipboard(
                &receiver,
                [&](ScreenshotExportClipboardResult result) {
                    require(result.succeeded() &&
                                result.payload.pngBytes() ==
                                    snow_shot::image_codec::encodePng(
                                        snow_shot::image_codec::srgbRowSource(image), 0),
                            "clipboard subscribed to a pending compressed PNG");
                    copied = true;
                }),
            "clipboard request rejected during high-compression encoding");
    processUntil([&] { return copied; });
    require(!encoded, "clipboard waited for the blocked compressed encoding");
    released->store(true);
    processUntil([&] { return encoded; });
}

void clipboardReusesAlreadyEncodedPngAtAnyCompression() {
    const QImage image = testImage();
    for (auto compression : {ScreenshotCompressionLevel::Low, ScreenshotCompressionLevel::Medium,
                             ScreenshotCompressionLevel::High}) {
        ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImage(image), compression);
        QObject receiver;
        QByteArray encoded;
        require(artifact.requestCanonicalPng(&receiver,
                                             [&](ScreenshotExportEncodingResult result) {
                                                 require(result.succeeded(),
                                                         "existing PNG encoding failed");
                                                 encoded = result.image.bytes();
                                             }),
                "existing PNG request rejected");
        processUntil([&] { return !encoded.isEmpty(); });
        bool copied = false;
        require(artifact.requestClipboard(
                    &receiver,
                    [&](ScreenshotExportClipboardResult result) {
                        require(result.succeeded() &&
                                    result.payload.pngBytes().constData() == encoded.constData(),
                                "clipboard re-encoded an already available PNG");
                        copied = true;
                    }),
                "clipboard reuse request rejected");
        processUntil([&] { return copied; });
    }
}

void failedPngRequestsCanRetryWithoutInvalidatingOtherLevels() {
    const QImage image = testImage();
    auto fail = std::make_shared<std::atomic_bool>(true);
    auto rows = rowSourceFor(image, {});
    rows.readRows = [read = rows.readRows, fail](int first, int count, qsizetype stride,
                                                 uchar* target, qsizetype capacity) {
        return !fail->load() && read(first, count, stride, target, capacity);
    };
    ScreenshotExportArtifact artifact(ScreenshotExportSource::fromProducer(
                                          {},
                                          [rows](std::function<bool()> cancellation) mutable {
                                              rows.cancellationRequested = std::move(cancellation);
                                              return rows;
                                          }),
                                      ScreenshotCompressionLevel::High);
    QObject receiver;
    int callbacks = 0;
    require(artifact.requestCanonicalPng(
                &receiver,
                [&](ScreenshotExportEncodingResult result) {
                    require(!result.succeeded() && !result.error.isEmpty(),
                            "failed high-compression PNG did not report its error");
                    ++callbacks;
                }),
            "failing PNG request rejected");
    processUntil([&] { return callbacks == 1; });
    fail->store(false);
    QByteArray clipboard;
    require(artifact.requestClipboard(
                &receiver,
                [&](ScreenshotExportClipboardResult result) {
                    require(result.succeeded(),
                            "file compression failure poisoned clipboard encoding");
                    clipboard = result.payload.pngBytes();
                    ++callbacks;
                }),
            "clipboard request rejected after file encoding failure");
    processUntil([&] { return callbacks == 2; });
    require(artifact.requestCanonicalPng(
                &receiver,
                [&](ScreenshotExportEncodingResult result) {
                    require(result.succeeded() &&
                                result.image.bytes() ==
                                    snow_shot::image_codec::encodePng(
                                        snow_shot::image_codec::srgbRowSource(image), 9),
                            "a new PNG request could not retry a transient failure");
                    ++callbacks;
                }),
            "retry PNG request rejected");
    processUntil([&] { return callbacks == 3; });
    require(artifact.requestPng(
                &receiver, ScreenshotCompressionLevel::Low,
                [&](ScreenshotExportEncodingResult result) {
                    require(result.image.bytes().constData() == clipboard.constData(),
                            "retrying a different level invalidated the clipboard cache");
                    ++callbacks;
                }),
            "cached clipboard PNG request rejected");
    require(callbacks == 4, "successful clipboard encoding was not cached");
}

void matchingRequestsSurviveSubscriberDestruction() {
    std::function<void(QImage)> finishLoad;
    ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImageLoader(
                                          [&](QObject*, std::function<void(QImage)> callback) {
                                              finishLoad = std::move(callback);
                                              return true;
                                          }),
                                      ScreenshotCompressionLevel::Low);
    QObject receiver;
    auto destroyed = std::make_unique<QObject>();
    int callbacks = 0;
    QByteArray first;
    QByteArray second;
    require(artifact.requestPng(destroyed.get(), ScreenshotCompressionLevel::Low,
                                [](ScreenshotExportEncodingResult) {
                                    require(false, "destroyed PNG subscriber called");
                                }) &&
                artifact.requestCanonicalPng(&receiver,
                                             [&](ScreenshotExportEncodingResult result) {
                                                 require(result.succeeded(),
                                                         "surviving PNG subscriber failed");
                                                 first = result.image.bytes();
                                                 ++callbacks;
                                             }) &&
                artifact.requestClipboard(&receiver,
                                          [&](ScreenshotExportClipboardResult result) {
                                              require(result.succeeded(),
                                                      "surviving clipboard subscriber failed");
                                              second = result.payload.pngBytes();
                                              ++callbacks;
                                          }),
            "concurrent PNG requests rejected");
    destroyed.reset();
    finishLoad(testImage());
    processUntil([&] { return callbacks == 2; });
    require(first.constData() == second.constData(),
            "subscriber destruction cancelled or duplicated the shared PNG encoding");
}

void manualPngSavesUseTheirRequestedCompression() {
    const QImage image = testImage();
    ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImage(image),
                                      ScreenshotCompressionLevel::High);
    QObject receiver;
    QTemporaryDir directory;
    for (auto compression : {ScreenshotCompressionLevel::Low, ScreenshotCompressionLevel::Medium,
                             ScreenshotCompressionLevel::High}) {
        QByteArray png;
        QString path;
        int callbacks = 0;
        require(artifact.requestPng(&receiver, compression,
                                    [&](ScreenshotExportEncodingResult result) {
                                        require(result.succeeded(),
                                                "requested PNG encoding failed");
                                        png = result.image.bytes();
                                        ++callbacks;
                                    }),
                "requested PNG encoding rejected");
        require(
            artifact.requestSaveToPath(&receiver, directory.filePath(QStringLiteral("manual.png")),
                                       ScreenshotImageFileFormat::Png,
                                       ScreenshotImageEncodingOptions{100, compression},
                                       [&](ScreenshotExportTaskResult result) {
                                           require(result.succeeded(), "manual PNG save failed");
                                           path = result.savedPath;
                                           ++callbacks;
                                       }),
            "manual PNG save rejected");
        processUntil([&] { return callbacks == 2; });
        const int level =
            ScreenshotImageFileService::encodeOptions(
                ScreenshotImageFileFormat::Png, ScreenshotImageEncodingOptions{100, compression})
                .compression_level;
        QFile file(path);
        require(file.open(QIODevice::ReadOnly) && file.readAll() == png &&
                    png == snow_shot::image_codec::encodePng(
                               snow_shot::image_codec::srgbRowSource(image), level),
                "manual PNG save ignored its requested compression");
        require(artifact.requestPng(&receiver, compression,
                                    [&](ScreenshotExportEncodingResult result) {
                                        require(result.image.bytes().constData() == png.constData(),
                                                "repeated PNG request re-encoded cached bytes");
                                        ++callbacks;
                                    }),
                "cached PNG request rejected");
        require(callbacks == 3, "cached PNG was not delivered immediately");
    }
}

void clipboardUsesFastEncodingAndSavesShareCanonicalEncoding(
    ScreenshotCompressionLevel saveCompression, bool clipboardFirst) {
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
        ScreenshotExportArtifact artifact(
            ScreenshotExportSource::fromProducer(
                [&materializations, image](const ScreenshotExportCancellation&) {
                    ++materializations;
                    return image;
                },
                factory),
            saveCompression);
        QTemporaryDir directory;
        require(directory.isValid(), "temporary save directory unavailable");
        QObject receiver;
        int callbacks = 0;
        QByteArray canonical;
        QByteArray clipboard;
        QString path;
        QString systemPath;
        const auto requestCanonical = [&] {
            require(artifact.requestCanonicalPng(&receiver,
                                                 [&](ScreenshotExportEncodingResult result) {
                                                     require(result.succeeded(),
                                                             "canonical PNG failed");
                                                     canonical = result.image.bytes();
                                                     ++callbacks;
                                                 }),
                    "canonical request rejected");
        };
        if (!clipboardFirst)
            requestCanonical();
        require(artifact.requestClipboard(&receiver,
                                          [&](ScreenshotExportClipboardResult result) {
                                              require(result.succeeded(),
                                                      "clipboard preparation failed");
                                              clipboard = result.payload.pngBytes();
                                              ++callbacks;
                                          }),
                "clipboard request rejected");
        if (clipboardFirst)
            requestCanonical();
        require(artifact.requestAutomaticSave(
                    &receiver, {directory.path()}, ScreenshotImageFileFormat::Png,
                    QStringLiteral("shared"),
                    [&](ScreenshotExportTaskResult result) {
                        require(result.succeeded(), "automatic PNG save failed");
                        path = result.savedPath;
                        ++callbacks;
                    }),
                "save request rejected");
        require(artifact.requestSaveToPath(
                    &receiver, directory.filePath(QStringLiteral("system.png")),
                    ScreenshotImageFileFormat::Png,
                    ScreenshotImageEncodingOptions{100, saveCompression},
                    [&](ScreenshotExportTaskResult result) {
                        require(result.succeeded(), "system-dialog PNG save failed");
                        systemPath = result.savedPath;
                        ++callbacks;
                    }),
                "system-dialog save request rejected");
        processUntil([&] { return callbacks == 4; });
        require(clipboard == snow_shot::image_codec::encodePng(
                                 snow_shot::image_codec::srgbRowSource(image), 0),
                "clipboard did not use the fastest PNG encoding");
        if (saveCompression == ScreenshotCompressionLevel::Low)
            require(canonical.constData() == clipboard.constData(),
                    "concurrent clipboard/save/history requests encoded level 0 more than once");
        QFile saved(path);
        require(saved.open(QIODevice::ReadOnly) && saved.readAll() == canonical,
                "saved PNG differs from history encoding");
        QFile systemSaved(systemPath);
        require(systemSaved.open(QIODevice::ReadOnly) && systemSaved.readAll() == canonical,
                "system-dialog PNG save did not reuse the canonical history bytes");
        require(materializations == (rowBacked ? 0 : 1),
                "image source was materialized more than once");
        require(rowFactories == (rowBacked ? 1 : 0),
                "PNG consumers did not reuse the cached row source");
        require(artifact.requestClipboard(
                    &receiver,
                    [&](ScreenshotExportClipboardResult result) {
                        require(result.succeeded() &&
                                    result.payload.pngBytes().constData() == clipboard.constData(),
                                "repeated clipboard request changed the fast PNG encoding");
                        ++callbacks;
                    }),
                "cached clipboard request rejected");
        processUntil([&] { return callbacks == 5; });
        require(rowFactories == (rowBacked ? 1 : 0),
                "cached clipboard request recreated the row source");
        require(artifact.requestSaveToPath(
                    &receiver, directory.filePath(QStringLiteral("compatible.bmp")),
                    ScreenshotImageFileFormat::Bmp,
                    ScreenshotImageEncodingOptions{100, saveCompression},
                    [&](ScreenshotExportTaskResult result) {
                        QFile file(result.savedPath);
                        require(
                            result.succeeded() && file.open(QIODevice::ReadOnly) &&
                                file.readAll().startsWith("BM") &&
                                QImage(result.savedPath).convertToFormat(QImage::Format_RGBA8888) ==
                                    image,
                            "non-PNG output reused incompatible canonical PNG bytes");
                        ++callbacks;
                    }),
                "non-PNG path save request rejected");
        processUntil([&] { return callbacks == 6; });
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

void pngCacheBudgetEvictsLeastRecentlyUsedResults() {
    const QImage image = testImage();
    const auto rows = snow_shot::image_codec::srgbRowSource(image);
    const auto low = snow_shot::image_codec::encodePng(rows, 0);
    const auto medium = snow_shot::image_codec::encodePng(
        rows, ScreenshotImageFileService::encodeOptions(
                  ScreenshotImageFileFormat::Png,
                  ScreenshotImageEncodingOptions{100, ScreenshotCompressionLevel::Medium})
                  .compression_level);
    const auto high = snow_shot::image_codec::encodePng(rows, 9);
    ScreenshotExportArtifact artifact(
        ScreenshotExportSource::fromImage(image), ScreenshotCompressionLevel::Low,
        ScreenshotExportArtifact::PngCachePolicy{low.size() + qMax(medium.size(), high.size())});
    QObject receiver;
    auto request = [&](ScreenshotCompressionLevel level) {
        QByteArray result;
        require(artifact.requestPng(&receiver, level,
                                    [&](ScreenshotExportEncodingResult encoded) {
                                        require(encoded.succeeded(),
                                                "bounded cache encoding failed");
                                        result = encoded.image.bytes();
                                    }),
                "bounded cache request rejected");
        processUntil([&] { return !result.isEmpty(); });
        return result;
    };
    const auto first = request(ScreenshotCompressionLevel::Low);
    const auto second = request(ScreenshotCompressionLevel::Medium);
    require(artifact.cachedPng(ScreenshotCompressionLevel::Low).bytes().constData() ==
                first.constData(),
            "cached low PNG lost its buffer");
    const auto third = request(ScreenshotCompressionLevel::High);
    require(!artifact.cachedPng(ScreenshotCompressionLevel::Medium).isValid() &&
                artifact.cachedPng(ScreenshotCompressionLevel::Low).isValid() &&
                artifact.cachedPng(ScreenshotCompressionLevel::High).isValid(),
            "PNG cache did not evict the least recently used encoding");
    require(second == medium && third == high && first == low,
            "cache eviction invalidated an outstanding consumer's bytes");
    require(request(ScreenshotCompressionLevel::Medium) == medium,
            "evicted PNG could not be encoded again");
}

void fileOutputsStreamBeyondCacheBudget() {
    const QImage image = testImage();
    QTemporaryDir directory;
    require(directory.isValid(), "streaming fixture directory unavailable");
    QObject receiver;
    for (auto compression : {ScreenshotCompressionLevel::Low, ScreenshotCompressionLevel::Medium,
                             ScreenshotCompressionLevel::High}) {
        ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImage(image), compression,
                                          ScreenshotExportArtifact::PngCachePolicy{1});
        int callbacks = 0;
        QString automaticPath;
        const QString manualPath = directory.filePath(QStringLiteral("manual.png"));
        require(artifact.requestSaveToPath(&receiver, manualPath, ScreenshotImageFileFormat::Png,
                                           ScreenshotImageEncodingOptions{100, compression},
                                           [&](ScreenshotExportTaskResult result) {
                                               require(result.succeeded(),
                                                       "streaming manual save failed");
                                               ++callbacks;
                                           }) &&
                    artifact.requestAutomaticSave(
                        &receiver, {directory.path()}, ScreenshotImageFileFormat::Png,
                        QStringLiteral("automatic"),
                        [&](ScreenshotExportTaskResult result) {
                            require(result.succeeded(), "streaming automatic save failed");
                            automaticPath = result.savedPath;
                            ++callbacks;
                        }),
                "streaming file request rejected");
        processUntil([&] { return callbacks == 2; });
        const int level =
            ScreenshotImageFileService::encodeOptions(
                ScreenshotImageFileFormat::Png, ScreenshotImageEncodingOptions{100, compression})
                .compression_level;
        const auto expected =
            snow_shot::image_codec::encodePng(snow_shot::image_codec::srgbRowSource(image), level);
        for (const auto& path : {manualPath, automaticPath}) {
            QFile file(path);
            require(file.open(QIODevice::ReadOnly) && file.readAll() == expected,
                    "streaming file changed the requested PNG encoding");
        }
        require(!artifact.cachedPng(compression).isValid(),
                "streaming file output populated the in-memory cache");
    }
}

void oversizedPngStillFansOutWithoutRetention() {
    ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImage(testImage()),
                                      ScreenshotCompressionLevel::Low,
                                      ScreenshotExportArtifact::PngCachePolicy{1});
    QObject receiver;
    int callbacks = 0;
    QByteArray first;
    QByteArray second;
    for (auto* bytes : {&first, &second}) {
        require(artifact.requestPng(&receiver, ScreenshotCompressionLevel::Low,
                                    [&, bytes](ScreenshotExportEncodingResult result) {
                                        require(result.succeeded(), "oversized PNG failed");
                                        *bytes = result.image.bytes();
                                        ++callbacks;
                                    }),
                "oversized PNG request rejected");
    }
    processUntil([&] { return callbacks == 2; });
    require(first.constData() == second.constData() &&
                !artifact.cachedPng(ScreenshotCompressionLevel::Low).isValid(),
            "oversized PNG was retained or concurrent requests did not share its bytes");
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
    require(artifact.requestPng(&receiver, ScreenshotCompressionLevel::Low,
                                [&](ScreenshotExportEncodingResult) { ++callbacks; }) &&
                artifact.requestPng(&receiver, ScreenshotCompressionLevel::High,
                                    [&](ScreenshotExportEncodingResult) { ++callbacks; }) &&
                artifact.requestClipboard(&receiver,
                                          [&](ScreenshotExportClipboardResult) { ++callbacks; }),
            "pending PNG/clipboard cancellation requests were rejected");
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
                settings.setLastManualSaveFormat(QStringLiteral("jpeg")) &&
                settings.setCompressionLevel(QStringLiteral("high")) && settings.setImageQuality(0),
            "quick save settings setup failed");
    require(settings.setPdfPageSize(QStringLiteral("a4_landscape")),
            "PDF page setting must persist");
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
          QStringLiteral("webp"), QStringLiteral("jxl"), QStringLiteral("avif"),
          QStringLiteral("pdf")}) {
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
        if (format == QStringLiteral("pdf")) {
            QFile file(first.savedPath);
            require(file.open(QIODevice::ReadOnly), "quick save PDF must be readable");
            const QByteArray pdf = file.readAll();
            require(pdf.startsWith("%PDF-1.7") &&
                        pdf.contains("/MediaBox [0 0 841.88976378 595.27559055]") &&
                        !pdf.contains("/DCTDecode"),
                    "quick save must honor the PDF setting and default to lossless");
        }
        if (format == QStringLiteral("png")) {
            QFile file(first.savedPath);
            require(file.open(QIODevice::ReadOnly) &&
                        file.readAll() == snow_shot::image_codec::encodePng(image, 9) &&
                        QImage(first.savedPath).convertToFormat(QImage::Format_RGBA8888) == image,
                    "quick save must preserve pixels and honor global PNG compression");
        }
        if (format == QStringLiteral("jpeg")) {
            const QString referencePath = directory.filePath(QStringLiteral("jpeg-reference.jpg"));
            require(ScreenshotImageFileService::write(
                        image, referencePath, ScreenshotImageFileFormat::Jpeg, {}, {},
                        ScreenshotImageEncodingOptions{100, ScreenshotCompressionLevel::High})
                        .succeeded(),
                    "quick-save JPEG reference could not be encoded");
            QFile saved(first.savedPath);
            QFile reference(referencePath);
            require(saved.open(QIODevice::ReadOnly) && reference.open(QIODevice::ReadOnly) &&
                        saved.readAll() == reference.readAll(),
                    "global image quality must not lower automatic or quick-save quality");
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
        clipboardReusesFastEncodingWithoutChangingFileCompression();
        manualPngSavesUseTheirRequestedCompression();
        pendingHighCompressionDoesNotBecomeAClipboardDependency();
        clipboardReusesAlreadyEncodedPngAtAnyCompression();
        failedPngRequestsCanRetryWithoutInvalidatingOtherLevels();
        matchingRequestsSurviveSubscriberDestruction();
        for (auto compression :
             {ScreenshotCompressionLevel::Low, ScreenshotCompressionLevel::Medium,
              ScreenshotCompressionLevel::High}) {
            for (bool clipboardFirst : {false, true})
                clipboardUsesFastEncodingAndSavesShareCanonicalEncoding(compression,
                                                                        clipboardFirst);
        }
        nonPngSaveReadsPixelsWithoutEncodingPng();
        imageRequestsShareOneAsyncLoad();
        canonicalEncodingCoalescesAndPreservesBufferIdentity();
        encodingFailureFansOutOnce();
        rowRequestsCoalesceAndReuseBackingImage();
        pngCacheBudgetEvictsLeastRecentlyUsedResults();
        oversizedPngStillFansOutWithoutRetention();
        fileOutputsStreamBeyondCacheBudget();
        rowRequestFailureAndCancellationFanOut();
        cancellationSuppressesPendingCallbacks();
        pinnedViewportSourceRendersExpectedPixels();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
