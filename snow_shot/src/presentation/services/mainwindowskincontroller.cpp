#include "snow_shot/presentation/mainwindowskincontroller.h"

#include "snow_shot/storage/applicationstorage.h"
#include "snowimageqtcodec.h"
#include "snow_draw_engine_qt/snow_canvas_region_filter.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QJsonValue>
#include <QPainter>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <list>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace snow_shot::presentation {
namespace {
constexpr qint64 MAX_PREPARED_PIXELS = 16LL * 1000 * 1000;
constexpr qint64 MAX_FILTER_PIXELS = 64LL * 1024 * 1024;
constexpr int PREPARATION_DEBOUNCE_MS = 80;
constexpr qsizetype MAX_IDLE_FRAME_BYTES = 64LL * 1024 * 1024;
constexpr int MAX_IDLE_FRAME_COUNT = 128;
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

QPointF positionFactors(SkinPosition position) {
    const int index = static_cast<int>(position);
    return QPointF((index % 3) * 0.5, (index / 3) * 0.5);
}
} // namespace

SkinPosition skinPositionFromString(const QString& position) {
    static const std::array<QString, 9> names = {
        QStringLiteral("top_left"),    QStringLiteral("top_center"),
        QStringLiteral("top_right"),   QStringLiteral("center_left"),
        QStringLiteral("center"),      QStringLiteral("center_right"),
        QStringLiteral("bottom_left"), QStringLiteral("bottom_center"),
        QStringLiteral("bottom_right")};
    const auto found = std::find(names.begin(), names.end(), position);
    return found == names.end() ? SkinPosition::Center
                                : static_cast<SkinPosition>(found - names.begin());
}

MainWindowSkinFrame prepareMainWindowSkin(const QImage& source, const QSize& logicalSize,
                                          qreal devicePixelRatio, MainWindowSkinDisplayMode mode,
                                          int blurLevel, SnowCanvasRegionFilterScratch* scratch,
                                          SkinPosition position) {
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
    const auto factors = positionFactors(position);
    // Contain anchors move only placement. Keep integer-rounding edge extension
    // identical at every anchor so its prepared raster remains reusable.
    const auto cropFactors =
        mode == MainWindowSkinDisplayMode::Overlay ? factors : QPointF(0.5, 0.5);
    const QRectF sampled((source.width() - sampledSize.width()) * cropFactors.x(),
                         (source.height() - sampledSize.height()) * cropFactors.y(),
                         sampledSize.width(), sampledSize.height());
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
    const QRectF placement((viewport.width() - content.width()) * factors.x() / viewport.width(),
                           (viewport.height() - content.height()) * factors.y() / viewport.height(),
                           qreal(content.width()) / viewport.width(),
                           qreal(content.height()) / viewport.height());
    return {std::move(prepared), placement};
}

struct MainWindowSkinController::Impl {
    struct Source {
        QString path;
        quint64 generation = 0;
        QImage image;
        image_codec::SkinDecodeError error = image_codec::SkinDecodeError::None;
        bool loading = false;
        bool validationPending = false;
        // Keep validation status when invisible skins release their decoded pixels.
        bool validated = false;
    };
    struct Profile {
        QString path;
        SkinPosition position = SkinPosition::Center;
        std::shared_ptr<Source> source;
    };
    struct RasterKey {
        quint64 generation = 0;
        QSize size;
        qreal dpr = 1.0;
        MainWindowSkinDisplayMode mode = MainWindowSkinDisplayMode::Overlay;
        int blur = 0;
        SkinPosition position = SkinPosition::Center;

        bool operator==(const RasterKey&) const = default;
    };
    struct Prepared {
        RasterKey key;
        MainWindowSkinFrame frame;
        QPixmap pixmap;

        qsizetype bytes() const {
            // Conservatively account for both the CPU raster and platform pixmap.
            return frame.image.sizeInBytes() + qint64(pixmap.width()) * pixmap.height() * 4;
        }
    };
    struct View {
        QPointer<QObject> object;
        SkinSurface surface = SkinSurface::MainWindow;
        QSize size;
        qreal dpr = 1.0;
        quint64 token = 0;
        qint64 readyAt = 0;
        bool pending = false;
        image_codec::SkinDecodeError error = image_codec::SkinDecodeError::None;
        std::shared_ptr<Prepared> prepared;
        QMetaObject::Connection destroyedConnection;
    };
    struct Request {
        std::shared_ptr<Source> source;
        RasterKey key;
        QImage decoded;
        bool prepareRaster = true;
        std::shared_ptr<std::atomic<bool>> cancelled;
    };
    struct Result {
        Request request;
        MainWindowSkinFrame frame;
        image_codec::SkinDecodeError decodeError = image_codec::SkinDecodeError::None;
        bool preparationFailed = false;
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
        // Accessed only by the single image worker, never by attached views.
        std::unique_ptr<SnowCanvasRegionFilterScratch> scratch;
        qsizetype retainedScratch = 0;
    };
    struct Retirement {
        std::unique_ptr<Executor> executor;
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };

    explicit Impl(MainWindowSkinController& owner) : q(owner) {
        clock.start();
        auto& appStorage = storage::ApplicationStorage::instance();
        if (!appStorage.isInitialized())
            static_cast<void>(appStorage.initialize());
        QObject::connect(&appStorage.configuration(), &storage::ConfigurationStore::valueChanged,
                         &q, [this](const QString& key) {
                             if (retiringSingleton || !isSkinKey(key) ||
                                 configurationRefreshPending)
                                 return;
                             configurationRefreshPending = true;
                             QTimer::singleShot(0, &q, [this] {
                                 configurationRefreshPending = false;
                                 readConfiguration(std::nullopt);
                             });
                         });
        readConfiguration(std::nullopt);
    }
    ~Impl() {
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

    static bool isSkinKey(const QString& key) {
        return key.startsWith(QStringLiteral("interface/skin_")) ||
               key.startsWith(QStringLiteral("interface/toolbar_skin_")) ||
               key.startsWith(QStringLiteral("interface/tray_menu_skin_"));
    }
    static std::size_t index(SkinSurface surface) {
        return static_cast<std::size_t>(surface);
    }
    Profile& profile(SkinSurface surface) {
        return profiles[index(surface)];
    }
    const Profile& profile(SkinSurface surface) const {
        return profiles[index(surface)];
    }
    static QString normalizedPath(const QString& path) {
        return path.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    }
    RasterKey keyFor(const View& view) const {
        const auto& selected = profile(view.surface);
        return {selected.source ? selected.source->generation : 0,
                view.size,
                view.dpr,
                mode,
                blur,
                mode == MainWindowSkinDisplayMode::Contain ? SkinPosition::Center
                                                           : selected.position};
    }
    static MainWindowSkinFrame positionedFrame(const std::shared_ptr<Prepared>& prepared,
                                               SkinPosition position) {
        if (!prepared)
            return {};
        auto result = prepared->frame;
        if (prepared->key.mode == MainWindowSkinDisplayMode::Contain) {
            const auto factors = positionFactors(position);
            result.normalizedPlacement.moveTo(
                (1.0 - result.normalizedPlacement.width()) * factors.x(),
                (1.0 - result.normalizedPlacement.height()) * factors.y());
        }
        return result;
    }
    void stopPreparationTimer() {
        if (timer) {
            timer->stop();
            timer.release()->deleteLater();
        }
    }
    void armPreparationTimer() {
        if (running)
            return;
        std::optional<qint64> next;
        for (const auto& view : views) {
            if (view.pending)
                next = next ? std::min(*next, view.readyAt) : view.readyAt;
        }
        if (!next) {
            stopPreparationTimer();
            return;
        }
        if (!timer) {
            timer = std::make_unique<QTimer>(&q);
            timer->setSingleShot(true);
            QObject::connect(timer.get(), &QTimer::timeout, &q, [this] { start(); });
        }
        timer->start(static_cast<int>(std::clamp(*next - clock.elapsed(), qint64(0), qint64(80))));
    }
    void reapRetirements() {
        for (auto entry = retirements.begin(); entry != retirements.end();) {
            if (entry->finished->load()) {
                entry->thread.join();
                // Destroy the GUI-affine pool after its workers and scratch are gone.
                entry = retirements.erase(entry);
            } else {
                ++entry;
            }
        }
    }
    bool hasConfiguredSkin() const {
        return std::any_of(profiles.begin(), profiles.end(),
                           [](const Profile& selected) { return !selected.path.isEmpty(); });
    }
    bool hasPending() const {
        return std::any_of(views.begin(), views.end(),
                           [](const View& view) { return view.pending; }) ||
               std::any_of(sources.begin(), sources.end(),
                           [](const auto& source) { return source->validationPending; });
    }
    void retireSingletonIfIdle() {
        if (!applicationOwned || retiringSingleton || hasConfiguredSkin() || running ||
            hasPending() || executor || !retirements.empty())
            return;
        retiringSingleton = true;
        if (applicationController == &q)
            applicationController.clear();
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
    void trimCache() {
        qsizetype idleBytes = 0;
        int idleCount = 0;
        for (const auto& prepared : frames) {
            if (prepared.use_count() == 1) {
                idleBytes += prepared->bytes();
                ++idleCount;
            }
        }
        for (auto entry = frames.begin();
             entry != frames.end() &&
             (idleBytes > MAX_IDLE_FRAME_BYTES || idleCount > MAX_IDLE_FRAME_COUNT);) {
            if (entry->use_count() == 1) {
                idleBytes -= (*entry)->bytes();
                --idleCount;
                entry = frames.erase(entry);
            } else {
                ++entry;
            }
        }
    }
    std::shared_ptr<Prepared> cached(const RasterKey& key) {
        const auto found = std::find_if(frames.begin(), frames.end(),
                                        [&key](const auto& frame) { return frame->key == key; });
        if (found == frames.end())
            return {};
        auto result = *found;
        frames.splice(frames.end(), frames, found);
        return result;
    }
    void rememberLegacy(const View& view) {
        if (view.surface == SkinSurface::MainWindow && view.token == legacyViewToken) {
            legacyFrame = view.prepared;
            legacyPosition = profile(SkinSurface::MainWindow).position;
        }
    }
    void notifyViews(const std::vector<QPointer<QObject>>& changed, bool legacyChanged) {
        const QPointer<MainWindowSkinController> receiver(&q);
        for (const auto& view : changed) {
            if (view)
                emit q.viewFrameChanged(view);
            if (!receiver)
                return;
        }
        if (legacyChanged)
            emit q.frameChanged();
    }
    void requestPreparation(View& view, bool immediate) {
        view.error = image_codec::SkinDecodeError::None;
        const auto& selected = profile(view.surface);
        view.pending = opacity > 0.0 && view.object && !view.size.isEmpty() &&
                       std::isfinite(view.dpr) && view.dpr > 0.0 && selected.source &&
                       selected.source->error == image_codec::SkinDecodeError::None;
        view.readyAt = immediate ? clock.elapsed() : clock.elapsed() + PREPARATION_DEBOUNCE_MS;
    }
    void pruneSourcesAndFrames() {
        for (auto source = sources.begin(); source != sources.end();) {
            const bool referenced =
                std::any_of(profiles.begin(), profiles.end(), [&source](const Profile& selected) {
                    return selected.source == source.value();
                });
            if (!referenced)
                source = sources.erase(source);
            else
                ++source;
        }
        for (auto frame = frames.begin(); frame != frames.end();) {
            const bool referenced =
                std::any_of(profiles.begin(), profiles.end(), [&frame](const Profile& selected) {
                    return selected.source &&
                           selected.source->generation == (*frame)->key.generation;
                });
            if (!referenced)
                frame = frames.erase(frame);
            else
                ++frame;
        }
        trimCache();
    }
    void readConfiguration(std::optional<SkinSurface> reloadSurface) {
        if (retiringSingleton)
            return;
        const QPointer<MainWindowSkinController> receiver(&q);
        const auto snapshot = storage::ApplicationStorage::instance().configuration().snapshot();
        static const std::array<QString, 3> pathKeys = {
            QStringLiteral("interface/skin_path"), QStringLiteral("interface/toolbar_skin_path"),
            QStringLiteral("interface/tray_menu_skin_path")};
        static const std::array<QString, 3> positionKeys = {
            QStringLiteral("interface/skin_position"),
            QStringLiteral("interface/toolbar_skin_position"),
            QStringLiteral("interface/tray_menu_skin_position")};
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
        const bool visibilityChanged = (nextOpacity > 0.0) != (opacity > 0.0);
        const bool preparationChanged = nextMode != mode || nextBlur != blur || visibilityChanged;
        const bool paintChanged = nextOpacity != opacity || nextMask != mask;
        mode = nextMode;
        blur = nextBlur;
        opacity = nextOpacity;
        mask = nextMask;
        if (opacity <= 0.0) {
            // In-flight jobs finish asynchronously but must never restore invisible pixels.
            if (auto cancellation = runningCancellation.lock())
                cancellation->store(true);
            frames.clear();
            legacyFrame.reset();
            for (const auto& source : sources)
                source->image = {};
        }
        const QString reloadedPath =
            reloadSurface
                ? normalizedPath(snapshot.value(pathKeys[index(*reloadSurface)]).toString())
                : QString();
        if (!reloadedPath.isEmpty())
            sources.remove(reloadedPath);
        std::array<bool, 3> sourceChanged{};
        std::array<bool, 3> positionChanged{};
        bool statusChanged = visibilityChanged;
        for (std::size_t i = 0; i < profiles.size(); ++i) {
            auto& selected = profiles[i];
            const auto nextPath = normalizedPath(snapshot.value(pathKeys[i]).toString());
            const auto nextPosition =
                skinPositionFromString(snapshot.value(positionKeys[i]).toString());
            sourceChanged[i] =
                selected.path != nextPath || (!nextPath.isEmpty() && nextPath == reloadedPath);
            positionChanged[i] = selected.position != nextPosition;
            selected.path = nextPath;
            selected.position = nextPosition;
            if (sourceChanged[i] || (!nextPath.isEmpty() && !selected.source)) {
                selected.source.reset();
                if (!nextPath.isEmpty()) {
                    auto& source = sources[nextPath];
                    if (!source) {
                        source = std::make_shared<Source>();
                        source->path = nextPath;
                        source->generation = ++sourceGeneration;
                    }
                    selected.source = source;
                }
                statusChanged = true;
            }
        }
        std::vector<QPointer<QObject>> changed;
        bool legacyChanged = false;
        for (auto& view : views) {
            const auto i = index(view.surface);
            const auto& selected = profile(view.surface);
            if (opacity <= 0.0 || !selected.source ||
                selected.source->error != image_codec::SkinDecodeError::None) {
                view.pending = false;
                view.error =
                    selected.source ? selected.source->error : image_codec::SkinDecodeError::None;
                if (view.prepared) {
                    view.prepared.reset();
                    changed.push_back(view.object);
                    legacyChanged = legacyChanged || view.surface == SkinSurface::MainWindow;
                }
            } else if (sourceChanged[i] || preparationChanged || positionChanged[i]) {
                if (!sourceChanged[i] && !preparationChanged &&
                    mode == MainWindowSkinDisplayMode::Contain && view.prepared &&
                    view.prepared->key == keyFor(view)) {
                    changed.push_back(view.object);
                    legacyChanged = legacyChanged || view.surface == SkinSurface::MainWindow;
                } else {
                    requestPreparation(view, sourceChanged[i]);
                }
            }
            rememberLegacy(view);
        }
        if (!profile(SkinSurface::MainWindow).source) {
            legacyChanged = legacyChanged || bool(legacyFrame);
            legacyFrame.reset();
        }
        legacyPosition = profile(SkinSurface::MainWindow).position;
        pruneSourcesAndFrames();
        if (!hasConfiguredSkin() || opacity <= 0.0) {
            stopPreparationTimer();
            if (!hasPending())
                retireExecutor();
            else
                start();
        } else {
            start();
        }
        if (!receiver)
            return;
        notifyViews(changed, legacyChanged);
        if (!receiver)
            return;
        if (statusChanged) {
            emit q.statusChanged();
            if (!receiver)
                return;
        }
        if (paintChanged)
            emit q.appearanceChanged();
    }
    void start() {
        if (running || retiringSingleton)
            return;
        const QPointer<MainWindowSkinController> receiver(&q);
        std::vector<QPointer<QObject>> changed;
        bool legacyChanged = false;
        std::optional<View> next;
        // Drain reusable rasters before choosing the next outstanding geometry.
        for (auto& view : views) {
            if (!view.pending || view.readyAt > clock.elapsed())
                continue;
            const auto& selected = profile(view.surface);
            if (!view.object || !selected.source ||
                selected.source->error != image_codec::SkinDecodeError::None) {
                view.pending = false;
                continue;
            }
            if (auto prepared = cached(keyFor(view))) {
                ++counts.cacheHits;
                view.prepared = std::move(prepared);
                view.pending = false;
                rememberLegacy(view);
                changed.push_back(view.object);
                legacyChanged = legacyChanged || view.surface == SkinSurface::MainWindow;
            } else if (!next ||
                       (view.token > lastServedToken &&
                        (next->token <= lastServedToken || view.token < next->token)) ||
                       (view.token <= lastServedToken && next->token <= lastServedToken &&
                        view.token < next->token)) {
                next = view;
            }
        }
        std::shared_ptr<Source> source;
        if (next) {
            source = profile(next->surface).source;
        } else {
            for (const auto& candidate : sources) {
                if (candidate->validationPending) {
                    source = candidate;
                    break;
                }
            }
        }
        bool notifyLoading = false;
        if (source) {
            running = true;
            if (next)
                lastServedToken = next->token;
            if (!executor) {
                if (!resources)
                    resources = std::make_shared<ResourceCounts>();
                executor = std::make_unique<Executor>(resources);
            }
            Request request{source, next ? keyFor(*next) : RasterKey{}, source->image,
                            next.has_value(),
                            next ? std::make_shared<std::atomic<bool>>(false) : nullptr};
            runningCancellation = request.cancelled;
            if (request.decoded.isNull()) {
                ++counts.decodeJobs;
                notifyLoading = !source->loading;
                source->loading = true;
            }
            if (request.prepareRaster)
                ++counts.preparationJobs;
            QObject* application = QCoreApplication::instance();
            Executor* worker = executor.get();
            worker->pool.start([request = std::move(request), receiver, application,
                                worker]() mutable {
                Result result{std::move(request), {}};
                const auto cancelled = [&] {
                    return result.request.cancelled && result.request.cancelled->load();
                };
                try {
                    if (!cancelled() && result.request.decoded.isNull()) {
                        auto decoded = image_codec::decodeSkinFile(result.request.source->path);
                        result.request.decoded = std::move(decoded.image);
                        result.decodeError = decoded.error;
                    }
                    if (!cancelled() && result.decodeError == image_codec::SkinDecodeError::None &&
                        result.request.decoded.isNull())
                        result.decodeError = image_codec::SkinDecodeError::ResourceLimit;
                    if (!cancelled() && result.decodeError == image_codec::SkinDecodeError::None &&
                        result.request.prepareRaster) {
                        if (result.request.key.blur > 0 && !worker->scratch)
                            worker->scratch = std::make_unique<SnowCanvasRegionFilterScratch>();
                        const auto& key = result.request.key;
                        result.frame = prepareMainWindowSkin(result.request.decoded, key.size,
                                                             key.dpr, key.mode, key.blur,
                                                             worker->scratch.get(), key.position);
                        result.preparationFailed = result.frame.image.isNull();
                    }
                } catch (...) {
                    if (result.request.decoded.isNull())
                        result.decodeError = image_codec::SkinDecodeError::ResourceLimit;
                    else
                        result.preparationFailed = true;
                }
                worker->updateScratchCount();
                QMetaObject::invokeMethod(
                    application,
                    [receiver, result = std::move(result)]() mutable {
                        if (receiver)
                            receiver->m_impl->finish(std::move(result));
                    },
                    Qt::QueuedConnection);
            });
        }
        armPreparationTimer();
        trimCache();
        notifyViews(changed, legacyChanged);
        if (!receiver)
            return;
        if (notifyLoading)
            emit q.statusChanged();
    }
    void finish(Result result) {
        const QPointer<MainWindowSkinController> receiver(&q);
        running = false;
        const auto source = result.request.source;
        const bool currentSource = sources.value(source->path) == source;
        const bool cancelled = result.request.cancelled && result.request.cancelled->load();
        if (currentSource)
            source->loading = false;
        std::vector<QPointer<QObject>> changed;
        bool legacyChanged = false;
        bool published = false;
        if (currentSource && !cancelled) {
            source->image = opacity > 0.0 ? std::move(result.request.decoded) : QImage{};
            source->validated = true;
            source->error = result.decodeError;
            source->loading = false;
            source->validationPending = false;
            std::shared_ptr<Prepared> prepared;
            for (auto& view : views) {
                if (opacity <= 0.0 || profile(view.surface).source != source)
                    continue;
                const bool decodeFailed = source->error != image_codec::SkinDecodeError::None;
                if (!decodeFailed &&
                    (!result.request.prepareRaster || keyFor(view) != result.request.key))
                    continue;
                view.pending = false;
                if (decodeFailed || result.preparationFailed) {
                    view.prepared.reset();
                    view.error =
                        decodeFailed ? source->error : image_codec::SkinDecodeError::ResourceLimit;
                } else {
                    if (!prepared) {
                        prepared = std::make_shared<Prepared>();
                        prepared->key = result.request.key;
                        prepared->frame = std::move(result.frame);
                        prepared->pixmap = QPixmap::fromImage(prepared->frame.image);
                        ++counts.pixmapConversions;
                        if (prepared->pixmap.isNull()) {
                            prepared.reset();
                            result.preparationFailed = true;
                            view.prepared.reset();
                            view.error = image_codec::SkinDecodeError::ResourceLimit;
                        } else {
                            frames.push_back(prepared);
                            view.prepared = prepared;
                            view.error = image_codec::SkinDecodeError::None;
                        }
                    } else {
                        view.prepared = prepared;
                        view.error = image_codec::SkinDecodeError::None;
                    }
                }
                rememberLegacy(view);
                changed.push_back(view.object);
                legacyChanged = legacyChanged || view.surface == SkinSurface::MainWindow;
                published = true;
            }
        }
        if (!published && (result.request.prepareRaster || !currentSource))
            ++counts.staleResults;
        trimCache();
        // Complete the state transition before synchronous listeners can clear or close a view.
        notifyViews(changed, legacyChanged);
        if (!receiver)
            return;
        if (currentSource) {
            emit q.statusChanged();
            if (!receiver)
                return;
        }
        if (hasPending()) {
            start();
            if (!receiver)
                return;
        }
        const bool hasUsableView =
            std::any_of(views.begin(), views.end(), [this](const View& view) {
                const auto& selected = profile(view.surface);
                return opacity > 0.0 && view.object && selected.source &&
                       selected.source->error == image_codec::SkinDecodeError::None;
            });
        if (!running && !hasPending() && !hasUsableView)
            retireExecutor();
        pollRetirements();
    }
    image_codec::SkinDecodeError errorFor(SkinSurface surface) const {
        const auto& selected = profile(surface);
        if (selected.source && selected.source->error != image_codec::SkinDecodeError::None)
            return selected.source->error;
        for (const auto& view : views) {
            if (view.surface == surface && view.error != image_codec::SkinDecodeError::None)
                return view.error;
        }
        return image_codec::SkinDecodeError::None;
    }

    bool loadingFor(SkinSurface surface) const {
        const auto& source = profile(surface).source;
        if (!source || (opacity <= 0.0 && !source->validationPending))
            return false;
        if (source->loading || source->validationPending)
            return true;
        return source->image.isNull() && source->error == image_codec::SkinDecodeError::None &&
               std::any_of(views.begin(), views.end(), [this, &source](const View& view) {
                   return view.pending && profile(view.surface).source == source;
               });
    }

    MainWindowSkinController& q;
    std::shared_ptr<ResourceCounts> resources;
    std::unique_ptr<Executor> executor;
    std::vector<Retirement> retirements;
    std::unique_ptr<QTimer> timer;
    std::unique_ptr<QTimer> cleanupTimer;
    QElapsedTimer clock;
    std::array<Profile, 3> profiles;
    QHash<QString, std::shared_ptr<Source>> sources;
    QHash<QObject*, View> views;
    std::list<std::shared_ptr<Prepared>> frames;
    QPointer<QObject> legacyView;
    std::shared_ptr<Prepared> legacyFrame;
    SkinPosition legacyPosition = SkinPosition::Center;
    MainWindowSkinDisplayMode mode = MainWindowSkinDisplayMode::Overlay;
    int blur = 0;
    qreal opacity = 1.0;
    qreal mask = 0.8;
    quint64 sourceGeneration = 0;
    quint64 nextViewToken = 0;
    quint64 legacyViewToken = 0;
    quint64 lastServedToken = 0;
    bool running = false;
    std::weak_ptr<std::atomic<bool>> runningCancellation;
    bool configurationRefreshPending = false;
    bool applicationOwned = false;
    bool retiringSingleton = false;
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
    attach(view, SkinSurface::MainWindow, size, devicePixelRatio);
}
void MainWindowSkinController::attach(QObject* object, SkinSurface surface, const QSize& size,
                                      qreal devicePixelRatio) {
    if (!object || m_impl->retiringSingleton)
        return;
    const QPointer<MainWindowSkinController> receiver(this);
    const QPointer<QObject> guardedObject(object);
    m_impl->readConfiguration(std::nullopt);
    if (!receiver || !guardedObject || m_impl->retiringSingleton)
        return;
    const bool wasLoading = m_impl->loadingFor(surface);
    const bool queuedBehindRunning = m_impl->running;
    auto& view = m_impl->views[object];
    if (!view.object || view.surface != surface) {
        if (view.token != 0 && view.token == m_impl->legacyViewToken) {
            m_impl->legacyView.clear();
            m_impl->legacyViewToken = 0;
            m_impl->legacyFrame.reset();
        }
        QObject::disconnect(view.destroyedConnection);
        view = {};
        view.object = object;
        view.surface = surface;
        view.token = ++m_impl->nextViewToken;
        view.destroyedConnection =
            connect(object, &QObject::destroyed, this, [this, object] { detach(object); });
    }
    view.size = size;
    view.dpr = devicePixelRatio;
    if (surface == SkinSurface::MainWindow) {
        m_impl->legacyView = object;
        m_impl->legacyViewToken = view.token;
    }
    m_impl->requestPreparation(view, true);
    m_impl->start();
    if (receiver && queuedBehindRunning && !wasLoading && m_impl->loadingFor(surface))
        emit statusChanged();
}
void MainWindowSkinController::detach(QObject* object) {
    const auto found = m_impl->views.find(object);
    if (found == m_impl->views.end())
        return;
    const auto surface = found->surface;
    const bool wasLoading = m_impl->loadingFor(surface);
    const bool legacy = found->token == m_impl->legacyViewToken;
    QObject::disconnect(found->destroyedConnection);
    m_impl->views.erase(found);
    if (legacy) {
        m_impl->legacyView.clear();
        m_impl->legacyViewToken = 0;
        m_impl->legacyFrame.reset();
    }
    m_impl->trimCache();
    if (m_impl->views.isEmpty()) {
        m_impl->stopPreparationTimer();
        m_impl->retireExecutor();
    } else {
        m_impl->armPreparationTimer();
    }
    if (wasLoading && !m_impl->loadingFor(surface))
        emit statusChanged();
}
void MainWindowSkinController::setViewport(QObject* object, const QSize& size,
                                           qreal devicePixelRatio, bool immediate) {
    const auto found = m_impl->views.find(object);
    if (found == m_impl->views.end() ||
        (found->size == size && qFuzzyCompare(found->dpr, devicePixelRatio)))
        return;
    const bool dprChanged = !qFuzzyCompare(found->dpr, devicePixelRatio);
    found->size = size;
    found->dpr = devicePixelRatio;
    m_impl->requestPreparation(*found, immediate || dprChanged);
    m_impl->start();
}
void MainWindowSkinController::reload() {
    reload(SkinSurface::MainWindow);
}
void MainWindowSkinController::reload(SkinSurface surface) {
    m_impl->readConfiguration(surface);
}
void MainWindowSkinController::validate(SkinSurface surface) {
    const QPointer<MainWindowSkinController> receiver(this);
    m_impl->readConfiguration(std::nullopt);
    if (!receiver || m_impl->retiringSingleton)
        return;
    const auto source = m_impl->profile(surface).source;
    if (!source || source->validated || source->error != image_codec::SkinDecodeError::None)
        return;
    const bool wasLoading = m_impl->loadingFor(surface);
    const bool queuedBehindRunning = m_impl->running;
    source->validationPending = true;
    m_impl->start();
    if (receiver && queuedBehindRunning && !wasLoading && m_impl->loadingFor(surface))
        emit statusChanged();
}
MainWindowSkinFrame MainWindowSkinController::frame() const {
    return Impl::positionedFrame(m_impl->legacyFrame, m_impl->legacyPosition);
}
MainWindowSkinFrame MainWindowSkinController::frame(QObject* object) const {
    const auto found = m_impl->views.constFind(object);
    return found == m_impl->views.cend()
               ? MainWindowSkinFrame{}
               : Impl::positionedFrame(found->prepared, m_impl->profile(found->surface).position);
}
QPixmap MainWindowSkinController::pixmap(QObject* object) const {
    const auto found = m_impl->views.constFind(object);
    return found == m_impl->views.cend() || !found->prepared ? QPixmap() : found->prepared->pixmap;
}
bool MainWindowSkinController::skinActive() const {
    return bool(m_impl->legacyFrame);
}
bool MainWindowSkinController::skinActive(QObject* object) const {
    const auto found = m_impl->views.constFind(object);
    return found != m_impl->views.cend() && bool(found->prepared);
}
qreal MainWindowSkinController::opacity() const {
    return m_impl->opacity;
}
qreal MainWindowSkinController::maskOpacity() const {
    return m_impl->mask;
}
bool MainWindowSkinController::hasError() const {
    return hasError(SkinSurface::MainWindow);
}
bool MainWindowSkinController::hasError(SkinSurface surface) const {
    return m_impl->errorFor(surface) != image_codec::SkinDecodeError::None;
}
QString MainWindowSkinController::statusText() const {
    return statusText(SkinSurface::MainWindow);
}
QString MainWindowSkinController::statusText(SkinSurface surface) const {
    if (m_impl->loadingFor(surface))
        return skinText(QT_TRANSLATE_NOOP("MainWindowSkin", "Loading skin..."));
    switch (m_impl->errorFor(surface)) {
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
    for (const auto& source : m_impl->sources)
        result.retainedBytes += source->image.sizeInBytes();
    for (const auto& prepared : m_impl->frames) {
        result.retainedBytes += prepared->bytes();
        if (prepared.use_count() == 1) {
            result.idleFrameBytes += prepared->bytes();
            ++result.idleFrameCount;
        }
    }
    // Frames from an old source can remain visible while the replacement is prepared.
    std::vector<const Impl::Prepared*> oldFrames;
    const auto countOldFrame = [&](const std::shared_ptr<Impl::Prepared>& prepared) {
        if (!prepared ||
            std::find(oldFrames.begin(), oldFrames.end(), prepared.get()) != oldFrames.end())
            return;
        const auto cached =
            std::any_of(m_impl->frames.begin(), m_impl->frames.end(),
                        [&prepared](const auto& entry) { return entry == prepared; });
        if (!cached) {
            oldFrames.push_back(prepared.get());
            result.retainedBytes += prepared->bytes();
        }
    };
    countOldFrame(m_impl->legacyFrame);
    for (const auto& view : m_impl->views)
        countOldFrame(view.prepared);
    if (m_impl->resources) {
        result.executorCount = m_impl->resources->executors.load();
        result.scratchRetainedBytes = m_impl->resources->scratchBytes.load();
        result.executorAllocated = result.executorCount > 0;
    }
    result.busy = m_impl->running || m_impl->hasPending() || !m_impl->retirements.empty();
    return result;
}
} // namespace snow_shot::presentation
