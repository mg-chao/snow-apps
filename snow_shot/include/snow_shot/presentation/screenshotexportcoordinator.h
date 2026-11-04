#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTEXPORTCOORDINATOR_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTEXPORTCOORDINATOR_H

#include "snow_shot/presentation/screenshotclipboardservice.h"

#include <QImage>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QQueue>
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
    File,
    Clipboard,
    Internal,
};

struct ScreenshotExportTaskResult final {
    ScreenshotExportFailureStage failureStage = ScreenshotExportFailureStage::None;
    QString error;
    QImage image;
    QString savedPath;
    std::shared_ptr<ScreenshotClipboardPayload> clipboardPayload;
    QByteArray encodedSha256;
    qint64 encodedByteCount = -1;

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
    // Raise queued work without submitting a second job or interrupting a running worker.
    void promoteToForeground() const;
    [[nodiscard]] bool isValid() const;
    [[nodiscard]] bool isCancellationRequested() const;

  private:
    friend class ScreenshotExportCoordinator;
    ScreenshotExportJobHandle(std::shared_ptr<std::atomic_bool> cancelled,
                              std::function<void()> cancelQueued,
                              std::function<void()> promoteQueued);
    std::shared_ptr<std::atomic_bool> m_cancelled;
    std::function<void()> m_cancelQueued;
    std::function<void()> m_promoteQueued;
};

class ScreenshotExportQueue;

class ScreenshotExportCoordinator final : public QObject {
  public:
    enum class Priority { Background, Foreground };
    using Work = std::function<ScreenshotExportTaskResult(const ScreenshotExportCancellation&)>;
    using Completion = std::function<void(ScreenshotExportTaskResult)>;

    explicit ScreenshotExportCoordinator(QObject* parent = nullptr);
    // The deadline bounds how long shutdown() waits for workers that ignore
    // their cancellation tokens; tests shrink it to exercise abandonment.
    ScreenshotExportCoordinator(int shutdownTimeoutMilliseconds, QObject* parent = nullptr);
    ScreenshotExportCoordinator(int shutdownTimeoutMilliseconds, qint64 maximumWorkingBytes,
                                QObject* parent = nullptr);
    ~ScreenshotExportCoordinator() override;

    [[nodiscard]] static ScreenshotExportCoordinator& shared();
    [[nodiscard]] ScreenshotExportJobHandle submit(QObject* receiver, Priority priority, Work work,
                                                   Completion completion,
                                                   qint64 estimatedWorkingBytes = 0);
    // Estimates bound concurrent scratch/output preparation, separately from cached or externally
    // owned results. A job larger than the budget runs alone so existing large exports still work.
    [[nodiscard]] qint64 activeWorkingBytes() const;
    [[nodiscard]] int pendingJobCount() const;
    [[nodiscard]] bool acceptingSubmissions() const;
    void shutdown();

  private:
    friend class ScreenshotExportQueue;
    void dispatchQueues();
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

// GUI-thread FIFO admission for independent requests. Only two jobs per queue enter the
// coordinator at once; the remaining work waits for capacity without decoding images.
// cancel(), destruction and coordinator shutdown discard pending work and completions.
class ScreenshotExportQueue final : public QObject {
  public:
    explicit ScreenshotExportQueue(ScreenshotExportCoordinator& coordinator,
                                   QObject* parent = nullptr);
    ~ScreenshotExportQueue() override;
    [[nodiscard]] bool enqueue(ScreenshotExportCoordinator::Priority priority,
                               ScreenshotExportCoordinator::Work work,
                               ScreenshotExportCoordinator::Completion completion,
                               qint64 estimatedWorkingBytes = 0);
    void cancel();
    [[nodiscard]] bool active() const;

  private:
    friend class ScreenshotExportCoordinator;
    void dispatch();
    struct Request {
        ScreenshotExportCoordinator::Priority priority;
        ScreenshotExportCoordinator::Work work;
        ScreenshotExportCoordinator::Completion completion;
        qint64 estimatedWorkingBytes = 0;
    };
    QPointer<ScreenshotExportCoordinator> m_coordinator;
    QQueue<Request> m_requests;
    QHash<quint64, ScreenshotExportJobHandle> m_jobs;
    quint64 m_generation = 0;
    quint64 m_nextId = 0;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTEXPORTCOORDINATOR_H
