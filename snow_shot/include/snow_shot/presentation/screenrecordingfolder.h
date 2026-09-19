#ifndef SNOW_SHOT_PRESENTATION_SCREENRECORDINGFOLDER_H
#define SNOW_SHOT_PRESENTATION_SCREENRECORDINGFOLDER_H

#include <QStringList>

namespace snow_shot::presentation::recording {
[[nodiscard]] QStringList screenRecordingDirectories();
[[nodiscard]] QString screenRecordingDirectory();
[[nodiscard]] bool openScreenRecordingFolder();
} // namespace snow_shot::presentation::recording

#endif // SNOW_SHOT_PRESENTATION_SCREENRECORDINGFOLDER_H
