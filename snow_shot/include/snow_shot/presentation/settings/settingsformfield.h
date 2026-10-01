#ifndef SNOW_SHOT_PRESENTATION_SETTINGS_SETTINGSFORMFIELD_H
#define SNOW_SHOT_PRESENTATION_SETTINGS_SETTINGSFORMFIELD_H

#include "snow_shot/presentation/components/formfields.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"

#include <QObject>

#include <memory>

namespace snow_shot::presentation::settings {

// A field's draft and persistence state belong to the runtime session. This adapter only
// presents that state and forwards completed edits to its stable registry field ID.
class SettingsFormField final : public QObject {
  public:
    [[nodiscard]] static bool supports(const SettingsFieldDescriptor& descriptor);
    [[nodiscard]] static SettingsFormField* create(const SettingsFieldDescriptor& descriptor,
                                                   SettingsRuntimeSession& session,
                                                   components::form_fields::Options options = {});
    ~SettingsFormField() override;

    [[nodiscard]] const SettingsFieldDescriptor& descriptor() const;
    [[nodiscard]] QWidget* viewWidget() const;
    [[nodiscard]] QWidget* editor() const;
    [[nodiscard]] QWidget* focusTarget() const;
    [[nodiscard]] components::form_fields::FormField* controller() const;

    void sync(const SettingsFieldState* state = nullptr);
    void retranslateUi();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    SettingsFormField(const SettingsFieldDescriptor& descriptor, SettingsRuntimeSession& session,
                      const components::form_fields::Options& options);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace snow_shot::presentation::settings

#endif // SNOW_SHOT_PRESENTATION_SETTINGS_SETTINGSFORMFIELD_H
