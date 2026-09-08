#ifndef SNOW_SHOT_PRESENTATION_SCREENRECORDINGCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_SCREENRECORDINGCONTROLLER_H

#include "snow_shot/presentation/screenshottoolpalette.h"

#include <QObject>
#include <QRect>

#include <memory>

class ScreenRecordingController final : public QObject {
    Q_OBJECT

  public:
    explicit ScreenRecordingController(QObject* parent = nullptr);
    ~ScreenRecordingController() override;

    void open(const QRect& physicalRegion);
    bool isOpen() const;
    bool isRecording() const;
    void startRecording();
    void stopRecordingAndCopy();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENRECORDINGCONTROLLER_H
