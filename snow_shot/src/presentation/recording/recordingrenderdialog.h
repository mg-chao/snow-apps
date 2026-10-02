#ifndef SNOW_SHOT_RECORDINGRENDERDIALOG_H
#define SNOW_SHOT_RECORDINGRENDERDIALOG_H

#include "snow_recording.h"
#include "widgets/modal.h"
#include <functional>

class QLabel;
namespace adqt::widgets {
class AdProgress;
class AdButton;
} // namespace adqt::widgets

// Shared presentation for recording renders and trimmed clip exports.
class RecordingRenderDialog final : public QObject {
  public:
    RecordingRenderDialog(QObject* parent, QScreen* screen, const QRect& anchorGeometry,
                          QWidget* windowOwner);
    void refresh();
    adqt::widgets::AdModal* modal = nullptr;
    bool retained = false;
    bool terminalSucceeded = false;
    bool cancelRequested = false;
    bool busy = false;
    double percent = 0;
    uint32_t stage = SNOW_RECORDING_RENDER_STAGE_PREPARE;
    QString path;
    QString lastError;
    std::function<void()> cancel;
    std::function<void()> retry;
    std::function<void()> keep;
    std::function<void()> discard;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    QLabel* status = nullptr;
    QLabel* details = nullptr;
    adqt::widgets::AdProgress* progress = nullptr;
    adqt::widgets::AdButton* cancelButton = nullptr;
    adqt::widgets::AdButton* retryButton = nullptr;
    adqt::widgets::AdButton* keepButton = nullptr;
    adqt::widgets::AdButton* discardButton = nullptr;
};

#endif
