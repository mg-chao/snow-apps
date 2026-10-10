#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONMODEL_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONMODEL_H

#include "snow_shot/network/snowshotapiclient.h"

// These selection identities are local preferences, never chat model names.
[[nodiscard]] inline QString screenshotDedicatedRecognitionModelId() {
    return QStringLiteral("dedicated");
}

[[nodiscard]] inline QString screenshotDefaultVisionRecognitionModelId() {
    return QStringLiteral("snow-shot:vision");
}

struct ScreenshotRecognitionModelState {
    QString selection;
    QVector<SnowShotChatModel> models;
    bool loading = false;
    QString error;
    QString effectiveModel;

    [[nodiscard]] bool operator==(const ScreenshotRecognitionModelState&) const = default;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONMODEL_H
