#ifndef SNOW_SHOT_PRESENTATION_SCREENRECORDINGSHORTCUTCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_SCREENRECORDINGSHORTCUTCONTROLLER_H

#include "snow_shot/presentation/windowshortcutmanager.h"

#include <QMap>
#include <QPointer>

class ScreenRecordingAreaWindow;
class ScreenRecordingToolbarWindow;

class ScreenRecordingShortcutController final : public QObject {
  public:
    ScreenRecordingShortcutController(ScreenRecordingAreaWindow& area,
                                      ScreenRecordingToolbarWindow& toolbar,
                                      QObject* parent = nullptr);

  private:
    using ShortcutManager = snow_shot::presentation::WindowShortcutManager;
    bool canActivate(const ShortcutManager::ActivationContext& context) const;
    void reloadConfiguredShortcuts();

    QPointer<ScreenRecordingAreaWindow> m_area;
    QPointer<ScreenRecordingToolbarWindow> m_toolbar;
    ShortcutManager m_shortcutManager;
    QMap<QString, ShortcutManager::BindingHandle> m_recordingBindings;
    QMap<QString, ShortcutManager::BindingHandle> m_drawingBindings;
    QMap<QString, ShortcutManager::BindingHandle> m_historyBindings;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENRECORDINGSHORTCUTCONTROLLER_H
