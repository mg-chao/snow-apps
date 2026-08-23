#include "screenshotselectorserviceclient.h"
#include "screenshotselectorpolicy.h"

#include "snow_ui_selector.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationschema.h"

#include <QByteArray>
#include <QMetaObject>
#include <QPointer>

#include <utility>

struct ScreenshotSelectorServiceClient::RefreshCallbackContext {
    QPointer<ScreenshotSelectorServiceClient> client;
};

struct ScreenshotSelectorServiceClient::HitTestCallbackContext {
    QPointer<ScreenshotSelectorServiceClient> client;
};

namespace {
QByteArray configuredSelectorBackend() {
    QByteArray backend = qgetenv("SNOW_SHOT_SELECTOR_BACKEND");
    if (backend.isEmpty()) {
        backend = qgetenv("SNOW_SHOT_UI_SELECTOR_BACKEND");
    }
    return backend.trimmed().toLower();
}

bool smartSelectionEnabled() {
    const auto& storage = snow_shot::storage::ApplicationStorage::instance();
    return storage.isInitialized()
               ? storage.smartSelectionEnabled()
               : snow_shot::storage::ConfigurationSchema::defaultValue(
                     QStringLiteral("screenshot_selection/smart_selection"))
                     .toBool();
}

SnowUiSelectorBackend selectorBackendForCurrentMode() {
    return screenshotSelectorLookupPolicy(smartSelectionEnabled(), configuredSelectorBackend())
        .backend;
}

SnowUiSelectorHitTestMode hitTestModeForRequestedTarget(
    ScreenshotSelectorHitTestMode requestedMode) {
    return screenshotSelectorHitTestMode(smartSelectionEnabled(), requestedMode);
}

QVector<QRectF> rectsFromHitPath(SnowUiSelectorHitPath* path) {
    QVector<QRectF> rects;
    if (path == nullptr) {
        return rects;
    }

    const size_t count = snow_ui_selector_hit_path_count(path);
    rects.reserve(static_cast<int>(count));
    for (size_t index = 0; index < count; ++index) {
        SnowUiSelectorRect rect{};
        if (snow_ui_selector_hit_path_rect(path, index, &rect) == 0) {
            continue;
        }
        if (rect.right <= rect.left || rect.bottom <= rect.top) {
            continue;
        }
        rects.push_back(QRectF(QPointF(rect.left, rect.top),
                               QSizeF(rect.right - rect.left, rect.bottom - rect.top)));
    }
    return rects;
}
} // namespace

ScreenshotSelectorServiceClient::ScreenshotSelectorServiceClient(
    ScreenshotSelectorServiceClientCallbacks callbacks, QObject* parent)
    : QObject(parent), m_callbacks(std::move(callbacks)) {}

ScreenshotSelectorServiceClient::~ScreenshotSelectorServiceClient() {
    destroyService();
}

bool ScreenshotSelectorServiceClient::hasService() const {
    return m_service != nullptr;
}

bool ScreenshotSelectorServiceClient::ensureService() {
    const SnowUiSelectorBackend desiredBackend = selectorBackendForCurrentMode();
    if (m_service != nullptr &&
        m_serviceBackend != static_cast<int>(desiredBackend)) {
        destroyService();
    }
    if (m_service != nullptr) {
        return true;
    }

    m_service = snow_ui_selector_service_create(desiredBackend);
    if (m_service != nullptr) {
        m_serviceBackend = static_cast<int>(desiredBackend);
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

    const std::uintptr_t* data = excludedHwnds.isEmpty() ? nullptr : excludedHwnds.constData();
    auto* context = new RefreshCallbackContext{QPointer<ScreenshotSelectorServiceClient>(this)};
    const uint8_t started = snow_ui_selector_service_refresh_async(
        m_service, static_cast<std::uint64_t>(requestId), data,
        static_cast<size_t>(excludedHwnds.size()),
        &ScreenshotSelectorServiceClient::refreshCallback, context);
    if (started == 0) {
        delete context;
        return false;
    }
    return true;
}

bool ScreenshotSelectorServiceClient::startHitTest(quint64 requestId,
                                                   const QPoint& physicalPoint,
                                                   ScreenshotSelectorHitTestMode mode) {
    if (!ensureService()) {
        return false;
    }

    auto* context = new HitTestCallbackContext{QPointer<ScreenshotSelectorServiceClient>(this)};
    const uint8_t started = snow_ui_selector_service_hit_test_point_async(
        m_service, static_cast<std::uint64_t>(requestId), physicalPoint.x(), physicalPoint.y(),
        hitTestModeForRequestedTarget(mode),
        &ScreenshotSelectorServiceClient::hitTestCallback, context);
    if (started == 0) {
        delete context;
        return false;
    }
    return true;
}

void ScreenshotSelectorServiceClient::refreshCallback(std::uint64_t requestId, std::uint8_t ok,
                                                      void* userdata) {
    auto* context = static_cast<RefreshCallbackContext*>(userdata);
    const QPointer<ScreenshotSelectorServiceClient> client =
        context != nullptr ? context->client : QPointer<ScreenshotSelectorServiceClient>();
    delete context;
    if (client.isNull()) {
        return;
    }

    QMetaObject::invokeMethod(
        client,
        [client, requestId, ok]() {
            if (!client.isNull() && client->m_callbacks.refreshFinished) {
                client->m_callbacks.refreshFinished(static_cast<quint64>(requestId), ok != 0);
            }
        },
        Qt::QueuedConnection);
}

void ScreenshotSelectorServiceClient::hitTestCallback(std::uint64_t requestId,
                                                      SnowUiSelectorHitPath* path, std::uint8_t ok,
                                                      void* userdata) {
    QVector<QRectF> rects;
    if (ok != 0) {
        rects = rectsFromHitPath(path);
    }
    if (path != nullptr) {
        snow_ui_selector_hit_path_destroy(path);
    }

    auto* context = static_cast<HitTestCallbackContext*>(userdata);
    const QPointer<ScreenshotSelectorServiceClient> client =
        context != nullptr ? context->client : QPointer<ScreenshotSelectorServiceClient>();
    delete context;
    if (client.isNull()) {
        return;
    }

    QMetaObject::invokeMethod(
        client,
        [client, requestId, ok, rects = std::move(rects)]() {
            if (!client.isNull() && client->m_callbacks.hitTestFinished) {
                client->m_callbacks.hitTestFinished(static_cast<quint64>(requestId), ok != 0,
                                                    rects);
            }
        },
        Qt::QueuedConnection);
}
