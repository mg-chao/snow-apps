#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORSERVICECLIENT_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORSERVICECLIENT_H

#include "snow_shot/presentation/screenshotselectorworkflowports.h"

#include <QObject>
#include <QPoint>
#include <QRectF>
#include <QVector>

#include <cstdint>
#include <functional>
#include <memory>

typedef struct SnowUiSelectorServiceImpl SnowUiSelectorService;
struct SnowUiSelectorEvent;

struct ScreenshotSelectorServiceClientCallbacks {
    std::function<void(quint64 requestId, bool ok)> refreshFinished;
    std::function<void(const ScreenshotSelectorResult&)> resultReady;
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
    [[nodiscard]] bool startHitTest(quint64 epoch, quint64 requestId, quint64 generation,
                                    const QPoint& physicalPoint,
                                    ScreenshotSelectorHitTestMode mode);
    [[nodiscard]] bool startRefinement(const ScreenshotSelectorResult& initial);
    void invalidateRefinement();

  private:
    struct CallbackBridge;
    static void refreshCallback(std::uint64_t epoch, std::uint8_t ok, void* userdata);
    static void resultCallback(const SnowUiSelectorEvent* event, void* userdata);
    std::unique_ptr<CallbackBridge> m_bridge;
    ScreenshotSelectorServiceClientCallbacks m_callbacks;
    SnowUiSelectorService* m_service = nullptr;
    int m_serviceBackend = -1;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORSERVICECLIENT_H
