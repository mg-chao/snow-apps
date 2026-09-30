#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONRESULTS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONRESULTS_H

#include "snow_shot/app/edition.h"
#include "snow_shot/network/snowshotapiclient.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotqrrecognitionservice.h"
#include "snow_shot/presentation/screenshotimageconversion.h"

#include <QString>

#include <optional>
#include <memory>

struct ScreenshotRecognitionResults {
    QString key;
    std::optional<SnowShotLatexResult> latex;
    bool visibleLatex = false;
    std::optional<ScreenshotOcrRecognitionResult> text;
    std::optional<SnowShotTableResult> table;
    std::optional<ScreenshotQrRecognitionResult> qr;
    std::shared_ptr<ScreenshotOcrPresentation> translatedText;
    QVector<ScreenshotImageConversionEntry> conversions;
    std::optional<SnowShotImageConversionFormat> visibleConversion;

    [[nodiscard]] bool isEmpty() const {
        return !latex.has_value() && !text.has_value() && !table.has_value() && !qr.has_value() &&
               conversions.isEmpty();
    }

    [[nodiscard]] bool isValidFor(const QString& targetKey) const {
        return !key.isEmpty() && key == targetKey;
    }
};

inline void sanitizeEditionRecognitionResults(ScreenshotRecognitionResults& results) {
    using namespace snow_shot::app;
    if constexpr (!edition::tableRecognition)
        results.table.reset();
    if constexpr (!edition::qrRecognition)
        results.qr.reset();
    if constexpr (!edition::latexRecognition) {
        results.latex.reset();
        results.visibleLatex = false;
    }
    if constexpr (!edition::imageConversion) {
        results.conversions.clear();
        results.visibleConversion.reset();
    }
    if constexpr (!edition::textTranslation)
        results.translatedText.reset();
}

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONRESULTS_H
