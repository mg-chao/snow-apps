#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDCOPYSERVICE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDCOPYSERVICE_H

#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"

#include <QByteArray>
#include <QImage>
#include <QRectF>
#include <QSize>

#include <functional>
#include <memory>

class QObject;

struct ScreenshotPinnedViewportCopyRequest {
    QByteArray documentSession;
    QImage backgroundImage;
    QRectF backgroundCanvasRect;
    QSize contentPixelSize;
    ScreenshotResultStyle resultStyle;
};

class ScreenshotPinnedCopyService final {
  public:
    using Callback = std::function<void(ScreenshotClipboardPayload)>;
    using ImageCallback = std::function<void(QImage)>;

    ScreenshotPinnedCopyService();
    ~ScreenshotPinnedCopyService();

    ScreenshotPinnedCopyService(const ScreenshotPinnedCopyService&) = delete;
    ScreenshotPinnedCopyService& operator=(const ScreenshotPinnedCopyService&) = delete;

    [[nodiscard]] bool requestCurrentViewport(ScreenshotPinnedViewportCopyRequest request,
                                              QObject* receiver, Callback callback);
    [[nodiscard]] bool requestCurrentImage(ScreenshotPinnedViewportCopyRequest request,
                                           QObject* receiver, ImageCallback callback);
    [[nodiscard]] bool requestOriginalImage(QImage image, QObject* receiver, Callback callback);
    void invalidate();

  private:
    enum class RequestKind {
        None,
        CurrentViewport,
        CurrentImage,
        OriginalImage,
    };

    [[nodiscard]] bool beginRequest(RequestKind kind, quint64* generation);

    struct State;

    ScreenshotExportJobHandle m_job;
    std::shared_ptr<State> m_state;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDCOPYSERVICE_H
