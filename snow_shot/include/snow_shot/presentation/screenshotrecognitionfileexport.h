#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONFILEEXPORT_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONFILEEXPORT_H

#include <QString>
#include <QStringList>

class QWidget;

enum class ScreenshotRecognitionFileKind { Html, Markdown, Qr };

struct ScreenshotRecognitionFileSnapshot {
    ScreenshotRecognitionFileKind kind = ScreenshotRecognitionFileKind::Qr;
    QString source;
};

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

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTRECOGNITIONFILEEXPORT_H
