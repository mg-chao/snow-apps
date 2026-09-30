#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_shot/diagnostics/diagnostics.h"
#include <QUuid>

#include "screenshotclipboardperfinstrumentation.h"
#include "snowimageqtcodec.h"
#ifdef Q_OS_MACOS
#include "../../platform/macos/imageclipboard.h"
#endif

#include <QClipboard>
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QHash>
#include <QMimeData>
#include <QPointer>
#include <QThread>
#include <QTimer>
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
#if !defined(Q_OS_WIN)
// Keep canonical PNG bytes for native consumers and lazily provide Qt's image
// representation when a local reader requests it. Publishing never decodes.
class PngClipboardMimeData final : public QMimeData {
  public:
    explicit PngClipboardMimeData(QByteArray png, QByteArray placement = {}) {
#ifdef Q_OS_MACOS
        snow_shot::platform::macos::initializeImageClipboardConverter();
#endif
        ensureScreenshotClipboardPlacementMimeSupport();
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
        if (mime == QStringLiteral("application/x-qt-image")) {
            if (m_image.isNull()) {
                m_image = snow_shot::image_codec::decode(data(QStringLiteral("image/png")),
                                                         snow::image::Format::png, "clipboard.png");
            }
            return m_image;
        }
        return QMimeData::retrieveData(mime, type);
    }

  private:
    mutable QImage m_image;
};
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

HGLOBAL copyToGlobal(const QByteArray& bytes) {
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, static_cast<SIZE_T>(bytes.size()));
    if (handle == nullptr)
        return nullptr;
    void* memory = GlobalLock(handle);
    if (memory == nullptr) {
        GlobalFree(handle);
        return nullptr;
    }
    std::memcpy(memory, bytes.constData(), static_cast<std::size_t>(bytes.size()));
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
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, static_cast<SIZE_T>(totalBytes));
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
                for (int channel = 0; channel < 3; ++channel) {
                    output[static_cast<qsizetype>(x) * 3 + channel] = static_cast<uchar>(
                        (rgba[2 - channel] * alpha + 255U * (255U - alpha) + 127U) / 255U);
                }
            }
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
                                                void** placementHandle) {
    SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.publish_total");
    if (*pngHandle == nullptr || *dibHandle == nullptr) {
        return {ScreenshotClipboardCommitFailure::InvalidPayload, ERROR_INVALID_DATA};
    }
    const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
    const UINT placementFormat =
        *placementHandle ? RegisterClipboardFormatW(L"SnowShotScreenshotPlacement") : 0;
    const HWND owner = clipboardOwnerWindow();
    if (pngFormat == 0 || (*placementHandle && placementFormat == 0) || owner == nullptr) {
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
    m_placementHandle = nullptr;
#endif
    m_pngBytes.clear();
    m_placementBytes.clear();
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
#endif
    m_pngBytes = std::move(other.m_pngBytes);
    m_placementBytes = std::move(other.m_placementBytes);
    return *this;
}

bool ScreenshotClipboardPayload::isValid() const {
#if defined(Q_OS_WIN) || defined(_WIN32)
    return m_dibHandle != nullptr && m_pngHandle != nullptr && !m_pngBytes.isEmpty() &&
           (m_placementBytes.isEmpty() || m_placementHandle != nullptr);
#else
    return !m_pngBytes.isEmpty();
#endif
}

ScreenshotClipboardPayload
ScreenshotClipboardService::prepare(const ScreenshotImageRowSource& source,
                                    const QByteArray& canonicalPng,
                                    std::optional<ScreenshotClipboardPlacement> placement) {
    SNOW_SHOT_CLIPBOARD_PERF_SCOPE("clipboard.prepare_total");
    if (!source.isValid() || (source.cancellationRequested && source.cancellationRequested())) {
        return {};
    }
    ScreenshotClipboardPayload payload;
    if (placement) {
        placement->rasterSize = source.size;
        payload.m_placementBytes = encodeScreenshotClipboardPlacement(*placement);
    }
    payload.m_pngBytes =
        canonicalPng.isEmpty() ? snow_shot::image_codec::encodePng(source, 0) : canonicalPng;
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("clipboard.png_encoded", canonicalPng.isEmpty() ? 1 : 0);
    if (payload.m_pngBytes.isEmpty())
        return {};
#if defined(Q_OS_WIN) || defined(_WIN32)
    payload.m_dibHandle = prepareDib(source);
    if (payload.m_dibHandle == nullptr)
        return {};
    payload.m_pngHandle = copyToGlobal(payload.m_pngBytes);
    if (!payload.m_placementBytes.isEmpty())
        payload.m_placementHandle = copyToGlobal(payload.m_placementBytes);
#endif
    if (source.cancellationRequested && source.cancellationRequested())
        return {};
    return payload;
}

ScreenshotClipboardPayload
ScreenshotClipboardService::prepareImage(const QImage& image, const QByteArray& canonicalPng,
                                         std::optional<ScreenshotClipboardPlacement> placement) {
    return prepare(snow_shot::image_codec::srgbRowSource(image), canonicalPng,
                   std::move(placement));
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
                                       &sharedPayload->m_placementHandle);
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
        auto* mime =
            new PngClipboardMimeData(sharedPayload->m_pngBytes, sharedPayload->m_placementBytes);
        guardedClipboard->setMimeData(mime, QClipboard::Clipboard);
        sharedPayload->reset();
        return ClipboardPublishAttempt{};
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
    const auto formats = mimeData->formats();
    const bool fileOnly =
        mimeData->hasUrls() &&
        std::all_of(formats.begin(), formats.end(), [&placementFormat](const QString& format) {
            return format == QStringLiteral("text/uri-list") || format == placementFormat;
        });
    const QList<QUrl> fileUrls = fileOnly ? mimeData->urls() : QList<QUrl>{};
    const QByteArray placementBytes = fileOnly ? mimeData->data(placementFormat) : QByteArray{};
    const QPointer<QClipboard> guardedClipboard(clipboard);
    auto attempt = [guardedClipboard, holder, publicationId, fileUrls, placementFormat,
                    placementBytes]() {
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
            auto* attemptMime = new QMimeData();
            attemptMime->setUrls(fileUrls);
            if (!placementBytes.isEmpty())
                attemptMime->setData(placementFormat, placementBytes);
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
                                   &payload.m_placementHandle)
        .succeeded();
#else
    if (clipboard == nullptr || !payload.isValid()) {
        qWarning("Screenshot clipboard is unavailable");
        return false;
    }
    auto* mime = new PngClipboardMimeData(payload.m_pngBytes, payload.m_placementBytes);
    clipboard->setMimeData(mime, QClipboard::Clipboard);
    return true;
#endif
}

bool ScreenshotClipboardService::publishImage(QClipboard* clipboard, const QImage& image) {
    return publish(clipboard, prepareImage(image));
}
