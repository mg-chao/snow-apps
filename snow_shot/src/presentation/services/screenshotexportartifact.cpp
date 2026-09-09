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

ScreenshotExportEncodingResult encodeCanonicalPng(const ScreenshotImageRowSource& source) {
    if (!source.isValid()) {
        return {{}, QStringLiteral("The screenshot row source is unavailable")};
    }
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly)) {
        return {{}, QStringLiteral("The screenshot PNG buffer could not be opened")};
    }
    snow::image::EncodeOptions options;
    options.compression_level = 1;
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

    explicit Impl(ScreenshotExportSource value) : source(std::move(value)) {}

    ScreenshotExportSource source;
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
    RequestPhase encodingPhase = RequestPhase::Empty;
    snow_shot::storage::PreparedPngImage encoding;
    QString encodingError;
    std::vector<EncodingSubscriber> encodingSubscribers;
    ScreenshotExportJobHandle imageJob;
    ScreenshotExportJobHandle rowSourceJob;
    ScreenshotExportJobHandle encodingJob;
    std::vector<ScreenshotExportJobHandle> outputJobs;
};

ScreenshotExportArtifact::ScreenshotExportArtifact(ScreenshotExportSource source, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(std::move(source))) {}

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

bool ScreenshotExportArtifact::requestCanonicalPng(QObject* receiver, EncodingCallback callback) {
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
        if (m_impl->encodingPhase == RequestPhase::Ready) {
            ready.image = m_impl->encoding;
            dispatchReady = true;
        } else if (m_impl->encodingPhase == RequestPhase::Failed) {
            ready.error = m_impl->encodingError;
            dispatchReady = true;
        } else {
            m_impl->encodingSubscribers.push_back({receiver, std::move(callback)});
            if (m_impl->encodingPhase == RequestPhase::Empty) {
                m_impl->encodingPhase = RequestPhase::Pending;
                start = true;
            }
        }
    }
    if (dispatchReady) {
        dispatchResult(receiver, std::move(callback), std::move(ready));
    } else if (start) {
        startCanonicalPng();
    }
    return true;
}

void ScreenshotExportArtifact::startCanonicalPng() {
    const QPointer<ScreenshotExportArtifact> guarded(this);
    if (!requestRowSource(this, [guarded](ScreenshotImageRowSource source, QString error) mutable {
            if (!guarded.isNull()) {
                if (source.isValid() && error.isEmpty())
                    guarded->startCanonicalPngFromRows(std::move(source));
                else
                    guarded->completeCanonicalPng({{}, std::move(error)});
            }
        })) {
        completeCanonicalPng(
            {{}, QStringLiteral("The screenshot row source request could not be started")});
    }
}

void ScreenshotExportArtifact::startCanonicalPngFromRows(ScreenshotImageRowSource source) {
    {
        QMutexLocker lock(&m_impl->mutex);
        if (m_impl->cancelled || m_impl->encodingPhase != RequestPhase::Pending)
            return;
    }
    const QPointer<ScreenshotExportArtifact> guarded(this);
    auto encoded = std::make_shared<ScreenshotExportEncodingResult>();
    const ScreenshotExportJobHandle job = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Background,
        [source = std::move(source), encoded](const ScreenshotExportCancellation& cancellation) {
            if (cancellation.isCancellationRequested()) {
                return ScreenshotExportTaskResult::failure(
                    ScreenshotExportFailureStage::Cancelled,
                    QStringLiteral("The screenshot PNG encoding was cancelled"));
            }
            *encoded = encodeCanonicalPng(withCancellation(
                source, [&cancellation] { return cancellation.isCancellationRequested(); }));
            return encoded->succeeded() ? ScreenshotExportTaskResult{}
                                        : ScreenshotExportTaskResult::failure(
                                              ScreenshotExportFailureStage::Render, encoded->error);
        },
        [guarded, encoded](ScreenshotExportTaskResult result) mutable {
            if (!guarded.isNull()) {
                guarded->completeCanonicalPng(
                    result.succeeded() ? std::move(*encoded)
                                       : ScreenshotExportEncodingResult{{}, result.error});
            }
        });
    if (!job.isValid()) {
        completeCanonicalPng({{}, QStringLiteral("The screenshot export queue is full")});
        return;
    }
    bool retained = false;
    {
        QMutexLocker lock(&m_impl->mutex);
        if (!m_impl->cancelled && m_impl->encodingPhase == RequestPhase::Pending) {
            m_impl->encodingJob = job;
            retained = true;
        }
    }
    if (!retained)
        job.cancel();
}

bool ScreenshotExportArtifact::adoptCanonicalPng(snow_shot::storage::PreparedPngImage image) {
    if (!image.isValid() || m_impl == nullptr)
        return false;

    ScreenshotExportJobHandle redundantJob;
    std::vector<Impl::EncodingSubscriber> subscribers;
    {
        QMutexLocker lock(&m_impl->mutex);
        if (m_impl->cancelled || m_impl->rowSourcePhase != RequestPhase::Ready ||
            !m_impl->rowSource.isValid() || image.pixelSize() != m_impl->rowSource.size) {
            return false;
        }
        if (m_impl->encodingPhase == RequestPhase::Ready)
            return true;
        if (m_impl->encodingPhase == RequestPhase::Pending) {
            redundantJob = m_impl->encodingJob;
            subscribers = std::move(m_impl->encodingSubscribers);
            m_impl->encodingSubscribers.clear();
        }
        m_impl->encodingPhase = RequestPhase::Ready;
        m_impl->encoding = image;
        m_impl->encodingError.clear();
    }
    redundantJob.cancel();
    for (auto& subscriber : subscribers) {
        if (!subscriber.receiver.isNull()) {
            dispatchResult(subscriber.receiver, std::move(subscriber.callback),
                           ScreenshotExportEncodingResult{image, {}});
        }
    }
    return true;
}

void ScreenshotExportArtifact::completeCanonicalPng(ScreenshotExportEncodingResult result) {
    std::vector<Impl::EncodingSubscriber> subscribers;
    {
        QMutexLocker lock(&m_impl->mutex);
        if (m_impl->cancelled || m_impl->encodingPhase != RequestPhase::Pending) {
            return;
        }
        if (result.succeeded()) {
            m_impl->encodingPhase = RequestPhase::Ready;
            m_impl->encoding = result.image;
        } else {
            m_impl->encodingPhase = RequestPhase::Failed;
            m_impl->encodingError = result.error.isEmpty()
                                        ? QStringLiteral("The screenshot PNG is unavailable")
                                        : result.error;
            result.error = m_impl->encodingError;
        }
        subscribers = std::move(m_impl->encodingSubscribers);
        m_impl->encodingSubscribers.clear();
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
    const QPointer<ScreenshotExportArtifact> guarded(this);
    const QPointer<QObject> target(receiver);
    return requestCanonicalPng(this, [guarded, target, callback = std::move(callback)](
                                         ScreenshotExportEncodingResult result) mutable {
        if (guarded.isNull() || guarded->isCancelled() || target.isNull())
            return;
        if (!result.succeeded()) {
            dispatchResult(target, std::move(callback),
                           ScreenshotExportClipboardResult{{}, result.error});
            return;
        }
        // Clipboard, PNG saving and history subscribe to the same immutable encoding.
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
    return requestAutomaticSave(receiver, {directory},
                                ScreenshotImageFileService::formatForKey(settings.imageFormat()),
                                settings.autoSaveFilenameFormat(), std::move(callback));
}

bool ScreenshotExportArtifact::requestAutomaticSave(
    QObject* receiver, QStringList directories, ScreenshotImageFileFormat format,
    QString filenameFormat, ScreenshotExportCoordinator::Completion callback) {
    if (receiver == nullptr || !callback || isCancelled())
        return false;
    const QPointer<ScreenshotExportArtifact> guarded(this);
    const QPointer<QObject> target(receiver);
    auto schedule = [guarded, target, directories = std::move(directories), format,
                     filenameFormat = std::move(filenameFormat), callback = std::move(callback)](
                        snow_shot::storage::PreparedPngImage png, ScreenshotImageRowSource rows,
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
            [directories, format, filenameFormat, png = std::move(png),
             rows = std::move(rows)](const ScreenshotExportCancellation& cancellation) mutable {
                if (cancellation.isCancellationRequested()) {
                    return ScreenshotExportTaskResult::failure(
                        ScreenshotExportFailureStage::Cancelled,
                        QStringLiteral("The screenshot save was cancelled"));
                }
                ScreenshotImageFileSaveResult saved;
                if (png.isValid()) {
                    saved = ScreenshotImageFileService::saveAutomatically(png, directories,
                                                                          filenameFormat);
                } else {
                    rows = withCancellation(
                        rows, [&cancellation] { return cancellation.isCancellationRequested(); });
                    saved = ScreenshotImageFileService::saveAutomatically(rows, directories, format,
                                                                          filenameFormat);
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
    if (format == ScreenshotImageFileFormat::Png) {
        return requestCanonicalPng(
            this, [schedule = std::move(schedule)](ScreenshotExportEncodingResult result) mutable {
                schedule(std::move(result.image), {}, std::move(result.error));
            });
    }
    return requestRowSource(this, [schedule = std::move(schedule)](ScreenshotImageRowSource source,
                                                                   QString error) mutable {
        schedule({}, std::move(source), std::move(error));
    });
}

void ScreenshotExportArtifact::cancel() {
    if (m_impl == nullptr) {
        return;
    }
    ScreenshotExportJobHandle imageJob;
    ScreenshotExportJobHandle rowSourceJob;
    ScreenshotExportJobHandle encodingJob;
    std::vector<ScreenshotExportJobHandle> outputJobs;
    bool pending = false;
    {
        QMutexLocker lock(&m_impl->mutex);
        if (m_impl->cancelled) {
            return;
        }
        m_impl->cancelled = true;
        pending = m_impl->imagePhase == RequestPhase::Pending ||
                  m_impl->rowSourcePhase == RequestPhase::Pending ||
                  m_impl->encodingPhase == RequestPhase::Pending;
        imageJob = m_impl->imageJob;
        rowSourceJob = m_impl->rowSourceJob;
        encodingJob = m_impl->encodingJob;
        outputJobs = m_impl->outputJobs;
        m_impl->imageSubscribers.clear();
        m_impl->rowSourceSubscribers.clear();
        m_impl->encodingSubscribers.clear();
    }
    if (pending) {
        snow_shot::diagnostics::logEvent(
            QStringLiteral("snow_shot.export"), QStringLiteral("export.cancelled"),
            {{QStringLiteral("operation"), diagnosticId()},
             {QStringLiteral("outcome"), QStringLiteral("cancelled")}});
    }
    imageJob.cancel();
    rowSourceJob.cancel();
    encodingJob.cancel();
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
