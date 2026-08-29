#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCAPTURECOORDINATOR_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCAPTURECOORDINATOR_H

#include "snow_shot/presentation/screenshottypes.h"

#include <QObject>

#include <memory>

class QThread;
class ScreenshotCaptureWorker;

class ScreenshotCaptureCoordinator final : public QObject {
    Q_OBJECT

  public:
    explicit ScreenshotCaptureCoordinator(QObject* parent = nullptr);
    ~ScreenshotCaptureCoordinator() override;

    bool hasWorker() const;
    void ensureWorker();
    void prepareAsync(quint64 requestId);
    void refreshLayoutAsync(quint64 requestId);
    void captureAsync(const ScreenshotCaptureRequest& request);
    void cancelActiveCapture();
    void shutdown();

  signals:
    void prepared(quint64 requestId, bool ok);
    void layoutRefreshed(quint64 requestId, bool ok);
    void captureFinished(ScreenshotCaptureResult result);

  private:
    template <typename Task> bool postWorkerTask(Task&& task);

    struct CancellationState;

    QThread* m_thread = nullptr;
    ScreenshotCaptureWorker* m_worker = nullptr;
    std::shared_ptr<CancellationState> m_activeCancellation;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCAPTURECOORDINATOR_H
