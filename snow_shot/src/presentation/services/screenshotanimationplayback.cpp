#include "snow_shot/presentation/screenshotanimationplayback.h"

#include "snow_shot/presentation/screenshotpresentationframescheduler.h"
#include "snow_shot/runtime/runtimeactivitytracker.h"
#include "../../image/snowimageqtcodec.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"

#include <QCoreApplication>
#include <QColorSpace>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QThreadPool>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <list>
#include <new>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#elif defined(Q_OS_MACOS)
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

namespace {
constexpr qint64 animationDeadlineTolerance = 2'000'000;
qint64 monotonicNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Only animated pins share this deadline service. Capture retains its independent
// precise scheduler. Two milliseconds of late-only coalescing joins nearby pin wakes.
class AnimationDeadlines final : public QObject {
  public:
    explicit AnimationDeadlines(QObject* parent)
        : QObject(parent), m_scheduler(monotonicNow, [this] { deliver(); }, this) {}

    void schedule(QObject* owner, qint64 deadline, std::function<void()> callback) {
        m_entries[owner] = {deadline, std::move(callback)};
        arm();
    }
    void cancel(QObject* owner) {
        m_entries.erase(owner);
        arm();
    }
    quint64 wakeups() const {
        return m_wakeups;
    }
    quint64 activeDeadlines() const {
        return static_cast<quint64>(m_entries.size());
    }

  private:
    struct Entry {
        qint64 deadline;
        std::function<void()> callback;
    };
    void arm() {
        if (m_entries.empty()) {
            m_scheduler.stop();
            return;
        }
        qint64 earliest = std::numeric_limits<qint64>::max();
        for (const auto& [owner, entry] : m_entries) {
            Q_UNUSED(owner);
            earliest = std::min(earliest, entry.deadline);
        }
        m_scheduler.setDeadline(earliest > std::numeric_limits<qint64>::max() -
                                               animationDeadlineTolerance
                                    ? earliest
                                    : earliest + animationDeadlineTolerance);
    }
    void deliver() {
        ++m_wakeups;
        const qint64 now = monotonicNow();
        std::vector<std::function<void()>> callbacks;
        for (auto entry = m_entries.begin(); entry != m_entries.end();) {
            if (entry->second.deadline <= now) {
                callbacks.push_back(std::move(entry->second.callback));
                entry = m_entries.erase(entry);
            } else {
                ++entry;
            }
        }
        for (const auto& callback : callbacks)
            callback();
        arm();
    }
    ScreenshotPresentationFrameScheduler m_scheduler;
    std::unordered_map<QObject*, Entry> m_entries;
    quint64 m_wakeups = 0;
};

AnimationDeadlines& animationDeadlines() {
    static QPointer<AnimationDeadlines> service;
    if (!service)
        service = new AnimationDeadlines(QCoreApplication::instance());
    return *service;
}

QThreadPool& animationDecodePool() {
    static QPointer<QThreadPool> pool;
    if (!pool) {
        pool = new QThreadPool(QCoreApplication::instance());
        pool->setMaxThreadCount(2);
        pool->setExpiryTimeout(1000);
    }
    return *pool;
}

class AnimationFrameCache final {
  public:
    struct Frame {
        QImage original;
        QImage transformed;
    };
    qint64 retainedBytes() {
        const QMutexLocker lock(&m_mutex);
        prune();
        qint64 bytes = 0;
        for (const auto& entry : m_entries)
            bytes += entry.bytes();
        return bytes;
    }
    void trim() {
        const QMutexLocker lock(&m_mutex);
        m_entries.clear();
    }
    Frame find(const std::shared_ptr<const ScreenshotAnimationSource>& source, int index,
               const QTransform& transform) {
        const QMutexLocker lock(&m_mutex);
        prune();
        for (auto& entry : m_entries) {
            if (entry.source.lock() == source && entry.index == index &&
                entry.transform == transform) {
                entry.age = ++m_age;
                return {entry.original, entry.transformed};
            }
        }
        return {};
    }
    QImage findOriginal(const std::shared_ptr<const ScreenshotAnimationSource>& source, int index) {
        const QMutexLocker lock(&m_mutex);
        prune();
        for (auto& entry : m_entries) {
            if (entry.source.lock() == source && entry.index == index) {
                entry.age = ++m_age;
                return entry.original;
            }
        }
        return {};
    }
    void insert(const std::shared_ptr<const ScreenshotAnimationSource>& source, int index,
                const QTransform& transform, const QImage& original, const QImage& transformed) {
        constexpr qint64 perSourceLimit = 64 * 1024 * 1024;
        constexpr qint64 globalLimit = 256 * 1024 * 1024;
        const qint64 bytes = Entry::bytesFor(original, transformed);
        if (bytes <= 0 || bytes > perSourceLimit)
            return;
        const QMutexLocker lock(&m_mutex);
        prune();
        for (auto& entry : m_entries) {
            if (entry.source.lock() == source && entry.index == index &&
                entry.transform == transform) {
                entry.age = ++m_age;
                return;
            }
        }
        qint64 sourceBytes = 0;
        qint64 totalBytes = 0;
        for (const auto& entry : m_entries) {
            totalBytes += entry.bytes();
            if (entry.source.lock() == source)
                sourceBytes += entry.bytes();
        }
        while (!m_entries.empty() &&
               (sourceBytes > perSourceLimit - bytes || totalBytes > globalLimit - bytes)) {
            auto oldest = m_entries.end();
            for (auto entry = m_entries.begin(); entry != m_entries.end(); ++entry) {
                const bool sameSource = entry->source.lock() == source;
                if (sourceBytes > perSourceLimit - bytes && !sameSource)
                    continue;
                if (oldest == m_entries.end() || entry->age < oldest->age)
                    oldest = entry;
            }
            if (oldest == m_entries.end())
                break;
            const qint64 removed = oldest->bytes();
            if (oldest->source.lock() == source)
                sourceBytes -= removed;
            totalBytes -= removed;
            m_entries.erase(oldest);
        }
        try {
            m_entries.push_back({source, index, transform, original, transformed, ++m_age});
        } catch (const std::bad_alloc&) {
            // Cache nodes are optional. Give their storage back under pressure
            // while the caller retains its complete mandatory publication.
            m_entries.clear();
        }
    }
    void retainSource(const ScreenshotAnimationSource* source) {
        const QMutexLocker lock(&m_mutex);
        ++m_activeSources[source];
    }
    void releaseSource(const ScreenshotAnimationSource* source) {
        const QMutexLocker lock(&m_mutex);
        const auto found = m_activeSources.find(source);
        if (found == m_activeSources.end() || --found->second != 0)
            return;
        m_activeSources.erase(found);
        std::erase_if(m_entries, [source](const Entry& entry) {
            const auto owner = entry.source.lock();
            return !owner || owner.get() == source;
        });
    }

  private:
    struct Entry {
        std::weak_ptr<const ScreenshotAnimationSource> source;
        int index;
        QTransform transform;
        QImage original;
        QImage transformed;
        quint64 age;
        qint64 bytes() const {
            return bytesFor(original, transformed);
        }
        static qint64 bytesFor(const QImage& original, const QImage& transformed) {
            const bool shared = original.cacheKey() == transformed.cacheKey();
            // Include list links/allocator padding and conservative Qt image-data
            // and managed-raster control storage. Identity shares one raster and
            // its metadata. ICC data remains shared with the admitted source.
            constexpr qint64 nodeOverhead = sizeof(Entry) + 4 * sizeof(void*);
            constexpr qint64 rasterMetadata = 512;
            return nodeOverhead + original.sizeInBytes() + rasterMetadata +
                   (shared ? 0 : transformed.sizeInBytes() + rasterMetadata);
        }
    };
    void prune() {
        std::erase_if(m_entries, [](const Entry& entry) { return entry.source.expired(); });
    }
    QMutex m_mutex;
    std::list<Entry> m_entries;
    // Optional frames live until the last worker for a source retires. Closing
    // one pin must not evict immutable frames still useful to another pin.
    std::unordered_map<const ScreenshotAnimationSource*, std::size_t> m_activeSources;
    quint64 m_age = 0;
};

AnimationFrameCache& frameCache() {
    static auto* cache = new AnimationFrameCache;
    return *cache;
}

qint64 availableMemory() {
#ifdef Q_OS_WIN
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
        return static_cast<qint64>(
            std::min<quint64>(std::min(status.ullAvailPhys, status.ullAvailPageFile),
                              static_cast<quint64>(std::numeric_limits<qint64>::max())));
#elif defined(Q_OS_MACOS)
    vm_statistics64_data_t status{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t pageSize = 0;
    const auto host = mach_host_self();
    const bool available =
        host_page_size(host, &pageSize) == KERN_SUCCESS &&
        host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&status), &count) ==
            KERN_SUCCESS;
    mach_port_deallocate(mach_task_self(), host);
    if (available)
        return (static_cast<qint64>(status.free_count) +
                static_cast<qint64>(status.inactive_count)) *
               static_cast<qint64>(pageSize);
#endif
    return std::numeric_limits<qint64>::max();
}

// Reservations cover conservative decoder working storage and its current/next
// publication. Optional cache must give way before a mandatory operation fails.
class AnimationMemoryBudget final {
  public:
    bool reserve(const void* owner, qint64 bytes) {
        constexpr qint64 limit = 1024LL * 1024 * 1024;
        constexpr qint64 headroom = 64LL * 1024 * 1024;
        if (bytes < 0 || bytes > limit)
            return false;
        const QMutexLocker lock(&m_mutex);
        const qint64 previous = m_reservations[owner];
        const qint64 next = m_total - previous + bytes;
        const qint64 additional = std::max<qint64>(0, bytes - previous);
        const qint64 time = monotonicNow();
        if (additional != 0 || time - m_memoryChecked >= 5'000'000'000) {
            m_available = availableMemory();
            m_memoryChecked = time;
        }
        const bool underPressure = m_available < additional + headroom;
        if (next > limit - frameCache().retainedBytes() || underPressure) {
            frameCache().trim();
            if (underPressure)
                m_available = availableMemory();
        }
        if (next > limit || m_available < additional + headroom)
            return false;
        m_reservations[owner] = bytes;
        m_total = next;
        return true;
    }
    void release(const void* owner) {
        const QMutexLocker lock(&m_mutex);
        const auto found = m_reservations.find(owner);
        if (found != m_reservations.end()) {
            m_total -= found->second;
            m_reservations.erase(found);
        }
    }
    qint64 reservedBytes() {
        const QMutexLocker lock(&m_mutex);
        return m_total;
    }

  private:
    QMutex m_mutex;
    std::unordered_map<const void*, qint64> m_reservations;
    qint64 m_total = 0;
    qint64 m_available = std::numeric_limits<qint64>::max();
    qint64 m_memoryChecked = 0;
};

AnimationMemoryBudget& memoryBudget() {
    static auto* budget = new AnimationMemoryBudget;
    return *budget;
}

struct AnimationWorkerDiagnostics final {
    std::atomic<quint64> cacheHits = 0;
    std::atomic<quint64> cacheMisses = 0;
    std::atomic<quint64> decoderResets = 0;
    std::atomic<quint64> compositedFrames = 0;
};

QImage prepareImagePixels(QImage& original, const QTransform& transform,
                          const ScreenshotAnimationPlayback::PreparationCancellation& cancelled) {
    if (original.isNull() || cancelled->load())
        return {};
    const QColorSpace srgb(QColorSpace::SRgb);
    const bool preserveOriginalProfile =
        original.colorSpace().isValid() && original.colorSpace() != srgb;
    QImage normalized =
        preserveOriginalProfile
            ? snowCanvasColorConvertedImage(original, srgb, QImage::Format_ARGB32_Premultiplied)
            : snowCanvasConvertImage(original, QImage::Format_ARGB32_Premultiplied);
    if (normalized.devicePixelRatio() != 1.0) {
        if (!snowCanvasDetachImage(normalized))
            return {};
        normalized.setDevicePixelRatio(1.0);
    }
    // Format normalization preserves source pixels and metadata. A color conversion
    // retains the original profile and pixels in their separate source raster.
    if (!preserveOriginalProfile && !normalized.isNull())
        original = normalized;
    if (cancelled->load())
        return {};
    return transform.isIdentity()
               ? normalized
               : snowCanvasTransformImage(normalized, transform, Qt::SmoothTransformation);
}

struct ImagePreparationReservation final {
    ~ImagePreparationReservation() {
        memoryBudget().release(this);
    }
    static std::optional<qint64> transformedRasterBytes(const QImage& original,
                                                        const QTransform& transform) {
        for (qreal component :
             {transform.m11(), transform.m12(), transform.m13(), transform.m21(), transform.m22(),
              transform.m23(), transform.m31(), transform.m32(), transform.m33()})
            if (!std::isfinite(component))
                return {};
        if (original.isNull())
            return {};
        const QRectF mapped = transform.mapRect(QRectF(original.rect()));
        const qreal left = std::floor(mapped.left());
        const qreal top = std::floor(mapped.top());
        const qreal right = std::ceil(mapped.right());
        const qreal bottom = std::ceil(mapped.bottom());
        constexpr qreal maximum = std::numeric_limits<int>::max();
        // trueMatrix first converts these bounds to QRect, then negates its
        // top-left point. Validate both integer coordinates and extents before
        // Qt performs those conversions, including the unrepresentable -INT_MIN.
        if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) ||
            !std::isfinite(bottom) || left < -maximum || top < -maximum || right > maximum ||
            bottom > maximum || right - left <= 0 || bottom - top <= 0 || right - left > maximum ||
            bottom - top > maximum)
            return {};
        const long double bytes =
            static_cast<long double>(right - left) * static_cast<long double>(bottom - top) * 4;
        constexpr qint64 rasterLimit = 256 * 1024 * 1024;
        if (bytes > rasterLimit)
            return {};
        return static_cast<qint64>(bytes);
    }
    bool admit(const QImage& original, const QTransform& transform) const {
        const auto outputBytes = transformedRasterBytes(original, transform);
        if (!outputBytes)
            return false;
        const long double bytes = static_cast<long double>(original.sizeInBytes()) * 3 +
                                  static_cast<long double>(*outputBytes) + 8 * 1024 * 1024;
        return bytes <= static_cast<long double>(std::numeric_limits<qint64>::max()) &&
               memoryBudget().reserve(this, static_cast<qint64>(bytes));
    }
};

struct ImagePreparationDelivery final {
    ~ImagePreparationDelivery() {
        QObject::disconnect(destructionConnection);
    }
    QMutex mutex;
    QObject* recipient = nullptr;
    QMetaObject::Connection destructionConnection;
};

ScreenshotAnimationPlayback::PreparationCancellation
startImagePreparation(QObject* context, QImage original, const QTransform& transform,
                      ScreenshotAnimationPlayback::DecodeCompletion completion,
                      ScreenshotAnimationPlayback::PreparationCancellation cancelled = {}) {
    if (!cancelled)
        cancelled = std::make_shared<std::atomic_bool>(context == nullptr);
    if (!context)
        return cancelled;
    auto delivery = std::make_shared<ImagePreparationDelivery>();
    delivery->recipient = context;
    const QPointer<QObject> recipient(context);
    const std::weak_ptr<ImagePreparationDelivery> weakDelivery = delivery;
    delivery->destructionConnection =
        QObject::connect(context, &QObject::destroyed, [weakDelivery, cancelled] {
            cancelled->store(true);
            if (const auto pending = weakDelivery.lock()) {
                const QMutexLocker lock(&pending->mutex);
                pending->recipient = nullptr;
            }
        });
    animationDecodePool().start(snow_shot::runtime::trackRuntimeWork(
        [delivery, recipient, cancelled, original = std::move(original), transform,
         completion = std::move(completion)]() mutable {
            QImage transformed;
            QString error;
            const ImagePreparationReservation reservation;
            if (!cancelled->load()) {
                if (reservation.admit(original, transform))
                    transformed = prepareImagePixels(original, transform, cancelled);
                if (transformed.isNull() && !cancelled->load())
                    error = QCoreApplication::translate("SnowImageQtCodec",
                                                        "Could not decode animated image.");
            }
            if (cancelled->load())
                return;
            // The caller owns the receiver. Synchronize only posting against its
            // destruction; queued results never own a QObject whose deleter could
            // re-enter Qt's post-event lock while application cleanup discards them.
            const QMutexLocker lock(&delivery->mutex);
            if (!delivery->recipient || cancelled->load())
                return;
            QMetaObject::invokeMethod(
                delivery->recipient,
                [delivery, recipient, cancelled, completion = std::move(completion),
                 original = std::move(original), transformed = std::move(transformed),
                 error = std::move(error)]() mutable {
                    // Keep the cancellation hook until this result is delivered
                    // or discarded. This state owns no QObject or posted event.
                    static_cast<void>(delivery);
                    if (recipient && !cancelled->load() && completion)
                        completion(std::move(original), std::move(transformed), std::move(error));
                },
                Qt::QueuedConnection);
        }));
    return cancelled;
}

struct AnimationDecodeWorker final {
    explicit AnimationDecodeWorker(std::shared_ptr<AnimationWorkerDiagnostics> value)
        : diagnostics(std::move(value)) {}
    QMutex mutex;
    std::shared_ptr<const ScreenshotAnimationSource> source;
    std::unique_ptr<snow_shot::image_codec::AnimationDecoder> decoder;
    bool resetRequired = false;
    qint64 admittedBytes = 0;
    qint64 admittedNativeBytes = 0;
    qint64 maximumPublicationBytes = 0;
    std::shared_ptr<std::atomic_bool> activeCancellation;
    std::shared_ptr<AnimationWorkerDiagnostics> diagnostics;

    ~AnimationDecodeWorker() {
        decoder.reset();
        if (source)
            frameCache().releaseSource(source.get());
        memoryBudget().release(this);
    }

    void decode(ScreenshotAnimationPlayback::DecodeRequest request,
                const ScreenshotAnimationPlayback::DecodeCompletion& completion) {
        const auto cancelled = [&request] { return request.cancelled->load(); };
        QImage original;
        QImage transformed;
        QString error;
        const auto outputBytes = ImagePreparationReservation::transformedRasterBytes(
            request.source->firstFrame, request.transform);
        if (!outputBytes) {
            completion({}, {},
                       QCoreApplication::translate("SnowImageQtCodec",
                                                   "Could not decode animated image."));
            return;
        }
        {
            const QMutexLocker lock(&mutex);
            activeCancellation = request.cancelled;
            if (!cancelled()) {
                if (source != request.source) {
                    frameCache().retainSource(request.source.get());
                    decoder.reset();
                    if (source)
                        frameCache().releaseSource(source.get());
                    source = request.source;
                    resetRequired = false;
                }
                const qint64 sourceRasterBytes =
                    qint64(source->canvasSize.width()) * source->canvasSize.height() * 4;
                // Scaled transforms may enlarge both the old and upcoming display
                // rasters. Keep the largest allowance until this worker retires.
                maximumPublicationBytes =
                    std::max(maximumPublicationBytes,
                             sourceRasterBytes * 6 +
                                 std::max<qint64>(0, *outputBytes - sourceRasterBytes) * 2);
                auto cached =
                    frameCache().find(request.source, request.frameIndex, request.transform);
                original = std::move(cached.original);
                transformed = std::move(cached.transformed);
                if (!transformed.isNull())
                    ++diagnostics->cacheHits;
                else
                    ++diagnostics->cacheMisses;
                if (original.isNull())
                    original = frameCache().findOriginal(request.source, request.frameIndex);
                if (original.isNull() && request.frameIndex == 0) {
                    original = request.source->firstFrame;
                    if (!request.source->firstDisplayFrame.isNull() &&
                        request.source->firstDisplayFrame.colorSpace() == original.colorSpace() &&
                        (!original.colorSpace().isValid() ||
                         original.colorSpace() == QColorSpace(QColorSpace::SRgb)))
                        original = request.source->firstDisplayFrame;
                }
                if (original.isNull()) {
                    if (!decoder) {
                        // Both imported source rasters, old original/display, normalization,
                        // and the new transformed raster accompany the native working estimate.
                        decoder = snow_shot::image_codec::openAnimationDecoder(
                            source, &error,
                            [this](qint64 bytes) {
                                if (activeCancellation->load() ||
                                    bytes > std::numeric_limits<qint64>::max() -
                                                maximumPublicationBytes)
                                    return false;
                                admittedNativeBytes = std::max(admittedNativeBytes, bytes);
                                admittedBytes = std::max(
                                    admittedBytes, admittedNativeBytes + maximumPublicationBytes);
                                return memoryBudget().reserve(this, admittedBytes);
                            },
                            cancelled);
                    }
                    if (decoder && (resetRequired || decoder->frameIndex() > request.frameIndex)) {
                        ++diagnostics->decoderResets;
                        if (!decoder->reset(&error))
                            decoder.reset();
                        resetRequired = false;
                    }
                    while (decoder && error.isEmpty() && !cancelled() &&
                           decoder->frameIndex() < request.frameIndex) {
                        if (!decoder->advance(&error, cancelled)) {
                            resetRequired = true;
                            if (error.isEmpty() && !cancelled())
                                error = QCoreApplication::translate(
                                    "SnowImageQtCodec", "Could not decode animated image.");
                            break;
                        }
                        ++diagnostics->compositedFrames;
                    }
                    if (decoder && error.isEmpty() && !cancelled()) {
                        original = decoder->snapshot(&error);
                    }
                    if (cancelled())
                        resetRequired = true;
                }
                if (!cancelled() && !original.isNull() && transformed.isNull()) {
                    admittedBytes =
                        std::max({admittedBytes, admittedNativeBytes + maximumPublicationBytes,
                                  original.sizeInBytes() * 3 + *outputBytes + 8 * 1024 * 1024});
                    if (!memoryBudget().reserve(this, admittedBytes)) {
                        error = QCoreApplication::translate("SnowImageQtCodec",
                                                            "Could not decode animated image.");
                    }
                    if (error.isEmpty()) {
                        transformed =
                            prepareImagePixels(original, request.transform, request.cancelled);
                    }
                    if (transformed.isNull() && error.isEmpty())
                        error = QCoreApplication::translate("SnowImageQtCodec",
                                                            "Could not decode animated image.");
                    if (!cancelled() && !transformed.isNull())
                        frameCache().insert(request.source, request.frameIndex, request.transform,
                                            original, transformed);
                }
            }
        }
        completion(std::move(original), std::move(transformed), std::move(error));
    }
};
} // namespace

struct ScreenshotAnimationPlayback::Impl {
    struct Lifetime {
        Impl* owner = nullptr;
    };
    Impl(ScreenshotAnimationPlayback& object, Hooks value, Options execution)
        : q(object), hooks(std::move(value)), options(std::move(execution)),
          lifetime(std::make_shared<Lifetime>()) {
        lifetime->owner = this;
        if (!options.monotonicNanoseconds)
            options.monotonicNanoseconds = monotonicNow;
    }
    void shutdown() {
        cancel();
        lifetime->owner = nullptr;
    }
    qint64 now() const {
        return options.monotonicNanoseconds();
    }
    bool effective() const {
        return source && requested && !isSuspended && !finished && painted && !failed &&
               !transformHeld;
    }
    long double positionAt(qint64 time) const {
        if (!effective())
            return position;
        return position +
               static_cast<long double>(std::max<qint64>(0, time - anchor)) * speed / 100;
    }
    void settle() {
        const qint64 time = now();
        position = positionAt(time);
        anchor = time;
    }
    void settleDisplayed() {
        const bool playing = effective();
        settle();
        if (!playing)
            return;
        const long double end = displayedOccurrence +
                                starts[static_cast<std::size_t>(displayedIndex) + 1] -
                                starts[static_cast<std::size_t>(displayedIndex)];
        // Explicit interactions freeze the painted frame, even when decoding has
        // left the media clock ahead of the last visible complete occurrence.
        position =
            std::clamp(position, displayedOccurrence, std::nextafter(end, displayedOccurrence));
    }
    void changed() const {
        if (hooks.stateChanged)
            hooks.stateChanged();
    }
    void cancelDeadline() {
        if (options.cancelDeadline)
            options.cancelDeadline();
        else
            animationDeadlines().cancel(&q);
    }
    void cancelJob() {
        if (cancellation)
            cancellation->store(true);
        wanted.reset();
        wantedPresentation.reset();
        decodingPresentation.reset();
        manualTarget.reset();
        ready.reset();
    }
    void discardPending(bool preserveInitial = true) {
        if (pendingToken == 0 || (preserveInitial && !painted))
            return;
        const quint64 discarded = pendingToken;
        pendingToken = 0;
        pendingOriginal = {};
        if (hooks.frameDiscarded)
            hooks.frameDiscarded(discarded);
    }
    void cancel() {
        clearTransformPreparation();
        cancelDeadline();
        cancelJob();
        ++generation;
        decoding = false;
        pendingToken = 0;
        retireWorker();
    }
    void clearTransformPreparation() {
        if (transformCancellation)
            transformCancellation->store(true);
        transformCancellation.reset();
        preparedTransform.reset();
        deferredStepTarget.reset();
        transformHeld = false;
        transformAdopted = false;
        ++preparationSerial;
    }
    void releaseTransformHold() {
        const auto deferred = deferredStepTarget;
        if (!transformAdopted)
            transform = displayedTransform;
        clearTransformPreparation();
        anchor = now();
        if (deferred && source) {
            manualTarget = deferred;
            position = starts[static_cast<std::size_t>(*deferred)];
            requestFrame(*deferred, true);
        } else {
            schedule();
        }
        changed();
    }
    void retireWorker() {
        if (!worker)
            return;
        // Decoder operations and destruction stay off the UI thread. The job's
        // shared owner also prevents retirement from racing an admitted advance.
        animationDecodePool().start(snow_shot::runtime::trackRuntimeWork(
            [retired = std::exchange(worker, {})]() mutable { retired.reset(); }, false));
    }
    int frameAt(long double at, bool* atEnd = nullptr) const {
        const bool ended = source->totalPlayCount != 0 &&
                           at >= total * static_cast<long double>(source->totalPlayCount);
        if (atEnd)
            *atEnd = ended;
        if (ended)
            return static_cast<int>(source->frameDurationsNs.size()) - 1;
        const long double phase = std::fmod(std::max<long double>(0, at), total);
        const auto found = std::upper_bound(starts.begin(), starts.end(), phase);
        return std::clamp(static_cast<int>(found - starts.begin()) - 1, 0,
                          static_cast<int>(source->frameDurationsNs.size()) - 1);
    }
    long double nextBoundary(long double at) const {
        const long double phase = std::fmod(std::max<long double>(0, at), total);
        const int index = frameAt(at);
        return at - phase + starts[static_cast<std::size_t>(index) + 1];
    }
    struct Presentation {
        long double at;
        long double until;
    };
    static qint64 saturatedAdd(qint64 time, qint64 delay) {
        return time > std::numeric_limits<qint64>::max() - delay
                   ? std::numeric_limits<qint64>::max()
                   : time + delay;
    }
    qint64 deadlineFor(long double mediaTime, qint64 time, long double at) const {
        const long double remaining = std::max<long double>(0, mediaTime - at) * 100 / speed;
        const auto delay = static_cast<qint64>(std::clamp<long double>(
            std::ceil(remaining), 1,
            static_cast<long double>(std::numeric_limits<qint64>::max()) / 2));
        return saturatedAdd(time, delay);
    }
    qint64 nominalLateness() const {
        return options.scheduleDeadline
                   ? std::clamp<qint64>(options.nominalDeadlineLatenessNs, 0, 1'000'000'000)
                   : animationDeadlineTolerance;
    }
    qint64 wakeDeadlineFor(long double mediaTime, qint64 time, long double at) const {
        return std::max(saturatedAdd(time, 1),
                        deadlineFor(mediaTime, time, at) - nominalLateness());
    }
    void armDeadline(qint64 deadline) {
        const std::weak_ptr<Lifetime> weak = lifetime;
        const auto wakeup = [weak] {
            if (const auto alive = weak.lock(); alive && alive->owner)
                alive->owner->tick();
        };
        if (options.scheduleDeadline)
            options.scheduleDeadline(deadline, wakeup);
        else
            animationDeadlines().schedule(&q, deadline, wakeup);
    }
    void observeLatency(qint64 latency, bool expired) {
        // An index may be needed in a later loop when sequential composition is
        // slower than the source rate. Widen the future window until a complete
        // frame can arrive before its occurrence, then reduce it conservatively.
        constexpr qint64 maximumLead = 5'000'000'000;
        constexpr qint64 margin = 2'000'000;
        const auto estimate = static_cast<qint64>(std::clamp<long double>(
            static_cast<long double>(latency) * 1.25L + margin, margin, maximumLead));
        const qint64 previous = expired ? std::min(maximumLead, preparationLead * 2)
                                        : preparationLead - preparationLead / 16;
        preparationLead = std::max(estimate, previous);
    }
    void schedule() {
        if (!effective() || pendingToken != 0) {
            cancelDeadline();
            return;
        }
        const qint64 time = now();
        const long double at = positionAt(time);
        bool atEnd = false;
        const int target = frameAt(at, &atEnd);
        const qint64 terminal =
            source->totalPlayCount == 0
                ? std::numeric_limits<qint64>::max()
                : deadlineFor(total * static_cast<long double>(source->totalPlayCount), time, at);
        if (atEnd) {
            armDeadline(saturatedAdd(time, 1));
            return;
        }
        const qint64 nextPresentation =
            saturatedAdd(std::max(lastPaintTime, lastAttemptTime), refreshPeriod);
        if (ready) {
            const long double earliestPresentation = positionAt(
                saturatedAdd(std::max(saturatedAdd(time, 1), nextPresentation), nominalLateness()));
            if (ready->index == target && target != displayedIndex && at < ready->presentation.at &&
                earliestPresentation < nextBoundary(at))
                ready->presentation = {at, nextBoundary(at)};
            const qint64 deadline =
                std::max(wakeDeadlineFor(ready->presentation.at, time, at), nextPresentation);
            const long double expectedAt = positionAt(saturatedAdd(deadline, nominalLateness()));
            if (at >= ready->presentation.until || expectedAt >= ready->presentation.until) {
                bool expectedEnd = false;
                if (ready->index == frameAt(expectedAt, &expectedEnd) && !expectedEnd) {
                    // Composition is immutable across loops. Identical pixels
                    // can serve a fresh occurrence at the actual future sample.
                    ready->presentation = {expectedAt, nextBoundary(expectedAt)};
                } else {
                    ++metrics.staleCompletions;
                    ready.reset();
                }
            }
            if (ready && ready->index == displayedIndex) {
                ready.reset();
            } else if (ready) {
                armDeadline(std::min(deadline, terminal));
                return;
            }
        }
        if (decoding) {
            // No polling or retargeting while an admitted operation progresses.
            // Speed changes rebase a future occurrence through the source clock.
            if (decodingPresentation && at < decodingPresentation->at)
                armDeadline(
                    std::min(wakeDeadlineFor(decodingPresentation->at, time, at), terminal));
            else if (terminal != std::numeric_limits<qint64>::max())
                armDeadline(terminal);
            else
                cancelDeadline();
            return;
        }
        qint64 deadline =
            std::max({target == displayedIndex ? deadlineFor(nextBoundary(at), time, at)
                                               : saturatedAdd(time, 1),
                      nextPresentation, saturatedAdd(time, preparationLead)});
        long double mediaTime = positionAt(saturatedAdd(deadline, nominalLateness()));
        bool preparationEnds = false;
        int upcoming = frameAt(mediaTime, &preparationEnds);
        const long double phase = std::fmod(std::max<long double>(0, mediaTime), total);
        if (!preparationEnds &&
            static_cast<long double>(preparationLead) >
                starts[static_cast<std::size_t>(upcoming) + 1] -
                    starts[static_cast<std::size_t>(upcoming)] &&
            phase > starts[static_cast<std::size_t>(upcoming)]) {
            // Retain a complete occurrence for presentation. Preparing near the
            // end of a short frame otherwise leaves only timer jitter to spare.
            mediaTime = nextBoundary(mediaTime);
            deadline = wakeDeadlineFor(mediaTime, time, at);
            upcoming = frameAt(mediaTime, &preparationEnds);
        }
        if (!preparationEnds && upcoming == displayedIndex) {
            mediaTime = nextBoundary(mediaTime);
            deadline = wakeDeadlineFor(mediaTime, time, at);
            upcoming = frameAt(mediaTime, &preparationEnds);
        }
        armDeadline(std::min(deadline, terminal));
        if (!preparationEnds && deadline < terminal)
            requestFrame(upcoming, false, true, Presentation{mediaTime, nextBoundary(mediaTime)});
    }
    void tick() {
        ++metrics.deadlineWakeups;
        if (!effective())
            return;
        lastAttemptTime = now();
        bool atEnd = false;
        const int target = frameAt(positionAt(now()), &atEnd);
        if (atEnd) {
            settle();
            finished = true;
            requested = false;
            cancelDeadline();
            cancelJob();
            if (target == displayedIndex)
                retireWorker();
            changed();
            if (target != displayedIndex)
                requestFrame(target, true);
            return;
        }
        if (ready && ready->index == target && target != displayedIndex) {
            auto prepared = std::move(*ready);
            ready.reset();
            publish(std::move(prepared.original), std::move(prepared.transformed), target);
        } else {
            schedule();
        }
    }
    void publish(QImage original, QImage transformed, int index) {
        pendingIndex = index;
        pendingToken = ++token;
        pendingOriginal = original;
        ++metrics.publishedFrames;
        if (hooks.frameReady)
            hooks.frameReady(std::move(original), std::move(transformed), index, pendingToken);
    }
    void requestFrame(int index, bool forced = false, bool prepare = false,
                      std::optional<Presentation> presentation = {}) {
        if (!source || failed || (!forced && !effective()))
            return;
        wanted = index;
        wantedForced = forced;
        wantedPrepared = prepare;
        wantedPresentation = presentation;
        if (decoding) {
            if (forced && decodingIndex != index && cancellation)
                cancellation->store(true);
            return;
        }
        if (!prepare)
            cancelDeadline();
        decoding = true;
        decodingIndex = index;
        decodingPresentation = presentation;
        cancellation = std::make_shared<std::atomic_bool>(false);
        const auto cancelled = cancellation;
        const quint64 epoch = generation;
        const qint64 admittedAt = now();
        const std::weak_ptr<Lifetime> weak = lifetime;
        DecodeRequest request{source, index, transform, cancelled};
        ++metrics.decodeRequests;
        const auto completion = [weak, epoch, index, cancelled, admittedAt, forced,
                                 presentation](QImage original, QImage transformed, QString error) {
            if (const auto alive = weak.lock(); alive && alive->owner)
                alive->owner->completed(epoch, index, cancelled, admittedAt, forced, presentation,
                                        std::move(original), std::move(transformed),
                                        std::move(error));
        };
        if (options.decode) {
            options.decode(std::move(request), completion);
        } else {
            if (!worker)
                worker = std::make_shared<AnimationDecodeWorker>(workerMetrics);
            const auto activeWorker = worker;
            animationDecodePool().start(snow_shot::runtime::trackRuntimeWork(
                [activeWorker, request = std::move(request), completion]() mutable {
                    activeWorker->decode(
                        std::move(request),
                        [completion](QImage original, QImage transformed, QString error) mutable {
                            QMetaObject::invokeMethod(
                                QCoreApplication::instance(),
                                [completion, original = std::move(original),
                                 transformed = std::move(transformed),
                                 error = std::move(error)]() mutable {
                                    completion(std::move(original), std::move(transformed),
                                               std::move(error));
                                },
                                Qt::QueuedConnection);
                        });
                }));
        }
    }
    void completed(quint64 epoch, int index, const std::shared_ptr<std::atomic_bool>& cancelled,
                   qint64 admittedAt, bool forced, std::optional<Presentation> presentation,
                   QImage original, QImage transformed, QString error) {
        if (epoch != generation) {
            ++metrics.staleCompletions;
            return;
        }
        decoding = false;
        decodingPresentation.reset();
        const qint64 latency = std::max<qint64>(0, now() - admittedAt);
        ++metrics.completedDecodes;
        metrics.decodeLatencyNs += latency;
        metrics.maximumDecodeLatencyNs = std::max(metrics.maximumDecodeLatencyNs, latency);
        if (cancelled->load()) {
            ++metrics.staleCompletions;
            const auto next = wanted;
            const bool retryForced = wantedForced;
            const bool prepare = wantedPrepared;
            const auto planned = wantedPresentation;
            if (next && (effective() || retryForced))
                requestFrame(*next, retryForced, prepare, planned);
            else
                schedule();
            return;
        }
        if (!error.isEmpty() || original.isNull() || transformed.isNull()) {
            settleDisplayed();
            requested = false;
            failed = true;
            wanted.reset();
            cancelDeadline();
            retireWorker();
            changed();
            if (hooks.error)
                hooks.error(error.isEmpty()
                                ? QCoreApplication::translate("SnowImageQtCodec",
                                                              "Could not decode animated image.")
                                : std::move(error));
            return;
        }
        wanted.reset();
        wantedPresentation.reset();
        if (presentation && effective()) {
            const long double at = positionAt(now());
            bool atEnd = false;
            if (at >= presentation->until && index == frameAt(at, &atEnd) && !atEnd)
                presentation = Presentation{at, nextBoundary(at)};
            const bool expired = at >= presentation->until;
            observeLatency(latency, expired);
            if (expired) {
                ++metrics.staleCompletions;
            } else {
                ready = PreparedFrame{std::move(original), std::move(transformed), index,
                                      *presentation};
            }
            schedule();
            return;
        }
        if (!forced) {
            ++metrics.staleCompletions;
            schedule();
            return;
        }
        publish(std::move(original), std::move(transformed), index);
    }
    void step(int delta) {
        if (!source)
            return;
        if (transformHeld) {
            const int base = deferredStepTarget.value_or(displayedIndex);
            const int count = static_cast<int>(source->frameDurationsNs.size());
            deferredStepTarget = (base + delta + count) % count;
            requested = false;
            finished = false;
            failed = false;
            changed();
            return;
        }
        const int base = manualTarget.value_or(displayedIndex);
        settle();
        requested = false;
        finished = false;
        failed = false;
        cancelDeadline();
        cancelJob();
        discardPending();
        const int count = static_cast<int>(source->frameDurationsNs.size());
        const int next = (base + delta + count) % count;
        manualTarget = next;
        position = starts[static_cast<std::size_t>(next)];
        anchor = now();
        requestFrame(next, true);
        changed();
    }
    void setSpeed(int value) {
        value = std::clamp(value, 10, 400);
        if (speed == value)
            return;
        settleDisplayed();
        speed = value;
        schedule();
        changed();
    }

    ScreenshotAnimationPlayback& q;
    Hooks hooks;
    Options options;
    std::shared_ptr<Lifetime> lifetime;
    std::shared_ptr<const ScreenshotAnimationSource> source;
    std::shared_ptr<AnimationDecodeWorker> worker;
    std::shared_ptr<AnimationWorkerDiagnostics> workerMetrics =
        std::make_shared<AnimationWorkerDiagnostics>();
    std::shared_ptr<std::atomic_bool> cancellation;
    PreparationCancellation transformCancellation;
    std::optional<PreparedTransform> preparedTransform;
    std::optional<int> deferredStepTarget;
    std::vector<long double> starts;
    QTransform transform;
    QTransform displayedTransform;
    QImage pendingOriginal;
    QImage displayedOriginal;
    struct PreparedFrame {
        QImage original;
        QImage transformed;
        int index;
        Presentation presentation;
    };
    std::optional<PreparedFrame> ready;
    long double total = 0;
    long double position = 0;
    long double displayedOccurrence = 0;
    qint64 anchor = 0;
    qint64 lastPaintTime = 0;
    qint64 lastAttemptTime = 0;
    qint64 refreshPeriod = 16'666'667;
    qint64 preparationLead = 0;
    quint64 generation = 0;
    quint64 preparationSerial = 0;
    quint64 token = 0;
    quint64 pendingToken = 0;
    quint64 paintedToken = 0;
    int speed = 100;
    int displayedIndex = 0;
    int pendingIndex = 0;
    int decodingIndex = 0;
    std::optional<int> wanted;
    std::optional<int> manualTarget;
    std::optional<Presentation> decodingPresentation;
    std::optional<Presentation> wantedPresentation;
    bool wantedForced = false;
    bool wantedPrepared = false;
    bool requested = false;
    bool isSuspended = true;
    bool painted = false;
    bool decoding = false;
    bool finished = false;
    bool failed = false;
    bool transformHeld = false;
    bool transformAdopted = false;
    Diagnostics metrics;
};

ScreenshotAnimationPlayback::ScreenshotAnimationPlayback(QObject* parent)
    : ScreenshotAnimationPlayback(Hooks{}, Options{}, parent) {}
ScreenshotAnimationPlayback::ScreenshotAnimationPlayback(Hooks hooks, QObject* parent)
    : ScreenshotAnimationPlayback(std::move(hooks), Options{}, parent) {}
ScreenshotAnimationPlayback::ScreenshotAnimationPlayback(Hooks hooks, Options options,
                                                         QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, std::move(hooks), std::move(options))) {
}
ScreenshotAnimationPlayback::~ScreenshotAnimationPlayback() {
    // The supplied scheduler can inspect playback while canceling its final deadline.
    m_impl->shutdown();
}

void ScreenshotAnimationPlayback::setSource(std::shared_ptr<const ScreenshotAnimationSource> source,
                                            bool playing, int speed, QImage initialTransformed) {
    m_impl->cancel();
    m_impl->source = std::move(source);
    m_impl->painted = false;
    m_impl->displayedOriginal = {};
    m_impl->pendingOriginal = {};
    m_impl->finished = false;
    m_impl->failed = false;
    m_impl->displayedIndex = 0;
    m_impl->paintedToken = 0;
    m_impl->position = 0;
    m_impl->displayedOccurrence = 0;
    m_impl->displayedTransform = m_impl->transform;
    m_impl->preparationLead = 0;
    m_impl->starts.clear();
    m_impl->total = 0;
    m_impl->speed = std::clamp(speed, 10, 400);
    m_impl->requested = m_impl->source && m_impl->source->isAnimated() && playing;
    if (!m_impl->source || !m_impl->source->isAnimated()) {
        m_impl->source.reset();
        m_impl->changed();
        return;
    }
    m_impl->starts.push_back(0);
    for (qint64 duration : m_impl->source->frameDurationsNs) {
        m_impl->total += static_cast<long double>(std::max<qint64>(1, duration));
        m_impl->starts.push_back(m_impl->total);
    }
    m_impl->anchor = m_impl->now();
    m_impl->lastAttemptTime = m_impl->anchor;
    if (initialTransformed.isNull() && m_impl->transform.isIdentity())
        initialTransformed = m_impl->source->firstDisplayFrame;
    if (!initialTransformed.isNull()) {
        QImage original = m_impl->source->firstFrame;
        if (m_impl->transform.isIdentity() &&
            initialTransformed.format() == QImage::Format_ARGB32_Premultiplied &&
            initialTransformed.size() == original.size() &&
            initialTransformed.devicePixelRatio() == 1.0 &&
            initialTransformed.colorSpace() == original.colorSpace() &&
            (!original.colorSpace().isValid() ||
             original.colorSpace() == QColorSpace(QColorSpace::SRgb)))
            original = initialTransformed;
        m_impl->publish(std::move(original), std::move(initialTransformed), 0);
    } else if (m_impl->transform.isIdentity() && !m_impl->source->firstFrame.isNull() &&
               m_impl->source->firstFrame.format() == QImage::Format_ARGB32_Premultiplied &&
               (!m_impl->source->firstFrame.colorSpace().isValid() ||
                m_impl->source->firstFrame.colorSpace() == QColorSpace(QColorSpace::SRgb)) &&
               m_impl->source->firstFrame.devicePixelRatio() == 1.0)
        m_impl->publish(m_impl->source->firstFrame, m_impl->source->firstFrame, 0);
    else
        m_impl->requestFrame(0, true);
    m_impl->changed();
}
bool ScreenshotAnimationPlayback::adoptCroppedSource(
    std::shared_ptr<const ScreenshotAnimationSource> source, QImage original, QImage transformed,
    const QTransform& transform) {
    if (!source || !m_impl->source || m_impl->transformHeld || original.isNull() ||
        transformed.isNull() || source->frameDurationsNs != m_impl->source->frameDurationsNs)
        return false;
    m_impl->settleDisplayed();
    m_impl->cancel();
    m_impl->source = std::move(source);
    m_impl->transform = transform;
    m_impl->displayedTransform = transform;
    m_impl->anchor = m_impl->now();
    m_impl->publish(std::move(original), std::move(transformed), m_impl->displayedIndex);
    m_impl->changed();
    return true;
}

void ScreenshotAnimationPlayback::clear() {
    m_impl->cancel();
    m_impl->source.reset();
    m_impl->pendingOriginal = {};
    m_impl->displayedOriginal = {};
    m_impl->requested = false;
    m_impl->painted = false;
    m_impl->changed();
}
void ScreenshotAnimationPlayback::setSuspended(bool suspended) {
    if (m_impl->isSuspended == suspended)
        return;
    if (suspended)
        m_impl->settleDisplayed();
    else
        m_impl->settle();
    m_impl->isSuspended = suspended;
    if (suspended) {
        m_impl->cancelDeadline();
        m_impl->cancelJob();
        if (!m_impl->transformAdopted)
            m_impl->discardPending();
        m_impl->retireWorker();
    } else {
        m_impl->schedule();
    }
    m_impl->changed();
}
void ScreenshotAnimationPlayback::setTransform(const QTransform& transform) {
    if (m_impl->transformHeld) {
        m_impl->clearTransformPreparation();
        m_impl->anchor = m_impl->now();
    }
    if (m_impl->transform == transform)
        return;
    m_impl->transform = transform;
    m_impl->cancelJob();
    m_impl->discardPending(false);
    if (m_impl->source)
        m_impl->requestFrame(m_impl->displayedIndex, true);
}
quint64 ScreenshotAnimationPlayback::prepareTransform(const QTransform& transform,
                                                      TransformCompletion completion) {
    if (!m_impl->source)
        return 0;
    m_impl->settleDisplayed();
    if (m_impl->transformCancellation)
        m_impl->transformCancellation->store(true);
    m_impl->preparedTransform.reset();
    m_impl->transformHeld = true;
    m_impl->transformAdopted = false;
    const quint64 preparation = ++m_impl->preparationSerial;
    const int index = m_impl->displayedIndex;
    QImage original =
        m_impl->displayedOriginal.isNull() ? m_impl->source->firstFrame : m_impl->displayedOriginal;
    m_impl->discardPending(false);
    // The host may close and clear playback if restoring the painted native
    // geometry fails while discarding an unpainted transform.
    if (!m_impl->source || preparation != m_impl->preparationSerial || !m_impl->transformHeld)
        return 0;
    m_impl->cancelDeadline();
    m_impl->cancelJob();
    ++m_impl->generation;
    m_impl->decoding = false;
    m_impl->retireWorker();
    const std::weak_ptr<Impl::Lifetime> weak = m_impl->lifetime;
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    m_impl->transformCancellation = cancelled;
    auto ready = [weak, preparation, index, transform, cancelled,
                  completion = std::move(completion)](QImage frame, QImage transformed,
                                                      QString error) {
        const auto alive = weak.lock();
        if (!alive || !alive->owner || cancelled->load() ||
            preparation != alive->owner->preparationSerial)
            return;
        auto& owner = *alive->owner;
        if (frame.isNull() || transformed.isNull() || !error.isEmpty()) {
            if (error.isEmpty())
                error = QCoreApplication::translate("SnowImageQtCodec",
                                                    "Could not decode animated image.");
            owner.releaseTransformHold();
            if (completion)
                completion({}, std::move(error));
            return;
        }
        owner.preparedTransform = PreparedTransform{std::move(frame), std::move(transformed),
                                                    transform, index, preparation};
        if (completion)
            completion(*owner.preparedTransform, {});
    };
    m_impl->changed();
    if (m_impl->options.prepare) {
        m_impl->options.prepare({std::move(original), transform, cancelled}, std::move(ready));
    } else {
        startImagePreparation(this, std::move(original), transform, std::move(ready), cancelled);
    }
    return preparation;
}
bool ScreenshotAnimationPlayback::adoptPreparedTransform(const PreparedTransform& prepared) {
    if (!m_impl->transformHeld || m_impl->transformAdopted || !m_impl->preparedTransform ||
        prepared.token != m_impl->preparationSerial ||
        prepared.transform != m_impl->preparedTransform->transform ||
        prepared.frameIndex != m_impl->displayedIndex ||
        prepared.original.cacheKey() != m_impl->preparedTransform->original.cacheKey() ||
        prepared.transformed.cacheKey() != m_impl->preparedTransform->transformed.cacheKey())
        return false;
    auto ready = std::move(*m_impl->preparedTransform);
    m_impl->preparedTransform.reset();
    m_impl->transform = ready.transform;
    m_impl->transformAdopted = true;
    m_impl->publish(std::move(ready.original), std::move(ready.transformed), ready.frameIndex);
    return true;
}
void ScreenshotAnimationPlayback::cancelTransformPreparation() {
    // An adopted transform already owns the host's geometry. Its staged raster must
    // survive visibility or pause changes until painting; source replacement clears it.
    if (m_impl->transformHeld && !m_impl->transformAdopted)
        m_impl->releaseTransformHold();
}
ScreenshotAnimationPlayback::PreparationCancellation ScreenshotAnimationPlayback::prepareImage(
    QObject* context, QImage original, const QTransform& transform, DecodeCompletion completion) {
    return startImagePreparation(context, std::move(original), transform, std::move(completion));
}
void ScreenshotAnimationPlayback::setRefreshRate(qreal rate) {
    const qreal valid = std::isfinite(rate) && rate > 1 ? rate : 60;
    const qint64 period = std::max<qint64>(1, qRound64(1'000'000'000.0 / valid));
    if (period == m_impl->refreshPeriod)
        return;
    m_impl->refreshPeriod = period;
    m_impl->schedule();
}
void ScreenshotAnimationPlayback::play() {
    if (!m_impl->source || m_impl->requested)
        return;
    m_impl->settle();
    m_impl->failed = false;
    m_impl->manualTarget.reset();
    m_impl->requested = true;
    if (m_impl->finished) {
        m_impl->finished = false;
        m_impl->position = 0;
        m_impl->painted = false;
        m_impl->requestFrame(0, true);
    } else {
        m_impl->schedule();
    }
    m_impl->changed();
}
void ScreenshotAnimationPlayback::pause() {
    if (!m_impl->requested)
        return;
    m_impl->settleDisplayed();
    m_impl->requested = false;
    m_impl->cancelDeadline();
    m_impl->cancelJob();
    if (!m_impl->transformAdopted)
        m_impl->discardPending();
    m_impl->retireWorker();
    m_impl->changed();
}
void ScreenshotAnimationPlayback::toggle() {
    if (requestedPlaying())
        pause();
    else
        play();
}
void ScreenshotAnimationPlayback::nextFrame() {
    m_impl->step(1);
}
void ScreenshotAnimationPlayback::previousFrame() {
    m_impl->step(-1);
}
void ScreenshotAnimationPlayback::decreaseSpeed() {
    m_impl->setSpeed(m_impl->speed - 10);
}
void ScreenshotAnimationPlayback::increaseSpeed() {
    m_impl->setSpeed(m_impl->speed + 10);
}
void ScreenshotAnimationPlayback::resetSpeed() {
    m_impl->setSpeed(100);
}
void ScreenshotAnimationPlayback::framePainted(quint64 token) {
    if (token == 0 || token != m_impl->pendingToken)
        return;
    const qint64 time = m_impl->now();
    const long double at = m_impl->positionAt(time);
    if (m_impl->frameAt(at) == m_impl->pendingIndex ||
        m_impl->displayedIndex != m_impl->pendingIndex) {
        const long double phase = std::fmod(std::max<long double>(0, at), m_impl->total);
        m_impl->displayedOccurrence =
            at - phase + m_impl->starts[static_cast<std::size_t>(m_impl->pendingIndex)];
    }
    m_impl->displayedIndex = m_impl->pendingIndex;
    if (m_impl->manualTarget == m_impl->displayedIndex)
        m_impl->manualTarget.reset();
    m_impl->displayedOriginal = std::move(m_impl->pendingOriginal);
    m_impl->displayedTransform = m_impl->transform;
    m_impl->paintedToken = token;
    m_impl->pendingToken = 0;
    m_impl->lastPaintTime = time;
    if (!m_impl->painted) {
        m_impl->painted = true;
        m_impl->anchor = m_impl->lastPaintTime;
    }
    ++m_impl->metrics.displayedFrames;
    if (m_impl->transformAdopted)
        m_impl->releaseTransformHold();
    else
        m_impl->schedule();
}
bool ScreenshotAnimationPlayback::requestedPlaying() const {
    return m_impl->requested;
}
bool ScreenshotAnimationPlayback::effectivePlaying() const {
    return m_impl->effective();
}
bool ScreenshotAnimationPlayback::suspended() const {
    return m_impl->isSuspended;
}
bool ScreenshotAnimationPlayback::busy() const {
    return m_impl->decoding ||
           (m_impl->transformHeld && !m_impl->transformAdopted && !m_impl->preparedTransform);
}
int ScreenshotAnimationPlayback::speedPercent() const {
    return m_impl->speed;
}
int ScreenshotAnimationPlayback::frameIndex() const {
    return m_impl->displayedIndex;
}
quint64 ScreenshotAnimationPlayback::currentFrameToken() const {
    return m_impl->pendingToken != 0 ? m_impl->pendingToken : m_impl->paintedToken;
}
ScreenshotAnimationPlayback::Diagnostics ScreenshotAnimationPlayback::diagnostics() const {
    auto result = m_impl->metrics;
    if (!m_impl->options.scheduleDeadline) {
        result.coordinatorWakeups = animationDeadlines().wakeups();
        result.activeDeadlines = animationDeadlines().activeDeadlines();
    }
    result.preparationLeadNs = m_impl->preparationLead;
    result.cachedBytes = frameCache().retainedBytes();
    result.reservedWorkingBytes = memoryBudget().reservedBytes();
    result.cacheHits = m_impl->workerMetrics->cacheHits.load();
    result.cacheMisses = m_impl->workerMetrics->cacheMisses.load();
    result.decoderResets = m_impl->workerMetrics->decoderResets.load();
    result.compositedFrames = m_impl->workerMetrics->compositedFrames.load();
    return result;
}
