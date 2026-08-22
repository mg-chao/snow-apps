#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGEFILESERVICE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGEFILESERVICE_H

#include "snow_shot/presentation/screenshotimagerowsource.h"

#include <QDateTime>
#include <QImage>
#include <QString>
#include <QStringList>

#include <snow/image/codec.h>
#include <snow/image/format.h>

#include <optional>

class QClipboard;

enum class ScreenshotImageFileFormat {
    Png,
    Jpeg,
    Webp,
    Jxl,
    Avif,
};

struct ScreenshotImageFileSaveResult {
    QString path;
    QString error;

    [[nodiscard]] bool succeeded() const {
        return !path.isEmpty() && error.isEmpty();
    }
};

class ScreenshotImageFileService final {
  public:
    [[nodiscard]] static QString
    suggestedBaseName(const QDateTime& timestamp = QDateTime::currentDateTime());
    [[nodiscard]] static QString
    suggestedBaseName(const QString& filenameFormat,
                      const QDateTime& timestamp = QDateTime::currentDateTime());
    [[nodiscard]] static QString dialogFilter(ScreenshotImageFileFormat format);
    [[nodiscard]] static QString saveDialogFilter();
    [[nodiscard]] static QString automaticDirectory();
    [[nodiscard]] static QStringList automaticDirectories();
    [[nodiscard]] static QStringList automaticDirectories(const QString& configuredDirectory);
    [[nodiscard]] static QString saveDialogDirectory(const QString& lastDirectory,
                                                     const QString& configuredDirectory);
    [[nodiscard]] static QString extension(ScreenshotImageFileFormat format);
    [[nodiscard]] static ScreenshotImageFileFormat formatForKey(const QString& key);
    [[nodiscard]] static QString normalizedPath(QString path, ScreenshotImageFileFormat format);
    [[nodiscard]] static std::optional<ScreenshotImageFileFormat>
    formatForPath(const QString& path);
    [[nodiscard]] static ScreenshotImageFileFormat
    formatForDialogSelection(const QString& path, const QString& selectedFilter);
    [[nodiscard]] static snow::image::Format snowImageFormat(ScreenshotImageFileFormat format);
    [[nodiscard]] static snow::image::EncodeOptions encodeOptions(ScreenshotImageFileFormat format);

    [[nodiscard]] static ScreenshotImageFileSaveResult
    write(const QImage& image, const QString& path, ScreenshotImageFileFormat format);
    [[nodiscard]] static ScreenshotImageFileSaveResult write(const ScreenshotImageRowSource& source,
                                                             const QString& path,
                                                             ScreenshotImageFileFormat format);
    [[nodiscard]] static ScreenshotImageFileSaveResult saveAutomatically(const QImage& image);
    [[nodiscard]] static ScreenshotImageFileSaveResult
    saveAutomatically(const QImage& image, const QStringList& candidateDirectories,
                      const QDateTime& timestamp);
    [[nodiscard]] static ScreenshotImageFileSaveResult
    saveAutomatically(const QImage& image, const QStringList& candidateDirectories,
                      ScreenshotImageFileFormat format, const QString& filenameFormat,
                      const QDateTime& timestamp = QDateTime::currentDateTime());
    [[nodiscard]] static ScreenshotImageFileSaveResult
    saveAutomatically(const ScreenshotImageRowSource& source,
                      const QStringList& candidateDirectories, ScreenshotImageFileFormat format,
                      const QString& filenameFormat,
                      const QDateTime& timestamp = QDateTime::currentDateTime());
    [[nodiscard]] static bool publishFileToClipboard(QClipboard* clipboard, const QString& path);
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGEFILESERVICE_H
