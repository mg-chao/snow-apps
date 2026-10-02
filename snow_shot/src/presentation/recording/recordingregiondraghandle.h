#ifndef SNOW_SHOT_PRESENTATION_RECORDING_RECORDINGREGIONDRAGHANDLE_H
#define SNOW_SHOT_PRESENTATION_RECORDING_RECORDINGREGIONDRAGHANDLE_H

#include "../components/overlaycontrolbutton.h"

class RecordingRegionDragHandle final : public snow_shot::presentation::OverlayControlButton {
    Q_OBJECT

  public:
    explicit RecordingRegionDragHandle(QWidget* parent);
    [[nodiscard]] bool isDragging() const {
        return m_dragging;
    }
    // End capture without publishing a second finish or cancellation to the owner.
    void stopDragging();

  signals:
    void dragStarted(const QPointF& globalPosition);
    void dragMoved(const QPointF& globalPosition);
    void dragFinished(const QPointF& globalPosition);
    void dragCancelled();

  protected:
    bool event(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

  private:
    void cancelDrag();
    void retranslateUi();
    bool m_dragging = false;
};

#endif // SNOW_SHOT_PRESENTATION_RECORDING_RECORDINGREGIONDRAGHANDLE_H
