#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONRESULTS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONRESULTS_H

#include "snow_shot/network/snowshotapiclient.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotqrrecognitionservice.h"

#include <QString>

#include <optional>

struct ScreenshotRecognitionResults {
    QString key;
    std::optional<ScreenshotOcrRecognitionResult> text;
    std::optional<SnowShotTableResult> table;
    std::optional<ScreenshotQrRecognitionResult> qr;

    [[nodiscard]] bool isEmpty() const {
        return !text.has_value() && !table.has_value() && !qr.has_value();
    }

    [[nodiscard]] bool isValidFor(const QString& targetKey) const {
        return !key.isEmpty() && key == targetKey;
    }
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONRESULTS_H
