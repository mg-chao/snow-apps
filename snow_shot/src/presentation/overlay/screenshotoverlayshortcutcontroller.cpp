#include "snow_shot/presentation/screenshotoverlayshortcutcontroller.h"

#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QKeyEvent>
#include <QMap>

#include <utility>

namespace {
using ShortcutManager = snow_shot::presentation::WindowShortcutManager;
using BindingHandle = ShortcutManager::BindingHandle;

bool recognitionTool(ScreenshotActiveTool tool) {
    return tool == ScreenshotActiveTool::Ocr || tool == ScreenshotActiveTool::Table ||
           tool == ScreenshotActiveTool::Qr;
}

QList<QKeyCombination> anyModifierCombinations(Qt::Key key) {
    QList<QKeyCombination> combinations;
    constexpr Qt::KeyboardModifier modifiers[] = {
        Qt::ShiftModifier,
        Qt::ControlModifier,
        Qt::AltModifier,
        Qt::MetaModifier,
        Qt::KeypadModifier,
    };
    constexpr int combinationCount = 1 << 5;
    combinations.reserve(combinationCount);
    for (int mask = 0; mask < combinationCount; ++mask) {
        Qt::KeyboardModifiers combination;
        for (int index = 0; index < 5; ++index) {
            if ((mask & (1 << index)) != 0) {
                combination |= modifiers[index];
            }
        }
        combinations.push_back(QKeyCombination(combination, key));
    }
    return combinations;
}

ShortcutManager::Binding fixedBinding(QString id, QList<QKeyCombination> combinations,
                                      int priority,
                                      std::function<bool()> canActivate,
                                      std::function<bool()> activate) {
    ShortcutManager::Binding binding;
    binding.id = std::move(id);
    binding.keyCombinations = std::move(combinations);
    binding.priority = priority;
    binding.canActivate = [canActivate = std::move(canActivate)](const auto&) {
        return !canActivate || canActivate();
    };
    binding.activate = [activate = std::move(activate)](const auto&) {
        return activate && activate();
    };
    return binding;
}

} // namespace

struct ScreenshotOverlayShortcutController::Impl {
    Impl(ScreenshotOverlayShortcutController& owner, ShortcutManager& manager,
         ScreenshotOverlayInputHandler& handler, ScreenshotInteractionState& interactionState,
         ScreenshotIntelligentSelectionModel& intelligent,
         ScreenshotOverlayInputActions inputActions)
        : q(owner), shortcutManager(manager), inputHandler(handler), interaction(interactionState),
          intelligentSelection(intelligent), actions(std::move(inputActions)) {
        registerFixedBindings();
        registerConfiguredBindings();
        reloadConfiguredShortcuts();

        auto& storage = snow_shot::storage::ApplicationStorage::instance();
        if (storage.isInitialized()) {
            QObject::connect(
                &storage.configuration(), &snow_shot::storage::ConfigurationStore::valueChanged,
                &q, [this](const QString& key, const QJsonValue&) {
                    if (key.startsWith(QStringLiteral("screenshot_shortcuts/")) ||
                        key.startsWith(QStringLiteral("drawing_shortcuts/"))) {
                        reloadConfiguredShortcuts();
                    }
                });
        }
    }

    [[nodiscard]] bool localShortcutState() const {
        return (interaction.movingSelection() || interaction.modifyingSelection() ||
                interaction.editing()) &&
               !recognitionTool(interaction.activeTool()) &&
               actions.localShortcutInputAllowed();
    }

    [[nodiscard]] bool toolbarToolShortcutState() const {
        return actions.mainToolbarVisible() && localShortcutState();
    }

    [[nodiscard]] bool screenshotShortcutState() const {
        return (interaction.manualSelecting() || interaction.movingSelection() ||
                interaction.editing()) &&
               actions.localShortcutInputAllowed();
    }

    [[nodiscard]] bool cursorMovementShortcutState() const {
        if (!actions.physicalCursorMovementAvailable()) {
            return false;
        }
        if (inputHandler.canvasColorSamplingActive()) {
            return true;
        }
        return interaction.cursorMovementEnabled() && actions.localShortcutInputAllowed();
    }

    bool navigateHistory(bool previous) {
        if (interaction.manualSelecting()) {
            if (interaction.dragging()) {
                interaction.cancelDrag();
            }
            inputHandler.confirmSelection();
        }
        inputHandler.resetTransientShortcuts();
        actions.pauseIntelligentSelection();
        intelligentSelection.clearPress();
        return previous ? actions.navigateHistoryPrevious() : actions.navigateHistoryNext();
    }

    void registerFixedBindings() {
        QList<QKeyCombination> confirmationKeys = anyModifierCombinations(Qt::Key_Return);
        confirmationKeys.append(anyModifierCombinations(Qt::Key_Enter));
        static_cast<void>(shortcutManager.addBinding(
            &q, fixedBinding(QStringLiteral("screenshot.confirm_selection"),
                             std::move(confirmationKeys),
                             ShortcutManager::StandardPriority::WindowCommand,
                             [this]() {
                                  return !recognitionTool(interaction.activeTool()) &&
                                         interaction.selecting() &&
                                         !interaction.dragging() &&
                                         actions.localShortcutInputAllowed();
                             },
                             [this]() {
                                 inputHandler.confirmSelection();
                                 return true;
                             })));

        static_cast<void>(shortcutManager.addBinding(
            &q, fixedBinding(QStringLiteral("screenshot.cycle_color_format"),
                             {QKeyCombination(Qt::ShiftModifier, Qt::Key_Shift)},
                             ShortcutManager::StandardPriority::ContextualFallback,
                             [this]() {
                                 return interaction.moveToolActive() &&
                                        !interaction.dragging() &&
                                        actions.localShortcutInputAllowed();
                             },
                             [this]() { return actions.cycleColorPickerFormat(); })));
    }

    void registerConfiguredBindings() {
        const QStringList screenshotIds = {
            QStringLiteral("move_tool"),
            QStringLiteral("move_cursor_up"),
            QStringLiteral("move_cursor_down"),
            QStringLiteral("move_cursor_left"),
            QStringLiteral("move_cursor_right"),
            QStringLiteral("move_entire_selection"),
            QStringLiteral("keep_selection_width_and_height_consistent"),
            QStringLiteral("switch_selection_between_window_and_window_sub_element"),
            QStringLiteral("previous_screenshot_history"),
            QStringLiteral("next_screenshot_history"),
            QStringLiteral("select_previously_selected_area"),
            QStringLiteral("copy_color"),
            QStringLiteral("table_recognition"),
            QStringLiteral("qr_code_recognition"),
            QStringLiteral("video_recording"),
            QStringLiteral("text_recognition"),
            QStringLiteral("text_translation"),
            QStringLiteral("scrolling_screenshot"),
            QStringLiteral("save_as_file"),
            QStringLiteral("pin_to_screen"),
            QStringLiteral("cancel_screenshot"),
            QStringLiteral("copy_to_clipboard"),
            QStringLiteral("undo"),
            QStringLiteral("redo"),
        };
        for (const QString& actionId : screenshotIds) {
            ShortcutManager::Binding binding;
            binding.id = QStringLiteral("screenshot.configured.") + actionId;
            binding.priority = ShortcutManager::StandardPriority::ScreenshotShortcut;
            binding.autoRepeat = actionId.startsWith(QStringLiteral("move_cursor_"));
            binding.canActivate = [this, actionId](const auto&) {
                if (actionId == QStringLiteral("cancel_screenshot") ||
                    actionId == QStringLiteral("copy_to_clipboard")) {
                    return !recognitionTool(interaction.activeTool()) &&
                           actions.localShortcutInputAllowed();
                }
                if (actionId == QStringLiteral("undo") || actionId == QStringLiteral("redo")) {
                    return actions.mainToolbarVisible() && screenshotShortcutState();
                }
                if (actionId == QStringLiteral("table_recognition") ||
                    actionId == QStringLiteral("qr_code_recognition") ||
                    actionId == QStringLiteral("text_recognition") ||
                    actionId == QStringLiteral("text_translation") ||
                    actionId == QStringLiteral("video_recording") ||
                    actionId == QStringLiteral("scrolling_screenshot") ||
                    actionId == QStringLiteral("save_as_file") ||
                    actionId == QStringLiteral("pin_to_screen")) {
                    return toolbarToolShortcutState();
                }
                if (actionId == QStringLiteral("previous_screenshot_history") ||
                    actionId == QStringLiteral("next_screenshot_history")) {
                    return !recognitionTool(interaction.activeTool()) &&
                           (interaction.selecting() || interaction.movingSelection()) &&
                           !interaction.modifyingSelection() &&
                           actions.localShortcutInputAllowed();
                }
                if (actionId == QStringLiteral(
                        "switch_selection_between_window_and_window_sub_element")) {
                    return interaction.intelligentSelecting() &&
                           intelligentSelection.smartSelectionEnabled() &&
                           actions.localShortcutInputAllowed();
                }
                if (actionId == QStringLiteral("move_entire_selection")) {
                    return (interaction.movingSelection() || interaction.modifyingSelection() ||
                            interaction.manualSelecting()) &&
                           actions.localShortcutInputAllowed();
                }
                if (actionId == QStringLiteral("keep_selection_width_and_height_consistent")) {
                    return (interaction.movingSelection() || interaction.modifyingSelection() ||
                            interaction.manualSelecting() || interaction.editing()) &&
                           !recognitionTool(interaction.activeTool()) &&
                           actions.localShortcutInputAllowed();
                }
                if (actionId == QStringLiteral("select_previously_selected_area")) {
                    return interaction.moveToolActive() && !interaction.dragging() &&
                           !interaction.scrollingCapture() &&
                           actions.localShortcutInputAllowed();
                }
                if (actionId == QStringLiteral("copy_color")) {
                    return interaction.moveToolActive() &&
                           actions.localShortcutInputAllowed();
                }
                if (actionId == QStringLiteral("move_tool")) {
                    return actions.mainToolbarVisible() && screenshotShortcutState();
                }
                if (actionId.startsWith(QStringLiteral("move_cursor_"))) {
                    return cursorMovementShortcutState();
                }
                return false;
            };
            if (actionId.startsWith(QStringLiteral("move_cursor_"))) {
                binding.canActivateOutsideScope = [this](const auto&) {
                    return inputHandler.canvasColorSamplingActive();
                };
            }
            binding.activate = [this, actionId](const auto& context) {
                if (actionId == QStringLiteral("move_tool")) {
                    return actions.activateMoveTool();
                }
                if (actionId == QStringLiteral("move_cursor_up")) {
                    return actions.moveCursorOnePixel(
                        snow_shot::platform::PhysicalCursorDirection::Up);
                }
                if (actionId == QStringLiteral("move_cursor_down")) {
                    return actions.moveCursorOnePixel(
                        snow_shot::platform::PhysicalCursorDirection::Down);
                }
                if (actionId == QStringLiteral("move_cursor_left")) {
                    return actions.moveCursorOnePixel(
                        snow_shot::platform::PhysicalCursorDirection::Left);
                }
                if (actionId == QStringLiteral("move_cursor_right")) {
                    return actions.moveCursorOnePixel(
                        snow_shot::platform::PhysicalCursorDirection::Right);
                }
                if (actionId == QStringLiteral("move_entire_selection")) {
                    return inputHandler.activateMoveEntireSelectionShortcut();
                }
                if (actionId == QStringLiteral("keep_selection_width_and_height_consistent")) {
                    const Qt::KeyboardModifiers eventModifiers =
                        context.event != nullptr ? context.event->modifiers() : Qt::NoModifier;
                    const bool plainShiftColorFormatFallback =
                        context.event != nullptr && context.event->key() == Qt::Key_Shift &&
                        (eventModifiers == Qt::NoModifier ||
                         eventModifiers == Qt::ShiftModifier) &&
                        interaction.moveToolActive();
                    return inputHandler.activateKeepSelectionAspectRatioShortcut(
                        plainShiftColorFormatFallback);
                }
                if (actionId == QStringLiteral(
                        "switch_selection_between_window_and_window_sub_element")) {
                    return inputHandler.toggleIntelligentSelectionTargetShortcut();
                }
                if (actionId == QStringLiteral("previous_screenshot_history")) {
                    return navigateHistory(true);
                }
                if (actionId == QStringLiteral("next_screenshot_history")) {
                    return navigateHistory(false);
                }
                if (actionId == QStringLiteral("select_previously_selected_area")) {
                    const bool selected = actions.selectPreviousSelection();
                    if (selected) {
                        actions.pauseIntelligentSelection();
                        intelligentSelection.clearPress();
                    }
                    return selected;
                }
                if (actionId == QStringLiteral("copy_color")) {
                    return actions.copyColorPickerColorToClipboard();
                }
                if (actionId == QStringLiteral("table_recognition")) {
                    return actions.activateTableRecognition();
                }
                if (actionId == QStringLiteral("qr_code_recognition")) {
                    return actions.activateQrRecognition();
                }
                if (actionId == QStringLiteral("text_recognition")) {
                    return actions.activateTextRecognition();
                }
                if (actionId == QStringLiteral("text_translation")) {
                    return actions.activateTextTranslation();
                }
                if (actionId == QStringLiteral("video_recording")) {
                    return actions.startVideoRecording();
                }
                if (actionId == QStringLiteral("scrolling_screenshot")) {
                    return actions.startScrollingScreenshot();
                }
                if (actionId == QStringLiteral("save_as_file")) {
                    return actions.saveAsFile();
                }
                if (actionId == QStringLiteral("pin_to_screen")) {
                    return actions.pinSelectionToScreen();
                }
                if (actionId == QStringLiteral("cancel_screenshot")) {
                    actions.cancelCapture();
                    return true;
                }
                if (actionId == QStringLiteral("copy_to_clipboard")) {
                    actions.copySelectionToClipboard();
                    return true;
                }
                if (actionId == QStringLiteral("undo")) {
                    return actions.undo();
                }
                if (actionId == QStringLiteral("redo")) {
                    return actions.redo();
                }
                return false;
            };
            if (actionId == QStringLiteral("move_entire_selection")) {
                binding.allowedAdditionalModifiers = Qt::ShiftModifier;
                binding.release = [this](const auto&) {
                    return inputHandler.releaseMoveEntireSelectionShortcut();
                };
            } else if (actionId ==
                       QStringLiteral("keep_selection_width_and_height_consistent")) {
                binding.release = [this](const auto&) {
                    return inputHandler.releaseKeepSelectionAspectRatioShortcut();
                };
            }
            screenshotBindings.insert(actionId,
                                      shortcutManager.addBinding(&q, std::move(binding)));
        }

        const auto drawingShortcuts =
            snow_shot::storage::DrawingShortcutSettings().allShortcuts();
        for (auto tool = drawingShortcuts.cbegin(); tool != drawingShortcuts.cend(); ++tool) {
            ShortcutManager::Binding binding;
            binding.id = QStringLiteral("drawing.configured.") + tool.key();
            binding.priority = ShortcutManager::StandardPriority::DrawingShortcut;
            binding.canActivate = [this](const auto&) { return toolbarToolShortcutState(); };
            binding.activate = [this, toolId = tool.key()](const auto&) {
                return actions.activateDrawingShortcut(toolId);
            };
            drawingBindings.insert(tool.key(),
                                   shortcutManager.addBinding(&q, std::move(binding)));
        }
    }

    void reloadConfiguredShortcuts() {
        const snow_shot::storage::ScreenshotShortcutSettings screenshotSettings;
        for (auto binding = screenshotBindings.cbegin(); binding != screenshotBindings.cend();
             ++binding) {
            static_cast<void>(shortcutManager.setKeyCombinations(
                binding.value(), ShortcutManager::keyCombinationsFromPortableText(
                                     screenshotSettings.shortcuts(binding.key()))));
        }

        const snow_shot::storage::DrawingShortcutSettings drawingSettings;
        for (auto binding = drawingBindings.cbegin(); binding != drawingBindings.cend();
             ++binding) {
            static_cast<void>(shortcutManager.setKeyCombinations(
                binding.value(), ShortcutManager::keyCombinationsFromPortableText(
                                     drawingSettings.shortcuts(binding.key()))));
        }
    }

    ScreenshotOverlayShortcutController& q;
    ShortcutManager& shortcutManager;
    ScreenshotOverlayInputHandler& inputHandler;
    ScreenshotInteractionState& interaction;
    ScreenshotIntelligentSelectionModel& intelligentSelection;
    ScreenshotOverlayInputActions actions;
    QMap<QString, BindingHandle> screenshotBindings;
    QMap<QString, BindingHandle> drawingBindings;
};

ScreenshotOverlayShortcutController::ScreenshotOverlayShortcutController(
    ShortcutManager& shortcutManager, ScreenshotOverlayInputHandler& inputHandler,
    ScreenshotInteractionState& interaction,
    ScreenshotIntelligentSelectionModel& intelligentSelection,
    ScreenshotOverlayInputActions actions, QObject* parent)
    : QObject(parent),
      m_impl(std::make_unique<Impl>(*this, shortcutManager, inputHandler, interaction,
                                    intelligentSelection, std::move(actions))) {}

ScreenshotOverlayShortcutController::~ScreenshotOverlayShortcutController() = default;

void ScreenshotOverlayShortcutController::reloadConfiguredShortcuts() {
    m_impl->reloadConfiguredShortcuts();
}
