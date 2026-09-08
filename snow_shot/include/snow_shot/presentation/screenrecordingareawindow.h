#ifndef SNOW_SHOT_PRESENTATION_SCREENRECORDINGAREAWINDOW_H
#define SNOW_SHOT_PRESENTATION_SCREENRECORDINGAREAWINDOW_H

#include "snow_shot/presentation/screenshottoolpalette.h"

#include <QRect>
#include <QRectF>
#include <QWidget>

#include <memory>

class QEvent;
class QShowEvent;
class SnowCanvasRuntime;
class SnowCanvasWidget;

class ScreenRecordingAreaWindow final : public QWidget {
    Q_OBJECT

  public:
    enum class InputMode {
        PassThrough,
        Drawing,
    };

    explicit ScreenRecordingAreaWindow(QWidget* parent = nullptr);
    ~ScreenRecordingAreaWindow() override;

    void setPhysicalRegion(const QRect& region);
    void setRecordingState(ScreenshotToolPalette::RecordingState state);
    void setInputMode(InputMode mode);
    [[nodiscard]] InputMode inputMode() const;
    void setDrawingBlocked(bool blocked);
    [[nodiscard]] bool drawingBlocked() const;
    [[nodiscard]] SnowCanvasWidget* canvas() const;
    [[nodiscard]] QRect canvasGeometry() const;

  signals:
    void drawingDeactivationRequested();
    void drawingWheelRequested(int direction);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void applyInputMode();
    void applyNativePassThrough(bool enabled);

    QRectF m_frameRect;
    QRectF m_selectionRect;
    QRect m_physicalRegion;
    qreal m_paddingWidth = 0.0;
    ScreenshotToolPalette::RecordingState m_state = ScreenshotToolPalette::RecordingState::Idle;
    InputMode m_inputMode = InputMode::PassThrough;
    bool m_drawingBlocked = false;
    bool m_gestureInProgress = false;
    std::unique_ptr<SnowCanvasRuntime> m_canvasRuntime;
    SnowCanvasWidget* m_canvas = nullptr;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENRECORDINGAREAWINDOW_H
