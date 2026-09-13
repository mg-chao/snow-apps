#pragma once

#include <QObject>
#include <QImage>
#include <QPointer>
#include <QRectF>
#include <QElapsedTimer>
#include <functional>
#include "snow_draw_engine_qt/snow_canvas_types.h"

class QWidget;
class QTimer;
class SnowCanvasWidget;
class ScreenshotAutoFilterVisual;

class ScreenshotAutoFilterController final : public QObject {
    Q_OBJECT
  public:
    using ImageCompletion = std::function<void(QImage)>;
    using Source = std::function<void(ImageCompletion)>;
    using Completion = std::function<void(QList<SnowCanvasAutoFilterRegion>, QString)>;
    using Detector = std::function<void(QImage, Completion)>;
    ScreenshotAutoFilterController(std::function<QRectF()> bounds, Source source,
                                   QObject* parent = nullptr, Detector detector = {},
                                   std::function<qint64()> clock = {});
    ~ScreenshotAutoFilterController() override;
    void attachCanvas(SnowCanvasWidget* canvas);
    void validate();
    void resetSession();
    void refresh();
    bool available() const;
    void fillCategory(const QString& category);
    bool detecting() const {
        return m_busy;
    }
    qint64 elapsed() const {
        return m_clock ? m_clock() : m_elapsed.elapsed();
    }
    QRectF currentBounds() const {
        return m_bounds();
    }
    const QList<SnowCanvasAutoFilterRegion>& flashRegions() const {
        return m_flashRegions;
    }
    bool flashing() const {
        return m_flashUntil > elapsed();
    }
  signals:
    void availabilityChanged(bool available);
    void detectionFailed(const QString& message);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void updateAvailability();
    SnowCanvasWidget* canvas() const;
    bool active() const;
    void finish(quint64 session, quint64 generation, QRectF bounds, QSize pixels,
                QList<SnowCanvasAutoFilterRegion> regions, const QString& error);
    std::function<QRectF()> m_bounds;
    Source m_source;
    Detector m_detector;
    QList<QPointer<SnowCanvasWidget>> m_canvases;
    QList<QPointer<QWidget>> m_visuals;
    QTimer* m_timer = nullptr;
    QElapsedTimer m_elapsed;
    std::function<qint64()> m_clock;
    bool m_available = false;
    QList<SnowCanvasAutoFilterRegion> m_flashRegions;
    qint64 m_flashUntil = 0;
    quint64 m_session = 0;
    bool m_busy = false;
};
