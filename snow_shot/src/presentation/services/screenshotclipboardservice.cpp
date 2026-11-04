#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_shot/runtime/runtimeactivitytracker.h"
#include "snow_shot/diagnostics/diagnostics.h"
#include <QUuid>

#include "screenshotclipboardperfinstrumentation.h"
#include "screenshotclipboarddibwriter.h"
#include "screenshotclipboardfilepayload.h"
#include "snowimageqtcodec.h"
#ifdef Q_OS_MACOS
#include "../../platform/macos/imageclipboard.h"
#endif

#include <QClipboard>
#include <QBuffer>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QHash>
#include <QGuiApplication>
#include <QMimeData>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
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
#if !defined(Q_OS_WIN)
// Keep canonical PNG bytes for native consumers and lazily provide Qt's image
// representation when a reader requests it. Decoded pixels belong to that reader.
class PngClipboardMimeData final : public QMimeData {
  public:
    explicit PngClipboardMimeData(QByteArray png, QByteArray placement = {},
                                  QByteArray appearance = {}, const QString& filePath = {}) {
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
        if (!filePath.isEmpty())
            setUrls({QUrl::fromLocalFile(filePath)});
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
                         const QByteArray& appearance, const QString& filePath) {
#ifdef Q_OS_MACOS
    if (QGuiApplication::platformName() == QStringLiteral("cocoa")) {
        ensureScreenshotClipboardPlacementMimeSupport();
        ensureScreenshotClipboardAppearanceMimeSupport();
        return snow_shot::platform::macos::publishImageClipboard(clipboard, png, placement,
                                                                 appearance, filePath);
    }
#endif
    clipboard->setMimeData(new PngClipboardMimeData(png, placement, appearance, filePath),
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
    bool superseded = false;

    [[nodiscard]] bool succeeded() const {
        return failure == ScreenshotClipboardCommitFailure::None && !superseded;
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
        if (attempt.superseded) {
            ScreenshotClipboardCommitResult result;
            result.attempts = m_attempts;
            result.superseded = true;
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
        const bool superseded = result.superseded;
        snow_shot::diagnostics::logEvent(
            QStringLiteral("snow_shot.clipboard"), QStringLiteral("clipboard.finished"),
            {{QStringLiteral("operation"), m_operation},
             {QStringLiteral("duration_ms"), m_elapsed.isValid() ? m_elapsed.elapsed() : 0},
             {QStringLiteral("count"), result.attempts},
             {QStringLiteral("code"), static_cast<qint64>(result.nativeError)},
             {QStringLiteral("outcome"), cancelled            ? QStringLiteral("cancelled")
                                         : superseded           ? QStringLiteral("superseded")
                                         : result.succeeded()   ? QStringLiteral("succeeded")
                                                                : QStringLiteral("failed")}},
            cancelled || superseded || result.succeeded() ? QtInfoMsg : QtWarningMsg);
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
#include <ShlObj.h>

namespace snow_shot::presentation::clipboard_detail {
WindowsClipboardFileData encodeWindowsClipboardFiles(const QList<QUrl>& urls) {
    if (urls.isEmpty())
        return {};
    QString paths;
    for (const auto& url : urls) {
        const QString path = QDir::toNativeSeparators(url.toLocalFile());
        if (!url.isLocalFile() || path.isEmpty() || path.contains(QChar(u'\0')))
            return {};
        paths += path;
        paths += QChar(u'\0');
    }
    paths += QChar(u'\0');
    constexpr qsizetype headerBytes = sizeof(DROPFILES);
    if (paths.size() > (std::numeric_limits<qsizetype>::max() - headerBytes) / 2)
        return {};
    WindowsClipboardFileData data;
    data.paths = QByteArray(headerBytes + paths.size() * 2, '\0');
    DROPFILES header{};
    header.pFiles = sizeof(DROPFILES);
    header.fWide = TRUE;
    std::memcpy(data.paths.data(), &header, sizeof(header));
    std::memcpy(data.paths.data() + headerBytes, paths.utf16(), size_t(paths.size()) * 2);
    const QString firstUrl = urls.first().toString();
    data.wideUrl = QByteArray(reinterpret_cast<const char*>(firstUrl.utf16()), firstUrl.size() * 2);
    data.wideUrl.append(2, '\0');
    data.url = firstUrl.toLocal8Bit();
    data.url.append('\0');
    data.preferredDropEffect = QByteArray(sizeof(DWORD), '\0');
    qToLittleEndian(quint32(DROPEFFECT_COPY), data.preferredDropEffect.data());
    return data;
}
} // namespace snow_shot::presentation::clipboard_detail

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

HGLOBAL cloneGlobal(HGLOBAL source) {
    const SIZE_T bytes = GlobalSize(source);
    const void* input = bytes ? GlobalLock(source) : nullptr;
    if (!input)
        return nullptr;
    HGLOBAL result = GlobalAlloc(GMEM_MOVEABLE, bytes);
    void* output = result ? GlobalLock(result) : nullptr;
    if (output) {
        std::memcpy(output, input, bytes);
        GlobalUnlock(result);
    } else if (result) {
        GlobalFree(result);
        result = nullptr;
    }
    GlobalUnlock(source);
    return result;
}

class DibWriter final {
  public:
    explicit DibWriter(QSize size, bool opaque) : m_size(size), m_opaque(opaque) {
        if (size.isEmpty())
            return;
        m_stride = (quint64(size.width()) * 3 + 3) & ~quint64{3};
        const quint64 pixelBytes = m_stride * quint64(size.height());
        const quint64 totalBytes = sizeof(BITMAPINFOHEADER) + pixelBytes;
        if (pixelBytes > std::numeric_limits<DWORD>::max() ||
            totalBytes > std::numeric_limits<SIZE_T>::max())
            return;
        m_handle = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(totalBytes));
        if (!m_handle)
            return;
        auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(m_handle));
        if (!header) {
            GlobalFree(m_handle);
            m_handle = nullptr;
            return;
        }
        *header = {};
        clearGlobalPadding(m_handle, header, static_cast<SIZE_T>(totalBytes));
        header->biSize = sizeof(*header);
        header->biWidth = size.width();
        header->biHeight = size.height();
        header->biPlanes = 1;
        header->biBitCount = 24;
        header->biCompression = BI_RGB;
        header->biSizeImage = static_cast<DWORD>(pixelBytes);
        m_pixels = reinterpret_cast<uchar*>(header + 1);
    }
    ~DibWriter() {
        if (m_handle) {
            GlobalUnlock(m_handle);
            GlobalFree(m_handle);
        }
    }
    DibWriter(const DibWriter&) = delete;
    DibWriter& operator=(const DibWriter&) = delete;
    bool isValid() const {
        return m_pixels != nullptr;
    }
    bool write(int first, int count, qsizetype stride, const uchar* input, qsizetype capacity) {
        const qsizetype rowBytes = qsizetype(m_size.width()) * 4;
        if (!isValid() || !input || first < 0 || count <= 0 || count > m_size.height() ||
            first > m_size.height() - count || stride < rowBytes || capacity < rowBytes ||
            qsizetype(count - 1) > (capacity - rowBytes) / stride)
            return false;
        for (int row = 0; row < count; ++row) {
            const auto* source = input + qsizetype(row) * stride;
            auto* output = m_pixels + quint64(m_size.height() - 1 - first - row) * m_stride;
            snow_shot::presentation::clipboard_detail::writeBgr(source, output, m_size.width(),
                                                                m_opaque);
            std::memset(output + qsizetype(m_size.width()) * 3, 0,
                        size_t(m_stride - quint64(m_size.width()) * 3));
        }
        return true;
    }
    HGLOBAL finish() {
        if (!m_handle)
            return nullptr;
        GlobalUnlock(m_handle);
        m_pixels = nullptr;
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.pixel_bytes",
                                         qint64(m_stride) * m_size.height());
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.dib_bytes",
                                         qint64(sizeof(BITMAPINFOHEADER)) +
                                             qint64(m_stride) * m_size.height());
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.dib_prepared", 1);
        return std::exchange(m_handle, nullptr);
    }

  private:
    QSize m_size;
    bool m_opaque = false;
    quint64 m_stride = 0;
    HGLOBAL m_handle = nullptr;
    uchar* m_pixels = nullptr;
};

HGLOBAL prepareDib(const ScreenshotImageRowSource& source) {
    SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.prepare_dib");
    if (!source.isValid())
        return nullptr;
    DibWriter dib(source.size, source.hasVerifiedOpaquePixels());
    if (!dib.isValid())
        return nullptr;
    constexpr int batchRows = 64;
    const qsizetype rgbaStride = static_cast<qsizetype>(source.size.width()) * 4;
    QByteArray rows(rgbaStride * std::min(batchRows, source.size.height()), Qt::Uninitialized);
    // Bounded scratch space also supports scrolling sources without materializing a QImage.
    bool succeeded = true;
    for (int first = 0; first < source.size.height();) {
        const int count = std::min(batchRows, source.size.height() - first);
        if ((source.cancellationRequested && source.cancellationRequested()) ||
            !source.readRows(first, count, rgbaStride, reinterpret_cast<uchar*>(rows.data()),
                             rows.size()) ||
            !dib.write(first, count, rgbaStride, reinterpret_cast<const uchar*>(rows.constData()),
                       rows.size())) {
            succeeded = false;
            break;
        }
        first += count;
    }
    if (!succeeded || (source.cancellationRequested && source.cancellationRequested())) {
        return nullptr;
    }
    return dib.finish();
}

struct NativeClipboardFormat {
    UINT format;
    void** handle;
};

ClipboardPublishAttempt publishClipboardFormats(std::span<const NativeClipboardFormat> formats) {
    SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.publish_total");
    const HWND owner = clipboardOwnerWindow();
    if (owner == nullptr || std::any_of(formats.begin(), formats.end(), [](const auto& entry) {
            return *entry.handle && entry.format == 0;
        })) {
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
    bool published = true;
    for (const auto& entry : formats) {
        if (*entry.handle) {
            if (SetClipboardData(entry.format, static_cast<HGLOBAL>(*entry.handle)) == nullptr) {
                published = false;
                break;
            }
            *entry.handle = nullptr;
        }
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

ClipboardPublishAttempt publishClipboardPayload(void** pngHandle, void** dibHandle,
                                                void** placementHandle, void** appearanceHandle,
                                                void** fileHandle) {
    if (*pngHandle == nullptr || *dibHandle == nullptr)
        return {ScreenshotClipboardCommitFailure::InvalidPayload, ERROR_INVALID_DATA};
    const std::array formats{
        NativeClipboardFormat{RegisterClipboardFormatW(L"PNG"), pngHandle},
        NativeClipboardFormat{CF_DIB, dibHandle},
        NativeClipboardFormat{
            *placementHandle ? RegisterClipboardFormatW(L"SnowShotScreenshotPlacement") : 0,
            placementHandle},
        NativeClipboardFormat{
            *appearanceHandle ? RegisterClipboardFormatW(L"SnowShotScreenshotAppearance") : 0,
            appearanceHandle},
        NativeClipboardFormat{CF_HDROP, fileHandle}};
    return publishClipboardFormats(formats);
}

struct NativeClipboardFiles {
    std::array<void*, 6> handles{};
    bool valid = false;
    ~NativeClipboardFiles() {
        for (void* handle : handles) {
            if (handle)
                GlobalFree(static_cast<HGLOBAL>(handle));
        }
    }
};

std::shared_ptr<NativeClipboardFiles> prepareClipboardFiles(const QList<QUrl>& urls,
                                                            const QByteArray& placement,
                                                            const QByteArray& appearance) {
    auto payload = std::make_shared<NativeClipboardFiles>();
    const auto data = snow_shot::presentation::clipboard_detail::encodeWindowsClipboardFiles(urls);
    if (!data.isValid())
        return payload;
    const std::array bytes{&data.paths, &data.wideUrl, &data.url, &data.preferredDropEffect,
                           &placement,  &appearance};
    payload->valid = true;
    for (size_t index = 0; index < bytes.size(); ++index) {
        if (!bytes[index]->isEmpty()) {
            payload->handles[index] = copyToGlobal(*bytes[index]);
            payload->valid = payload->valid && payload->handles[index] != nullptr;
        }
    }
    return payload;
}

ClipboardPublishAttempt publishClipboardFiles(NativeClipboardFiles& payload) {
    if (!payload.valid)
        return {ScreenshotClipboardCommitFailure::InvalidPayload, ERROR_INVALID_DATA};
    const std::array formats{
        NativeClipboardFormat{CF_HDROP, &payload.handles[0]},
        NativeClipboardFormat{RegisterClipboardFormatW(L"UniformResourceLocatorW"),
                              &payload.handles[1]},
        NativeClipboardFormat{RegisterClipboardFormatW(L"UniformResourceLocator"),
                              &payload.handles[2]},
        NativeClipboardFormat{RegisterClipboardFormatW(L"Preferred DropEffect"),
                              &payload.handles[3]},
        NativeClipboardFormat{
            payload.handles[4] ? RegisterClipboardFormatW(L"SnowShotScreenshotPlacement") : 0,
            &payload.handles[4]},
        NativeClipboardFormat{
            payload.handles[5] ? RegisterClipboardFormatW(L"SnowShotScreenshotAppearance") : 0,
            &payload.handles[5]}};
    return publishClipboardFormats(formats);
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
    if (m_fileHandle != nullptr)
        GlobalFree(static_cast<HGLOBAL>(m_fileHandle));
    m_fileHandle = nullptr;
#endif
    m_pngBytes.clear();
    m_placementBytes.clear();
    m_appearanceBytes.clear();
    m_filePath.clear();
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
    m_fileHandle = std::exchange(other.m_fileHandle, nullptr);
#endif
    m_pngBytes = std::move(other.m_pngBytes);
    m_placementBytes = std::move(other.m_placementBytes);
    m_appearanceBytes = std::move(other.m_appearanceBytes);
    m_filePath = std::move(other.m_filePath);
    return *this;
}

bool ScreenshotClipboardPayload::isValid() const {
#if defined(Q_OS_WIN) || defined(_WIN32)
    return m_dibHandle != nullptr && m_pngHandle != nullptr && !m_pngBytes.isEmpty() &&
           (m_placementBytes.isEmpty() || m_placementHandle != nullptr) &&
           (m_appearanceBytes.isEmpty() || m_appearanceHandle != nullptr) &&
           (m_filePath.isEmpty() || m_fileHandle != nullptr);
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
    if (!source.isValid() || (source.cancellationRequested && source.cancellationRequested()))
        return {};
    ScreenshotImageRowSource input = source;
    input.isKnownOpaque = source.hasVerifiedOpaquePixels();
#if defined(Q_OS_WIN) || defined(_WIN32)
    DibWriter dib(source.size, input.isKnownOpaque);
    if (dib.isValid()) {
        input.readRows = [&source, &dib](int first, int count, qsizetype stride, uchar* destination,
                                         qsizetype capacity) {
            return source.readRows(first, count, stride, destination, capacity) &&
                   dib.write(first, count, stride, destination, capacity);
        };
    }
#endif
    QByteArray png;
    {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.encode_png");
        QBuffer buffer(&png);
        if (!buffer.open(QIODevice::WriteOnly) ||
            !snow_shot::image_codec::encodeToDevice(
                input, &buffer, snow::image::Format::png,
                ScreenshotImageFileService::encodeOptions(ScreenshotImageFileFormat::Png,
                                                          encoding))) {
            return {};
        }
    }
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.png_encoded", 1);
    ScreenshotClipboardPayload payload;
    payload.m_pngBytes = std::move(png);
    if (placement) {
        placement->rasterSize = source.size;
        payload.m_placementBytes = encodeScreenshotClipboardPlacement(*placement);
    }
    if (appearance) {
        appearance->rasterSize = source.size;
        payload.m_appearanceBytes = encodeScreenshotClipboardAppearance(*appearance);
    }
#if defined(Q_OS_WIN) || defined(_WIN32)
    payload.m_dibHandle = dib.finish();
    // Retain successfully encoded bytes even if native allocation fails. Concurrent PNG
    // consumers can still use their encoding; the clipboard payload reports invalid ownership.
    if (payload.m_dibHandle) {
        payload.m_pngHandle = copyToGlobal(payload.m_pngBytes);
        if (!payload.m_placementBytes.isEmpty())
            payload.m_placementHandle = copyToGlobal(payload.m_placementBytes);
        if (!payload.m_appearanceBytes.isEmpty())
            payload.m_appearanceHandle = copyToGlobal(payload.m_appearanceBytes);
    }
#endif
    if (source.cancellationRequested && source.cancellationRequested())
        return {};
    return payload;
}

ScreenshotClipboardPayload
ScreenshotClipboardService::clonePrepared(const ScreenshotClipboardPayload& source) {
    if (!source.isValid())
        return {};
    ScreenshotClipboardPayload result;
    result.m_pngBytes = source.m_pngBytes;
    result.m_placementBytes = source.m_placementBytes;
    result.m_appearanceBytes = source.m_appearanceBytes;
    result.m_filePath = source.m_filePath;
#if defined(Q_OS_WIN) || defined(_WIN32)
    result.m_dibHandle = cloneGlobal(static_cast<HGLOBAL>(source.m_dibHandle));
    result.m_pngHandle = copyToGlobal(result.m_pngBytes);
    if (!result.m_placementBytes.isEmpty())
        result.m_placementHandle = copyToGlobal(result.m_placementBytes);
    if (!result.m_appearanceBytes.isEmpty())
        result.m_appearanceHandle = copyToGlobal(result.m_appearanceBytes);
    if (source.m_fileHandle)
        result.m_fileHandle = cloneGlobal(static_cast<HGLOBAL>(source.m_fileHandle));
#endif
    return result;
}

ScreenshotClipboardPayload ScreenshotClipboardService::withFile(ScreenshotClipboardPayload payload,
                                                                const QString& path) {
    if (!payload.isValid() || !QDir::isAbsolutePath(path) || path.contains(QChar::Null))
        return {};
    payload.m_filePath = QDir::fromNativeSeparators(path);
#if defined(Q_OS_WIN) || defined(_WIN32)
    const QString nativePath = QDir::toNativeSeparators(path);
    // CF_HDROP carries a double-NUL-terminated UTF-16 list after DROPFILES.
    QByteArray bytes(sizeof(DROPFILES) + (nativePath.size() + 2) * sizeof(wchar_t), '\0');
    DROPFILES header{};
    header.pFiles = sizeof(DROPFILES);
    header.fWide = TRUE;
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), nativePath.utf16(),
                static_cast<size_t>(nativePath.size()) * sizeof(wchar_t));
    if (payload.m_fileHandle)
        GlobalFree(static_cast<HGLOBAL>(payload.m_fileHandle));
    payload.m_fileHandle = copyToGlobal(bytes);
    if (!payload.isValid())
        return {};
#endif
    return payload;
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
            return ClipboardPublishAttempt{ScreenshotClipboardCommitFailure::None, 0, true};
        }
        if (!sharedPayload->isValid())
            return ClipboardPublishAttempt{ScreenshotClipboardCommitFailure::InvalidPayload, 0};
        return publishClipboardPayload(&sharedPayload->m_pngHandle, &sharedPayload->m_dibHandle,
                                       &sharedPayload->m_placementHandle,
                                       &sharedPayload->m_appearanceHandle,
                                       &sharedPayload->m_fileHandle);
    };
#else
    const QPointer<QClipboard> guardedClipboard(clipboard);
    auto attempt = [guardedClipboard, sharedPayload, publicationId]() {
        if (publicationId != g_latestPublicationId.load(std::memory_order_acquire)) {
            return ClipboardPublishAttempt{ScreenshotClipboardCommitFailure::None, 0, true};
        }
        if (guardedClipboard.isNull()) {
            return ClipboardPublishAttempt{ScreenshotClipboardCommitFailure::ClipboardUnavailable,
                                           0};
        }
        if (!sharedPayload->isValid()) {
            return ClipboardPublishAttempt{ScreenshotClipboardCommitFailure::InvalidPayload, 0};
        }
        const bool published = publishPngClipboard(
            guardedClipboard, sharedPayload->m_pngBytes, sharedPayload->m_placementBytes,
            sharedPayload->m_appearanceBytes, sharedPayload->m_filePath);
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
    ClipboardCommitOperation::Attempt nativeFilePublication;
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (!fileUrls.isEmpty() && QGuiApplication::platformName() == QStringLiteral("windows") &&
        std::all_of(fileUrls.begin(), fileUrls.end(),
                    [](const QUrl& url) { return url.isLocalFile(); })) {
        // Qt's OleSetClipboard retries sleep on the GUI thread. Use the same nonblocking native
        // transaction as image publication so the operation owns retry timing and cancellation.
        auto files = prepareClipboardFiles(fileUrls, placementBytes, appearanceBytes);
        nativeFilePublication = [files] { return publishClipboardFiles(*files); };
    }
#endif
    const QPointer<QClipboard> guardedClipboard(clipboard);
    auto attempt = [guardedClipboard, holder, publicationId, fileUrls, placementFormat,
                    placementBytes, appearanceFormat, appearanceBytes,
                    nativeFilePublication = std::move(nativeFilePublication)]() {
        if (publicationId != g_latestPublicationId.load(std::memory_order_acquire)) {
            return ClipboardPublishAttempt{ScreenshotClipboardCommitFailure::None, 0, true};
        }
        if (guardedClipboard.isNull()) {
            return ClipboardPublishAttempt{ScreenshotClipboardCommitFailure::ClipboardUnavailable,
                                           0};
        }
        if (*holder == nullptr) {
            return ClipboardPublishAttempt{ScreenshotClipboardCommitFailure::InvalidPayload, 0};
        }
        if (nativeFilePublication)
            return nativeFilePublication();
        if (!fileUrls.isEmpty()) {
            // Qt owns each attempted MIME object, including failed native publications.
            auto* attemptMime = new QMimeData();
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

bool ScreenshotClipboardService::publish(QClipboard* clipboard,
                                         ScreenshotClipboardPayload payload) {
    static_cast<void>(reservePublication());
    if (!payload.isValid())
        return false;
#if defined(Q_OS_WIN) || defined(_WIN32)
    Q_UNUSED(clipboard);
    return publishClipboardPayload(&payload.m_pngHandle, &payload.m_dibHandle,
                                   &payload.m_placementHandle, &payload.m_appearanceHandle,
                                   &payload.m_fileHandle)
        .succeeded();
#else
    if (clipboard == nullptr || !payload.isValid()) {
        qWarning("Screenshot clipboard is unavailable");
        return false;
    }
    return publishPngClipboard(clipboard, payload.m_pngBytes, payload.m_placementBytes,
                               payload.m_appearanceBytes, payload.m_filePath);
#endif
}

bool ScreenshotClipboardService::publishImage(QClipboard* clipboard, const QImage& image,
                                              ScreenshotImageEncodingOptions encoding) {
    return publish(clipboard, prepareImage(image, encoding));
}
