#include "recordingtrimsession.h"
#include "recordingrenderdialog.h"
#include "recordingtrimtoolbar.h"
#include "snow_shot/presentation/screenrecordingareawindow.h"
#include "snow_shot/presentation/screenrecordingtoolbarwindow.h"
#include "snow_shot/platform/applicationqos.h"
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMimeData>
#include <QSaveFile>
#include <QUrl>
#include <QUuid>
#include <climits>
#include <thread>

namespace {
QString trimText(const char* text) {
    return QCoreApplication::translate("RecordingTrimSession", text);
}
QString nativeError() {
    return QString::fromUtf8(snow_recording_last_error_message());
}
template <class Read> QString nativeText(Read read) {
    const size_t size = read(nullptr, 0);
    if (size == 0 || size > 1024 * 1024)
        return {};
    QByteArray data(static_cast<qsizetype>(size), '\0');
    read(data.data(), size);
    return QString::fromUtf8(data.constData());
}
void copyRecording(const QString& path) {
    auto* data = new QMimeData;
    data->setUrls({QUrl::fromLocalFile(path)});
    QApplication::clipboard()->setMimeData(data);
}
QString nextOutput(const QString& original) {
    const QFileInfo file(original);
    const QString stem = file.completeBaseName() + QStringLiteral("-trim-") +
                         QUuid::createUuid().toString(QUuid::Id128).left(8);
    return file.dir().filePath(stem + QStringLiteral(".") + file.suffix());
}
} // namespace

RecordingTrimSession::RecordingTrimSession(ScreenRecordingAreaWindow* area,
                                           ScreenRecordingToolbarWindow* toolbar, QObject* parent)
    : QObject(parent), m_area(area), m_toolbar(toolbar) {
    m_panel = new RecordingTrimToolbar;
    m_toolbar->palette()->setRecordingTrimPanel(m_panel, true);
    m_panel->commitControlScale(adqt::widgets::controlScaleContextFor(m_toolbar->palette()));
    m_area->setTrimming(true);
    m_panel->replayRequested = [this] { seek(static_cast<int>(m_first), true); };
    m_panel->seekRequested = [this](int frame) { seek(frame, false); };
    m_panel->rangeChanged = [this](int first, int end, int preview) {
        m_first = static_cast<quint64>(first);
        m_end = static_cast<quint64>(end);
        seek(preview, false);
    };
    m_timer.setInterval(16);
    connect(&m_timer, &QTimer::timeout, this, [this] { poll(); });
}

RecordingTrimSession::~RecordingTrimSession() {
    if (m_dialog)
        delete m_dialog;
    // A controller can be destroyed during acquisition or an export. Transfer the
    // complete dependency chain before releasing any native resource.
    auto clip = m_clip;
    auto task = m_export;
    std::thread([open = std::move(m_openFuture), publish = std::move(m_publishFuture),
                 cancellation = std::move(m_cancelFuture), clip, task]() mutable {
        snow_shot::platform::applyApplicationQoSToCurrentThread();
        if (open.valid())
            clip = open.get().clip;
        if (publish.valid())
            static_cast<void>(publish.get());
        if (cancellation.valid())
            cancellation.get();
        if (task)
            snow_recording_clip_export_destroy(task);
        if (clip)
            snow_recording_clip_destroy(clip);
    }).detach();
}

void RecordingTrimSession::open(const QString& sourcePath, const QString& outputPath,
                                const SnowRecordingClipOptions& options, bool deferred,
                                bool saveWhenReady) {
    m_sourcePath = sourcePath;
    m_outputPath = outputPath;
    m_saveWhenReady = saveWhenReady;
    m_deferred = deferred;
    m_openFuture = std::async(std::launch::async, [sourcePath, options, deferred] {
        snow_shot::platform::applyApplicationQoSToCurrentThread();
        OpenResult result;
        result.clip = snow_recording_clip_open(sourcePath.toUtf8().constData(),
                                               deferred ? nullptr : &options);
        if (!result.clip)
            result.error = nativeError();
        return result;
    });
    m_timer.start();
}

bool RecordingTrimSession::ready() const {
    return m_clip && !m_busy;
}
QString RecordingTrimSession::phase() const {
    switch (m_phase) {
    case Phase::Preparing:
        return QStringLiteral("preparing");
    case Phase::Trimming:
        return QStringLiteral("trimming");
    case Phase::Exporting:
        return QStringLiteral("exporting");
    }
    Q_UNREACHABLE();
}
void RecordingTrimSession::setBusy(bool busy) {
    m_busy = busy;
    if (m_toolbar && m_panel)
        m_toolbar->palette()->setRecordingTrimPanel(m_panel, busy);
}
void RecordingTrimSession::seek(int frame, bool play) {
    if (!m_clip || m_busy || m_detached)
        return;
    m_timer.setInterval(16);
    m_revision = snow_recording_clip_seek(m_clip, static_cast<quint64>(frame), m_end, play ? 1 : 0);
}

void RecordingTrimSession::poll() {
    if (m_publication)
        m_percent = m_publication->percent.load();
    if (m_openFuture.valid() &&
        m_openFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        const auto result = m_openFuture.get();
        m_clip = result.clip;
        if (m_detached) {
            releaseMedia();
            return;
        }
        if (!m_clip || !snow_recording_clip_info(m_clip, &m_info) || m_info.frame_count > INT_MAX) {
            m_timer.stop();
            if (reportError)
                reportError(trimText(QT_TRANSLATE_NOOP("RecordingTrimSession",
                                                       "Unable to preview recording: %1"))
                                .arg(result.error));
            return;
        }
        m_end = m_info.frame_count;
        m_panel->setTimeline(static_cast<int>(m_end), m_info.duration_us, [this](int frame) {
            return snow_recording_clip_boundary(m_clip, static_cast<quint64>(frame));
        });
        m_phase = Phase::Trimming;
        setBusy(false);
        if (m_saveWhenReady)
            exportClip(true);
        else
            seek(0, true);
    }
    if (m_export) {
        float percent = 0;
        const auto state = snow_recording_clip_export_poll(m_export, &percent);
        m_percent = static_cast<double>(percent);
        if (state != SNOW_RECORDING_RENDER_STATE_RUNNING &&
            (!m_cancelFuture.valid() ||
             m_cancelFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)) {
            if (m_cancelFuture.valid())
                m_cancelFuture.get();
            const QString error = nativeText([this](char* data, size_t capacity) {
                return snow_recording_clip_export_error(m_export, data, capacity);
            });
            auto* completed = m_export;
            m_export = nullptr;
            std::thread([completed] { snow_recording_clip_export_destroy(completed); }).detach();
            if (state == SNOW_RECORDING_RENDER_STATE_SUCCEEDED) {
                m_cachePath = m_pendingPath;
                m_cachedFirst = m_first;
                m_cachedEnd = m_end;
                publish();
            } else
                finishExport(false, error);
        }
    }
    if (m_publishFuture.valid() &&
        m_publishFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        const auto result = m_publishFuture.get();
        finishExport(result.ok, result.error);
    }
    if (m_dialog) {
        m_dialog->percent = m_percent;
        m_dialog->stage = m_publication ? SNOW_RECORDING_RENDER_STAGE_FINALIZE
                                        : SNOW_RECORDING_RENDER_STAGE_RENDER;
        m_dialog->refresh();
    }
    if (!m_clip || m_detached || m_previewFailed)
        return;
    SnowRecordingClipPreview preview{};
    auto* lease = snow_recording_clip_acquire(m_clip, &preview);
    if (lease && preview.revision >= m_revision &&
        preview.byte_count == static_cast<size_t>(m_info.width) * m_info.height * 4) {
        const QImage frame(
            preview.rgba, static_cast<int>(m_info.width), static_cast<int>(m_info.height),
            static_cast<qsizetype>(m_info.width) * 4, QImage::Format_RGBA8888,
            [](void* value) {
                snow_recording_clip_frame_destroy(static_cast<SnowRecordingClipFrame*>(value));
            },
            lease);
        m_area->setPreviewFrame(frame);
        m_panel->setPosition(preview.position_us);
        m_timer.setInterval(preview.playing ? 16 : 100);
    } else if (lease)
        snow_recording_clip_frame_destroy(lease);
    const QString error = nativeText([this](char* data, size_t capacity) {
        return snow_recording_clip_error(m_clip, data, capacity);
    });
    if (!error.isEmpty()) {
        m_previewFailed = true;
        if (reportError)
            reportError(trimText(QT_TRANSLATE_NOOP("RecordingTrimSession",
                                                   "Unable to preview recording: %1"))
                            .arg(error));
    }
}

void RecordingTrimSession::exportClip(bool save) {
    if (!ready() || m_detached)
        return;
    seek(static_cast<int>(m_first), false);
    m_destination.clear();
    m_copy = !save;
    if (save) {
        const QString suffix = QFileInfo(m_outputPath).suffix();
        const QString filter = QStringLiteral("%1 (*.%2)").arg(suffix.toUpper(), suffix);
        QFileDialog dialog(m_area,
                           trimText(QT_TRANSLATE_NOOP("RecordingTrimSession", "Save to File")),
                           nextOutput(m_outputPath), filter);
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setDefaultSuffix(suffix);
        // The dialog resolves the default suffix before confirming replacement.
        if (dialog.exec() != QDialog::Accepted || m_detached)
            return;
        m_destination = dialog.selectedFiles().value(0);
        if (m_destination.isEmpty())
            return;
        if (QFileInfo(m_destination).canonicalFilePath() ==
            QFileInfo(m_sourcePath).canonicalFilePath()) {
            if (reportError)
                reportError(trimText(QT_TRANSLATE_NOOP(
                    "RecordingTrimSession",
                    "Choose a different file to preserve the original recording.")));
            return;
        }
    }
    m_percent = 0;
    m_phase = Phase::Exporting;
    setBusy(true);
    if (!m_deferred && m_first == 0 && m_end == m_info.frame_count) {
        m_cachePath = m_sourcePath;
        m_cachedFirst = m_first;
        m_cachedEnd = m_end;
    }
    if (m_cachedFirst == m_first && m_cachedEnd == m_end && QFileInfo::exists(m_cachePath)) {
        publish();
        return;
    }
    m_pendingPath = nextOutput(m_outputPath);
    m_export = snow_recording_clip_export_start(m_clip, m_first, m_end,
                                                m_pendingPath.toUtf8().constData());
    if (!m_export) {
        finishExport(false, nativeError());
        return;
    }
    QTimer::singleShot(300, this, [this] {
        if (m_export)
            showProgress();
    });
}

void RecordingTrimSession::publish() {
    if (m_copy) {
        copyRecording(m_cachePath);
        finishExport(true, {});
        return;
    }
    const QString from = m_cachePath, to = m_destination;
    m_publication = std::make_shared<Publication>();
    m_publishFuture = std::async(std::launch::async, [from, to, state = m_publication] {
        snow_shot::platform::applyApplicationQoSToCurrentThread();
        PublishResult result;
        QFile source(from);
        QSaveFile destination(to);
        if (!source.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly)) {
            result.error = source.isOpen() ? destination.errorString() : source.errorString();
            return result;
        }
        QByteArray buffer(256 * 1024, Qt::Uninitialized);
        qint64 copied = 0;
        while (true) {
            if (state->state.load() == Publication::Canceled)
                return result;
            const qint64 size = source.read(buffer.data(), buffer.size());
            if (size == 0)
                break;
            if (size < 0 || destination.write(buffer.constData(), size) != size) {
                result.error = size < 0 ? source.errorString() : destination.errorString();
                return result;
            }
            copied += size;
            state->percent.store(
                static_cast<int>(100.0 * static_cast<double>(copied) /
                                 static_cast<double>(qMax<qint64>(1, source.size()))));
        }
        int copying = Publication::Copying;
        if (!state->state.compare_exchange_strong(copying, Publication::Committing))
            return result;
        result.ok = destination.commit();
        if (!result.ok)
            result.error = destination.errorString();
        return result;
    });
    QTimer::singleShot(300, this, [this] {
        if (m_publishFuture.valid())
            showProgress();
    });
}
void RecordingTrimSession::finishExport(bool ok, const QString& error) {
    m_publication.reset();
    m_phase = Phase::Trimming;
    if (m_dialog) {
        delete m_dialog;
        m_dialog = nullptr;
    }
    setBusy(false);
    if (!ok && !error.isEmpty() && reportError && !m_detached)
        reportError(
            trimText(QT_TRANSLATE_NOOP("RecordingTrimSession", "Unable to export recording: %1"))
                .arg(error));
    if (m_detached)
        releaseMedia();
    if (ok && exported) {
        const QString completedPath = m_copy ? m_cachePath : m_destination;
        const auto onExported = exported;
        onExported(completedPath);
    }
}
void RecordingTrimSession::showProgress() {
    if (m_dialog || m_detached)
        return;
    m_dialog = new RecordingRenderDialog(this, m_area ? m_area->screen() : nullptr,
                                         m_area ? m_area->geometry() : QRect(), m_area);
    m_dialog->percent = m_percent;
    m_dialog->stage =
        m_publication ? SNOW_RECORDING_RENDER_STAGE_FINALIZE : SNOW_RECORDING_RENDER_STAGE_RENDER;
    m_dialog->cancel = [this] {
        if (m_dialog->cancelRequested)
            return;
        m_dialog->cancelRequested = true;
        m_dialog->refresh();
        if (m_publication) {
            int copying = Publication::Copying;
            m_publication->state.compare_exchange_strong(copying, Publication::Canceled);
        }
        if (m_export && !m_cancelFuture.valid())
            m_cancelFuture = std::async(
                std::launch::async, [task = m_export] { snow_recording_clip_export_cancel(task); });
    };
    m_dialog->refresh();
    m_dialog->modal->open();
}
void RecordingTrimSession::detach() {
    m_detached = true;
    if (m_toolbar)
        m_toolbar->palette()->setRecordingTrimPanel(nullptr, false);
    if (m_panel)
        m_panel->deleteLater();
    if (m_dialog) {
        delete m_dialog;
        m_dialog = nullptr;
    }
    m_area = nullptr;
    m_toolbar = nullptr;
    m_panel = nullptr;
    reportError = {};
    // Export completion still reports its durable file after the windows close.
    if (!m_export && !m_publishFuture.valid() && !m_openFuture.valid())
        releaseMedia();
}
void RecordingTrimSession::releaseMedia() {
    m_timer.stop();
    deleteLater();
}
