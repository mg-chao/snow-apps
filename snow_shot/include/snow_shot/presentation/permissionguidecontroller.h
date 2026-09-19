#ifndef SNOW_SHOT_PRESENTATION_PERMISSIONGUIDECONTROLLER_H
#define SNOW_SHOT_PRESENTATION_PERMISSIONGUIDECONTROLLER_H

#include "snow_shot/presentation/apppermissionservice.h"
#include <QIcon>
#include <QRect>
#include <QUrl>
#include <optional>

class QWidget;
namespace snow_shot::presentation {
class PermissionGuideWidget;
struct PermissionGuideApplication {
    QString name;
    QUrl bundleUrl;
    QIcon icon;
};
struct PermissionGuideWindow {
    quint32 id = 0;
    QRectF bounds;
    int layer = 0;
    qreal alpha = 1;
    bool onScreen = true;
};
struct PermissionGuideEnvironment {
    bool settingsRunning = false;
    bool settingsActive = false;
    bool guideActive = false;
    QVector<PermissionGuideWindow> windows;
    QVector<QRect> availableScreens;
};
// Native lifetime and window discovery are injectable; tests never touch TCC or Settings.
class PermissionGuidePlatform {
  public:
    virtual ~PermissionGuidePlatform() = default;
    virtual PermissionGuideApplication application() = 0;
    virtual PermissionGuideEnvironment environment() = 0;
    virtual void start(std::function<void()> changed) = 0;
    virtual void stop() = 0;
    virtual void prepareWindow(QWidget* window) = 0;
    virtual qint64 monotonicMilliseconds() const;
};
std::unique_ptr<PermissionGuidePlatform> createPermissionGuidePlatform();
std::optional<PermissionGuideWindow>
selectPermissionGuideWindow(const QVector<PermissionGuideWindow>& windows, quint32 previous);
QRect permissionGuidePlacement(const std::optional<QRectF>& window, const QVector<QRect>& screens,
                               int height);

class PermissionGuideController final : public QObject {
    Q_OBJECT
  public:
    explicit PermissionGuideController(AppPermissionService& service, QObject* parent = nullptr);
    PermissionGuideController(AppPermissionService& service,
                              std::unique_ptr<PermissionGuidePlatform> platform,
                              QObject* parent = nullptr);
    ~PermissionGuideController() override;
    void showFor(AppPermission permission);
    void dismiss();
    void updatePlacement();
    bool tracking() const {
        return m_timer.isActive();
    }
    PermissionGuideWidget* widget() const {
        return m_widget;
    }

  private:
    void setVisible(bool visible);
    void updateContent();
    AppPermissionService& m_service;
    std::unique_ptr<PermissionGuidePlatform> m_platform;
    PermissionGuideWidget* m_widget = nullptr;
    QTimer m_timer;
    qint64 m_discoveryStarted = 0;
    std::optional<AppPermission> m_permission;
    quint32 m_windowId = 0;
    bool m_foundWindow = false;
    bool m_sawSettings = false;
    bool m_fallback = false;
    bool m_dismissAfterInteraction = false;
};
} // namespace snow_shot::presentation
#endif
