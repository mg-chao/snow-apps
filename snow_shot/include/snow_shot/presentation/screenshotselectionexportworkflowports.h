#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEXPORTWORKFLOWPORTS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEXPORTWORKFLOWPORTS_H

#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotimagesource.h"
#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_shot/presentation/screenshotselectionparams.h"

#include <QColor>
#include <QImage>
#include <QPointer>
#include <QRect>

#include <functional>
#include <optional>

class QObject;
class QScreen;

struct ScreenshotPinnedSelectionRequest {
    ScreenshotImageSource imageSource;
    ScreenshotPinnedImageGeometry geometry;
    QRectF contentCanvasRect;
    QRectF surfaceCanvasRect;
    ScreenshotResultStyle resultStyle;
    QSize fullResolutionScaleBasis;
    QPointer<QScreen> screen;

    [[nodiscard]] bool isValid() const {
        return imageSource.isValid() && geometry.nativeGeometry.isValid() &&
               !geometry.nativeGeometry.isEmpty() && contentCanvasRect.isValid() &&
               !contentCanvasRect.isEmpty() && surfaceCanvasRect.isValid() &&
               !surfaceCanvasRect.isEmpty() && surfaceCanvasRect.contains(contentCanvasRect) &&
               fullResolutionScaleBasis.isValid() && !fullResolutionScaleBasis.isEmpty() &&
               screen != nullptr;
    }
};

struct ScreenshotSelectionClipboardResult {
    QImage image;
    ScreenshotClipboardPayload payload;

    [[nodiscard]] bool isValid() const {
        return !image.isNull() && !image.size().isEmpty() && payload.isValid();
    }
};

class ScreenshotSelectionImageComposerPort {
  public:
    using ImageCallback = std::function<void(QImage)>;
    using ClipboardCallback = std::function<void(ScreenshotSelectionClipboardResult)>;
    using PinRequestCallback = std::function<void(ScreenshotPinnedSelectionRequest)>;

    virtual ~ScreenshotSelectionImageComposerPort() = default;

    [[nodiscard]] virtual bool requestSelectionResult(const QRect& selection,
                                                      const ScreenshotResultStyle& style,
                                                      QObject* receiver,
                                                      ImageCallback callback) = 0;
    [[nodiscard]] virtual bool requestSelectionClipboard(const QRect& selection,
                                                         const ScreenshotResultStyle& style,
                                                         QObject* receiver,
                                                         ClipboardCallback callback) = 0;
    [[nodiscard]] virtual std::optional<ScreenshotPinnedSelectionRequest>
    preparePinnedSelection(const QRect& selection, const ScreenshotResultStyle& style) const = 0;
    [[nodiscard]] virtual bool schedulePinnedSelection(ScreenshotPinnedSelectionRequest request,
                                                       QObject* receiver,
                                                       PinRequestCallback callback) = 0;
};

class ScreenshotSelectionExportDestinationPort {
  public:
    using ClipboardCompletion = std::function<void(bool)>;

    virtual ~ScreenshotSelectionExportDestinationPort() = default;

    [[nodiscard]] virtual bool publishClipboard(QObject* receiver,
                                                ScreenshotClipboardPayload payload,
                                                ClipboardCompletion completion) = 0;

    [[nodiscard]] virtual bool
    presentPinnedSelection(const ScreenshotPinnedSelectionRequest& request) = 0;
};

class ScreenshotSelectionParamsStorePort {
  public:
    virtual ~ScreenshotSelectionParamsStorePort() = default;

    virtual void setPreviousSelectionParams(const ScreenshotSelectionParams& params) = 0;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEXPORTWORKFLOWPORTS_H
