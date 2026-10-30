// SPDX-License-Identifier: GPL-3.0-or-later
#include "imageeditingexportcontroller.h"
#include "snow_shot/presentation/screenshotclouduploadservice.h"
#include "snow_shot/presentation/screenshotmanualsave.h"
#include "snow_shot/presentation/screenshotprintservice.h"
#include "snow_shot/presentation/screenshotsaveasfiledialog.h"
#include <QApplication>
#include <QAbstractButton>
#include <QClipboard>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QSaveFile>
#include <utility>

namespace snow_shot::presentation {
namespace {
using Operation = ImageEditingExportController::Operation;
struct Request final : QObject {
    explicit Request(QObject* parent, ImageEditingExportController::Completion done)
        : QObject(parent), completion(std::move(done)) {}
    ImageEditingExportController::Completion completion;
    ScreenshotExportJobHandle job;
    ScreenshotManualSaveHandle save;
    ScreenshotClipboardCommitHandle clipboard;
    std::shared_ptr<ScreenshotExportArtifact> artifact;
    QPointer<ScreenshotCloudUploadJob> upload;
    QPointer<QFileDialog> dialog;
    QPointer<QMessageBox> overwrite;
    QPointer<QWidget> owner;
    bool destinationChosen = false;
};
struct Destination {
    QString directory;
    QString name;
};
Destination destination(Operation operation) {
    const storage::ScreenshotSettings settings;
    const auto timestamp = QDateTime::currentDateTime();
    const bool quick = operation == Operation::QuickSave;
    return {
        quick ? storage::expandScreenshotExportVariables(settings.imageSaveDirectory(), timestamp)
              : ScreenshotImageFileService::saveDialogDirectory(settings.lastManualSaveDirectory(),
                                                                settings.imageSaveDirectory()),
        ScreenshotImageFileService::suggestedBaseName(quick ? settings.autoSaveFilenameFormat()
                                                            : settings.manualSaveFilenameFormat(),
                                                      timestamp)};
}
ScreenshotExportTaskResult saveText(const QByteArray& bytes, const QString& path,
                                    const Destination& output, Operation operation,
                                    const ScreenshotExportCancellation& cancel) {
    if (cancel.isCancellationRequested())
        return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::Cancelled, {});
    if (operation == Operation::QuickSave) {
        if (output.directory.trimmed().isEmpty() || !QDir().mkpath(output.directory))
            return ScreenshotExportTaskResult::failure(
                ScreenshotExportFailureStage::File,
                QCoreApplication::translate("ImageEditingWindow",
                                            "The save directory is not available."));
        for (int index = 0; index < 1000000; ++index) {
            if (cancel.isCancellationRequested())
                return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::Cancelled,
                                                           {});
            const QString suffix =
                (index ? QStringLiteral("_%1").arg(index) : QString{}) + QStringLiteral(".txt");
            QFile file(QDir(output.directory)
                           .filePath(storage::composeExportFileName(output.name, suffix)));
            if (QFileInfo::exists(file.fileName()))
                continue;
            if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::File,
                                                           file.errorString());
            if (file.write(bytes) != bytes.size() || !file.flush()) {
                const auto error = file.errorString();
                file.remove();
                return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::File,
                                                           error);
            }
            return {};
        }
        return ScreenshotExportTaskResult::failure(
            ScreenshotExportFailureStage::File,
            QCoreApplication::translate("ImageEditingWindow",
                                        "No available output filename was found."));
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::File,
                                                   file.errorString());
    return {};
}
} // namespace

struct ImageEditingExportController::Impl {
    ImageEditingExportController& q;
    ScreenshotPrintService* printing;
    Pin pin;
    QPointer<Request> active;

    void finish(QPointer<Request> request, bool success, QString error = {}) {
        if (!request || active != request)
            return;
        active = nullptr;
        auto completion = std::move(request->completion);
        // Keep artifacts alive through their delivery callbacks.
        request->deleteLater();
        if (completion)
            completion(success, std::move(error));
    }
    void submit(QPointer<Request> request, ScreenshotExportCoordinator::Work work,
                Operation operation, QString path) {
        request->job = ScreenshotExportCoordinator::shared().submit(
            request, ScreenshotExportCoordinator::Priority::Foreground, std::move(work),
            [this, request, operation, path = std::move(path)](auto result) {
                if (!request || active != request)
                    return;
                if (result.succeeded() && operation == Operation::Save)
                    static_cast<void>(storage::ScreenshotSettings().setLastManualSaveDirectory(
                        QFileInfo(path).absolutePath()));
                finish(request, result.succeeded(), result.error);
            });
        if (!request->job.isValid())
            finish(request, false,
                   QCoreApplication::translate("snow_shot::presentation::ImageEditingWindow",
                                               "Export could not be started."));
    }
    void confirmOverwrite(QPointer<Request> request, const QStringList& paths,
                          std::function<void()> write) {
        QStringList existing;
        for (const auto& path : paths)
            if (QFileInfo::exists(path))
                existing.append(path);
        if (existing.isEmpty()) {
            write();
            return;
        }
        auto* dialog =
            new QMessageBox(QMessageBox::Question,
                            QCoreApplication::translate("ScreenshotRecognitionFileExport",
                                                        "Replace existing files?"),
                            QCoreApplication::translate("ScreenshotRecognitionFileExport",
                                                        "Replace the existing file(s)?\n%1")
                                .arg(existing.join(QLatin1Char('\n'))),
                            QMessageBox::Yes | QMessageBox::No, request->owner);
        request->overwrite = dialog;
        dialog->setObjectName(QStringLiteral("imageEditingRecognitionOverwriteDialog"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setOption(QMessageBox::Option::DontUseNativeDialog);
        dialog->setWindowModality(Qt::WindowModal);
        dialog->button(QMessageBox::Yes)
            ->setText(QCoreApplication::translate("ScreenshotRecognitionFileExport", "Replace"));
        dialog->button(QMessageBox::No)
            ->setText(QCoreApplication::translate("ScreenshotRecognitionFileExport", "Cancel"));
        dialog->setDefaultButton(QMessageBox::No);
        QObject::connect(dialog, &QDialog::finished, request,
                         [this, request, write = std::move(write)](int result) {
                             if (!request || active != request)
                                 return;
                             request->overwrite.clear();
                             if (result == QMessageBox::Yes)
                                 write();
                             else
                                 finish(request, false);
                         });
        QObject::connect(dialog, &QObject::destroyed, request, [this, request] {
            if (request && !request->job.isValid())
                finish(request, false);
        });
        dialog->show();
    }
    void saveRecognition(QPointer<Request> request, Operation operation, Content content,
                         QWidget* owner) {
        const auto output = destination(operation);
        const bool text = std::holds_alternative<QString>(content);
        QString extension = QStringLiteral("txt");
        QString filter = QCoreApplication::translate("snow_shot::presentation::ImageEditingWindow",
                                                     "Text files (*.txt)");
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION ||                     \
    SNOW_SHOT_ENABLE_QR_RECOGNITION || SNOW_SHOT_ENABLE_TABLE_RECOGNITION
        if (!text) {
            const auto kind = std::get<ScreenshotRecognitionFileSnapshot>(content).kind;
            extension = ScreenshotRecognitionFileExport::extension(kind);
            filter = ScreenshotRecognitionFileExport::dialogFilter(kind);
        }
#endif
        const auto write = [this, request, operation, output,
                            content = std::make_shared<Content>(std::move(content))](QString path) {
            if (!request || active != request)
                return;
            if (const auto* value = std::get_if<QString>(content.get())) {
                submit(
                    request,
                    [bytes = value->toUtf8(), path, output, operation](const auto& cancel) {
                        return saveText(bytes, path, output, operation, cancel);
                    },
                    operation, path);
                return;
            }
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION ||                     \
    SNOW_SHOT_ENABLE_QR_RECOGNITION || SNOW_SHOT_ENABLE_TABLE_RECOGNITION
            auto snapshot = std::get<ScreenshotRecognitionFileSnapshot>(std::move(*content));
            if (operation == Operation::Save)
                path = ScreenshotRecognitionFileExport::normalizedPath(path, snapshot.kind);
            const auto paths = ScreenshotRecognitionFileExport::outputPaths(path, snapshot.kind);
            auto submitSnapshot = [this, request, snapshot = std::move(snapshot), output, path,
                                   operation]() mutable {
                if (!request || active != request)
                    return;
                submit(
                    request,
                    [snapshot = std::move(snapshot), output, path, operation](const auto& cancel) {
                        if (cancel.isCancellationRequested())
                            return ScreenshotExportTaskResult::failure(
                                ScreenshotExportFailureStage::Cancelled, {});
                        const auto result =
                            operation == Operation::QuickSave
                                ? ScreenshotRecognitionFileExport::quickSave(
                                      snapshot, output.directory, output.name)
                                : ScreenshotRecognitionFileExport::saveToPath(snapshot, path, true);
                        return result.succeeded()
                                   ? ScreenshotExportTaskResult{}
                                   : ScreenshotExportTaskResult::failure(
                                         ScreenshotExportFailureStage::File, result.error);
                    },
                    operation, path);
            };
            if (operation == Operation::Save)
                confirmOverwrite(request, paths, std::move(submitSnapshot));
            else
                submitSnapshot();
#endif
        };
        if (operation == Operation::QuickSave) {
            write({});
            return;
        }
        auto* dialog = new QFileDialog(
            owner, QCoreApplication::translate("snow_shot::presentation::ImageEditingWindow",
                                               "Save as file"));
        request->dialog = dialog;
        dialog->setObjectName(QStringLiteral("imageEditingRecognitionSaveDialog"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setAcceptMode(QFileDialog::AcceptSave);
        dialog->setFileMode(QFileDialog::AnyFile);
        dialog->setWindowModality(Qt::WindowModal);
        dialog->setNameFilter(filter);
        dialog->setOption(QFileDialog::DontConfirmOverwrite, !text);
        dialog->selectFile(
            QDir(output.directory).filePath(output.name + QLatin1Char('.') + extension));
        QObject::connect(dialog, &QDialog::accepted, request, [this, request, dialog, write] {
            if (!request || request->destinationChosen)
                return;
            request->destinationChosen = true;
            const auto paths = dialog->selectedFiles();
            if (paths.isEmpty()) {
                finish(request, false);
                return;
            }
            // Leave QFileDialog::done() before opening another dialog or invoking client code.
            QMetaObject::invokeMethod(
                request, [write, path = paths.first()] { write(path); }, Qt::QueuedConnection);
        });
        QObject::connect(dialog, &QDialog::rejected, request,
                         [this, request] { finish(request, false); });
        QObject::connect(dialog, &QObject::destroyed, request, [this, request] {
            if (request && !request->destinationChosen)
                finish(request, false);
        });
        dialog->show();
    }
    void image(QPointer<Request> request, Operation operation, ScreenshotExportSource source,
               QWidget* owner, QWidget* stackingOwner) {
        request->artifact = std::make_shared<ScreenshotExportArtifact>(std::move(source));
        const auto artifact = request->artifact;
        const auto done = [this, request](auto result) {
            finish(request, result.succeeded(), result.error);
        };
        switch (operation) {
        case Operation::Pin: {
            const QPointer<ImageEditingExportController> lifetime(&q);
            if (!artifact->requestImage(request, [lifetime, request](auto result) {
                    if (!lifetime || !request || lifetime->m_impl->active != request)
                        return;
                    [[maybe_unused]] const auto rendering = request->artifact;
                    if (!result.succeeded()) {
                        lifetime->m_impl->finish(request, false, result.error);
                        return;
                    }
                    const auto finish = [lifetime, request](bool success) {
                        if (lifetime && request)
                            lifetime->m_impl->finish(
                                request, success,
                                success ? QString{}
                                        : QCoreApplication::translate(
                                              "snow_shot::presentation::ImageEditingWindow",
                                              "Export could not be started."));
                    };
                    // Keep the callback alive if a synchronous completion destroys the editor.
                    const auto handler = lifetime->m_impl->pin;
                    if (!handler || !handler(std::move(result.image), finish))
                        finish(false);
                })) {
                if (lifetime)
                    lifetime->m_impl->finish(
                        request, false,
                        QCoreApplication::translate("snow_shot::presentation::ImageEditingWindow",
                                                    "Export could not be started."));
            }
            break;
        }
        case Operation::Copy:
            if (!artifact->requestClipboard(request, [this, request](auto result) {
                    if (!request || active != request)
                        return;
                    if (!result.succeeded()) {
                        finish(request, false, result.error);
                        return;
                    }
                    request->clipboard = ScreenshotClipboardService::commit(
                        QApplication::clipboard(), request, std::move(result.payload),
                        [this, request](auto committed) {
                            finish(request, committed.succeeded(), committed.errorString());
                        });
                }))
                finish(request, false,
                       QCoreApplication::translate("snow_shot::presentation::ImageEditingWindow",
                                                   "Export could not be started."));
            break;
        case Operation::Save:
            if (storage::ScreenshotSettings().saveAsFileDialog() == QStringLiteral("snow_shot")) {
                if (!ScreenshotSaveAsFileDialog::open(
                        request, owner, artifact, {},
                        [this, request](bool saved) { finish(request, saved); }, stackingOwner,
                        Qt::WindowModal))
                    finish(
                        request, false,
                        QCoreApplication::translate("snow_shot::presentation::ImageEditingWindow",
                                                    "Export could not be started."));
            } else {
                const auto save = ScreenshotManualSaveService::open(request, owner, artifact, done);
                if (!request || active != request)
                    return;
                request->save = save;
                if (!save.isValid())
                    finish(
                        request, false,
                        QCoreApplication::translate("snow_shot::presentation::ImageEditingWindow",
                                                    "Export could not be started."));
            }
            break;
        case Operation::QuickSave:
            if (!artifact->requestQuickSave(request, done))
                finish(request, false,
                       QCoreApplication::translate("snow_shot::presentation::ImageEditingWindow",
                                                   "Export could not be started."));
            break;
        case Operation::Print:
            if (!artifact->requestImage(request, [this, request,
                                                  window = QPointer<QWidget>(owner)](auto result) {
                    if (!request || active != request)
                        return;
                    if (!result.succeeded()) {
                        finish(request, false, result.error);
                        return;
                    }
                    auto& service = printing ? *printing : ScreenshotPrintService::shared();
                    if (!service.printImage(
                            request, window, std::move(result.image),
                            [this, request](auto printed) {
                                finish(request,
                                       printed.status == ScreenshotPrintService::Status::Submitted,
                                       printed.error);
                            }))
                        finish(request, false,
                               QCoreApplication::translate(
                                   "snow_shot::presentation::ImageEditingWindow",
                                   "Printing is unavailable."));
                }))
                finish(request, false,
                       QCoreApplication::translate("snow_shot::presentation::ImageEditingWindow",
                                                   "Export could not be started."));
            break;
        case Operation::Upload: {
            const auto config = ScreenshotCloudUploadService::configuration();
            if (!config) {
                finish(request, false,
                       QCoreApplication::translate(
                           "snow_shot::presentation::ImageEditingWindow",
                           "Configure cloud upload before uploading an image."));
                break;
            }
            request->upload = ScreenshotCloudUploadService::upload(
                artifact, *config, ScreenshotCloudUploadService::currentOptions(), request,
                [this, request](auto result) {
                    if (!request || active != request)
                        return;
                    const QPointer<ImageEditingExportController> lifetime(&q);
                    const bool success =
                        result.status == ScreenshotCloudUploadResult::Status::Succeeded;
                    if (success)
                        QApplication::clipboard()->setText(result.url.toString());
                    if (lifetime)
                        finish(request, success, result.error);
                });
            break;
        }
        }
    }
};

ImageEditingExportController::ImageEditingExportController(ScreenshotPrintService* printing,
                                                           QObject* parent, Pin pin)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, printing, std::move(pin))) {}
ImageEditingExportController::~ImageEditingExportController() {
    cancel();
}
bool ImageEditingExportController::busy() const {
    return m_impl->active;
}
void ImageEditingExportController::start(Operation operation, Content content, QWidget* owner,
                                         QWidget* stackingOwner, Completion completion) {
    if (busy() || !owner)
        return;
    auto& d = *m_impl;
    auto* request = new Request(this, std::move(completion));
    d.active = request;
    request->owner = owner;
    QObject::connect(owner, &QObject::destroyed, request,
                     [this, pending = QPointer<Request>(request)] {
                         if (pending && m_impl->active == pending)
                             cancel();
                     });
    if (auto* mime = std::get_if<std::unique_ptr<QMimeData>>(&content)) {
        const QPointer<ImageEditingExportController> lifetime(this);
        const QPointer<Request> pending(request);
        QApplication::clipboard()->setMimeData(mime->release());
        if (lifetime)
            d.finish(pending, true);
    } else if (auto* source = std::get_if<ScreenshotExportSource>(&content)) {
        d.image(request, operation, std::move(*source), owner, stackingOwner);
    } else {
        d.saveRecognition(request, operation, std::move(content), owner);
    }
}
void ImageEditingExportController::cancel() {
    const auto request = std::exchange(m_impl->active, {});
    if (!request)
        return;
    [[maybe_unused]] const auto completion = std::move(request->completion);
    request->job.cancel();
    request->save.cancel();
    if (!request)
        return;
    request->clipboard.cancel();
    if (request->artifact)
        request->artifact->cancel();
    if (request->upload)
        request->upload->cancel();
    if (!request)
        return;
    const auto dismiss = [request](QPointer<QDialog> dialog) {
        if (!dialog)
            return;
        if (request)
            QObject::disconnect(dialog, nullptr, request, nullptr);
        if (dialog->isVisible())
            dialog->reject();
        if (dialog)
            dialog->deleteLater();
    };
    const QPointer<QDialog> overwrite = request->overwrite;
    const QPointer<QDialog> dialog = request->dialog;
    dismiss(overwrite);
    dismiss(dialog);
    if (request)
        delete request;
}
} // namespace snow_shot::presentation
