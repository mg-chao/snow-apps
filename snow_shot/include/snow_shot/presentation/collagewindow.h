// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_COLLAGEWINDOW_H
#define SNOW_SHOT_PRESENTATION_COLLAGEWINDOW_H

#include "snow_shot/presentation/collagedocument.h"
#include "snow_shot/presentation/collagepdfexport.h"
#include "snow_shot/storage/capturehistorytypes.h"
#include "snow_shot/presentation/screenshotpinnedimagesnapshot.h"
#include "snow_shot/platform/selectedfiles.h"
#include <QObject>
#include <functional>
#include <memory>
#include <optional>

class ScreenshotExportArtifact;
class QWidget;
namespace adqt::widgets {
class AdModal;
}
namespace snow_shot::presentation::components {
class CollageEditorWidget;
}
namespace snow_shot::presentation {

struct ImageSplitSnapshot;

// Narrow asynchronous ports make terminal publication/writer behavior deterministic in tests.
struct CollageWindowServices final {
    using Completion = std::function<void(bool, QString)>;
    using Export = std::function<void(std::shared_ptr<ScreenshotExportArtifact>, QObject*, QWidget*,
                                      Completion)>;
    Export copy;
    Export save;
    std::function<void(QWidget*, bool clear, std::function<void(bool)>)> confirm;
    std::function<std::optional<QImage>(const storage::CaptureHistoryRecord&)> readHistory;
    using PdfChoice = std::function<void(std::optional<CollagePdfDestination>)>;
    std::function<void(QWidget*, CollagePdfExportMode, PdfChoice)> choosePdfDestination;
    std::function<CollagePdfExportResult(const QVector<CollageItem>&, const CollagePdfSettings&,
                                         const CollagePdfDestination&, std::function<bool()>,
                                         std::function<void(const CollagePdfExportProgress&)>)>
        exportPdf;
};

class CollageWindow final : public QObject {
    Q_OBJECT
  public:
    explicit CollageWindow(QObject* parent = nullptr, CollageWindowServices services = {});
    ~CollageWindow() override;
    void present(const QVector<storage::CaptureHistoryRecord>& records = {});
    void presentPinnedRecords(const QStringList& recordIds);
    void presentPinnedWindows(const QVector<ScreenshotPinnedImageSnapshot>& snapshots);
    [[nodiscard]] bool presentSplitImagesAsPdf(ImageSplitSnapshot snapshot);
    void addFiles(const QStringList& paths);
    void paste();
    void addSelectedFiles(std::shared_ptr<platform::SelectedFileBackend> backend,
                          platform::SelectedFileTarget target);
    void replaceFile(quint64 id, const QString& path);
    void requestClose();
    void requestClear();
    void copy();
    void save();
    void shutdown();
    [[nodiscard]] bool busy() const;
    [[nodiscard]] bool loading() const;
    [[nodiscard]] CollageDocument* document() const;
    [[nodiscard]] components::CollageEditorWidget* editor() const;
    [[nodiscard]] adqt::widgets::AdModal* modal() const;

  signals:
    void closed();
    void importFinished(const QStringList& failures);
    void exportFinished(bool success);
    void pdfExportFinished(const CollagePdfExportResult& result);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace snow_shot::presentation
#endif
