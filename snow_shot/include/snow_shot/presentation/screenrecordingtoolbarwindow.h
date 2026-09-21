#ifndef SNOW_SHOT_PRESENTATION_SCREENRECORDINGTOOLBARWINDOW_H
#define SNOW_SHOT_PRESENTATION_SCREENRECORDINGTOOLBARWINDOW_H

#include "snow_shot/presentation/screenshotfloatingtoolpalettewindow.h"

#include <QRect>

class ScreenRecordingToolbarWindow final : public ScreenshotFloatingToolPaletteWindow {
    Q_OBJECT

  public:
    explicit ScreenRecordingToolbarWindow(QWidget* parent = nullptr);

    void placeForRecordingRegion(const QRect& recordingRegion);
    void showAndActivate();
    void beginRegionInteraction();
    void endRegionInteraction(const QRect& recordingRegion);

  signals:
    void closeRequested();

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    QRect m_recordingRegion;
    bool m_manuallyDragged = false;
    bool m_placing = false;
    bool m_regionInteractionActive = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENRECORDINGTOOLBARWINDOW_H
