#ifndef SNOW_SHOT_PRESENTATION_WINDOWSHORTCUTMANAGER_H
#define SNOW_SHOT_PRESENTATION_WINDOWSHORTCUTMANAGER_H

#include "snow_shot/shortcuts/shortcutbinding.h"

#include <QKeyCombination>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class QKeyEvent;
class QWidget;
class QWindow;

namespace snow_shot::presentation {

class WindowShortcutManager final : public QObject {
  public:
    using BindingHandle = quint64;
    using InputSuspensionHandle = quint64;

    struct StandardPriority {
        static constexpr int ContextualFallback = 100;
        static constexpr int DrawingShortcut = 200;
        static constexpr int ScreenshotShortcut = 300;
        static constexpr int WindowCommand = 400;
    };

    struct ActivationContext {
        QObject* receiver = nullptr;
        QWidget* focusWidget = nullptr;
        const QKeyEvent* event = nullptr;
        // The registered scope root reached from the event receiver. This is
        // populated for direct scope children and owned/transient tool windows.
        QWidget* scopeWindow = nullptr;
        QObject* scopeObject = nullptr;
        QObject* focusObject = nullptr;
    };

    struct Binding {
        enum class ActivationTrigger { Press, Release };

        QString id;
        shortcuts::ShortcutBindingList shortcutBindings;
        // Compatibility input for fixed application commands. Configurable
        // shortcuts use shortcutBindings so physical metadata survives.
        QList<QKeyCombination> keyCombinations;
        int priority = 0;
        bool autoRepeat = false;
        // Release actions reserve the initial press and execute only after its
        // physical release. They cannot also define held-control callbacks.
        ActivationTrigger activationTrigger = ActivationTrigger::Press;
        // Held contextual shortcuts may tolerate a narrowly scoped extra
        // modifier (for example Shift followed by Space during a resize).
        Qt::KeyboardModifiers allowedAdditionalModifiers = Qt::NoModifier;
        std::function<bool(const ActivationContext&)> canActivate = [](const ActivationContext&) {
            return true;
        };
        std::function<bool(const ActivationContext&)> activate;
        // Optional key-release action. It is used by held local modifiers
        // whose state must end before the mouse gesture is released.
        std::function<bool(const ActivationContext&)> release;
        // Required together with release. Ends a held action when input is
        // interrupted, without performing physical-release-only actions.
        std::function<void()> cancel;
        // Modal interactions may receive keys through a transient tool window
        // that is not part of the owner's normal shortcut scope.
        std::function<bool(const ActivationContext&)> canActivateOutsideScope;
    };

    explicit WindowShortcutManager(QObject* parent = nullptr);
    ~WindowShortcutManager() override;

    void addScopeWindow(QWidget* window);
    void removeScopeWindow(QWidget* window);
    void addScopeWindow(QWindow* window);
    void removeScopeWindow(QWindow* window);

    // Temporarily prevents this manager from dispatching shortcut presses.
    // Suspension is tokenized so nested modal interactions cannot resume one
    // another accidentally.
    [[nodiscard]] InputSuspensionHandle suspendInput();
    void resumeInput(InputSuspensionHandle handle);

    [[nodiscard]] BindingHandle addBinding(QObject* owner, Binding binding);
    [[nodiscard]] bool setShortcuts(BindingHandle handle,
                                    const shortcuts::ShortcutBindingList& shortcuts);
    [[nodiscard]] bool setKeyCombinations(BindingHandle handle,
                                          const QList<QKeyCombination>& keyCombinations);
    [[nodiscard]] bool removeBinding(BindingHandle handle);

    [[nodiscard]] static QList<QKeyCombination>
    keyCombinationsFromBindings(const shortcuts::ShortcutBindingList& shortcuts);
    [[nodiscard]] static shortcuts::ShortcutBindingList
    shortcutBindingsFromKeyCombinations(const QList<QKeyCombination>& keyCombinations);

    // Returns whether keyboard focus belongs to an editable text control.
    // Read-only text surfaces remain eligible for window command shortcuts.
    [[nodiscard]] static bool focusAcceptsTextInput(QWidget* focusWidget);
    [[nodiscard]] static bool focusObjectAcceptsTextInput(QObject* focusObject);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_WINDOWSHORTCUTMANAGER_H
