// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "snow_shot/presentation/imageeditingdocument.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
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
};
class ImageEditingWindow final : public QObject {
    Q_OBJECT
  public:
    explicit ImageEditingWindow(ImageEditingServices services = {}, QObject* parent = nullptr);
    ~ImageEditingWindow() override;
    void present(ImageEditingSource source);
    void presentSource(ScreenshotExportSource source);
    void presentHistory(storage::CaptureHistoryRecord record);
    void presentPinnedRecord(QString id);
    void requestClose();
    void confirmDiscard(std::function<void(bool)> completion);
    void shutdown();
    bool dirty() const;
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

  protected:
    bool eventFilter(QObject*, QEvent*) override;

  private:
    friend class ImageEditingWindowCoordinator;
    bool preparePendingEdits();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
class ImageEditingWindowCoordinator final : public QObject {
    Q_OBJECT
  public:
    explicit ImageEditingWindowCoordinator(ImageEditingServices services,
                                           QObject* parent = nullptr);
    ~ImageEditingWindowCoordinator() override;
    void presentHistory(const storage::CaptureHistoryRecord& record);
    void presentSource(ScreenshotExportSource source);
    void presentPinnedRecord(const QString& id);
    void confirmShutdown(std::function<void(bool)> completion);
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
    bool m_reviewPending = false;
    bool m_quitPending = false;
    bool m_quitApproved = false;
};
} // namespace snow_shot::presentation
