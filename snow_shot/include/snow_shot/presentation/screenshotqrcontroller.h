#pragma once

#include "snow_shot/app/edition.h"

#include "snow_shot/presentation/screenshotimagesource.h"
#include "snow_shot/presentation/screenshotqrrecognitionservice.h"

#include <QPainterPath>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <atomic>

class QWidget;
class SnowCanvasWidget;
class ScreenshotQrPopover;
class ScreenshotQrMarker;

// Ephemeral editor UI. Never becomes part of the canvas document or export renderer.
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
class ScreenshotQrController final : public QObject {
    Q_OBJECT
  public:
    struct Snapshot {
        QList<ScreenshotImageLayer> layers;
        QRectF bounds;
        QSize pixelSize;
        QPainterPath selection;
    };
    struct Actions {
        std::function<bool(const QUrl&)> openUrl;
        std::function<void()> closeEditor;
    };
    explicit ScreenshotQrController(ScreenshotQrRecognitionPort& recognition, Actions actions,
                                    QObject* parent = nullptr);
    ~ScreenshotQrController() override;
    void attachCanvas(SnowCanvasWidget* canvas);
    void recognize(Snapshot snapshot);
    // Updates presentation only; temporary selection gestures retain recognition results.
    void synchronize(const QPainterPath& selection, bool editing, bool moveActive);
    void invalidate();
    void setEnabled(bool enabled);
    void setSuspended(bool suspended);
    void setMarkersVisible(bool visible);
    void dismissPopover();
    bool available() const;
    bool ownsInput(const QWidget* widget) const;
    bool markersVisible() const {
        return m_visible;
    }
    bool busy() const {
        return m_busy;
    }
    QString error() const {
        return m_error;
    }
    static QUrl webUrl(const QString& text);
    static QImage prepareImage(const Snapshot& snapshot, const std::atomic_bool& cancelled);

  signals:
    void stateChanged();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void finish(quint64 generation, QSize pixels, ScreenshotQrRecognitionResult result);
    void refreshMarkers();
    void showPopover(ScreenshotQrMarker* marker, bool focus);
    void updatePopoverGeometry();
    void scheduleDismiss();
    void clearMarkers();
    QPointer<ScreenshotQrRecognitionPort> m_recognition;
    Actions m_actions;
    QList<QPointer<SnowCanvasWidget>> m_canvases;
    QList<QPointer<ScreenshotQrMarker>> m_markers;
    QPointer<ScreenshotQrPopover> m_popover;
    QPointer<ScreenshotQrMarker> m_hoverMarker;
    QPointer<ScreenshotQrMarker> m_popoverMarker;
    QTimer m_hoverTimer;
    QTimer m_dismissTimer;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    ScreenshotQrRecognitionPort::RequestToken m_request = 0;
    quint64 m_generation = 0;
    QRectF m_bounds;
    QPainterPath m_sourceSelection;
    QPainterPath m_selection;
    QList<ScreenshotQrDetection> m_detections;
    QString m_error;
    bool m_enabled = true;
    bool m_busy = false;
    bool m_visible = true;
    bool m_moveActive = false;
    bool m_editing = false;
    bool m_suspended = false;
};

#endif // SNOW_SHOT_ENABLE_QR_RECOGNITION
