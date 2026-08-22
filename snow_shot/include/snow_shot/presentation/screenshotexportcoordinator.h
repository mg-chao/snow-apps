#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTEXPORTCOORDINATOR_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTEXPORTCOORDINATOR_H

#include "snow_shot/presentation/screenshotclipboardservice.h"

#include <QImage>
#include <QObject>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>

enum class ScreenshotExportFailureStage {
    None,
    Queue,
    Cancelled,
    Source,
    Render,
    Encode,
    File,
    Clipboard,
    Presentation,
    Internal,
};

struct ScreenshotExportTaskResult final {
    ScreenshotExportFailureStage failureStage = ScreenshotExportFailureStage::None;
    QString error;
    QString warning;
    QImage image;
    QString savedPath;
    std::shared_ptr<ScreenshotClipboardPayload> clipboardPayload;

    [[nodiscard]] bool succeeded() const {
        return failureStage == ScreenshotExportFailureStage::None && error.isEmpty();
    }

    [[nodiscard]] static ScreenshotExportTaskResult failure(ScreenshotExportFailureStage stage,
                                                            QString errorMessage);
};

class ScreenshotExportCancellation final {
  public:
    [[nodiscard]] bool isCancellationRequested() const;

  private:
    friend class ScreenshotExportCoordinator;
    explicit ScreenshotExportCancellation(std::shared_ptr<std::atomic_bool> cancelled);
    std::shared_ptr<std::atomic_bool> m_cancelled;
};

class ScreenshotExportJobHandle final {
  public:
    ScreenshotExportJobHandle() = default;

    void cancel() const;
    [[nodiscard]] bool isValid() const;
    [[nodiscard]] bool isCancellationRequested() const;

  private:
    friend class ScreenshotExportCoordinator;
    explicit ScreenshotExportJobHandle(std::shared_ptr<std::atomic_bool> cancelled);
    std::shared_ptr<std::atomic_bool> m_cancelled;
};

class ScreenshotExportCoordinator final : public QObject {
  public:
    enum class Priority { Background, Foreground };
    using Work = std::function<ScreenshotExportTaskResult(const ScreenshotExportCancellation&)>;
    using Completion = std::function<void(ScreenshotExportTaskResult)>;

    explicit ScreenshotExportCoordinator(QObject* parent = nullptr);
    ~ScreenshotExportCoordinator() override;

    [[nodiscard]] static ScreenshotExportCoordinator& shared();
    [[nodiscard]] ScreenshotExportJobHandle submit(QObject* receiver, Priority priority, Work work,
                                                   Completion completion);
    [[nodiscard]] int pendingJobCount() const;
    void shutdown();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTEXPORTCOORDINATOR_H
