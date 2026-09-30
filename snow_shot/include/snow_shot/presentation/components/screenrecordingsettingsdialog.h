#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_SCREENRECORDINGSETTINGSDIALOG_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_SCREENRECORDINGSETTINGSDIALOG_H

class QObject;
class QWidget;
namespace adqt::widgets {
class AdModal;
}

namespace snow_shot::presentation {
// Creates the shared feature/system recording preferences on demand.
// The caller owns opening and closing the window; finished dialogs delete themselves.
adqt::widgets::AdModal* createScreenRecordingSettingsDialog(QWidget* owner, QObject* parent);
} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_SCREENRECORDINGSETTINGSDIALOG_H
