#include "screenshotselectorserviceclient.h"
#include "screenshotselectorpolicy.h"

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
#include "../capture/screenshotcaptureperfinstrumentation.h"
#include "snow_ui_selector.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/settingsadapters.h"
#endif

#include <QByteArray>
#include <QMetaObject>
#ifdef Q_OS_MACOS
#include <QCursor>
#include <CoreGraphics/CoreGraphics.h>
#endif

#include <utility>

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
struct ScreenshotSelectorServiceClient::CallbackBridge {
    ScreenshotSelectorServiceClient* client;
};

namespace {
#ifndef Q_OS_MACOS
QByteArray configuredSelectorBackend() {
    QByteArray backend = qgetenv("SNOW_SHOT_SELECTOR_BACKEND");
    if (backend.isEmpty()) {
        backend = qgetenv("SNOW_SHOT_UI_SELECTOR_BACKEND");
    }
    if (backend.isEmpty()) {
        backend = snow_shot::storage::ApplicationStorage::instance().isInitialized()
                      ? snow_shot::storage::ScreenshotSettings().windowElementApi().toLatin1()
                      : snow_shot::storage::ConfigurationSchema::defaultValue(
                            QStringLiteral("screenshot/window_element_api"))
                            .toString()
                            .toLatin1();
    }
    return backend.trimmed().toLower();
}

#endif

bool smartSelectionEnabled() {
    const auto& storage = snow_shot::storage::ApplicationStorage::instance();
    return storage.isInitialized() ? storage.smartSelectionEnabled()
                                   : snow_shot::storage::ConfigurationSchema::defaultValue(
                                         QStringLiteral("screenshot_selection/smart_selection"))
                                         .toBool();
}

SnowUiSelectorBackend selectorBackendForCurrentMode() {
#ifdef Q_OS_MACOS
    return SNOW_UI_SELECTOR_BACKEND_ACCESSIBILITY;
#else
    return screenshotSelectorLookupPolicy(smartSelectionEnabled(), configuredSelectorBackend())
        .backend;
#endif
}

SnowUiSelectorHitTestMode
hitTestModeForRequestedTarget(ScreenshotSelectorHitTestMode requestedMode) {
    return screenshotSelectorHitTestMode(smartSelectionEnabled(), requestedMode);
}

} // namespace

ScreenshotSelectorServiceClient::ScreenshotSelectorServiceClient(
    ScreenshotSelectorServiceClientCallbacks callbacks, QObject* parent)
    : QObject(parent), m_bridge(std::make_unique<CallbackBridge>(CallbackBridge{this})),
      m_callbacks(std::move(callbacks)) {}

ScreenshotSelectorServiceClient::~ScreenshotSelectorServiceClient() {
    destroyService();
}

quint32 ScreenshotSelectorServiceClient::displayIdAtCursor() {
    uint32_t displayId = 0;
#ifdef Q_OS_MACOS
    const QPoint cursor = QCursor::pos();
    uint32_t count = 0;
    CGGetDisplaysWithPoint(CGPointMake(cursor.x(), cursor.y()), 1, &displayId, &count);
#endif
    return displayId;
}

bool ScreenshotSelectorServiceClient::hasService() const {
    return m_service != nullptr;
}

bool ScreenshotSelectorServiceClient::ensureService() {
    if (m_service != nullptr) {
        return true;
    }

    {
        SNOW_SHOT_CAPTURE_PERF_SCOPE("selector.service_create");
        m_service = snow_ui_selector_service_create(
            &ScreenshotSelectorServiceClient::resultCallback,
            &ScreenshotSelectorServiceClient::refreshCallback, m_bridge.get());
    }
    if (m_service != nullptr) {
        SNOW_SHOT_CAPTURE_PERF_COUNTER("selector.service_created", 1);
        SNOW_SHOT_CAPTURE_PERF_MILESTONE("selector.service_ready");
    }
    return m_service != nullptr;
}

bool ScreenshotSelectorServiceClient::releaseCache() {
    if (m_service == nullptr) {
        return true;
    }

    return snow_ui_selector_service_release_cache(m_service) != 0;
}

void ScreenshotSelectorServiceClient::destroyService() {
    SnowUiSelectorService* service = std::exchange(m_service, nullptr);
    m_serviceBackend = -1;
    if (service == nullptr) {
        return;
    }

    snow_ui_selector_service_destroy(service);
}

bool ScreenshotSelectorServiceClient::startRefresh(quint64 requestId,
                                                   const QVector<std::uintptr_t>& excludedHwnds) {
    if (!ensureService()) {
        return false;
    }

    m_serviceBackend = static_cast<int>(selectorBackendForCurrentMode());
    const std::uintptr_t* data = excludedHwnds.isEmpty() ? nullptr : excludedHwnds.constData();
    const bool started =
        snow_ui_selector_service_refresh(m_service, requestId,
                                         static_cast<SnowUiSelectorBackend>(m_serviceBackend), data,
                                         static_cast<size_t>(excludedHwnds.size())) != 0;
    if (started)
        SNOW_SHOT_CAPTURE_PERF_MILESTONE("selector.refresh_dispatched");
    return started;
}

bool ScreenshotSelectorServiceClient::startHitTest(quint64 epoch, quint64 requestId,
                                                   quint64 generation, const QPoint& point,
                                                   ScreenshotSelectorHitTestMode mode,
                                                   quint32 displayId) {
    if (!hasService())
        return false;
    const SnowUiSelectorQuery query{epoch,     requestId, generation,
                                    point.x(), point.y(), hitTestModeForRequestedTarget(mode),
                                    displayId};
    const bool started = snow_ui_selector_service_query(m_service, &query) != 0;
    if (started)
        SNOW_SHOT_CAPTURE_PERF_MILESTONE("selector.hit_test_dispatched");
    return started;
}

bool ScreenshotSelectorServiceClient::startRefinement(const ScreenshotSelectorResult& initial) {
    if (!hasService() || !initial.canRefine)
        return false;
    const SnowUiSelectorQuery query{initial.epoch,      initial.requestId,
                                    initial.generation, initial.point.x(),
                                    initial.point.y(),  SNOW_UI_SELECTOR_HIT_TEST_MODE_UI_ELEMENT,
                                    initial.displayId};
    return snow_ui_selector_service_refine(m_service, &query) != 0;
}

void ScreenshotSelectorServiceClient::invalidateRefinement() {
    if (hasService())
        snow_ui_selector_service_invalidate_refinement(m_service);
}

void ScreenshotSelectorServiceClient::refreshCallback(std::uint64_t epoch, std::uint8_t ok,
                                                      void* userdata) {
    auto* client = static_cast<CallbackBridge*>(userdata)->client;
    // The native close barrier keeps the bridge and QObject alive throughout this call.
    QMetaObject::invokeMethod(
        client,
        [client, epoch, ok]() {
            if (client->m_callbacks.refreshFinished)
                client->m_callbacks.refreshFinished(epoch, ok != 0);
        },
        Qt::QueuedConnection);
}

void ScreenshotSelectorServiceClient::resultCallback(const SnowUiSelectorEvent* event,
                                                     void* userdata) {
    auto* client = static_cast<CallbackBridge*>(userdata)->client;
    ScreenshotSelectorResult result;
    result.epoch = event->query.epoch;
    result.requestId = event->query.request_id;
    result.generation = event->query.generation;
    result.point = QPoint(event->query.x, event->query.y);
    result.mode = event->query.mode == SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW
                      ? ScreenshotSelectorHitTestMode::Window
                      : ScreenshotSelectorHitTestMode::WindowSubElement;
    result.phase = static_cast<ScreenshotSelectorResultPhase>(event->phase);
    result.stopReason = static_cast<ScreenshotSelectorStopReason>(event->reason);
    result.ok = event->ok != 0;
    result.elapsedUs = event->elapsed_us;
    result.displayId = event->query.display_id;
    result.rects.reserve(static_cast<qsizetype>(event->count));
    for (size_t i = 0; i < event->count; ++i) {
        const auto& rect = event->rects[i];
        if (rect.right > rect.left && rect.bottom > rect.top)
            result.rects.push_back(
                QRectF(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top));
    }
    QMetaObject::invokeMethod(
        client,
        [client, result = std::move(result)]() mutable {
            result.canRefine =
                (client->m_serviceBackend == SNOW_UI_SELECTOR_BACKEND_UIA ||
                 client->m_serviceBackend == SNOW_UI_SELECTOR_BACKEND_ACCESSIBILITY) &&
                result.ok && result.mode == ScreenshotSelectorHitTestMode::WindowSubElement &&
                (result.stopReason == ScreenshotSelectorStopReason::BudgetExhausted ||
                 result.stopReason == ScreenshotSelectorStopReason::DecodingPending ||
                 result.stopReason == ScreenshotSelectorStopReason::ProviderTimeout ||
                 result.stopReason == ScreenshotSelectorStopReason::AccessibilityPending);
            if (client->m_callbacks.resultReady)
                client->m_callbacks.resultReady(result);
        },
        Qt::QueuedConnection);
}

#else
// Native selector unavailable on unsupported platforms.
quint32 ScreenshotSelectorServiceClient::displayIdAtCursor() {
    return 0;
}
struct ScreenshotSelectorServiceClient::CallbackBridge {};
ScreenshotSelectorServiceClient::ScreenshotSelectorServiceClient(
    ScreenshotSelectorServiceClientCallbacks callbacks, QObject* parent)
    : QObject(parent), m_callbacks(std::move(callbacks)) {}
ScreenshotSelectorServiceClient::~ScreenshotSelectorServiceClient() = default;
bool ScreenshotSelectorServiceClient::hasService() const {
    return false;
}
bool ScreenshotSelectorServiceClient::ensureService() {
    return false;
}
bool ScreenshotSelectorServiceClient::releaseCache() {
    return true;
}
void ScreenshotSelectorServiceClient::destroyService() {}
bool ScreenshotSelectorServiceClient::startRefresh(quint64, const QVector<std::uintptr_t>&) {
    return false;
}
bool ScreenshotSelectorServiceClient::startHitTest(quint64, quint64, quint64, const QPoint&,
                                                   ScreenshotSelectorHitTestMode, quint32) {
    return false;
}
bool ScreenshotSelectorServiceClient::startRefinement(const ScreenshotSelectorResult&) {
    return false;
}
void ScreenshotSelectorServiceClient::invalidateRefinement() {}
#endif
