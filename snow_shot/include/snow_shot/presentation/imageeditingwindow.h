// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "snow_shot/presentation/imageeditingdocument.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/platform/selectedfiles.h"
#include <QPointer>

namespace adqt::widgets {
class AdModal;
}
class ScreenshotToolPalette;
class ScreenshotPrintService;
class ScreenshotRecognitionSessionController;
namespace snow_shot::presentation {
class ImageEditingCanvas;
class PinnedWindowGroupManager;
struct ImageEditingServices {
    std::function<ScreenshotPinnedRecognitionProviders()> providers;
    PinnedWindowGroupManager* pins = nullptr;
    ScreenshotPrintService* printing = nullptr;
    std::function<bool(QImage, std::function<void(bool)>)> pin = {};
    std::function<void(QWidget*, std::function<void(QStringList)>)> chooseFiles = {};
};
class ImageEditingWindow final : public QObject {
    Q_OBJECT
  public:
    explicit ImageEditingWindow(ImageEditingServices services = {}, QObject* parent = nullptr);
    ~ImageEditingWindow() override;
    void presentEmpty();
    void presentFile(QString path);
    void openFiles(const QStringList& paths);
    void present(ImageEditingSource source);
    void presentSource(ScreenshotExportSource source, QString originalFileName = {});
    void presentHistory(storage::CaptureHistoryRecord record);
    void presentPinnedRecord(QString id);
    void requestClose();
    void confirmDiscard(std::function<void(bool)> completion);
    void shutdown();
    bool dirty() const;
    bool loading() const;
    QByteArray contentCheckpoint() const;
    ImageEditingDocument* document() const;
    ImageEditingCanvas* canvas() const;
    ScreenshotToolPalette* palette() const;
    adqt::widgets::AdModal* modal() const;
    ScreenshotRecognitionSessionController* recognition() const;
  signals:
    void closed();
    void loaded(bool success);
    void exportFinished(bool success);
    void additionalFilesRequested(const QStringList& paths);

  protected:
    bool eventFilter(QObject*, QEvent*) override;

  private:
    friend class ImageEditingWindowCoordinator;
    bool preparePendingEdits(bool requireCanvasCommit = false);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
class ImageEditingWindowCoordinator final : public QObject {
    Q_OBJECT
  public:
    explicit ImageEditingWindowCoordinator(ImageEditingServices services,
                                           QObject* parent = nullptr);
    ~ImageEditingWindowCoordinator() override;
    void presentFiles(const QStringList& paths);
    void presentSelectedFiles(std::shared_ptr<platform::SelectedFileBackend> backend,
                              platform::SelectedFileTarget target);
    void presentHistory(const storage::CaptureHistoryRecord& record);
    void presentSource(ScreenshotExportSource source);
    void presentPinnedRecord(const QString& id);
    void confirmShutdown(std::function<void(bool)> completion);
    // Owner-thread final admission only: commit current drafts without prompting.
    // Refuse dirty/closing windows, a pending review, or membership changes during
    // preparation. This inspects current state; callers must still close admission.
    [[nodiscard]] bool prepareForUpdateHandoff();
    void shutdown();
    bool hasUnsavedChanges() const;
    QVector<QPointer<ImageEditingWindow>> windows() const {
        return m_windows;
    }

  protected:
    bool eventFilter(QObject*, QEvent*) override;

  private:
    ImageEditingWindow* createWindow();
    ImageEditingServices m_services;
    QVector<QPointer<ImageEditingWindow>> m_windows;
    quint64 m_windowRevision = 0;
    bool m_handoffPreparing = false;
    bool m_reviewPending = false;
    bool m_quitPending = false;
    bool m_quitApproved = false;
};
} // namespace snow_shot::presentation
