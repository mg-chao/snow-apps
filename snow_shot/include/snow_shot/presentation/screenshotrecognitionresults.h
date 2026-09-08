#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONRESULTS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONRESULTS_H

#include "snow_shot/network/snowshotapiclient.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotqrrecognitionservice.h"
#include "snow_shot/presentation/screenshotimageconversion.h"

#include <QString>

#include <optional>
#include <memory>

struct ScreenshotRecognitionResults {
    QString key;
    std::optional<ScreenshotOcrRecognitionResult> text;
    std::optional<SnowShotTableResult> table;
    std::optional<ScreenshotQrRecognitionResult> qr;
    std::shared_ptr<ScreenshotOcrPresentation> translatedText;
    QVector<ScreenshotImageConversionEntry> conversions;
    std::optional<SnowShotImageConversionFormat> visibleConversion;

    [[nodiscard]] bool isEmpty() const {
        return !text.has_value() && !table.has_value() && !qr.has_value() && conversions.isEmpty();
    }

    [[nodiscard]] bool isValidFor(const QString& targetKey) const {
        return !key.isEmpty() && key == targetKey;
    }
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONRESULTS_H
