#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/screenshotencodingsettings.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/storage/exportfilename.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "widgets/message.h"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>

namespace {
QPointer<QObject> activeBatchSave;

// QFile::copy refuses existing destinations, including files created after our collision check.
// Stage first so encoders never open a user-owned destination with overwrite semantics.
ScreenshotExportTaskResult publishBatchFiles(const QStringList& sources, const QString& directory,
                                             const QString& baseName, int& suffix,
                                             const ScreenshotExportCancellation& cancellation) {
    const auto failure = [](const QString& error) {
        return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::File, error);
    };
    for (; suffix < 1000000; ++suffix) {
        if (cancellation.isCancellationRequested())
            return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::Cancelled, {});
        QStringList endings;
        QString longest;
        for (const auto& source : sources) {
            const QString ending =
                QStringLiteral("_%1.%2").arg(suffix).arg(QFileInfo(source).suffix());
            endings.append(ending);
            if (ending.toUtf8().size() > longest.toUtf8().size())
                longest = ending;
        }
        QString stem = snow_shot::storage::composeExportFileName(baseName, longest);
        stem.chop(longest.size());
        QStringList destinations;
        for (const auto& ending : endings)
            destinations.append(QDir(directory).filePath(stem + ending));
        if (std::any_of(destinations.cbegin(), destinations.cend(), [](const auto& path) {
                const QFileInfo info(path);
                return info.exists() || info.isSymLink();
            }))
            continue;
        QStringList created;
        bool collision = false;
        QString error;
        for (qsizetype i = 0; i < sources.size(); ++i) {
            QFile source(sources.at(i));
            if (cancellation.isCancellationRequested() || !source.copy(destinations.at(i))) {
                collision = QFileInfo::exists(destinations.at(i)) ||
                            QFileInfo(destinations.at(i)).isSymLink();
                error = source.errorString();
                break;
            }
            created.append(destinations.at(i));
        }
        if (created.size() == destinations.size()) {
            ++suffix;
            ScreenshotExportTaskResult result;
            result.savedPath = destinations.first();
            return result;
        }
        for (const auto& path : created)
            QFile::remove(path);
        if (cancellation.isCancellationRequested())
            return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::Cancelled, {});
        if (!collision)
            return failure(error);
    }
    return failure(ScreenshotPinnedWindow::tr("No available output filename was found"));
}
} // namespace

// One session owns the folder dialog, export snapshots, staging files and sequential queue.
// The owner and every target are identity-checked because live pin objects can be pooled.
class ScreenshotPinnedBatchSaveSession final : public QObject {
  public:
    explicit ScreenshotPinnedBatchSaveSession(ScreenshotPinnedWindow* owner)
        : QObject(owner), m_owner(owner), m_ownerId(owner->persistenceId()),
          m_ownerGeneration(owner->m_presentationGeneration) {
        activeBatchSave = this;
        for (const auto& window : owner->displayedPinnedWindows())
            m_targets.append({window, window->persistenceId(), window->m_presentationGeneration});
        connect(owner, &ScreenshotPinnedWindow::closingForPersistence, this, [this] { cancel(); });
        const snow_shot::storage::ScreenshotSettings settings;
        m_baseName = ScreenshotImageFileService::suggestedBaseName(
            settings.manualSaveFilenameFormat(), QDateTime::currentDateTime());
        m_format = ScreenshotImageFileService::formatForKey(settings.lastManualSaveFormat());
        m_encoding = snow_shot::presentation::screenshotEncodingOptions(settings);
        m_pdf = {screenshot_pdf::pageSizeForKey(settings.pdfPageSize())};
        setObjectName(QStringLiteral("screenshotPinnedBatchSaveSession"));
    }

    ~ScreenshotPinnedBatchSaveSession() override {
        releaseDialog();
        m_job.cancel();
        if (m_artifact)
            m_artifact->cancel();
    }

    void chooseDirectory() {
        const snow_shot::storage::ScreenshotSettings settings;
        const QString initial = ScreenshotImageFileService::saveDialogDirectory(
            settings.lastManualSaveDirectory(), settings.imageSaveDirectory());
        const QPointer<ScreenshotPinnedBatchSaveSession> lifetime(this);
        m_dialogOpen = true;
        m_owner->setProperty("saveDialogOpen", true);
        const QString directory = QFileDialog::getExistingDirectory(
            m_owner, ScreenshotPinnedWindow::tr("Save displayed windows"), initial);
        if (!lifetime)
            return;
        releaseDialog();
        start(directory);
    }

    void start(const QString& directory) {
        if (directory.isEmpty() || !ownerValid() || m_targets.isEmpty()) {
            cancel();
            return;
        }
        m_directory = directory;
        QTimer::singleShot(0, this, [this] { next(); });
    }

  private:
    struct Target {
        QPointer<ScreenshotPinnedWindow> window;
        QString id;
        quint64 generation;
        [[nodiscard]] bool valid() const {
            return window && !window->m_destroying && !window->m_closing &&
                   window->persistenceId() == id && window->m_presentationGeneration == generation;
        }
    };

    [[nodiscard]] bool ownerValid() const {
        return !m_cancelled && m_owner && !m_owner->m_destroying && !m_owner->m_closing &&
               m_owner->persistenceId() == m_ownerId &&
               m_owner->m_presentationGeneration == m_ownerGeneration;
    }

    void releaseDialog() {
        if (m_dialogOpen && m_owner && m_owner->persistenceId() == m_ownerId &&
            m_owner->m_presentationGeneration == m_ownerGeneration)
            m_owner->setProperty("saveDialogOpen", false);
        m_dialogOpen = false;
    }

    void cancel() {
        m_cancelled = true;
        m_job.cancel();
        if (m_artifact)
            m_artifact->cancel();
        deleteLater();
    }

    void next() {
        if (!ownerValid()) {
            cancel();
            return;
        }
        m_artifact.reset();
        m_staging.reset();
        if (m_index == m_targets.size()) {
            const QPointer<ScreenshotPinnedBatchSaveSession> lifetime(this);
            adqt::widgets::AdMessage::Request message;
            message.key = QStringLiteral("pinned-batch-save");
            message.content =
                m_failed == 0 ? ScreenshotPinnedWindow::tr("Saved %n window(s).", nullptr, m_saved)
                              : ScreenshotPinnedWindow::tr("Saved: %1; failed: %2.")
                                    .arg(m_saved)
                                    .arg(m_failed);
            if (!m_lastError.isEmpty())
                message.content += QLatin1Char('\n') + m_lastError;
            if (m_failed)
                adqt::widgets::AdMessageService::error(std::move(message), m_owner);
            else
                adqt::widgets::AdMessageService::success(std::move(message), m_owner);
            if (lifetime)
                deleteLater();
            return;
        }
        prepare(m_targets.at(m_index));
    }

    void prepare(const Target& target) {
        if (!ownerValid()) {
            cancel();
            return;
        }
        if (!target.valid() || target.window->m_printPending ||
            target.window->property("saveDialogOpen").toBool()) {
            complete(ScreenshotExportTaskResult::failure(
                ScreenshotExportFailureStage::Source,
                ScreenshotPinnedWindow::tr("A pinned window is no longer available to save.")));
            return;
        }
        const QPointer<ScreenshotPinnedBatchSaveSession> lifetime(this);
        auto* window = target.window.data();
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION ||                     \
    SNOW_SHOT_ENABLE_QR_RECOGNITION || SNOW_SHOT_ENABLE_TABLE_RECOGNITION
        const auto recognition = window->m_recognitionSession
                                     ? window->m_recognitionSession->fileExportSnapshot()
                                     : std::nullopt;
        if (!lifetime)
            return;
        if (!target.valid()) {
            complete(ScreenshotExportTaskResult::failure(
                ScreenshotExportFailureStage::Source,
                ScreenshotPinnedWindow::tr("A pinned window is no longer available to save.")));
            return;
        }
        if (recognition) {
            m_staging = std::make_shared<QTemporaryDir>();
            const QString path =
                m_staging->filePath(QStringLiteral("result.") +
                                    ScreenshotRecognitionFileExport::extension(recognition->kind));
            submitPublication(ScreenshotRecognitionFileExport::outputPaths(path, recognition->kind),
                              *recognition);
            return;
        }
#endif
        if (window->m_originalImage.isNull()) {
            window->requestMaterializedImage([lifetime, target](bool success) {
                if (!lifetime)
                    return;
                if (success && target.valid())
                    lifetime->prepare(target);
                else
                    lifetime->complete(ScreenshotExportTaskResult::failure(
                        ScreenshotExportFailureStage::Source,
                        ScreenshotPinnedWindow::tr("The pinned image could not be prepared")));
            });
            return;
        }
        const bool editingReady = window->animatedImage() || !window->m_canvas ||
                                  window->m_canvas->resetEditingStatePreservingTool();
        if (!lifetime)
            return;
        if (!editingReady || !target.valid()) {
            complete(ScreenshotExportTaskResult::failure(
                ScreenshotExportFailureStage::Source,
                ScreenshotPinnedWindow::tr("The pinned image could not be prepared")));
            return;
        }
        m_artifact = window->fileSaveArtifact();
        m_staging = std::make_shared<QTemporaryDir>();
        const QString path = m_staging->filePath(QStringLiteral("image.") +
                                                 ScreenshotImageFileService::extension(m_format));
        if (!m_staging->isValid() || !m_artifact ||
            !m_artifact->requestSaveToPath(
                this, path, m_format, m_encoding,
                [this, path, target](ScreenshotExportTaskResult result) {
                    if (!ownerValid()) {
                        cancel();
                        return;
                    }
                    if (!target.valid())
                        complete(ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::Source,
                            ScreenshotPinnedWindow::tr(
                                "A pinned window is no longer available to save.")));
                    else if (result.succeeded())
                        submitPublication({path});
                    else
                        complete(std::move(result));
                },
                m_pdf, m_staging)) {
            complete(ScreenshotExportTaskResult::failure(
                ScreenshotExportFailureStage::Source,
                ScreenshotPinnedWindow::tr("The pinned image save could not be started")));
        }
    }

    void submitPublication(const QStringList& paths,
                           std::optional<ScreenshotRecognitionFileSnapshot> recognition = {}) {
        auto suffix = std::make_shared<int>(m_suffix);
        m_job = ScreenshotExportCoordinator::shared().submit(
            this, ScreenshotExportCoordinator::Priority::Foreground,
            [paths, recognition = std::move(recognition), staging = m_staging,
             directory = m_directory, baseName = m_baseName, suffix](const auto& cancellation) {
                if (!staging || !staging->isValid())
                    return ScreenshotExportTaskResult::failure(
                        ScreenshotExportFailureStage::File,
                        ScreenshotPinnedWindow::tr("The output directory could not be created"));
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION ||                     \
    SNOW_SHOT_ENABLE_QR_RECOGNITION || SNOW_SHOT_ENABLE_TABLE_RECOGNITION
                if (recognition) {
                    const auto result =
                        ScreenshotRecognitionFileExport::saveToPath(*recognition, paths.first());
                    if (!result.succeeded())
                        return ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::File, result.error);
                }
#else
                Q_UNUSED(recognition);
#endif
                return publishBatchFiles(paths, directory, baseName, *suffix, cancellation);
            },
            [this, suffix](ScreenshotExportTaskResult result) {
                m_suffix = *suffix;
                complete(std::move(result));
            });
        if (!m_job.isValid())
            complete(ScreenshotExportTaskResult::failure(
                ScreenshotExportFailureStage::Queue,
                ScreenshotPinnedWindow::tr("The pinned image save could not be started")));
    }

    void complete(ScreenshotExportTaskResult result) {
        if (!ownerValid()) {
            cancel();
            return;
        }
        if (result.succeeded()) {
            ++m_saved;
            const QPointer<ScreenshotPinnedBatchSaveSession> lifetime(this);
            static_cast<void>(
                snow_shot::storage::ScreenshotSettings().setLastManualSaveDirectory(m_directory));
            if (!lifetime)
                return;
        } else {
            ++m_failed;
            m_lastError = result.error;
        }
        ++m_index;
        QTimer::singleShot(0, this, [this] { next(); });
    }

    QPointer<ScreenshotPinnedWindow> m_owner;
    QString m_ownerId;
    quint64 m_ownerGeneration;
    QList<Target> m_targets;
    qsizetype m_index = 0;
    int m_suffix = 1;
    int m_saved = 0;
    int m_failed = 0;
    bool m_cancelled = false;
    bool m_dialogOpen = false;
    QString m_directory;
    QString m_baseName;
    QString m_lastError;
    ScreenshotImageFileFormat m_format;
    ScreenshotImageEncodingOptions m_encoding;
    ScreenshotPdfOptions m_pdf;
    std::shared_ptr<QTemporaryDir> m_staging;
    std::shared_ptr<ScreenshotExportArtifact> m_artifact;
    ScreenshotExportJobHandle m_job;
};

bool ScreenshotPinnedWindow::batchSavePending() {
    return !activeBatchSave.isNull();
}

void ScreenshotPinnedWindow::saveDisplayedPinnedWindows() {
    if (!m_closing && !m_printPending && !property("saveDialogOpen").toBool() &&
        !batchSavePending())
        (new ScreenshotPinnedBatchSaveSession(this))->chooseDirectory();
}

void ScreenshotPinnedWindow::saveDisplayedPinnedWindowsToDirectory(const QString& directory) {
    if (!m_closing && !batchSavePending())
        (new ScreenshotPinnedBatchSaveSession(this))->start(directory);
}
