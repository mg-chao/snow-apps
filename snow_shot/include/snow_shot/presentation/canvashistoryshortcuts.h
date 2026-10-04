#ifndef SNOW_SHOT_PRESENTATION_CANVASHISTORYSHORTCUTS_H
#define SNOW_SHOT_PRESENTATION_CANVASHISTORYSHORTCUTS_H

#include "snow_shot/presentation/windowshortcutmanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QMap>
#include <QPointer>
#include <utility>

namespace snow_shot::presentation {

// Every canvas host registers history as a pair. The host supplies its interaction
// policy and command path; this object owns bindings and live configuration updates.
class CanvasHistoryShortcuts final : public QObject {
  public:
    using Context = WindowShortcutManager::ActivationContext;
    using Guard = std::function<bool(const Context&)>;
    using Command = std::function<bool(const QString&)>;

    CanvasHistoryShortcuts(WindowShortcutManager& manager, QObject* owner, Guard allowed,
                           Command activate)
        : QObject(owner), m_manager(&manager) {
        for (const QString& action : {QStringLiteral("undo"), QStringLiteral("redo")}) {
            WindowShortcutManager::Binding binding;
            binding.id = QStringLiteral("canvas.history.") + action;
            binding.priority = WindowShortcutManager::StandardPriority::ScreenshotShortcut;
            binding.canActivate = [allowed](const Context& context) {
                return !WindowShortcutManager::focusAcceptsTextInput(context.focusWidget) &&
                       allowed(context);
            };
            binding.activate = [activate, action](const Context&) { return activate(action); };
            m_bindings.insert(action, manager.addBinding(this, std::move(binding)));
        }
        reload();
        auto& storage = storage::ApplicationStorage::instance();
        if (storage.isInitialized()) {
            connect(&storage.configuration(), &storage::ConfigurationStore::valueChanged, this,
                    [this](const QString& key, const QJsonValue&) {
                        if (key == QStringLiteral("screenshot_shortcuts/undo") ||
                            key == QStringLiteral("screenshot_shortcuts/redo")) {
                            reload();
                        }
                    });
        }
    }

  private:
    void reload() {
        if (!m_manager)
            return;
        const storage::ScreenshotShortcutSettings settings;
        for (auto it = m_bindings.cbegin(); it != m_bindings.cend(); ++it)
            static_cast<void>(m_manager->setShortcuts(it.value(), settings.shortcuts(it.key())));
    }

    QPointer<WindowShortcutManager> m_manager;
    QMap<QString, WindowShortcutManager::BindingHandle> m_bindings;
};

} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_CANVASHISTORYSHORTCUTS_H
