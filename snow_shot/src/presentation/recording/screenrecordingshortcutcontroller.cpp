#include "snow_shot/presentation/canvashistoryshortcuts.h"
#include "snow_shot/presentation/screenrecordingshortcutcontroller.h"
#include "recordingaudiogainpopover.h"
#include "widgets/popover.h"
#include "widgets/select.h"

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
    m_shortcutManager.addScopeWindow(toolbar.autoHideControlWindow());
    connect(toolbar.palette(), &ScreenshotToolPalette::materializedScope, this,
            [this](QWidget* scope) { m_shortcutManager.addScopeWindow(scope); });

    const auto drawingShortcuts = snow_shot::storage::DrawingShortcutSettings().allShortcuts();
    for (auto tool = drawingShortcuts.cbegin(); tool != drawingShortcuts.cend(); ++tool) {
        ShortcutManager::Binding binding;
        binding.id = QStringLiteral("recording.drawing.") + tool.key();
        binding.priority = ShortcutManager::StandardPriority::DrawingShortcut;
        binding.canActivate = [this, toolId = tool.key()](const auto& context) {
            return canActivate(context) && !m_area->drawingBlocked() &&
                   m_toolbar->palette()->canActivateDrawingShortcut(toolId);
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
        const bool areaHistory = action.key() == QStringLiteral("previous_recording_area") ||
                                 action.key() == QStringLiteral("next_recording_area");
        ShortcutManager::Binding binding;
        binding.id = QStringLiteral("recording.control.") + action.key();
        if (action.key() == QStringLiteral("end_recording")) {
            binding.activationTrigger = ShortcutManager::Binding::ActivationTrigger::Release;
        }
        binding.priority = ShortcutManager::StandardPriority::WindowCommand;
        binding.canActivate = [this, actionId = action.key(), areaHistory](const auto& context) {
            if (!canActivate(context))
                return false;
            if (areaHistory) {
                return m_area->canRestoreRecordingRegion() &&
                       m_toolbar->palette()->recordingExportSettingsVisible() &&
                       m_canNavigateAreaHistory && m_canNavigateAreaHistory();
            }
            return m_toolbar->palette()->canActivateRecordingShortcut(actionId);
        };
        binding.activate = [this, actionId = action.key(), areaHistory](const auto&) {
            if (areaHistory) {
                if (m_navigateAreaHistory)
                    m_navigateAreaHistory(actionId == QStringLiteral("previous_recording_area"));
                // Empty history and endpoints still consume this command, rather than
                // falling through to a drawing tool that shares the configured key.
                return true;
            }
            return m_toolbar->palette()->activateRecordingShortcut(actionId);
        };
        auto tapBinding = binding;
        tapBinding.id += QStringLiteral(".tap");
        tapBinding.activationTrigger = ShortcutManager::Binding::ActivationTrigger::Tap;
        m_recordingTapBindings.insert(action.key(),
                                      m_shortcutManager.addBinding(this, std::move(tapBinding)));
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

void ScreenRecordingShortcutController::setAreaHistoryActions(std::function<bool()> canNavigate,
                                                              std::function<void(bool)> navigate) {
    m_canNavigateAreaHistory = std::move(canNavigate);
    m_navigateAreaHistory = std::move(navigate);
}

bool ScreenRecordingShortcutController::canActivate(
    const ShortcutManager::ActivationContext& context) const {
    const auto* receiver = qobject_cast<QWidget*>(context.receiver);
    if (m_toolbar && receiver) {
        auto* trim = m_toolbar->palette()->recordingTrimPanel();
        if (auto* speed = trim ? trim->findChild<adqt::widgets::AdSelect*>(
                                     QStringLiteral("screenRecordingTrimSpeed"))
                               : nullptr;
            speed) {
            if (speed->popupVisible())
                return false;
            const bool speedFocused =
                context.focusWidget == speed ||
                (context.focusWidget && speed->isAncestorOf(context.focusWidget));
            if (speedFocused && context.event) {
                switch (context.event->key()) {
                case Qt::Key_Up:
                case Qt::Key_Down:
                case Qt::Key_Home:
                case Qt::Key_End:
                case Qt::Key_PageUp:
                case Qt::Key_PageDown:
                case Qt::Key_Enter:
                case Qt::Key_Return:
                case Qt::Key_Space:
                case Qt::Key_F4:
                    return false;
                default:
                    break;
                }
            }
        }
        for (bool microphone : {false, true}) {
            auto* popup = m_toolbar->palette()->recordingAudioGainPopover(microphone);
            if (!popup || !popup->popover()->isVisible())
                continue;
            const QWidget* content = popup->popover()->contentWidget();
            if ((content && (content == receiver || content->isAncestorOf(receiver))) ||
                (context.event && context.event->key() == Qt::Key_Escape))
                return false;
        }
        auto* followOptions = m_toolbar->palette()->recordingFollowMousePopover();
        if (followOptions && receiver == followOptions->sourceWidget() && context.event &&
            context.event->key() == Qt::Key_Space && context.event->modifiers() == Qt::NoModifier)
            return false;
        for (const auto* popup :
             {m_toolbar->palette()->recordingKeyboardPopover(), followOptions}) {
            if (!popup)
                continue;
            const bool opensOptions = receiver == popup->sourceWidget() && context.event &&
                                      context.event->key() == Qt::Key_Down &&
                                      (context.event->modifiers() == Qt::NoModifier ||
                                       context.event->modifiers() == Qt::AltModifier);
            if (opensOptions)
                return false;
            const QWidget* content = popup->contentWidget();
            const bool optionsFocused =
                content && context.focusWidget &&
                (content == context.focusWidget || content->isAncestorOf(context.focusWidget));
            const QWidget* surface = popup->surfaceWidget();
            const bool optionsReceiver =
                (content && (content == receiver || content->isAncestorOf(receiver))) ||
                (surface && (surface == receiver || surface->isAncestorOf(receiver)));
            if (popup->isVisible() && (optionsFocused || optionsReceiver ||
                                       (context.event && context.event->key() == Qt::Key_Escape)))
                return false;
        }
    }
    return m_area != nullptr && m_toolbar != nullptr && m_area->isVisible() &&
           m_toolbar->recordingUiPresented() && receiver != nullptr && receiver->isVisible() &&
           !ShortcutManager::focusAcceptsTextInput(context.focusWidget) &&
           !m_area->canvas()->hasActiveTextEditing();
}

void ScreenRecordingShortcutController::reloadConfiguredShortcuts() {
    const snow_shot::storage::ScreenRecordingShortcutSettings recording;
    for (auto binding = m_recordingBindings.cbegin(); binding != m_recordingBindings.cend();
         ++binding) {
        snow_shot::shortcuts::ShortcutBindingList presses;
        snow_shot::shortcuts::ShortcutBindingList taps;
        for (const auto& shortcut : recording.shortcuts(binding.key())) {
            (shortcut.portableText == QStringLiteral("Shift") ? taps : presses).append(shortcut);
        }
        static_cast<void>(m_shortcutManager.setShortcuts(binding.value(), presses));
        static_cast<void>(
            m_shortcutManager.setShortcuts(m_recordingTapBindings.value(binding.key()), taps));
    }
    const snow_shot::storage::DrawingShortcutSettings drawing;
    for (auto binding = m_drawingBindings.cbegin(); binding != m_drawingBindings.cend();
         ++binding) {
        static_cast<void>(
            m_shortcutManager.setShortcuts(binding.value(), drawing.shortcuts(binding.key())));
    }
}
