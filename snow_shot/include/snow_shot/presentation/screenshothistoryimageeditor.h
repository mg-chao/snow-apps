#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTHISTORYIMAGEEDITOR_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTHISTORYIMAGEEDITOR_H

#include <QString>
#include <functional>

class QObject;
class QScreen;
class ScreenshotPinnedWindow;
namespace snow_shot::storage {
class CaptureHistoryRepository;
}

// Open the saved result without a new desktop capture or the original display layout.
[[nodiscard]] ScreenshotPinnedWindow*
openScreenshotHistoryImageEditor(snow_shot::storage::CaptureHistoryRepository& repository,
                                 const QString& recordId, QScreen* screen, QObject* lifetime,
                                 std::function<void(const QString&)> reportFailure);

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTHISTORYIMAGEEDITOR_H
