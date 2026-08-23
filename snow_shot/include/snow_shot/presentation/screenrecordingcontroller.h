#ifndef SNOW_SHOT_PRESENTATION_SCREENRECORDINGCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_SCREENRECORDINGCONTROLLER_H

#include "snow_shot/presentation/screenshottoolpalette.h"

#include <QObject>
#include <QRect>

#include <memory>

class QEvent;

class ScreenRecordingController final : public QObject {
    Q_OBJECT

  public:
    explicit ScreenRecordingController(QObject* parent = nullptr);
    ~ScreenRecordingController() override;

    void open(const QRect& physicalRegion);
    bool isOpen() const;
    bool isRecording() const;
    bool hasActiveWork() const;
    void startRecording();
    void stopRecordingAndCopyVideo();

  signals:
    void openChanged(bool open);
    void activeWorkChanged(bool active);

  private:
    bool eventFilter(QObject* watched, QEvent* event) override;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENRECORDINGCONTROLLER_H
