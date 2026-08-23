#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYSHORTCUTCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYSHORTCUTCONTROLLER_H

#include "snow_shot/presentation/screenshotoverlayinputhandler.h"

#include <QObject>

#include <memory>

class ScreenshotIntelligentSelectionModel;
class ScreenshotInteractionState;
class ScreenshotOverlayInputHandler;

namespace snow_shot::presentation {
class WindowShortcutManager;
}

class ScreenshotOverlayShortcutController final : public QObject {
  public:
    ScreenshotOverlayShortcutController(
        snow_shot::presentation::WindowShortcutManager& shortcutManager,
        ScreenshotOverlayInputHandler& inputHandler, ScreenshotInteractionState& interaction,
        ScreenshotIntelligentSelectionModel& intelligentSelection,
        ScreenshotOverlayInputActions actions, QObject* parent = nullptr);
    ~ScreenshotOverlayShortcutController() override;

    void reloadConfiguredShortcuts();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYSHORTCUTCONTROLLER_H
