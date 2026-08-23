#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORSERVICECLIENT_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORSERVICECLIENT_H

#include "snow_shot/presentation/screenshotselectorworkflowports.h"

#include <QObject>
#include <QPoint>
#include <QRectF>
#include <QVector>

#include <cstdint>
#include <functional>

typedef struct SnowUiSelectorServiceImpl SnowUiSelectorService;
typedef struct SnowUiSelectorHitPathImpl SnowUiSelectorHitPath;

struct ScreenshotSelectorServiceClientCallbacks {
    std::function<void(quint64 requestId, bool ok)> refreshFinished;
    std::function<void(quint64 requestId, bool ok, const QVector<QRectF>& hitRects)>
        hitTestFinished;
};

class ScreenshotSelectorServiceClient final : public QObject {
  public:
    explicit ScreenshotSelectorServiceClient(ScreenshotSelectorServiceClientCallbacks callbacks,
                                             QObject* parent = nullptr);
    ~ScreenshotSelectorServiceClient() override;

    [[nodiscard]] bool hasService() const;
    [[nodiscard]] bool ensureService();
    [[nodiscard]] bool releaseCache();
    void destroyService();

    [[nodiscard]] bool startRefresh(quint64 requestId,
                                    const QVector<std::uintptr_t>& excludedHwnds);
    [[nodiscard]] bool startHitTest(quint64 requestId, const QPoint& physicalPoint,
                                    ScreenshotSelectorHitTestMode mode);

  private:
    struct RefreshCallbackContext;
    struct HitTestCallbackContext;

    static void refreshCallback(std::uint64_t requestId, std::uint8_t ok, void* userdata);
    static void hitTestCallback(std::uint64_t requestId, SnowUiSelectorHitPath* path,
                                std::uint8_t ok, void* userdata);

    ScreenshotSelectorServiceClientCallbacks m_callbacks;
    SnowUiSelectorService* m_service = nullptr;
    int m_serviceBackend = -1;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORSERVICECLIENT_H
