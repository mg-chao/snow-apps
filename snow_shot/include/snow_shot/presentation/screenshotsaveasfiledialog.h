#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSAVEASFILEDIALOG_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSAVEASFILEDIALOG_H

#include "snow_shot/presentation/screenshotimagefileservice.h"

#include <QObject>
#include <QSize>
#include <functional>
#include <memory>

class QWidget;
class ScreenshotExportArtifact;

struct ScreenshotSaveExportOptions {
    QSize size;
    ScreenshotImageFileFormat format = ScreenshotImageFileFormat::Png;
    int quality = 100;
    friend bool operator==(const ScreenshotSaveExportOptions&,
                           const ScreenshotSaveExportOptions&) = default;
};

struct ScreenshotSaveDialogState {
    QString directory;
    QString filename;
    QSize sourceSize;
    ScreenshotSaveExportOptions output;
    bool lockAspectRatio = true;
    [[nodiscard]] static ScreenshotSaveDialogState initial(QSize sourceSize);
    void setDimension(bool width, int value);
    [[nodiscard]] QString imageValidationError() const;
    [[nodiscard]] QString validationError() const;
    [[nodiscard]] QString outputPath() const;
    [[nodiscard]] bool lossless() const;
};

class ScreenshotSaveAsFileDialog final {
  public:
    using Saved = std::function<void(const QString& path)>;
    using Finished = std::function<void(bool saved)>;
    // owner selects the display/centering; stackingOwner optionally keeps a floating toolbar
    // below the dialog without changing the toolbar's visibility.
    [[nodiscard]] static bool open(QObject* lifetime, QWidget* owner,
                                   std::shared_ptr<ScreenshotExportArtifact> source,
                                   Saved saved = {}, Finished finished = {},
                                   QWidget* stackingOwner = nullptr);
    [[nodiscard]] static bool open(QObject* lifetime, QWidget* owner, const QImage& image,
                                   Saved saved = {}, Finished finished = {},
                                   QWidget* stackingOwner = nullptr);
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSAVEASFILEDIALOG_H
