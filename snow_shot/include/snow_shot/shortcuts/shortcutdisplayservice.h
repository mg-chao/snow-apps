#ifndef SNOW_SHOT_SHORTCUTS_SHORTCUTDISPLAYSERVICE_H
#define SNOW_SHOT_SHORTCUTS_SHORTCUTDISPLAYSERVICE_H

#include "snow_shot/shortcuts/shortcutbinding.h"

#include <QObject>

#include <memory>

namespace snow_shot::shortcuts {

class ShortcutDisplayService final : public QObject {
    Q_OBJECT

  public:
    [[nodiscard]] static ShortcutDisplayService& instance();

    [[nodiscard]] QString text(const ShortcutBinding& binding) const;
    [[nodiscard]] QString text(const ShortcutBindingList& bindings) const;
    [[nodiscard]] QString modifierText(Qt::KeyboardModifiers modifiers) const;

    // Re-resolves native key legends and notifies every cached presentation.
    // Public so platform glue and deterministic tests share the same path.
    void refresh();

  signals:
    void displayChanged();

  private:
    ShortcutDisplayService();
    ~ShortcutDisplayService() override;

    class Impl;
    std::unique_ptr<Impl> m_impl;
};

[[nodiscard]] QString formatShortcutDisplayText(const ShortcutBinding& binding);
[[nodiscard]] QString formatShortcutListDisplayText(const ShortcutBindingList& bindings);

} // namespace snow_shot::shortcuts

#endif // SNOW_SHOT_SHORTCUTS_SHORTCUTDISPLAYSERVICE_H
