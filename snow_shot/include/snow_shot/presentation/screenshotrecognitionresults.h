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

struct ScreenshotTableRecognitionEntry {
    QString model;
    QString modelFingerprint;
    int promptVersion = 1;
    SnowShotTableResult result;
    std::optional<QString> draftHtml;

    [[nodiscard]] bool isValid() const {
        return !model.isEmpty() && model.size() <= 256 && modelFingerprint.size() <= 256 &&
               promptVersion == 1 && result.succeeded() && result.html.size() <= 4 * 1024 * 1024 &&
               (!draftHtml || draftHtml->size() <= 4 * 1024 * 1024);
    }
};

struct ScreenshotLatexRecognitionEntry {
    QString model;
    QString modelFingerprint;
    int promptVersion = 1;
    SnowShotLatexResult result;
    std::optional<QString> draft;

    [[nodiscard]] bool isValid() const {
        return !model.isEmpty() && model.size() <= 256 && modelFingerprint.size() <= 256 &&
               promptVersion == 1 && result.succeeded() && result.latex.size() <= 4 * 1024 * 1024 &&
               (!draft || draft->size() <= 4 * 1024 * 1024);
    }
};

struct ScreenshotRecognitionResults {
    QString key;
    std::optional<SnowShotLatexResult> latex;
    // An empty draft is a valid edit; the successful recognition remains the reset baseline.
    std::optional<QString> latexDraft;
    bool visibleLatex = false;
    std::optional<ScreenshotOcrRecognitionResult> text;
    std::optional<SnowShotTableResult> table;
    std::optional<ScreenshotQrRecognitionResult> qr;
    std::shared_ptr<ScreenshotOcrPresentation> translatedText;
    QVector<ScreenshotImageConversionEntry> conversions;
    std::optional<SnowShotImageConversionFormat> visibleConversion;
    QVector<ScreenshotTableRecognitionEntry> tableEntries;
    QVector<ScreenshotLatexRecognitionEntry> latexEntries;
    QString tableModelSelection;
    QString latexModelSelection;
    // Transient admission hints for bounded snapshots; selections retain their semantic IDs.
    QString tableEffectiveModel;
    QString latexEffectiveModel;

    [[nodiscard]] bool isEmpty() const {
        return !latex.has_value() && !text.has_value() && !table.has_value() && !qr.has_value() &&
               conversions.isEmpty() && tableEntries.isEmpty() && latexEntries.isEmpty() &&
               tableModelSelection.isEmpty() && latexModelSelection.isEmpty();
    }

    [[nodiscard]] bool isValidFor(const QString& targetKey) const {
        return !key.isEmpty() && key == targetKey;
    }
};

inline void sanitizeEditionRecognitionResults(ScreenshotRecognitionResults& results) {
    using namespace snow_shot::app;
    if constexpr (!edition::tableRecognition) {
        results.table.reset();
        results.tableEntries.clear();
        results.tableModelSelection.clear();
        results.tableEffectiveModel.clear();
    }
    if constexpr (!edition::qrRecognition)
        results.qr.reset();
    if constexpr (!edition::latexRecognition) {
        results.latex.reset();
        results.latexDraft.reset();
        results.visibleLatex = false;
        results.latexEntries.clear();
        results.latexModelSelection.clear();
        results.latexEffectiveModel.clear();
    }
    if constexpr (!edition::imageConversion) {
        results.conversions.clear();
        results.visibleConversion.reset();
    }
    if constexpr (!edition::textTranslation)
        results.translatedText.reset();
}

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONRESULTS_H
