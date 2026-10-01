#include "recordingrenderjob.h"
#include "snow_shot/platform/applicationqos.h"
#include "widgets/button.h"
#include "widgets/modal.h"
#include "widgets/progress.h"
#include <QCoreApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <optional>
#include <thread>

namespace {
QString renderText(const char* source) {
    return QCoreApplication::translate("RecordingRenderDialog", source);
}
QString nativeError() {
    const char* error = snow_recording_last_error_message();
    return QString::fromUtf8(error ? error : "");
}
template <typename Read> QString nativeString(Read read) {
    const size_t required = read(nullptr, 0);
    if (required == 0 || required > 1024 * 1024)
        return {};
    QByteArray bytes(static_cast<qsizetype>(required), '\0');
    read(bytes.data(), required);
    return QString::fromUtf8(bytes.constData());
}
struct StartResult {
    SnowRecordingRenderTask* task = nullptr;
    QString error;
};
struct CleanupResult {
    QString error;
    SnowRecordingSource* retainedSource = nullptr;
};
} // namespace

struct RecordingRenderJob::Impl {
    explicit Impl(RecordingRenderJob& owner, SnowRecordingSource* source, bool visible,
                  QScreen* screen, const QRect& anchorGeometry)
        : owner(owner), source(source), visible(visible), screen(screen),
          anchorGeometry(anchorGeometry) {
        path = nativeString([source](char* buffer, size_t capacity) {
            return snow_recording_source_path(source, buffer, capacity);
        });
        timer.setInterval(100);
        QObject::connect(&timer, &QTimer::timeout, &owner, [this] { poll(); });
    }
    ~Impl() {
        timer.stop();
        if (modal) {
            modal->close();
            delete modal;
        }
        if (!startFuture.valid() && !cleanupFuture.valid() && !cancelFuture.valid() && !task &&
            !source)
            return;
        // A controller can disappear while creation, rendering, or cleanup is
        // pending. Keep source ownership behind the last native operation.
        std::thread([start = std::move(startFuture), cleanup = std::move(cleanupFuture),
                     cancellation = std::move(cancelFuture), task = task,
                     source = source]() mutable {
            snow_shot::platform::applyApplicationQoSToCurrentThread();
            if (start.valid()) {
                const auto result = start.get();
                if (result.task)
                    task = result.task;
            }
            if (cleanup.valid()) {
                const auto result = cleanup.get();
                if (result.retainedSource)
                    snow_recording_source_destroy(result.retainedSource);
            }
            if (cancellation.valid())
                cancellation.get();
            if (task)
                snow_recording_render_task_destroy(task);
            if (source)
                snow_recording_source_destroy(source);
        }).detach();
    }
    void notify() {
        refresh();
        if (owner.changed)
            owner.changed();
    }
    void setError(QString error, const char* fallback = nullptr) {
        errorTranslation = error.isEmpty() ? fallback : nullptr;
        lastError = errorTranslation ? renderText(errorTranslation) : std::move(error);
    }
    void ensureModal() {
        if (!visible || modal)
            return;
        using namespace adqt::widgets;
        modal = new AdModal(&owner);
        modal->setObjectName(QStringLiteral("screenRecordingRenderModal"));
        modal->setMode(AdModal::Mode::Window);
        modal->setWindowModeDetached(true);
        modal->setWindowScreen(screen);
        modal->setWindowAnchorGeometry(anchorGeometry);
        modal->setWindowModality(Qt::ApplicationModal);
        // Detached dialogs cannot inherit the recording windows' topmost layer from an owner.
        modal->setWindowAlwaysOnTop(true);
        modal->setCentered(true);
        modal->setPreferredWidth(500);
        modal->setMaskVisible(false);
        modal->setCloseOnMaskClick(false);
        modal->setClosePolicy(AdModal::ClosePolicy::Manual);
        modal->setStandardButtons(AdModal::StandardButton::NoButton);
        auto* body = new QWidget;
        body->installEventFilter(&owner);
        auto* layout = new QVBoxLayout(body);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(12);
        status = new QLabel(body);
        status->setTextFormat(Qt::PlainText);
        status->setWordWrap(true);
        layout->addWidget(status);
        progress = new AdProgress(body);
        progress->setObjectName(QStringLiteral("screenRecordingRenderProgress"));
        progress->setType(AdProgress::Type::Line);
        progress->setAnimationEnabled(false);
        layout->addWidget(progress);
        details = new QLabel(body);
        details->setTextFormat(Qt::PlainText);
        details->setWordWrap(true);
        details->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        layout->addWidget(details);
        modal->setContentWidget(body);
        auto* footer = new QWidget;
        auto* actions = new QHBoxLayout(footer);
        actions->setContentsMargins(0, 0, 0, 0);
        actions->addStretch();
        cancelButton = new AdButton(footer);
        retryButton = new AdButton(footer);
        keepButton = new AdButton(footer);
        discardButton = new AdButton(footer);
        discardButton->setAccentRole(AdButton::AccentRole::Danger);
        cancelButton->setObjectName(QStringLiteral("screenRecordingRenderCancel"));
        retryButton->setObjectName(QStringLiteral("screenRecordingRenderRetry"));
        keepButton->setObjectName(QStringLiteral("screenRecordingRenderKeepSource"));
        discardButton->setObjectName(QStringLiteral("screenRecordingRenderDiscard"));
        retryButton->setButtonStyle(AdButton::ButtonStyle::Solid);
        retryButton->setAccentRole(AdButton::AccentRole::Primary);
        for (auto* button : {cancelButton, retryButton, keepButton, discardButton})
            actions->addWidget(button);
        modal->setFooterWidget(footer);
        QObject::connect(cancelButton, &AdButton::clicked, &owner,
                         [this] { static_cast<void>(owner.cancel()); });
        QObject::connect(retryButton, &AdButton::clicked, &owner,
                         [this] { static_cast<void>(owner.retry()); });
        QObject::connect(keepButton, &AdButton::clicked, &owner,
                         [this] { static_cast<void>(owner.release(false)); });
        QObject::connect(discardButton, &AdButton::clicked, &owner,
                         [this] { static_cast<void>(owner.release(true)); });
        QObject::connect(modal, &AdModal::closeRequested, &owner, [this](AdModal::CloseReason) {
            if (retained)
                static_cast<void>(owner.release(false));
            else
                static_cast<void>(owner.cancel());
        });
        refresh();
    }
    void refresh() {
        if (!modal)
            return;
        using adqt::widgets::AdProgress;
        modal->setWindowTitle(
            renderText(QT_TRANSLATE_NOOP("RecordingRenderDialog", "Rendering recording")));
        progress->setAccessibleName(
            renderText(QT_TRANSLATE_NOOP("RecordingRenderDialog", "Rendering progress")));
        progress->setPercent(std::floor(percent));
        progress->setStatus(retained && !lastError.isEmpty() ? AdProgress::Status::Exception
                            : terminalSucceeded              ? AdProgress::Status::Success
                                                             : AdProgress::Status::Active);
        const char* text =
            retained ? (lastError.isEmpty()
                            ? QT_TRANSLATE_NOOP("RecordingRenderDialog", "Rendering canceled")
                            : QT_TRANSLATE_NOOP("RecordingRenderDialog", "Rendering failed"))
            : cancelRequested
                ? QT_TRANSLATE_NOOP("RecordingRenderDialog", "Cancelling rendering...")
            : stage == SNOW_RECORDING_RENDER_STAGE_PREPARE
                ? QT_TRANSLATE_NOOP("RecordingRenderDialog", "Preparing recording...")
            : stage == SNOW_RECORDING_RENDER_STAGE_FINALIZE
                ? QT_TRANSLATE_NOOP("RecordingRenderDialog", "Finalizing recording...")
                : QT_TRANSLATE_NOOP("RecordingRenderDialog", "Rendering video...");
        status->setText(renderText(text));
        details->setVisible(retained);
        details->setText((lastError.isEmpty() ? QString() : lastError + QStringLiteral("\n\n")) +
                         renderText(QT_TRANSLATE_NOOP("RecordingRenderDialog",
                                                      "Source files are preserved in:\n%1"))
                             .arg(path));
        cancelButton->setText(renderText(QT_TRANSLATE_NOOP("RecordingRenderDialog", "Cancel")));
        retryButton->setText(renderText(QT_TRANSLATE_NOOP("RecordingRenderDialog", "Retry")));
        keepButton->setText(renderText(QT_TRANSLATE_NOOP("RecordingRenderDialog", "Keep Source")));
        discardButton->setText(renderText(QT_TRANSLATE_NOOP("RecordingRenderDialog", "Discard")));
        cancelButton->setVisible(!retained);
        cancelButton->setEnabled(!cancelRequested && !cleanupFuture.valid());
        for (auto* button : {retryButton, keepButton, discardButton}) {
            button->setVisible(retained);
            button->setEnabled(!cleanupFuture.valid());
        }
        modal->setInitialFocusWidget(retained ? retryButton : cancelButton);
    }
    void start() {
        retained = false;
        cancelRequested = false;
        percent = 0;
        stage = SNOW_RECORDING_RENDER_STAGE_PREPARE;
        lastError.clear();
        errorTranslation = nullptr;
        startFuture = std::async(std::launch::async, [source = source] {
            snow_shot::platform::applyApplicationQoSToCurrentThread();
            StartResult result;
            if (snow_recording_source_render_start(source, &result.task) !=
                SNOW_RECORDING_RESULT_OK)
                result.error = nativeError();
            return result;
        });
        timer.start();
        ensureModal();
        notify();
        if (modal)
            modal->open();
    }
    void retain(QString error) {
        retained = true;
        if (error != lastError)
            setError(std::move(error));
        timer.stop();
        notify();
        // Reconcile the natural window height after adding error/source details
        // and switching footer actions. Percentage-only updates do not resize it.
        if (modal)
            modal->open();
    }
    void disposeTask() {
        if (!task)
            return;
        auto* retiring = task;
        task = nullptr;
        cleanupFuture = std::async(std::launch::async,
                                   [retiring, cancellation = std::move(cancelFuture)]() mutable {
                                       snow_shot::platform::applyApplicationQoSToCurrentThread();
                                       if (cancellation.valid())
                                           cancellation.get();
                                       snow_recording_render_task_destroy(retiring);
                                       return CleanupResult{};
                                   });
    }
    void requestCancellation() {
        if (!task || cancelFuture.valid())
            return;
        // Native cancellation synchronizes with publication. Its commit may be
        // waiting on filesystem work, so keep that wait away from the GUI and
        // preserve the borrowed task until this operation completes.
        cancelFuture = std::async(std::launch::async, [canceling = task] {
            snow_shot::platform::applyApplicationQoSToCurrentThread();
            static_cast<void>(snow_recording_render_task_cancel(canceling));
        });
    }
    void release(Outcome outcome, bool discard) {
        releasing = outcome;
        auto* retiring = source;
        source = nullptr;
        cleanupFuture = std::async(std::launch::async, [retiring, discard, outcome] {
            snow_shot::platform::applyApplicationQoSToCurrentThread();
            CleanupResult result;
            if (discard && snow_recording_source_discard(retiring) != SNOW_RECORDING_RESULT_OK) {
                result.error = nativeError();
                if (outcome == Outcome::Discarded) {
                    result.retainedSource = retiring;
                    return result;
                }
            }
            snow_recording_source_destroy(retiring);
            return result;
        });
        timer.start();
        notify();
    }
    void poll() {
        if (cleanupFuture.valid()) {
            if (cleanupFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
                return;
            const auto cleanup = cleanupFuture.get();
            if (releasing) {
                timer.stop();
                if (cleanup.retainedSource) {
                    source = cleanup.retainedSource;
                    releasing.reset();
                    retain(cleanup.error);
                    return;
                }
                if (!cleanup.error.isEmpty())
                    setError(cleanup.error);
                if (modal)
                    modal->close();
                if (owner.finished)
                    owner.finished(*releasing);
                return;
            }
            if (terminalSucceeded) {
                // Successful publication owns source cleanup in the backend.
                release(Outcome::Succeeded, false);
                return;
            }
            retain(lastError);
            return;
        }
        if (startFuture.valid()) {
            if (startFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
                return;
            auto result = startFuture.get();
            task = result.task;
            if (!task) {
                setError(result.error,
                         QT_TRANSLATE_NOOP("RecordingRenderDialog", "Unable to start rendering"));
                retain(lastError);
                return;
            }
            if (cancelRequested)
                requestCancellation();
        }
        if (!task)
            return;
        SnowRecordingRenderProgress snapshot{};
        snapshot.version = SNOW_RECORDING_RENDER_PROGRESS_VERSION;
        snapshot.struct_size = sizeof(snapshot);
        if (snow_recording_render_task_poll(task, &snapshot) != SNOW_RECORDING_RESULT_OK) {
            setError(nativeError(), QT_TRANSLATE_NOOP("RecordingRenderDialog",
                                                      "Unable to read rendering progress"));
            requestCancellation();
            disposeTask();
            notify();
            return;
        }
        const double next = std::isfinite(snapshot.percent)
                                ? std::clamp(static_cast<double>(snapshot.percent), 0.0, 100.0)
                                : percent;
        const double updated = std::max(percent, next);
        const bool progressChanged =
            updated != percent || stage != snapshot.stage || durationMs != snapshot.duration_ms;
        percent = updated;
        stage = snapshot.stage;
        durationMs = snapshot.duration_ms;
        if (snapshot.state == SNOW_RECORDING_RENDER_STATE_RUNNING) {
            if (progressChanged)
                notify();
            return;
        }
        terminalSucceeded = snapshot.state == SNOW_RECORDING_RENDER_STATE_SUCCEEDED;
        if (terminalSucceeded)
            percent = 100;
        else if (snapshot.state == SNOW_RECORDING_RENDER_STATE_FAILED)
            setError(nativeString([this](char* buffer, size_t capacity) {
                         return snow_recording_render_task_error(task, buffer, capacity);
                     }),
                     QT_TRANSLATE_NOOP("RecordingRenderDialog", "Rendering failed"));
        disposeTask();
        notify();
    }
    RecordingRenderJob& owner;
    SnowRecordingSource* source = nullptr;
    SnowRecordingRenderTask* task = nullptr;
    const bool visible;
    QPointer<QScreen> screen;
    const QRect anchorGeometry;
    QTimer timer;
    std::future<StartResult> startFuture;
    std::future<CleanupResult> cleanupFuture;
    std::future<void> cancelFuture;
    std::optional<Outcome> releasing;
    bool retained = false;
    bool terminalSucceeded = false;
    bool cancelRequested = false;
    double percent = 0;
    uint32_t stage = SNOW_RECORDING_RENDER_STAGE_PREPARE;
    uint64_t durationMs = 0;
    QString path;
    QString lastError;
    const char* errorTranslation = nullptr;
    QPointer<adqt::widgets::AdModal> modal;
    QLabel* status = nullptr;
    QLabel* details = nullptr;
    adqt::widgets::AdProgress* progress = nullptr;
    adqt::widgets::AdButton* cancelButton = nullptr;
    adqt::widgets::AdButton* retryButton = nullptr;
    adqt::widgets::AdButton* keepButton = nullptr;
    adqt::widgets::AdButton* discardButton = nullptr;
};

RecordingRenderJob::RecordingRenderJob(SnowRecordingSource* source, bool showDialog,
                                       QScreen* screen, QObject* parent,
                                       const QRect& anchorGeometry)
    : QObject(parent),
      m_impl(std::make_unique<Impl>(*this, source, showDialog, screen, anchorGeometry)) {}
RecordingRenderJob::~RecordingRenderJob() = default;
void RecordingRenderJob::start() {
    m_impl->start();
}
bool RecordingRenderJob::cancel() {
    auto& s = *m_impl;
    if (s.retained || s.releasing || s.cancelRequested || s.cleanupFuture.valid())
        return false;
    s.cancelRequested = true;
    s.requestCancellation();
    s.notify();
    return true;
}
bool RecordingRenderJob::retry() {
    if (!m_impl->retained || m_impl->releasing || !m_impl->source || m_impl->cleanupFuture.valid())
        return false;
    m_impl->terminalSucceeded = false;
    m_impl->start();
    return true;
}
bool RecordingRenderJob::release(bool discard) {
    if (!m_impl->retained || m_impl->releasing || !m_impl->source || m_impl->cleanupFuture.valid())
        return false;
    m_impl->release(discard ? Outcome::Discarded : Outcome::Kept, discard);
    return true;
}
QJsonObject RecordingRenderJob::state() const {
    const auto& s = *m_impl;
    const QStringList stages{QStringLiteral("preparing"), QStringLiteral("rendering"),
                             QStringLiteral("finalizing")};
    return {{QStringLiteral("render_phase"), s.retained ? QStringLiteral("retained")
                                             : s.cancelRequested
                                                 ? QStringLiteral("cancelling")
                                                 : stages.value(static_cast<int>(s.stage))},
            {QStringLiteral("render_progress"), s.percent},
            {QStringLiteral("source_retained"), s.retained},
            {QStringLiteral("source_path"), s.path},
            {QStringLiteral("render_duration_ms"), static_cast<qint64>(s.durationMs)}};
}
QString RecordingRenderJob::sourcePath() const {
    return m_impl->path;
}
QString RecordingRenderJob::error() const {
    return m_impl->lastError;
}
bool RecordingRenderJob::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        if (m_impl->errorTranslation) {
            m_impl->lastError = renderText(m_impl->errorTranslation);
            m_impl->notify();
        } else {
            m_impl->refresh();
        }
        if (m_impl->modal && m_impl->modal->isOpen())
            m_impl->modal->open();
    }
    return QObject::eventFilter(watched, event);
}
