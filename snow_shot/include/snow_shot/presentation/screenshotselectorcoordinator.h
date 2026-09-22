#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORCOORDINATOR_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORCOORDINATOR_H

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>
#include <QPoint>
#include <QRectF>
#include <QVector>

#include <cstdint>
#include <memory>
#include <functional>

#include "snow_shot/presentation/screenshotselectorworkflowports.h"
#include "snow_shot/presentation/screenshottypes.h"

class ScreenshotSelectorServiceClient;

class ScreenshotSelectorCoordinator final : public QObject, public ScreenshotSelectorServicePort {
    Q_OBJECT

  public:
    explicit ScreenshotSelectorCoordinator(QObject* parent = nullptr,
                                           std::function<qint64()> now = {});
    ~ScreenshotSelectorCoordinator() override;

    [[nodiscard]] bool ready() const override;
    [[nodiscard]] bool refreshInFlight() const override;
    bool hitTestInFlight() const;

    void resetRequests();
    void resetHitTestState();
    void releaseCache();
    void destroyService();

    [[nodiscard]] bool
    startRefreshWithDisplays(const QVector<std::uintptr_t>& excluded,
                             const QVector<CapturedDisplayModel>& displays) override;
    [[nodiscard]] bool requestHitTestOnDisplay(const QPoint& point,
                                               ScreenshotSelectorHitTestMode mode,
                                               quint32 displayId) override;
    [[nodiscard]] bool startRefresh(const QVector<std::uintptr_t>& excludedHwnds) override;
    [[nodiscard]] bool requestHitTest(const QPoint& physicalPoint,
                                      ScreenshotSelectorHitTestMode mode) override;

  signals:
    void refreshFinished(bool ok);
    void initialResultReady(bool ok, QVector<QRectF> hitRects, quint32 displayId = 0);
    void refinementReady(QVector<QRectF> hitRects, quint32 displayId = 0, bool replacePath = false);
    void targetChanged();
    void accessibilityPermissionRequired();

  private:
    [[nodiscard]] bool dispatchRefresh(const QVector<std::uintptr_t>& excludedHwnds,
                                       const QVector<CapturedDisplayModel>* displays);
    void startNextHitTest();
    void handleRefreshFinished(quint64 requestId, bool ok);
    void handleResult(const ScreenshotSelectorResult& result);
    Q_SLOT void scheduleRefinement();
    void cancelRefinement();
    QTimer m_refinementTimer;
    QElapsedTimer m_clock;
    std::function<qint64()> m_now;
    qint64 m_targetChangedAt = 0;
    ScreenshotSelectorResult m_initial;
    quint64 m_targetGeneration = 0;
    bool m_hasTarget = false;
    bool m_refinementSubmitted = false;

    std::unique_ptr<ScreenshotSelectorServiceClient> m_serviceClient;
    quint64 m_refreshRequestId = 0;
    quint64 m_hitTestRequestId = 0;
    bool m_ready = false;
    bool m_permissionWarningShown = false;
    bool m_refreshInFlight = false;
    bool m_hitTestInFlight = false;
    bool m_hasPendingHitTestPoint = false;
    QPoint m_pendingHitTestPoint;
    quint32 m_pendingDisplayId = 0;
    ScreenshotSelectorHitTestMode m_pendingHitTestMode = ScreenshotSelectorHitTestMode::Window;
    QVector<std::uintptr_t> m_lastExcludedHwnds;
    QVector<CapturedDisplayModel> m_lastDisplays;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORCOORDINATOR_H
