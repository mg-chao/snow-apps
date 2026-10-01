#ifndef SNOW_SHOT_RECORDINGRENDERJOB_H
#define SNOW_SHOT_RECORDINGRENDERJOB_H

#include "snow_recording.h"
#include <QJsonObject>
#include <QObject>
#include <QRect>
#include <functional>
#include <memory>

class QScreen;

// Owns one immutable source and its serial render attempts independently of the
// recording selection windows. All potentially blocking native disposal is off-thread.
class RecordingRenderJob final : public QObject {
  public:
    enum class Outcome { Succeeded, Kept, Discarded };
    RecordingRenderJob(SnowRecordingSource* source, bool showDialog, QScreen* screen,
                       QObject* parent, const QRect& anchorGeometry = {});
    ~RecordingRenderJob() override;
    void start();
    bool cancel();
    bool retry();
    bool release(bool discard);
    [[nodiscard]] QJsonObject state() const;
    [[nodiscard]] QString sourcePath() const;
    [[nodiscard]] QString error() const;
    std::function<void()> changed;
    std::function<void(Outcome)> finished;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif
