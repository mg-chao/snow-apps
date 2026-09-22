#ifndef SNOW_SHOT_PRESENTATION_GLOBALSHORTCUTMANAGER_H
#define SNOW_SHOT_PRESENTATION_GLOBALSHORTCUTMANAGER_H

#include "snow_shot/presentation/globalshortcuttypes.h"

#include <QList>
#include <QObject>

#include <functional>
#include <memory>

namespace snow_shot::presentation {
class GlobalShortcutBackend {
  public:
    using ActivationHandler = std::function<void(int)>;
    using AvailabilityChangedHandler = std::function<void(const QList<int>&)>;

    virtual ~GlobalShortcutBackend() = default;

    virtual void setActivationHandler(ActivationHandler handler) = 0;
    // Called on the manager's thread when native availability changes. The IDs
    // identify registrations the manager must release before reconciling; an
    // empty list still requests a retry of previously unavailable bindings.
    virtual void setAvailabilityChangedHandler(AvailabilityChangedHandler) {}
    virtual void refreshAvailability() {}
    [[nodiscard]] virtual GlobalShortcutValidationResult
    validateShortcut(const snow_shot::shortcuts::ShortcutBinding& binding) const = 0;
    [[nodiscard]] virtual GlobalShortcutBackendResult
    registerShortcut(int registrationId, const snow_shot::shortcuts::ShortcutBinding& binding) = 0;
    virtual void unregisterShortcut(int registrationId) = 0;
};

class GlobalShortcutManager final : public QObject {
    Q_OBJECT

  public:
    using RegistrationSuspensionHandle = quint64;
    explicit GlobalShortcutManager(QObject* parent = nullptr);
    GlobalShortcutManager(std::unique_ptr<GlobalShortcutBackend> backend, QObject* parent = nullptr,
                          std::function<bool()> focusedFullscreenDetector = {});
    ~GlobalShortcutManager() override;

    void initialize();
    [[nodiscard]] GlobalShortcutRegistrationState state(GlobalShortcutAction action) const;
    [[nodiscard]] GlobalShortcutValidationResult
    validateShortcut(GlobalShortcutAction action,
                     const snow_shot::shortcuts::ShortcutBinding& shortcut) const;
    [[nodiscard]] GlobalShortcutValidationResult validateShortcut(const QString& shortcut) const;
    bool setShortcuts(GlobalShortcutAction action,
                      const snow_shot::shortcuts::ShortcutBindingList& shortcuts);
    [[nodiscard]] RegistrationSuspensionHandle suspendRegistrations();
    void resumeRegistrations(RegistrationSuspensionHandle handle);
    void setGlobalHotkeysEnabled(bool enabled);
    [[nodiscard]] bool globalHotkeysEnabled() const;

  signals:
    void activated(snow_shot::presentation::GlobalShortcutAction action);
    void stateChanged(snow_shot::presentation::GlobalShortcutAction action,
                      const snow_shot::presentation::GlobalShortcutRegistrationState& state);
    void globalHotkeysEnabledChanged(bool enabled);

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_GLOBALSHORTCUTMANAGER_H
