#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTMESSAGESERVICE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTMESSAGESERVICE_H

#include <QHash>
#include <QPointer>
#include <QRectF>
#include <QString>

#include <functional>

class ScreenshotDisplaySession;
class ScreenshotGeometryMapper;
class ScreenshotSelectionModel;
class QWidget;

class ScreenshotMessageService final {
  public:
    ScreenshotMessageService(ScreenshotDisplaySession& displaySession,
                             ScreenshotGeometryMapper& geometry,
                             ScreenshotSelectionModel& selection,
                             std::function<QWidget*()> toolbarFallback = {});

    void warning(const QString& key, const QString& message,
                 const QRectF& canvasRect = {}, QWidget* preferredOwner = nullptr) const;
    void error(const QString& key, const QString& message,
               const QRectF& canvasRect = {}, QWidget* preferredOwner = nullptr) const;
    void loading(const QString& key, const QString& message,
                 const QRectF& canvasRect = {}, QWidget* preferredOwner = nullptr) const;
    void destroy(const QString& key) const;

    static void loadingFor(QWidget* owner, const QString& key, const QString& message);
    static void destroyFor(QWidget* owner, const QString& key);

  private:
    [[nodiscard]] QWidget* ownerFor(const QRectF& canvasRect, QWidget* preferredOwner) const;
    void rememberOwner(const QString& key, QWidget* owner) const;

    ScreenshotDisplaySession& m_displaySession;
    ScreenshotGeometryMapper& m_geometry;
    ScreenshotSelectionModel& m_selection;
    std::function<QWidget*()> m_toolbarFallback;
    mutable QHash<QString, QPointer<QWidget>> m_messageOwners;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTMESSAGESERVICE_H
