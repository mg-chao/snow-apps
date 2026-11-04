#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONFILEEXPORT_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONFILEEXPORT_H

#include "snow_shot/app/edition.h"

#include <QString>
#include <functional>
class QUrl;
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

enum class ScreenshotRecognitionFileKind { Html, Markdown, Qr, Latex, Table };

struct ScreenshotRecognitionFileSnapshot {
    ScreenshotRecognitionFileKind kind = ScreenshotRecognitionFileKind::Qr;
    QString source;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    std::optional<ScreenshotTableDocument> table = std::nullopt;
    // Full column widths in device-independent pixels, captured from the table editor.
    QVector<int> tableColumnWidths = {};
#endif

    [[nodiscard]] bool hasContent() const {
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
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    // Successful launches retain their temporary workbook for the external application.
    [[nodiscard]] static ScreenshotRecognitionFileSaveResult
    openTemporaryTable(const ScreenshotRecognitionFileSnapshot& snapshot,
                       std::function<bool(const QUrl&)> openUrl = {});
#endif
    [[nodiscard]] static QString extension(ScreenshotRecognitionFileKind kind);
    [[nodiscard]] static QString dialogFilter(ScreenshotRecognitionFileKind kind);
    [[nodiscard]] static QString normalizedPath(const QString& path,
                                                ScreenshotRecognitionFileKind kind);
    [[nodiscard]] static QStringList outputPaths(const QString& primaryPath,
                                                 ScreenshotRecognitionFileKind kind);
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
