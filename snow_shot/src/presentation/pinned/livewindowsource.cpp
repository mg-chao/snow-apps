#include "snow_shot/presentation/livewindowsource.h"
#include "livewindownative_p.h"
#include "livewindowframecropper_p.h"
#include "snow_capture.h"
#include "snow_shot/runtime/runtimeactivitytracker.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QEvent>
#include <QMetaObject>
#include <QTimer>
#include <QWidget>
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
    std::shared_ptr<const live_window_detail::NativeWindowFrame> native;
    QRectF desktopBounds;
    quint64 requestGeneration = 0;
    bool duplicate = false;
    QRect initialDesktopRegion;
    QRectF crop;
    QSize pixelSize() const {
        return native ? native->size() : image.size();
    }
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
        : m_target(target), m_native(live_window_detail::createNativeWindowCapture(target)),
          m_worker([this] { run(); }) {}
    ~SharedWindowCapture() {
        m_stopping = true;
        m_changed.notify_all();
        if (m_native)
            m_native->cancel();
        {
            std::lock_guard lock(m_streamMutex);
            if (m_stream)
                snow_capture_stream_stop(m_stream);
        }
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
        if (m_native) {
            while (!m_stopping) {
                live_window_detail::NativeCaptureFrame native;
                if (!m_native->receive(&native)) {
                    if (!m_stopping)
                        fail();
                    break;
                }
                if (!native.pixels)
                    continue;
                CapturedWindowFrame frame;
                frame.native = std::move(native.pixels);
                frame.desktopBounds = native.desktopBounds;
                frame.duplicate = native.duplicate;
                {
                    std::lock_guard lock(m_mutex);
                    m_latest = std::move(frame);
                    ++m_sequence;
                }
                m_changed.notify_all();
            }
            return;
        }
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
        {
            std::lock_guard lock(m_streamMutex);
            m_stream = stream;
            if (m_stopping)
                snow_capture_stream_stop(stream);
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
        {
            std::lock_guard lock(m_streamMutex);
            m_stream = nullptr;
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
    std::mutex m_streamMutex;
    SnowCaptureStream* m_stream = nullptr;
    std::unique_ptr<live_window_detail::NativeWindowCapture> m_native;
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

class SharedWindowMonitor final : public QObject,
                                  public std::enable_shared_from_this<SharedWindowMonitor> {
    Q_OBJECT
  public:
    explicit SharedWindowMonitor(const LiveWindowTarget& target)
        : m_native(live_window_detail::createNativeWindowMonitor(target)) {}
    ~SharedWindowMonitor() override {
        if (m_native)
            m_native->changed = {};
    }
    bool initialize(QString* error) {
        if (!m_native || !m_native->initialize(error))
            return false;
        m_state = m_native->inspect();
        m_native->changed = [this] { refresh(); };
        m_timer.setInterval(m_native->hasLifecycleNotifications() ? 2000 : 200);
        connect(&m_timer, &QTimer::timeout, this, &SharedWindowMonitor::refresh);
        m_timer.start();
        return true;
    }
    const live_window_detail::NativeState& state() const {
        return m_state;
    }
    void refresh() {
        const auto keepAlive = shared_from_this();
        if (m_state.state == LiveWindowState::Closed)
            return;
        auto state = m_native->inspect();
        if (state.state == m_state.state && state.desktopBounds == m_state.desktopBounds &&
            state.error == m_state.error)
            return;
        m_state = std::move(state);
        if (m_state.state == LiveWindowState::Closed)
            m_timer.stop();
        emit changed();
    }

  signals:
    void changed();

  private:
    std::unique_ptr<live_window_detail::NativeWindowMonitor> m_native;
    live_window_detail::NativeState m_state;
    QTimer m_timer;
};

std::shared_ptr<SharedWindowMonitor> sharedMonitor(const LiveWindowTarget& target, QString* error) {
    // Sources and native observers belong to the GUI thread. Only capture pixels
    // cross into the worker; monitoring is shared independently of presentation.
    static std::map<std::pair<quintptr, qint64>, std::weak_ptr<SharedWindowMonitor>> monitors;
    const auto key = std::make_pair(target.nativeId, target.processId);
    auto monitor = monitors[key].lock();
    if (!monitor || monitor->state().state == LiveWindowState::Closed) {
        monitor = std::make_shared<SharedWindowMonitor>(target);
        if (!monitor->initialize(error))
            return {};
        monitors[key] = monitor;
    }
    for (auto iterator = monitors.begin(); iterator != monitors.end();) {
        if (iterator->second.expired())
            iterator = monitors.erase(iterator);
        else
            ++iterator;
    }
    return monitor;
}

// The worker owns no QObject. Closing a pin invalidates its dispatch target on
// the GUI thread; in-flight native capture and stream destruction can then finish
// without retaining or joining the shell.
class LiveCaptureWorker final : public std::enable_shared_from_this<LiveCaptureWorker> {
  public:
    LiveCaptureWorker(LiveWindowTarget target, QRect selection, QObject* receiver,
                      std::function<void(CapturedWindowFrame)> deliver,
                      std::function<void(const QString&)> fail)
        : m_target(std::move(target)), m_selection(selection), m_receiver(receiver),
          m_deliver(std::move(deliver)), m_fail(std::move(fail)) {}
    void start() {
        std::thread([self = shared_from_this()] { self->captureLoop(); }).detach();
    }
    void stop() {
        {
            // Serialize invokeMethod with invalidation before QObject destruction.
            std::lock_guard lock(m_dispatchMutex);
            m_receiver = nullptr;
            m_deliver = {};
            m_fail = {};
        }
        {
            std::lock_guard lock(m_workMutex);
            m_retirementActivity = snow_shot::runtime::RuntimeActivityTracker::shared().acquire();
            m_stopped = true;
            m_continuous = false;
            m_captureRequested = false;
        }
        {
            std::lock_guard lock(m_frameMutex);
            m_queuedFrame = {};
        }
        m_workChanged.notify_all();
    }
    quint64 requestCapture() {
        std::lock_guard lock(m_workMutex);
        m_captureRequested = true;
        m_workChanged.notify_all();
        return ++m_requestGeneration;
    }
    void setContinuous(bool value, bool materialize) {
        std::lock_guard lock(m_workMutex);
        m_continuous = value;
        m_materialize = materialize;
        m_workChanged.notify_all();
    }
    void cancelRequest() {
        std::lock_guard lock(m_workMutex);
        m_captureRequested = false;
    }

  private:
    void postFailure(const QString& error) {
        std::lock_guard lock(m_dispatchMutex);
        if (m_receiver) {
            QMetaObject::invokeMethod(
                m_receiver,
                [self = shared_from_this(), fail = m_fail, error] {
                    if (!self->m_stopped)
                        fail(error);
                },
                Qt::QueuedConnection);
        }
    }
    void captureLoop() {
        std::shared_ptr<SharedWindowCapture> capture;
        live_window_detail::FrameCropper cropper;
        quint64 sequence = 0;
        quint64 lastRequest = 0;
        QRectF initialBounds;
        QRect initialRegion;
        QRectF crop;
        QSize cropPixels;
        auto requestDeadline = std::chrono::steady_clock::time_point::max();
        while (!m_stopped) {
            bool requested = false;
            bool materialize = true;
            quint64 generation = 0;
            {
                std::unique_lock lock(m_workMutex);
                if (!m_continuous && !m_captureRequested) {
                    lock.unlock();
                    capture.reset();
                    cropper.clear();
                    lock.lock();
                    m_workChanged.wait(
                        lock, [this] { return m_stopped || m_continuous || m_captureRequested; });
                }
                if (m_stopped)
                    break;
                if (!m_continuous && !m_captureRequested)
                    continue;
                // Coalesce at the shared raw-frame broker before allocating or
                // cropping another image while the GUI still has a delivery queued.
                m_workChanged.wait(lock, [this] { return m_stopped || !m_deliveryQueued; });
                if (m_stopped)
                    break;
                if (!m_continuous && !m_captureRequested)
                    continue;
                requested = m_captureRequested;
                materialize = m_materialize || requested;
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
                postFailure(error);
                continue;
            }
            if (frame.pixelSize().isEmpty() || (frame.duplicate && !requested && !crop.isEmpty()))
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
                    postFailure(liveText(QT_TRANSLATE_NOOP(
                        "LiveWindowSource",
                        "The selection does not intersect the selected window.")));
                    continue;
                }
                initialRegion = intersection.toAlignedRect();
                crop = intersection.translated(-initialBounds.topLeft());
                cropPixels = QSize(qMax(1, qRound(crop.width() * frame.pixelSize().width() /
                                                  initialBounds.width())),
                                   qMax(1, qRound(crop.height() * frame.pixelSize().height() /
                                                  initialBounds.height())));
            }
            if (materialize || !frame.native) {
                if (frame.native)
                    frame.image = frame.native->image();
                frame.image = cropper.crop(frame.image, frame.desktopBounds, crop, cropPixels);
                if (frame.image.isNull()) {
                    {
                        std::lock_guard lock(m_workMutex);
                        m_captureRequested = false;
                        m_continuous = false;
                    }
                    capture.reset();
                    postFailure(liveText(QT_TRANSLATE_NOOP(
                        "LiveWindowSource",
                        "The window content is temporarily unavailable. Retrying…")));
                    continue;
                }
            } else {
                cropper.clear();
            }
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
                const auto self = shared_from_this();
                std::lock_guard dispatchLock(m_dispatchMutex);
                if (m_receiver) {
                    QMetaObject::invokeMethod(
                        m_receiver,
                        [self, deliver = m_deliver] {
                            CapturedWindowFrame frame;
                            {
                                std::lock_guard lock(self->m_frameMutex);
                                frame = std::move(self->m_queuedFrame);
                            }
                            if (!self->m_stopped)
                                deliver(std::move(frame));
                            {
                                std::lock_guard lock(self->m_workMutex);
                                self->m_deliveryQueued = false;
                            }
                            self->m_workChanged.notify_all();
                        },
                        Qt::QueuedConnection);
                }
            }
        }
    }
    LiveWindowTarget m_target;
    QRect m_selection;
    std::mutex m_dispatchMutex;
    QObject* m_receiver;
    snow_shot::runtime::RuntimeActivityLease m_retirementActivity;
    std::function<void(CapturedWindowFrame)> m_deliver;
    std::function<void(const QString&)> m_fail;
    std::atomic<bool> m_stopped{false};
    std::mutex m_workMutex;
    std::condition_variable m_workChanged;
    bool m_continuous = false;
    bool m_materialize = true;
    bool m_captureRequested = false;
    quint64 m_requestGeneration = 0;
    std::mutex m_frameMutex;
    CapturedWindowFrame m_queuedFrame;
    std::atomic<bool> m_deliveryQueued{false};
};

class CapturedLiveWindowSource final
    : public LiveWindowSource,
      public std::enable_shared_from_this<CapturedLiveWindowSource> {
  public:
    CapturedLiveWindowSource(LiveWindowTarget target, QRect selection)
        : m_target(std::move(target)), m_selection(selection),
          m_native(live_window_detail::createNativeWindowPresentation(m_target)) {
        m_retryTimer.setSingleShot(true);
        m_retryTimer.setTimerType(Qt::PreciseTimer);
        connect(&m_retryTimer, &QTimer::timeout, this, [this] { inspectSource(); });
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
        if (m_worker)
            m_worker->stop();
        m_worker.reset();
        if (m_monitor)
            disconnect(m_monitor.get(), nullptr, this, nullptr);
        QString error;
        m_monitor = sharedMonitor(m_target, &error);
        if (!m_monitor) {
            completion(false, error);
            return;
        }
        m_initializing = true;
        connect(m_monitor.get(), &SharedWindowMonitor::changed, this, [this] { inspectSource(); });
        m_initializationCompletion = std::move(completion);
        m_worker = std::make_shared<LiveCaptureWorker>(
            m_target, m_selection, this,
            [this](CapturedWindowFrame frame) { acceptFrame(std::move(frame)); },
            [this](const QString& failure) { failCapture(failure); });
        m_worker->start();
        requestCapture();
    }
    QImage initialFrame() const override {
        return m_initialFrame;
    }
    void releaseInitialFrame() override {
        m_initialFrame = {};
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
    QString windowTitle() const override {
        return m_target.title;
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
        const bool native = m_native && !m_nativeDisabled &&
                            m_native->supportsTransform(presentation.imageTransform) &&
                            m_state == LiveWindowState::Live;
        if (native) {
            m_frameHandoffPending = false;
            if (!m_native->present(presentation, presentationCrop(),
                                   m_monitor->state().desktopBounds, error)) {
                if (m_native->consumesFrames()) {
                    disableNativePresentation();
                    return !m_stopped;
                }
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
            if (m_native->consumesFrames() && m_nativeFrame && presentation.visible &&
                !m_native->presentFrame(m_nativeFrame, m_nativeFrameBounds)) {
                disableNativePresentation();
                return !m_stopped;
            }
            m_retryAttempt = 0;
        } else {
            if (before && presentation.visible && m_state == LiveWindowState::Live) {
                if (!m_frameHandoffPending) {
                    m_frameHandoffPending = true;
                    requestCapture();
                }
            } else {
                stopNativePresentation();
            }
        }
        if (m_stopped)
            return false;
        setContinuous((!native || m_native->consumesFrames()) && presentation.visible &&
                      m_state == LiveWindowState::Live);
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
        m_retryTimer.stop();
        if (m_monitor)
            disconnect(m_monitor.get(), nullptr, this, nullptr);
        if (m_worker)
            m_worker->stop();
        m_worker.reset();
        if (m_native)
            m_native->stopPresentation();
        m_native.reset();
        m_nativeFrame.reset();
        m_monitor.reset();
        m_initialFrame = {};
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
    void disableNativePresentation() {
        m_nativeDisabled = true;
        stopNativePresentation();
        setContinuous(m_presentation.visible && m_state == LiveWindowState::Live);
        requestCapture();
        emit nativePresentationChanged(false);
    }
    void stopNativePresentation() {
        if (m_native)
            m_native->stopPresentation();
    }
    bool event(QEvent* event) override {
        const auto keepAlive = weak_from_this().lock();
        if (event && event->type() == QEvent::LanguageChange && !m_stopped) {
            QString error;
            if (m_state == LiveWindowState::Unavailable && m_monitor) {
                const auto monitor = m_monitor;
                monitor->refresh();
                if (m_stopped)
                    return LiveWindowSource::event(event);
                const auto current = monitor->state();
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
        if (m_stopped || !m_monitor || m_state == LiveWindowState::Closed)
            return;
        const auto current = m_monitor->state();
        if (current.state != LiveWindowState::Live) {
            m_retryPending = false;
            const bool native = usesNativePresentation();
            stopNativePresentation();
            m_frameHandoffPending = false;
            setContinuous(false);
            if (m_worker)
                m_worker->cancelRequest();
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
        if (m_native && !m_nativeDisabled && m_presentation.owner &&
            m_native->supportsTransform(m_presentation.imageTransform)) {
            QString error;
            if (!m_native->present(m_presentation, presentationCrop(), current.desktopBounds,
                                   &error)) {
                if (m_native->consumesFrames()) {
                    disableNativePresentation();
                    return;
                }
                setState(LiveWindowState::Unavailable, error);
                if (m_stopped)
                    return;
                scheduleRetry();
                emit nativePresentationChanged(false);
            }
        }
    }
    quint64 requestCapture() {
        return m_worker->requestCapture();
    }
    void setContinuous(bool continuous) {
        if (m_worker)
            m_worker->setContinuous(
                continuous, !m_native || m_nativeDisabled || !m_native->consumesFrames() ||
                                !m_native->supportsTransform(m_presentation.imageTransform));
    }
    void acceptFrame(CapturedWindowFrame frame) {
        const auto keepAlive = shared_from_this();
        if (m_stopped || m_state == LiveWindowState::Closed)
            return;
        m_retryPending = false;
        if (!m_initialized || m_state != LiveWindowState::Live) {
            const auto monitor = m_monitor;
            monitor->refresh();
            if (m_stopped)
                return;
            const auto nativeState = monitor->state();
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
        m_nativeFrame = std::move(frame.native);
        m_nativeFrameBounds = frame.desktopBounds;
        if (!m_initialized) {
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
        if (image.isNull() && !m_nativeFrame) {
            failCapture(liveText(QT_TRANSLATE_NOOP(
                "LiveWindowSource", "The window content is temporarily unavailable. Retrying…")));
            return;
        }
        const bool resumed = m_state != LiveWindowState::Live;
        setState(LiveWindowState::Live);
        if (m_stopped)
            return;
        if (m_nativeFrame && usesNativePresentation() &&
            !m_native->presentFrame(m_nativeFrame, m_nativeFrameBounds)) {
            disableNativePresentation();
        }
        if (m_stopped)
            return;
        if (!image.isNull())
            emit frameChanged(image);
        if (m_stopped)
            return;
        if (m_frameHandoffPending && !image.isNull() &&
            (!m_native || !m_native->supportsTransform(m_presentation.imageTransform))) {
            m_frameHandoffPending = false;
            stopNativePresentation();
            emit nativePresentationChanged(false);
        }
        if (m_stopped)
            return;
        const auto pending = std::exchange(m_pendingFrames, {});
        for (const auto& request : pending) {
            if (m_stopped)
                request.second({}, liveText(QT_TRANSLATE_NOOP("LiveWindowSource",
                                                              "The live window was closed.")));
            else if (!image.isNull() && request.first <= frame.requestGeneration)
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
        stopNativePresentation();
        m_frameHandoffPending = false;
        if (native)
            emit nativePresentationChanged(false);
        if (m_stopped)
            return;
        const auto monitor = m_monitor;
        monitor->refresh();
        if (m_stopped)
            return;
        const auto state = monitor->state();
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
        m_retryTimer.start(delays[qMin(m_retryAttempt, 3)]);
        m_retryAttempt = qMin(m_retryAttempt + 1, 3);
    }
    LiveWindowTarget m_target;
    QRect m_selection;
    std::unique_ptr<live_window_detail::NativeWindowPresentation> m_native;
    std::shared_ptr<const live_window_detail::NativeWindowFrame> m_nativeFrame;
    QRectF m_nativeFrameBounds;
    bool m_nativeDisabled = false;
    std::shared_ptr<SharedWindowMonitor> m_monitor;
    QTimer m_retryTimer;
    InitializationCompletion m_initializationCompletion;
    QVector<std::pair<quint64, FrameCompletion>> m_pendingFrames;
    QImage m_initialFrame;
    QRectF presentationCrop() const {
        const QRectF region = m_presentation.sourceViewport.intersected(QRectF(0, 0, 1, 1));
        return QRectF(m_crop.x() + region.x() * m_crop.width(),
                      m_crop.y() + region.y() * m_crop.height(), region.width() * m_crop.width(),
                      region.height() * m_crop.height());
    }
    QRect m_initialDesktopRegion;
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
    std::shared_ptr<LiveCaptureWorker> m_worker;
};
} // namespace

std::shared_ptr<LiveWindowSource> createLiveWindowSource(const LiveWindowTarget& target,
                                                         const QRect& selection) {
    if (!target.nativeId || target.processId <= 0 || selection.isEmpty())
        return {};
    return std::make_shared<CapturedLiveWindowSource>(target, selection);
}
} // namespace snow_shot::presentation

#include "livewindowsource.moc"
