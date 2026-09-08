#include "snow_shot/presentation/screenrecordingshortcutcontroller.h"

#include "snow_shot/presentation/screenrecordingareawindow.h"
#include "snow_shot/presentation/screenrecordingtoolbarwindow.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <utility>

ScreenRecordingShortcutController::ScreenRecordingShortcutController(
    ScreenRecordingAreaWindow& area, ScreenRecordingToolbarWindow& toolbar, QObject* parent)
    : QObject(parent), m_area(&area), m_toolbar(&toolbar), m_shortcutManager(this) {
    m_shortcutManager.addScopeWindow(&area);
    m_shortcutManager.addScopeWindow(&toolbar);
    connect(toolbar.palette(), &ScreenshotToolPalette::materializedScope, this,
            [this](QWidget* scope) { m_shortcutManager.addScopeWindow(scope); });

    const auto drawingShortcuts = snow_shot::storage::DrawingShortcutSettings().allShortcuts();
    for (auto tool = drawingShortcuts.cbegin(); tool != drawingShortcuts.cend(); ++tool) {
        ShortcutManager::Binding binding;
        binding.id = QStringLiteral("recording.drawing.") + tool.key();
        binding.priority = ShortcutManager::StandardPriority::DrawingShortcut;
        binding.canActivate = [this](const auto& context) { return canActivate(context); };
        binding.activate = [this, toolId = tool.key()](const auto&) {
            return m_toolbar->palette()->activateDrawingShortcut(toolId);
        };
        m_drawingBindings.insert(tool.key(),
                                 m_shortcutManager.addBinding(this, std::move(binding)));
    }
    for (const QString& action : {QStringLiteral("undo"), QStringLiteral("redo")}) {
        const bool undo = action == QStringLiteral("undo");
        ShortcutManager::Binding binding;
        binding.id = QStringLiteral("recording.") + action;
        binding.priority = ShortcutManager::StandardPriority::ScreenshotShortcut;
        binding.canActivate = [this, undo](const auto& context) {
            if (!canActivate(context)) {
                return false;
            }
            const auto history = m_area->canvas()->canvasHistoryState();
            return undo ? history.canUndo : history.canRedo;
        };
        binding.activate = [this, undo](const auto&) {
            if (undo) {
                emit m_toolbar->palette()->undoRequested();
            } else {
                emit m_toolbar->palette()->redoRequested();
            }
            return true;
        };
        m_historyBindings.insert(action, m_shortcutManager.addBinding(this, std::move(binding)));
    }
    reloadConfiguredShortcuts();
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    connect(&storage.configuration(), &snow_shot::storage::ConfigurationStore::valueChanged, this,
            [this](const QString& key, const QJsonValue&) {
                if (key.startsWith(QStringLiteral("drawing_shortcuts/")) ||
                    key == QStringLiteral("screenshot_shortcuts/undo") ||
                    key == QStringLiteral("screenshot_shortcuts/redo")) {
                    reloadConfiguredShortcuts();
                }
            });
}

bool ScreenRecordingShortcutController::canActivate(
    const ShortcutManager::ActivationContext& context) const {
    const auto* receiver = qobject_cast<QWidget*>(context.receiver);
    return m_area != nullptr && m_toolbar != nullptr && m_area->isVisible() &&
           m_toolbar->isVisible() && receiver != nullptr && receiver->isVisible() &&
           !m_area->drawingBlocked() &&
           !ShortcutManager::focusAcceptsTextInput(context.focusWidget) &&
           !m_area->canvas()->hasActiveTextEditing();
}

void ScreenRecordingShortcutController::reloadConfiguredShortcuts() {
    const snow_shot::storage::DrawingShortcutSettings drawing;
    for (auto binding = m_drawingBindings.cbegin(); binding != m_drawingBindings.cend();
         ++binding) {
        static_cast<void>(m_shortcutManager.setKeyCombinations(
            binding.value(),
            ShortcutManager::keyCombinationsFromPortableText(drawing.shortcuts(binding.key()))));
    }
    const snow_shot::storage::ScreenshotShortcutSettings history;
    for (auto binding = m_historyBindings.cbegin(); binding != m_historyBindings.cend();
         ++binding) {
        static_cast<void>(m_shortcutManager.setKeyCombinations(
            binding.value(),
            ShortcutManager::keyCombinationsFromPortableText(history.shortcuts(binding.key()))));
    }
}
