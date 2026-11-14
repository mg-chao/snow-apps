#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONFILEEXPORT_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONFILEEXPORT_H

#include "snow_shot/app/edition.h"
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#endif

#include <QString>
#include "snow_shot/presentation/screenshotrecognizeddocument.h"
#include <functional>
class QUrl;
class QMimeData;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
#include "snow_shot/presentation/screenshottabledocument.h"
#include <optional>
#endif
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION ||                     \
    SNOW_SHOT_ENABLE_QR_RECOGNITION || SNOW_SHOT_ENABLE_TABLE_RECOGNITION
#include <QStringList>

class QWidget;
#endif
class ScreenshotRecognitionFileExport;

enum class ScreenshotRecognitionFileKind {
    Html,
    Markdown,
    Qr,
    Latex,
    Table,
    DocumentWord,
    DocumentPdf,
    DocumentMarkdown,
    DocumentLatex,
    DocumentHtml
};

struct ScreenshotRecognitionFileSnapshot {
    ScreenshotRecognitionFileKind kind = ScreenshotRecognitionFileKind::Qr;
    QString source;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    std::optional<ScreenshotTableDocument> table = std::nullopt;
    // Full column widths in device-independent pixels, captured from the table editor.
    QVector<int> tableColumnWidths = {};
#endif

    std::shared_ptr<const ScreenshotRecognizedDocument> document;

    [[nodiscard]] bool hasContent() const {
        if (kind >= ScreenshotRecognitionFileKind::DocumentWord)
            return document && !document->html.isEmpty();
        if (kind == ScreenshotRecognitionFileKind::Table) {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
            return table.has_value() && !table->empty();
#else
            return false;
#endif
        }
        return !source.isEmpty();
    }
};

#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
struct ScreenshotDocumentCopyRequest {
    std::shared_ptr<const ScreenshotRecognizedDocument> document;
    QString selectionHtml;
    bool asFile = false;

    using Completion = std::function<void(std::unique_ptr<QMimeData>, QString)>;
    [[nodiscard]] ScreenshotExportJobHandle start(QObject* receiver, Completion completion) const;
    [[nodiscard]] std::unique_ptr<QMimeData> mimeData() const;
};
#endif

#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION ||                     \
    SNOW_SHOT_ENABLE_QR_RECOGNITION || SNOW_SHOT_ENABLE_TABLE_RECOGNITION
struct ScreenshotRecognitionFileSaveResult {
    QString path;
    QString error;
    bool wasCancelled = false;

    [[nodiscard]] bool succeeded() const {
        return !path.isEmpty() && error.isEmpty();
    }
};

class ScreenshotRecognitionFileExport final {
  public:
    [[nodiscard]] static ScreenshotRecognitionFileKind
    kindForPath(ScreenshotRecognitionFileKind kind, const QString& path);
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
    [[nodiscard]] static ScreenshotRecognitionFileSaveResult
    openTemporaryDocument(const ScreenshotRecognitionFileSnapshot& snapshot,
                          std::function<bool(const QUrl&)> openUrl = {});
#endif
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    // Successful launches retain their temporary workbook for the external application.
    [[nodiscard]] static ScreenshotRecognitionFileSaveResult
    openTemporaryTable(const ScreenshotRecognitionFileSnapshot& snapshot,
                       std::function<bool(const QUrl&)> openUrl = {});
#endif
    [[nodiscard]] static ScreenshotRecognitionFileSaveResult
    copyDocumentFile(const ScreenshotRecognitionFileSnapshot& snapshot,
                     const std::function<bool()>& cancelled = {});
    [[nodiscard]] static QString pathForSelectedFilter(const QString& path, const QString& filter);
    [[nodiscard]] static QString extension(ScreenshotRecognitionFileKind kind);
    [[nodiscard]] static QString dialogFilter(ScreenshotRecognitionFileKind kind);
    [[nodiscard]] static QString normalizedPath(const QString& path,
                                                ScreenshotRecognitionFileKind kind);
    [[nodiscard]] static QStringList outputPaths(const QString& primaryPath,
                                                 const ScreenshotRecognitionFileSnapshot& snapshot);
    [[nodiscard]] static bool confirmOverwrite(QWidget* owner, const QStringList& paths);
    [[nodiscard]] static ScreenshotRecognitionFileSaveResult
    saveToPath(const ScreenshotRecognitionFileSnapshot& snapshot, const QString& path,
               bool allowOverwrite = false, std::function<bool()> cancelled = {});
    [[nodiscard]] static ScreenshotRecognitionFileSaveResult
    quickSave(const ScreenshotRecognitionFileSnapshot& snapshot, const QString& directory,
              const QString& baseName, std::function<bool()> cancelled = {});
};
#endif

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONFILEEXPORT_H
