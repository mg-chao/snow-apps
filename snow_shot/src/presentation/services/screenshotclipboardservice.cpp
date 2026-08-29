#include "snow_shot/presentation/screenshotclipboardservice.h"

#include "screenshotclipboardperfinstrumentation.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QMimeData>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace {
constexpr int kMaximumCommitAttempts = 5;
constexpr qint64 kMaximumCommitDurationMs = 300;
constexpr std::array<int, kMaximumCommitAttempts - 1> kCommitRetryDelaysMs{10, 25, 60, 100};
std::atomic<quint64> g_latestPublicationId{0};

struct ClipboardPublishAttempt final {
    ScreenshotClipboardCommitFailure failure = ScreenshotClipboardCommitFailure::None;
    quint32 nativeError = 0;

    [[nodiscard]] bool succeeded() const {
        return failure == ScreenshotClipboardCommitFailure::None;
    }
};

class ClipboardCommitOperation final : public QObject {
  public:
    using Attempt = std::function<ClipboardPublishAttempt()>;

    ClipboardCommitOperation(QObject* receiver, std::shared_ptr<std::atomic_bool> cancelled,
                             Attempt attempt,
                             ScreenshotClipboardService::CommitCompletion completion)
        : m_receiver(receiver), m_cancelled(std::move(cancelled)), m_attempt(std::move(attempt)),
          m_completion(std::move(completion)) {
        if (receiver != nullptr) {
            connect(receiver, &QObject::destroyed, this, [this]() { finish({}, false); });
        }
    }

    void start() {
        m_elapsed.start();
        QTimer::singleShot(0, this, [this]() { runAttempt(); });
    }

  private:
    void runAttempt() {
        if (m_finished) {
            return;
        }
        if (m_cancelled == nullptr || m_cancelled->load(std::memory_order_acquire)) {
            ScreenshotClipboardCommitResult result;
            result.failure = ScreenshotClipboardCommitFailure::Cancelled;
            result.attempts = m_attempts;
            finish(result, true);
            return;
        }

        ++m_attempts;
        const ClipboardPublishAttempt attempt = m_attempt();
        if (attempt.succeeded()) {
            ScreenshotClipboardCommitResult result;
            result.attempts = m_attempts;
            finish(result, true);
            return;
        }

        const bool retryable = attempt.failure == ScreenshotClipboardCommitFailure::Busy;
        if (retryable && m_attempts < kMaximumCommitAttempts) {
            const int requestedDelay =
                kCommitRetryDelaysMs[static_cast<std::size_t>(m_attempts - 1)];
            const qint64 remaining = kMaximumCommitDurationMs - m_elapsed.elapsed();
            if (remaining > 0) {
                QTimer::singleShot(static_cast<int>((std::min)(remaining, qint64(requestedDelay))),
                                   this, [this]() { runAttempt(); });
                return;
            }
        }

        ScreenshotClipboardCommitResult result;
        result.failure = attempt.failure;
        result.nativeError = attempt.nativeError;
        result.attempts = m_attempts;
        finish(result, true);
    }

    void finish(ScreenshotClipboardCommitResult result, bool notify) {
        if (m_finished) {
            return;
        }
        m_finished = true;
        if (notify && !m_receiver.isNull() && m_completion) {
            m_completion(result);
        }
        m_completion = {};
        m_attempt = {};
        deleteLater();
    }

    QPointer<QObject> m_receiver;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    Attempt m_attempt;
    ScreenshotClipboardService::CommitCompletion m_completion;
    QElapsedTimer m_elapsed;
    int m_attempts = 0;
    bool m_finished = false;
};
} // namespace

#if defined(Q_OS_WIN) || defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace {
HWND clipboardOwnerWindow() {
    static const HWND owner =
        CreateWindowExW(0, L"STATIC", L"SnowShotClipboardOwner", 0, 0, 0, 0, 0, HWND_MESSAGE,
                        nullptr, GetModuleHandleW(nullptr), nullptr);
    return owner;
}

HGLOBAL prepareDib(const ScreenshotClipboardPixelSource& source) {
    if (!source.isValid()) {
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.failure.invalid_image", 1);
        return nullptr;
    }

    const QImage& sourceImage = source.image();
    QImage image;
    if (source.format() == ScreenshotClipboardPixelSource::Format::Argb32 ||
        source.format() == ScreenshotClipboardPixelSource::Format::Rgba8888) {
        image = sourceImage;
    } else {
        image = sourceImage.convertToFormat(QImage::Format_ARGB32);
    }

    const quint64 stride = static_cast<quint64>(image.width()) * 4U;
    const quint64 pixelBytes = stride * static_cast<quint64>(image.height());
    const quint64 totalBytes = sizeof(BITMAPINFOHEADER) + pixelBytes;
    if (pixelBytes > std::numeric_limits<DWORD>::max() ||
        totalBytes > std::numeric_limits<SIZE_T>::max()) {
        qWarning("Screenshot clipboard image is too large for CF_DIB");
        return nullptr;
    }

    HGLOBAL allocation = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(totalBytes));
    if (allocation == nullptr) {
        qWarning("Failed to allocate CF_DIB clipboard image");
        return nullptr;
    }
    void* memory = GlobalLock(allocation);
    if (memory == nullptr) {
        GlobalFree(allocation);
        qWarning("Failed to lock CF_DIB clipboard image");
        return nullptr;
    }

    auto* header = static_cast<BITMAPINFOHEADER*>(memory);
    std::memset(header, 0, sizeof(*header));
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = image.width();
    header->biHeight = image.height();
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    header->biSizeImage = static_cast<DWORD>(pixelBytes);

    auto* destination = reinterpret_cast<uchar*>(header + 1);
    if (source.format() == ScreenshotClipboardPixelSource::Format::Rgba8888) {
        const auto* sourcePixels = image.constBits();
        for (int row = 0; row < image.height(); ++row) {
            const auto* sourceRow = sourcePixels + static_cast<quint64>(row) * image.bytesPerLine();
            auto* destinationRow =
                destination + static_cast<quint64>(image.height() - 1 - row) * stride;
            for (int column = 0; column < image.width(); ++column) {
                const auto* pixel = sourceRow + static_cast<std::size_t>(column) * 4U;
                auto* output = destinationRow + static_cast<std::size_t>(column) * 4U;
                output[0] = pixel[2];
                output[1] = pixel[1];
                output[2] = pixel[0];
                output[3] = pixel[3];
            }
        }
    } else {
        for (int row = 0; row < image.height(); ++row) {
            std::memcpy(
                destination + static_cast<quint64>(image.height() - 1 - row) * stride,
                image.constScanLine(row), static_cast<std::size_t>(stride));
        }
    }
    GlobalUnlock(allocation);
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.dib_prepared", 1);
    return allocation;
}

HGLOBAL prepareDibV5(const ScreenshotClipboardPixelSource& source) {
    SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.prepare_total");
    if (!source.isValid()) {
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.failure.invalid_image", 1);
        return nullptr;
    }

    const QImage& sourceImage = source.image();

    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.input_width", sourceImage.width());
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.input_height", sourceImage.height());
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.input_bytes", sourceImage.sizeInBytes());
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.input_format",
                                     static_cast<qint64>(sourceImage.format()));

    QImage image;
    if (source.format() == ScreenshotClipboardPixelSource::Format::Argb32 ||
        source.format() == ScreenshotClipboardPixelSource::Format::Rgba8888) {
        image = sourceImage;
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.fused_source", 1);
    } else {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.convert_argb32");
        image = sourceImage.convertToFormat(QImage::Format_ARGB32);
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.compatibility_conversion", 1);
    }
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.conversion_detached",
                                     image.constBits() != sourceImage.constBits() ? 1 : 0);

    const quint64 stride = static_cast<quint64>(image.width()) * 4;
    const quint64 pixelBytes = stride * static_cast<quint64>(image.height());
    const quint64 totalBytes = sizeof(BITMAPV5HEADER) + pixelBytes;
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.pixel_bytes", static_cast<qint64>(pixelBytes));
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.dib_bytes", static_cast<qint64>(totalBytes));
    if (pixelBytes > std::numeric_limits<DWORD>::max() ||
        totalBytes > std::numeric_limits<SIZE_T>::max()) {
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.failure.image_too_large", 1);
        qWarning("Screenshot clipboard image is too large for CF_DIBV5");
        return nullptr;
    }

    HGLOBAL allocation = nullptr;
    {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.global_alloc");
        allocation = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(totalBytes));
    }
    if (allocation == nullptr) {
        const DWORD error = GetLastError();
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.failure.global_alloc", 1);
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.last_error", error);
        qWarning("Failed to allocate CF_DIBV5 clipboard image");
        return nullptr;
    }

    void* memory = nullptr;
    {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.global_lock");
        memory = GlobalLock(allocation);
    }
    if (memory == nullptr) {
        const DWORD error = GetLastError();
        GlobalFree(allocation);
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.failure.global_lock", 1);
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.last_error", error);
        qWarning("Failed to lock CF_DIBV5 clipboard image");
        return nullptr;
    }

    auto* header = static_cast<BITMAPV5HEADER*>(memory);
    {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.initialize_header");
        std::memset(header, 0, sizeof(*header));
        header->bV5Size = sizeof(BITMAPV5HEADER);
        header->bV5Width = image.width();
        header->bV5Height = -image.height();
        header->bV5Planes = 1;
        header->bV5BitCount = 32;
        header->bV5Compression = BI_BITFIELDS;
        header->bV5SizeImage = static_cast<DWORD>(pixelBytes);
        header->bV5RedMask = 0x00ff0000;
        header->bV5GreenMask = 0x0000ff00;
        header->bV5BlueMask = 0x000000ff;
        header->bV5AlphaMask = 0xff000000;
        header->bV5CSType = LCS_sRGB;
        header->bV5Intent = LCS_GM_IMAGES;
    }

    auto* destination = reinterpret_cast<uchar*>(header + 1);
    if (source.format() == ScreenshotClipboardPixelSource::Format::Rgba8888) {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.fused_rgba8888");
        const auto* sourcePixels = image.constBits();
        for (int row = 0; row < image.height(); ++row) {
            const auto* sourceRow = sourcePixels + static_cast<quint64>(row) * image.bytesPerLine();
            auto* destinationRow = destination + static_cast<quint64>(row) * stride;
            for (int column = 0; column < image.width(); ++column) {
                const auto* pixel = sourceRow + static_cast<std::size_t>(column) * 4;
                auto* output = destinationRow + static_cast<std::size_t>(column) * 4;
                output[0] = pixel[2];
                output[1] = pixel[1];
                output[2] = pixel[0];
                output[3] = pixel[3];
            }
        }
    } else {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.copy_pixels");
        for (int row = 0; row < image.height(); ++row) {
            std::memcpy(destination + static_cast<quint64>(row) * stride, image.constScanLine(row),
                        static_cast<std::size_t>(stride));
        }
    }
    {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.global_unlock");
        GlobalUnlock(allocation);
    }
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.prepared", 1);
    return allocation;
}

HGLOBAL prepareDibV5(const ScreenshotImageRowSource& source) {
    SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.prepare_rows_total");
    if (!source.isValid()) {
        return nullptr;
    }
    const quint64 stride = static_cast<quint64>(source.size.width()) * 4U;
    const quint64 pixelBytes = stride * static_cast<quint64>(source.size.height());
    const quint64 totalBytes = sizeof(BITMAPV5HEADER) + pixelBytes;
    if (pixelBytes > std::numeric_limits<DWORD>::max() ||
        totalBytes > std::numeric_limits<SIZE_T>::max() ||
        stride > static_cast<quint64>(std::numeric_limits<qsizetype>::max()) ||
        pixelBytes > static_cast<quint64>(std::numeric_limits<qsizetype>::max())) {
        return nullptr;
    }

    HGLOBAL allocation = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(totalBytes));
    if (allocation == nullptr) {
        return nullptr;
    }
    void* memory = GlobalLock(allocation);
    if (memory == nullptr) {
        GlobalFree(allocation);
        return nullptr;
    }

    auto* header = static_cast<BITMAPV5HEADER*>(memory);
    std::memset(header, 0, sizeof(*header));
    header->bV5Size = sizeof(BITMAPV5HEADER);
    header->bV5Width = source.size.width();
    header->bV5Height = -source.size.height();
    header->bV5Planes = 1;
    header->bV5BitCount = 32;
    header->bV5Compression = BI_BITFIELDS;
    header->bV5SizeImage = static_cast<DWORD>(pixelBytes);
    header->bV5RedMask = 0x00ff0000;
    header->bV5GreenMask = 0x0000ff00;
    header->bV5BlueMask = 0x000000ff;
    header->bV5AlphaMask = 0xff000000;
    header->bV5CSType = LCS_sRGB;
    header->bV5Intent = LCS_GM_IMAGES;

    auto* pixels = reinterpret_cast<uchar*>(header + 1);
    const bool read = source.readRows(0, source.size.height(), static_cast<qsizetype>(stride),
                                      pixels, static_cast<qsizetype>(pixelBytes));
    if (read) {
        for (quint64 offset = 0; offset < pixelBytes; offset += 4) {
            std::swap(pixels[offset], pixels[offset + 2]);
        }
    }
    GlobalUnlock(allocation);
    if (!read || (source.cancellationRequested && source.cancellationRequested())) {
        GlobalFree(allocation);
        return nullptr;
    }
    return allocation;
}

HGLOBAL prepareDib(const ScreenshotImageRowSource& source) {
    SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.prepare_rows_dib");
    if (!source.isValid()) {
        return nullptr;
    }
    const quint64 stride = static_cast<quint64>(source.size.width()) * 4U;
    const quint64 pixelBytes = stride * static_cast<quint64>(source.size.height());
    const quint64 totalBytes = sizeof(BITMAPINFOHEADER) + pixelBytes;
    if (pixelBytes > std::numeric_limits<DWORD>::max() ||
        totalBytes > std::numeric_limits<SIZE_T>::max() ||
        stride > static_cast<quint64>(std::numeric_limits<qsizetype>::max()) ||
        pixelBytes > static_cast<quint64>(std::numeric_limits<qsizetype>::max())) {
        return nullptr;
    }

    HGLOBAL allocation = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(totalBytes));
    if (allocation == nullptr) {
        return nullptr;
    }
    void* memory = GlobalLock(allocation);
    if (memory == nullptr) {
        GlobalFree(allocation);
        return nullptr;
    }

    auto* header = static_cast<BITMAPINFOHEADER*>(memory);
    std::memset(header, 0, sizeof(*header));
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = source.size.width();
    header->biHeight = source.size.height();
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    header->biSizeImage = static_cast<DWORD>(pixelBytes);

    auto* pixels = reinterpret_cast<uchar*>(header + 1);
    const bool read = source.readRows(0, source.size.height(), static_cast<qsizetype>(stride),
                                      pixels, static_cast<qsizetype>(pixelBytes));
    if (read) {
        for (quint64 offset = 0; offset < pixelBytes; offset += 4U) {
            std::swap(pixels[offset], pixels[offset + 2U]);
        }
        for (int row = 0; row < source.size.height() / 2; ++row) {
            auto* top = pixels + static_cast<quint64>(row) * stride;
            auto* bottom =
                pixels + static_cast<quint64>(source.size.height() - 1 - row) * stride;
            std::swap_ranges(top, top + stride, bottom);
        }
    }
    GlobalUnlock(allocation);
    if (!read || (source.cancellationRequested && source.cancellationRequested())) {
        GlobalFree(allocation);
        return nullptr;
    }
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.dib_prepared", 1);
    return allocation;
}

ClipboardPublishAttempt publishClipboardPayload(void** nativeHandle,
                                                ScreenshotClipboardFormatMode formatMode) {
    SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.publish_total");
    if (nativeHandle == nullptr || *nativeHandle == nullptr) {
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.failure.invalid_payload", 1);
        return {ScreenshotClipboardCommitFailure::InvalidPayload, ERROR_INVALID_DATA};
    }

    const UINT nativeFormat = formatMode == ScreenshotClipboardFormatMode::CompatibleDib
                                  ? CF_DIB
                                  : CF_DIBV5;
    const HGLOBAL allocation = static_cast<HGLOBAL>(*nativeHandle);
    HWND owner = nullptr;
    {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.owner_window");
        owner = clipboardOwnerWindow();
    }
    BOOL opened = FALSE;
    if (owner != nullptr) {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.open");
        opened = OpenClipboard(owner);
    }
    if (owner == nullptr || opened == FALSE) {
        const DWORD error = GetLastError();
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.failure.open", 1);
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.last_error", error);
        return {owner == nullptr ? ScreenshotClipboardCommitFailure::ClipboardUnavailable
                                 : ScreenshotClipboardCommitFailure::Busy,
                error};
    }

    bool emptied = false;
    {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.empty");
        emptied = EmptyClipboard() != FALSE;
    }
    DWORD clipboardError = ERROR_SUCCESS;
    if (!emptied) {
        clipboardError = GetLastError();
    }

    bool published = false;
    if (emptied) {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.set_data");
        published = SetClipboardData(nativeFormat, allocation) != nullptr;
        if (!published) {
            clipboardError = GetLastError();
        }
    }
    {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.close");
        CloseClipboard();
    }
    if (!published) {
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER(
            emptied ? "clipboard.failure.set_data" : "clipboard.failure.empty", 1);
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.last_error", clipboardError);
        qWarning("Failed to publish %s clipboard image",
                 nativeFormat == CF_DIB ? "CF_DIB" : "CF_DIBV5");
        return {emptied ? ScreenshotClipboardCommitFailure::PublishFailed
                        : ScreenshotClipboardCommitFailure::ClearFailed,
                clipboardError};
    }

    *nativeHandle = nullptr;
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.success", 1);
    return {};
}
} // namespace
#endif

QString ScreenshotClipboardCommitResult::errorString() const {
    switch (failure) {
    case ScreenshotClipboardCommitFailure::None:
        return {};
    case ScreenshotClipboardCommitFailure::Cancelled:
        return QCoreApplication::translate("ScreenshotClipboardService",
                                           "The clipboard operation was cancelled");
    case ScreenshotClipboardCommitFailure::InvalidPayload:
        return QCoreApplication::translate("ScreenshotClipboardService",
                                           "The prepared clipboard image is invalid");
    case ScreenshotClipboardCommitFailure::ClipboardUnavailable:
        return QCoreApplication::translate("ScreenshotClipboardService",
                                           "The clipboard is unavailable");
    case ScreenshotClipboardCommitFailure::Busy:
        return QCoreApplication::translate("ScreenshotClipboardService",
                                           "The clipboard is busy");
    case ScreenshotClipboardCommitFailure::ClearFailed:
        return QCoreApplication::translate("ScreenshotClipboardService",
                                           "The clipboard could not be cleared");
    case ScreenshotClipboardCommitFailure::PublishFailed:
        return QCoreApplication::translate("ScreenshotClipboardService",
                                           "The clipboard did not accept the image");
    }
    return QCoreApplication::translate("ScreenshotClipboardService",
                                       "The clipboard operation failed");
}

ScreenshotClipboardCommitHandle::ScreenshotClipboardCommitHandle(
    std::shared_ptr<std::atomic_bool> cancelled)
    : m_cancelled(std::move(cancelled)) {}

void ScreenshotClipboardCommitHandle::cancel() const {
    if (m_cancelled != nullptr) {
        m_cancelled->store(true, std::memory_order_release);
    }
}

bool ScreenshotClipboardCommitHandle::isValid() const {
    return m_cancelled != nullptr;
}

bool ScreenshotClipboardCommitHandle::isCancellationRequested() const {
    return !isValid() || m_cancelled->load(std::memory_order_acquire);
}

ScreenshotClipboardPayload::~ScreenshotClipboardPayload() {
    reset();
}

void ScreenshotClipboardPayload::reset() noexcept {
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (m_nativeHandle != nullptr) {
        GlobalFree(static_cast<HGLOBAL>(m_nativeHandle));
        m_nativeHandle = nullptr;
    }
#else
    m_image = {};
#endif
}

ScreenshotClipboardPayload::ScreenshotClipboardPayload(
    ScreenshotClipboardPayload&& other) noexcept {
#if defined(Q_OS_WIN) || defined(_WIN32)
    m_nativeHandle = other.m_nativeHandle;
    m_formatMode = other.m_formatMode;
    other.m_nativeHandle = nullptr;
#else
    m_image = std::move(other.m_image);
#endif
}

ScreenshotClipboardPayload&
ScreenshotClipboardPayload::operator=(ScreenshotClipboardPayload&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
#if defined(Q_OS_WIN) || defined(_WIN32)
    m_nativeHandle = other.m_nativeHandle;
    m_formatMode = other.m_formatMode;
    other.m_nativeHandle = nullptr;
#else
    m_image = std::move(other.m_image);
#endif
    return *this;
}

bool ScreenshotClipboardPayload::isValid() const {
#if defined(Q_OS_WIN) || defined(_WIN32)
    return m_nativeHandle != nullptr;
#else
    return !m_image.isNull();
#endif
}

ScreenshotClipboardPayload
ScreenshotClipboardService::prepare(ScreenshotClipboardPixelSource source,
                                    ScreenshotClipboardFormatMode formatMode) {
    if (!source.isValid()) {
        qWarning("Ignoring null screenshot clipboard image");
        return {};
    }
#if defined(Q_OS_WIN) || defined(_WIN32)
    ScreenshotClipboardPayload payload;
    payload.m_formatMode = formatMode;
    payload.m_nativeHandle = formatMode == ScreenshotClipboardFormatMode::CompatibleDib
                                 ? prepareDib(source)
                                 : prepareDibV5(source);
    return payload;
#else
    Q_UNUSED(formatMode);
    ScreenshotClipboardPayload payload;
    payload.m_image = source.image();
    return payload;
#endif
}

ScreenshotClipboardPayload
ScreenshotClipboardService::prepare(const ScreenshotImageRowSource& source,
                                    ScreenshotClipboardFormatMode formatMode) {
    if (!source.isValid()) {
        return {};
    }
#if defined(Q_OS_WIN) || defined(_WIN32)
    ScreenshotClipboardPayload payload;
    payload.m_formatMode = formatMode;
    payload.m_nativeHandle = formatMode == ScreenshotClipboardFormatMode::CompatibleDib
                                 ? prepareDib(source)
                                 : prepareDibV5(source);
    return payload;
#else
    Q_UNUSED(formatMode);
    QImage image(source.size, QImage::Format_RGBA8888);
    if (image.isNull() || !source.readRows(0, image.height(), image.bytesPerLine(), image.bits(),
                                           image.sizeInBytes())) {
        return {};
    }
    ScreenshotClipboardPayload payload;
    payload.m_image = std::move(image);
    return payload;
#endif
}

ScreenshotClipboardPayload
ScreenshotClipboardService::prepareImage(const QImage& image,
                                         ScreenshotClipboardFormatMode formatMode) {
    return prepare(ScreenshotClipboardPixelSource(image), formatMode);
}

ScreenshotClipboardCommitHandle
ScreenshotClipboardService::commit(QClipboard* clipboard, QObject* receiver,
                                   ScreenshotClipboardPayload payload,
                                   CommitCompletion completion) {
    return commit(clipboard, receiver, std::move(payload), reservePublication(),
                  std::move(completion));
}

ScreenshotClipboardService::PublicationId ScreenshotClipboardService::reservePublication() {
    return g_latestPublicationId.fetch_add(1, std::memory_order_acq_rel) + 1;
}

ScreenshotClipboardCommitHandle ScreenshotClipboardService::commit(
    QClipboard* clipboard, QObject* receiver, ScreenshotClipboardPayload payload,
    PublicationId publicationId, CommitCompletion completion) {
    QCoreApplication* application = QCoreApplication::instance();
    if (receiver == nullptr || !completion || application == nullptr ||
        QThread::currentThread() != application->thread()) {
        return {};
    }

    auto cancelled = std::make_shared<std::atomic_bool>(false);
    auto sharedPayload = std::make_shared<ScreenshotClipboardPayload>(std::move(payload));
#if defined(Q_OS_WIN) || defined(_WIN32)
    Q_UNUSED(clipboard);
    auto attempt = [sharedPayload, publicationId]() {
        if (publicationId != g_latestPublicationId.load(std::memory_order_acquire)) {
            return ClipboardPublishAttempt{};
        }
        return publishClipboardPayload(&sharedPayload->m_nativeHandle,
                                       sharedPayload->m_formatMode);
    };
#else
    const QPointer<QClipboard> guardedClipboard(clipboard);
    auto attempt = [guardedClipboard, sharedPayload, publicationId]() {
        if (publicationId != g_latestPublicationId.load(std::memory_order_acquire)) {
            return ClipboardPublishAttempt{};
        }
        if (guardedClipboard.isNull()) {
            return ClipboardPublishAttempt{ScreenshotClipboardCommitFailure::ClipboardUnavailable,
                                           0};
        }
        if (!sharedPayload->isValid()) {
            return ClipboardPublishAttempt{ScreenshotClipboardCommitFailure::InvalidPayload, 0};
        }
        guardedClipboard->setImage(sharedPayload->m_image, QClipboard::Clipboard);
        sharedPayload->reset();
        return ClipboardPublishAttempt{};
    };
#endif
    auto* operation = new ClipboardCommitOperation(receiver, cancelled, std::move(attempt),
                                                   std::move(completion));
    operation->start();
    return ScreenshotClipboardCommitHandle(std::move(cancelled));
}

ScreenshotClipboardCommitHandle ScreenshotClipboardService::commitMimeData(
    QClipboard* clipboard, QObject* receiver, QMimeData* mimeData,
    CommitCompletion completion) {
    return commitMimeData(clipboard, receiver, mimeData, reservePublication(),
                          std::move(completion));
}

ScreenshotClipboardCommitHandle ScreenshotClipboardService::commitMimeData(
    QClipboard* clipboard, QObject* receiver, QMimeData* mimeData,
    PublicationId publicationId, CommitCompletion completion) {
    QCoreApplication* application = QCoreApplication::instance();
    if (receiver == nullptr || mimeData == nullptr || !completion || application == nullptr ||
        QThread::currentThread() != application->thread()) {
        delete mimeData;
        return {};
    }

    auto cancelled = std::make_shared<std::atomic_bool>(false);
    auto holder = std::make_shared<std::unique_ptr<QMimeData>>(mimeData);
    const QPointer<QClipboard> guardedClipboard(clipboard);
    auto attempt = [guardedClipboard, holder, publicationId]() {
        if (publicationId != g_latestPublicationId.load(std::memory_order_acquire)) {
            return ClipboardPublishAttempt{};
        }
        if (guardedClipboard.isNull()) {
            return ClipboardPublishAttempt{ScreenshotClipboardCommitFailure::ClipboardUnavailable, 0};
        }
        if (*holder == nullptr) {
            return ClipboardPublishAttempt{ScreenshotClipboardCommitFailure::InvalidPayload, 0};
        }
        guardedClipboard->setMimeData(holder->release(), QClipboard::Clipboard);
        return ClipboardPublishAttempt{};
    };
    auto* operation = new ClipboardCommitOperation(receiver, cancelled, std::move(attempt),
                                                   std::move(completion));
    operation->start();
    return ScreenshotClipboardCommitHandle(std::move(cancelled));
}

bool ScreenshotClipboardService::publish(QClipboard* clipboard,
                                         ScreenshotClipboardPayload payload) {
    static_cast<void>(reservePublication());
#if defined(Q_OS_WIN) || defined(_WIN32)
    Q_UNUSED(clipboard);
    return publishClipboardPayload(&payload.m_nativeHandle, payload.m_formatMode).succeeded();
#else
    if (clipboard == nullptr || !payload.isValid()) {
        qWarning("Screenshot clipboard is unavailable");
        return false;
    }
    clipboard->setImage(payload.m_image, QClipboard::Clipboard);
    return true;
#endif
}

bool ScreenshotClipboardService::publishImage(QClipboard* clipboard, const QImage& image,
                                              ScreenshotClipboardFormatMode formatMode) {
    return publish(clipboard, prepareImage(image, formatMode));
}
