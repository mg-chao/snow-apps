#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONFILEEXPORT_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONFILEEXPORT_H

#include "snow_shot/app/edition.h"

#include <QString>
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION ||                     \
    SNOW_SHOT_ENABLE_QR_RECOGNITION
#include <QStringList>

class QWidget;
#endif
class ScreenshotRecognitionFileExport;

enum class ScreenshotRecognitionFileKind { Html, Markdown, Qr, Latex };

struct ScreenshotRecognitionFileSnapshot {
    ScreenshotRecognitionFileKind kind = ScreenshotRecognitionFileKind::Qr;
    QString source;
};

#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION ||                     \
    SNOW_SHOT_ENABLE_QR_RECOGNITION
struct ScreenshotRecognitionFileSaveResult {
    QString path;
    QString error;

    [[nodiscard]] bool succeeded() const {
        return !path.isEmpty() && error.isEmpty();
    }
};

class ScreenshotRecognitionFileExport final {
  public:
    [[nodiscard]] static QString extension(ScreenshotRecognitionFileKind kind);
    [[nodiscard]] static QString dialogFilter(ScreenshotRecognitionFileKind kind);
    [[nodiscard]] static QString normalizedPath(const QString& path,
                                                ScreenshotRecognitionFileKind kind);
    [[nodiscard]] static QStringList outputPaths(const QString& primaryPath,
                                                 ScreenshotRecognitionFileKind kind);
    [[nodiscard]] static bool confirmOverwrite(QWidget* owner, const QStringList& paths);
    [[nodiscard]] static ScreenshotRecognitionFileSaveResult
    saveToPath(const ScreenshotRecognitionFileSnapshot& snapshot, const QString& path,
               bool allowOverwrite = false);
    [[nodiscard]] static ScreenshotRecognitionFileSaveResult
    quickSave(const ScreenshotRecognitionFileSnapshot& snapshot, const QString& directory,
              const QString& baseName);
};
#endif

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONFILEEXPORT_H
