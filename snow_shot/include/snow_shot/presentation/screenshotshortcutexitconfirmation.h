#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSHORTCUTEXITCONFIRMATION_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSHORTCUTEXITCONFIRMATION_H

#include <QObject>

#include <functional>
#include <memory>

class QWidget;

namespace snow_shot::presentation {

class WindowShortcutManager;

class ScreenshotShortcutExitConfirmation final : public QObject {
  public:
    using ExitAction = std::function<void()>;
    using RestoreOwner = std::function<void(QWidget*)>;

    ScreenshotShortcutExitConfirmation(WindowShortcutManager& shortcutManager,
                                       ExitAction exitAction, RestoreOwner restoreOwner,
                                       QObject* parent = nullptr);
    ~ScreenshotShortcutExitConfirmation() override;

    [[nodiscard]] bool request(bool confirmationRequired, QWidget* owner);
    void dismiss();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSHORTCUTEXITCONFIRMATION_H
