#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGEFILESERVICE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGEFILESERVICE_H

#include "snow_shot/presentation/screenshotimagerowsource.h"
#include "snow_shot/storage/preparedpngimage.h"

#include "snow_shot/presentation/screenshotpdfexport.h"

#include <QDateTime>
#include <QImage>
#include <QString>
#include <QStringList>

#include <snow/image/codec.h>
#include <snow/image/format.h>

#include <optional>
#include <functional>

class QClipboard;

enum class ScreenshotImageFileFormat {
    Png,
    Jpeg,
    Webp,
    Jxl,
    Avif,
    Bmp,
    Pdf,
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
    [[nodiscard]] static QStringList automaticDirectories();
    [[nodiscard]] static QStringList automaticDirectories(const QString& configuredDirectory);
    [[nodiscard]] static QString saveDialogDirectory(const QString& lastDirectory,
                                                     const QString& configuredDirectory);
    [[nodiscard]] static QString extension(ScreenshotImageFileFormat format);
    [[nodiscard]] static ScreenshotImageFileFormat formatForKey(const QString& key);
    [[nodiscard]] static QString formatKey(ScreenshotImageFileFormat format);
    [[nodiscard]] static QString normalizedPath(QString path, ScreenshotImageFileFormat format);
    [[nodiscard]] static std::optional<ScreenshotImageFileFormat>
    formatForPath(const QString& path);
    [[nodiscard]] static ScreenshotImageFileFormat
    formatForDialogSelection(const QString& path, const QString& selectedFilter);
    [[nodiscard]] static snow::image::Format snowImageFormat(ScreenshotImageFileFormat format);
    [[nodiscard]] static snow::image::EncodeOptions encodeOptions(ScreenshotImageFileFormat format,
                                                                  int quality = 100);
    [[nodiscard]] static ScreenshotImageFileSaveResult
    writeEncodedFile(const QString& encodedFile, const QString& path,
                     ScreenshotImageFileFormat format, std::function<bool()> cancelled = {});

    [[nodiscard]] static ScreenshotImageFileSaveResult
    writePdf(const screenshot_pdf::Payload& payload, const QString& path,
             ScreenshotPdfOptions options, std::function<bool()> cancelled = {});
    [[nodiscard]] static ScreenshotImageFileSaveResult
    write(const QImage& image, const QString& path, ScreenshotImageFileFormat format,
          ScreenshotPdfOptions pdf = {}, std::function<bool()> cancelled = {});
    [[nodiscard]] static ScreenshotImageFileSaveResult write(const ScreenshotImageRowSource& source,
                                                             const QString& path,
                                                             ScreenshotImageFileFormat format,
                                                             ScreenshotPdfOptions pdf = {});
    [[nodiscard]] static ScreenshotImageFileSaveResult
    saveAutomatically(const QImage& image, const QStringList& candidateDirectories,
                      const QDateTime& timestamp);
    [[nodiscard]] static ScreenshotImageFileSaveResult
    saveAutomatically(const QImage& image, const QStringList& candidateDirectories,
                      ScreenshotImageFileFormat format, const QString& filenameFormat,
                      const QDateTime& timestamp = QDateTime::currentDateTime(),
                      ScreenshotPdfOptions pdf = {});
    [[nodiscard]] static ScreenshotImageFileSaveResult saveAutomatically(
        const ScreenshotImageRowSource& source, const QStringList& candidateDirectories,
        ScreenshotImageFileFormat format, const QString& filenameFormat,
        const QDateTime& timestamp = QDateTime::currentDateTime(), ScreenshotPdfOptions pdf = {});
    [[nodiscard]] static bool publishFileToClipboard(QClipboard* clipboard, const QString& path);
    [[nodiscard]] static ScreenshotImageFileSaveResult
    saveAutomatically(const snow_shot::storage::PreparedPngImage& png,
                      const QStringList& candidateDirectories, const QString& filenameFormat,
                      const QDateTime& timestamp = QDateTime::currentDateTime());
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGEFILESERVICE_H
