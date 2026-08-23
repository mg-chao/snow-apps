#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORCOORDINATOR_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORCOORDINATOR_H

#include <QObject>
#include <QPoint>
#include <QRectF>
#include <QVector>

#include <cstdint>
#include <memory>

#include "snow_shot/presentation/screenshotselectorworkflowports.h"

class ScreenshotSelectorServiceClient;

class ScreenshotSelectorCoordinator final : public QObject, public ScreenshotSelectorServicePort {
    Q_OBJECT

  public:
    explicit ScreenshotSelectorCoordinator(QObject* parent = nullptr);
    ~ScreenshotSelectorCoordinator() override;

    [[nodiscard]] bool ready() const override;
    [[nodiscard]] bool refreshInFlight() const override;
    bool hitTestInFlight() const;

    void resetRequests();
    void resetHitTestState();
    void releaseCache();
    void destroyService();

    [[nodiscard]] bool startRefresh(const QVector<std::uintptr_t>& excludedHwnds) override;
    [[nodiscard]] bool requestHitTest(const QPoint& physicalPoint,
                                      ScreenshotSelectorHitTestMode mode) override;
    void startNextHitTest() override;

  signals:
    void refreshFinished(bool ok);
    void hitTestFinished(bool ok, QPoint physicalPoint, QVector<QRectF> hitRects);

  private:
    void handleRefreshFinished(quint64 requestId, bool ok);
    void handleHitTestFinished(quint64 requestId, bool ok, const QVector<QRectF>& hitRects);

    std::unique_ptr<ScreenshotSelectorServiceClient> m_serviceClient;
    quint64 m_refreshRequestId = 0;
    quint64 m_hitTestRequestId = 0;
    bool m_ready = false;
    bool m_refreshInFlight = false;
    bool m_hitTestInFlight = false;
    QPoint m_inFlightHitTestPoint;
    bool m_hasPendingHitTestPoint = false;
    QPoint m_pendingHitTestPoint;
    ScreenshotSelectorHitTestMode m_pendingHitTestMode =
        ScreenshotSelectorHitTestMode::Window;
    QVector<std::uintptr_t> m_lastExcludedHwnds;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORCOORDINATOR_H
