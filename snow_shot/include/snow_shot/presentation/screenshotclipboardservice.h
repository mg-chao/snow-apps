#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDSERVICE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDSERVICE_H

#include "snow_shot/presentation/screenshotimagerowsource.h"

#include <QImage>
#include <QString>

#include <QtGlobal>

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

class ScreenshotClipboardPixelSource final {
  public:
    enum class Format {
        Unsupported,
        Argb32,
        Argb32Premultiplied,
        Rgba8888,
    };

    ScreenshotClipboardPixelSource() = default;
    explicit ScreenshotClipboardPixelSource(QImage image) : m_image(std::move(image)) {}

    [[nodiscard]] bool isValid() const {
        return !m_image.isNull() && m_image.width() > 0 && m_image.height() > 0 &&
               m_image.constBits() != nullptr;
    }
    [[nodiscard]] Format format() const {
        switch (m_image.format()) {
        case QImage::Format_ARGB32:
            return Format::Argb32;
        case QImage::Format_ARGB32_Premultiplied:
            return Format::Argb32Premultiplied;
        case QImage::Format_RGBA8888:
            return Format::Rgba8888;
        default:
            return Format::Unsupported;
        }
    }
    [[nodiscard]] const QImage& image() const {
        return m_image;
    }

  private:
    QImage m_image;
};

class QClipboard;
class QObject;
struct ScreenshotClipboardPayloadTestAccess;

enum class ScreenshotClipboardFormatMode {
    CompatibleDib,
    DibV5,
};

class ScreenshotClipboardPayload final {
  public:
    ScreenshotClipboardPayload() = default;
    ~ScreenshotClipboardPayload();

    ScreenshotClipboardPayload(const ScreenshotClipboardPayload&) = delete;
    ScreenshotClipboardPayload& operator=(const ScreenshotClipboardPayload&) = delete;
    ScreenshotClipboardPayload(ScreenshotClipboardPayload&& other) noexcept;
    ScreenshotClipboardPayload& operator=(ScreenshotClipboardPayload&& other) noexcept;

    [[nodiscard]] bool isValid() const;

  private:
    friend class ScreenshotClipboardService;
    friend struct ScreenshotClipboardPayloadTestAccess;

    void reset() noexcept;

#if defined(Q_OS_WIN) || defined(_WIN32)
    void* m_nativeHandle = nullptr;
    ScreenshotClipboardFormatMode m_formatMode = ScreenshotClipboardFormatMode::DibV5;
#else
    QImage m_image;
#endif
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

  private:
    friend class ScreenshotClipboardService;
    explicit ScreenshotClipboardCommitHandle(std::shared_ptr<std::atomic_bool> cancelled);
    std::shared_ptr<std::atomic_bool> m_cancelled;
};

class ScreenshotClipboardService final {
  public:
    using CommitCompletion = std::function<void(ScreenshotClipboardCommitResult)>;

    [[nodiscard]] static ScreenshotClipboardPayload prepare(
        ScreenshotClipboardPixelSource source,
        ScreenshotClipboardFormatMode formatMode =
            ScreenshotClipboardFormatMode::DibV5);
    [[nodiscard]] static ScreenshotClipboardPayload prepare(
        const ScreenshotImageRowSource& source,
        ScreenshotClipboardFormatMode formatMode =
            ScreenshotClipboardFormatMode::DibV5);
    [[nodiscard]] static ScreenshotClipboardPayload prepareImage(
        const QImage& image,
        ScreenshotClipboardFormatMode formatMode =
            ScreenshotClipboardFormatMode::DibV5);
    [[nodiscard]] static ScreenshotClipboardCommitHandle commit(QClipboard* clipboard,
                                                                QObject* receiver,
                                                                ScreenshotClipboardPayload payload,
                                                                CommitCompletion completion);
    [[nodiscard]] static bool publish(QClipboard* clipboard, ScreenshotClipboardPayload payload);
    [[nodiscard]] static bool publishImage(
        QClipboard* clipboard, const QImage& image,
        ScreenshotClipboardFormatMode formatMode =
            ScreenshotClipboardFormatMode::DibV5);
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDSERVICE_H
