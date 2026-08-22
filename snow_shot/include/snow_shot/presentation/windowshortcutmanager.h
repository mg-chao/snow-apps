#ifndef SNOW_SHOT_PRESENTATION_WINDOWSHORTCUTMANAGER_H
#define SNOW_SHOT_PRESENTATION_WINDOWSHORTCUTMANAGER_H

#include <QKeyCombination>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

class QKeyEvent;
class QWidget;

namespace snow_shot::presentation {

class WindowShortcutManager final : public QObject {
  public:
    using BindingHandle = quint64;

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
    };

    struct Binding {
        QString id;
        QList<QKeyCombination> keyCombinations;
        int priority = 0;
        bool autoRepeat = false;
        // Held contextual shortcuts may tolerate a narrowly scoped extra
        // modifier (for example Shift followed by Space during a resize).
        Qt::KeyboardModifiers allowedAdditionalModifiers = Qt::NoModifier;
        std::function<bool(const ActivationContext&)> canActivate =
            [](const ActivationContext&) { return true; };
        std::function<bool(const ActivationContext&)> activate;
        // Optional key-release action. It is used by held local modifiers
        // whose state must end before the mouse gesture is released.
        std::function<bool(const ActivationContext&)> release;
        // Modal interactions may receive keys through a transient tool window
        // that is not part of the owner's normal shortcut scope.
        std::function<bool(const ActivationContext&)> canActivateOutsideScope;
    };

    explicit WindowShortcutManager(QObject* parent = nullptr);
    ~WindowShortcutManager() override;

    void addScopeWindow(QWidget* window);
    void removeScopeWindow(QWidget* window);

    [[nodiscard]] BindingHandle addBinding(QObject* owner, Binding binding);
    [[nodiscard]] bool setKeyCombinations(BindingHandle handle,
                                          const QList<QKeyCombination>& keyCombinations);
    [[nodiscard]] bool removeBinding(BindingHandle handle);

    [[nodiscard]] static QList<QKeyCombination>
    keyCombinationsFromPortableText(const QStringList& shortcuts);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_WINDOWSHORTCUTMANAGER_H
