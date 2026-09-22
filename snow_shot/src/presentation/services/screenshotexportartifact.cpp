#include "snow_shot/presentation/screenshotexportartifact.h"

#include "snowimageqtcodec.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/diagnostics/diagnostics.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QThread>
#include <QUuid>

#include <snow/image/codec.h>
#include <snow/image/format.h>

#include <array>
#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace {
enum class RequestPhase { Empty, Pending, Ready, Failed };

template <typename Result, typename Callback>
void dispatchResult(QObject* receiver, Callback callback, Result result) {
    if (receiver == nullptr || !callback) {
        return;
    }
    const QPointer<QObject> guardedReceiver(receiver);
    if (QThread::currentThread() == receiver->thread()) {
        if (!guardedReceiver.isNull()) {
            callback(std::move(result));
        }
        return;
    }
    static_cast<void>(QMetaObject::invokeMethod(
        receiver,
        [guardedReceiver, callback = std::move(callback), result = std::move(result)]() mutable {
            if (!guardedReceiver.isNull()) {
                callback(std::move(result));
            }
        },
        Qt::QueuedConnection));
}

int pngCompression(ScreenshotCompressionLevel level) {
    return ScreenshotImageFileService::encodeOptions(ScreenshotImageFileFormat::Png,
                                                     ScreenshotImageEncodingOptions{100, level})
        .compression_level;
}

ScreenshotExportEncodingResult encodePng(const ScreenshotImageRowSource& source,
                                         int compressionLevel) {
    if (!source.isValid()) {
        return {{}, QStringLiteral("The screenshot row source is unavailable")};
    }
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly)) {
        return {{}, QStringLiteral("The screenshot PNG buffer could not be opened")};
    }
    snow::image::EncodeOptions options = ScreenshotImageFileService::encodeOptions(
        ScreenshotImageFileFormat::Png, ScreenshotImageEncodingOptions{});
    options.compression_level = compressionLevel;
    QString error;
    if (!snow_shot::image_codec::encodeToDevice(source, &buffer, snow::image::Format::png, options,
                                                &error)) {
        return {{},
                error.isEmpty() ? QStringLiteral("The screenshot could not be PNG encoded")
                                : error};
    }
    auto prepared = snow_shot::storage::PreparedPngImage::fromBytes(
        source.size, std::make_shared<const QByteArray>(std::move(bytes)));
    if (!prepared.has_value()) {
        return {{}, QStringLiteral("The encoded screenshot PNG is invalid")};
    }
    return {std::move(*prepared), {}};
}

void dispatchRowSource(QObject* receiver, ScreenshotExportArtifact::RowSourceCallback callback,
                       ScreenshotImageRowSource source, QString error) {
    if (receiver == nullptr || !callback)
        return;
    const QPointer<QObject> guardedReceiver(receiver);
    if (QThread::currentThread() == receiver->thread()) {
        if (!guardedReceiver.isNull())
            callback(std::move(source), std::move(error));
        return;
    }
    static_cast<void>(QMetaObject::invokeMethod(
        receiver,
        [guardedReceiver, callback = std::move(callback), source = std::move(source),
         error = std::move(error)]() mutable {
            if (!guardedReceiver.isNull())
                callback(std::move(source), std::move(error));
        },
        Qt::QueuedConnection));
}

ScreenshotImageRowSource withCancellation(const ScreenshotImageRowSource& source,
                                          std::function<bool()> cancellation) {
    ScreenshotImageRowSource result = source;
    const auto sourceCancellation = source.cancellationRequested;
    result.cancellationRequested = [sourceCancellation, cancellation = std::move(cancellation)] {
        return (sourceCancellation && sourceCancellation()) || (cancellation && cancellation());
    };
    return result;
}
} // namespace

ScreenshotExportSource ScreenshotExportSource::fromImage(QImage image) {
    return fromProducer(
        [image = std::move(image)](const ScreenshotExportCancellation& cancellation) {
            return cancellation.isCancellationRequested() ? QImage{} : image;
        });
}

ScreenshotExportSource ScreenshotExportSource::fromImageLoader(ImageLoader loader) {
    ScreenshotExportSource source;
    source.m_imageLoader = std::move(loader);
    return source;
}

ScreenshotExportSource ScreenshotExportSource::fromProducer(ImageProducer producer,
                                                            RowSourceFactory rowSourceFactory) {
    ScreenshotExportSource source;
    source.m_imageProducer = std::move(producer);
    source.m_rowSourceFactory = std::move(rowSourceFactory);
    return source;
}

bool ScreenshotExportSource::isValid() const {
    return static_cast<bool>(m_imageLoader) || static_cast<bool>(m_imageProducer) ||
           static_cast<bool>(m_rowSourceFactory);
}

struct ScreenshotExportArtifact::Impl final {
    struct ImageSubscriber final {
        QPointer<QObject> receiver;
        ImageCallback callback;
    };
    struct EncodingSubscriber final {
        QPointer<QObject> receiver;
        EncodingCallback callback;
    };
    struct RowSourceSubscriber final {
        QPointer<QObject> receiver;
        RowSourceCallback callback;
    };

    Impl(ScreenshotExportSource value, ScreenshotCompressionLevel compression,
         PngCachePolicy policy)
        : source(std::move(value)), compressionLevel(compression),
          maximumPngBytes(std::max(qsizetype{0}, policy.maximumBytes)) {}

    ScreenshotExportSource source;
    const ScreenshotCompressionLevel compressionLevel;
    const qsizetype maximumPngBytes;
    quint64 pngUseSerial = 0;
    const QString diagnosticId = QUuid::createUuid().toString(QUuid::Id128);
    mutable QMutex mutex;
    bool cancelled = false;
    RequestPhase imagePhase = RequestPhase::Empty;
    QImage image;
    QString imageError;
    std::vector<ImageSubscriber> imageSubscribers;
    RequestPhase rowSourcePhase = RequestPhase::Empty;
    ScreenshotImageRowSource rowSource;
    QString rowSourceError;
    std::vector<RowSourceSubscriber> rowSourceSubscribers;
    struct PngEncoding final {
        RequestPhase phase = RequestPhase::Empty;
        snow_shot::storage::PreparedPngImage image;
        std::vector<EncodingSubscriber> subscribers;
        ScreenshotExportJobHandle job;
        quint64 lastUsed = 0;
    };
    // The source pixels and PNG options other than compression are immutable.
    std::array<PngEncoding, 10> pngEncodings;

    // Called under mutex. Pending encodings are never evicted; their subscribers
    // receive the result even when it is too large to retain in this cache.
    void retainPng(int level, const snow_shot::storage::PreparedPngImage& encoded) {
        auto& target = pngEncodings[level];
        target.phase = RequestPhase::Empty;
        target.image = {};
        const qsizetype bytes = encoded.bytes().size();
        if (bytes > maximumPngBytes)
            return;
        qsizetype retained = 0;
        for (const auto& entry : pngEncodings)
            retained += entry.image.bytes().size();
        while (retained > maximumPngBytes - bytes) {
            PngEncoding* oldest = nullptr;
            for (auto& entry : pngEncodings) {
                if (entry.phase == RequestPhase::Ready &&
                    (!oldest || entry.lastUsed < oldest->lastUsed))
                    oldest = &entry;
            }
            if (!oldest)
                break;
            retained -= oldest->image.bytes().size();
            oldest->image = {};
            oldest->phase = RequestPhase::Empty;
        }
        target.phase = RequestPhase::Ready;
        target.image = encoded;
        target.lastUsed = ++pngUseSerial;
    }
    ScreenshotExportJobHandle imageJob;
    ScreenshotExportJobHandle rowSourceJob;
    std::vector<ScreenshotExportJobHandle> outputJobs;
};

ScreenshotExportArtifact::ScreenshotExportArtifact(ScreenshotExportSource source, QObject* parent)
    : ScreenshotExportArtifact(std::move(source),
                               ScreenshotImageFileService::compressionLevelForKey(
                                   snow_shot::storage::ScreenshotSettings().compressionLevel()),
                               parent) {}

ScreenshotExportArtifact::ScreenshotExportArtifact(ScreenshotExportSource source,
                                                   ScreenshotCompressionLevel compressionLevel,
                                                   QObject* parent)
    : ScreenshotExportArtifact(std::move(source), compressionLevel, PngCachePolicy{}, parent) {}

ScreenshotExportArtifact::ScreenshotExportArtifact(ScreenshotExportSource source,
                                                   ScreenshotCompressionLevel compressionLevel,
                                                   PngCachePolicy cachePolicy, QObject* parent)
    : QObject(parent),
      m_impl(std::make_unique<Impl>(std::move(source), compressionLevel, cachePolicy)) {}

QString ScreenshotExportArtifact::diagnosticId() const {
    return m_impl->diagnosticId;
}

ScreenshotExportArtifact::~ScreenshotExportArtifact() {
    cancel();
}

bool ScreenshotExportArtifact::requestImage(QObject* receiver, ImageCallback callback) {
    if (receiver == nullptr || !callback || m_impl == nullptr) {
        return false;
    }
    ScreenshotExportImageResult ready;
    bool dispatchReady = false;
    bool start = false;
    {
        QMutexLocker lock(&m_impl->mutex);
        if (m_impl->cancelled || !m_impl->source.isValid()) {
            return false;
        }
        if (m_impl->imagePhase == RequestPhase::Ready) {
            ready.image = m_impl->image;
            dispatchReady = true;
        } else if (m_impl->imagePhase == RequestPhase::Failed) {
            ready.error = m_impl->imageError;
            dispatchReady = true;
        } else {
            m_impl->imageSubscribers.push_back({receiver, std::move(callback)});
            if (m_impl->imagePhase == RequestPhase::Empty) {
                m_impl->imagePhase = RequestPhase::Pending;
                start = true;
            }
        }
    }
    if (dispatchReady) {
        dispatchResult(receiver, std::move(callback), std::move(ready));
    } else if (start) {
        startImage();
    }
    return true;
}

bool ScreenshotExportArtifact::requestRowSource(QObject* receiver, RowSourceCallback callback) {
    if (receiver == nullptr || !callback || m_impl == nullptr)
        return false;
    ScreenshotImageRowSource ready;
    QString readyError;
    bool dispatchReady = false;
    bool start = false;
    {
        QMutexLocker lock(&m_impl->mutex);
        if (m_impl->cancelled || !m_impl->source.isValid())
            return false;
        if (m_impl->rowSourcePhase == RequestPhase::Ready) {
            ready = m_impl->rowSource;
            dispatchReady = true;
        } else if (m_impl->rowSourcePhase == RequestPhase::Failed) {
            readyError = m_impl->rowSourceError;
            dispatchReady = true;
        } else {
            m_impl->rowSourceSubscribers.push_back({receiver, std::move(callback)});
            if (m_impl->rowSourcePhase == RequestPhase::Empty) {
                m_impl->rowSourcePhase = RequestPhase::Pending;
                start = true;
            }
        }
    }
    if (dispatchReady) {
        dispatchRowSource(receiver, std::move(callback), std::move(ready), std::move(readyError));
    } else if (start) {
        startRowSource();
    }
    return true;
}

void ScreenshotExportArtifact::startRowSource() {
    const QPointer<ScreenshotExportArtifact> guarded(this);
    if (!m_impl->source.m_rowSourceFactory) {
        if (!requestImage(this, [guarded](ScreenshotExportImageResult result) mutable {
                if (guarded.isNull())
                    return;
                if (result.succeeded())
                    guarded->startRowSourceFromImage(std::move(result.image));
                else
                    guarded->completeRowSource({}, std::move(result.error));
            })) {
            completeRowSource({},
                              QStringLiteral("The screenshot image request could not be started"));
        }
        return;
    }

    const auto factory = m_impl->source.m_rowSourceFactory;
    auto rows = std::make_shared<ScreenshotImageRowSource>();
    const ScreenshotExportJobHandle job = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Foreground,
        [factory, rows](const ScreenshotExportCancellation& cancellation) {
            *rows = withCancellation(
                factory([cancellation] { return cancellation.isCancellationRequested(); }),
                [cancellation] { return cancellation.isCancellationRequested(); });
            return rows->isValid() ? ScreenshotExportTaskResult{}
                                   : ScreenshotExportTaskResult::failure(
                                         cancellation.isCancellationRequested()
                                             ? ScreenshotExportFailureStage::Cancelled
                                             : ScreenshotExportFailureStage::Source,
                                         QCoreApplication::translate("ScreenshotExportArtifact",
                                                                     "Image source unavailable"));
        },
        [guarded, rows](ScreenshotExportTaskResult result) mutable {
            if (!guarded.isNull()) {
                guarded->completeRowSource(result.succeeded() ? std::move(*rows)
                                                              : ScreenshotImageRowSource{},
                                           std::move(result.error));
            }
        });
    if (!job.isValid()) {
        completeRowSource({}, QCoreApplication::translate("ScreenshotExportArtifact",
                                                          "The screenshot export queue is full"));
        return;
    }
    bool retained = false;
    {
        QMutexLocker lock(&m_impl->mutex);
        if (!m_impl->cancelled && m_impl->rowSourcePhase == RequestPhase::Pending) {
            m_impl->rowSourceJob = job;
            retained = true;
        }
    }
    if (!retained)
        job.cancel();
}

void ScreenshotExportArtifact::startRowSourceFromImage(QImage image) {
    const QPointer<ScreenshotExportArtifact> guarded(this);
    auto rows = std::make_shared<ScreenshotImageRowSource>();
    const ScreenshotExportJobHandle job = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Foreground,
        [image = std::move(image), rows](const ScreenshotExportCancellation& cancellation) {
            if (!cancellation.isCancellationRequested()) {
                *rows =
                    withCancellation(snow_shot::image_codec::srgbRowSource(image), [cancellation] {
                        return cancellation.isCancellationRequested();
                    });
            }
            return rows->isValid() ? ScreenshotExportTaskResult{}
                                   : ScreenshotExportTaskResult::failure(
                                         cancellation.isCancellationRequested()
                                             ? ScreenshotExportFailureStage::Cancelled
                                             : ScreenshotExportFailureStage::Source,
                                         QCoreApplication::translate("ScreenshotExportArtifact",
                                                                     "Image source unavailable"));
        },
        [guarded, rows](ScreenshotExportTaskResult result) mutable {
            if (!guarded.isNull()) {
                guarded->completeRowSource(result.succeeded() ? std::move(*rows)
                                                              : ScreenshotImageRowSource{},
                                           std::move(result.error));
            }
        });
    if (!job.isValid()) {
        completeRowSource({}, QCoreApplication::translate("ScreenshotExportArtifact",
                                                          "The screenshot export queue is full"));
        return;
    }
    bool retained = false;
    {
        QMutexLocker lock(&m_impl->mutex);
        if (!m_impl->cancelled && m_impl->rowSourcePhase == RequestPhase::Pending) {
            m_impl->rowSourceJob = job;
            retained = true;
        }
    }
    if (!retained)
        job.cancel();
}

void ScreenshotExportArtifact::completeRowSource(ScreenshotImageRowSource source, QString error) {
    std::vector<Impl::RowSourceSubscriber> subscribers;
    {
        QMutexLocker lock(&m_impl->mutex);
        if (m_impl->cancelled || m_impl->rowSourcePhase != RequestPhase::Pending)
            return;
        if (source.isValid() && error.isEmpty()) {
            m_impl->rowSourcePhase = RequestPhase::Ready;
            m_impl->rowSource = source;
        } else {
            m_impl->rowSourcePhase = RequestPhase::Failed;
            m_impl->rowSourceError =
                error.isEmpty() ? QStringLiteral("The screenshot row source is unavailable")
                                : std::move(error);
            error = m_impl->rowSourceError;
        }
        subscribers = std::move(m_impl->rowSourceSubscribers);
        m_impl->rowSourceSubscribers.clear();
    }
    for (auto& subscriber : subscribers) {
        if (!subscriber.receiver.isNull()) {
            dispatchRowSource(subscriber.receiver, std::move(subscriber.callback), source, error);
        }
    }
}

void ScreenshotExportArtifact::startImage() {
    const QPointer<ScreenshotExportArtifact> guarded(this);
    if (m_impl->source.m_imageLoader) {
        const bool scheduled = m_impl->source.m_imageLoader(this, [guarded](QImage image) mutable {
            if (!guarded.isNull()) {
                guarded->completeImage({std::move(image), {}});
            }
        });
        if (!scheduled) {
            completeImage(
                {{}, QStringLiteral("The screenshot image request could not be started")});
        }
        return;
    }
    if (!m_impl->source.m_imageProducer) {
        completeImage({{}, QStringLiteral("The screenshot image source is unavailable")});
        return;
    }
    const auto producer = m_impl->source.m_imageProducer;
    const ScreenshotExportJobHandle job = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Foreground,
        [producer](const ScreenshotExportCancellation& cancellation) {
            ScreenshotExportTaskResult result;
            result.image = producer(cancellation);
            if (result.image.isNull()) {
                return ScreenshotExportTaskResult::failure(
                    cancellation.isCancellationRequested() ? ScreenshotExportFailureStage::Cancelled
                                                           : ScreenshotExportFailureStage::Render,
                    cancellation.isCancellationRequested()
                        ? QStringLiteral("The screenshot image request was cancelled")
                        : QStringLiteral("The screenshot image could not be rendered"));
            }
            return result;
        },
        [guarded](ScreenshotExportTaskResult result) mutable {
            if (!guarded.isNull()) {
                guarded->completeImage(
                    {result.succeeded() ? std::move(result.image) : QImage{}, result.error});
            }
        });
    if (!job.isValid()) {
        completeImage({{}, QStringLiteral("The screenshot export queue is full")});
        return;
    }
    bool retained = false;
    {
        QMutexLocker lock(&m_impl->mutex);
        if (!m_impl->cancelled && m_impl->imagePhase == RequestPhase::Pending) {
            m_impl->imageJob = job;
            retained = true;
        }
    }
    if (!retained)
        job.cancel();
}

void ScreenshotExportArtifact::completeImage(ScreenshotExportImageResult result) {
    std::vector<Impl::ImageSubscriber> subscribers;
    {
        QMutexLocker lock(&m_impl->mutex);
        if (m_impl->cancelled || m_impl->imagePhase != RequestPhase::Pending) {
            return;
        }
        if (result.succeeded()) {
            m_impl->imagePhase = RequestPhase::Ready;
            m_impl->image = result.image;
        } else {
            m_impl->imagePhase = RequestPhase::Failed;
            m_impl->imageError = result.error.isEmpty()
                                     ? QStringLiteral("The screenshot image is unavailable")
                                     : result.error;
            result.error = m_impl->imageError;
        }
        subscribers = std::move(m_impl->imageSubscribers);
        m_impl->imageSubscribers.clear();
    }
    snow_shot::diagnostics::DiagnosticsService::instance().record(
        result.succeeded() ? QtInfoMsg : QtWarningMsg, QStringLiteral("snow_shot.export"),
        QStringLiteral("export.image_finished"), result.error,
        {{QStringLiteral("operation"), diagnosticId()},
         {QStringLiteral("outcome"),
          result.succeeded() ? QStringLiteral("succeeded") : QStringLiteral("failed")},
         {QStringLiteral("width"), result.image.width()},
         {QStringLiteral("height"), result.image.height()}});
    for (auto& subscriber : subscribers) {
        if (!subscriber.receiver.isNull()) {
            ScreenshotExportImageResult delivered{result.image, result.error};
            dispatchResult(subscriber.receiver, std::move(subscriber.callback),
                           std::move(delivered));
        }
    }
}

bool ScreenshotExportArtifact::shouldCachePng(QSize pixelSize) const {
    // Use the uncompressed footprint before starting work. The actual PNG size
    // is checked separately when retaining the result (including PNG overhead).
    return pixelSize.width() > 0 && pixelSize.height() > 0 &&
           qint64(pixelSize.width()) * pixelSize.height() <= m_impl->maximumPngBytes / 4;
}

snow_shot::storage::PreparedPngImage
ScreenshotExportArtifact::cachedPng(ScreenshotCompressionLevel compression) {
    QMutexLocker lock(&m_impl->mutex);
    auto& entry = m_impl->pngEncodings[pngCompression(compression)];
    if (m_impl->cancelled || entry.phase != RequestPhase::Ready)
        return {};
    entry.lastUsed = ++m_impl->pngUseSerial;
    return entry.image;
}

bool ScreenshotExportArtifact::requestCanonicalPng(QObject* receiver, EncodingCallback callback) {
    return requestPng(receiver, m_impl->compressionLevel, std::move(callback));
}

bool ScreenshotExportArtifact::requestPng(QObject* receiver, ScreenshotCompressionLevel compression,
                                          EncodingCallback callback) {
    return requestPngCompression(receiver, pngCompression(compression), std::move(callback));
}

bool ScreenshotExportArtifact::requestPngCompression(QObject* receiver, int compressionLevel,
                                                     EncodingCallback callback) {
    if (receiver == nullptr || !callback || m_impl == nullptr) {
        return false;
    }
    ScreenshotExportEncodingResult ready;
    bool dispatchReady = false;
    bool start = false;
    {
        QMutexLocker lock(&m_impl->mutex);
        if (m_impl->cancelled || !m_impl->source.isValid()) {
            return false;
        }
        if (m_impl->pngEncodings[compressionLevel].phase == RequestPhase::Ready) {
            ready.image = m_impl->pngEncodings[compressionLevel].image;
            m_impl->pngEncodings[compressionLevel].lastUsed = ++m_impl->pngUseSerial;
            dispatchReady = true;
        } else {
            m_impl->pngEncodings[compressionLevel].subscribers.push_back(
                {receiver, std::move(callback)});
            if (m_impl->pngEncodings[compressionLevel].phase == RequestPhase::Empty) {
                m_impl->pngEncodings[compressionLevel].phase = RequestPhase::Pending;
                start = true;
            }
        }
    }
    if (dispatchReady) {
        dispatchResult(receiver, std::move(callback), std::move(ready));
    } else if (start) {
        startPng(compressionLevel);
    }
    return true;
}

void ScreenshotExportArtifact::startPng(int compressionLevel) {
    const QPointer<ScreenshotExportArtifact> guarded(this);
    if (!requestRowSource(this, [guarded, compressionLevel](ScreenshotImageRowSource source,
                                                            QString error) mutable {
            if (!guarded.isNull()) {
                if (source.isValid() && error.isEmpty())
                    guarded->startPngFromRows(compressionLevel, std::move(source));
                else
                    guarded->completePng(compressionLevel, {{}, std::move(error)});
            }
        })) {
        completePng(compressionLevel,
                    {{}, QStringLiteral("The screenshot row source request could not be started")});
    }
}

void ScreenshotExportArtifact::startPngFromRows(int compressionLevel,
                                                ScreenshotImageRowSource source) {
    {
        QMutexLocker lock(&m_impl->mutex);
        if (m_impl->cancelled ||
            m_impl->pngEncodings[compressionLevel].phase != RequestPhase::Pending)
            return;
    }
    const QPointer<ScreenshotExportArtifact> guarded(this);
    auto encoded = std::make_shared<ScreenshotExportEncodingResult>();
    const ScreenshotExportJobHandle job = ScreenshotExportCoordinator::shared().submit(
        this,
        compressionLevel == 0 ? ScreenshotExportCoordinator::Priority::Foreground
                              : ScreenshotExportCoordinator::Priority::Background,
        [source = std::move(source), encoded,
         compressionLevel](const ScreenshotExportCancellation& cancellation) {
            if (cancellation.isCancellationRequested()) {
                return ScreenshotExportTaskResult::failure(
                    ScreenshotExportFailureStage::Cancelled,
                    QStringLiteral("The screenshot PNG encoding was cancelled"));
            }
            *encoded = encodePng(
                withCancellation(
                    source, [&cancellation] { return cancellation.isCancellationRequested(); }),
                compressionLevel);
            return encoded->succeeded() ? ScreenshotExportTaskResult{}
                                        : ScreenshotExportTaskResult::failure(
                                              ScreenshotExportFailureStage::Render, encoded->error);
        },
        [guarded, encoded, compressionLevel](ScreenshotExportTaskResult result) mutable {
            if (!guarded.isNull()) {
                guarded->completePng(compressionLevel,
                                     result.succeeded()
                                         ? std::move(*encoded)
                                         : ScreenshotExportEncodingResult{{}, result.error});
            }
        });
    if (!job.isValid()) {
        completePng(compressionLevel, {{}, QStringLiteral("The screenshot export queue is full")});
        return;
    }
    bool retained = false;
    {
        QMutexLocker lock(&m_impl->mutex);
        if (!m_impl->cancelled &&
            m_impl->pngEncodings[compressionLevel].phase == RequestPhase::Pending) {
            m_impl->pngEncodings[compressionLevel].job = job;
            retained = true;
        }
    }
    if (!retained)
        job.cancel();
}

void ScreenshotExportArtifact::completePng(int compressionLevel,
                                           ScreenshotExportEncodingResult result) {
    std::vector<Impl::EncodingSubscriber> subscribers;
    {
        QMutexLocker lock(&m_impl->mutex);
        if (m_impl->cancelled ||
            m_impl->pngEncodings[compressionLevel].phase != RequestPhase::Pending) {
            return;
        }
        if (result.succeeded()) {
            m_impl->retainPng(compressionLevel, result.image);
        } else {
            // Only successful encodings are cached. A new user request may retry a
            // transient queue/read failure; current subscribers all receive this failure.
            m_impl->pngEncodings[compressionLevel].phase = RequestPhase::Empty;
            if (result.error.isEmpty())
                result.error = QStringLiteral("The screenshot PNG is unavailable");
        }
        subscribers = std::move(m_impl->pngEncodings[compressionLevel].subscribers);
        m_impl->pngEncodings[compressionLevel].subscribers.clear();
    }
    for (auto& subscriber : subscribers) {
        if (!subscriber.receiver.isNull()) {
            ScreenshotExportEncodingResult delivered{result.image, result.error};
            dispatchResult(subscriber.receiver, std::move(subscriber.callback),
                           std::move(delivered));
        }
    }
}

bool ScreenshotExportArtifact::requestClipboard(QObject* receiver, ClipboardCallback callback) {
    if (receiver == nullptr || !callback || isCancelled())
        return false;
    QByteArray readyPng;
    {
        QMutexLocker lock(&m_impl->mutex);
        for (auto& encoding : m_impl->pngEncodings) {
            if (encoding.phase == RequestPhase::Ready) {
                readyPng = encoding.image.bytes();
                encoding.lastUsed = ++m_impl->pngUseSerial;
                break;
            }
        }
    }
    // Existing bytes are cheapest regardless of compression. Otherwise share level 0;
    // never subscribe the clipboard to a pending higher-compression encoding.
    if (!readyPng.isEmpty())
        return prepareClipboard(receiver, std::move(readyPng), std::move(callback));
    const QPointer<ScreenshotExportArtifact> guarded(this);
    const QPointer<QObject> target(receiver);
    return requestPngCompression(
        this, 0,
        [guarded, target,
         callback = std::move(callback)](ScreenshotExportEncodingResult result) mutable {
            if (guarded.isNull() || guarded->isCancelled() || target.isNull())
                return;
            if (!result.succeeded()) {
                dispatchResult(target, std::move(callback),
                               ScreenshotExportClipboardResult{{}, result.error});
                return;
            }
            auto completion = std::make_shared<ClipboardCallback>(std::move(callback));
            const bool scheduled = guarded->prepareClipboard(
                target, result.image.bytes(),
                [completion](ScreenshotExportClipboardResult prepared) mutable {
                    if (*completion) {
                        auto deliver = std::move(*completion);
                        deliver(std::move(prepared));
                    }
                });
            if (!scheduled && *completion) {
                dispatchResult(target, std::move(*completion),
                               ScreenshotExportClipboardResult{
                                   {}, QStringLiteral("The screenshot export queue is full")});
            }
        });
}

bool ScreenshotExportArtifact::prepareClipboard(QObject* receiver, QByteArray canonicalPng,
                                                ClipboardCallback callback) {
    if (receiver == nullptr || !callback || m_impl == nullptr || isCancelled()) {
        return false;
    }
    const QPointer<ScreenshotExportArtifact> guardedArtifact(this);
    const QPointer<QObject> guardedReceiver(receiver);
    return requestRowSource(
        this,
        [guardedArtifact, guardedReceiver, receiver, canonicalPng = std::move(canonicalPng),
         callback = std::move(callback)](ScreenshotImageRowSource source, QString error) mutable {
            if (guardedArtifact.isNull() || guardedArtifact->isCancelled() ||
                guardedReceiver.isNull()) {
                return;
            }
            if (!source.isValid() || !error.isEmpty()) {
                dispatchResult(guardedReceiver, std::move(callback),
                               ScreenshotExportClipboardResult{{}, std::move(error)});
                return;
            }
            auto payload = std::make_shared<ScreenshotClipboardPayload>();
            auto completion = std::make_shared<ClipboardCallback>(std::move(callback));
            ScreenshotExportJobHandle job = ScreenshotExportCoordinator::shared().submit(
                receiver, ScreenshotExportCoordinator::Priority::Foreground,
                [source = std::move(source), canonicalPng,
                 payload](const ScreenshotExportCancellation& cancellation) mutable {
                    ScreenshotImageRowSource rows = withCancellation(
                        source, [&cancellation] { return cancellation.isCancellationRequested(); });
                    *payload = ScreenshotClipboardService::prepare(rows, canonicalPng);
                    return payload->isValid()
                               ? ScreenshotExportTaskResult{}
                               : ScreenshotExportTaskResult::failure(
                                     cancellation.isCancellationRequested()
                                         ? ScreenshotExportFailureStage::Cancelled
                                         : ScreenshotExportFailureStage::Clipboard,
                                     QStringLiteral("The screenshot clipboard payload is invalid"));
                },
                [guardedArtifact, guardedReceiver, payload,
                 completion](ScreenshotExportTaskResult result) mutable {
                    if (!guardedArtifact.isNull() && !guardedArtifact->isCancelled() &&
                        !guardedReceiver.isNull() && *completion) {
                        dispatchResult(guardedReceiver, std::move(*completion),
                                       ScreenshotExportClipboardResult{
                                           result.succeeded() ? std::move(*payload)
                                                              : ScreenshotClipboardPayload{},
                                           result.error});
                    }
                });
            if (!job.isValid()) {
                dispatchResult(guardedReceiver, std::move(*completion),
                               ScreenshotExportClipboardResult{
                                   {}, QStringLiteral("The screenshot export queue is full")});
                return;
            }
            bool retained = false;
            {
                QMutexLocker lock(&guardedArtifact->m_impl->mutex);
                if (!guardedArtifact->m_impl->cancelled) {
                    guardedArtifact->m_impl->outputJobs.push_back(job);
                    retained = true;
                }
            }
            if (!retained)
                job.cancel();
        });
}

bool ScreenshotExportArtifact::requestQuickSave(QObject* receiver,
                                                ScreenshotExportCoordinator::Completion callback) {
    if (receiver == nullptr || !callback || isCancelled())
        return false;
    const snow_shot::storage::ScreenshotSettings settings;
    const QString directory = settings.imageSaveDirectory().trimmed();
    if (directory.isEmpty()) {
        const QPointer<ScreenshotExportArtifact> guarded(this);
        return QMetaObject::invokeMethod(
            receiver,
            [guarded, callback = std::move(callback)]() mutable {
                if (guarded && !guarded->isCancelled()) {
                    callback(ScreenshotExportTaskResult::failure(
                        ScreenshotExportFailureStage::File,
                        QCoreApplication::translate("ScreenshotExportArtifact",
                                                    "The image save directory is not configured")));
                }
            },
            Qt::QueuedConnection);
    }
    return requestAutomaticSave(
        receiver, {directory}, ScreenshotImageFileService::formatForKey(settings.imageFormat()),
        settings.autoSaveFilenameFormat(), std::move(callback),
        ScreenshotPdfOptions{screenshot_pdf::pageSizeForKey(settings.pdfPageSize())});
}

bool ScreenshotExportArtifact::requestSaveToPath(QObject* receiver, QString path,
                                                 ScreenshotImageFileFormat format,
                                                 ScreenshotImageEncodingOptions encoding,
                                                 ScreenshotExportCoordinator::Completion callback,
                                                 ScreenshotPdfOptions pdf) {
    if (receiver == nullptr || !callback || isCancelled())
        return false;
    const QPointer<ScreenshotExportArtifact> guarded(this);
    const QPointer<QObject> target(receiver);
    auto schedule = [guarded, target, path = std::move(path), format, encoding, pdf,
                     callback = std::move(callback)](snow_shot::storage::PreparedPngImage png,
                                                     ScreenshotImageRowSource rows,
                                                     QString error) mutable {
        if (guarded.isNull() || guarded->isCancelled() || target.isNull())
            return;
        if (!error.isEmpty()) {
            dispatchResult(target, std::move(callback),
                           ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::File,
                                                               std::move(error)));
            return;
        }
        auto completion =
            std::make_shared<ScreenshotExportCoordinator::Completion>(std::move(callback));
        const ScreenshotExportJobHandle job = ScreenshotExportCoordinator::shared().submit(
            target, ScreenshotExportCoordinator::Priority::Foreground,
            [path, format, encoding, pdf, png = std::move(png),
             rows = std::move(rows)](const ScreenshotExportCancellation& cancellation) mutable {
                if (cancellation.isCancellationRequested()) {
                    return ScreenshotExportTaskResult::failure(
                        ScreenshotExportFailureStage::Cancelled,
                        QStringLiteral("The screenshot save was cancelled"));
                }
                ScreenshotImageFileSaveResult saved;
                if (png.isValid()) {
                    saved = ScreenshotImageFileService::write(png, path, [&cancellation] {
                        return cancellation.isCancellationRequested();
                    });
                } else {
                    rows = withCancellation(
                        rows, [&cancellation] { return cancellation.isCancellationRequested(); });
                    saved = ScreenshotImageFileService::write(rows, path, format, pdf, encoding);
                }
                if (!saved.succeeded()) {
                    return ScreenshotExportTaskResult::failure(
                        cancellation.isCancellationRequested()
                            ? ScreenshotExportFailureStage::Cancelled
                            : ScreenshotExportFailureStage::File,
                        saved.error);
                }
                ScreenshotExportTaskResult result;
                result.savedPath = saved.path;
                return result;
            },
            [guarded, target, completion](ScreenshotExportTaskResult result) mutable {
                if (!guarded.isNull() && !guarded->isCancelled() && !target.isNull()) {
                    dispatchResult(target, std::move(*completion), std::move(result));
                }
            });
        if (!job.isValid()) {
            dispatchResult(target, std::move(*completion),
                           ScreenshotExportTaskResult::failure(
                               ScreenshotExportFailureStage::Queue,
                               QStringLiteral("The screenshot export queue is full")));
            return;
        }
        bool retained = false;
        {
            QMutexLocker lock(&guarded->m_impl->mutex);
            if (!guarded->m_impl->cancelled) {
                guarded->m_impl->outputJobs.push_back(job);
                retained = true;
            }
        }
        if (!retained)
            job.cancel();
    };
    return requestFileSource(format, encoding.compressionLevel, std::move(schedule));
}

bool ScreenshotExportArtifact::requestFileSource(ScreenshotImageFileFormat format,
                                                 ScreenshotCompressionLevel compression,
                                                 FileSourceCallback callback) {
    const QPointer<ScreenshotExportArtifact> guarded(this);
    return requestRowSource(this, [guarded, format, compression, callback = std::move(callback)](
                                      ScreenshotImageRowSource rows, QString error) mutable {
        if (!guarded || guarded->isCancelled())
            return;
        if (!error.isEmpty() || format != ScreenshotImageFileFormat::Png) {
            callback({}, std::move(rows), std::move(error));
            return;
        }
        auto cached = guarded->cachedPng(compression);
        if (cached.isValid()) {
            callback(std::move(cached), {}, {});
            return;
        }
        if (!guarded->shouldCachePng(rows.size)) {
            callback({}, std::move(rows), {});
            return;
        }
        auto completion = std::make_shared<FileSourceCallback>(std::move(callback));
        if (!guarded->requestPng(
                guarded, compression, [completion](ScreenshotExportEncodingResult result) {
                    (*completion)(std::move(result.image), {}, std::move(result.error));
                })) {
            (*completion)({}, {}, QStringLiteral("The screenshot export queue is full"));
        }
    });
}

bool ScreenshotExportArtifact::requestAutomaticSave(
    QObject* receiver, QStringList directories, ScreenshotImageFileFormat format,
    QString filenameFormat, ScreenshotExportCoordinator::Completion callback,
    ScreenshotPdfOptions pdf, QDateTime requestedAt) {
    if (receiver == nullptr || !callback || isCancelled())
        return false;
    const QPointer<ScreenshotExportArtifact> guarded(this);
    const QPointer<QObject> target(receiver);
    const ScreenshotImageEncodingOptions encoding{100, m_impl->compressionLevel};
    if (!requestedAt.isValid())
        requestedAt = QDateTime::currentDateTime();
    auto schedule = [guarded, target, directories = std::move(directories), format, pdf, encoding,
                     requestedAt, filenameFormat = std::move(filenameFormat),
                     callback = std::move(callback)](snow_shot::storage::PreparedPngImage png,
                                                     ScreenshotImageRowSource rows,
                                                     QString error) mutable {
        if (guarded.isNull() || guarded->isCancelled() || target.isNull())
            return;
        if (!error.isEmpty()) {
            dispatchResult(target, std::move(callback),
                           ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::File,
                                                               std::move(error)));
            return;
        }
        auto completion =
            std::make_shared<ScreenshotExportCoordinator::Completion>(std::move(callback));
        auto job = ScreenshotExportCoordinator::shared().submit(
            target, ScreenshotExportCoordinator::Priority::Background,
            [directories, format, pdf, encoding, requestedAt, filenameFormat, png = std::move(png),
             rows = std::move(rows)](const ScreenshotExportCancellation& cancellation) mutable {
                if (cancellation.isCancellationRequested()) {
                    return ScreenshotExportTaskResult::failure(
                        ScreenshotExportFailureStage::Cancelled,
                        QStringLiteral("The screenshot save was cancelled"));
                }
                ScreenshotImageFileSaveResult saved;
                if (png.isValid()) {
                    saved = ScreenshotImageFileService::saveAutomatically(
                        png, directories, filenameFormat, requestedAt);
                } else {
                    rows = withCancellation(
                        rows, [&cancellation] { return cancellation.isCancellationRequested(); });
                    saved = ScreenshotImageFileService::saveAutomatically(
                        rows, directories, format, filenameFormat, requestedAt, pdf, encoding);
                }
                if (!saved.succeeded()) {
                    return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::File,
                                                               saved.error);
                }
                ScreenshotExportTaskResult result;
                result.savedPath = saved.path;
                return result;
            },
            [guarded, target, completion](ScreenshotExportTaskResult result) mutable {
                if (!guarded.isNull() && !guarded->isCancelled() && !target.isNull()) {
                    dispatchResult(target, std::move(*completion), std::move(result));
                }
            });
        if (!job.isValid()) {
            dispatchResult(target, std::move(*completion),
                           ScreenshotExportTaskResult::failure(
                               ScreenshotExportFailureStage::Queue,
                               QStringLiteral("The screenshot export queue is full")));
            return;
        }
        bool retained = false;
        {
            QMutexLocker lock(&guarded->m_impl->mutex);
            if (!guarded->m_impl->cancelled) {
                guarded->m_impl->outputJobs.push_back(job);
                retained = true;
            }
        }
        if (!retained)
            job.cancel();
    };
    return requestFileSource(format, encoding.compressionLevel, std::move(schedule));
}

void ScreenshotExportArtifact::cancel() {
    if (m_impl == nullptr) {
        return;
    }
    ScreenshotExportJobHandle imageJob;
    ScreenshotExportJobHandle rowSourceJob;
    std::vector<ScreenshotExportJobHandle> outputJobs;
    bool pending = false;
    {
        QMutexLocker lock(&m_impl->mutex);
        if (m_impl->cancelled) {
            return;
        }
        m_impl->cancelled = true;
        pending = m_impl->imagePhase == RequestPhase::Pending ||
                  m_impl->rowSourcePhase == RequestPhase::Pending;
        imageJob = m_impl->imageJob;
        rowSourceJob = m_impl->rowSourceJob;
        outputJobs = m_impl->outputJobs;
        m_impl->imageSubscribers.clear();
        m_impl->rowSourceSubscribers.clear();
        for (auto& encoding : m_impl->pngEncodings) {
            pending = pending || encoding.phase == RequestPhase::Pending;
            outputJobs.push_back(encoding.job);
            encoding.subscribers.clear();
        }
    }
    if (pending) {
        snow_shot::diagnostics::logEvent(
            QStringLiteral("snow_shot.export"), QStringLiteral("export.cancelled"),
            {{QStringLiteral("operation"), diagnosticId()},
             {QStringLiteral("outcome"), QStringLiteral("cancelled")}});
    }
    imageJob.cancel();
    rowSourceJob.cancel();
    for (const auto& job : outputJobs) {
        job.cancel();
    }
}

bool ScreenshotExportArtifact::isValid() const {
    return m_impl != nullptr && m_impl->source.isValid();
}

bool ScreenshotExportArtifact::isCancelled() const {
    if (m_impl == nullptr) {
        return true;
    }
    QMutexLocker lock(&m_impl->mutex);
    return m_impl->cancelled;
}
