#ifndef SNOW_SHOT_PRESENTATION_SCREENRECORDINGTOOLBARWINDOW_H
#define SNOW_SHOT_PRESENTATION_SCREENRECORDINGTOOLBARWINDOW_H

#include "snow_shot/presentation/screenshotfloatingtoolpalettewindow.h"

#include <QRect>

class ScreenRecordingToolbarWindow final : public ScreenshotFloatingToolPaletteWindow {
    Q_OBJECT

  public:
    explicit ScreenRecordingToolbarWindow(QWidget* parent = nullptr);

    void placeForPhysicalRegion(const QRect& physicalRegion);
    void showAndActivate();

  private:
    QRect m_physicalRegion;
    bool m_manuallyDragged = false;
    bool m_placing = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENRECORDINGTOOLBARWINDOW_H
