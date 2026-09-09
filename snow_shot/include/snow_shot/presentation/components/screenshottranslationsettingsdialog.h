#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTRANSLATIONSETTINGSDIALOG_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTRANSLATIONSETTINGSDIALOG_H
#include <functional>
class QObject;
class QWidget;
namespace adqt::widgets {
class AdModal;
}
namespace snow_shot::translation {
class TranslationService;
}
namespace snow_shot::presentation {
adqt::widgets::AdModal*
createScreenshotTranslationSettingsDialog(translation::TranslationService& service, QWidget* owner,
                                          QObject* parent,
                                          std::function<void(bool before)> displayModeChanged);
}
#endif
