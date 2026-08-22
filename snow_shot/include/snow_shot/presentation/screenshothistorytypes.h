#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTHISTORYTYPES_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTHISTORYTYPES_H

#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"
#include "snow_shot/presentation/screenshotselectionparams.h"
#include "snow_shot/storage/capturehistorytypes.h"

#include <QByteArray>
#include <QDateTime>
#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>

#include <optional>

struct ScreenshotHistoryDisplay {
    QString stableId;
    QString name;
    QImage image;
};

struct ScreenshotHistoryEntry {
    QString id;
    QDateTime createdUtc;
    QRect recordedCanvasBounds;
    ScreenshotSelectionParams selection;
    QByteArray canvasHistory;
    QVector<ScreenshotHistoryDisplay> displays;
    std::optional<QImage> resultImage;
    snow_shot::storage::CaptureHistorySource source =
        snow_shot::storage::CaptureHistorySource::CopiedToClipboard;
    bool intelligentSelectionMode = false;
    bool persistent = true;
    std::optional<ScreenshotIntelligentSelectionModel> liveIntelligentSelection;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTHISTORYTYPES_H
