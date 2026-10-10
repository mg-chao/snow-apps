#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/screenshotencodingsettings.h"
#include "snow_shot/runtime/runtimeactivitytracker.h"

#include "snowimageqtcodec.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/diagnostics/diagnostics.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QFileInfo>
#include <QFile>
#include <QMimeData>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

#include <snow/image/codec.h>
#include <snow/image/format.h>

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>
#include <map>
#include <tuple>
#include <limits>

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
        snow_shot::runtime::trackRuntimeWork([guardedReceiver, callback = std::move(callback),
                                              result = std::move(result)]() mutable {
            if (!guardedReceiver.isNull()) {
                callback(std::move(result));
            }
        }),
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
        snow_shot::runtime::trackRuntimeWork([guardedReceiver, callback = std::move(callback),
                                              source = std::move(source),
                                              error = std::move(error)]() mutable {
            if (!guardedReceiver.isNull())
                callback(std::move(source), std::move(error));
        }),
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

ScreenshotExportSource
ScreenshotExportSource::fromImage(QImage image,
                                  std::optional<ScreenshotClipboardPlacement> placement,
                                  std::optional<ScreenshotClipboardAppearance> appearance) {
    if (placement)
        placement->rasterSize = image.size();
    auto source =
        fromProducer([image = std::move(image)](const ScreenshotExportCancellation& cancellation) {
            return cancellation.isCancellationRequested() ? QImage{} : image;
        });
    source.m_clipboardPlacement = std::move(placement);
    source.m_clipboardAppearance = std::move(appearance);
    return source;
}

ScreenshotExportSource
ScreenshotExportSource::fromImageLoader(ImageLoader loader,
                                        std::optional<ScreenshotClipboardPlacement> placement,
                                        std::optional<ScreenshotClipboardAppearance> appearance) {
    ScreenshotExportSource source;
    source.m_imageLoader = std::move(loader);
    source.m_clipboardPlacement = std::move(placement);
    source.m_clipboardAppearance = std::move(appearance);
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

struct ScreenshotExportArtifact::FileSource final {
    snow_shot::storage::PreparedPngImage png;
    std::shared_ptr<QTemporaryDir> directory;
    QString path;
    QByteArray bytes;
    ScreenshotImageRowSource rows;
    std::shared_ptr<screenshot_pdf::Payload> pdf;
    qint64 byteCount = 0;

    // Start with bounded encoded bytes; spill only large outputs to disk. Small
    // cold exports then require only the final file write, like the direct path.
    class Output final : public QIODevice {
      public:
        Output(FileSource& source, qsizetype limit) : m_source(source), m_limit(limit) {
            open(QIODevice::WriteOnly);
        }
        bool isSequential() const override {
            return true;
        }
        qint64 pos() const override {
            return m_position;
        }
        bool finish() {
            if (m_file.isOpen()) {
                if (!m_file.flush()) {
                    setErrorString(m_file.errorString());
                    return false;
                }
                m_source.byteCount = m_file.size();
                m_file.close();
            } else {
                m_source.bytes.squeeze();
                m_source.byteCount = m_source.bytes.size();
            }
            return true;
        }

      protected:
        qint64 readData(char*, qint64) override {
            return -1;
        }
        qint64 writeData(const char* data, qint64 size) override {
            if (!m_file.isOpen() && size <= m_limit - m_source.bytes.size()) {
                m_source.bytes.append(data, size);
                m_position += size;
                return size;
            }
            if (!m_file.isOpen()) {
                m_source.directory = std::make_shared<QTemporaryDir>(
                    QDir::temp().filePath(QStringLiteral("snow-shot-export-XXXXXX")));
                if (!m_source.directory->isValid()) {
                    setErrorString(m_source.directory->errorString());
                    return -1;
                }
                m_source.path = m_source.directory->filePath(QStringLiteral("encoded"));
                m_file.setFileName(m_source.path);
                if (!m_file.open(QIODevice::WriteOnly) ||
                    m_file.write(m_source.bytes) != m_source.bytes.size()) {
                    setErrorString(m_file.errorString());
                    return -1;
                }
                m_source.bytes = {};
            }
            const qint64 written = m_file.write(data, size);
            if (written > 0)
                m_position += written;
            if (written != size)
                setErrorString(m_file.errorString());
            return written;
        }

      private:
        FileSource& m_source;
        const qsizetype m_limit;
        qint64 m_position = 0;
        QFile m_file;
    };
};

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
          maximumPngBytes(std::max(qsizetype{0}, policy.maximumBytes)),
          encodingStarted(std::move(policy.encodingStarted)),
          maximumFileBytes(std::max(qint64{0}, policy.maximumFileBytes)),
          fileEncodingStarted(std::move(policy.fileEncodingStarted)) {}

    ScreenshotExportSource source;
    const ScreenshotCompressionLevel compressionLevel;
    const qsizetype maximumPngBytes;
    const std::function<void()> encodingStarted;
    const qint64 maximumFileBytes;
    const std::function<void(ScreenshotImageFileFormat)> fileEncodingStarted;
    using FileKey = std::tuple<ScreenshotImageFileFormat, int, int>;
    struct FileEncoding final {
        RequestPhase phase = RequestPhase::Empty;
        std::shared_ptr<FileSource> source;
        std::shared_ptr<std::vector<FileSourceCallback>> subscribers;
        ScreenshotExportJobHandle job;
        quint64 lastUsed = 0;
    };
    std::map<FileKey, FileEncoding> fileEncodings;
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
        std::shared_ptr<std::vector<EncodingSubscriber>> subscribers;
        ScreenshotExportJobHandle job;
        quint64 lastUsed = 0;
    };
    // The source pixels and PNG options other than compression are immutable.
    std::array<PngEncoding, 10> pngEncodings;

    // PNG compression levels are 0..9. std::array indexes with size_t.
    [[nodiscard]] PngEncoding& encodingAt(int level) {
        return pngEncodings[static_cast<std::size_t>(level)];
    }

    // Called under mutex. Pending encodings are never evicted; their subscribers
    // receive the result even when it is too large to retain in this cache.
    void retainPng(int level, const snow_shot::storage::PreparedPngImage& encoded) {
        auto& target = encodingAt(level);
        target.phase = RequestPhase::Empty;
        target.image = {};
        const qsizetype bytes = encoded.bytes().size();
        if (bytes > maximumPngBytes)
            return;
        makeMemoryRoom(bytes);
        target.phase = RequestPhase::Ready;
        target.image = encoded;
        target.lastUsed = ++pngUseSerial;
    }
    void makeMemoryRoom(qsizetype bytes) {
        qsizetype retained = 0;
        for (const auto& entry : pngEncodings)
            retained += entry.image.bytes().size();
        for (const auto& [key, entry] : fileEncodings) {
            Q_UNUSED(key);
            if (entry.source)
                retained += entry.source->bytes.size();
        }
        while (retained > maximumPngBytes - bytes) {
            PngEncoding* png = nullptr;
            FileEncoding* file = nullptr;
            quint64 oldest = std::numeric_limits<quint64>::max();
            for (auto& entry : pngEncodings) {
                if (entry.phase == RequestPhase::Ready && entry.lastUsed < oldest) {
                    png = &entry;
                    oldest = entry.lastUsed;
                }
            }
            for (auto& [key, entry] : fileEncodings) {
                Q_UNUSED(key);
                if (entry.phase == RequestPhase::Ready && !entry.source->bytes.isEmpty() &&
                    entry.lastUsed < oldest) {
                    file = &entry;
                    oldest = entry.lastUsed;
                }
            }
            if (file) {
                retained -= file->source->bytes.size();
                file->source.reset();
                file->phase = RequestPhase::Empty;
            } else if (png) {
                retained -= png->image.bytes().size();
                png->image = {};
                png->phase = RequestPhase::Empty;
            } else {
                break;
            }
        }
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
    auto& entry = m_impl->encodingAt(pngCompression(compression));
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
    if (receiver == nullptr || !callback || m_impl == nullptr) {
        return false;
    }
    const int compressionLevel = pngCompression(compression);
    ScreenshotExportEncodingResult ready;
    bool dispatchReady = false;
    bool start = false;
    {
        QMutexLocker lock(&m_impl->mutex);
        if (m_impl->cancelled || !m_impl->source.isValid()) {
            return false;
        }
        if (m_impl->encodingAt(compressionLevel).phase == RequestPhase::Ready) {
            ready.image = m_impl->encodingAt(compressionLevel).image;
            m_impl->encodingAt(compressionLevel).lastUsed = ++m_impl->pngUseSerial;
            dispatchReady = true;
        } else {
            auto& pending = m_impl->encodingAt(compressionLevel);
            if (!pending.subscribers)
                pending.subscribers = std::make_shared<std::vector<Impl::EncodingSubscriber>>();
            pending.subscribers->push_back({receiver, std::move(callback)});
            if (m_impl->encodingAt(compressionLevel).phase == RequestPhase::Empty) {
                m_impl->encodingAt(compressionLevel).phase = RequestPhase::Pending;
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
    std::shared_ptr<std::vector<Impl::EncodingSubscriber>> subscribers;
    {
        QMutexLocker lock(&m_impl->mutex);
        if (m_impl->cancelled ||
            m_impl->encodingAt(compressionLevel).phase != RequestPhase::Pending)
            return;
        subscribers = m_impl->encodingAt(compressionLevel).subscribers;
    }
    const QPointer<ScreenshotExportArtifact> guarded(this);
    auto encoded = std::make_shared<ScreenshotExportEncodingResult>();
    const ScreenshotExportJobHandle job = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Background,
        [source = std::move(source), encoded, subscribers, observed = m_impl->encodingStarted,
         compressionLevel](const ScreenshotExportCancellation& cancellation) {
            // Subscriber callbacks own any consumer output leases. Keep them
            // alive until the encoder unwinds, even after artifact cancellation.
            Q_UNUSED(subscribers);
            if (cancellation.isCancellationRequested()) {
                return ScreenshotExportTaskResult::failure(
                    ScreenshotExportFailureStage::Cancelled,
                    QStringLiteral("The screenshot PNG encoding was cancelled"));
            }
            if (observed)
                observed();
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
            m_impl->encodingAt(compressionLevel).phase == RequestPhase::Pending) {
            m_impl->encodingAt(compressionLevel).job = job;
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
            m_impl->encodingAt(compressionLevel).phase != RequestPhase::Pending) {
            return;
        }
        if (result.succeeded()) {
            m_impl->retainPng(compressionLevel, result.image);
        } else {
            // Only successful encodings are cached. A new user request may retry a
            // transient queue/read failure; current subscribers all receive this failure.
            m_impl->encodingAt(compressionLevel).phase = RequestPhase::Empty;
            if (result.error.isEmpty())
                result.error = QStringLiteral("The screenshot PNG is unavailable");
        }
        if (auto pending = std::move(m_impl->encodingAt(compressionLevel).subscribers))
            subscribers = std::move(*pending);
    }
    for (auto& subscriber : subscribers) {
        if (!subscriber.receiver.isNull()) {
            ScreenshotExportEncodingResult delivered{result.image, result.error};
            dispatchResult(subscriber.receiver, std::move(subscriber.callback),
                           std::move(delivered));
        }
    }
}

std::optional<ScreenshotClipboardPlacement> ScreenshotExportArtifact::clipboardPlacement() const {
    QMutexLocker lock(&m_impl->mutex);
    auto placement = m_impl->source.m_clipboardPlacement;
    if (placement && m_impl->rowSource.isValid())
        placement->rasterSize = m_impl->rowSource.size;
    else if (placement && !m_impl->image.isNull())
        placement->rasterSize = m_impl->image.size();
    return placement;
}

std::optional<ScreenshotClipboardAppearance> ScreenshotExportArtifact::clipboardAppearance() const {
    if (!m_impl)
        return {};
    QMutexLocker lock(&m_impl->mutex);
    auto appearance = m_impl->source.m_clipboardAppearance;
    if (appearance && m_impl->rowSource.isValid())
        appearance->rasterSize = m_impl->rowSource.size;
    else if (appearance && !m_impl->image.isNull())
        appearance->rasterSize = m_impl->image.size();
    return appearance;
}

void ScreenshotExportArtifact::setClipboardFileMetadata(QMimeData& mime,
                                                        const QString& path) const {
    const QFileInfo info(path);
    const auto bindFile = [&info](auto& metadata) {
        metadata.filePath = screenshotClipboardFilePath(info.absoluteFilePath());
        metadata.fileSize = info.size();
        metadata.fileModifiedMs = info.lastModified().toUTC().toMSecsSinceEpoch();
    };
    if (auto placement = clipboardPlacement()) {
        bindFile(*placement);
        setScreenshotClipboardPlacement(mime, *placement);
    }
    if (auto appearance = clipboardAppearance()) {
        bindFile(*appearance);
        setScreenshotClipboardAppearance(mime, *appearance);
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
                [source = std::move(source), canonicalPng, payload,
                 placement = guardedArtifact->m_impl->source.m_clipboardPlacement,
                 appearance = guardedArtifact->m_impl->source.m_clipboardAppearance](
                    const ScreenshotExportCancellation& cancellation) mutable {
                    ScreenshotImageRowSource rows = withCancellation(
                        source, [&cancellation] { return cancellation.isCancellationRequested(); });
                    *payload = ScreenshotClipboardService::prepareEncoded(rows, canonicalPng,
                                                                          placement, appearance);
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
            snow_shot::runtime::trackRuntimeWork([guarded,
                                                  callback = std::move(callback)]() mutable {
                if (guarded && !guarded->isCancelled()) {
                    callback(ScreenshotExportTaskResult::failure(
                        ScreenshotExportFailureStage::File,
                        QCoreApplication::translate("ScreenshotExportArtifact",
                                                    "The image save directory is not configured")));
                }
            }),
            Qt::QueuedConnection);
    }
    return requestAutomaticSave(
        receiver, {directory}, ScreenshotImageFileService::formatForKey(settings.imageFormat()),
        settings.autoSaveFilenameFormat(),
        snow_shot::presentation::screenshotEncodingOptions(settings), std::move(callback),
        ScreenshotPdfOptions{screenshot_pdf::pageSizeForKey(settings.pdfPageSize())});
}

bool ScreenshotExportArtifact::requestSaveToPath(QObject* receiver, QString path,
                                                 ScreenshotImageFileFormat format,
                                                 ScreenshotImageEncodingOptions encoding,
                                                 ScreenshotExportCoordinator::Completion callback,
                                                 ScreenshotPdfOptions pdf,
                                                 std::shared_ptr<void> keepAlive) {
    if (receiver == nullptr || !callback || isCancelled())
        return false;
    const QPointer<ScreenshotExportArtifact> guarded(this);
    const QPointer<QObject> target(receiver);
    auto schedule = [guarded, target, path = std::move(path), format, pdf, keepAlive,
                     callback = std::move(callback)](std::shared_ptr<FileSource> source,
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
            [path, format, pdf, keepAlive,
             source = std::move(source)](const ScreenshotExportCancellation& cancellation) mutable {
                if (cancellation.isCancellationRequested()) {
                    return ScreenshotExportTaskResult::failure(
                        ScreenshotExportFailureStage::Cancelled,
                        QStringLiteral("The screenshot save was cancelled"));
                }
                Q_UNUSED(keepAlive);
                ScreenshotImageFileSaveResult saved;
                if (source->png.isValid()) {
                    saved = ScreenshotImageFileService::write(source->png, path, [&cancellation] {
                        return cancellation.isCancellationRequested();
                    });
                } else if (!source->bytes.isEmpty()) {
                    saved = ScreenshotImageFileService::writeEncodedBytes(
                        source->bytes, path, format,
                        [&cancellation] { return cancellation.isCancellationRequested(); });
                } else if (source->rows.isValid()) {
                    auto rows = withCancellation(source->rows, [&cancellation] {
                        return cancellation.isCancellationRequested();
                    });
                    saved = ScreenshotImageFileService::write(rows, path, format, pdf);
                } else if (source->pdf) {
                    saved = ScreenshotImageFileService::writePdf(
                        *source->pdf, path, pdf,
                        [&cancellation] { return cancellation.isCancellationRequested(); });
                } else {
                    saved = ScreenshotImageFileService::writeEncodedFile(
                        source->path, path, format,
                        [&cancellation] { return cancellation.isCancellationRequested(); });
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
                result.encodedSha256 = saved.encodedSha256;
                result.encodedByteCount = saved.encodedByteCount;
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
    return requestFileSource(format, encoding, std::move(schedule));
}

bool ScreenshotExportArtifact::requestFileSource(ScreenshotImageFileFormat format,
                                                 ScreenshotImageEncodingOptions encoding,
                                                 FileSourceCallback callback) {
    const QPointer<ScreenshotExportArtifact> guarded(this);
    return requestRowSource(this, [guarded, format, encoding, callback = std::move(callback)](
                                      ScreenshotImageRowSource rows, QString error) mutable {
        if (!guarded || guarded->isCancelled())
            return;
        if (!error.isEmpty()) {
            callback({}, std::move(error));
            return;
        }
        // BMP performs only row packing. Spooling an uncompressed raster larger
        // than the memory budget adds more disk traffic than streaming it again.
        if (format == ScreenshotImageFileFormat::Bmp &&
            qint64(rows.size.width()) * rows.size.height() * 4 + 122 >
                guarded->m_impl->maximumPngBytes) {
            auto source = std::make_shared<FileSource>();
            source->rows = std::move(rows);
            callback(std::move(source), {});
            return;
        }
        if (format == ScreenshotImageFileFormat::Png) {
            // The pixel budget limits starting a new in-memory encoding. An
            // already retained PNG may fit even when its source pixels do not.
            auto cached = guarded->cachedPng(encoding.compressionLevel);
            if (cached.isValid()) {
                auto source = std::make_shared<FileSource>();
                source->png = std::move(cached);
                callback(std::move(source), {});
                return;
            }
        }
        if (format == ScreenshotImageFileFormat::Png && guarded->shouldCachePng(rows.size)) {
            auto completion = std::make_shared<FileSourceCallback>(std::move(callback));
            if (!guarded->requestPng(guarded, encoding.compressionLevel,
                                     [completion](ScreenshotExportEncodingResult result) {
                                         auto source = std::make_shared<FileSource>();
                                         source->png = std::move(result.image);
                                         (*completion)(std::move(source), std::move(result.error));
                                     }))
                (*completion)({}, QStringLiteral("The screenshot export queue is full"));
            return;
        }
        guarded->requestPreparedFile(std::move(rows), format, encoding, std::move(callback));
    });
}

void ScreenshotExportArtifact::requestPreparedFile(ScreenshotImageRowSource rows,
                                                   ScreenshotImageFileFormat format,
                                                   ScreenshotImageEncodingOptions encoding,
                                                   FileSourceCallback callback) {
    const auto options = ScreenshotImageFileService::encodeOptions(format, encoding);
    const int quality =
        (format == ScreenshotImageFileFormat::Pdf || format == ScreenshotImageFileFormat::Jpeg)
            ? qMax(1, options.quality)
        : ScreenshotImageFileService::supportsQuality(format) ? options.quality
                                                              : 100;
    const int effort =
        format == ScreenshotImageFileFormat::Png                        ? options.compression_level
        : format == ScreenshotImageFileFormat::Webp && options.lossless ? options.lossless_effort
        : ScreenshotImageFileService::supportsCompressionLevel(format)  ? options.effort
                                                                        : 0;
    const Impl::FileKey key{format, quality, effort};
    std::shared_ptr<FileSource> ready;
    std::shared_ptr<std::vector<FileSourceCallback>> lifetimes;
    {
        QMutexLocker lock(&m_impl->mutex);
        if (m_impl->cancelled)
            return;
        auto& entry = m_impl->fileEncodings[key];
        if (entry.phase == RequestPhase::Ready) {
            ready = entry.source;
            entry.lastUsed = ++m_impl->pngUseSerial;
        } else {
            if (!entry.subscribers)
                entry.subscribers = std::make_shared<std::vector<FileSourceCallback>>();
            entry.subscribers->push_back(std::move(callback));
            if (entry.phase == RequestPhase::Pending)
                return;
            entry.phase = RequestPhase::Pending;
            lifetimes = entry.subscribers;
        }
    }
    if (ready) {
        callback(std::move(ready), {});
        return;
    }
    auto prepared = std::make_shared<FileSource>();
    const QPointer<ScreenshotExportArtifact> guarded(this);
    auto complete = [guarded, prepared, key](ScreenshotExportTaskResult result) mutable {
        if (!guarded)
            return;
        std::vector<FileSourceCallback> subscribers;
        {
            QMutexLocker lock(&guarded->m_impl->mutex);
            if (guarded->m_impl->cancelled)
                return;
            auto& entry = guarded->m_impl->fileEncodings[key];
            if (auto pending = std::move(entry.subscribers))
                subscribers = std::move(*pending);
            entry.phase = RequestPhase::Empty;
            entry.job = {};
            const qint64 budget = guarded->m_impl->maximumFileBytes;
            if (result.succeeded() && prepared->byteCount <= budget) {
                qint64 retained = 0;
                for (const auto& [unused, cached] : guarded->m_impl->fileEncodings) {
                    Q_UNUSED(unused);
                    if (cached.source)
                        retained += cached.source->byteCount;
                }
                while (retained > budget - prepared->byteCount) {
                    Impl::FileEncoding* oldest = nullptr;
                    for (auto& [unused, cached] : guarded->m_impl->fileEncodings) {
                        Q_UNUSED(unused);
                        if (cached.phase == RequestPhase::Ready &&
                            (!oldest || cached.lastUsed < oldest->lastUsed))
                            oldest = &cached;
                    }
                    if (!oldest)
                        break;
                    retained -= oldest->source->byteCount;
                    oldest->source.reset();
                    oldest->phase = RequestPhase::Empty;
                }
                guarded->m_impl->makeMemoryRoom(prepared->bytes.size());
                entry.source = prepared;
                entry.phase = RequestPhase::Ready;
                entry.lastUsed = ++guarded->m_impl->pngUseSerial;
            }
        }
        for (auto& subscriber : subscribers) {
            if (!guarded || guarded->isCancelled())
                break;
            subscriber(result.succeeded() ? prepared : nullptr, result.error);
        }
    };
    const auto notify = m_impl->fileEncodingStarted;
    const auto memoryLimit = m_impl->maximumPngBytes;
    const auto job = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Foreground,
        [rows = std::move(rows), prepared, lifetimes, format, encoding, notify,
         memoryLimit](const ScreenshotExportCancellation& cancellation) mutable {
            Q_UNUSED(lifetimes);
            rows = withCancellation(
                rows, [&cancellation] { return cancellation.isCancellationRequested(); });
            if (cancellation.isCancellationRequested())
                return ScreenshotExportTaskResult::failure(
                    ScreenshotExportFailureStage::Cancelled,
                    QStringLiteral("The screenshot save was cancelled"));
            if (notify)
                notify(format);
            QString error;
            if (format == ScreenshotImageFileFormat::Pdf) {
                prepared->pdf =
                    screenshot_pdf::prepare(rows, qBound(0, encoding.quality, 100), &error);
                if (prepared->pdf)
                    prepared->byteCount = QFileInfo(prepared->pdf->path()).size();
            } else {
                FileSource::Output output(*prepared, memoryLimit);
                if (!snow_shot::image_codec::encodeToDevice(
                        rows, &output, ScreenshotImageFileService::snowImageFormat(format),
                        ScreenshotImageFileService::encodeOptions(format, encoding), &error) ||
                    !output.finish()) {
                    if (error.isEmpty())
                        error = output.errorString();
                }
            }
            if (cancellation.isCancellationRequested())
                return ScreenshotExportTaskResult::failure(
                    ScreenshotExportFailureStage::Cancelled,
                    QStringLiteral("The screenshot save was cancelled"));
            if (!error.isEmpty() || prepared->byteCount <= 0)
                return ScreenshotExportTaskResult::failure(
                    ScreenshotExportFailureStage::File,
                    error.isEmpty() ? QStringLiteral("The image could not be encoded") : error);
            return ScreenshotExportTaskResult{};
        },
        complete);
    if (!job.isValid()) {
        complete(ScreenshotExportTaskResult::failure(
            ScreenshotExportFailureStage::Queue,
            QStringLiteral("The screenshot export queue is full")));
        return;
    }
    QMutexLocker lock(&m_impl->mutex);
    if (m_impl->cancelled)
        job.cancel();
    else
        m_impl->fileEncodings[key].job = job;
}

bool ScreenshotExportArtifact::requestClipboardFile(
    QObject* receiver, ScreenshotImageFileFormat format, QString filenameFormat,
    ScreenshotImageEncodingOptions encoding, ClipboardFileCallback callback,
    ScreenshotPdfOptions pdf, QDateTime requestedAt) {
    if (receiver == nullptr || !callback || isCancelled())
        return false;
    auto directory = std::make_shared<QTemporaryDir>(
        QDir::temp().filePath(QStringLiteral("snow-shot-clipboard-XXXXXX")));
    if (!directory->isValid()) {
        std::function<void(QString)> completion = [callback =
                                                       std::move(callback)](QString error) mutable {
            callback({}, std::move(error));
        };
        dispatchResult(receiver, std::move(completion), directory->errorString());
        return true;
    }
    const QString path = directory->path();
    return requestFileSave(
        receiver, {path}, format, std::move(filenameFormat), encoding,
        [this, directory = std::move(directory),
         callback = std::move(callback)](ScreenshotExportTaskResult result) mutable {
            if (!result.succeeded()) {
                callback({}, std::move(result.error));
                return;
            }
            auto mime = ScreenshotClipboardService::temporaryFileMimeData(result.savedPath,
                                                                          std::move(directory));
            setClipboardFileMetadata(*mime, result.savedPath);
            callback(std::move(mime), {});
        },
        pdf, requestedAt, ScreenshotExportCoordinator::Priority::Foreground);
}

bool ScreenshotExportArtifact::requestAutomaticSave(
    QObject* receiver, QStringList directories, ScreenshotImageFileFormat format,
    QString filenameFormat, ScreenshotImageEncodingOptions encoding,
    ScreenshotExportCoordinator::Completion callback, ScreenshotPdfOptions pdf,
    QDateTime requestedAt) {
    return requestFileSave(receiver, std::move(directories), format, std::move(filenameFormat),
                           encoding, std::move(callback), pdf, requestedAt,
                           ScreenshotExportCoordinator::Priority::Background);
}

bool ScreenshotExportArtifact::requestFileSave(QObject* receiver, QStringList directories,
                                               ScreenshotImageFileFormat format,
                                               QString filenameFormat,
                                               ScreenshotImageEncodingOptions encoding,
                                               ScreenshotExportCoordinator::Completion callback,
                                               ScreenshotPdfOptions pdf, QDateTime requestedAt,
                                               ScreenshotExportCoordinator::Priority priority) {
    if (receiver == nullptr || !callback || isCancelled())
        return false;
    const QPointer<ScreenshotExportArtifact> guarded(this);
    const QPointer<QObject> target(receiver);
    if (!requestedAt.isValid())
        requestedAt = QDateTime::currentDateTime();
    auto schedule = [guarded, target, directories = std::move(directories), format, pdf, priority,
                     requestedAt, filenameFormat = std::move(filenameFormat),
                     callback = std::move(callback)](std::shared_ptr<FileSource> source,
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
            target, priority,
            [directories, format, pdf, requestedAt, filenameFormat,
             source = std::move(source)](const ScreenshotExportCancellation& cancellation) mutable {
                if (cancellation.isCancellationRequested()) {
                    return ScreenshotExportTaskResult::failure(
                        ScreenshotExportFailureStage::Cancelled,
                        QStringLiteral("The screenshot save was cancelled"));
                }
                ScreenshotImageFileSaveResult saved;
                const auto cancelled = [&cancellation] {
                    return cancellation.isCancellationRequested();
                };
                if (source->png.isValid()) {
                    // Use the same cancellation-aware atomic writer as manual output.
                    saved = ScreenshotImageFileService::saveAutomatically(
                        source->png, directories, filenameFormat, requestedAt, cancelled);
                } else if (!source->bytes.isEmpty()) {
                    saved = ScreenshotImageFileService::saveEncodedBytesAutomatically(
                        source->bytes, directories, format, filenameFormat, requestedAt, cancelled);
                } else if (source->rows.isValid()) {
                    auto rows = withCancellation(source->rows, cancelled);
                    saved = ScreenshotImageFileService::saveAutomatically(
                        rows, directories, format, filenameFormat, requestedAt, pdf);
                } else if (source->pdf) {
                    saved = ScreenshotImageFileService::savePdfAutomatically(
                        *source->pdf, directories, filenameFormat, requestedAt, pdf, cancelled);
                } else {
                    saved = ScreenshotImageFileService::saveEncodedAutomatically(
                        source->path, directories, format, filenameFormat, requestedAt, cancelled);
                }
                if (!saved.succeeded()) {
                    return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::File,
                                                               saved.error);
                }
                ScreenshotExportTaskResult result;
                result.savedPath = saved.path;
                result.encodedSha256 = saved.encodedSha256;
                result.encodedByteCount = saved.encodedByteCount;
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
    return requestFileSource(format, encoding, std::move(schedule));
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
        for (auto& [key, encoding] : m_impl->fileEncodings) {
            Q_UNUSED(key);
            pending = pending || encoding.phase == RequestPhase::Pending;
            outputJobs.push_back(encoding.job);
            encoding.subscribers.reset();
            encoding.source.reset();
        }
        for (auto& encoding : m_impl->pngEncodings) {
            pending = pending || encoding.phase == RequestPhase::Pending;
            outputJobs.push_back(encoding.job);
            encoding.subscribers.reset();
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
