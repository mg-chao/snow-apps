// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/imagesplitwindow.h"
#include "snow_shot/presentation/collagedocument.h"
#include "snow_shot/presentation/components/imagespliteditorwidget.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/windowcloseshortcut.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/capturehistoryrepository.h"
#include "imagesplitimagepreparation_p.h"
#include "../services/imageimportsourcereader_p.h"
#include "icon_renderer.h"
#include "widgets/button.h"
#include "widgets/message.h"
#include "widgets/modal.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <utility>

namespace snow_shot::presentation {
namespace {
using adqt::widgets::AdButton;
using adqt::widgets::AdMessage;
using adqt::widgets::AdMessageHandle;
using adqt::widgets::AdMessageService;
using adqt::widgets::AdModal;

QString splitText(const char* source) {
    return QCoreApplication::translate("ImageSplitWindow", source);
}
[[maybe_unused]] constexpr const char* labels[] = {
    QT_TRANSLATE_NOOP("ImageSplitWindow", "Image Splitting"),
    QT_TRANSLATE_NOOP("ImageSplitWindow", "Save All"),
    QT_TRANSLATE_NOOP("ImageSplitWindow", "Stitch to PDF"),
    QT_TRANSLATE_NOOP("ImageSplitWindow", "Image Stitching is busy. Try again when it finishes."),
    QT_TRANSLATE_NOOP("ImageSplitWindow", "Cancel"),
    QT_TRANSLATE_NOOP("ImageSplitWindow", "Images"),
    QT_TRANSLATE_NOOP("ImageSplitWindow", "Select Image"),
    QT_TRANSLATE_NOOP("ImageSplitWindow", "Select Destination Folder"),
    QT_TRANSLATE_NOOP("ImageSplitWindow", "Replace Image"),
    QT_TRANSLATE_NOOP("ImageSplitWindow", "Replacing the image will discard the current edits"),
    QT_TRANSLATE_NOOP("ImageSplitWindow", "Discard these image splits?"),
    QT_TRANSLATE_NOOP("ImageSplitWindow", "The image and current split edits will be discarded."),
    QT_TRANSLATE_NOOP("ImageSplitWindow", "Discard"),
    QT_TRANSLATE_NOOP("ImageSplitWindow", "Loading image…"),
    QT_TRANSLATE_NOOP("ImageSplitWindow", "Saving images: %1 of %2"),
    QT_TRANSLATE_NOOP("ImageSplitWindow", "Saved %1 images to %2"),
    QT_TRANSLATE_NOOP("ImageSplitWindow", "Saved %1 of %2 images to %3. %4"),
    QT_TRANSLATE_NOOP("ImageSplitWindow", "Drop one image file at a time."),
    QT_TRANSLATE_NOOP("ImageSplitWindow", "The image could not be read or is no longer available."),
    QT_TRANSLATE_NOOP("ImageSplitWindow", "Image loading could not be started. Try again."),
    QT_TRANSLATE_NOOP("ImageSplitWindow", "Export could not be started. Try again."),
    QT_TRANSLATE_NOOP("ImageSplitWindow", "Image processing failed. Try again."),
    QT_TRANSLATE_NOOP("ImageSplitWindow", "The images could not be exported. Try again."),
    QT_TRANSLATE_NOOP("ImageSplitWindow",
                      "The image is too large or there is not enough memory to load it."),
};

QString imageFilter() {
    QStringList patterns;
    for (const auto& suffix : ScreenshotClipboardContentReader::supportedImageFileExtensions())
        patterns.append(QStringLiteral("*.") + suffix);
    return QStringLiteral("%1 (%2)").arg(splitText("Images"), patterns.join(u' '));
}

const char* selectedFileErrorSource(platform::SelectedFileError error) {
    switch (error) {
    case platform::SelectedFileError::PermissionDenied:
        return QT_TRANSLATE_NOOP("ImageSplitWindow",
                                 "Allow Snow Shot to access Finder in System Settings > Privacy & "
                                 "Security > Automation, then try again.");
    case platform::SelectedFileError::Timeout:
        return QT_TRANSLATE_NOOP("ImageSplitWindow",
                                 "The file manager took too long to return the selected files. "
                                 "Please try again.");
    case platform::SelectedFileError::Unavailable:
        return QT_TRANSLATE_NOOP("ImageSplitWindow",
                                 "The file manager is unavailable. Open it and try again.");
    case platform::SelectedFileError::QueryFailed:
    case platform::SelectedFileError::None:
        return QT_TRANSLATE_NOOP("ImageSplitWindow",
                                 "Could not read the selected image files. Please try again.");
    }
    return nullptr;
}

struct LoadedImage final {
    QImage image;
    QImage preview;
    std::shared_ptr<CollageImportReservation> reservation;
    const char* errorSource = nullptr;
    QString originalFileName;
    QDateTime createdUtc;
    std::optional<ImageSplitDirection> initialDirection;
};

struct ExportProgress final {
    std::atomic<qint64> completed = 0;
    std::atomic<qint64> total = 0;
};

QString imageMemoryError() {
    return splitText("The image is too large or there is not enough memory to load it.");
}
bool reserveImage(QSize size, const std::shared_ptr<LoadedImage>& loaded, qint64 nativeBytes = 0) {
    auto bytes = collageImageImportBytes(size);
    if (bytes == 0)
        return false;
    if (nativeBytes > 0) {
        const bool needsPreview = size.width() > 2048 || size.height() > 2048;
        const QSize preview = size.scaled(2048, 2048, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
        const qint64 previewBytes = needsPreview ? qint64(preview.width()) * preview.height() * 4 +
                                                       imageSplitPreviewMaximumWorkingBytes
                                                 : 0;
        if (nativeBytes > std::numeric_limits<qint64>::max() - previewBytes)
            return false;
        bytes = std::max(bytes, nativeBytes + previewBytes);
    }
    if (!loaded->reservation)
        loaded->reservation = reserveCollageImport(bytes);
    else if (!loaded->reservation->resize(bytes))
        loaded->reservation.reset();
    return loaded->reservation != nullptr;
}

ScreenshotExportTaskResult loadingFailure(const std::shared_ptr<LoadedImage>& loaded,
                                          ScreenshotExportFailureStage stage, const char* source) {
    loaded->errorSource = source;
    return ScreenshotExportTaskResult::failure(stage, splitText(source));
}

ScreenshotExportTaskResult prepareImage(QImage image, const std::shared_ptr<LoadedImage>& loaded,
                                        const ScreenshotExportCancellation& cancellation) {
    if (cancellation.isCancellationRequested())
        return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::Cancelled, {});
    if (image.isNull())
        return loadingFailure(loaded, ScreenshotExportFailureStage::Source,
                              "The image could not be read or is no longer available.");
    if (!reserveImage(image.size(), loaded, image.sizeInBytes()))
        return loadingFailure(loaded, ScreenshotExportFailureStage::Source,
                              "The image is too large or there is not enough memory to load it.");
    // The document and canvas use physical pixel coordinates. Retain native pixels and metadata;
    // format/color conversion belongs to the bounded preview and export readers.
    loaded->preview = imageSplitPreview(
        image, [&cancellation] { return cancellation.isCancellationRequested(); });
    if (cancellation.isCancellationRequested())
        return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::Cancelled, {});
    if (loaded->preview.isNull())
        return loadingFailure(loaded, ScreenshotExportFailureStage::Source,
                              "The image could not be read or is no longer available.");
    loaded->image = std::move(image);
    const qint64 retained =
        loaded->image.sizeInBytes() + (loaded->preview.constBits() == loaded->image.constBits()
                                           ? 0
                                           : loaded->preview.sizeInBytes());
    if (!loaded->reservation->resize(retained))
        return loadingFailure(loaded, ScreenshotExportFailureStage::Source,
                              "The image is too large or there is not enough memory to load it.");
    return {};
}
} // namespace

struct ImageSplitWindow::Impl final {
    enum class Status {
        None,
        Loading,
        Saving,
        Saved,
        LoadError,
        ExportError,
        DropError,
        StitchError
    };
    ImageSplitWindow& q;
    ImageSplitWindowServices services;
    ImageSplitDocument* document;
    components::ImageSplitEditorWidget* editor;
    AdModal* modal;
    AdButton* save;
    AdButton* stitch;
    AdButton* cancel;
    QPointer<AdModal> confirmation;
    QPointer<QFileDialog> fileDialog;
    QPointer<AdMessageHandle> message;
    ScreenshotExportJobHandle job;
    std::shared_ptr<ScreenshotExportArtifact> artifact;
    std::shared_ptr<CollageImportReservation> imageReservation;
    QString sourceName;
    QString error;
    const char* loadingErrorSource = nullptr;
    const char* windowExportErrorSource = nullptr;
    ImageSplitExportResult exportResult;
    qint64 completed = 0;
    qint64 total = 0;
    quint64 generation = 0;
    Status status = Status::None;
    bool importing = false;
    bool exporting = false;
    bool choosing = false;
    bool confirming = false;
    bool replacing = false;
    bool closing = false;
    QTimer* progressTimer;
    std::shared_ptr<ExportProgress> progress;
    std::function<void()> afterConfirmation;
    WindowShortcutManager* pasteShortcuts = nullptr;

    Impl(ImageSplitWindow& owner, ImageSplitWindowServices ports)
        : q(owner), services(std::move(ports)), document(new ImageSplitDocument(&q)),
          editor(new components::ImageSplitEditorWidget(document)), modal(new AdModal(&q)),
          save(new AdButton), stitch(new AdButton), cancel(new AdButton),
          progressTimer(new QTimer(&q)) {
        progressTimer->setInterval(100);
        QObject::connect(progressTimer, &QTimer::timeout, &q, [this] {
            if (!exporting || !progress)
                return;
            const auto current = progress->completed.load();
            if (current != completed) {
                completed = current;
                total = progress->total.load();
                showStatus();
            }
        });
        modal->setObjectName(QStringLiteral("imageSplitModal"));
        modal->setMode(AdModal::Mode::Window);
        modal->setCentered(true);
        modal->setWindowModeDetached(true);
        modal->setWindowModality(Qt::NonModal);
        modal->setWindowResizable(true);
        modal->setWindowTaskbarVisible(true);
        modal->setWindowMinimizeButtonVisible(true);
        modal->setWindowMaximizeButtonVisible(true);
        modal->setWindowAlwaysOnTopButtonVisible(true);
        modal->setWindowAlwaysOnTop(true);
        modal->setWindowIcon(adqt::icons::makeIcon(icons::custom::app::ApplicationIcon()));
        modal->setClosePolicy(AdModal::ClosePolicy::Manual);
        modal->setCloseOnMaskClick(false);
        modal->setContentWidget(editor);
        auto* footer = new QWidget;
        footer->setObjectName(QStringLiteral("imageSplitFooter"));
        auto* layout = new QHBoxLayout(footer);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(
            styles::ThemeManager::instance().themeColorScheme().metricAlias.marginXS);
        layout->addWidget(editor->outputDescriptionWidget(), 1);
        editor->outputDescriptionWidget()->show();
        cancel->setObjectName(QStringLiteral("imageSplitCancel"));
        save->setObjectName(QStringLiteral("imageSplitSaveAll"));
        stitch->setObjectName(QStringLiteral("imageSplitStitchToPdf"));
        save->setButtonStyle(AdButton::ButtonStyle::Solid);
        save->setAccentRole(AdButton::AccentRole::Primary);
        for (auto* button : {cancel, stitch, save}) {
            button->setFocusPolicy(Qt::StrongFocus);
            layout->addWidget(button);
        }
        modal->setFooterWidget(footer);
        QObject::connect(save, &AdButton::clicked, &q, &ImageSplitWindow::saveAll);
        QObject::connect(stitch, &AdButton::clicked, &q, &ImageSplitWindow::stitchToPdf);
        QObject::connect(cancel, &AdButton::clicked, &q, &ImageSplitWindow::requestClose);
        QObject::connect(modal, &AdModal::closeRequested, &q,
                         [this](AdModal::CloseReason) { q.requestClose(); });
        QObject::connect(modal, &AdModal::finished, &q, [this](AdModal::DialogCode) {
            closing = true;
            cancelWork();
            emit q.closed();
        });
        QObject::connect(editor, &components::ImageSplitEditorWidget::reloadRequested, &q,
                         &ImageSplitWindow::reload);
        QObject::connect(editor, &components::ImageSplitEditorWidget::imageFilesDropped, &q,
                         &ImageSplitWindow::dropFiles);
        QObject::connect(editor, &components::ImageSplitEditorWidget::validationChanged, &q,
                         [this] { updateActions(); });
        QObject::connect(document, &ImageSplitDocument::changed, &q, [this] { updateActions(); });
        QObject::connect(&LanguageManager::instance(), &LanguageManager::languageChanged, &q,
                         [this] { retranslate(); });
        pasteShortcuts = new WindowShortcutManager(&q);
        WindowShortcutManager::Binding binding;
        binding.id = QStringLiteral("imageSplitPaste");
        for (const auto& sequence : QKeySequence::keyBindings(QKeySequence::Paste)) {
            if (sequence.count() == 1)
                binding.keyCombinations.append(sequence[0]);
        }
        binding.priority = WindowShortcutManager::StandardPriority::WindowCommand;
        binding.canActivate = [this](const auto& context) {
            const auto* receiver = qobject_cast<QWidget*>(context.receiver);
            return !unavailable() && modal->isOpen() && receiver &&
                   receiver->window() == editor->window() &&
                   !WindowShortcutManager::focusAcceptsTextInput(context.focusWidget);
        };
        binding.activate = [this](const auto&) {
            q.paste();
            return true;
        };
        static_cast<void>(pasteShortcuts->addBinding(&q, std::move(binding)));
        retranslate();
        updateActions();
    }

    bool unavailable() const {
        return closing || importing || exporting || choosing || confirming;
    }
    void updateActions() {
        // Busy blocks activation while retaining the primary button's loading appearance.
        save->setEnabled(exporting || (!unavailable() && editor->configurationValid()));
        save->setBusy(exporting);
        stitch->setEnabled(!unavailable() && services.stitchToPdf && editor->configurationValid());
        editor->setLoading(importing || choosing || confirming);
        editor->setExportBusy(exporting);
    }
    void retranslate() {
        modal->setWindowTitle(splitText("Image Splitting"));
        save->setText(splitText("Save All"));
        stitch->setText(splitText("Stitch to PDF"));
        cancel->setText(splitText("Cancel"));
        save->setAccessibleName(save->text());
        stitch->setAccessibleName(stitch->text());
        cancel->setAccessibleName(cancel->text());
        editor->retranslateUi();
        if (confirmation)
            translateConfirmation();
        if (status == Status::LoadError && loadingErrorSource)
            error = splitText(loadingErrorSource);
        if (status == Status::StitchError)
            error = splitText("Image Stitching is busy. Try again when it finishes.");
        if (status == Status::ExportError) {
            exportResult.error = windowExportErrorSource ? splitText(windowExportErrorSource)
                                                         : exportResult.errorMessage();
            error = exportResult.error;
        }
        showStatus();
    }
    void showStatus() {
        QString content;
        switch (status) {
        case Status::Loading:
            content = splitText("Loading image…");
            break;
        case Status::Saving:
            content = splitText("Saving images: %1 of %2").arg(completed).arg(total);
            break;
        case Status::Saved:
            content = splitText("Saved %1 images to %2")
                          .arg(exportResult.savedCount)
                          .arg(exportResult.directory);
            break;
        case Status::LoadError:
        case Status::StitchError:
            content = error;
            break;
        case Status::ExportError:
            content = exportResult.directory.isEmpty()
                          ? exportResult.error
                          : splitText("Saved %1 of %2 images to %3. %4")
                                .arg(exportResult.savedCount)
                                .arg(exportResult.totalCount)
                                .arg(exportResult.directory, exportResult.error);
            break;
        case Status::DropError:
            content = splitText("Drop one image file at a time.");
            break;
        case Status::None:
            break;
        }
        if (content.isEmpty()) {
            if (message)
                message->close();
            message.clear();
            return;
        }
        AdMessage::Request request;
        request.key = QStringLiteral("imageSplitStatus");
        request.content = content;
        if (status == Status::Loading || status == Status::Saving) {
            request.durationMs = 0;
            message = AdMessageService::loading(std::move(request), editor->window());
        } else if (status == Status::Saved) {
            message = AdMessageService::success(std::move(request), editor->window());
        } else {
            message = AdMessageService::error(std::move(request), editor->window());
        }
    }
    void present() {
        if (!modal->isOpen()) {
            auto* screen = QGuiApplication::screenAt(QCursor::pos());
            if (!screen)
                screen = QGuiApplication::primaryScreen();
            if (screen) {
                modal->setWindowScreen(screen);
                const auto available = screen->availableGeometry().size() - QSize(40, 40);
                modal->setWindowPreferredSize(QSize(1200, 800).boundedTo(available));
                modal->setWindowMinimumSize(QSize(900, 600).boundedTo(available));
            }
        }
        modal->present();
        pasteShortcuts->addScopeWindow(editor->window());
        installWindowCloseShortcut(editor->window(), [this] { q.requestClose(); });
    }
    void translateConfirmation() {
        confirmation->setWindowTitle(replacing ? splitText("Replace Image")
                                               : splitText("Discard these image splits?"));
        confirmation->setText(
            replacing ? splitText("Replacing the image will discard the current edits")
                      : splitText("The image and current split edits will be discarded."));
        confirmation->setAcceptText(replacing ? splitText("Replace Image") : splitText("Discard"));
        confirmation->setRejectText(splitText("Cancel"));
    }
    void askConfirmation(bool replace, std::function<void(bool)> complete) {
        if (closing || confirming)
            return;
        confirming = true;
        replacing = replace;
        updateActions();
        const QPointer<ImageSplitWindow> guard(&q);
        auto done = [guard, complete = std::move(complete)](bool accepted) mutable {
            if (!guard || guard->m_impl->closing)
                return;
            auto& d = *guard->m_impl;
            d.confirming = false;
            complete(accepted);
            if (guard && !d.closing && d.afterConfirmation) {
                auto deferred = std::exchange(d.afterConfirmation, {});
                deferred();
            }
            if (guard)
                d.updateActions();
        };
        if (services.confirm) {
            services.confirm(editor->window(), replace, std::move(done));
            return;
        }
        auto* dialog = new AdModal(&q);
        confirmation = dialog;
        dialog->setObjectName(QStringLiteral("imageSplitConfirmation"));
        dialog->setMode(AdModal::Mode::Window);
        dialog->setOwnerWindow(editor->window());
        dialog->setWindowModality(Qt::WindowModal);
        dialog->setPreset(AdModal::Preset::Confirm);
        translateConfirmation();
        dialog->setAcceptAccentRole(replace ? AdButton::AccentRole::Primary
                                            : AdButton::AccentRole::Danger);
        QObject::connect(dialog, &AdModal::finished, &q,
                         [done = std::move(done)](AdModal::DialogCode code) mutable {
                             done(code == AdModal::DialogCode::Accepted);
                         });
        QObject::connect(dialog, &AdModal::finished, dialog, &QObject::deleteLater);
        dialog->present();
    }
    void cancelWork() {
        ++generation;
        job.cancel();
        job = {};
        progressTimer->stop();
        progress.reset();
        afterConfirmation = {};
        if (artifact)
            artifact->cancel();
        artifact.reset();
        importing = exporting = choosing = false;
        status = Status::None;
        showStatus();
        updateActions();
        if (fileDialog)
            fileDialog->reject();
        if (confirmation)
            confirmation->reject();
    }
    void beginLoading() {
        ++generation;
        importing = true;
        error.clear();
        loadingErrorSource = nullptr;
        status = Status::Loading;
        updateActions();
        showStatus();
    }
    void finishLoading(const std::shared_ptr<LoadedImage>& loaded, QString filename,
                       QDateTime createdUtc, bool replacement, quint64 token,
                       ScreenshotExportTaskResult result) {
        if (closing || token != generation)
            return;
        if (confirming && !replacing) {
            // A close confirmation can overlap a worker finishing a replacement. Wait for that
            // decision so canceling close still proceeds to the decoded-image confirmation.
            afterConfirmation = [this, loaded, filename = std::move(filename), createdUtc,
                                 replacement, token, result = std::move(result)]() mutable {
                finishLoading(loaded, std::move(filename), createdUtc, replacement, token,
                              std::move(result));
            };
            return;
        }
        job = {};
        artifact.reset();
        importing = false;
        status = Status::None;
        if (!result.succeeded()) {
            loadingErrorSource = result.failureStage == ScreenshotExportFailureStage::Internal
                                     ? "Image processing failed. Try again."
                                     : loaded->errorSource;
            if (!loadingErrorSource && result.error.isEmpty())
                loadingErrorSource = "The image could not be read or is no longer available.";
            error = loadingErrorSource ? splitText(loadingErrorSource) : result.error;
            status = Status::LoadError;
            showStatus();
            updateActions();
            emit q.imageLoaded(false);
            return;
        }
        showStatus();
        auto commit = [this, loaded, filename = std::move(filename), createdUtc, token](bool yes) {
            if (closing || token != generation)
                return;
            if (yes) {
                const auto stem = QFileInfo(filename).completeBaseName();
                sourceName = stem.isEmpty()
                                 ? QStringLiteral("Screenshot_%1")
                                       .arg((createdUtc.isValid() ? createdUtc
                                                                  : QDateTime::currentDateTimeUtc())
                                                .toLocalTime()
                                                .toString(QStringLiteral("yyyyMMdd-HHmmss")))
                                 : stem;
                ImageSplitSettings initialSettings;
                if (loaded->initialDirection)
                    initialSettings.direction = *loaded->initialDirection;
                document->setImage(loaded->image, loaded->preview, initialSettings);
                imageReservation = loaded->reservation;
                editor->canvas()->fitToCanvas();
            }
            updateActions();
            emit q.imageLoaded(yes);
        };
        if (replacement && !document->image().isNull())
            askConfirmation(true, std::move(commit));
        else
            commit(true);
    }
    void choose(bool directory) {
        if (unavailable())
            return;
        choosing = true;
        updateActions();
        const quint64 token = generation;
        const QPointer<ImageSplitWindow> guard(&q);
        auto done = [guard, token, directory](QString path) {
            if (!guard || guard->m_impl->closing || token != guard->m_impl->generation)
                return;
            auto& d = *guard->m_impl;
            d.choosing = false;
            d.fileDialog.clear();
            d.updateActions();
            if (!path.isEmpty()) {
                if (directory)
                    d.startExport(std::move(path));
                else
                    guard->replaceFile(path);
            }
        };
        auto& port = directory ? services.chooseDirectory : services.chooseFile;
        if (port) {
            port(editor->window(), std::move(done));
            return;
        }
        auto* dialog = new QFileDialog(editor->window());
        fileDialog = dialog;
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setObjectName(directory ? QStringLiteral("imageSplitFolderDialog")
                                        : QStringLiteral("imageSplitFileDialog"));
        dialog->setWindowTitle(directory ? splitText("Select Destination Folder")
                                         : splitText("Select Image"));
        dialog->setFileMode(directory ? QFileDialog::Directory : QFileDialog::ExistingFile);
        dialog->setAcceptMode(QFileDialog::AcceptOpen);
        if (directory)
            dialog->setOption(QFileDialog::ShowDirsOnly);
        else
            dialog->setNameFilter(imageFilter());
        QObject::connect(dialog, &QFileDialog::finished, &q,
                         [dialog, done = std::move(done)](int code) mutable {
                             done(code == QDialog::Accepted && !dialog->selectedFiles().isEmpty()
                                      ? dialog->selectedFiles().first()
                                      : QString());
                         });
        dialog->open();
    }
    void startExport(QString directory) {
        if (unavailable() || !editor->configurationValid())
            return;
        exporting = true;
        completed = 0;
        total = document->layout().outputCount();
        status = Status::Saving;
        error.clear();
        windowExportErrorSource = nullptr;
        updateActions();
        showStatus();
        const quint64 token = ++generation;
        auto result = std::make_shared<ImageSplitExportResult>();
        progress = std::make_shared<ExportProgress>();
        progress->total = total;
        progressTimer->start();
        const auto writer = services.exportBatch;
        job = ScreenshotExportCoordinator::shared().submit(
            &q, ScreenshotExportCoordinator::Priority::Foreground,
            [image = document->image(), layout = document->layout(),
             directory = std::move(directory), name = sourceName, reservation = imageReservation,
             result, writer,
             progressState = progress](const ScreenshotExportCancellation& cancellation) {
                Q_UNUSED(reservation);
                const auto reportProgress = [progressState](qint64 count, qint64 maximum) {
                    progressState->total = maximum;
                    progressState->completed = count;
                };
                const auto cancelled = [&cancellation] {
                    return cancellation.isCancellationRequested();
                };
                *result = writer ? writer(image, layout, directory, name, cancelled, reportProgress)
                                 : exportSplitImages(image, layout, directory, name, cancelled,
                                                     reportProgress);
                return ScreenshotExportTaskResult{};
            },
            [this, result, token](ScreenshotExportTaskResult task) {
                if (closing || token != generation)
                    return;
                job = {};
                progressTimer->stop();
                progress.reset();
                exporting = false;
                if (!task.succeeded() && result->error.isEmpty())
                    result->error = task.failureStage == ScreenshotExportFailureStage::Internal
                                        ? splitText("The images could not be exported. Try again.")
                                        : task.error;
                if (task.failureStage == ScreenshotExportFailureStage::Internal)
                    windowExportErrorSource = "The images could not be exported. Try again.";
                exportResult = *result;
                exportResult.error = result->errorMessage();
                error = exportResult.error;
                status = result->succeeded() ? Status::Saved : Status::ExportError;
                showStatus();
                updateActions();
                if (exportResult.succeeded()) {
                    if (services.openDirectory)
                        services.openDirectory(exportResult.directory);
                    else
                        QDesktopServices::openUrl(QUrl::fromLocalFile(exportResult.directory));
                }
                emit q.exportFinished(exportResult);
            });
        if (!job.isValid()) {
            progressTimer->stop();
            progress.reset();
            exporting = false;
            exportResult = {};
            exportResult.error = splitText("Export could not be started. Try again.");
            windowExportErrorSource = "Export could not be started. Try again.";
            error = exportResult.error;
            status = Status::ExportError;
            updateActions();
            showStatus();
            emit q.exportFinished(exportResult);
        }
    }
};

ImageSplitWindow::ImageSplitWindow(QObject* parent, ImageSplitWindowServices services)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, std::move(services))) {}
ImageSplitWindow::~ImageSplitWindow() {
    shutdown();
}

void ImageSplitWindow::presentHistory(const storage::CaptureHistoryRecord& record) {
    auto& d = *m_impl;
    if (d.unavailable())
        return;
    d.present();
    d.beginLoading();
    const auto token = d.generation;
    auto loaded = std::make_shared<LoadedImage>();
    const auto reader = d.services.readHistory;
    d.job = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Background,
        [record, loaded, reader](const ScreenshotExportCancellation& cancellation) {
            if (cancellation.isCancellationRequested())
                return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::Cancelled,
                                                           {});
            if (record.result && !reserveImage(record.result->imageSize, loaded))
                return loadingFailure(
                    loaded, ScreenshotExportFailureStage::Source,
                    "The image is too large or there is not enough memory to load it.");
            const auto image = reader ? reader(record)
                                      : storage::ApplicationStorage::instance()
                                            .captureHistory()
                                            .loadResultImagePreservingRecord(record);
            return prepareImage(image.value_or(QImage()), loaded, cancellation);
        },
        [this, record, loaded, token](ScreenshotExportTaskResult result) {
            m_impl->finishLoading(loaded, {}, record.createdUtc, false, token, std::move(result));
        });
    if (!d.job.isValid())
        d.finishLoading(loaded, {}, record.createdUtc, false, token,
                        loadingFailure(loaded, ScreenshotExportFailureStage::Queue,
                                       "Image loading could not be started. Try again."));
}

void ImageSplitWindow::presentPinnedRecord(const QString& recordId) {
    auto& d = *m_impl;
    if (d.unavailable())
        return;
    d.present();
    d.beginLoading();
    const auto token = d.generation;
    auto loaded = std::make_shared<LoadedImage>();
    d.job = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Background,
        [recordId, loaded, swatchStyle = capturePinnedColorSwatchStyle()](
            const ScreenshotExportCancellation& cancellation) {
            const auto cancelled = [&cancellation] {
                return cancellation.isCancellationRequested();
            };
            const auto allocate = [loaded, cancelled](qint64 bytes) {
                if (cancelled())
                    return false;
                if (!loaded->reservation)
                    loaded->reservation = reserveCollageImport(bytes);
                else if (!loaded->reservation->resize(bytes))
                    loaded->reservation.reset();
                if (!loaded->reservation)
                    loaded->errorSource =
                        "The image is too large or there is not enough memory to load it.";
                return loaded->reservation != nullptr;
            };
            auto source = readImageImportPinnedRecord(recordId, cancelled, allocate, swatchStyle);
            loaded->originalFileName = std::move(source.originalFileName);
            loaded->createdUtc = source.createdUtc;
            if (loaded->errorSource)
                return loadingFailure(loaded, ScreenshotExportFailureStage::Source,
                                      loaded->errorSource);
            return prepareImage(std::move(source.image), loaded, cancellation);
        },
        [this, loaded, token](ScreenshotExportTaskResult result) {
            m_impl->finishLoading(loaded, loaded->originalFileName, loaded->createdUtc, false,
                                  token, std::move(result));
        });
    if (!d.job.isValid())
        d.finishLoading(loaded, {}, {}, false, token,
                        loadingFailure(loaded, ScreenshotExportFailureStage::Queue,
                                       "Image loading could not be started. Try again."));
}

void ImageSplitWindow::presentSource(ImageSplitSource source) {
    auto& d = *m_impl;
    if (d.unavailable())
        return;
    d.present();
    d.beginLoading();
    const auto token = d.generation;
    auto loaded = std::make_shared<LoadedImage>();
    loaded->initialDirection = source.initialDirection;
    if (!source.error.isEmpty() || !source.source.isValid()) {
        if (source.error == imageMemoryError())
            loaded->errorSource =
                "The image is too large or there is not enough memory to load it.";
        d.finishLoading(loaded, source.originalFileName, source.createdUtc, false, token,
                        ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::Source,
                                                            source.error));
        return;
    }
    if (!source.outputSize.isEmpty() && !reserveImage(source.outputSize, loaded)) {
        d.finishLoading(
            loaded, source.originalFileName, source.createdUtc, false, token,
            loadingFailure(loaded, ScreenshotExportFailureStage::Source,
                           "The image is too large or there is not enough memory to load it."));
        return;
    }
    d.artifact = std::make_shared<ScreenshotExportArtifact>(source.source);
    if (!d.artifact->requestImage(this, [this, loaded, source,
                                         token](ScreenshotExportImageResult image) {
            auto& impl = *m_impl;
            if (impl.closing || token != impl.generation)
                return;
            if (!image.succeeded()) {
                impl.finishLoading(loaded, source.originalFileName, source.createdUtc, false, token,
                                   ScreenshotExportTaskResult::failure(
                                       ScreenshotExportFailureStage::Render, image.error));
                return;
            }
            impl.job = ScreenshotExportCoordinator::shared().submit(
                this, ScreenshotExportCoordinator::Priority::Background,
                [pixels = std::move(image.image), loaded](const auto& cancellation) {
                    return prepareImage(pixels, loaded, cancellation);
                },
                [this, loaded, source, token](ScreenshotExportTaskResult result) {
                    m_impl->finishLoading(loaded, source.originalFileName, source.createdUtc, false,
                                          token, std::move(result));
                });
            if (!impl.job.isValid())
                impl.finishLoading(
                    loaded, source.originalFileName, source.createdUtc, false, token,
                    loadingFailure(loaded, ScreenshotExportFailureStage::Queue,
                                   "Image loading could not be started. Try again."));
        }))
        d.finishLoading(loaded, source.originalFileName, source.createdUtc, false, token,
                        loadingFailure(loaded, ScreenshotExportFailureStage::Queue,
                                       "Image loading could not be started. Try again."));
}

void ImageSplitWindow::presentImage(QImage image, QString originalFileName, QDateTime createdUtc) {
    const auto size = image.size();
    presentSource({ScreenshotExportSource::fromImage(std::move(image)),
                   std::move(originalFileName),
                   createdUtc,
                   {},
                   size});
}
void ImageSplitWindow::reload() {
    m_impl->choose(false);
}
void ImageSplitWindow::replaceFile(const QString& path) {
    auto& d = *m_impl;
    if (d.unavailable() || path.isEmpty())
        return;
    d.beginLoading();
    const auto token = d.generation;
    auto loaded = std::make_shared<LoadedImage>();
    const auto reader = d.services.readFile;
    d.job = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Background,
        [path, loaded, reader](const ScreenshotExportCancellation& cancellation) {
            const auto cancelled = [&cancellation] {
                return cancellation.isCancellationRequested();
            };
            QImage image;
            if (reader) {
                image = reader(path, cancelled).value_or(QImage());
            } else {
                QString error;
                loaded->reservation = reserveCollageImport(0, &error);
                if (loaded->reservation)
                    image = readImageImportFile(
                        path, std::nullopt, cancelled, [loaded, &error](qint64 bytes) {
                            return loaded->reservation->resize(bytes, &error);
                        });
                if (image.isNull() && !error.isEmpty())
                    return loadingFailure(
                        loaded, ScreenshotExportFailureStage::Source,
                        "The image is too large or there is not enough memory to load it.");
            }
            return prepareImage(std::move(image), loaded, cancellation);
        },
        [this, path, loaded, token](ScreenshotExportTaskResult result) {
            m_impl->finishLoading(loaded, QFileInfo(path).fileName(), {}, true, token,
                                  std::move(result));
        });
    if (!d.job.isValid())
        d.finishLoading(loaded, {}, {}, true, token,
                        loadingFailure(loaded, ScreenshotExportFailureStage::Queue,
                                       "Image loading could not be started. Try again."));
}
void ImageSplitWindow::dropFiles(const QStringList& paths) {
    if (m_impl->unavailable())
        return;
    if (paths.size() != 1) {
        m_impl->status = Impl::Status::DropError;
        m_impl->showStatus();
        return;
    }
    replaceFile(paths.first());
}
void ImageSplitWindow::paste() {
    auto& d = *m_impl;
    if (d.unavailable() || !d.modal->isOpen())
        return;
    auto* clipboard = QApplication::clipboard();
    const auto paths = ScreenshotClipboardContentReader::localImageFilePaths(clipboard->mimeData());
    if (!paths.isEmpty()) {
        replaceFile(paths.first());
        return;
    }
    auto loaded = std::make_shared<LoadedImage>();
    loaded->reservation = reserveCollageImport(0);
    bool rejected = !loaded->reservation;
    auto snapshot = ScreenshotClipboardContentReader::snapshotImage(
        clipboard, editor()->devicePixelRatioF(), [loaded, &rejected](qint64 bytes) {
            rejected = rejected || !loaded->reservation || !loaded->reservation->resize(bytes);
            return !rejected;
        });
    if (!snapshot && !rejected)
        return;
    d.beginLoading();
    const auto token = d.generation;
    const auto fail = [this, loaded, token](ScreenshotExportTaskResult result) {
        m_impl->finishLoading(loaded, {}, {}, true, token, std::move(result));
    };
    if (rejected) {
        fail(loadingFailure(loaded, ScreenshotExportFailureStage::Source,
                            "The image is too large or there is not enough memory to load it."));
        return;
    }
    d.job = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Background,
        [snapshot = std::move(*snapshot), loaded](const auto& cancellation) mutable {
            QImage image = readImageImportClipboard(
                std::move(snapshot),
                [&cancellation] { return cancellation.isCancellationRequested(); },
                [loaded](qint64 bytes) {
                    if (loaded->reservation->resize(bytes))
                        return true;
                    loaded->errorSource =
                        "The image is too large or there is not enough memory to load it.";
                    return false;
                });
            if (loaded->errorSource)
                return loadingFailure(loaded, ScreenshotExportFailureStage::Source,
                                      loaded->errorSource);
            return prepareImage(std::move(image), loaded, cancellation);
        },
        fail);
    if (!d.job.isValid())
        fail(loadingFailure(loaded, ScreenshotExportFailureStage::Queue,
                            "Image loading could not be started. Try again."));
}
void ImageSplitWindow::saveAll() {
    if (!m_impl->unavailable() && m_impl->editor->commitPendingSettings())
        m_impl->choose(true);
}
void ImageSplitWindow::stitchToPdf() {
    auto& d = *m_impl;
    if (d.unavailable() || !d.services.stitchToPdf || !d.editor->commitPendingSettings())
        return;
    ImageSplitSnapshot snapshot{d.document->image(), d.document->layout(), d.sourceName,
                                d.imageReservation};
    if (d.services.stitchToPdf(std::move(snapshot))) {
        d.error.clear();
        d.status = Impl::Status::None;
    } else {
        d.error = splitText("Image Stitching is busy. Try again when it finishes.");
        d.status = Impl::Status::StitchError;
    }
    d.showStatus();
}
void ImageSplitWindow::requestClose() {
    if (m_impl->closing || m_impl->confirming)
        return;
    const auto close = [this](bool accepted) {
        if (accepted) {
            m_impl->closing = true;
            m_impl->cancelWork();
            m_impl->modal->reject();
        }
    };
    if (m_impl->document->image().isNull())
        close(true);
    else
        m_impl->askConfirmation(false, close);
}
void ImageSplitWindow::shutdown() {
    if (!m_impl || m_impl->closing)
        return;
    m_impl->closing = true;
    m_impl->cancelWork();
    m_impl->modal->reject();
}
bool ImageSplitWindow::busy() const {
    return m_impl->exporting;
}
bool ImageSplitWindow::loading() const {
    return m_impl->importing;
}
ImageSplitDocument* ImageSplitWindow::document() const {
    return m_impl->document;
}
components::ImageSplitEditorWidget* ImageSplitWindow::editor() const {
    return m_impl->editor;
}
AdModal* ImageSplitWindow::modal() const {
    return m_impl->modal;
}
QString ImageSplitWindow::imageName() const {
    return m_impl->sourceName;
}
QString ImageSplitWindow::lastError() const {
    return m_impl->error;
}

ImageSplitWindowCoordinator::ImageSplitWindowCoordinator(QObject* parent,
                                                         ImageSplitWindowServices services)
    : QObject(parent), m_services(std::move(services)) {}
ImageSplitWindowCoordinator::~ImageSplitWindowCoordinator() {
    shutdown();
}
ImageSplitWindow* ImageSplitWindowCoordinator::createWindow() {
    if (m_shutdown)
        return nullptr;
    auto* window = new ImageSplitWindow(this, m_services);
    m_windows.append(window);
    connect(window, &ImageSplitWindow::closed, this, [this, window] {
        m_windows.removeAll(window);
        window->deleteLater();
    });
    return window;
}
void ImageSplitWindowCoordinator::presentHistory(const storage::CaptureHistoryRecord& record) {
    if (auto* window = createWindow())
        window->presentHistory(record);
}
void ImageSplitWindowCoordinator::presentSource(ImageSplitSource source) {
    if (auto* window = createWindow())
        window->presentSource(std::move(source));
}
void ImageSplitWindowCoordinator::presentSelectedFiles(
    std::shared_ptr<platform::SelectedFileBackend> backend, platform::SelectedFileTarget target) {
    auto* window = createWindow();
    if (!window)
        return;
    auto& d = *window->m_impl;
    d.present();
    if (!backend || !backend->isValidTarget(target))
        return;
    d.beginLoading();
    const auto token = d.generation;
    auto selection = std::make_shared<platform::SelectedFileResult>();
    auto paths = std::make_shared<QStringList>();
    d.job = ScreenshotExportCoordinator::shared().submit(
        window, ScreenshotExportCoordinator::Priority::Background,
        [backend = std::move(backend), target, selection,
         paths](const ScreenshotExportCancellation& cancellation) {
            const auto cancelled = [&cancellation] {
                return cancellation.isCancellationRequested();
            };
            *selection = backend->selectedFiles(target, cancelled);
            if (selection->error == platform::SelectedFileError::None && !cancelled()) {
                QStringList images;
                const auto extensions =
                    ScreenshotClipboardContentReader::supportedImageFileExtensions();
                for (const auto& path : selection->paths) {
                    if (extensions.contains(QFileInfo(path).suffix().toLower()))
                        images.append(path);
                }
                for (const auto& file : ScreenshotClipboardContentReader::snapshotLocalFiles(
                         images, cancelled,
                         ScreenshotClipboardContentReader::FilePurpose::ImageImport))
                    paths->append(file.absolutePath);
            }
            return cancelled() ? ScreenshotExportTaskResult::failure(
                                     ScreenshotExportFailureStage::Cancelled, {})
                               : ScreenshotExportTaskResult{};
        },
        [this, window, selection, paths, token](ScreenshotExportTaskResult result) {
            auto& impl = *window->m_impl;
            if (impl.closing || token != impl.generation)
                return;
            if (!result.succeeded() || selection->error != platform::SelectedFileError::None) {
                auto failed = std::make_shared<LoadedImage>();
                impl.finishLoading(failed, {}, {}, false, token,
                                   loadingFailure(failed, ScreenshotExportFailureStage::Source,
                                                  selectedFileErrorSource(selection->error)));
                return;
            }
            impl.job = {};
            impl.importing = false;
            impl.status = ImageSplitWindow::Impl::Status::None;
            impl.showStatus();
            impl.updateActions();
            for (qsizetype index = 0; index < paths->size(); ++index) {
                auto* receiving = index == 0 ? window : createWindow();
                if (!receiving)
                    break;
                receiving->m_impl->present();
                receiving->replaceFile(paths->at(index));
            }
        });
    if (!d.job.isValid()) {
        auto failed = std::make_shared<LoadedImage>();
        d.finishLoading(failed, {}, {}, false, token,
                        loadingFailure(failed, ScreenshotExportFailureStage::Queue,
                                       "Image loading could not be started. Try again."));
    }
}
void ImageSplitWindowCoordinator::presentPinnedRecord(const QString& recordId) {
    if (auto* window = createWindow())
        window->presentPinnedRecord(recordId);
}
void ImageSplitWindowCoordinator::shutdown() {
    m_shutdown = true;
    const auto active = m_windows;
    for (const auto& window : active)
        if (window)
            window->shutdown();
}
QVector<ImageSplitWindow*> ImageSplitWindowCoordinator::windows() const {
    QVector<ImageSplitWindow*> result;
    for (const auto& window : m_windows)
        if (window)
            result.append(window);
    return result;
}
} // namespace snow_shot::presentation
