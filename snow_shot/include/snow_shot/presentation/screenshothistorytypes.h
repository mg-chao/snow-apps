#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTHISTORYTYPES_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTHISTORYTYPES_H

#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"
#include "snow_shot/presentation/screenshotselectionparams.h"
#include "snow_shot/storage/capturehistorytypes.h"
#include "snow_shot/storage/preparedpngimage.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/screenshotrecognitionresults.h"

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
    std::optional<QPoint> sourceCanvasOrigin{};
    std::optional<QRect> sourceCanvasRect{};
    bool canvasUsesPoints = false;
    qreal backingScale = 0.0;
    quint32 nativeDisplayId = 0;
};

struct ScreenshotHistoryEntry {
    snow_shot::storage::CaptureHistoryContentKind contentKind =
        snow_shot::storage::CaptureHistoryContentKind::ScreenshotSession;
    QString id;
    QDateTime createdUtc;
    QRect recordedCanvasBounds;
    ScreenshotSelectionParams selection;
    QByteArray canvasHistory;
    QVector<ScreenshotHistoryDisplay> displays;
    std::optional<QImage> resultImage;
    std::optional<snow_shot::storage::PreparedPngImage> preparedResultImage;
    snow_shot::storage::CaptureHistorySource source =
        snow_shot::storage::CaptureHistorySource::CopiedToClipboard;
    // Absent on entries restored from records persisted before the scrolling marker existed.
    std::optional<bool> scrolling{};
    std::optional<snow_shot::storage::CaptureHistoryDesktopGeometry> desktopGeometry{};
    bool intelligentSelectionMode = false;
    bool persistent = true;
    std::optional<ScreenshotIntelligentSelectionModel> liveIntelligentSelection;
    // Transient MCP handoff state; historical on-disk records remain unchanged.
    QByteArray documentSession;
    ScreenshotClipboardOriginalContent originalContent;
    ScreenshotRecognitionResults recognitionResults;
    QString tool;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTHISTORYTYPES_H
