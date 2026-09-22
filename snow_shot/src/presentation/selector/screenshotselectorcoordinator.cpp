#include "snow_shot/presentation/screenshotselectorcoordinator.h"

#include "../capture/screenshotcaptureperfinstrumentation.h"
#include "screenshotselectorserviceclient.h"
#include "snow_shot/storage/applicationstorage.h"

ScreenshotSelectorCoordinator::ScreenshotSelectorCoordinator(QObject* parent,
                                                             std::function<qint64()> now)
    : QObject(parent), m_now(std::move(now)) {
    m_clock.start();
    if (!m_now)
        m_now = [this]() { return m_clock.elapsed(); };
    m_serviceClient = std::make_unique<ScreenshotSelectorServiceClient>(
        ScreenshotSelectorServiceClientCallbacks{
            [this](quint64 requestId, bool ok) { handleRefreshFinished(requestId, ok); },
            [this](const ScreenshotSelectorResult& result) { handleResult(result); },
        },
        this);

    m_refinementTimer.setSingleShot(true);
    m_refinementTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_refinementTimer, &QTimer::timeout, this, [this]() { scheduleRefinement(); });
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    if (storage.isInitialized()) {
        connect(&storage.configuration(), &snow_shot::storage::ConfigurationStore::valueChanged,
                this, [this](const QString& key, const QJsonValue&) {
#ifdef Q_OS_MACOS
                    if (key == QStringLiteral("screenshot/window_element_api"))
                        return;
#endif
                    if (key != QStringLiteral("screenshot_selection/smart_selection") &&
                        key != QStringLiteral("screenshot/window_element_api")) {
                        return;
                    }
                    const bool refreshRequired = m_ready || m_refreshInFlight;
                    const QVector<std::uintptr_t> excludedHwnds = m_lastExcludedHwnds;
                    releaseCache();
                    if (refreshRequired) {
                        if (m_lastDisplays.isEmpty())
                            static_cast<void>(startRefresh(excludedHwnds));
                        else
                            static_cast<void>(
                                startRefreshWithDisplays(excludedHwnds, m_lastDisplays));
                    }
                });
    }
}

ScreenshotSelectorCoordinator::~ScreenshotSelectorCoordinator() {
    destroyService();
}

bool ScreenshotSelectorCoordinator::ready() const {
    return m_ready;
}

bool ScreenshotSelectorCoordinator::refreshInFlight() const {
    return m_refreshInFlight;
}

bool ScreenshotSelectorCoordinator::hitTestInFlight() const {
    return m_hitTestInFlight;
}

void ScreenshotSelectorCoordinator::resetRequests() {
    cancelRefinement();
    ++m_targetGeneration;
    m_hasTarget = false;
    ++m_refreshRequestId;
    ++m_hitTestRequestId;
    m_permissionWarningShown = false;
    m_ready = false;
    m_refreshInFlight = false;
    m_hitTestInFlight = false;
    m_hasPendingHitTestPoint = false;
    m_pendingHitTestPoint = QPoint();
    m_pendingHitTestMode = ScreenshotSelectorHitTestMode::Window;
}

void ScreenshotSelectorCoordinator::resetHitTestState() {
    cancelRefinement();
    ++m_targetGeneration;
    m_hasTarget = false;
    ++m_hitTestRequestId;
    m_hitTestInFlight = false;
    m_hasPendingHitTestPoint = false;
    m_pendingHitTestPoint = QPoint();
    m_pendingHitTestMode = ScreenshotSelectorHitTestMode::Window;
}

void ScreenshotSelectorCoordinator::releaseCache() {
    resetRequests();
    if (!m_serviceClient->releaseCache()) {
        // A closed worker leaves the service handle unusable. Drop it so the
        // next capture can create a fresh worker instead of reusing a dead one.
        m_serviceClient->destroyService();
    }
}

void ScreenshotSelectorCoordinator::destroyService() {
    resetRequests();
    m_ready = false;
    m_serviceClient->destroyService();
}

bool ScreenshotSelectorCoordinator::dispatchRefresh(const QVector<std::uintptr_t>& excludedHwnds,
                                                    const QVector<CapturedDisplayModel>* displays) {
    if (m_refreshInFlight || (displays != nullptr && displays->isEmpty()))
        return false;
    resetHitTestState();
    m_lastExcludedHwnds = excludedHwnds;
    m_lastDisplays = displays != nullptr ? *displays : QVector<CapturedDisplayModel>{};
    const quint64 requestId = ++m_refreshRequestId;
    m_refreshInFlight = true;
    m_ready = false;
    const bool started =
        displays != nullptr
            ? m_serviceClient->startRefreshWithDisplays(requestId, excludedHwnds, *displays)
            : m_serviceClient->startRefresh(requestId, excludedHwnds);
    if (!started) {
        m_refreshInFlight = false;
        m_ready = false;
        return false;
    }
    return true;
}

bool ScreenshotSelectorCoordinator::startRefresh(const QVector<std::uintptr_t>& excludedHwnds) {
    return dispatchRefresh(excludedHwnds, nullptr);
}

bool ScreenshotSelectorCoordinator::startRefreshWithDisplays(
    const QVector<std::uintptr_t>& excludedHwnds, const QVector<CapturedDisplayModel>& displays) {
    return dispatchRefresh(excludedHwnds, &displays);
}

bool ScreenshotSelectorCoordinator::requestHitTest(const QPoint& physicalPoint,
                                                   ScreenshotSelectorHitTestMode mode) {
    return requestHitTestOnDisplay(physicalPoint, mode, 0);
}

bool ScreenshotSelectorCoordinator::requestHitTestOnDisplay(const QPoint& physicalPoint,
                                                            ScreenshotSelectorHitTestMode mode,
                                                            quint32 displayId) {
    if ((!m_ready && !m_refreshInFlight) || !m_serviceClient->hasService()) {
        return false;
    }

    if (m_hasTarget && m_pendingHitTestPoint == physicalPoint && m_pendingHitTestMode == mode &&
        m_pendingDisplayId == displayId) {
        return true;
    }
    cancelRefinement();
    ++m_targetGeneration;
    m_hasTarget = true;
    m_targetChangedAt = m_now();
    emit targetChanged();
    m_pendingHitTestPoint = physicalPoint;
    m_pendingDisplayId = displayId;
    m_pendingHitTestMode = mode;
    m_hasPendingHitTestPoint = true;
    if (!m_hitTestInFlight) {
        startNextHitTest();
    }
    return true;
}

void ScreenshotSelectorCoordinator::startNextHitTest() {
    if (!m_hasPendingHitTestPoint || m_hitTestInFlight || !m_serviceClient->hasService()) {
        return;
    }

    const QPoint point = m_pendingHitTestPoint;
    const ScreenshotSelectorHitTestMode mode = m_pendingHitTestMode;
    m_hasPendingHitTestPoint = false;
    const quint64 requestId = ++m_hitTestRequestId;
    m_hitTestInFlight = true;
    if (!m_serviceClient->startHitTest(m_refreshRequestId, requestId, m_targetGeneration, point,
                                       mode, m_pendingDisplayId)) {
        m_hitTestInFlight = false;
        emit initialResultReady(false, {});
    }
}

void ScreenshotSelectorCoordinator::handleRefreshFinished(quint64 requestId, bool ok) {
    if (requestId != m_refreshRequestId) {
        return;
    }

    m_refreshInFlight = false;
    m_ready = ok;
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("selector.refresh_finished");
    SNOW_SHOT_CAPTURE_PERF_COUNTER("selector.refresh_ok", ok ? 1 : 0);
    if (!ok) {
        m_hasPendingHitTestPoint = false;
        m_hasTarget = false;
        m_hitTestInFlight = false;
    }
    emit refreshFinished(ok);
    if (ok)
        startNextHitTest();
}

void ScreenshotSelectorCoordinator::cancelRefinement() {
    m_refinementTimer.stop();
    // Moving through complete cached paths must not touch the refinement worker.
    // One invalidation is sufficient even if its obsolete provider call is still returning.
    if (m_refinementSubmitted && m_serviceClient)
        m_serviceClient->invalidateRefinement();
    m_refinementSubmitted = false;
    m_initial = {};
}

void ScreenshotSelectorCoordinator::scheduleRefinement() {
    if (!m_ready || m_hitTestInFlight || m_hasPendingHitTestPoint || m_refinementSubmitted ||
        !m_hasTarget || !m_initial.canRefine || m_initial.generation != m_targetGeneration)
        return;
    const qint64 remaining = 80 - (m_now() - m_targetChangedAt);
    if (remaining > 0) {
        m_refinementTimer.start(static_cast<int>(remaining));
        return;
    }
    m_refinementSubmitted = true;
    static_cast<void>(m_serviceClient->startRefinement(m_initial));
}

void ScreenshotSelectorCoordinator::handleResult(const ScreenshotSelectorResult& result) {
    if (result.epoch != m_refreshRequestId)
        return;
    SNOW_SHOT_CAPTURE_PERF_COUNTER("selector.result_phase", static_cast<int>(result.phase));
    SNOW_SHOT_CAPTURE_PERF_COUNTER("selector.stop_reason", static_cast<int>(result.stopReason));
    SNOW_SHOT_CAPTURE_PERF_COUNTER("selector.elapsed_us", static_cast<qint64>(result.elapsedUs));
    const auto warnIfPermissionRequired = [this, &result] {
        if (result.stopReason == ScreenshotSelectorStopReason::PermissionRequired &&
            !m_permissionWarningShown) {
            m_permissionWarningShown = true;
            emit accessibilityPermissionRequired();
        }
    };
    if (result.phase == ScreenshotSelectorResultPhase::Initial) {
        if (!m_hitTestInFlight || result.requestId != m_hitTestRequestId)
            return;
        m_hitTestInFlight = false;
#ifdef Q_OS_MACOS
        if (!m_hasTarget || result.generation != m_targetGeneration ||
            result.point != m_pendingHitTestPoint || result.mode != m_pendingHitTestMode ||
            result.displayId != m_pendingDisplayId) {
            startNextHitTest();
            return;
        }
#endif
        m_initial = result.canRefine ? result : ScreenshotSelectorResult{};
        SNOW_SHOT_CAPTURE_PERF_MILESTONE("selector.hit_test_finished");
        SNOW_SHOT_CAPTURE_PERF_COUNTER("selector.hit_test_ok", result.ok ? 1 : 0);
        emit initialResultReady(result.ok, result.rects, result.displayId);
        warnIfPermissionRequired();
        startNextHitTest();
        scheduleRefinement();
        return;
    }
    if (!m_hasTarget || result.generation != m_targetGeneration ||
        result.requestId != m_initial.requestId || result.point != m_pendingHitTestPoint ||
        result.mode != m_pendingHitTestMode || result.displayId != m_pendingDisplayId ||
        m_hitTestInFlight || m_hasPendingHitTestPoint)
        return;
    if (result.ok && result.stopReason != ScreenshotSelectorStopReason::Cancelled &&
        !result.rects.isEmpty()) {
        const bool replacePath =
            result.stopReason == ScreenshotSelectorStopReason::PermissionRequired
#ifdef Q_OS_MACOS
            || result.stopReason == ScreenshotSelectorStopReason::ProviderFailure
#endif
            ;
        emit refinementReady(result.rects, result.displayId, replacePath);
        warnIfPermissionRequired();
    }
}
