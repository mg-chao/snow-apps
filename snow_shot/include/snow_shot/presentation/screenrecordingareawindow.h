#ifndef SNOW_SHOT_PRESENTATION_SCREENRECORDINGAREAWINDOW_H
#define SNOW_SHOT_PRESENTATION_SCREENRECORDINGAREAWINDOW_H

#include "snow_shot/presentation/screenshottoolpalette.h"

#include <QRect>
#include <QRectF>
#include <QWidget>

class ScreenRecordingAreaWindow final : public QWidget {
    Q_OBJECT

  public:
    explicit ScreenRecordingAreaWindow(QWidget* parent = nullptr);

    void setPhysicalRegion(const QRect& region);
    QRect physicalRegion() const;
    void setRecordingState(ScreenshotToolPalette::RecordingState state);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QRect m_physicalRegion;
    QRectF m_frameRect;
    QRectF m_selectionRect;
    ScreenshotToolPalette::RecordingState m_state = ScreenshotToolPalette::RecordingState::Idle;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENRECORDINGAREAWINDOW_H
