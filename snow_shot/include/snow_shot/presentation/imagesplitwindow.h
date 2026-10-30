// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_IMAGESPLITWINDOW_H
#define SNOW_SHOT_PRESENTATION_IMAGESPLITWINDOW_H

#include "snow_shot/presentation/imagesplitdocument.h"
#include "snow_shot/platform/selectedfiles.h"
#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/storage/capturehistorytypes.h"

#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <functional>
#include <memory>
#include <optional>

class QWidget;
namespace adqt::widgets {
class AdModal;
}
namespace snow_shot::presentation::components {
class ImageSplitEditorWidget;
}
namespace snow_shot::presentation {

struct ImageSplitSource final {
    ScreenshotExportSource source;
    QString originalFileName;
    QDateTime createdUtc;
    QString error;
    QSize outputSize;
    std::optional<ImageSplitDirection> initialDirection = std::nullopt;
};

// UI choices are asynchronous; readers and the batch writer run on an export worker.
struct ImageSplitWindowServices final {
    using Choice = std::function<void(QString)>;
    using Cancellation = std::function<bool()>;
    std::function<void(QWidget*, Choice)> chooseFile;
    std::function<void(QWidget*, Choice)> chooseDirectory;
    std::function<void(QWidget*, bool replacing, std::function<void(bool)>)> confirm;
    std::function<std::optional<QImage>(const storage::CaptureHistoryRecord&)> readHistory;
    std::function<std::optional<QImage>(const QString&, Cancellation)> readFile;
    std::function<ImageSplitExportResult(const QImage&, const ImageSplitLayout&, const QString&,
                                         const QString&, Cancellation,
                                         std::function<void(qint64, qint64)>)>
        exportBatch;
    std::function<void(const QString&)> openDirectory;
    std::function<bool(ImageSplitSnapshot)> stitchToPdf;
};

class ImageSplitWindow final : public QObject {
    Q_OBJECT
  public:
    explicit ImageSplitWindow(QObject* parent = nullptr, ImageSplitWindowServices services = {});
    ~ImageSplitWindow() override;
    void presentHistory(const storage::CaptureHistoryRecord& record);
    void presentPinnedRecord(const QString& recordId);
    void presentSource(ImageSplitSource source);
    void presentImage(QImage image, QString originalFileName = {}, QDateTime createdUtc = {});
    void reload();
    void replaceFile(const QString& path);
    void dropFiles(const QStringList& paths);
    void paste();
    void saveAll();
    void stitchToPdf();
    void requestClose();
    void shutdown();
    [[nodiscard]] bool busy() const;
    [[nodiscard]] bool loading() const;
    [[nodiscard]] ImageSplitDocument* document() const;
    [[nodiscard]] components::ImageSplitEditorWidget* editor() const;
    [[nodiscard]] adqt::widgets::AdModal* modal() const;
    [[nodiscard]] QString imageName() const;
    [[nodiscard]] QString lastError() const;

  signals:
    void closed();
    void imageLoaded(bool success);
    void exportFinished(const ImageSplitExportResult& result);

  private:
    friend class ImageSplitWindowCoordinator;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class ImageSplitWindowCoordinator final : public QObject {
    Q_OBJECT
  public:
    explicit ImageSplitWindowCoordinator(QObject* parent = nullptr,
                                         ImageSplitWindowServices services = {});
    ~ImageSplitWindowCoordinator() override;
    void presentHistory(const storage::CaptureHistoryRecord& record);
    void presentPinnedRecord(const QString& recordId);
    void presentSource(ImageSplitSource source);
    void presentSelectedFiles(std::shared_ptr<platform::SelectedFileBackend> backend,
                              platform::SelectedFileTarget target);
    void shutdown();
    [[nodiscard]] QVector<ImageSplitWindow*> windows() const;

  private:
    ImageSplitWindow* createWindow();
    ImageSplitWindowServices m_services;
    QVector<QPointer<ImageSplitWindow>> m_windows;
    bool m_shutdown = false;
};
} // namespace snow_shot::presentation
#endif
