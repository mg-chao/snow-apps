#include "snow_shot/presentation/screenshotclipboardservice.h"

#include "snow_shot/presentation/screenshotasyncactivitytracker.h"

#include "screenshotclipboardperfinstrumentation.h"
#include "screenshotclipboardworkerprotocol.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include <utility>

namespace {
constexpr int kMaximumCommitAttempts = 5;
constexpr qint64 kMaximumCommitDurationMs = 300;
constexpr std::array<int, kMaximumCommitAttempts - 1> kCommitRetryDelaysMs{10, 25, 60, 100};

struct ClipboardPublishAttempt final {
    ScreenshotClipboardCommitFailure failure = ScreenshotClipboardCommitFailure::None;
    quint32 nativeError = 0;
    int attempts = 1;

    [[nodiscard]] bool succeeded() const {
        return failure == ScreenshotClipboardCommitFailure::None;
    }
};

class ClipboardCommitOperation final : public QObject {
  public:
    using Attempt = std::function<ClipboardPublishAttempt()>;

    ClipboardCommitOperation(QObject* receiver, std::shared_ptr<std::atomic_bool> cancelled,
                             Attempt attempt, bool backgroundAttempt,
                             ScreenshotClipboardService::CommitCompletion completion,
                             ScreenshotAsyncActivityLease activityLease)
        : m_receiver(receiver), m_cancelled(std::move(cancelled)), m_attempt(std::move(attempt)),
          m_completion(std::move(completion)), m_activityLease(std::move(activityLease)),
          m_backgroundAttempt(backgroundAttempt) {
        if (receiver != nullptr) {
            connect(receiver, &QObject::destroyed, this, [this]() {
                m_receiverDestroyed = true;
                if (!m_attemptInFlight) {
                    finish({}, false);
                }
            });
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

        if (m_backgroundAttempt) {
            m_attemptInFlight = true;
            const Attempt attempt = m_attempt;
            std::thread([this, attempt]() {
                const ClipboardPublishAttempt result = attempt();
                QMetaObject::invokeMethod(
                    this, [this, result]() { completeAttempt(result); }, Qt::QueuedConnection);
            }).detach();
            return;
        }
        completeAttempt(m_attempt());
    }

    void completeAttempt(const ClipboardPublishAttempt& attempt) {
        if (m_finished) {
            return;
        }
        m_attemptInFlight = false;
        m_attempts += (std::max)(attempt.attempts, 1);
        if (m_receiverDestroyed) {
            finish({}, false);
            return;
        }
        if (m_cancelled == nullptr || m_cancelled->load(std::memory_order_acquire)) {
            ScreenshotClipboardCommitResult result;
            result.failure = ScreenshotClipboardCommitFailure::Cancelled;
            result.attempts = m_attempts;
            finish(result, true);
            return;
        }
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
        m_activityLease.reset();
        deleteLater();
    }

    QPointer<QObject> m_receiver;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    Attempt m_attempt;
    ScreenshotClipboardService::CommitCompletion m_completion;
    ScreenshotAsyncActivityLease m_activityLease;
    QElapsedTimer m_elapsed;
    int m_attempts = 0;
    bool m_finished = false;
    bool m_backgroundAttempt = false;
    bool m_attemptInFlight = false;
    bool m_receiverDestroyed = false;
};
} // namespace

#if defined(Q_OS_WIN) || defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace {
namespace clipboard_worker_protocol =
    snow_shot::presentation::clipboard_worker_protocol;

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

QString clipboardWorkerExecutablePath() {
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("snow-shot-clipboard-worker.exe"));
}

bool cancellationRequested(const std::atomic_bool* cancelled) {
    return cancelled != nullptr && cancelled->load(std::memory_order_acquire);
}

bool writeWorkerBytes(QProcess& process, const char* bytes, quint64 length,
                      const std::atomic_bool* cancelled, QElapsedTimer& elapsed) {
    constexpr quint64 kChunkBytes = 64U * 1024U;
    constexpr qint64 kWorkerTimeoutMs = 60'000;
    quint64 offset = 0;
    while (offset < length) {
        if (cancellationRequested(cancelled) || elapsed.elapsed() >= kWorkerTimeoutMs) {
            return false;
        }
        const qint64 requested = static_cast<qint64>((std::min)(kChunkBytes, length - offset));
        const qint64 accepted = process.write(bytes + offset, requested);
        if (accepted <= 0) {
            return false;
        }
        offset += static_cast<quint64>(accepted);
        while (process.bytesToWrite() > 0) {
            if (cancellationRequested(cancelled) || elapsed.elapsed() >= kWorkerTimeoutMs) {
                return false;
            }
            if (!process.waitForBytesWritten(25) && process.state() == QProcess::NotRunning) {
                return false;
            }
        }
    }
    return true;
}

ScreenshotClipboardCommitFailure commitFailureForWorkerStatus(
    clipboard_worker_protocol::Status status) {
    switch (status) {
    case clipboard_worker_protocol::Status::Success:
        return ScreenshotClipboardCommitFailure::None;
    case clipboard_worker_protocol::Status::InvalidPayload:
        return ScreenshotClipboardCommitFailure::InvalidPayload;
    case clipboard_worker_protocol::Status::ClipboardUnavailable:
        return ScreenshotClipboardCommitFailure::ClipboardUnavailable;
    case clipboard_worker_protocol::Status::Busy:
        return ScreenshotClipboardCommitFailure::Busy;
    case clipboard_worker_protocol::Status::ClearFailed:
        return ScreenshotClipboardCommitFailure::ClearFailed;
    case clipboard_worker_protocol::Status::PublishFailed:
        return ScreenshotClipboardCommitFailure::PublishFailed;
    }
    return ScreenshotClipboardCommitFailure::PublishFailed;
}

ClipboardPublishAttempt publishClipboardPayload(
    void** nativeHandle, ScreenshotClipboardFormatMode formatMode, const QString& workerPath,
    const std::atomic_bool* cancelled = nullptr) {
    SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.publish_total");
    if (nativeHandle == nullptr || *nativeHandle == nullptr) {
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.failure.invalid_payload", 1);
        return {ScreenshotClipboardCommitFailure::InvalidPayload, ERROR_INVALID_DATA};
    }

    const UINT nativeFormat = formatMode == ScreenshotClipboardFormatMode::CompatibleDib
                                  ? CF_DIB
                                  : CF_DIBV5;
    const HGLOBAL allocation = static_cast<HGLOBAL>(*nativeHandle);
    const SIZE_T allocationBytes = GlobalSize(allocation);
    if (allocationBytes == 0 ||
        allocationBytes > clipboard_worker_protocol::kMaximumPayloadBytes) {
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.failure.invalid_payload", 1);
        return {ScreenshotClipboardCommitFailure::InvalidPayload, ERROR_INVALID_DATA};
    }
    if (!QFileInfo(workerPath).isExecutable()) {
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.failure.worker_missing", 1);
        return {ScreenshotClipboardCommitFailure::ClipboardUnavailable, ERROR_FILE_NOT_FOUND};
    }

    void* memory = GlobalLock(allocation);
    if (memory == nullptr) {
        const DWORD error = GetLastError();
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.failure.global_lock", 1);
        return {ScreenshotClipboardCommitFailure::InvalidPayload, error};
    }

    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(workerPath, QStringList{}, QIODevice::ReadWrite);
    QElapsedTimer elapsed;
    elapsed.start();
    bool sent = process.waitForStarted(5'000);
    clipboard_worker_protocol::RequestHeader request;
    request.nativeFormat = nativeFormat;
    request.payloadBytes = static_cast<std::uint64_t>(allocationBytes);
    if (sent) {
        sent = writeWorkerBytes(process, reinterpret_cast<const char*>(&request), sizeof(request),
                                cancelled, elapsed) &&
               writeWorkerBytes(process, static_cast<const char*>(memory), request.payloadBytes,
                                cancelled, elapsed);
    }
    GlobalUnlock(allocation);
    process.closeWriteChannel();

    constexpr qint64 kWorkerTimeoutMs = 60'000;
    bool finished = sent;
    while (finished && process.state() != QProcess::NotRunning) {
        if (cancellationRequested(cancelled) || elapsed.elapsed() >= kWorkerTimeoutMs) {
            finished = false;
            break;
        }
        static_cast<void>(process.waitForFinished(25));
    }
    if (!finished) {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            static_cast<void>(process.waitForFinished(5'000));
        }
        const bool wasCancelled = cancellationRequested(cancelled);
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER(
            wasCancelled ? "clipboard.failure.cancelled" : "clipboard.failure.worker_io", 1);
        return {wasCancelled ? ScreenshotClipboardCommitFailure::Cancelled
                             : ScreenshotClipboardCommitFailure::PublishFailed,
                static_cast<quint32>(wasCancelled ? ERROR_CANCELLED : ERROR_PROCESS_ABORTED)};
    }

    const QByteArray output = process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 ||
        output.size() != static_cast<qsizetype>(sizeof(clipboard_worker_protocol::ResponseHeader))) {
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.failure.worker_protocol", 1);
        return {ScreenshotClipboardCommitFailure::PublishFailed, ERROR_INVALID_DATA};
    }
    clipboard_worker_protocol::ResponseHeader response;
    std::memcpy(&response, output.constData(), sizeof(response));
    if (response.magic != clipboard_worker_protocol::kResponseMagic ||
        response.version != clipboard_worker_protocol::kVersion || response.reserved != 0 ||
        response.attempts > static_cast<quint32>((std::numeric_limits<int>::max)())) {
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.failure.worker_protocol", 1);
        return {ScreenshotClipboardCommitFailure::PublishFailed, ERROR_INVALID_DATA};
    }
    const auto workerStatus =
        static_cast<clipboard_worker_protocol::Status>(response.status);
    const ScreenshotClipboardCommitFailure failure = commitFailureForWorkerStatus(workerStatus);
    if (failure != ScreenshotClipboardCommitFailure::None) {
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.failure.worker_publish", 1);
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.last_error", response.nativeError);
        return {failure, response.nativeError, static_cast<int>(response.attempts)};
    }

    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.worker_bytes",
                                     static_cast<qint64>(allocationBytes));
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.success", 1);
    return {ScreenshotClipboardCommitFailure::None, 0,
            static_cast<int>((std::max)(response.attempts, 1U))};
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
    QCoreApplication* application = QCoreApplication::instance();
    if (receiver == nullptr || !completion || application == nullptr ||
        QThread::currentThread() != application->thread()) {
        return {};
    }

    auto cancelled = std::make_shared<std::atomic_bool>(false);
    auto sharedPayload = std::make_shared<ScreenshotClipboardPayload>(std::move(payload));
    bool backgroundAttempt = false;
#if defined(Q_OS_WIN) || defined(_WIN32)
    Q_UNUSED(clipboard);
    backgroundAttempt = true;
    const QString workerPath = clipboardWorkerExecutablePath();
    auto attempt = [sharedPayload, cancelled, workerPath]() {
        return publishClipboardPayload(&sharedPayload->m_nativeHandle,
                                       sharedPayload->m_formatMode, workerPath, cancelled.get());
    };
#else
    const QPointer<QClipboard> guardedClipboard(clipboard);
    auto attempt = [guardedClipboard, sharedPayload]() {
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
    auto* operation = new ClipboardCommitOperation(
        receiver, cancelled, std::move(attempt), backgroundAttempt, std::move(completion),
        ScreenshotAsyncActivityTracker::shared().acquire());
    operation->start();
    return ScreenshotClipboardCommitHandle(std::move(cancelled));
}

bool ScreenshotClipboardService::publish(QClipboard* clipboard,
                                         ScreenshotClipboardPayload payload) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    Q_UNUSED(clipboard);
    return publishClipboardPayload(&payload.m_nativeHandle, payload.m_formatMode,
                                   clipboardWorkerExecutablePath())
        .succeeded();
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
