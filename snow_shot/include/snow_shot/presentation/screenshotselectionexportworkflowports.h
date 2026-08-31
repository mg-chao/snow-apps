#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEXPORTWORKFLOWPORTS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEXPORTWORKFLOWPORTS_H

#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_shot/presentation/screenshotrecognitionresults.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_shot/presentation/screenshotselectionparams.h"

#include <QColor>
#include <QImage>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QThread>
#include <QVector>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

class QObject;
class QScreen;
class ScreenshotExportService;

class ScreenshotPinnedSelectionResultHandle final {
  public:
    using Callback = std::function<void(bool, QImage)>;

    ScreenshotPinnedSelectionResultHandle() = default;
    ScreenshotPinnedSelectionResultHandle(const ScreenshotPinnedSelectionResultHandle&) = default;
    ScreenshotPinnedSelectionResultHandle& operator=(
        const ScreenshotPinnedSelectionResultHandle&) = default;
    ScreenshotPinnedSelectionResultHandle(ScreenshotPinnedSelectionResultHandle&&) noexcept =
        default;
    ScreenshotPinnedSelectionResultHandle& operator=(
        ScreenshotPinnedSelectionResultHandle&&) noexcept = default;
    ~ScreenshotPinnedSelectionResultHandle() = default;

    [[nodiscard]] bool subscribe(QObject* receiver, Callback callback) const {
        if (m_state == nullptr || receiver == nullptr || !callback) {
            return false;
        }
        bool completed = false;
        bool succeeded = false;
        QImage image;
        {
            const std::lock_guard lock(m_state->mutex);
            if (m_state->cancelled) {
                return false;
            }
            completed = m_state->completed;
            if (!completed) {
                m_state->subscribers.push_back(Subscriber{QPointer<QObject>(receiver),
                                                           std::move(callback)});
                return true;
            }
            succeeded = m_state->succeeded;
            image = m_state->image;
        }
        dispatch(receiver, std::move(callback), succeeded, image);
        return true;
    }

    void cancel() const {
        if (m_state != nullptr) {
            m_state->cancel();
        }
    }

    [[nodiscard]] bool isValid() const {
        if (m_state == nullptr) {
            return false;
        }
        return !m_state->isCancelled();
    }

  private:
    struct Subscriber {
        QPointer<QObject> receiver;
        Callback callback;
    };

    struct State {
        mutable std::mutex mutex;
        bool completed = false;
        bool cancelled = false;
        bool succeeded = false;
        QImage image;
        QVector<Subscriber> subscribers;

        [[nodiscard]] bool isCancelled() const {
            const std::lock_guard lock(mutex);
            return cancelled;
        }

        void cancel() {
            const std::lock_guard lock(mutex);
            if (!completed) {
                cancelled = true;
                subscribers.clear();
            }
        }

        void publish(bool success, QImage value) {
            QVector<Subscriber> pending;
            bool publishedSuccess = false;
            QImage publishedImage;
            {
                const std::lock_guard lock(mutex);
                if (completed || cancelled) {
                    return;
                }
                completed = true;
                succeeded = success && !value.isNull();
                image = std::move(value);
                pending = std::move(subscribers);
                subscribers.clear();
                publishedSuccess = succeeded;
                publishedImage = image;
            }
            for (Subscriber& subscriber : pending) {
                ScreenshotPinnedSelectionResultHandle::dispatch(
                    subscriber.receiver, std::move(subscriber.callback), publishedSuccess,
                    publishedImage);
            }
        }
    };

    static void dispatch(QObject* receiver, Callback callback, bool succeeded, const QImage& image) {
        if (receiver == nullptr || !callback) {
            return;
        }
        const QPointer<QObject> guardedReceiver(receiver);
        // Avoid an extra queued turn when publication and the subscriber share
        // a thread; cross-thread subscribers retain queued delivery.
        if (QThread::currentThread() == receiver->thread()) {
            if (!guardedReceiver.isNull()) {
                callback(succeeded, image);
            }
            return;
        }
        QMetaObject::invokeMethod(
            receiver,
            [guardedReceiver, callback = std::move(callback), succeeded, image]() mutable {
                if (!guardedReceiver.isNull()) {
                    callback(succeeded, image);
                }
            },
            Qt::QueuedConnection);
    }

    explicit ScreenshotPinnedSelectionResultHandle(std::shared_ptr<State> state)
        : m_state(std::move(state)) {}

    std::shared_ptr<State> m_state;

    friend class ScreenshotExportService;
};

struct ScreenshotPinnedSelectionRequest {
    QRectF contentCanvasRect;
    QRectF surfaceCanvasRect;
    QRect selection;
    ScreenshotResultStyle resultStyle;
    ScreenshotPinnedImageGeometry geometry;
    QSize fullResolutionScaleBasis;
    QPointer<QScreen> screen;
    ScreenshotRecognitionResults recognitionResults;

    [[nodiscard]] bool isPrepared() const {
        return !selection.isEmpty() && geometry.nativeGeometry.isValid() &&
               !geometry.nativeGeometry.isEmpty() && geometry.canvasSourceRect.isValid() &&
               !geometry.canvasSourceRect.isEmpty() && geometry.initialPhysicalSize.isValid() &&
               !geometry.initialPhysicalSize.isEmpty() &&
               contentCanvasRect.isValid() && !contentCanvasRect.isEmpty() &&
               surfaceCanvasRect.isValid() && !surfaceCanvasRect.isEmpty() &&
               surfaceCanvasRect.contains(contentCanvasRect) &&
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
    using PinRequestCallback =
        std::function<void(ScreenshotPinnedSelectionRequest,
                           ScreenshotPinnedSelectionResultHandle)>;

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
    using PinnedCompletion = std::function<void(bool, QImage)>;

    virtual ~ScreenshotSelectionExportDestinationPort() = default;

    [[nodiscard]] virtual bool publishClipboard(QObject* receiver,
                                                ScreenshotClipboardPayload payload,
                                                ClipboardCompletion completion) = 0;
    [[nodiscard]] virtual bool publishClipboard(QObject* receiver,
                                                ScreenshotClipboardPayload payload,
                                                ClipboardCompletion completion,
                                                quint64 publicationId) {
        Q_UNUSED(publicationId);
        return publishClipboard(receiver, std::move(payload), std::move(completion));
    }

    [[nodiscard]] virtual bool
    presentPinnedSelection(const ScreenshotPinnedSelectionRequest& request,
                           ScreenshotPinnedSelectionResultHandle result,
                           PinnedCompletion completion) = 0;
};

class ScreenshotSelectionParamsStorePort {
  public:
    virtual ~ScreenshotSelectionParamsStorePort() = default;

    virtual void setPreviousSelectionParams(const ScreenshotSelectionParams& params) = 0;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEXPORTWORKFLOWPORTS_H
