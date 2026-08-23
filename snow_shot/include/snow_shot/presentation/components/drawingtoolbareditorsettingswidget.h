#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_DRAWINGTOOLBAREDITSETTINGSWIDGET_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_DRAWINGTOOLBAREDITSETTINGSWIDGET_H

#include "snow_shot/presentation/components/settingscustomwidget.h"

#include <memory>

class QEvent;

namespace snow_shot::presentation::settings {
class SettingsRuntimeBindings;
}

class DrawingToolbarEditorSettingsWidget final : public SettingsCustomWidget {
  public:
    explicit DrawingToolbarEditorSettingsWidget(
        snow_shot::presentation::settings::SettingsRuntimeBindings& runtimeBindings,
        QWidget* parent = nullptr);
    ~DrawingToolbarEditorSettingsWidget() override;

    void applyTheme(
        const snow_shot::presentation::styles::ThemeColorScheme& scheme) override;
    void retranslateUi() override;

  protected:
    void changeEvent(QEvent* event) override;

  private:
    struct Private;
    std::unique_ptr<Private> m_private;
};

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_DRAWINGTOOLBAREDITSETTINGSWIDGET_H
