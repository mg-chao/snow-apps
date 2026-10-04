#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDSERVICE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDSERVICE_H

#include "snow_shot/presentation/screenshotimagerowsource.h"
#include "snow_shot/presentation/screenshotclipboardplacement.h"
#include "snow_shot/presentation/screenshotclipboardappearance.h"
#include "snow_shot/presentation/screenshotimagefileservice.h"

#include <QByteArray>
#include <QImage>
#include <QString>

#include <QtGlobal>

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

class QClipboard;
class QMimeData;
class QObject;
class QTemporaryDir;
struct ScreenshotClipboardPayloadTestAccess;
struct ScreenshotClipboardCommitState;
struct ScreenshotClipboardCommitScopeState;

class ScreenshotClipboardPayload final {
  public:
    ScreenshotClipboardPayload() = default;
    ~ScreenshotClipboardPayload();

    ScreenshotClipboardPayload(const ScreenshotClipboardPayload&) = delete;
    ScreenshotClipboardPayload& operator=(const ScreenshotClipboardPayload&) = delete;
    ScreenshotClipboardPayload(ScreenshotClipboardPayload&& other) noexcept;
    ScreenshotClipboardPayload& operator=(ScreenshotClipboardPayload&& other) noexcept;

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] const QByteArray& pngBytes() const {
        return m_pngBytes;
    }
    [[nodiscard]] const QByteArray& appearanceBytes() const {
        return m_appearanceBytes;
    }
    [[nodiscard]] const QByteArray& placementBytes() const {
        return m_placementBytes;
    }

  private:
    friend class ScreenshotClipboardService;
    friend struct ScreenshotClipboardPayloadTestAccess;

    void reset() noexcept;

#if defined(Q_OS_WIN) || defined(_WIN32)
    void* m_dibHandle = nullptr;
    void* m_pngHandle = nullptr;
    void* m_placementHandle = nullptr;
    void* m_appearanceHandle = nullptr;
#endif
    QByteArray m_pngBytes;
    QByteArray m_placementBytes;
    QByteArray m_appearanceBytes;
};

enum class ScreenshotClipboardCommitFailure {
    None,
    Cancelled,
    InvalidPayload,
    ClipboardUnavailable,
    Busy,
    ClearFailed,
    PublishFailed,
};

struct ScreenshotClipboardCommitResult final {
    ScreenshotClipboardCommitFailure failure = ScreenshotClipboardCommitFailure::None;
    quint32 nativeError = 0;
    int attempts = 0;

    [[nodiscard]] bool succeeded() const {
        return failure == ScreenshotClipboardCommitFailure::None;
    }
    [[nodiscard]] QString errorString() const;
};

class ScreenshotClipboardCommitHandle final {
  public:
    ScreenshotClipboardCommitHandle() = default;

    void cancel() const;
    [[nodiscard]] bool isValid() const;
    [[nodiscard]] bool isCancellationRequested() const;
    [[nodiscard]] bool isFinished() const;

  private:
    friend class ScreenshotClipboardService;
    friend class ScreenshotClipboardCommitScope;
    explicit ScreenshotClipboardCommitHandle(std::shared_ptr<ScreenshotClipboardCommitState> state);
    std::shared_ptr<ScreenshotClipboardCommitState> m_state;
};

class ScreenshotClipboardService final {
  public:
    using PublicationId = quint64;
    using CommitCompletion = std::function<void(ScreenshotClipboardCommitResult)>;

    [[nodiscard]] static PublicationId reservePublication();

    // Callers snapshot image export settings before scheduling asynchronous preparation.
    [[nodiscard]] static ScreenshotClipboardPayload
    prepare(const ScreenshotImageRowSource& source, ScreenshotImageEncodingOptions encoding = {},
            std::optional<ScreenshotClipboardPlacement> placement = {},
            std::optional<ScreenshotClipboardAppearance> appearance = {});
    [[nodiscard]] static ScreenshotClipboardPayload
    prepareImage(const QImage& image, ScreenshotImageEncodingOptions encoding = {},
                 std::optional<ScreenshotClipboardPlacement> placement = {},
                 std::optional<ScreenshotClipboardAppearance> appearance = {});
    // The supplied PNG must encode the same sRGB pixels as the source, using the
    // requested export settings. This only prepares native clipboard representations.
    [[nodiscard]] static ScreenshotClipboardPayload
    prepareEncoded(const ScreenshotImageRowSource& source, const QByteArray& png,
                   std::optional<ScreenshotClipboardPlacement> placement = {},
                   std::optional<ScreenshotClipboardAppearance> appearance = {});
    [[nodiscard]] static ScreenshotClipboardCommitHandle commit(QClipboard* clipboard,
                                                                QObject* receiver,
                                                                ScreenshotClipboardPayload payload,
                                                                CommitCompletion completion);
    [[nodiscard]] static ScreenshotClipboardCommitHandle
    commit(QClipboard* clipboard, QObject* receiver, ScreenshotClipboardPayload payload,
           PublicationId publicationId, CommitCompletion completion);
    [[nodiscard]] static ScreenshotClipboardCommitHandle
    commitMimeData(QClipboard* clipboard, QObject* receiver, QMimeData* mimeData,
                   CommitCompletion completion);
    [[nodiscard]] static ScreenshotClipboardCommitHandle
    commitMimeData(QClipboard* clipboard, QObject* receiver, QMimeData* mimeData,
                   PublicationId publicationId, CommitCompletion completion);
    // Clipboard ownership, including publication retries, retains the temporary file.
    [[nodiscard]] static std::unique_ptr<QMimeData>
    temporaryFileMimeData(const QString& path, std::shared_ptr<QTemporaryDir> directory);
    [[nodiscard]] static bool publish(QClipboard* clipboard, ScreenshotClipboardPayload payload);
    [[nodiscard]] static bool publishImage(QClipboard* clipboard, const QImage& image,
                                           ScreenshotImageEncodingOptions encoding = {});
};

// Owns cancellation for a set of publications on the GUI thread. Terminal operations
// leave the scope before notifying their receivers. Cancellation and destruction
// suppress remaining callbacks; the scope can be reused after cancelAll().
class ScreenshotClipboardCommitScope final {
  public:
    ScreenshotClipboardCommitScope() = default;
    ~ScreenshotClipboardCommitScope();
    ScreenshotClipboardCommitScope(const ScreenshotClipboardCommitScope&) = delete;
    ScreenshotClipboardCommitScope& operator=(const ScreenshotClipboardCommitScope&) = delete;

    [[nodiscard]] ScreenshotClipboardCommitHandle
    commit(QClipboard* clipboard, QObject* receiver, ScreenshotClipboardPayload payload,
           ScreenshotClipboardService::CommitCompletion completion,
           ScreenshotClipboardService::PublicationId publicationId = 0);
    [[nodiscard]] ScreenshotClipboardCommitHandle
    commitMimeData(QClipboard* clipboard, QObject* receiver, QMimeData* mimeData,
                   ScreenshotClipboardService::CommitCompletion completion,
                   ScreenshotClipboardService::PublicationId publicationId = 0);
    void cancelAll();
    [[nodiscard]] qsizetype pendingCount() const;

  private:
    void track(const ScreenshotClipboardCommitHandle& handle);
    std::shared_ptr<ScreenshotClipboardCommitScopeState> m_state;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDSERVICE_H
