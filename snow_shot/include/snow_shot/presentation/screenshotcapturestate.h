#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCAPTURESTATE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCAPTURESTATE_H

#include "snow_shot/presentation/screenshottypes.h"

struct ScreenshotCaptureState {
    ScreenshotSessionState sessionState = ScreenshotSessionState::IdleCold;
    quint64 sessionId = 0;
    bool captureInProgress = false;
    bool layoutDirty = false;
    bool restoreOriginalScreenColors = true;
    bool captureCursor = false;
    bool presentationSuppressed = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCAPTURESTATE_H
