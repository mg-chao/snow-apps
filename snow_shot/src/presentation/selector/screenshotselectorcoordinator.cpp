#include "snow_shot/presentation/screenshotselectorcoordinator.h"

#include "../capture/screenshotcaptureperfinstrumentation.h"
#include "screenshotselectorserviceclient.h"
#include "snow_shot/storage/applicationstorage.h"

ScreenshotSelectorCoordinator::ScreenshotSelectorCoordinator(QObject* parent) : QObject(parent) {
    m_serviceClient = std::make_unique<ScreenshotSelectorServiceClient>(
        ScreenshotSelectorServiceClientCallbacks{
            [this](quint64 requestId, bool ok) { handleRefreshFinished(requestId, ok); },
            [this](quint64 requestId, bool ok, const QVector<QRectF>& hitRects) {
                handleHitTestFinished(requestId, ok, hitRects);
            },
        },
        this);

    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    if (storage.isInitialized()) {
        connect(&storage, &snow_shot::storage::ApplicationStorage::smartSelectionChanged, this,
                [this](bool) {
                    const bool refreshRequired = m_ready || m_refreshInFlight;
                    const QVector<std::uintptr_t> excludedHwnds = m_lastExcludedHwnds;
                    destroyService();
                    if (refreshRequired) {
                        static_cast<void>(startRefresh(excludedHwnds));
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
    ++m_refreshRequestId;
    ++m_hitTestRequestId;
    m_ready = false;
    m_refreshInFlight = false;
    m_hitTestInFlight = false;
    m_hasPendingHitTestPoint = false;
    m_pendingHitTestPoint = QPoint();
    m_pendingHitTestMode = ScreenshotSelectorHitTestMode::Window;
}

void ScreenshotSelectorCoordinator::resetHitTestState() {
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

bool ScreenshotSelectorCoordinator::startRefresh(const QVector<std::uintptr_t>& excludedHwnds) {
    if (m_refreshInFlight) {
        return false;
    }
    m_lastExcludedHwnds = excludedHwnds;
    const quint64 requestId = ++m_refreshRequestId;
    m_refreshInFlight = true;
    m_ready = false;
    if (!m_serviceClient->startRefresh(requestId, excludedHwnds)) {
        m_refreshInFlight = false;
        m_ready = false;
        return false;
    }
    return true;
}

bool ScreenshotSelectorCoordinator::requestHitTest(const QPoint& physicalPoint,
                                                    ScreenshotSelectorHitTestMode mode) {
    if ((!m_ready && !m_refreshInFlight) || !m_serviceClient->hasService()) {
        return false;
    }

    m_pendingHitTestPoint = physicalPoint;
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
    if (!m_serviceClient->startHitTest(requestId, point, mode)) {
        m_hitTestInFlight = false;
        emit hitTestFinished(false, {});
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
    emit refreshFinished(ok);
}

void ScreenshotSelectorCoordinator::handleHitTestFinished(quint64 requestId, bool ok,
                                                          const QVector<QRectF>& hitRects) {
    if (requestId != m_hitTestRequestId) {
        return;
    }

    m_hitTestInFlight = false;
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("selector.hit_test_finished");
    SNOW_SHOT_CAPTURE_PERF_COUNTER("selector.hit_test_ok", ok ? 1 : 0);
    emit hitTestFinished(ok, hitRects);
}
