#ifndef SNOW_SHOT_PRESENTATION_SCREENRECORDINGCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_SCREENRECORDINGCONTROLLER_H

#include "snow_shot/presentation/screenshottoolpalette.h"

#include <QObject>
#include <QRect>

#include <memory>
#include <functional>

class RecordingEffectsSource;

class ScreenRecordingController final : public QObject {
    Q_OBJECT

  public:
    explicit ScreenRecordingController(QObject* parent = nullptr);
    using EffectsSourceFactory = std::function<std::unique_ptr<RecordingEffectsSource>()>;
    ScreenRecordingController(EffectsSourceFactory effectsSourceFactory, QObject* parent = nullptr);
    ~ScreenRecordingController() override;

    // The region is in desktop points on macOS and physical pixels on Windows.
    void open(const QRect& recordingRegion);
    using PermissionCheck = std::function<bool(bool microphone, bool input, bool notify)>;
    void setPermissionCheck(PermissionCheck check);
    bool isOpen() const;
    bool isRecording() const;
    void startRecording();
    void stopRecordingAndCopy();
    void openRecordingFolder();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENRECORDINGCONTROLLER_H
