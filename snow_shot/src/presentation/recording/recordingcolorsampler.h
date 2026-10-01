#pragma once

#include "snow_shot/presentation/screenrecordingareawindow.h"

#include <QImage>
#include <QObject>
#include <QPointer>

#include <functional>
#include <memory>

class ScreenRecordingToolbarWindow;
class ScreenshotCanvasColorSamplerWindow;
namespace adqt::widgets {
class AdColorPicker;
}

namespace snow_shot::presentation::recording {

// Sampling owns input until the complete click or cancellation has been consumed.
// The desktop reader is replaceable so the recording interaction can run offscreen.
class RecordingColorSampler final : public QObject {
  public:
    using ReadPreview = std::function<QImage(const QPointF&)>;
    RecordingColorSampler(ScreenRecordingAreaWindow& area, ScreenRecordingToolbarWindow& toolbar,
                          ReadPreview readPreview = {});
    ~RecordingColorSampler() override;

    void cancel();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void begin(adqt::widgets::AdColorPicker* picker);
    QImage previewAt(const QPointF& globalPosition);
    bool owns(QWidget* widget) const;

    ScreenRecordingAreaWindow& m_area;
    ScreenRecordingToolbarWindow& m_toolbar;
    ReadPreview m_readPreview;
    std::unique_ptr<ScreenshotCanvasColorSamplerWindow> m_preview;
    QPointer<adqt::widgets::AdColorPicker> m_target;
    QMetaObject::Connection m_destroyedConnection;
    ScreenRecordingAreaWindow::InputMode m_previousInputMode =
        ScreenRecordingAreaWindow::InputMode::PassThrough;
    bool m_active = false;
    bool m_pressed = false;
    bool m_escapeRelease = false;
};

} // namespace snow_shot::presentation::recording
