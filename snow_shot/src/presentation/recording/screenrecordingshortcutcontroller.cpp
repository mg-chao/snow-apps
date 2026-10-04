#include "snow_shot/presentation/canvashistoryshortcuts.h"
#include "snow_shot/presentation/screenrecordingshortcutcontroller.h"
#include "recordingaudiogainpopover.h"
#include "widgets/popover.h"

#include <QKeyEvent>

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
        binding.canActivate = [this](const auto& context) {
            return canActivate(context) && !m_area->drawingBlocked();
        };
        binding.activate = [this, toolId = tool.key()](const auto&) {
            return m_toolbar->palette()->activateDrawingShortcut(toolId);
        };
        m_drawingBindings.insert(tool.key(),
                                 m_shortcutManager.addBinding(this, std::move(binding)));
    }
    new snow_shot::presentation::CanvasHistoryShortcuts(
        m_shortcutManager, this,
        [this](const auto& context) { return canActivate(context) && !m_area->drawingBlocked(); },
        [this](const QString& action) {
            return m_toolbar->palette()->activateScreenshotShortcut(action);
        });
    const auto recordingShortcuts =
        snow_shot::storage::ScreenRecordingShortcutSettings().allShortcuts();
    for (auto action = recordingShortcuts.cbegin(); action != recordingShortcuts.cend(); ++action) {
        ShortcutManager::Binding binding;
        binding.id = QStringLiteral("recording.control.") + action.key();
        if (action.key() == QStringLiteral("end_recording")) {
            binding.activationTrigger = ShortcutManager::Binding::ActivationTrigger::Release;
        }
        binding.priority = ShortcutManager::StandardPriority::WindowCommand;
        binding.canActivate = [this, actionId = action.key()](const auto& context) {
            return canActivate(context) &&
                   m_toolbar->palette()->canActivateRecordingShortcut(actionId);
        };
        binding.activate = [this, actionId = action.key()](const auto&) {
            return m_toolbar->palette()->activateRecordingShortcut(actionId);
        };
        m_recordingBindings.insert(action.key(),
                                   m_shortcutManager.addBinding(this, std::move(binding)));
    }
    reloadConfiguredShortcuts();
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    connect(&storage.configuration(), &snow_shot::storage::ConfigurationStore::valueChanged, this,
            [this](const QString& key, const QJsonValue&) {
                if (key.startsWith(QStringLiteral("drawing_shortcuts/")) ||
                    key.startsWith(QStringLiteral("screen_recording_shortcuts/"))) {
                    reloadConfiguredShortcuts();
                }
            });
}

bool ScreenRecordingShortcutController::canActivate(
    const ShortcutManager::ActivationContext& context) const {
    const auto* receiver = qobject_cast<QWidget*>(context.receiver);
    if (m_toolbar && receiver) {
        for (bool microphone : {false, true}) {
            auto* popup = m_toolbar->palette()->recordingAudioGainPopover(microphone);
            if (!popup || !popup->popover()->isVisible())
                continue;
            const QWidget* content = popup->popover()->contentWidget();
            if ((content && (content == receiver || content->isAncestorOf(receiver))) ||
                (context.event && context.event->key() == Qt::Key_Escape))
                return false;
        }
    }
    return m_area != nullptr && m_toolbar != nullptr && m_area->isVisible() &&
           m_toolbar->isVisible() && receiver != nullptr && receiver->isVisible() &&
           !ShortcutManager::focusAcceptsTextInput(context.focusWidget) &&
           !m_area->canvas()->hasActiveTextEditing();
}

void ScreenRecordingShortcutController::reloadConfiguredShortcuts() {
    const snow_shot::storage::ScreenRecordingShortcutSettings recording;
    for (auto binding = m_recordingBindings.cbegin(); binding != m_recordingBindings.cend();
         ++binding) {
        static_cast<void>(
            m_shortcutManager.setShortcuts(binding.value(), recording.shortcuts(binding.key())));
    }
    const snow_shot::storage::DrawingShortcutSettings drawing;
    for (auto binding = m_drawingBindings.cbegin(); binding != m_drawingBindings.cend();
         ++binding) {
        static_cast<void>(
            m_shortcutManager.setShortcuts(binding.value(), drawing.shortcuts(binding.key())));
    }
}
