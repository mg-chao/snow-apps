// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/collagewindow.h"
#include "snow_shot/presentation/imagesplitdocument.h"
#include "snow_shot/presentation/collagesettingsstore.h"
#include "snow_shot/presentation/components/collageeditorwidget.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/screenshotencodingsettings.h"
#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/screenshotmanualsave.h"
#include "snow_shot/presentation/screenshotsaveasfiledialog.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/windowcloseshortcut.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/capturehistoryrepository.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "../services/imageimportsourcereader_p.h"
#include "icon_renderer.h"
#include "widgets/button.h"
#include "widgets/message.h"
#include "widgets/modal.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMimeData>
#include <QPointer>
#include <QScreen>
#include <QSet>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>

namespace snow_shot::presentation {
namespace {
using adqt::widgets::AdButton;
using adqt::widgets::AdMessage;
using adqt::widgets::AdMessageHandle;
using adqt::widgets::AdMessageService;
using adqt::widgets::AdModal;

[[maybe_unused]] constexpr const char* labels[] = {
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "%1: %2"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Add images"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow",
                      "Allow Snow Shot to access Finder in System Settings > "
                      "Privacy & Security > Automation, then try again"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow",
                      "All images will be removed from this stitching session"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Cancel"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Clear all"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Clear all images?"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Clipboard image"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Image stitching"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Copy"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Copying stitched image…"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Copying could not be started. Try again"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow",
                      "Could not read the selected image files. Please try again"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Discard"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Discard this stitching session?"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Export could not be started. Try again"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Image loading could not be started. Try again"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Images"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Loading images: %1 of %2"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Pinned window %1"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Preparing stitched image…"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Reading selected image files…"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Replace image"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Save"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Save as file"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Saving stitched image…"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Save PDF document"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Select destination folder"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "PDF documents"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Exporting PDF page %1 of %2: %3%"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Canceling PDF export…"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow",
                      "PDF export was canceled. Your images and edits are preserved"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Saved pages: %1\nFiles:\n%2\n%3"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Pages %1–%2 were not saved"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Page %1 was not saved"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "The PDFs could not be exported. Try again"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Saving could not be started. Try again"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Screenshot %1"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "Some images could not be loaded:\n%1"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "The final screenshot is unavailable"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow",
                      "The file manager is unavailable. Open it and try again"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow",
                      "The file manager took too long to return the selected files. Please try "
                      "again"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow",
                      "The image could not be decoded or is no longer available"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow",
                      "The images and edits in this stitching session will be discarded"),
    QT_TRANSLATE_NOOP("ImageStitchingWindow", "The layout settings could not be remembered: %1"),
};

QString imageStitchingText(const char* value) {
    return QCoreApplication::translate("ImageStitchingWindow", value);
}

QString imageFilter() {
    QStringList patterns;
    for (const QString& extension :
         ScreenshotClipboardContentReader::supportedImageFileExtensions())
        patterns.append(QStringLiteral("*.") + extension);
    // Qt requires ASCII parentheses around the patterns, regardless of the display language.
    return QStringLiteral("%1 (%2)").arg(imageStitchingText("Images"), patterns.join(u' '));
}

QString selectedFileErrorText(platform::SelectedFileError error) {
    switch (error) {
    case platform::SelectedFileError::PermissionDenied:
        return imageStitchingText("Allow Snow Shot to access Finder in System Settings > Privacy & "
                                  "Security > Automation, then try again");
    case platform::SelectedFileError::Timeout:
        return imageStitchingText(
            "The file manager took too long to return the selected files. Please "
            "try again");
    case platform::SelectedFileError::Unavailable:
        return imageStitchingText("The file manager is unavailable. Open it and try again");
    case platform::SelectedFileError::QueryFailed:
        return imageStitchingText("Could not read the selected image files. Please try again");
    case platform::SelectedFileError::None:
        return {};
    }
    return {};
}

struct SelectedFilesImport {
    std::shared_ptr<platform::SelectedFileBackend> backend;
    platform::SelectedFileTarget target;
};

struct ImportItem {
    std::optional<storage::CaptureHistoryRecord> record;
    QString pinnedId;
    QString path;
    QString label;
    quint64 replacement = 0;
    std::optional<ScreenshotPinnedImageSnapshot> live = std::nullopt;
    std::optional<SelectedFilesImport> selection = std::nullopt;
    std::optional<ScreenshotClipboardLocalImage> file = std::nullopt;
    std::optional<ScreenshotClipboardContentSnapshot> clipboard = std::nullopt;
    std::shared_ptr<ImageStitchingImportReservation> reservation = {};
    std::shared_ptr<const ImageSplitSnapshot> split = {};
    qint64 splitIndex = 0;
};

struct ImportedImage {
    QImage preview;
    std::shared_ptr<ImageStitchingImportReservation> reservation;
};
struct PdfProgressState final {
    std::atomic<qsizetype> completedPages{0};
    std::atomic<qsizetype> totalPages{0};
    std::atomic<qsizetype> pageNumber{0};
    std::atomic<int> pagePercent{0};
};
} // namespace

struct ImageStitchingWindow::Impl {
    enum class Status : quint8 {
        None,
        Loading,
        ReadingSelection,
        Preparing,
        Copying,
        Saving,
        ImportFailures,
        PdfSaving,
        PdfCancelling,
        PdfPartial,
        External
    };
    ImageStitchingWindow& q;
    ImageStitchingWindowServices services;
    ImageStitchingDocument* document;
    components::ImageStitchingEditorWidget* editor;
    AdModal* modal;
    AdButton* cancelButton;
    AdButton* copyButton;
    AdButton* saveButton;
    QPointer<AdModal> confirmation;
    QPointer<QFileDialog> fileDialog;
    QPointer<AdMessageHandle> statusMessage;
    ScreenshotExportJobHandle job;
    ScreenshotManualSaveHandle nativeSave;
    ScreenshotClipboardCommitScope clipboard;
    std::shared_ptr<ScreenshotExportArtifact> artifact;
    std::shared_ptr<ScreenshotExportArtifact> importArtifact;
    std::shared_ptr<void> clipboardReservation;
    QVector<ImportItem> imports;
    QStringList failures;
    QList<platform::SelectedFileError> selectionFailures;
    qsizetype importIndex = 0;
    qint64 importStarted = 0;
    qint64 importTotal = 0;
    quint64 generation = 0;
    bool importing = false;
    bool exporting = false;
    bool closing = false;
    bool confirmationPending = false;
    bool copying = false;
    Status status = Status::None;
    bool confirmationClear = false;
    quint64 confirmationGeneration = 0;
    bool pdfExporting = false;
    bool choosingPdf = false;
    bool pdfCancelRequested = false;
    QTimer* pdfProgressTimer;
    std::shared_ptr<PdfProgressState> pdfProgress;
    std::shared_ptr<std::atomic_bool> pdfCancellation;
    ImageStitchingPdfExportResult lastPdfResult;
    QSet<QString> pendingPdfSettingFields;
    WindowShortcutManager* pasteShortcuts = nullptr;

    Impl(ImageStitchingWindow& owner, ImageStitchingWindowServices ports)
        : q(owner), services(std::move(ports)), document(new ImageStitchingDocument(&q)),
          editor(new components::ImageStitchingEditorWidget(document)), modal(new AdModal(&q)),
          pdfProgressTimer(new QTimer(&q)) {
        auto& storage = storage::ApplicationStorage::instance();
        if (!storage.isInitialized())
            static_cast<void>(storage.initialize());
        document->setSettings(ImageStitchingSettingsStore(storage.configuration()).read());
        document->setPdfSettings(ImageStitchingSettingsStore(storage.configuration()).readPdf());
        pdfProgressTimer->setInterval(100);
        QObject::connect(pdfProgressTimer, &QTimer::timeout, &q, [this] {
            if (pdfExporting && !choosingPdf && !pdfCancelRequested)
                showStatus(Status::PdfSaving);
        });
        modal->setObjectName(QStringLiteral("collageModal"));
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
        footer->setObjectName(QStringLiteral("collageFooter"));
        footer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* layout = new QHBoxLayout(footer);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(
            styles::ThemeManager::instance().themeColorScheme().metricAlias.marginXS);
        layout->addWidget(editor->outputDescriptionWidget(), 1, Qt::AlignVCenter);
        editor->outputDescriptionWidget()->show();
        cancelButton = new AdButton(footer);
        copyButton = new AdButton(footer);
        saveButton = new AdButton(footer);
        cancelButton->setObjectName(QStringLiteral("collageCancel"));
        copyButton->setObjectName(QStringLiteral("collageCopy"));
        saveButton->setObjectName(QStringLiteral("collageSave"));
        saveButton->setButtonStyle(AdButton::ButtonStyle::Solid);
        saveButton->setAccentRole(AdButton::AccentRole::Primary);
        for (auto* button : {cancelButton, copyButton, saveButton}) {
            button->setFocusPolicy(Qt::StrongFocus);
            layout->addWidget(button);
        }
        modal->setFooterWidget(footer);
        QObject::connect(cancelButton, &AdButton::clicked, &q, &ImageStitchingWindow::requestClose);
        QObject::connect(copyButton, &AdButton::clicked, &q, &ImageStitchingWindow::copy);
        QObject::connect(saveButton, &AdButton::clicked, &q, &ImageStitchingWindow::save);
        QObject::connect(modal, &AdModal::closeRequested, &q,
                         [this](AdModal::CloseReason) { q.requestClose(); });
        QObject::connect(modal, &AdModal::finished, &q, [this](AdModal::DialogCode) {
            cancelWork();
            closing = true;
            emit q.closed();
        });
        QObject::connect(document, &ImageStitchingDocument::changed, &q,
                         [this] { updateActions(); });
        QObject::connect(editor, &components::ImageStitchingEditorWidget::addImagesRequested, &q,
                         [this] { chooseFiles(0); });
        QObject::connect(editor, &components::ImageStitchingEditorWidget::imageFilesDropped, &q,
                         &ImageStitchingWindow::addFiles);
        QObject::connect(editor, &components::ImageStitchingEditorWidget::replaceImageRequested, &q,
                         [this](quint64 id) { chooseFiles(id); });
        QObject::connect(editor, &components::ImageStitchingEditorWidget::clearRequested, &q,
                         &ImageStitchingWindow::requestClear);
        QObject::connect(editor, &components::ImageStitchingEditorWidget::settingsEdited, &q,
                         [this](const QString& field) { persistSettings({field}); });
        QObject::connect(document, &ImageStitchingDocument::customGridAdjusted, &q,
                         [this](const QStringList& fields) { persistSettings(fields); });
        QObject::connect(&LanguageManager::instance(), &LanguageManager::languageChanged, &q,
                         [this] { retranslate(); });
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, &q, &ImageStitchingWindow::shutdown);
        pasteShortcuts = new WindowShortcutManager(&q);
        WindowShortcutManager::Binding binding;
        binding.id = QStringLiteral("collagePaste");
        for (const auto& sequence : QKeySequence::keyBindings(QKeySequence::Paste)) {
            if (sequence.count() == 1)
                binding.keyCombinations.append(sequence[0]);
        }
        binding.priority = WindowShortcutManager::StandardPriority::WindowCommand;
        binding.canActivate = [this](const auto& context) {
            const auto* receiver = qobject_cast<QWidget*>(context.receiver);
            return canPaste() && receiver && receiver->window() == editor->window() &&
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

    void persistSettings(const QStringList& fields) {
        auto& configuration = storage::ApplicationStorage::instance().configuration();
        if (!fields.isEmpty() && fields.first().startsWith(QStringLiteral("pdf/"))) {
            for (const auto& field : fields)
                pendingPdfSettingFields.insert(field.mid(4));
            const auto settings = document->pdfSettings();
            if (!validateImageStitchingPdfSettings(settings).isEmpty())
                return;
            const auto persistable = ImageStitchingSettingsStore::persistablePdfFields(
                pendingPdfSettingFields.values(), settings);
            if (persistable.isEmpty())
                return;
            if (!ImageStitchingSettingsStore(configuration).writePdf(persistable, settings))
                externalStatus(imageStitchingText("The layout settings could not be remembered: %1")
                                   .arg(configuration.lastError()));
            else
                for (const auto& field : persistable)
                    pendingPdfSettingFields.remove(field);
            return;
        }
        if (!ImageStitchingSettingsStore(configuration).write(fields, document->settings()))
            externalStatus(imageStitchingText("The layout settings could not be remembered: %1")
                               .arg(configuration.lastError()));
    }

    void retranslate() {
        modal->setWindowTitle(imageStitchingText("Image stitching"));
        cancelButton->setText(imageStitchingText("Cancel"));
        copyButton->setText(imageStitchingText("Copy"));
        saveButton->setText(imageStitchingText("Save"));
        editor->retranslateUi();
        for (auto* button : {cancelButton, copyButton, saveButton})
            button->setAccessibleName(button->text());
        if (confirmation)
            translateConfirmation();
        if (status != Status::External)
            showStatus(status);
    }

    QStringList importFailures() const {
        QStringList result = failures;
        for (const auto error : selectionFailures)
            result.append(selectedFileErrorText(error));
        return result;
    }

    void showStatus(Status next) {
        status = next;
        QString message;
        switch (next) {
        case Status::Loading:
            message =
                imageStitchingText("Loading images: %1 of %2").arg(importStarted).arg(importTotal);
            break;
        case Status::ReadingSelection:
            message = imageStitchingText("Reading selected image files…");
            break;
        case Status::Preparing:
            message = imageStitchingText("Preparing stitched image…");
            break;
        case Status::Copying:
            message = imageStitchingText("Copying stitched image…");
            break;
        case Status::Saving:
            message = imageStitchingText("Saving stitched image…");
            break;
        case Status::ImportFailures:
            message = imageStitchingText("Some images could not be loaded:\n%1")
                          .arg(importFailures().join(u'\n'));
            break;
        case Status::PdfSaving:
            if (pdfProgress)
                message = imageStitchingText("Exporting PDF page %1 of %2: %3%")
                              .arg(std::max<qsizetype>(1, pdfProgress->pageNumber.load()))
                              .arg(pdfProgress->totalPages.load())
                              .arg(pdfProgress->pagePercent.load());
            break;
        case Status::PdfCancelling:
            message = imageStitchingText("Canceling PDF export…");
            break;
        case Status::PdfPartial: {
            QString reason =
                lastPdfResult.cancelled
                    ? imageStitchingText(
                          "PDF export was canceled. Your images and edits are preserved")
                    : lastPdfResult.errorMessage();
            if (reason.isEmpty())
                reason = imageStitchingText("The PDFs could not be exported. Try again");
            if (!lastPdfResult.completedPages.isEmpty()) {
                QStringList pages;
                for (auto page : lastPdfResult.completedPages)
                    pages.append(QString::number(page));
                const auto firstRemaining = lastPdfResult.completedPages.last() + 1;
                if (firstRemaining <= lastPdfResult.totalCount)
                    reason += u'\n' +
                              (firstRemaining == lastPdfResult.totalCount
                                   ? imageStitchingText("Page %1 was not saved").arg(firstRemaining)
                                   : imageStitchingText("Pages %1–%2 were not saved")
                                         .arg(firstRemaining)
                                         .arg(lastPdfResult.totalCount));
                reason = imageStitchingText("Saved pages: %1\nFiles:\n%2\n%3")
                             .arg(pages.join(QStringLiteral(", ")), lastPdfResult.paths.join(u'\n'),
                                  reason);
            }
            AdMessage::Request request;
            request.key = QStringLiteral("collageStatus");
            request.content = reason;
            request.durationMs = 0;
            statusMessage = lastPdfResult.cancelled
                                ? AdMessageService::info(std::move(request), editor->window())
                                : AdMessageService::error(std::move(request), editor->window());
            return;
        }
        case Status::None:
        case Status::External:
            break;
        }
        showMessage(message, next == Status::ImportFailures);
    }

    void showMessage(const QString& message, bool error = false) {
        if (message.isEmpty()) {
            if (statusMessage)
                statusMessage->close();
            statusMessage = nullptr;
            return;
        }
        AdMessage::Request request;
        request.key = QStringLiteral("collageStatus");
        request.content = message;
        if (error) {
            statusMessage = AdMessageService::error(std::move(request), editor->window());
        } else {
            request.durationMs = 0;
            statusMessage = AdMessageService::loading(std::move(request), editor->window());
        }
    }

    void externalStatus(const QString& message) {
        status = message.isEmpty() ? Status::None : Status::External;
        showMessage(message, true);
    }

    void translateConfirmation() {
        confirmation->setWindowTitle(confirmationClear
                                         ? imageStitchingText("Clear all images?")
                                         : imageStitchingText("Discard this stitching session?"));
        confirmation->setText(
            confirmationClear
                ? imageStitchingText("All images will be removed from this stitching session")
                : imageStitchingText(
                      "The images and edits in this stitching session will be discarded"));
        confirmation->setAcceptText(confirmationClear ? imageStitchingText("Clear all")
                                                      : imageStitchingText("Discard"));
        confirmation->setRejectText(imageStitchingText("Cancel"));
    }

    void updateActions() {
        const bool pdf = document->outputType() == ImageStitchingOutputType::PdfDocument;
        const bool valid =
            pdf ? !document->items().isEmpty() &&
                      validateImageStitchingPdfSettings(document->pdfSettings()).isEmpty()
                : document->layout().isValid();
        const bool ready = !closing && !importing && !exporting && !confirmationPending && valid;
        copyButton->setVisible(!pdf);
        copyButton->setEnabled(ready);
        saveButton->setEnabled(ready);
        copyButton->setBusy(exporting && copying);
        saveButton->setBusy(exporting && !copying);
        cancelButton->setEnabled(!pdfCancelRequested);
        editor->setLoading(importing);
        editor->setExportBusy(exporting);
    }

    bool canPaste() const {
        return modal->isOpen() && !closing && !importing && !exporting && !confirmationPending &&
               !fileDialog && !choosingPdf;
    }

    void cancelWork() {
        ++generation;
        job.cancel();
        job = {};
        if (pdfCancellation)
            pdfCancellation->store(true);
        pdfProgressTimer->stop();
        pdfCancellation.reset();
        pdfProgress.reset();
        pdfExporting = false;
        choosingPdf = false;
        pdfCancelRequested = false;
        clipboard.cancelAll();
        nativeSave.cancel();
        nativeSave = {};
        if (artifact)
            artifact->cancel();
        artifact.reset();
        if (importArtifact)
            importArtifact->cancel();
        importArtifact.reset();
        clipboardReservation.reset();
        importing = false;
        exporting = false;
        imports.clear();
        showStatus(Status::None);
        updateActions();
        if (fileDialog)
            fileDialog->reject();
        if (confirmation)
            confirmation->reject();
    }

    void finishExport(bool success, const QString& error = {}) {
        if (closing)
            return;
        closing = success;
        artifact.reset();
        clipboardReservation.reset();
        showStatus(Status::None);
        const QPointer<ImageStitchingWindow> guard(&q);
        const quint64 token = generation;
        emit q.exportFinished(success);
        if (!guard || token != generation)
            return;
        exporting = false;
        updateActions();
        if (success) {
            modal->accept();
        } else {
            status = error.isEmpty() ? Status::None : Status::External;
            showMessage(error, true);
        }
    }

    void askConfirmation(bool clear, std::function<void()> accepted) {
        if (closing || confirmationPending)
            return;
        confirmationPending = true;
        const auto confirmationToken = ++confirmationGeneration;
        updateActions();
        const QPointer<ImageStitchingWindow> guard(&q);
        auto complete = [guard, confirmationToken, accepted = std::move(accepted)](bool yes) {
            if (!guard || guard->m_impl->closing || !guard->m_impl->confirmationPending ||
                confirmationToken != guard->m_impl->confirmationGeneration)
                return;
            guard->m_impl->confirmationPending = false;
            guard->m_impl->updateActions();
            if (yes)
                accepted();
        };
        if (services.confirm) {
            services.confirm(editor->window(), clear, std::move(complete));
            return;
        }
        auto* dialog = new AdModal(&q);
        confirmation = dialog;
        confirmationClear = clear;
        dialog->setObjectName(QStringLiteral("collageDiscardConfirmation"));
        dialog->setMode(AdModal::Mode::Window);
        dialog->setOwnerWindow(editor->window());
        dialog->setWindowModality(Qt::WindowModal);
        dialog->setPreset(AdModal::Preset::Confirm);
        translateConfirmation();
        dialog->setAcceptAccentRole(AdButton::AccentRole::Danger);
        QObject::connect(dialog, &AdModal::finished, &q,
                         [complete = std::move(complete)](AdModal::DialogCode code) {
                             complete(code == AdModal::DialogCode::Accepted);
                         });
        QObject::connect(dialog, &AdModal::finished, dialog, &QObject::deleteLater);
        dialog->present();
    }

    void chooseFiles(quint64 replacement) {
        if (closing || exporting || importing || fileDialog)
            return;
        auto* dialog = new QFileDialog(editor->window());
        fileDialog = dialog;
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowModality(Qt::WindowModal);
        dialog->setWindowTitle(replacement ? imageStitchingText("Replace image")
                                           : imageStitchingText("Add images"));
        dialog->setNameFilter(imageFilter());
        dialog->setFileMode(replacement ? QFileDialog::ExistingFile : QFileDialog::ExistingFiles);
        QObject::connect(dialog, &QFileDialog::filesSelected, &q,
                         [this, replacement](const QStringList& paths) {
                             if (paths.isEmpty() || closing)
                                 return;
                             if (replacement)
                                 q.replaceFile(replacement, paths.first());
                             else
                                 q.addFiles(paths);
                         });
        dialog->open();
    }

    void beginImport(QVector<ImportItem> requested) {
        if (closing || exporting || requested.isEmpty())
            return;
        qint64 requestedCount = 0;
        for (const auto& item : requested)
            requestedCount += item.split ? item.split->layout.outputCount() : 1;
        if (importing) {
            importTotal += requestedCount;
            imports.append(std::move(requested));
            showStatus(status == Status::ReadingSelection ? Status::ReadingSelection
                                                          : Status::Loading);
            return;
        }
        imports = std::move(requested);
        failures.clear();
        selectionFailures.clear();
        importIndex = 0;
        importStarted = 0;
        importTotal = requestedCount;
        importing = true;
        ++generation;
        updateActions();
        nextImport();
    }

    void finishImport(const ImportItem& request, const std::shared_ptr<ImportedImage>& decoded,
                      quint64 token, ScreenshotExportTaskResult result) {
        if (closing || token != generation)
            return;
        importArtifact.reset();
        bool failed = !result.succeeded();
        if (!result.succeeded()) {
            failures.append(imageStitchingText("%1: %2").arg(request.label, result.error));
        } else {
            const bool accepted =
                request.replacement
                    ? document->replace(request.replacement, std::move(result.image), request.label,
                                        std::move(decoded->preview))
                    : document->append(std::move(result.image), request.label,
                                       std::move(decoded->preview)) != 0;
            if (!accepted)
                failures.append(
                    imageStitchingText("%1: %2").arg(request.label, document->lastError()));
            failed = !accepted;
        }
        if (failed)
            discardRemainingSplit(request);
        decoded->reservation.reset();
        nextImport();
    }

    void discardRemainingSplit(const ImportItem& request) {
        // A failed crop or admission must not enqueue millions of identical failures.
        if (request.split && importIndex < imports.size() &&
            imports.at(importIndex).split == request.split) {
            importTotal -= request.split->layout.outputCount() - imports.at(importIndex).splitIndex;
            imports[importIndex++] = {};
        }
    }

    void importLive(const ImportItem& request, const std::shared_ptr<ImportedImage>& decoded,
                    quint64 token) {
        const auto fail = [this, request, decoded, token](QString error) {
            QTimer::singleShot(0, &q, [this, request, decoded, token, error = std::move(error)] {
                finishImport(request, decoded, token,
                             ScreenshotExportTaskResult::failure(
                                 ScreenshotExportFailureStage::Source, error));
            });
        };
        const auto& snapshot = *request.live;
        if (!snapshot.error.isEmpty()) {
            fail(snapshot.error);
            return;
        }
        if (!snapshot.source.isValid() || snapshot.outputSize.isEmpty()) {
            fail(imageStitchingText("The image could not be decoded or is no longer available"));
            return;
        }
        QString error;
        const qint64 bytes = imageStitchingImageImportBytes(snapshot.outputSize, &error);
        if (bytes == 0) {
            fail(error);
            return;
        }
        decoded->reservation =
            reserveImageStitchingImport(std::max(bytes, snapshot.renderingBytes), &error);
        if (!decoded->reservation) {
            fail(error);
            return;
        }
        importArtifact = std::make_shared<ScreenshotExportArtifact>(snapshot.source);
        if (!importArtifact->requestImage(&q, [this, request, decoded,
                                               token](ScreenshotExportImageResult rendered) {
                if (closing || token != generation)
                    return;
                importArtifact.reset();
                if (!rendered.succeeded()) {
                    finishImport(request, decoded, token,
                                 ScreenshotExportTaskResult::failure(
                                     ScreenshotExportFailureStage::Render, rendered.error));
                    return;
                }
                job = ScreenshotExportCoordinator::shared().submit(
                    &q, ScreenshotExportCoordinator::Priority::Background,
                    [image = std::move(rendered.image),
                     decoded](const ScreenshotExportCancellation& cancellation) {
                        if (cancellation.isCancellationRequested())
                            return ScreenshotExportTaskResult::failure(
                                ScreenshotExportFailureStage::Cancelled, {});
                        decoded->preview =
                            image.width() <= 256 && image.height() <= 256
                                ? image
                                : snowCanvasScaleImage(
                                      image, image.size().scaled(256, 256, Qt::KeepAspectRatio),
                                      Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                        ScreenshotExportTaskResult result;
                        result.image = image;
                        return result;
                    },
                    [this, request, decoded, token](ScreenshotExportTaskResult result) {
                        finishImport(request, decoded, token, std::move(result));
                    });
                if (!job.isValid())
                    finishImport(
                        request, decoded, token,
                        ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::Queue,
                            imageStitchingText("Image loading could not be started. Try again")));
            }))
            fail(imageStitchingText("Image loading could not be started. Try again"));
    }

    void importSelection(const SelectedFilesImport& request, quint64 token) {
        auto selection = std::make_shared<platform::SelectedFileResult>();
        auto files = std::make_shared<QList<ScreenshotClipboardLocalImage>>();
        job = ScreenshotExportCoordinator::shared().submit(
            &q, ScreenshotExportCoordinator::Priority::Background,
            [request, selection, files](const ScreenshotExportCancellation& cancellation) {
                const auto cancelled = [&cancellation] {
                    return cancellation.isCancellationRequested();
                };
                *selection = request.backend->selectedFiles(request.target, cancelled);
                if (cancelled())
                    return ScreenshotExportTaskResult::failure(
                        ScreenshotExportFailureStage::Cancelled, {});
                if (selection->error == platform::SelectedFileError::None) {
                    QStringList images;
                    const auto extensions =
                        ScreenshotClipboardContentReader::supportedImageFileExtensions();
                    for (const auto& path : selection->paths) {
                        if (extensions.contains(QFileInfo(path).suffix().toLower()))
                            images.append(path);
                    }
                    *files = ScreenshotClipboardContentReader::snapshotLocalFiles(
                        images, cancelled,
                        ScreenshotClipboardContentReader::FilePurpose::ImageImport);
                    selection->paths.clear();
                }
                if (cancelled())
                    return ScreenshotExportTaskResult::failure(
                        ScreenshotExportFailureStage::Cancelled, {});
                return ScreenshotExportTaskResult{};
            },
            [this, selection, files, token](ScreenshotExportTaskResult result) {
                if (closing || token != generation)
                    return;
                imports.removeAt(--importIndex);
                --importStarted;
                --importTotal;
                if (!result.succeeded()) {
                    selectionFailures.append(platform::SelectedFileError::QueryFailed);
                } else if (selection->error != platform::SelectedFileError::None) {
                    selectionFailures.append(selection->error);
                } else {
                    importTotal += files->size();
                    for (qsizetype index = 0; index < files->size(); ++index) {
                        const auto& file = files->at(index);
                        ImportItem item{{}, {}, file.absolutePath, file.absolutePath, 0};
                        item.file = file;
                        imports.insert(importIndex + index, std::move(item));
                    }
                }
                nextImport();
            });
        if (!job.isValid()) {
            selectionFailures.append(platform::SelectedFileError::QueryFailed);
            QTimer::singleShot(0, &q, [this, token] {
                if (token == generation)
                    nextImport();
            });
        }
    }

    void nextImport() {
        if (closing || !importing)
            return;
        if (importIndex >= imports.size()) {
            importing = false;
            imports.clear();
            job = {};
            updateActions();
            const auto errors = importFailures();
            showStatus(errors.isEmpty() ? Status::None : Status::ImportFailures);
            emit q.importFinished(errors);
            return;
        }
        // Consume the source now; processed snapshots must not retain raster donors or rendered
        // caches while subsequent imports are pending. The worker/completion owns this request.
        // Moving a small std::function may retain its callable in the source. Explicitly
        // empty the consumed slot so completed image producers retire before the batch ends.
        ImportItem request;
        auto& queued = imports[importIndex];
        if (queued.split) {
            request = queued;
            request.label =
                QStringLiteral("%1 (%2)").arg(request.split->imageName).arg(request.splitIndex + 1);
            if (++queued.splitIndex == queued.split->layout.outputCount())
                imports[importIndex++] = {};
        } else {
            request = std::exchange(imports[importIndex++], ImportItem{});
        }
        ++importStarted;
        showStatus(request.selection ? Status::ReadingSelection : Status::Loading);
        const quint64 token = generation;
        if (request.selection) {
            importSelection(*request.selection, token);
            return;
        }
        auto decoded = std::make_shared<ImportedImage>();
        if (request.live) {
            importLive(request, decoded, token);
            return;
        }
        if (!request.pinnedId.isEmpty() && services.requestPinnedSnapshot) {
            const QPointer<ImageStitchingWindow> guard(&q);
            const QString id = request.pinnedId;
            services.requestPinnedSnapshot(
                id, [guard, request = std::move(request), decoded,
                     token](std::optional<ScreenshotPinnedImageSnapshot> snapshot) mutable {
                    if (!guard || guard->m_impl->closing || token != guard->m_impl->generation)
                        return;
                    if (snapshot) {
                        request.live = std::move(snapshot);
                        guard->m_impl->importLive(request, decoded, token);
                    } else {
                        guard->m_impl->importStored(request, decoded, token);
                    }
                });
            return;
        }
        importStored(request, decoded, token);
    }

    void importStored(const ImportItem& request, const std::shared_ptr<ImportedImage>& decoded,
                      quint64 token) {
        const auto historyReader = services.readHistory;
        job = ScreenshotExportCoordinator::shared().submit(
            &q, ScreenshotExportCoordinator::Priority::Background,
            [request, decoded, historyReader,
             preparation = captureScreenshotPinnedRenderPreparation()](
                const ScreenshotExportCancellation& cancellation) {
                QString error;
                decoded->reservation = request.reservation ? request.reservation
                                                           : reserveImageStitchingImport(0, &error);
                if (!decoded->reservation)
                    return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::Source,
                                                               error);
                const auto cancelled = [&cancellation] {
                    return cancellation.isCancellationRequested();
                };
                QImage image;
                if (request.split) {
                    const QRect region = request.split->layout.regionAt(request.splitIndex);
                    const qint64 bytes = imageStitchingImageImportBytes(region.size(), &error);
                    const qint64 nativeBytes =
                        ((qint64(region.width()) * request.split->image.depth() + 31) / 32) * 4 *
                        region.height();
                    if (bytes == 0 ||
                        !decoded->reservation->resize(std::max(bytes, nativeBytes * 2), &error))
                        return ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::Source, error);
                    if (!cancelled())
                        image = request.split->image.copy(region);
                } else if (request.clipboard) {
                    image = readImageImportClipboard(
                        *request.clipboard, cancelled, [decoded, &error](qint64 bytes) {
                            return decoded->reservation->resize(bytes, &error);
                        });
                } else if (request.record) {
                    const QSize size =
                        request.record->result ? request.record->result->imageSize : QSize();
                    const qint64 bytes =
                        size.isEmpty() ? 0 : imageStitchingImageImportBytes(size, &error);
                    if (bytes == 0 || !decoded->reservation->resize(bytes, &error))
                        return ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::Source,
                            error.isEmpty()
                                ? imageStitchingText("The final screenshot is unavailable")
                                : error);
                    const auto result = historyReader
                                            ? historyReader(*request.record)
                                            : storage::ApplicationStorage::instance()
                                                  .captureHistory()
                                                  .loadResultImagePreservingRecord(*request.record);
                    if (result)
                        image = *result;
                } else if (!request.pinnedId.isEmpty()) {
                    auto source = readImageImportPinnedRecord(
                        request.pinnedId, cancelled,
                        [decoded, &error](qint64 bytes) {
                            return decoded->reservation->resize(bytes, &error);
                        },
                        preparation);
                    image = std::move(source.image);
                    if (!source.error.isEmpty())
                        error = std::move(source.error);
                } else {
                    image = readImageImportFile(
                        request.path, request.file, cancelled, [decoded, &error](qint64 bytes) {
                            return decoded->reservation->resize(bytes, &error);
                        });
                }
                if (cancelled())
                    return ScreenshotExportTaskResult::failure(
                        ScreenshotExportFailureStage::Cancelled, {});
                if (image.isNull())
                    return ScreenshotExportTaskResult::failure(
                        ScreenshotExportFailureStage::Source,
                        error.isEmpty()
                            ? imageStitchingText(
                                  "The image could not be decoded or is no longer available")
                            : error);
                decoded->preview =
                    image.width() <= 256 && image.height() <= 256
                        ? image
                        : snowCanvasScaleImage(image,
                                               image.size().scaled(256, 256, Qt::KeepAspectRatio),
                                               Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                ScreenshotExportTaskResult result;
                result.image = std::move(image);
                return result;
            },
            [this, request, decoded, token](ScreenshotExportTaskResult result) {
                finishImport(request, decoded, token, std::move(result));
            });
        if (!job.isValid()) {
            failures.append(imageStitchingText("%1: %2").arg(
                request.label,
                imageStitchingText("Image loading could not be started. Try again")));
            discardRemainingSplit(request);
            QTimer::singleShot(0, &q, [this, token] {
                if (token == generation)
                    nextImport();
            });
        }
    }

    void publishCopy(quint64 token) {
        const storage::ScreenshotSettings settings;
        const QPointer<ImageStitchingWindow> guard(&q);
        const auto complete = [guard, token](bool success, const QString& error) {
            if (guard && !guard->m_impl->closing && token == guard->m_impl->generation)
                guard->m_impl->finishExport(success, error);
        };
        if (services.copy) {
            services.copy(artifact, &q, editor->window(), complete);
            return;
        }
        const auto committed = [guard, token](ScreenshotClipboardCommitResult result) {
            if (guard && token == guard->m_impl->generation)
                guard->m_impl->finishExport(result.succeeded(),
                                            result.succeeded() ? QString() : result.errorString());
        };
        if (settings.copyImageFileToClipboard()) {
            const auto encoding = screenshotEncodingOptions(settings);
            if (!artifact->requestClipboardFile(
                    &q, ScreenshotImageFileService::formatForKey(settings.imageFormat()),
                    settings.manualSaveFilenameFormat(), encoding,
                    [this, token, committed](ScreenshotExportClipboardResult prepared) {
                        if (closing || token != generation)
                            return;
                        if (!prepared.succeeded()) {
                            finishExport(false, prepared.error);
                            return;
                        }
                        static_cast<void>(clipboard.commit(QApplication::clipboard(), &q,
                                                           std::move(prepared.payload), committed));
                    },
                    ScreenshotPdfOptions{screenshot_pdf::pageSizeForKey(settings.pdfPageSize())}))
                finishExport(false, imageStitchingText("Copying could not be started. Try again"));
        } else if (!artifact->requestClipboard(&q, [this, token, committed](
                                                       ScreenshotExportClipboardResult result) {
                       if (closing || token != generation)
                           return;
                       if (!result.succeeded()) {
                           finishExport(false, result.error);
                           return;
                       }
                       static_cast<void>(clipboard.commit(QApplication::clipboard(), &q,
                                                          std::move(result.payload), committed));
                   })) {
            finishExport(false, imageStitchingText("Copying could not be started. Try again"));
        }
    }

    void publishSave(quint64 token) {
        const QPointer<ImageStitchingWindow> guard(&q);
        auto complete = [guard, token](bool success, const QString& error) {
            if (guard && token == guard->m_impl->generation)
                guard->m_impl->finishExport(success, error);
        };
        if (services.save) {
            services.save(artifact, &q, editor->window(), std::move(complete));
            return;
        }
        const storage::ScreenshotSettings settings;
        if (settings.saveAsFileDialog() == QStringLiteral("snow_shot")) {
            if (!ScreenshotSaveAsFileDialog::open(
                    &q, editor->window(), artifact, {},
                    [complete](bool success) { complete(success, {}); }, nullptr, Qt::WindowModal))
                complete(false, imageStitchingText("Saving could not be started. Try again"));
            return;
        }
        nativeSave = ScreenshotManualSaveService::open(
            &q, editor->window(), artifact,
            [complete](const ScreenshotExportTaskResult& result) {
                complete(result.succeeded(), result.error);
            },
            Qt::WindowModal,
            [guard, token] {
                if (guard && token == guard->m_impl->generation)
                    guard->m_impl->showStatus(Status::Saving);
            });
        if (!nativeSave.isValid())
            complete(false, imageStitchingText("Saving could not be started. Try again"));
    }

    void finishPdfExport(ImageStitchingPdfExportResult result) {
        if (closing || !pdfExporting)
            return;
        pdfProgressTimer->stop();
        job = {};
        choosingPdf = false;
        pdfCancelRequested = false;
        pdfCancellation.reset();
        pdfProgress.reset();
        lastPdfResult = std::move(result);
        const bool success = lastPdfResult.succeeded();
        showStatus(Status::None);
        const QPointer<ImageStitchingWindow> guard(&q);
        const quint64 token = generation;
        emit q.pdfExportFinished(lastPdfResult);
        if (!guard || token != generation)
            return;
        emit q.exportFinished(success);
        if (!guard || token != generation)
            return;
        pdfExporting = false;
        exporting = false;
        closing = success;
        updateActions();
        if (success)
            modal->accept();
        else
            showStatus(Status::PdfPartial);
    }

    void requestPdfCancellation() {
        if (!pdfExporting || pdfCancelRequested)
            return;
        pdfCancelRequested = true;
        if (pdfCancellation)
            pdfCancellation->store(true);
        if (choosingPdf) {
            // A chooser port may reply later; choosingPdf is cleared by the terminal completion.
            ImageStitchingPdfExportResult result;
            result.totalCount =
                imageStitchingPdfPageCount(document->items().size(), document->pdfSettings());
            result.cancelled = true;
            if (fileDialog)
                fileDialog->reject();
            finishPdfExport(std::move(result));
            return;
        }
        job.cancel();
        showStatus(Status::PdfCancelling);
        updateActions();
    }

    void writePdf(QVector<ImageStitchingItem> items, ImageStitchingPdfSettings settings,
                  ImageStitchingPdfDestination destination, quint64 token) {
        choosingPdf = false;
        auto result = std::make_shared<ImageStitchingPdfExportResult>();
        result->totalCount = imageStitchingPdfPageCount(items.size(), settings);
        const auto cancellation = pdfCancellation;
        const auto progress = pdfProgress;
        const auto writer = services.exportPdf;
        showStatus(Status::PdfSaving);
        pdfProgressTimer->start();
        job = ScreenshotExportCoordinator::shared().submit(
            &q, ScreenshotExportCoordinator::Priority::Foreground,
            [items = std::move(items), settings, destination = std::move(destination), result,
             cancellation, progress, writer](const ScreenshotExportCancellation& jobCancellation) {
                const auto cancelled = [cancellation, jobCancellation] {
                    return cancellation->load() || jobCancellation.isCancellationRequested();
                };
                const auto report = [progress](const ImageStitchingPdfExportProgress& value) {
                    progress->completedPages.store(value.completedPages);
                    progress->totalPages.store(value.totalPages);
                    progress->pageNumber.store(value.pageNumber);
                    progress->pagePercent.store(
                        qRound(std::clamp(value.pageFraction, 0.0, 1.0) * 100));
                };
                *result = writer ? writer(items, settings, destination, cancelled, report)
                                 : exportImageStitchingPdf(items, settings, destination, cancelled,
                                                           report);
                return ScreenshotExportTaskResult{};
            },
            [this, result, token](const ScreenshotExportTaskResult& task) {
                if (closing || token != generation || !pdfExporting)
                    return;
                if (!task.succeeded()) {
                    result->cancelled =
                        task.failureStage == ScreenshotExportFailureStage::Cancelled;
                    if (result->error.isEmpty() && !result->cancelled)
                        result->error =
                            task.error.isEmpty()
                                ? imageStitchingText("The PDFs could not be exported. Try again")
                                : task.error;
                }
                finishPdfExport(std::move(*result));
            });
        if (!job.isValid()) {
            result->error = imageStitchingText("Export could not be started. Try again");
            finishPdfExport(std::move(*result));
        }
    }

    void startPdfExport() {
        if (closing || exporting || importing || confirmationPending || fileDialog ||
            document->items().isEmpty() ||
            !validateImageStitchingPdfSettings(document->pdfSettings()).isEmpty())
            return;
        pdfExporting = true;
        exporting = true;
        copying = false;
        choosingPdf = true;
        pdfCancelRequested = false;
        pdfCancellation = std::make_shared<std::atomic_bool>(false);
        pdfProgress = std::make_shared<PdfProgressState>();
        const auto items = document->items();
        const auto settings = document->pdfSettings();
        pdfProgress->totalPages.store(imageStitchingPdfPageCount(items.size(), settings));
        const auto token = ++generation;
        updateActions();
        const QPointer<ImageStitchingWindow> guard(&q);
        auto chosen = [guard, items, settings,
                       token](std::optional<ImageStitchingPdfDestination> choice) {
            if (!guard)
                return;
            auto& d = *guard->m_impl;
            if (d.closing || token != d.generation || !d.pdfExporting || !d.choosingPdf)
                return;
            if (!choice || choice->path.isEmpty()) {
                ImageStitchingPdfExportResult result;
                result.totalCount = imageStitchingPdfPageCount(items.size(), settings);
                result.cancelled = true;
                d.finishPdfExport(std::move(result));
                return;
            }
            d.writePdf(items, settings, std::move(*choice), token);
        };
        if (services.choosePdfDestination) {
            services.choosePdfDestination(editor->window(), settings.exportMode, std::move(chosen));
            return;
        }
        auto* dialog = new QFileDialog(editor->window());
        fileDialog = dialog;
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowModality(Qt::WindowModal);
        const bool combined = settings.exportMode == ImageStitchingPdfExportMode::Combined;
        dialog->setWindowTitle(combined ? imageStitchingText("Save PDF document")
                                        : imageStitchingText("Select destination folder"));
        dialog->setDirectory(storage::ScreenshotSettings().lastManualSaveDirectory());
        if (combined) {
            dialog->setAcceptMode(QFileDialog::AcceptSave);
            dialog->setFileMode(QFileDialog::AnyFile);
            dialog->setNameFilter(
                QStringLiteral("%1 (*.pdf)").arg(imageStitchingText("PDF documents")));
            dialog->setDefaultSuffix(QStringLiteral("pdf"));
            dialog->setOption(QFileDialog::DontConfirmOverwrite, false);
            dialog->selectFile(QStringLiteral("SnowShot_%1.pdf")
                                   .arg(QDateTime::currentDateTime().toString(
                                       QStringLiteral("yyyy-MM-dd_HH-mm-ss"))));
        } else {
            dialog->setFileMode(QFileDialog::Directory);
            dialog->setOption(QFileDialog::ShowDirsOnly, true);
        }
        QObject::connect(
            dialog, &QDialog::finished, &q,
            [dialog, chosen = std::move(chosen), combined](int code) mutable {
                const auto files = dialog->selectedFiles();
                if (code != QDialog::Accepted || files.isEmpty())
                    chosen(std::nullopt);
                else {
                    QString path = files.first();
                    if (combined && QFileInfo(path).suffix().compare(QStringLiteral("pdf"),
                                                                     Qt::CaseInsensitive) != 0)
                        path += QStringLiteral(".pdf");
                    // Qt confirmed the exact typed path. A suffix adjustment never
                    // authorizes replacing a different destination.
                    chosen(ImageStitchingPdfDestination{path, combined && path == files.first() &&
                                                                  QFileInfo::exists(path)});
                }
            });
        dialog->open();
    }

    void startExport(bool copy) {
        if (document->outputType() == ImageStitchingOutputType::PdfDocument) {
            if (!copy)
                startPdfExport();
            return;
        }
        if (closing || exporting || importing || confirmationPending ||
            !document->layout().isValid())
            return;
        copying = copy;
        exporting = true;
        const quint64 token = ++generation;
        updateActions();
        showStatus(Status::Preparing);
        if (copy && !services.copy && !storage::ScreenshotSettings().copyImageFileToClipboard()) {
            const auto capacity = estimateImageStitchingClipboard(document->layout().size);
            if (!capacity.isValid()) {
                finishExport(false, capacity.error);
                return;
            }
        }
        const auto items = document->items();
        const auto settings = document->settings();
        auto rows = std::make_shared<ScreenshotImageRowSource>();
        job = ScreenshotExportCoordinator::shared().submit(
            &q, ScreenshotExportCoordinator::Priority::Foreground,
            [items, settings, rows](const ScreenshotExportCancellation& cancellation) {
                QString error;
                *rows = prepareImageStitchingRows(
                    items, settings,
                    [cancellation] { return cancellation.isCancellationRequested(); }, &error);
                return rows->isValid() ? ScreenshotExportTaskResult{}
                                       : ScreenshotExportTaskResult::failure(
                                             ScreenshotExportFailureStage::Render, error);
            },
            [this, rows, token, copy](const ScreenshotExportTaskResult& result) {
                if (closing || token != generation)
                    return;
                job = {};
                if (!result.succeeded()) {
                    finishExport(false, result.error);
                    return;
                }
                if (copy && !storage::ScreenshotSettings().copyImageFileToClipboard() &&
                    !services.copy) {
                    QString error;
                    clipboardReservation = reserveImageStitchingClipboard(rows->size, &error);
                    if (!clipboardReservation) {
                        finishExport(false, error);
                        return;
                    }
                }
                artifact =
                    std::make_shared<ScreenshotExportArtifact>(ScreenshotExportSource::fromProducer(
                        {}, [rows](std::function<bool()> cancelled) {
                            auto result = *rows;
                            result.cancellationRequested = std::move(cancelled);
                            return result;
                        }));
                showStatus(copy ? Status::Copying : Status::Saving);
                if (copy)
                    publishCopy(token);
                else
                    publishSave(token);
            });
        if (!job.isValid())
            finishExport(false, imageStitchingText("Export could not be started. Try again"));
    }
};

ImageStitchingWindow::ImageStitchingWindow(QObject* parent, ImageStitchingWindowServices services)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, std::move(services))) {}
ImageStitchingWindow::~ImageStitchingWindow() {
    shutdown();
}

void ImageStitchingWindow::present(const QVector<storage::CaptureHistoryRecord>& records) {
    if (m_impl->closing || m_impl->exporting)
        return;
    const bool firstPresentation = !m_impl->modal->isOpen();
    if (firstPresentation) {
        QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        m_impl->modal->setWindowScreen(screen);
        const QSize available = screen->availableGeometry().size() - QSize(40, 40);
        m_impl->modal->setWindowPreferredSize(QSize(1200, 800).boundedTo(available));
        m_impl->modal->setWindowMinimumSize(QSize(900, 600).boundedTo(available));
    }
    m_impl->modal->present();
    m_impl->pasteShortcuts->addScopeWindow(m_impl->editor->window());
    if (firstPresentation)
        installWindowCloseShortcut(m_impl->editor->window(), [this] { requestClose(); });
    QVector<ImportItem> requested;
    for (const auto& record : records)
        requested.append({record, {}, {}, imageStitchingText("Screenshot %1").arg(record.id), 0});
    m_impl->beginImport(std::move(requested));
}

void ImageStitchingWindow::presentPinnedRecords(const QStringList& recordIds) {
    present();
    QVector<ImportItem> requested;
    for (const QString& id : recordIds)
        requested.append({{}, id, {}, imageStitchingText("Pinned window %1").arg(id), 0});
    m_impl->beginImport(std::move(requested));
}

void ImageStitchingWindow::presentPinnedWindows(
    const QVector<ScreenshotPinnedImageSnapshot>& snapshots) {
    present();
    QVector<ImportItem> requested;
    requested.reserve(snapshots.size());
    for (const auto& snapshot : snapshots) {
        ImportItem item;
        item.label = imageStitchingText("Pinned window %1").arg(snapshot.label);
        item.live = snapshot;
        requested.append(std::move(item));
    }
    m_impl->beginImport(std::move(requested));
}

bool ImageStitchingWindow::presentSplitImagesAsPdf(ImageSplitSnapshot snapshot) {
    if (m_impl->closing || m_impl->exporting || m_impl->confirmationPending ||
        snapshot.image.isNull() || snapshot.layout.size != snapshot.image.size() ||
        !snapshot.layout.isValid() || snapshot.layout.outputCount() <= 0)
        return false;
    ImportItem request;
    request.split = std::make_shared<const ImageSplitSnapshot>(std::move(snapshot));
    m_impl->document->setOutputType(ImageStitchingOutputType::PdfDocument);
    present();
    m_impl->beginImport({std::move(request)});
    return true;
}

void ImageStitchingWindow::addFiles(const QStringList& paths) {
    QVector<ImportItem> requested;
    for (const QString& path : paths)
        requested.append({{}, {}, path, path, 0});
    m_impl->beginImport(std::move(requested));
}

void ImageStitchingWindow::paste() {
    auto& d = *m_impl;
    if (!d.canPaste())
        return;
    auto* clipboard = QApplication::clipboard();
    const auto paths = ScreenshotClipboardContentReader::localImageFilePaths(clipboard->mimeData());
    if (!paths.isEmpty()) {
        addFiles(paths);
        return;
    }
    QString error;
    auto reservation = reserveImageStitchingImport(0, &error);
    bool rejected = !reservation;
    auto snapshot = ScreenshotClipboardContentReader::snapshotImage(
        clipboard, editor()->devicePixelRatioF(), [&](qint64 bytes) {
            rejected = rejected || !reservation || !reservation->resize(bytes, &error);
            return !rejected;
        });
    if (rejected) {
        d.externalStatus(error);
        emit importFinished({error});
        return;
    }
    if (!snapshot)
        return;
    ImportItem item;
    item.label = imageStitchingText("Clipboard image");
    item.clipboard = std::move(snapshot);
    item.reservation = std::move(reservation);
    d.beginImport({std::move(item)});
}

void ImageStitchingWindow::addSelectedFiles(std::shared_ptr<platform::SelectedFileBackend> backend,
                                            platform::SelectedFileTarget target) {
    if (!backend)
        return;
    ImportItem request;
    request.selection = SelectedFilesImport{std::move(backend), target};
    m_impl->beginImport({std::move(request)});
}
void ImageStitchingWindow::replaceFile(quint64 id, const QString& path) {
    m_impl->beginImport({ImportItem{{}, {}, path, path, id}});
}
void ImageStitchingWindow::requestClose() {
    if (m_impl->closing)
        return;
    if (m_impl->pdfExporting) {
        m_impl->requestPdfCancellation();
        return;
    }
    const auto discard = [this] {
        m_impl->closing = true;
        m_impl->cancelWork();
        m_impl->modal->reject();
    };
    if (m_impl->document->items().isEmpty())
        discard();
    else
        m_impl->askConfirmation(false, discard);
}
void ImageStitchingWindow::requestClear() {
    if (m_impl->exporting || m_impl->document->items().isEmpty())
        return;
    m_impl->askConfirmation(true, [this] {
        if (m_impl->exporting)
            return;
        m_impl->cancelWork();
        m_impl->document->clear();
        m_impl->failures.clear();
        m_impl->selectionFailures.clear();
        m_impl->showStatus(Impl::Status::None);
        m_impl->updateActions();
    });
}
void ImageStitchingWindow::copy() {
    m_impl->startExport(true);
}
void ImageStitchingWindow::save() {
    m_impl->startExport(false);
}
void ImageStitchingWindow::shutdown() {
    if (!m_impl || m_impl->closing)
        return;
    m_impl->closing = true;
    m_impl->cancelWork();
    m_impl->modal->reject();
}
bool ImageStitchingWindow::busy() const {
    return m_impl->exporting;
}
bool ImageStitchingWindow::loading() const {
    return m_impl->importing;
}
ImageStitchingDocument* ImageStitchingWindow::document() const {
    return m_impl->document;
}
components::ImageStitchingEditorWidget* ImageStitchingWindow::editor() const {
    return m_impl->editor;
}
AdModal* ImageStitchingWindow::modal() const {
    return m_impl->modal;
}

} // namespace snow_shot::presentation
