#include "snow_shot/presentation/mainwindowskincontroller.h"

#include "snow_shot/storage/applicationstorage.h"
#include "snowimageqtcodec.h"
#include "snow_draw_engine_qt/snow_canvas_region_filter.h"

#include <QCoreApplication>
#include <QJsonValue>
#include <QPainter>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace snow_shot::presentation {
namespace {
constexpr qint64 MAX_PREPARED_PIXELS = 16LL * 1000 * 1000;
constexpr qint64 MAX_FILTER_PIXELS = 64LL * 1024 * 1024;
constexpr int PREPARATION_DEBOUNCE_MS = 80;
QPointer<MainWindowSkinController> applicationController;

// Split each axis into the image interior and its two clamped edges. This extends
// edge pixels without allocating a scaled source, including for panoramic images.
void drawClampedImage(QImage& destination, const QImage& source, const QRectF& sourceRect) {
    struct Span {
        qreal destinationStart;
        qreal destinationExtent;
        qreal sourceStart;
        qreal sourceExtent;
    };
    const auto spans = [](qreal start, qreal extent, int sourceExtent, int destinationExtent) {
        const qreal scale = destinationExtent / extent;
        const qreal first = std::clamp(-start * scale, 0.0, qreal(destinationExtent));
        const qreal last =
            std::clamp((sourceExtent - start) * scale, 0.0, qreal(destinationExtent));
        return std::array<Span, 3>{
            Span{0.0, first, 0.0, 1.0},
            Span{first, last - first, std::max(0.0, start), (last - first) / scale},
            Span{last, destinationExtent - last, qreal(sourceExtent - 1), 1.0}};
    };
    const auto horizontal =
        spans(sourceRect.x(), sourceRect.width(), source.width(), destination.width());
    const auto vertical =
        spans(sourceRect.y(), sourceRect.height(), source.height(), destination.height());
    QPainter painter(&destination);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    for (const auto& y : vertical) {
        for (const auto& x : horizontal) {
            if (x.destinationExtent > 0.0 && y.destinationExtent > 0.0) {
                painter.drawImage(
                    QRectF(x.destinationStart, y.destinationStart, x.destinationExtent,
                           y.destinationExtent),
                    source, QRectF(x.sourceStart, y.sourceStart, x.sourceExtent, y.sourceExtent));
            }
        }
    }
}

QString skinText(const char* source) {
    return QCoreApplication::translate("MainWindowSkin", source);
}
} // namespace

MainWindowSkinFrame prepareMainWindowSkin(const QImage& source, const QSize& logicalSize,
                                          qreal devicePixelRatio, MainWindowSkinDisplayMode mode,
                                          int blurLevel, SnowCanvasRegionFilterScratch* scratch) {
    if (source.isNull() || logicalSize.isEmpty() || !std::isfinite(devicePixelRatio) ||
        devicePixelRatio <= 0.0) {
        return {};
    }
    qreal scale = devicePixelRatio;
    const qreal requestedPixels = qreal(logicalSize.width()) * logicalSize.height() * scale * scale;
    if (requestedPixels > MAX_PREPARED_PIXELS) {
        scale *= std::sqrt(MAX_PREPARED_PIXELS / requestedPixels);
    }
    QSize viewport;
    QSize content;
    SnowCanvasRegionFilterParameters parameters;
    parameters.logicalSigma = std::clamp(blurLevel, 0, 100);
    int padding = 0;
    // Padded filtering buffers are bounded independently of the retained frame.
    for (;;) {
        viewport = QSize(std::max(1, qRound(logicalSize.width() * scale)),
                         std::max(1, qRound(logicalSize.height() * scale)));
        content = mode == MainWindowSkinDisplayMode::Contain
                      ? source.size().scaled(viewport, Qt::KeepAspectRatio)
                      : viewport;
        content.setWidth(std::max(1, content.width()));
        content.setHeight(std::max(1, content.height()));
        parameters.devicePixelRatio = scale;
        padding = blurLevel > 0 ? snowCanvasRegionFilterSupportPixels(parameters) : 0;
        const qint64 paddedPixels =
            (qint64(content.width()) + 2LL * padding) * (content.height() + 2LL * padding);
        const qint64 contentPixels = qint64(content.width()) * content.height();
        if (paddedPixels <= MAX_FILTER_PIXELS && contentPixels <= MAX_PREPARED_PIXELS) {
            break;
        }
        const qreal reduction = std::min(qreal(MAX_FILTER_PIXELS) / qreal(paddedPixels),
                                         qreal(MAX_PREPARED_PIXELS) / qreal(contentPixels));
        scale *= std::sqrt(reduction) * 0.95;
    }
    const qreal imageScale = mode == MainWindowSkinDisplayMode::Overlay
                                 ? std::max(qreal(content.width()) / source.width(),
                                            qreal(content.height()) / source.height())
                                 : std::min(qreal(content.width()) / source.width(),
                                            qreal(content.height()) / source.height());
    const QSizeF sampledSize(content.width() / imageScale, content.height() / imageScale);
    const QRectF sampled((source.width() - sampledSize.width()) / 2.0,
                         (source.height() - sampledSize.height()) / 2.0, sampledSize.width(),
                         sampledSize.height());
    QImage prepared(content + QSize(padding * 2, padding * 2), QImage::Format_ARGB32_Premultiplied);
    if (prepared.isNull()) {
        return {};
    }
    prepared.fill(Qt::transparent);
    drawClampedImage(prepared, source,
                     sampled.adjusted(-padding / imageScale, -padding / imageScale,
                                      padding / imageScale, padding / imageScale));
    if (padding > 0) {
        QImage filtered = prepared.copy();
        std::optional<SnowCanvasRegionFilterScratch> temporaryScratch;
        if (scratch == nullptr) {
            temporaryScratch.emplace(0);
            scratch = &*temporaryScratch;
        }
        const bool succeeded =
            !filtered.isNull() &&
            applySnowCanvasRegionFilter(prepared, filtered, QRegion(prepared.rect()), parameters,
                                        scratch, true);
        scratch->finishFrame();
        if (!succeeded) {
            return {};
        }
        prepared = filtered.copy(QRect(QPoint(padding, padding), content));
    }
    const QRectF placement((viewport.width() - content.width()) / (2.0 * viewport.width()),
                           (viewport.height() - content.height()) / (2.0 * viewport.height()),
                           qreal(content.width()) / viewport.width(),
                           qreal(content.height()) / viewport.height());
    return {std::move(prepared), placement};
}

struct MainWindowSkinController::Impl {
    struct Request {
        quint64 sourceGeneration;
        quint64 revision;
        QString path;
        QImage source;
        QSize size;
        qreal dpr;
        MainWindowSkinDisplayMode mode;
        int blur;
    };
    struct Result {
        Request request;
        MainWindowSkinFrame frame;
        image_codec::SkinDecodeError error = image_codec::SkinDecodeError::None;
    };

    struct ResourceCounts {
        std::atomic<int> executors{0};
        std::atomic<qsizetype> scratchBytes{0};
    };

    struct Executor {
        explicit Executor(std::shared_ptr<ResourceCounts> resourceCounts)
            : counts(std::move(resourceCounts)) {
            pool.setMaxThreadCount(1);
            pool.setExpiryTimeout(30000);
            ++counts->executors;
        }

        ~Executor() {
            pool.waitForDone();
            releaseScratch();
            --counts->executors;
        }

        void releaseScratch() {
            scratch.reset();
            counts->scratchBytes -= retainedScratch;
            retainedScratch = 0;
        }

        void updateScratchCount() {
            const auto retained = scratch ? static_cast<qsizetype>(scratch->retainedBytes()) : 0;
            counts->scratchBytes += retained - retainedScratch;
            retainedScratch = retained;
        }

        std::shared_ptr<ResourceCounts> counts;
        QThreadPool pool;
        // Only this executor's single worker accesses the scratch. It is freed
        // with the executor, instead of living in process/thread-local storage.
        std::unique_ptr<SnowCanvasRegionFilterScratch> scratch;
        qsizetype retainedScratch = 0;
    };

    struct Retirement {
        std::unique_ptr<Executor> executor;
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };

    explicit Impl(MainWindowSkinController& owner) : q(owner) {
        auto& appStorage = storage::ApplicationStorage::instance();
        if (!appStorage.isInitialized()) {
            static_cast<void>(appStorage.initialize());
        }
        QObject::connect(&appStorage.configuration(), &storage::ConfigurationStore::valueChanged,
                         &q, [this](const QString& key) {
                             if (retiringSingleton ||
                                 !key.startsWith(QStringLiteral("interface/skin_")) ||
                                 configurationRefreshPending) {
                                 return;
                             }
                             configurationRefreshPending = true;
                             QTimer::singleShot(0, &q, [this] {
                                 configurationRefreshPending = false;
                                 readConfiguration(false);
                             });
                         });
        readConfiguration(false);
    }

    ~Impl() {
        // The singleton is application-owned. Teardown joins all owned work
        // before QApplication and the queued-callback receiver can disappear.
        timer.reset();
        cleanupTimer.reset();
        if (executor) {
            executor->pool.setExpiryTimeout(0);
            executor.reset();
        }
        for (auto& retirement : retirements) {
            if (retirement.thread.joinable())
                retirement.thread.join();
        }
    }

    void stopPreparationTimer() {
        if (timer) {
            timer->stop();
            timer.release()->deleteLater();
        }
    }

    void ensurePreparationTimer() {
        if (timer)
            return;
        timer = std::make_unique<QTimer>(&q);
        timer->setSingleShot(true);
        timer->setInterval(PREPARATION_DEBOUNCE_MS);
        QObject::connect(timer.get(), &QTimer::timeout, &q, [this] { start(); });
    }

    void reapRetirements() {
        for (auto entry = retirements.begin(); entry != retirements.end();) {
            if (entry->finished->load()) {
                entry->thread.join();
                // QThreadPool is a QObject created in this GUI thread. Its
                // workers and scratch are already gone, so destruction is
                // immediate and respects the pool's thread affinity.
                entry = retirements.erase(entry);
            } else {
                ++entry;
            }
        }
    }

    void retireSingletonIfIdle() {
        if (!applicationOwned || retiringSingleton || !path.isEmpty() || running || pending ||
            executor || !retirements.empty()) {
            return;
        }
        retiringSingleton = true;
        if (applicationController == &q)
            applicationController.clear();
        // Hide this controller before scheduling deletion. Rapid re-enable
        // creates a fresh singleton; the retired one ignores future settings.
        q.deleteLater();
    }

    void pollRetirements() {
        reapRetirements();
        if (retirements.empty()) {
            if (cleanupTimer) {
                cleanupTimer->stop();
                cleanupTimer.release()->deleteLater();
            }
            retireSingletonIfIdle();
            return;
        }
        if (!cleanupTimer) {
            cleanupTimer = std::make_unique<QTimer>(&q);
            cleanupTimer->setSingleShot(true);
            QObject::connect(cleanupTimer.get(), &QTimer::timeout, &q,
                             [this] { pollRetirements(); });
        }
        cleanupTimer->start(5);
    }

    void retireExecutor() {
        if (executor) {
            executor->pool.setExpiryTimeout(0);
            retirements.emplace_back();
            auto& retirement = retirements.back();
            retirement.finished = std::make_shared<std::atomic<bool>>(false);
            retirement.executor = std::move(executor);
            const auto completed = retirement.finished;
            Executor* retired = retirement.executor.get();
            retirement.thread = std::thread([retired, completed] {
                retired->pool.waitForDone();
                retired->releaseScratch();
                completed->store(true);
            });
        }
        pollRetirements();
    }

    void readConfiguration(bool forceReload) {
        if (retiringSingleton)
            return;
        const QPointer<MainWindowSkinController> receiver(&q);
        const auto snapshot = storage::ApplicationStorage::instance().configuration().snapshot();
        const QString nextPath = snapshot.value(QStringLiteral("interface/skin_path")).toString();
        const auto nextMode =
            snapshot.value(QStringLiteral("interface/skin_display_mode")).toString() ==
                    QStringLiteral("contain")
                ? MainWindowSkinDisplayMode::Contain
                : MainWindowSkinDisplayMode::Overlay;
        const int nextBlur = snapshot.value(QStringLiteral("interface/skin_blur_level")).toInt(0);
        const qreal nextOpacity =
            snapshot.value(QStringLiteral("interface/skin_opacity")).toInt(100) / 100.0;
        const qreal nextMask =
            snapshot.value(QStringLiteral("interface/skin_mask_opacity")).toInt(80) / 100.0;
        const bool sourceChanged = nextPath != path || forceReload;
        const bool preparationChanged = nextMode != mode || nextBlur != blur;
        const bool appearanceChanged = nextOpacity != opacity || nextMask != mask;
        path = nextPath;
        mode = nextMode;
        blur = nextBlur;
        opacity = nextOpacity;
        mask = nextMask;
        if (sourceChanged) {
            ++sourceGeneration;
            source = {};
            error = image_codec::SkinDecodeError::None;
            loading = !path.isEmpty() && view;
        }
        bool clearedFrame = false;
        if (path.isEmpty()) {
            stopPreparationTimer();
            pending = false;
            ++revision;
            if (!frame.image.isNull()) {
                frame = {};
                clearedFrame = true;
            }
            retireExecutor();
        } else if (sourceChanged || preparationChanged) {
            requestPreparation(sourceChanged);
        }
        // Complete state transitions and submit work before notifying clients.
        // A synchronous signal listener may clear, replace or close the skin.
        if (!receiver)
            return;
        if (clearedFrame) {
            emit q.frameChanged();
            if (!receiver)
                return;
        }
        if (sourceChanged) {
            emit q.statusChanged();
            if (!receiver)
                return;
        }
        if (appearanceChanged) {
            emit q.appearanceChanged();
        }
    }

    void requestPreparation(bool immediate) {
        ++revision;
        if (!view || size.isEmpty() || path.isEmpty() ||
            error != image_codec::SkinDecodeError::None) {
            return;
        }
        pending = true;
        if (immediate) {
            stopPreparationTimer();
            start();
        } else {
            ensurePreparationTimer();
            timer->start();
        }
    }

    void start() {
        if (running || !pending || !view || path.isEmpty()) {
            return;
        }
        pending = false;
        running = true;
        if (!executor) {
            if (!resources)
                resources = std::make_shared<ResourceCounts>();
            executor = std::make_unique<Executor>(resources);
        }
        const bool notifyLoading = source.isNull() && !loading;
        if (source.isNull()) {
            ++counts.decodeJobs;
            loading = true;
        }
        ++counts.preparationJobs;
        Request request{sourceGeneration, revision, path, source, size, dpr, mode, blur};
        const QPointer<MainWindowSkinController> receiver(&q);
        // The application outlives this executor. Deliver through it, so a deleted
        // window or controller is never dereferenced from the worker thread.
        QObject* application = QCoreApplication::instance();
        Executor* worker = executor.get();
        worker->pool.start([request = std::move(request), receiver, application, worker]() mutable {
            Result result{std::move(request), {}};
            try {
                if (result.request.source.isNull()) {
                    auto decoded = image_codec::decodeSkinFile(result.request.path);
                    result.request.source = std::move(decoded.image);
                    result.error = decoded.error;
                }
                if (result.error == image_codec::SkinDecodeError::None) {
                    if (result.request.blur > 0 && !worker->scratch)
                        worker->scratch = std::make_unique<SnowCanvasRegionFilterScratch>();
                    result.frame = prepareMainWindowSkin(
                        result.request.source, result.request.size, result.request.dpr,
                        result.request.mode, result.request.blur, worker->scratch.get());
                    if (result.frame.image.isNull()) {
                        result.error = image_codec::SkinDecodeError::ResourceLimit;
                    }
                }
            } catch (...) {
                result.error = image_codec::SkinDecodeError::ResourceLimit;
            }
            worker->updateScratchCount();
            QMetaObject::invokeMethod(
                application,
                [receiver, result = std::move(result)]() mutable {
                    if (receiver) {
                        receiver->m_impl->finish(std::move(result));
                    }
                },
                Qt::QueuedConnection);
        });
        if (notifyLoading)
            emit q.statusChanged();
    }

    void finish(Result result) {
        const QPointer<MainWindowSkinController> receiver(&q);
        running = false;
        const bool currentSource = result.request.sourceGeneration == sourceGeneration;
        if (currentSource) {
            source = std::move(result.request.source);
            error = result.error;
            loading = false;
            if (error != image_codec::SkinDecodeError::None) {
                frame = {};
                pending = false;
                stopPreparationTimer();
                retireExecutor();
                emit q.frameChanged();
                if (!receiver)
                    return;
            } else if (result.request.revision == revision && view) {
                frame = std::move(result.frame);
                emit q.frameChanged();
                if (!receiver)
                    return;
            } else {
                ++counts.staleResults;
            }
            emit q.statusChanged();
            if (!receiver)
                return;
        } else {
            ++counts.staleResults;
        }
        if (pending && (!timer || !timer->isActive())) {
            start();
            if (!receiver)
                return;
        }
        pollRetirements();
    }

    MainWindowSkinController& q;
    std::shared_ptr<ResourceCounts> resources;
    std::unique_ptr<Executor> executor;
    std::vector<Retirement> retirements;
    std::unique_ptr<QTimer> timer;
    std::unique_ptr<QTimer> cleanupTimer;
    QPointer<QObject> view;
    QSize size;
    qreal dpr = 1.0;
    QString path;
    MainWindowSkinDisplayMode mode = MainWindowSkinDisplayMode::Overlay;
    int blur = 0;
    qreal opacity = 1.0;
    qreal mask = 0.8;
    quint64 sourceGeneration = 0;
    quint64 revision = 0;
    bool running = false;
    bool pending = false;
    bool loading = false;
    bool configurationRefreshPending = false;
    bool applicationOwned = false;
    bool retiringSingleton = false;
    image_codec::SkinDecodeError error = image_codec::SkinDecodeError::None;
    QImage source;
    MainWindowSkinFrame frame;
    MainWindowSkinDiagnostics counts;
};

MainWindowSkinController::MainWindowSkinController(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this)) {}

MainWindowSkinController::~MainWindowSkinController() = default;

MainWindowSkinController& MainWindowSkinController::instance() {
    if (!applicationController) {
        applicationController = new MainWindowSkinController(QCoreApplication::instance());
        applicationController->m_impl->applicationOwned = true;
    }
    return *applicationController;
}

MainWindowSkinController* MainWindowSkinController::existingInstance() noexcept {
    return applicationController.data();
}

void MainWindowSkinController::attach(QObject* view, const QSize& size, qreal devicePixelRatio) {
    const QPointer<MainWindowSkinController> receiver(this);
    m_impl->view = view;
    m_impl->size = size;
    m_impl->dpr = devicePixelRatio;
    m_impl->readConfiguration(false);
    if (!receiver)
        return;
    m_impl->requestPreparation(true);
}

void MainWindowSkinController::detach(QObject* view) {
    if (m_impl->view == view) {
        m_impl->view.clear();
        m_impl->stopPreparationTimer();
        m_impl->pending = false;
        ++m_impl->revision;
        m_impl->retireExecutor();
    }
}

void MainWindowSkinController::setViewport(QObject* view, const QSize& size, qreal devicePixelRatio,
                                           bool immediate) {
    if (m_impl->view != view ||
        (m_impl->size == size && qFuzzyCompare(m_impl->dpr, devicePixelRatio))) {
        return;
    }
    const bool dprChanged = !qFuzzyCompare(m_impl->dpr, devicePixelRatio);
    m_impl->size = size;
    m_impl->dpr = devicePixelRatio;
    m_impl->requestPreparation(immediate || dprChanged);
}

void MainWindowSkinController::reload() {
    m_impl->readConfiguration(true);
}

MainWindowSkinFrame MainWindowSkinController::frame() const {
    return m_impl->frame;
}

bool MainWindowSkinController::skinActive() const {
    return !m_impl->frame.image.isNull();
}

qreal MainWindowSkinController::opacity() const {
    return m_impl->opacity;
}

qreal MainWindowSkinController::maskOpacity() const {
    return m_impl->mask;
}

bool MainWindowSkinController::hasError() const {
    return m_impl->error != image_codec::SkinDecodeError::None;
}

QString MainWindowSkinController::statusText() const {
    if (m_impl->loading) {
        return skinText(QT_TRANSLATE_NOOP("MainWindowSkin", "Loading skin..."));
    }
    switch (m_impl->error) {
    case image_codec::SkinDecodeError::None:
        return {};
    case image_codec::SkinDecodeError::UnsupportedFormat:
        return skinText(QT_TRANSLATE_NOOP("MainWindowSkin", "Choose a PNG, JPG, or WebP image."));
    case image_codec::SkinDecodeError::UnreadableFile:
        return skinText(QT_TRANSLATE_NOOP("MainWindowSkin", "The skin image could not be opened."));
    case image_codec::SkinDecodeError::InputTooLarge:
        return skinText(QT_TRANSLATE_NOOP("MainWindowSkin", "The skin image exceeds 64 MiB."));
    case image_codec::SkinDecodeError::InvalidImage:
        return skinText(
            QT_TRANSLATE_NOOP("MainWindowSkin", "The skin image could not be decoded."));
    case image_codec::SkinDecodeError::ResourceLimit:
        return skinText(
            QT_TRANSLATE_NOOP("MainWindowSkin", "The skin image exceeds processing limits."));
    }
    return {};
}

MainWindowSkinDiagnostics MainWindowSkinController::diagnostics() const {
    auto result = m_impl->counts;
    result.retainedBytes = m_impl->source.sizeInBytes() + m_impl->frame.image.sizeInBytes();
    if (m_impl->resources) {
        result.executorCount = m_impl->resources->executors.load();
        result.scratchRetainedBytes = m_impl->resources->scratchBytes.load();
        result.executorAllocated = result.executorCount > 0;
    }
    result.busy = m_impl->running || m_impl->pending || !m_impl->retirements.empty();
    return result;
}
} // namespace snow_shot::presentation
