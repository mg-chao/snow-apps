#include "snow_shot/presentation/livewindowsource.h"
#include "livewindownative_p.h"
#include "snow_capture.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QEvent>
#include <QMetaObject>
#include <QPainter>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace snow_shot::presentation {
namespace {
QString liveText(const char* text) {
    return QCoreApplication::translate("LiveWindowSource", text);
}
QString messageFor(LiveWindowState state, const QString& error = {}) {
    if (state == LiveWindowState::Minimized)
        return liveText(
            QT_TRANSLATE_NOOP("LiveWindowSource",
                              "The window is minimized. The latest content cannot be displayed."));
    if (state == LiveWindowState::Closed)
        return liveText(QT_TRANSLATE_NOOP(
            "LiveWindowSource", "The window is closed. The latest content cannot be displayed."));
    if (state == LiveWindowState::Unavailable)
        return error.isEmpty() ? liveText(QT_TRANSLATE_NOOP(
                                     "LiveWindowSource",
                                     "The window content is temporarily unavailable. Retrying…"))
                               : error;
    return {};
}

struct CapturedWindowFrame {
    QImage image;
    QRectF desktopBounds;
    quint64 requestGeneration = 0;
    bool duplicate = false;
    QRect initialDesktopRegion;
    QRectF initialBounds;
    QRectF crop;
};

CapturedWindowFrame readFrame(SnowCaptureStreamFrame* frame) {
    SnowCaptureStreamFrameInfo info{};
    SnowCaptureWindowGeometry geometry{};
    if (!frame || !snow_capture_stream_frame_info(frame, &info) || !info.rgba_bytes ||
        !info.width || !info.height ||
        info.width > static_cast<uint32_t>(std::numeric_limits<int>::max() / 4) ||
        info.height > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        info.stride_bytes < info.width * 4 ||
        info.rgba_len < static_cast<size_t>(info.stride_bytes) * info.height) {
        if (frame)
            snow_capture_stream_frame_release(frame);
        return {};
    }
    QRectF bounds(info.x, info.y, info.width, info.height);
    if (snow_capture_stream_frame_window_geometry(frame, &geometry) && geometry.width > 0 &&
        geometry.height > 0)
        bounds = QRectF(geometry.x, geometry.y, geometry.width, geometry.height);
    QImage image(
        info.rgba_bytes, static_cast<int>(info.width), static_cast<int>(info.height),
        static_cast<int>(info.stride_bytes), QImage::Format_RGBA8888,
        [](void* data) {
            snow_capture_stream_frame_release(static_cast<SnowCaptureStreamFrame*>(data));
        },
        frame);
    image.setColorSpace(QColorSpace::SRgb);
    CapturedWindowFrame result;
    result.image = std::move(image);
    result.desktopBounds = bounds;
    result.duplicate = info.is_duplicate != 0;
    return result;
}

class SharedWindowCapture final {
  public:
    explicit SharedWindowCapture(const LiveWindowTarget& target)
        : m_target(target), m_worker([this] { run(); }) {}
    ~SharedWindowCapture() {
        m_stopping = true;
        m_changed.notify_all();
        if (m_worker.joinable())
            m_worker.join();
    }
    quint64 sequence() const {
        std::lock_guard lock(m_mutex);
        return m_sequence;
    }
    bool failed() const {
        std::lock_guard lock(m_mutex);
        return !m_error.isEmpty();
    }
    CapturedWindowFrame receive(quint64* after, QString* error) {
        std::unique_lock lock(m_mutex);
        m_changed.wait_for(lock, std::chrono::milliseconds(50),
                           [&] { return m_sequence > *after || !m_error.isEmpty(); });
        if (!m_error.isEmpty()) {
            *error = m_error;
            return {};
        }
        if (m_sequence <= *after)
            return {};
        *after = m_sequence;
        return m_latest;
    }

  private:
    void fail() {
        {
            std::lock_guard lock(m_mutex);
            m_error = liveText(QT_TRANSLATE_NOOP(
                "LiveWindowSource", "The window content is temporarily unavailable. Retrying…"));
        }
        m_changed.notify_all();
    }
    void run() {
        SnowCaptureWindowStreamConfig config{};
        config.version = SNOW_CAPTURE_WINDOW_STREAM_CONFIG_VERSION;
        config.struct_size = sizeof(config);
        config.window_id = static_cast<intptr_t>(m_target.nativeId);
        config.target_fps = 30;
        config.buffer_depth = 1;
        config.max_consecutive_errors = 3;
        config.capture_retry_count = 1;
        config.pixel_format = SNOW_CAPTURE_PIXEL_FORMAT_RGBA8;
        auto* stream = snow_capture_stream_create_window(&config);
        if (!stream) {
            fail();
            return;
        }
        while (!m_stopping) {
            SnowCaptureStreamEvent event{};
            if (!snow_capture_stream_receive(stream, 50, &event) ||
                event.kind == SNOW_CAPTURE_STREAM_EVENT_ERROR ||
                event.kind == SNOW_CAPTURE_STREAM_EVENT_ENDED) {
                if (event.frame)
                    snow_capture_stream_frame_release(event.frame);
                fail();
                break;
            }
            if (event.kind != SNOW_CAPTURE_STREAM_EVENT_FRAME)
                continue;
            auto frame = readFrame(event.frame);
            if (frame.image.isNull()) {
                fail();
                break;
            }
            {
                std::lock_guard lock(m_mutex);
                m_latest = std::move(frame);
                ++m_sequence;
            }
            m_changed.notify_all();
        }
        snow_capture_stream_destroy(stream);
    }
    LiveWindowTarget m_target;
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    CapturedWindowFrame m_latest;
    quint64 m_sequence = 0;
    QString m_error;
    std::atomic<bool> m_stopping{false};
    std::thread m_worker;
};

std::shared_ptr<SharedWindowCapture> sharedCapture(const LiveWindowTarget& target) {
    static std::mutex mutex;
    static std::map<std::pair<quintptr, qint64>, std::weak_ptr<SharedWindowCapture>> captures;
    std::lock_guard lock(mutex);
    const auto key = std::make_pair(target.nativeId, target.processId);
    auto capture = captures[key].lock();
    if (!capture || capture->failed()) {
        capture = std::make_shared<SharedWindowCapture>(target);
        captures[key] = capture;
    }
    for (auto iterator = captures.begin(); iterator != captures.end();) {
        if (iterator->second.expired())
            iterator = captures.erase(iterator);
        else
            ++iterator;
    }
    return capture;
}

QImage croppedFrame(const CapturedWindowFrame& frame, const QRectF& crop, const QSize& pixels) {
    if (frame.image.isNull() || frame.desktopBounds.isEmpty() || crop.isEmpty())
        return {};
    QImage result(pixels, QImage::Format_RGBA8888);
    result.fill(Qt::transparent);
    result.setColorSpace(frame.image.colorSpace());
    const auto mapping = live_window_detail::cropMapping(
        crop, QRectF(QPointF(), frame.desktopBounds.size()), QRectF(QPointF(), pixels));
    if (mapping.source.isEmpty())
        return result;
    const qreal scaleX = frame.image.width() / frame.desktopBounds.width();
    const qreal scaleY = frame.image.height() / frame.desktopBounds.height();
    const QRectF source(mapping.source.x() * scaleX, mapping.source.y() * scaleY,
                        mapping.source.width() * scaleX, mapping.source.height() * scaleY);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(mapping.destination, frame.image, source);
    return result;
}

class CapturedLiveWindowSource final
    : public LiveWindowSource,
      public std::enable_shared_from_this<CapturedLiveWindowSource> {
  public:
    CapturedLiveWindowSource(LiveWindowTarget target, QRect selection)
        : m_target(std::move(target)), m_selection(selection),
          m_native(live_window_detail::createNativeWindow(m_target)) {
        if (m_native)
            m_native->changed = [this] { inspectSource(); };
        m_inspectTimer.setInterval(200);
        connect(&m_inspectTimer, &QTimer::timeout, this, [this] { inspectSource(); });
    }
    ~CapturedLiveWindowSource() override {
        shutdown();
    }
    void initialize(InitializationCompletion completion) override {
        if (m_initialized || m_initializing || m_stopped) {
            const bool initialized = m_initialized && !m_stopped;
            completion(initialized,
                       initialized ? QString()
                                   : liveText(QT_TRANSLATE_NOOP(
                                         "LiveWindowSource",
                                         "The live window display could not be initialized.")));
            return;
        }
        QString error;
        if (!m_native || !m_native->initialize(&error)) {
            completion(false, error);
            return;
        }
        m_initializing = true;
        m_initializationCompletion = std::move(completion);
        m_worker = std::thread([this] { captureLoop(); });
        m_inspectTimer.start();
        requestCapture();
    }
    QImage initialFrame() const override {
        return m_initialFrame;
    }
    QRect initialDesktopRegion() const override {
        return m_initialDesktopRegion;
    }
    LiveWindowState state() const override {
        return m_state;
    }
    QString stateMessage() const override {
        return m_message;
    }
    void requestFrame(FrameCompletion completion) override {
        if (!m_initialized || m_state != LiveWindowState::Live || m_stopped) {
            completion({}, m_message.isEmpty()
                               ? liveText(QT_TRANSLATE_NOOP(
                                     "LiveWindowSource",
                                     "The window content is temporarily unavailable. Retrying…"))
                               : m_message);
            return;
        }
        m_pendingFrames.push_back({requestCapture(), std::move(completion)});
    }
    bool setPresentation(const LiveWindowPresentation& presentation, QString* error) override {
        const auto keepAlive = shared_from_this();
        if (m_stopped || !m_initialized)
            return false;
        m_presentation = presentation;
        const bool before = usesNativePresentation();
#ifdef Q_OS_WIN
        const bool native =
            presentation.imageTransform.isIdentity() && m_state == LiveWindowState::Live;
        if (native) {
            m_frameHandoffPending = false;
            if (!m_native->present(presentation, m_crop, m_initialBounds, error)) {
                setState(LiveWindowState::Unavailable, error ? *error : QString());
                if (m_stopped)
                    return false;
                scheduleRetry();
                setContinuous(false);
                if (before != usesNativePresentation())
                    emit nativePresentationChanged(usesNativePresentation());
                return false;
            }
            if (m_stopped)
                return false;
            m_retryAttempt = 0;
        } else {
            if (before && presentation.visible && m_state == LiveWindowState::Live) {
                if (!m_frameHandoffPending) {
                    m_frameHandoffPending = true;
                    requestCapture();
                }
            } else {
                m_native->stopPresentation();
            }
        }
        if (m_stopped)
            return false;
        setContinuous(!native && presentation.visible && m_state == LiveWindowState::Live);
#else
        Q_UNUSED(error);
        setContinuous(presentation.visible && m_state == LiveWindowState::Live);
#endif
        if (before != usesNativePresentation())
            emit nativePresentationChanged(usesNativePresentation());
        return !m_stopped;
    }
    bool usesNativePresentation() const override {
        return m_native && m_native->presenting();
    }
    void shutdown() override {
        const auto keepAlive = weak_from_this().lock();
        if (m_stopped.exchange(true))
            return;
        m_inspectTimer.stop();
        {
            std::lock_guard lock(m_workMutex);
            m_continuous = false;
            m_captureRequested = false;
        }
        m_workChanged.notify_all();
        if (m_worker.joinable())
            m_worker.join();
        if (m_native)
            m_native->stopPresentation();
        m_native.reset();
        m_initialFrame = {};
        {
            std::lock_guard lock(m_frameMutex);
            m_queuedFrame = {};
        }
        const auto pending = std::exchange(m_pendingFrames, {});
        for (const auto& pendingFrame : pending)
            pendingFrame.second(
                {}, liveText(QT_TRANSLATE_NOOP("LiveWindowSource", "The live window was closed.")));
        if (m_initializationCompletion) {
            const auto completion = std::exchange(m_initializationCompletion, {});
            completion(false, liveText(QT_TRANSLATE_NOOP("LiveWindowSource",
                                                         "The live window was closed.")));
        }
        emit stopped();
    }

  private:
    bool event(QEvent* event) override {
        const auto keepAlive = weak_from_this().lock();
        if (event && event->type() == QEvent::LanguageChange && !m_stopped) {
            QString error;
            if (m_state == LiveWindowState::Unavailable && m_native) {
                const auto current = m_native->inspect();
                if (current.state == LiveWindowState::Unavailable)
                    error = current.error;
            }
            setState(m_state, error);
        }
        return LiveWindowSource::event(event);
    }
    void setState(LiveWindowState state, const QString& error = {}) {
        const QString message = messageFor(state, error);
        if (m_state == state && m_message == message)
            return;
        m_state = state;
        m_message = message;
        emit stateChanged(state, message);
    }
    void inspectSource() {
        const auto keepAlive = shared_from_this();
        if (m_stopped || !m_native || m_state == LiveWindowState::Closed)
            return;
        const auto current = m_native->inspect();
        if (current.state != LiveWindowState::Live) {
            m_retryPending = false;
            const bool native = usesNativePresentation();
            m_native->stopPresentation();
            m_frameHandoffPending = false;
            setContinuous(false);
            {
                std::lock_guard lock(m_workMutex);
                m_captureRequested = false;
            }
            setState(current.state, current.error);
            if (m_stopped)
                return;
            if (current.state == LiveWindowState::Minimized)
                m_retryAt = std::chrono::steady_clock::now();
            const auto pending = std::exchange(m_pendingFrames, {});
            for (const auto& frame : pending)
                frame.second({}, m_message);
            if (m_initializationCompletion) {
                m_initializing = false;
                const auto completion = std::exchange(m_initializationCompletion, {});
                completion(false, m_message);
            }
            if (m_stopped)
                return;
            if (native)
                emit nativePresentationChanged(false);
            return;
        }
        if (!m_initialized)
            return;
        if (m_state != LiveWindowState::Live) {
            // A state observation is not a frame. Keep image actions disabled
            // until a fresh capture confirms that content is available again.
            if (!m_retryPending && std::chrono::steady_clock::now() >= m_retryAt) {
                m_retryPending = true;
                requestCapture();
            }
            return;
        }
#ifdef Q_OS_WIN
        if (m_presentation.owner && m_presentation.imageTransform.isIdentity()) {
            QString error;
            if (!m_native->present(m_presentation, m_crop, m_initialBounds, &error)) {
                setState(LiveWindowState::Unavailable, error);
                if (m_stopped)
                    return;
                scheduleRetry();
                emit nativePresentationChanged(false);
            }
        }
#endif
    }
    quint64 requestCapture() {
        quint64 generation = 0;
        {
            std::lock_guard lock(m_workMutex);
            m_captureRequested = true;
            generation = ++m_requestGeneration;
        }
        m_workChanged.notify_all();
        return generation;
    }
    void setContinuous(bool continuous) {
        {
            std::lock_guard lock(m_workMutex);
            m_continuous = continuous;
        }
        m_workChanged.notify_all();
    }
    void acceptFrame(CapturedWindowFrame frame) {
        const auto keepAlive = shared_from_this();
        if (m_stopped || m_state == LiveWindowState::Closed)
            return;
        m_retryPending = false;
        if (!m_initialized || m_state != LiveWindowState::Live) {
            const auto nativeState = m_native->inspect();
            if (nativeState.state != LiveWindowState::Live) {
                setState(nativeState.state, nativeState.error);
                if (m_stopped)
                    return;
                const auto pending = std::exchange(m_pendingFrames, {});
                for (const auto& request : pending)
                    request.second({}, m_message);
                return;
            }
        }
        if (!m_initialized) {
            m_initialBounds = frame.initialBounds;
            m_initialDesktopRegion = frame.initialDesktopRegion;
            m_crop = frame.crop;
            m_initialFrame = frame.image;
            if (m_initialFrame.isNull()) {
                failCapture(liveText(QT_TRANSLATE_NOOP(
                    "LiveWindowSource", "The live window display could not be initialized.")));
                return;
            }
            m_initialized = true;
            m_initializing = false;
            setState(LiveWindowState::Live);
            if (m_stopped)
                return;
            if (m_initializationCompletion) {
                const auto completion = std::exchange(m_initializationCompletion, {});
                completion(true, {});
            }
            if (m_stopped)
                return;
        }
        QImage image = std::move(frame.image);
        if (image.isNull()) {
            failCapture(liveText(QT_TRANSLATE_NOOP(
                "LiveWindowSource", "The window content is temporarily unavailable. Retrying…")));
            return;
        }
        const bool resumed = m_state != LiveWindowState::Live;
        setState(LiveWindowState::Live);
        if (m_stopped)
            return;
        emit frameChanged(image);
        if (m_stopped)
            return;
        if (m_frameHandoffPending && !m_presentation.imageTransform.isIdentity()) {
            m_frameHandoffPending = false;
            m_native->stopPresentation();
            emit nativePresentationChanged(false);
        }
        if (m_stopped)
            return;
        const auto pending = std::exchange(m_pendingFrames, {});
        for (const auto& request : pending) {
            if (m_stopped)
                request.second({}, liveText(QT_TRANSLATE_NOOP("LiveWindowSource",
                                                              "The live window was closed.")));
            else if (request.first <= frame.requestGeneration)
                request.second(image, {});
            else
                m_pendingFrames.push_back(request);
        }
        if (m_stopped)
            return;
        if (resumed && m_presentation.owner) {
            QString error;
            if (setPresentation(m_presentation, &error))
                m_retryAttempt = 0;
        } else
            m_retryAttempt = 0;
    }
    void failCapture(const QString& error) {
        const auto keepAlive = shared_from_this();
        if (m_stopped)
            return;
        m_retryPending = false;
        scheduleRetry();
        setContinuous(false);
        const bool native = usesNativePresentation();
        m_native->stopPresentation();
        m_frameHandoffPending = false;
        if (native)
            emit nativePresentationChanged(false);
        if (m_stopped)
            return;
        const auto state = m_native->inspect();
        setState(state.state == LiveWindowState::Live ? LiveWindowState::Unavailable : state.state,
                 error);
        if (m_stopped)
            return;
        if (m_initializationCompletion) {
            m_initializing = false;
            const auto completion = std::exchange(m_initializationCompletion, {});
            completion(false, error);
        }
        if (m_stopped)
            return;
        const auto pending = std::exchange(m_pendingFrames, {});
        for (const auto& request : pending)
            request.second({}, m_message);
    }
    void scheduleRetry() {
        constexpr int delays[] = {500, 1000, 2000, 5000};
        m_retryAt = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(delays[qMin(m_retryAttempt, 3)]);
        m_retryAttempt = qMin(m_retryAttempt + 1, 3);
    }
    void captureLoop() {
        std::shared_ptr<SharedWindowCapture> capture;
        quint64 sequence = 0;
        quint64 lastRequest = 0;
        QRectF initialBounds;
        QRect initialRegion;
        QRectF crop;
        QSize cropPixels;
        auto requestDeadline = std::chrono::steady_clock::time_point::max();
        while (!m_stopped) {
            bool requested = false;
            quint64 generation = 0;
            {
                std::unique_lock lock(m_workMutex);
                if (!m_continuous && !m_captureRequested) {
                    lock.unlock();
                    capture.reset();
                    lock.lock();
                    m_workChanged.wait_for(lock, std::chrono::milliseconds(100), [this] {
                        return m_stopped || m_continuous || m_captureRequested;
                    });
                }
                if (m_stopped)
                    break;
                if (!m_continuous && !m_captureRequested)
                    continue;
                requested = m_captureRequested;
                generation = m_requestGeneration;
            }
            if (!capture) {
                capture = sharedCapture(m_target);
                sequence = 0;
            }
            if (requested && lastRequest != generation) {
                lastRequest = generation;
                // Even an unchanged source produces a new stream observation.
                // A fresh action waits beyond the broker's current observation.
                sequence = capture->sequence();
                requestDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            }
            QString error;
            auto frame = capture->receive(&sequence, &error);
            if (!error.isEmpty() ||
                (requested && std::chrono::steady_clock::now() >= requestDeadline)) {
                capture.reset();
                {
                    std::lock_guard lock(m_workMutex);
                    m_captureRequested = false;
                    m_continuous = false;
                }
                if (error.isEmpty())
                    error = liveText(QT_TRANSLATE_NOOP(
                        "LiveWindowSource",
                        "The window content is temporarily unavailable. Retrying…"));
                QMetaObject::invokeMethod(
                    this, [this, error] { failCapture(error); }, Qt::QueuedConnection);
                continue;
            }
            if (frame.image.isNull() || (frame.duplicate && !requested && !crop.isEmpty()))
                continue;
            if (crop.isEmpty()) {
                initialBounds = frame.desktopBounds;
                const QRectF intersection = QRectF(m_selection).intersected(initialBounds);
                if (intersection.isEmpty()) {
                    {
                        std::lock_guard lock(m_workMutex);
                        m_captureRequested = false;
                        m_continuous = false;
                    }
                    capture.reset();
                    QMetaObject::invokeMethod(
                        this,
                        [this] {
                            failCapture(liveText(QT_TRANSLATE_NOOP(
                                "LiveWindowSource",
                                "The selection does not intersect the selected window.")));
                        },
                        Qt::QueuedConnection);
                    continue;
                }
                initialRegion = intersection.toAlignedRect();
                crop = intersection.translated(-initialBounds.topLeft());
                cropPixels = QSize(
                    qMax(1, qRound(crop.width() * frame.image.width() / initialBounds.width())),
                    qMax(1, qRound(crop.height() * frame.image.height() / initialBounds.height())));
            }
            frame.image = croppedFrame(frame, crop, cropPixels);
            frame.initialBounds = initialBounds;
            frame.initialDesktopRegion = initialRegion;
            frame.crop = crop;
            frame.requestGeneration = generation;
            {
                std::lock_guard lock(m_workMutex);
                if (generation == m_requestGeneration)
                    m_captureRequested = false;
            }
            bool queueDelivery = false;
            {
                std::lock_guard lock(m_frameMutex);
                m_queuedFrame = std::move(frame);
                queueDelivery = !m_deliveryQueued.exchange(true);
            }
            if (queueDelivery) {
                QMetaObject::invokeMethod(
                    this,
                    [this] {
                        CapturedWindowFrame frame;
                        {
                            std::lock_guard lock(m_frameMutex);
                            frame = std::move(m_queuedFrame);
                            m_deliveryQueued = false;
                        }
                        acceptFrame(std::move(frame));
                    },
                    Qt::QueuedConnection);
            }
        }
    }
    LiveWindowTarget m_target;
    QRect m_selection;
    std::unique_ptr<live_window_detail::NativeWindow> m_native;
    QTimer m_inspectTimer;
    InitializationCompletion m_initializationCompletion;
    QVector<std::pair<quint64, FrameCompletion>> m_pendingFrames;
    QImage m_initialFrame;
    QRect m_initialDesktopRegion;
    QRectF m_initialBounds;
    QRectF m_crop;
    LiveWindowPresentation m_presentation;
    LiveWindowState m_state = LiveWindowState::Initializing;
    QString m_message;
    bool m_initializing = false;
    bool m_initialized = false;
    bool m_retryPending = false;
    bool m_frameHandoffPending = false;
    int m_retryAttempt = 0;
    std::chrono::steady_clock::time_point m_retryAt = std::chrono::steady_clock::now();
    std::atomic<bool> m_stopped{false};
    std::thread m_worker;
    std::mutex m_workMutex;
    std::condition_variable m_workChanged;
    bool m_continuous = false;
    bool m_captureRequested = false;
    quint64 m_requestGeneration = 0;
    std::mutex m_frameMutex;
    CapturedWindowFrame m_queuedFrame;
    std::atomic<bool> m_deliveryQueued{false};
};
} // namespace

std::shared_ptr<LiveWindowSource> createLiveWindowSource(const LiveWindowTarget& target,
                                                         const QRect& selection) {
    if (!target.nativeId || target.processId <= 0 || selection.isEmpty())
        return {};
    return std::make_shared<CapturedLiveWindowSource>(target, selection);
}
} // namespace snow_shot::presentation
