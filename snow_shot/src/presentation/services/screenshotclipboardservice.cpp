#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_shot/runtime/runtimeactivitytracker.h"
#include "snow_shot/diagnostics/diagnostics.h"
#include <QUuid>

#include "screenshotclipboardperfinstrumentation.h"
#include "snowimageqtcodec.h"
#ifdef Q_OS_MACOS
#include "../../platform/macos/imageclipboard.h"
#endif

#include <QClipboard>
#include <QBuffer>
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QHash>
#include <QGuiApplication>
#include <QMimeData>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

struct ScreenshotClipboardCommitState {
    std::atomic_bool cancelled{false};
    std::atomic_bool finished{false};
    std::atomic_bool completionEnabled{true};
    std::weak_ptr<ScreenshotClipboardCommitScopeState> scope;
};

struct ScreenshotClipboardCommitScopeState {
    QHash<ScreenshotClipboardCommitState*, std::shared_ptr<ScreenshotClipboardCommitState>> pending;
};

namespace {
class TemporaryFileClipboardMimeData final : public QMimeData {
  public:
    explicit TemporaryFileClipboardMimeData(std::shared_ptr<QTemporaryDir> directory)
        : m_directory(std::move(directory)) {
        if (qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
            connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] {
                const auto* clipboard = QGuiApplication::clipboard();
                if (m_directory && clipboard && clipboard->mimeData() == this) {
                    // Native clipboards outlive the app. Leave the current file in system
                    // temporary storage so shutdown does not invalidate an unpasted URL.
                    m_directory->setAutoRemove(false);
                }
            });
        }
    }

    [[nodiscard]] std::shared_ptr<QTemporaryDir> directory() const {
        return m_directory;
    }

  private:
    std::shared_ptr<QTemporaryDir> m_directory;
};

#if !defined(Q_OS_WIN)
// Keep canonical PNG bytes for native consumers and lazily provide Qt's image
// representation when a reader requests it. Decoded pixels belong to that reader.
class PngClipboardMimeData final : public QMimeData {
  public:
    explicit PngClipboardMimeData(QByteArray png, QByteArray placement = {},
                                  QByteArray appearance = {}) {
#ifdef Q_OS_MACOS
        snow_shot::platform::macos::initializeImageClipboardConverter();
#endif
        ensureScreenshotClipboardPlacementMimeSupport();
        ensureScreenshotClipboardAppearanceMimeSupport();
        if (!appearance.isEmpty())
            setData(screenshotClipboardAppearanceNativeMimeType(), std::move(appearance));
        if (!placement.isEmpty())
            setData(screenshotClipboardPlacementNativeMimeType(), std::move(placement));
        setData(QStringLiteral("image/png"), std::move(png));
    }
    QStringList formats() const override {
        auto result = QMimeData::formats();
        result.append(QStringLiteral("application/x-qt-image"));
        return result;
    }
    bool hasFormat(const QString& mime) const override {
        return mime == QStringLiteral("application/x-qt-image") || QMimeData::hasFormat(mime);
    }

  protected:
    QVariant retrieveData(const QString& mime, QMetaType type) const override {
        const auto activity = snow_shot::runtime::RuntimeActivityTracker::shared().acquire();
        if (mime == QStringLiteral("application/x-qt-image")) {
            return snow_shot::image_codec::decode(data(QStringLiteral("image/png")),
                                                  snow::image::Format::png, "clipboard.png");
        }
        return QMimeData::retrieveData(mime, type);
    }
};

bool publishPngClipboard(QClipboard* clipboard, const QByteArray& png, const QByteArray& placement,
                         const QByteArray& appearance) {
#ifdef Q_OS_MACOS
    if (QGuiApplication::platformName() == QStringLiteral("cocoa")) {
        ensureScreenshotClipboardPlacementMimeSupport();
        ensureScreenshotClipboardAppearanceMimeSupport();
        return snow_shot::platform::macos::publishImageClipboard(clipboard, png, placement,
                                                                 appearance);
    }
#endif
    clipboard->setMimeData(new PngClipboardMimeData(png, placement, appearance),
                           QClipboard::Clipboard);
    return true;
}
#endif

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

    ClipboardCommitOperation(QObject* receiver,
                             std::shared_ptr<ScreenshotClipboardCommitState> state, Attempt attempt,
                             ScreenshotClipboardService::CommitCompletion completion)
        : m_receiver(receiver), m_state(std::move(state)), m_attempt(std::move(attempt)),
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
        const auto activity = m_activity;
        if (m_finished) {
            return;
        }
        if (m_state->cancelled.load(std::memory_order_acquire)) {
            ScreenshotClipboardCommitResult result;
            result.failure = ScreenshotClipboardCommitFailure::Cancelled;
            result.attempts = m_attempts;
            finish(result, true);
            return;
        }

        ++m_attempts;
        // Publication emits clipboard signals synchronously. A listener can
        // destroy the receiver and finish this operation during the call.
        // Keep the callable and its payload alive until publication returns.
        const QPointer<ClipboardCommitOperation> guardedOperation(this);
        Attempt publish = m_attempt;
        const ClipboardPublishAttempt attempt = publish();
        publish = {};
        if (guardedOperation.isNull() || m_finished) {
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
        const auto activity = std::move(m_activity);
        m_attempt = {};
        if (const auto scope = m_state->scope.lock()) {
            scope->pending.remove(m_state.get());
        }
        m_state->scope.reset();
        m_state->finished.store(true, std::memory_order_release);
        const bool cancelled = !notify ||
                               !m_state->completionEnabled.load(std::memory_order_acquire) ||
                               result.failure == ScreenshotClipboardCommitFailure::Cancelled;
        snow_shot::diagnostics::logEvent(
            QStringLiteral("snow_shot.clipboard"), QStringLiteral("clipboard.finished"),
            {{QStringLiteral("operation"), m_operation},
             {QStringLiteral("duration_ms"), m_elapsed.isValid() ? m_elapsed.elapsed() : 0},
             {QStringLiteral("count"), result.attempts},
             {QStringLiteral("code"), static_cast<qint64>(result.nativeError)},
             {QStringLiteral("outcome"), cancelled            ? QStringLiteral("cancelled")
                                         : result.succeeded() ? QStringLiteral("succeeded")
                                                              : QStringLiteral("failed")}},
            cancelled || result.succeeded() ? QtInfoMsg : QtWarningMsg);
        auto completion = std::move(m_completion);
        if (notify && m_state->completionEnabled.load(std::memory_order_acquire) &&
            !m_receiver.isNull() && completion) {
            completion(result);
        }
        deleteLater();
    }

    QPointer<QObject> m_receiver;
    std::shared_ptr<ScreenshotClipboardCommitState> m_state;
    Attempt m_attempt;
    ScreenshotClipboardService::CommitCompletion m_completion;
    QElapsedTimer m_elapsed;
    QString m_operation = QUuid::createUuid().toString(QUuid::Id128);
    int m_attempts = 0;
    bool m_finished = false;
    snow_shot::runtime::RuntimeActivityLease m_activity =
        snow_shot::runtime::RuntimeActivityTracker::shared().acquire();
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

void clearGlobalPadding(HGLOBAL handle, void* memory, SIZE_T payloadBytes) {
    const SIZE_T allocatedBytes = GlobalSize(handle);
    if (allocatedBytes > payloadBytes)
        std::memset(static_cast<uchar*>(memory) + payloadBytes, 0, allocatedBytes - payloadBytes);
}

HGLOBAL copyToGlobal(const QByteArray& bytes) {
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(bytes.size()));
    if (handle == nullptr)
        return nullptr;
    void* memory = GlobalLock(handle);
    if (memory == nullptr) {
        GlobalFree(handle);
        return nullptr;
    }
    std::memcpy(memory, bytes.constData(), static_cast<std::size_t>(bytes.size()));
    // Clipboard consumers can inspect the complete GlobalSize, including allocator rounding.
    clearGlobalPadding(handle, memory, static_cast<SIZE_T>(bytes.size()));
    GlobalUnlock(handle);
    return handle;
}

HGLOBAL prepareDib(const ScreenshotImageRowSource& source) {
    SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.prepare_dib");
    const quint64 stride = (static_cast<quint64>(source.size.width()) * 3 + 3) & ~quint64(3);
    const quint64 pixelBytes = stride * static_cast<quint64>(source.size.height());
    const quint64 totalBytes = sizeof(BITMAPINFOHEADER) + pixelBytes;
    if (!source.isValid() || pixelBytes > std::numeric_limits<DWORD>::max() ||
        totalBytes > std::numeric_limits<SIZE_T>::max()) {
        return nullptr;
    }
    constexpr int batchRows = 64;
    const qsizetype rgbaStride = static_cast<qsizetype>(source.size.width()) * 4;
    QByteArray rows(rgbaStride * std::min(batchRows, source.size.height()), Qt::Uninitialized);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(totalBytes));
    if (handle == nullptr)
        return nullptr;
    auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(handle));
    if (header == nullptr) {
        GlobalFree(handle);
        return nullptr;
    }
    const auto release = [](void* memory) {
        GlobalUnlock(static_cast<HGLOBAL>(memory));
        GlobalFree(static_cast<HGLOBAL>(memory));
    };
    std::unique_ptr<void, decltype(release)> allocation(handle, release);
    *header = {};
    clearGlobalPadding(handle, header, static_cast<SIZE_T>(totalBytes));
    header->biSize = sizeof(*header);
    header->biWidth = source.size.width();
    header->biHeight = source.size.height();
    header->biPlanes = 1;
    header->biBitCount = 24;
    header->biCompression = BI_RGB;
    header->biSizeImage = static_cast<DWORD>(pixelBytes);
    auto* pixels = reinterpret_cast<uchar*>(header + 1);
    // Bounded scratch space also supports scrolling sources without materializing a QImage.
    bool succeeded = true;
    for (int first = 0; first < source.size.height();) {
        const int count = std::min(batchRows, source.size.height() - first);
        if ((source.cancellationRequested && source.cancellationRequested()) ||
            !source.readRows(first, count, rgbaStride, reinterpret_cast<uchar*>(rows.data()),
                             rows.size())) {
            succeeded = false;
            break;
        }
        for (int row = 0; row < count; ++row) {
            const auto* input = reinterpret_cast<const uchar*>(rows.constData()) + row * rgbaStride;
            auto* output =
                pixels + static_cast<quint64>(source.size.height() - 1 - first - row) * stride;
            for (int x = 0; x < source.size.width(); ++x) {
                const auto* rgba = input + static_cast<qsizetype>(x) * 4;
                const unsigned alpha = rgba[3];
                if (alpha == 255) {
                    output[static_cast<qsizetype>(x) * 3] = rgba[2];
                    output[static_cast<qsizetype>(x) * 3 + 1] = rgba[1];
                    output[static_cast<qsizetype>(x) * 3 + 2] = rgba[0];
                    continue;
                }
                for (int channel = 0; channel < 3; ++channel) {
                    output[static_cast<qsizetype>(x) * 3 + channel] = static_cast<uchar>(
                        (rgba[2 - channel] * alpha + 255U * (255U - alpha) + 127U) / 255U);
                }
            }
            // Every pixel is written above; only DWORD alignment padding needs clearing.
            std::memset(output + static_cast<qsizetype>(source.size.width()) * 3, 0,
                        static_cast<std::size_t>(stride - source.size.width() * 3ULL));
        }
        first += count;
    }
    if (!succeeded || (source.cancellationRequested && source.cancellationRequested())) {
        return nullptr;
    }
    GlobalUnlock(handle);
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.pixel_bytes", static_cast<qint64>(pixelBytes));
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.dib_bytes", static_cast<qint64>(totalBytes));
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.dib_prepared", 1);
    return static_cast<HGLOBAL>(allocation.release());
}

ClipboardPublishAttempt publishClipboardPayload(void** pngHandle, void** dibHandle,
                                                void** placementHandle, void** appearanceHandle) {
    SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.publish_total");
    if (*pngHandle == nullptr || *dibHandle == nullptr) {
        return {ScreenshotClipboardCommitFailure::InvalidPayload, ERROR_INVALID_DATA};
    }
    const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
    const UINT placementFormat =
        *placementHandle ? RegisterClipboardFormatW(L"SnowShotScreenshotPlacement") : 0;
    const UINT appearanceFormat =
        *appearanceHandle ? RegisterClipboardFormatW(L"SnowShotScreenshotAppearance") : 0;
    const HWND owner = clipboardOwnerWindow();
    if (pngFormat == 0 || (*placementHandle && placementFormat == 0) ||
        (*appearanceHandle && appearanceFormat == 0) || owner == nullptr) {
        return {ScreenshotClipboardCommitFailure::ClipboardUnavailable, GetLastError()};
    }
    if (!OpenClipboard(owner)) {
        return {ScreenshotClipboardCommitFailure::Busy, GetLastError()};
    }
    if (!EmptyClipboard()) {
        const DWORD error = GetLastError();
        CloseClipboard();
        return {ScreenshotClipboardCommitFailure::ClearFailed, error};
    }
    // Ownership transfers separately for each successful SetClipboardData call.
    bool published = SetClipboardData(pngFormat, static_cast<HGLOBAL>(*pngHandle)) != nullptr;
    if (published) {
        *pngHandle = nullptr;
        published = SetClipboardData(CF_DIB, static_cast<HGLOBAL>(*dibHandle)) != nullptr;
        if (published)
            *dibHandle = nullptr;
    }
    if (published && *placementHandle) {
        published =
            SetClipboardData(placementFormat, static_cast<HGLOBAL>(*placementHandle)) != nullptr;
        if (published)
            *placementHandle = nullptr;
    }
    if (published && *appearanceHandle) {
        published =
            SetClipboardData(appearanceFormat, static_cast<HGLOBAL>(*appearanceHandle)) != nullptr;
        if (published)
            *appearanceHandle = nullptr;
    }
    const DWORD error = published ? ERROR_SUCCESS : GetLastError();
    if (!published) {
        // Do not leave a partial multi-format publication behind.
        static_cast<void>(EmptyClipboard());
    }
    CloseClipboard();
    if (!published) {
        return {ScreenshotClipboardCommitFailure::PublishFailed, error};
    }
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
        return QCoreApplication::translate("ScreenshotClipboardService", "The clipboard is busy");
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
    std::shared_ptr<ScreenshotClipboardCommitState> state)
    : m_state(std::move(state)) {}

void ScreenshotClipboardCommitHandle::cancel() const {
    if (m_state != nullptr) {
        m_state->cancelled.store(true, std::memory_order_release);
    }
}

bool ScreenshotClipboardCommitHandle::isValid() const {
    return m_state != nullptr;
}

bool ScreenshotClipboardCommitHandle::isCancellationRequested() const {
    return !isValid() || m_state->cancelled.load(std::memory_order_acquire);
}

bool ScreenshotClipboardCommitHandle::isFinished() const {
    return !isValid() || m_state->finished.load(std::memory_order_acquire);
}

ScreenshotClipboardCommitScope::~ScreenshotClipboardCommitScope() {
    cancelAll();
}

ScreenshotClipboardCommitHandle
ScreenshotClipboardCommitScope::commit(QClipboard* clipboard, QObject* receiver,
                                       ScreenshotClipboardPayload payload,
                                       ScreenshotClipboardService::CommitCompletion completion,
                                       ScreenshotClipboardService::PublicationId publicationId) {
    if (publicationId == 0) {
        publicationId = ScreenshotClipboardService::reservePublication();
    }
    auto handle = ScreenshotClipboardService::commit(clipboard, receiver, std::move(payload),
                                                     publicationId, std::move(completion));
    track(handle);
    return handle;
}

ScreenshotClipboardCommitHandle ScreenshotClipboardCommitScope::commitMimeData(
    QClipboard* clipboard, QObject* receiver, QMimeData* mimeData,
    ScreenshotClipboardService::CommitCompletion completion,
    ScreenshotClipboardService::PublicationId publicationId) {
    if (publicationId == 0) {
        publicationId = ScreenshotClipboardService::reservePublication();
    }
    auto handle = ScreenshotClipboardService::commitMimeData(clipboard, receiver, mimeData,
                                                             publicationId, std::move(completion));
    track(handle);
    return handle;
}

void ScreenshotClipboardCommitScope::track(const ScreenshotClipboardCommitHandle& handle) {
    if (handle.isFinished()) {
        return;
    }
    if (!m_state) {
        m_state = std::make_shared<ScreenshotClipboardCommitScopeState>();
    }
    handle.m_state->scope = m_state;
    m_state->pending.insert(handle.m_state.get(), handle.m_state);
}

void ScreenshotClipboardCommitScope::cancelAll() {
    // Detach the batch so a new publication cannot inherit cancellation or be
    // removed when an older cancelled operation eventually finishes.
    const auto state = std::exchange(m_state, {});
    if (!state) {
        return;
    }
    for (const auto& commit : state->pending) {
        commit->completionEnabled.store(false, std::memory_order_release);
        commit->cancelled.store(true, std::memory_order_release);
        commit->scope.reset();
    }
    state->pending.clear();
}

qsizetype ScreenshotClipboardCommitScope::pendingCount() const {
    return m_state ? m_state->pending.size() : 0;
}

ScreenshotClipboardPayload::~ScreenshotClipboardPayload() {
    reset();
}

void ScreenshotClipboardPayload::reset() noexcept {
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (m_dibHandle != nullptr)
        GlobalFree(static_cast<HGLOBAL>(m_dibHandle));
    if (m_pngHandle != nullptr)
        GlobalFree(static_cast<HGLOBAL>(m_pngHandle));
    if (m_placementHandle != nullptr)
        GlobalFree(static_cast<HGLOBAL>(m_placementHandle));
    m_dibHandle = nullptr;
    m_pngHandle = nullptr;
    if (m_appearanceHandle != nullptr)
        GlobalFree(static_cast<HGLOBAL>(m_appearanceHandle));
    m_placementHandle = nullptr;
    m_appearanceHandle = nullptr;
#endif
    m_pngBytes.clear();
    m_placementBytes.clear();
    m_appearanceBytes.clear();
}

ScreenshotClipboardPayload::ScreenshotClipboardPayload(
    ScreenshotClipboardPayload&& other) noexcept {
    *this = std::move(other);
}

ScreenshotClipboardPayload&
ScreenshotClipboardPayload::operator=(ScreenshotClipboardPayload&& other) noexcept {
    if (this == &other)
        return *this;
    reset();
#if defined(Q_OS_WIN) || defined(_WIN32)
    m_dibHandle = std::exchange(other.m_dibHandle, nullptr);
    m_pngHandle = std::exchange(other.m_pngHandle, nullptr);
    m_placementHandle = std::exchange(other.m_placementHandle, nullptr);
    m_appearanceHandle = std::exchange(other.m_appearanceHandle, nullptr);
#endif
    m_pngBytes = std::move(other.m_pngBytes);
    m_placementBytes = std::move(other.m_placementBytes);
    m_appearanceBytes = std::move(other.m_appearanceBytes);
    return *this;
}

bool ScreenshotClipboardPayload::isValid() const {
#if defined(Q_OS_WIN) || defined(_WIN32)
    return m_dibHandle != nullptr && m_pngHandle != nullptr && !m_pngBytes.isEmpty() &&
           (m_placementBytes.isEmpty() || m_placementHandle != nullptr) &&
           (m_appearanceBytes.isEmpty() || m_appearanceHandle != nullptr);
#else
    return !m_pngBytes.isEmpty();
#endif
}

ScreenshotClipboardPayload
ScreenshotClipboardService::prepare(const ScreenshotImageRowSource& source,
                                    ScreenshotImageEncodingOptions encoding,
                                    std::optional<ScreenshotClipboardPlacement> placement,
                                    std::optional<ScreenshotClipboardAppearance> appearance) {
    SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.prepare_total");
    QByteArray png;
    {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.encode_png");
        QBuffer buffer(&png);
        if (!buffer.open(QIODevice::WriteOnly) ||
            !snow_shot::image_codec::encodeToDevice(
                source, &buffer, snow::image::Format::png,
                ScreenshotImageFileService::encodeOptions(ScreenshotImageFileFormat::Png,
                                                          encoding))) {
            return {};
        }
    }
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.png_encoded", 1);
    return prepareEncoded(source, png, std::move(placement), std::move(appearance));
}

ScreenshotClipboardPayload ScreenshotClipboardService::prepareEncoded(
    const ScreenshotImageRowSource& source, const QByteArray& png,
    std::optional<ScreenshotClipboardPlacement> placement,
    std::optional<ScreenshotClipboardAppearance> appearance) {
    SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.prepare_native");
    if (!source.isValid() || png.isEmpty() ||
        (source.cancellationRequested && source.cancellationRequested())) {
        return {};
    }
    ScreenshotClipboardPayload payload;
    if (placement) {
        placement->rasterSize = source.size;
        payload.m_placementBytes = encodeScreenshotClipboardPlacement(*placement);
    }
    if (appearance) {
        appearance->rasterSize = source.size;
        payload.m_appearanceBytes = encodeScreenshotClipboardAppearance(*appearance);
    }
    payload.m_pngBytes = png;
#if defined(Q_OS_WIN) || defined(_WIN32)
    payload.m_dibHandle = prepareDib(source);
    if (payload.m_dibHandle == nullptr)
        return {};
    payload.m_pngHandle = copyToGlobal(payload.m_pngBytes);
    if (!payload.m_placementBytes.isEmpty())
        payload.m_placementHandle = copyToGlobal(payload.m_placementBytes);
    if (!payload.m_appearanceBytes.isEmpty())
        payload.m_appearanceHandle = copyToGlobal(payload.m_appearanceBytes);
#endif
    if (source.cancellationRequested && source.cancellationRequested())
        return {};
    return payload;
}

ScreenshotClipboardPayload
ScreenshotClipboardService::prepareImage(const QImage& image,
                                         ScreenshotImageEncodingOptions encoding,
                                         std::optional<ScreenshotClipboardPlacement> placement,
                                         std::optional<ScreenshotClipboardAppearance> appearance) {
    return prepare(snow_shot::image_codec::srgbRowSource(image), encoding, std::move(placement),
                   std::move(appearance));
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

ScreenshotClipboardCommitHandle
ScreenshotClipboardService::commit(QClipboard* clipboard, QObject* receiver,
                                   ScreenshotClipboardPayload payload, PublicationId publicationId,
                                   CommitCompletion completion) {
    QCoreApplication* application = QCoreApplication::instance();
    if (receiver == nullptr || !completion || application == nullptr ||
        QThread::currentThread() != application->thread()) {
        return {};
    }

    auto state = std::make_shared<ScreenshotClipboardCommitState>();
    auto sharedPayload = std::make_shared<ScreenshotClipboardPayload>(std::move(payload));
#if defined(Q_OS_WIN) || defined(_WIN32)
    Q_UNUSED(clipboard);
    auto attempt = [sharedPayload, publicationId]() {
        if (publicationId != g_latestPublicationId.load(std::memory_order_acquire)) {
            return ClipboardPublishAttempt{};
        }
        if (!sharedPayload->isValid())
            return ClipboardPublishAttempt{ScreenshotClipboardCommitFailure::InvalidPayload, 0};
        return publishClipboardPayload(&sharedPayload->m_pngHandle, &sharedPayload->m_dibHandle,
                                       &sharedPayload->m_placementHandle,
                                       &sharedPayload->m_appearanceHandle);
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
        const bool published =
            publishPngClipboard(guardedClipboard, sharedPayload->m_pngBytes,
                                sharedPayload->m_placementBytes, sharedPayload->m_appearanceBytes);
        sharedPayload->reset();
        return ClipboardPublishAttempt{published ? ScreenshotClipboardCommitFailure::None
                                                 : ScreenshotClipboardCommitFailure::PublishFailed,
                                       0};
    };
#endif
    auto* operation =
        new ClipboardCommitOperation(receiver, state, std::move(attempt), std::move(completion));
    operation->start();
    return ScreenshotClipboardCommitHandle(std::move(state));
}

ScreenshotClipboardCommitHandle
ScreenshotClipboardService::commitMimeData(QClipboard* clipboard, QObject* receiver,
                                           QMimeData* mimeData, CommitCompletion completion) {
    return commitMimeData(clipboard, receiver, mimeData, reservePublication(),
                          std::move(completion));
}

ScreenshotClipboardCommitHandle
ScreenshotClipboardService::commitMimeData(QClipboard* clipboard, QObject* receiver,
                                           QMimeData* mimeData, PublicationId publicationId,
                                           CommitCompletion completion) {
    QCoreApplication* application = QCoreApplication::instance();
    if (receiver == nullptr || mimeData == nullptr || !completion || application == nullptr ||
        QThread::currentThread() != application->thread()) {
        delete mimeData;
        return {};
    }

    auto state = std::make_shared<ScreenshotClipboardCommitState>();
    auto holder = std::make_shared<std::unique_ptr<QMimeData>>(mimeData);
    const auto placementFormat = screenshotClipboardPlacementNativeMimeType();
    const auto appearanceFormat = screenshotClipboardAppearanceNativeMimeType();
    const auto formats = mimeData->formats();
    const bool fileOnly = mimeData->hasUrls() &&
                          std::all_of(formats.begin(), formats.end(),
                                      [&placementFormat, &appearanceFormat](const QString& format) {
                                          return format == QStringLiteral("text/uri-list") ||
                                                 format == placementFormat ||
                                                 format == appearanceFormat;
                                      });
    const QList<QUrl> fileUrls = fileOnly ? mimeData->urls() : QList<QUrl>{};
    const QByteArray placementBytes = fileOnly ? mimeData->data(placementFormat) : QByteArray{};
    const QByteArray appearanceBytes = fileOnly ? mimeData->data(appearanceFormat) : QByteArray{};
    const auto* temporaryMime = dynamic_cast<TemporaryFileClipboardMimeData*>(mimeData);
    const auto directory = temporaryMime ? temporaryMime->directory() : nullptr;
    const QPointer<QClipboard> guardedClipboard(clipboard);
    auto attempt = [guardedClipboard, holder, publicationId, fileUrls, placementFormat,
                    placementBytes, appearanceFormat, appearanceBytes, directory]() {
        if (publicationId != g_latestPublicationId.load(std::memory_order_acquire)) {
            return ClipboardPublishAttempt{};
        }
        if (guardedClipboard.isNull()) {
            return ClipboardPublishAttempt{ScreenshotClipboardCommitFailure::ClipboardUnavailable,
                                           0};
        }
        if (*holder == nullptr) {
            return ClipboardPublishAttempt{ScreenshotClipboardCommitFailure::InvalidPayload, 0};
        }
        if (!fileUrls.isEmpty()) {
            // Qt owns each attempted MIME object, including failed native publications.
            auto* attemptMime =
                directory ? new TemporaryFileClipboardMimeData(directory) : new QMimeData();
            attemptMime->setUrls(fileUrls);
            if (!placementBytes.isEmpty())
                attemptMime->setData(placementFormat, placementBytes);
            if (!appearanceBytes.isEmpty())
                attemptMime->setData(appearanceFormat, appearanceBytes);
            guardedClipboard->setMimeData(attemptMime, QClipboard::Clipboard);
            if (!guardedClipboard->ownsClipboard() && guardedClipboard->mimeData() != attemptMime) {
                return ClipboardPublishAttempt{ScreenshotClipboardCommitFailure::Busy, 0};
            }
            return ClipboardPublishAttempt{};
        }
        guardedClipboard->setMimeData(holder->release(), QClipboard::Clipboard);
        return ClipboardPublishAttempt{};
    };
    auto* operation =
        new ClipboardCommitOperation(receiver, state, std::move(attempt), std::move(completion));
    operation->start();
    return ScreenshotClipboardCommitHandle(std::move(state));
}

std::unique_ptr<QMimeData>
ScreenshotClipboardService::temporaryFileMimeData(const QString& path,
                                                  std::shared_ptr<QTemporaryDir> directory) {
    auto mime = std::make_unique<TemporaryFileClipboardMimeData>(std::move(directory));
    mime->setUrls({QUrl::fromLocalFile(path)});
    return mime;
}

bool ScreenshotClipboardService::publish(QClipboard* clipboard,
                                         ScreenshotClipboardPayload payload) {
    static_cast<void>(reservePublication());
    if (!payload.isValid())
        return false;
#if defined(Q_OS_WIN) || defined(_WIN32)
    Q_UNUSED(clipboard);
    return publishClipboardPayload(&payload.m_pngHandle, &payload.m_dibHandle,
                                   &payload.m_placementHandle, &payload.m_appearanceHandle)
        .succeeded();
#else
    if (clipboard == nullptr || !payload.isValid()) {
        qWarning("Screenshot clipboard is unavailable");
        return false;
    }
    return publishPngClipboard(clipboard, payload.m_pngBytes, payload.m_placementBytes,
                               payload.m_appearanceBytes);
#endif
}

bool ScreenshotClipboardService::publishImage(QClipboard* clipboard, const QImage& image,
                                              ScreenshotImageEncodingOptions encoding) {
    return publish(clipboard, prepareImage(image, encoding));
}
