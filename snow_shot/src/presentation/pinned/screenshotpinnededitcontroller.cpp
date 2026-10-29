#include "snow_shot/presentation/screenshotdrawingtoolbaroptions.h"
#include "snow_shot/presentation/screenshotcanvasoperations.h"
#include "snow_shot/presentation/screenshotcanvastools.h"
#include "snow_shot/presentation/canvashistoryshortcuts.h"
#include "snow_shot/shortcuts/shortcutbinding.h"
#include "snow_shot/presentation/pinnedgeometry.h"
#include "snow_shot/presentation/screenshotautofiltercontroller.h"
#include "snow_shot/presentation/screenshotpinnededitcontroller.h"
#include <utility>

#include "snow_shot/presentation/screenshotcanvascolorsamplerwindow.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshotstylebinding.h"
#include "snow_shot/presentation/screenshotfloatingtoolpalettewindow.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/screenshottoolpalettehost.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/settingsadapters.h"

#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include "widgets/color_picker.h"
#include "widgets/message.h"
#include "widgets/select.h"

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTabletEvent>
#include <QPointer>
#include <QScreen>
#include <QScopeGuard>
#include <QTimer>
#include <QWheelEvent>
#include <QWindow>

namespace {
constexpr int kToolbarGap = 4;

} // namespace

ScreenshotPinnedEditController::ScreenshotPinnedEditController(
    ScreenshotPinnedWindow& pinnedWindow, SnowCanvasWidget& canvas,
    snow_shot::presentation::WindowShortcutManager& shortcutManager, QObject* parent)
    : QObject(parent), m_pinnedWindow(pinnedWindow), m_canvas(canvas),
      m_shortcutManager(shortcutManager) {
    m_autoFilterController = std::make_unique<ScreenshotAutoFilterController>(
        [this]() { return m_pinnedWindow.autoFilterSourceBounds(); },
        [this](ScreenshotAutoFilterController::ImageCompletion completion) {
            m_pinnedWindow.requestAutoFilterSource(std::move(completion));
        },
        this);
    m_autoFilterController->attachCanvas(&m_canvas);
    connect(m_autoFilterController.get(), &ScreenshotAutoFilterController::availabilityChanged,
            this, [this](bool available) {
                m_pinnedWindow.schedulePersistence();
                if (!available || m_automationFilterCategories.isEmpty())
                    return;
                const auto categories = std::exchange(m_automationFilterCategories, {});
                for (const auto& category : categories)
                    m_autoFilterController->fillCategory(category);
                emit automationAutoFilterStateChanged();
            });
    connect(m_autoFilterController.get(), &ScreenshotAutoFilterController::detectionFailed, this,
            [this](const QString& error) {
                m_pinnedWindow.schedulePersistence();
                if (!m_automationFilterCategories.isEmpty()) {
                    m_automationFilterCategories.clear();
                    m_automationFilterError = error;
                    emit automationAutoFilterStateChanged();
                }
            });
    connect(m_autoFilterController.get(), &ScreenshotAutoFilterController::detectionFailed, this,
            [this](const QString& message) {
                adqt::widgets::AdMessageService::error(message, -1, &m_pinnedWindow);
            });
    connect(m_autoFilterController.get(), &ScreenshotAutoFilterController::detectionFinished, this,
            [this](const QString& error) {
                if (m_automationFilterCategories.isEmpty())
                    return;
                m_automationFilterCategories.clear();
                m_automationFilterError = error;
                emit automationAutoFilterStateChanged();
            });
    connect(m_autoFilterController.get(), &ScreenshotAutoFilterController::availabilityChanged,
            this, [this](bool available) {
                if (m_toolbarWindow && m_toolbarWindow->palette()) {
                    m_toolbarWindow->palette()->setAutoFilterAvailable(available);
                }
            });
    m_canvas.installEventFilter(this);
    connect(&m_canvas, &SnowCanvasWidget::angleAdjustmentTargetChanged, this,
            [this]() { m_angleWheelSteps.reset(); });
    connect(&m_canvas, &SnowCanvasWidget::activeToolChanged, this, [this]() {
        m_angleWheelSteps.reset();
        syncPaletteFromCanvasTool();
    });
    connect(&m_canvas, &SnowCanvasWidget::styleToolbarStateChanged, this,
            &ScreenshotPinnedEditController::syncPaletteFromCanvasStyle);
    connect(&m_canvas, &SnowCanvasWidget::historyStateChanged, this, [this]() {
        if (m_toolbarWindow != nullptr) {
            if (ScreenshotToolPalette* toolbar = m_toolbarWindow->palette()) {
                toolbar->setHistoryState(m_canvas.canvasHistoryState());
            }
        }
    });

    new snow_shot::presentation::CanvasHistoryShortcuts(
        m_shortcutManager, this,
        [this](const auto&) {
            return m_editMode && !canvasColorSamplingActive() && !m_canvas.hasActiveTextEditing() &&
                   m_toolbarWindow && m_toolbarWindow->palette();
        },
        [this](const QString& action) {
            return m_toolbarWindow->palette()->activateScreenshotShortcut(action);
        });
    registerDrawingShortcuts();
    reloadDrawingShortcuts();
    registerScreenshotShortcuts();
    reloadScreenshotShortcuts();
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    if (storage.isInitialized()) {
        connect(&storage.configuration(), &snow_shot::storage::ConfigurationStore::valueChanged,
                this, [this](const QString& key, const QJsonValue&) {
                    if (key == QStringLiteral("pin_to_screen/action_tools_layout")) {
                        if (m_toolbarWindow != nullptr && m_toolbarWindow->palette() != nullptr) {
                            m_toolbarWindow->palette()->setActionToolsLayout(
                                snow_shot::storage::ScreenshotToolbarSettings().layout(
                                    snow_shot::storage::ScreenshotToolbarLayoutKind::
                                        PinnedActionTools));
                            updatePlacement();
                        }
                    } else if (key == QStringLiteral("screenshot_toolbar/layout")) {
                        if (m_toolbarWindow != nullptr && m_toolbarWindow->palette() != nullptr) {
                            m_toolbarWindow->palette()->setToolbarLayout(
                                snow_shot::storage::ScreenshotToolbarSettings().layout(
                                    snow_shot::storage::ScreenshotToolbarLayoutKind::DrawingTools));
                            updatePlacement();
                        }
                    } else if (key.startsWith(QStringLiteral("drawing_shortcuts/"))) {
                        reloadDrawingShortcuts();
                    } else if (key.startsWith(QStringLiteral("screenshot_shortcuts/"))) {
                        reloadScreenshotShortcuts();
                    }
                });
    }
}

ScreenshotPinnedEditController::~ScreenshotPinnedEditController() {
    destroyToolbar();
}

bool ScreenshotPinnedEditController::editMode() const {
    return m_editMode;
}

bool ScreenshotPinnedEditController::resizeWindowToolActive() const {
    return m_editMode && m_resizeWindowToolActive;
}

bool ScreenshotPinnedEditController::canvasColorSamplingActive() const {
    return m_canvasColorSampling && m_canvasColorSampling->active();
}

void ScreenshotPinnedEditController::updateCanvasColorSamplingAfterCursorMove(
    const QPoint& physicalPosition) {
    if (!canvasColorSamplingActive()) {
        return;
    }
    if (m_pinnedWindow.currentContentNativeGeometry().contains(physicalPosition)) {
        updateCanvasColorSamplingPreviewAtPhysicalPoint(
            physicalPosition, canvasColorGlobalPositionAt(physicalPosition));
    }
}

bool ScreenshotPinnedEditController::eventFilter(QObject* watched, QEvent* event) {
    if (watched != &m_canvas || event == nullptr || !m_editMode) {
        return QObject::eventFilter(watched, event);
    }
    if (event->type() == QEvent::FocusOut || event->type() == QEvent::Hide) {
        m_tabletFrameGesture = false;
        m_tabletSamplingReleasePending = false;
    }

    if (event->type() == QEvent::TabletPress || event->type() == QEvent::TabletMove ||
        event->type() == QEvent::TabletRelease) {
        auto* tablet = static_cast<QTabletEvent*>(event);
        if (m_tabletSamplingReleasePending) {
            if (event->type() == QEvent::TabletRelease) {
                m_tabletSamplingReleasePending = false;
            }
            event->accept();
            return true;
        }
        const bool outsideContent = !m_pinnedWindow.contentViewRect().contains(
            m_pinnedWindow.windowPositionForEvent(watched, tablet->position()));
        if (m_tabletFrameGesture || (event->type() != QEvent::TabletRelease &&
                                     QWidget::mouseGrabber() != &m_canvas && outsideContent)) {
            m_tabletFrameGesture = event->type() != QEvent::TabletRelease &&
                                   (m_tabletFrameGesture || event->type() == QEvent::TabletPress);
            // Keep Qt's existing mouse fallback for frame resizing, while preventing
            // the native brush handler from receiving this host-owned gesture.
            event->ignore();
            return true;
        }
        const auto type = event->type() == QEvent::TabletPress     ? QEvent::MouseButtonPress
                          : event->type() == QEvent::TabletRelease ? QEvent::MouseButtonRelease
                                                                   : QEvent::MouseMove;
        QMouseEvent pointer(type, tablet->position(), tablet->globalPosition(), tablet->button(),
                            tablet->buttons(), tablet->modifiers(), tablet->pointingDevice());
        const bool samplingPress =
            event->type() == QEvent::TabletPress && canvasColorSamplingActive();
        const QPointer<ScreenshotPinnedEditController> lifetime(this);
        if (eventFilter(watched, &pointer)) {
            if (lifetime) {
                m_tabletSamplingReleasePending = samplingPress;
            }
            event->accept();
            return true;
        }
        return false;
    }

    if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseButtonDblClick || event->type() == QEvent::Wheel) {
        const QPointF local = event->type() == QEvent::Wheel
                                  ? static_cast<QWheelEvent*>(event)->position()
                                  : static_cast<QMouseEvent*>(event)->position();
        if (!m_pinnedWindow.contentViewRect().contains(
                m_pinnedWindow.windowPositionForEvent(watched, local))) {
            // The host owns the reserved frame, including while a color picker
            // or an annotation tool is active. Starting its resize transaction
            // also cancels sampling through beginNativeWindowInteraction().
            return false;
        }
    }

    if (canvasColorSamplingActive()) {
        switch (event->type()) {
        case QEvent::MouseMove: {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            updateCanvasColorSamplingPreviewAtPhysicalPoint(
                canvasColorPhysicalPositionAt(mouseEvent->position()),
                mouseEvent->globalPosition().toPoint());
            mouseEvent->accept();
            return true;
        }
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick: {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::RightButton) {
                cancelCanvasColorSampling();
            } else if (mouseEvent->button() == Qt::LeftButton) {
                static_cast<void>(commitCanvasColorSampleAtPhysicalPoint(
                    canvasColorPhysicalPositionAt(mouseEvent->position())));
            }
            mouseEvent->accept();
            return true;
        }
        case QEvent::MouseButtonRelease:
        case QEvent::Wheel:
            event->accept();
            return true;
        case QEvent::KeyPress: {
            auto* keyEvent = static_cast<QKeyEvent*>(event);
            if (snow_shot::shortcuts::commandKey(*keyEvent) == Qt::Key_Escape) {
                cancelCanvasColorSampling();
                keyEvent->accept();
                return true;
            }
            break;
        }
        default:
            break;
        }
    }

    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::KeyPress)
        m_angleWheelSteps.reset();
    if (event->type() == QEvent::Wheel && !m_canvas.hasActiveTextEditing()) {
        auto* wheelEvent = static_cast<QWheelEvent*>(event);
        ScreenshotToolPalette* palette =
            m_toolbarWindow != nullptr ? m_toolbarWindow->palette() : nullptr;
        const bool handled =
            palette != nullptr && snow_shot::presentation::handleScreenshotStyleWheel(
                                      *palette, m_canvas, *wheelEvent, m_angleWheelSteps);
        if (handled) {
            wheelEvent->accept();
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}

void ScreenshotPinnedEditController::registerDrawingShortcuts() {
    const auto shortcuts = snow_shot::storage::DrawingShortcutSettings().allShortcuts();
    for (auto tool = shortcuts.cbegin(); tool != shortcuts.cend(); ++tool) {
        snow_shot::presentation::WindowShortcutManager::Binding binding;
        binding.id = QStringLiteral("pinned.drawing.") + tool.key();
        binding.priority =
            snow_shot::presentation::WindowShortcutManager::StandardPriority::DrawingShortcut;
        binding.canActivate = [this](const auto& context) {
            return m_editMode &&
                   !snow_shot::presentation::WindowShortcutManager::focusAcceptsTextInput(
                       context.focusWidget) &&
                   !m_canvas.hasActiveTextEditing() && m_toolbarWindow != nullptr &&
                   m_toolbarWindow->palette() != nullptr;
        };
        binding.activate = [this, toolId = tool.key()](const auto&) {
            ScreenshotToolPalette* toolbar =
                m_toolbarWindow != nullptr ? m_toolbarWindow->palette() : nullptr;
            return toolbar != nullptr && toolbar->activateDrawingShortcut(toolId);
        };
        m_drawingShortcutBindings.insert(tool.key(),
                                         m_shortcutManager.addBinding(this, std::move(binding)));
    }
}

void ScreenshotPinnedEditController::reloadDrawingShortcuts() {
    const snow_shot::storage::DrawingShortcutSettings settings;
    for (auto binding = m_drawingShortcutBindings.cbegin();
         binding != m_drawingShortcutBindings.cend(); ++binding) {
        static_cast<void>(
            m_shortcutManager.setShortcuts(binding.value(), settings.shortcuts(binding.key())));
    }
}

void ScreenshotPinnedEditController::registerScreenshotShortcuts() {
    const auto shortcuts = snow_shot::storage::ScreenshotShortcutSettings().allShortcuts();
    for (const QString& actionId :
         {QStringLiteral("table_recognition"), QStringLiteral("qr_code_recognition"),
          QStringLiteral("text_recognition"), QStringLiteral("text_translation")}) {
        if (!shortcuts.contains(actionId)) {
            continue;
        }
        snow_shot::presentation::WindowShortcutManager::Binding binding;
        binding.id = QStringLiteral("pinned.screenshot.") + actionId;
        binding.priority =
            snow_shot::presentation::WindowShortcutManager::StandardPriority::ScreenshotShortcut;
        binding.canActivate = [this](const auto& context) {
            return m_editMode &&
                   !snow_shot::presentation::WindowShortcutManager::focusAcceptsTextInput(
                       context.focusWidget) &&
                   !m_canvas.hasActiveTextEditing() && m_toolbarWindow != nullptr &&
                   m_toolbarWindow->palette() != nullptr;
        };
        binding.activate = [this, actionId](const auto&) {
            return m_toolbarWindow->palette()->activateScreenshotShortcut(actionId);
        };
        m_screenshotShortcutBindings.insert(actionId,
                                            m_shortcutManager.addBinding(this, std::move(binding)));
    }
}

void ScreenshotPinnedEditController::reloadScreenshotShortcuts() {
    const snow_shot::storage::ScreenshotShortcutSettings settings;
    for (auto binding = m_screenshotShortcutBindings.cbegin();
         binding != m_screenshotShortcutBindings.cend(); ++binding) {
        static_cast<void>(
            m_shortcutManager.setShortcuts(binding.value(), settings.shortcuts(binding.key())));
    }
}

ScreenshotFloatingToolPaletteWindow* ScreenshotPinnedEditController::toolbarWindow() const {
    return m_toolbarWindow;
}

ScreenshotToolPaletteHost* ScreenshotPinnedEditController::toolbarHost() const {
    return m_toolbarWindow != nullptr ? m_toolbarWindow->paletteHost() : nullptr;
}

void ScreenshotPinnedEditController::ensureToolbar() {
    if (m_toolbarWindow != nullptr) {
        return;
    }

    auto options = snow_shot::presentation::screenshotDrawingToolbarOptions();
    options.showAirDropButton = true;
    m_toolbarWindow = new ScreenshotFloatingToolPaletteWindow(options);
    m_toolbarWindow->setAttribute(Qt::WA_DeleteOnClose, false);
    m_toolbarWindow->setTransientOwnerWindow(&m_pinnedWindow);
    m_toolbarWindow->setStyleToolbarAboveMain(false);

    if (ScreenshotToolPalette* toolbar = m_toolbarWindow->palette()) {
        new snow_shot::presentation::ScreenshotStyleBinding(*toolbar, m_canvas, toolbar);
        toolbar->setDrawTemplateCallbacks(
            [this]() { return m_pinnedWindow.m_runtime.serializeSelectedDrawTemplate(); },
            [this](const QByteArray& payload) {
                m_canvas.insertDrawTemplate(payload, m_pinnedWindow.canvasPositionForViewPosition(
                                                         QRectF(m_canvas.rect()).center()));
            });
        toolbar->setHistoryState(m_canvas.canvasHistoryState());
        connect(toolbar, &ScreenshotToolPalette::undoRequested, this,
                [this]() { applyHistory(false); });
        connect(toolbar, &ScreenshotToolPalette::redoRequested, this,
                [this]() { applyHistory(true); });
        connect(toolbar, &ScreenshotToolPalette::moveRequested, this,
                &ScreenshotPinnedEditController::activateResizeWindowTool);
        snow_shot::presentation::bindScreenshotCanvasToolRequests(
            *toolbar, *this, [this](SnowCanvasTool tool) {
                activateCanvasTool(tool);
                if (tool == SnowCanvasTool::AutoFilter)
                    m_autoFilterController->validate();
            });
        connect(toolbar, &ScreenshotToolPalette::filterRequested, this,
                [this]() { activateCanvasTool(SnowCanvasTool::RectangleFilter); });
        toolbar->setAutoFilterAvailable(m_autoFilterController->available());
        connect(toolbar, &ScreenshotToolPalette::autoFilterCategoryRequested,
                m_autoFilterController.get(), &ScreenshotAutoFilterController::fillCategory);

        toolbar->setWatermarkConfig(m_canvas.canvasWatermarkConfig());
        toolbar->setSpotlightConfig(m_canvas.canvasSpotlightConfig());

        connect(toolbar, &ScreenshotToolPalette::watermarkPreviewChanged, this,
                [this](const SnowCanvasWatermarkConfig& config) {
                    m_canvas.previewCanvasWatermarkConfig(config);
                });

        connect(toolbar, &ScreenshotToolPalette::spotlightPreviewChanged, this,
                [this](const SnowCanvasSpotlightConfig& config) {
                    m_canvas.previewCanvasSpotlightConfig(config);
                });
        connect(toolbar, &ScreenshotToolPalette::ocrRequested, this, [this]() {
            prepareRecognitionToolActivation();
            emit textRecognitionRequested();
        });
        connect(toolbar, &ScreenshotToolPalette::tableRequested, this, [this]() {
            prepareRecognitionToolActivation();
            emit tableRecognitionRequested();
        });
        connect(toolbar, &ScreenshotToolPalette::qrRequested, this, [this]() {
            prepareRecognitionToolActivation();
            emit qrRecognitionRequested();
        });
        connect(toolbar, &ScreenshotToolPalette::textTranslationRequested, this, [this]() {
            prepareRecognitionToolActivation();
            emit textTranslationRequested();
        });
        snow_shot::presentation::bindScreenshotCanvasOperations(toolbar, m_canvas, this);

        connect(toolbar, &ScreenshotToolPalette::textStylePopupInteractionBegan, this,
                [this]() { m_canvas.beginTextStylePopupInteraction(); });
        connect(toolbar, &ScreenshotToolPalette::textStylePopupInteractionEnded, this,
                [this]() { m_canvas.endTextStylePopupInteraction(m_toolbarWindow); });

        connect(toolbar, &ScreenshotToolPalette::canvasColorSamplingRequested, this,
                &ScreenshotPinnedEditController::beginCanvasColorSampling);
        connect(toolbar, &ScreenshotToolPalette::confirmRequested, this,
                [this]() { QTimer::singleShot(0, this, [this]() { setEditMode(false); }); });
    }

    connect(m_toolbarWindow, &ScreenshotFloatingToolPaletteWindow::dragFinished, this,
            &ScreenshotPinnedEditController::markToolbarManuallyPlaced);
    if (ScreenshotToolPaletteHost* host = m_toolbarWindow->paletteHost()) {
        // Secondary rows can change their occupied extent inside the fixed window frame.
        // Observe the committed host layout even when the main row does not move.
        connect(host, &ScreenshotToolPaletteHost::visibleContentChanged, this, [this]() {
            if (m_editMode && !m_manuallyPlaced) {
                updatePlacement();
            }
        });
        connect(host, &ScreenshotToolPaletteHost::dragStarted, this,
                [this](const QPoint&) { markToolbarManuallyPlaced(); });
    }

    emit toolbarCreated(m_toolbarWindow);
}

void ScreenshotPinnedEditController::applyHistory(bool redo) {
    auto* session = m_pinnedWindow.m_recognitionSession.get();
    if (session && session->active()) {
        if (session->tableModeActive()) {
            if (redo)
                session->redoTableEdit();
            else
                session->undoTableEdit();
        } else {
            if (redo)
                session->redoTextEdit();
            else
                session->undoTextEdit();
        }
        return;
    }
    static_cast<void>(redo ? m_canvas.redo() : m_canvas.undo());
}

void ScreenshotPinnedEditController::setEditMode(bool enabled) {
    if (m_editMode == enabled) {
        return;
    }

    m_editMode = enabled;
    m_tabletFrameGesture = false;
    m_tabletSamplingReleasePending = false;
    m_angleWheelSteps.reset();
    if (enabled) {
        const QPointer<ScreenshotPinnedEditController> guard(this);
        ensureToolbar();
        if (guard == nullptr || !m_editMode || m_toolbarWindow == nullptr) {
            return;
        }
        const SnowCanvasStyleDefaults defaults =
            snow_shot::presentation::screenshotCanvasToolStyleDefaults();
        snow_shot::presentation::applyScreenshotCanvasToolStyles(m_canvas, defaults);
        if (m_toolbarWindow != nullptr && m_toolbarWindow->palette() != nullptr) {
            m_toolbarWindow->palette()->setCreationStyleDefaults(defaults);
        }
        m_canvas.setFocus(Qt::OtherFocusReason);
        m_canvas.setCanvasTool(SnowCanvasTool::Select);
        syncPaletteFromCanvasStyle();
        m_manuallyPlaced = false;
        if (m_toolbarWindow != nullptr) {
            m_toolbarWindow->cancelDrag();
            ScreenshotToolPalette* toolbarPalette = m_toolbarWindow->palette();
            // Opening the toolbar reflects the current mode. Only an explicit tool
            // command may replace active recognition with a drawing tool.
            if (m_pinnedWindow.m_ocrMode) {
                prepareRecognitionToolActivation();
                m_pinnedWindow.updateRecognitionToolbarState();
            } else if (toolbarPalette == nullptr ||
                       !toolbarPalette->activateRememberedDrawingTool()) {
                applyResizeWindowTool();
            }
            updatePlacement();
            if (guard == nullptr || !m_editMode || m_toolbarWindow == nullptr) {
                return;
            }
            m_toolbarWindow->prepareForDisplay();
            if (guard == nullptr || !m_editMode || m_toolbarWindow == nullptr) {
                return;
            }
            m_toolbarWindow->show();
            raiseToolbar();
        }
        emit editModeChanged(true);
        return;
    }

    cancelCanvasColorSampling();
    m_toolBeforeWindowResize.reset();
    m_resizeWindowToolActive = false;
    m_nativeWindowInteractionActive = false;
    m_recognitionToolActivationPending = false;
    static_cast<void>(m_canvas.resetEditingState());
    syncCanvasInteractionState();
    m_canvas.clearFocus();
    if (m_toolbarWindow != nullptr) {
        m_toolbarWindow->cancelDrag();
        if (ScreenshotToolPaletteHost* host = m_toolbarWindow->paletteHost()) {
            host->clearActiveTool();
        }
    }
    destroyToolbar();
    resetAutoFilterSession();
    m_pinnedWindow.m_runtime.clearRenderState();
    emit editModeChanged(false);
}

void ScreenshotPinnedEditController::activateResizeWindowTool() {
    if (!m_editMode) {
        return;
    }
    m_pinnedWindow.deactivateRecognition();
    applyResizeWindowTool();
}

void ScreenshotPinnedEditController::applyResizeWindowTool() {
    cancelCanvasColorSampling();
    m_toolBeforeWindowResize.reset();
    m_recognitionToolActivationPending = false;
    static_cast<void>(m_canvas.resetEditingState());
    m_resizeWindowToolActive = true;
    syncCanvasInteractionState();
    if (ScreenshotToolPaletteHost* host = toolbarHost()) {
        host->setActiveTool(ScreenshotToolPalette::Tool::Move);
    }
    m_pinnedWindow.updateWindowDragCursor(m_pinnedWindow.mapFromGlobal(QCursor::pos()));
}

bool ScreenshotPinnedEditController::beginTemporaryResizeWindowTool() {
    if (!m_editMode || m_toolBeforeWindowResize.has_value() || m_toolbarWindow == nullptr ||
        m_toolbarWindow->palette() == nullptr) {
        return false;
    }
    const std::optional<ScreenshotToolPalette::Tool> activeTool =
        m_toolbarWindow->palette()->activeTool();
    if (!activeTool.has_value() || *activeTool == ScreenshotToolPalette::Tool::Move) {
        return false;
    }
    m_toolBeforeWindowResize = static_cast<int>(*activeTool);
    m_resizeWindowToolActive = true;
    syncCanvasInteractionState();
    if (ScreenshotToolPaletteHost* host = toolbarHost()) {
        host->setActiveTool(ScreenshotToolPalette::Tool::Move);
    }
    m_pinnedWindow.updateWindowDragCursor(m_pinnedWindow.mapFromGlobal(QCursor::pos()));
    return true;
}

void ScreenshotPinnedEditController::endTemporaryResizeWindowTool() {
    if (!m_toolBeforeWindowResize.has_value()) {
        return;
    }
    const auto previousTool = static_cast<ScreenshotToolPalette::Tool>(*m_toolBeforeWindowResize);
    m_toolBeforeWindowResize.reset();
    m_resizeWindowToolActive = false;
    if (ScreenshotToolPaletteHost* host = toolbarHost()) {
        host->setActiveTool(previousTool);
    }
    syncCanvasInteractionState();
    m_pinnedWindow.updateWindowDragCursor(m_pinnedWindow.mapFromGlobal(QCursor::pos()));
}

void ScreenshotPinnedEditController::beginNativeWindowInteraction() {
    if (m_nativeWindowInteractionActive || m_toolbarWindow == nullptr) {
        return;
    }
    m_nativeWindowInteractionActive = true;
    cancelCanvasColorSampling();
    m_toolbarWindow->cancelDrag();
    if (ScreenshotToolPalette* palette = m_toolbarWindow->palette()) {
        for (auto* picker : palette->findChildren<adqt::widgets::AdColorPicker*>()) {
            picker->setPopupVisible(false);
        }
        for (auto* select : palette->findChildren<adqt::widgets::AdSelect*>()) {
            select->setPopupVisible(false);
        }
    }
    m_toolbarWindow->hide();
    for (QWidget* child : m_toolbarWindow->findChildren<QWidget*>()) {
        if (child->isWindow()) {
            child->hide();
        }
    }
}

void ScreenshotPinnedEditController::endNativeWindowInteraction() {
    if (!m_nativeWindowInteractionActive) {
        return;
    }
    m_nativeWindowInteractionActive = false;
    if (!m_editMode || m_toolbarWindow == nullptr || m_pinnedWindow.m_closing) {
        return;
    }
    m_manuallyPlaced = false;
    const QPointer<ScreenshotPinnedEditController> guard(this);
    updatePlacement();
    if (guard == nullptr || !m_editMode || m_toolbarWindow == nullptr) {
        return;
    }
    m_toolbarWindow->prepareForDisplay();
    if (guard == nullptr || !m_editMode || m_toolbarWindow == nullptr) {
        return;
    }
    m_toolbarWindow->show();
    raiseToolbar();
}

void ScreenshotPinnedEditController::recognitionDeactivated() {
    m_recognitionToolActivationPending = false;
    if (m_editMode) {
        applyResizeWindowTool();
    }
}

void ScreenshotPinnedEditController::updatePlacement() {
    if (m_toolbarWindow == nullptr || m_updatingPlacement || m_nativeWindowInteractionActive) {
        return;
    }

    // Layout preparation and row arrangement can emit content changes synchronously.
    const QPointer<ScreenshotPinnedEditController> guard(this);
    const QPointer<ScreenshotFloatingToolPaletteWindow> toolbar(m_toolbarWindow);
    m_updatingPlacement = true;
    // The callback may delete this controller, so cleanup must not retain a member reference.
    const auto updatingPlacement = qScopeGuard([guard] {
        if (guard != nullptr) {
            guard->m_updatingPlacement = false;
        }
    });
    const QRect logicalBounds = placementLogicalBounds();
    const QRect physicalBounds = placementPhysicalBounds();
    toolbar->setPlacementContext(placementScreen(), logicalBounds, physicalBounds);
    if (guard == nullptr || toolbar == nullptr || toolbar != m_toolbarWindow) {
        return;
    }
    toolbar->prepareForDisplay();
    if (guard == nullptr || toolbar == nullptr || toolbar != m_toolbarWindow) {
        return;
    }

    if (!m_manuallyPlaced) {
        const ScreenshotToolbarPlacementSnapshot toolbarGeometry =
            m_toolbarWindow->placementSnapshot();
        if (!toolbarGeometry.bottom.isValid()) {
            return;
        }
        // The outer window reserves shadow padding even when shadows are hidden.
        // Anchor the toolbar to the visible content in the screen's logical coordinates.
        const QRect pinnedContentGeometry = snow_shot::presentation::pinnedLogicalRect(
            m_pinnedWindow.currentContentNativeGeometry(), placementScreen());
        QRect placementBounds;
        if (const QScreen* screen = placementScreen()) {
            placementBounds = screen->geometry();
        }
        if (!placementBounds.isValid() || placementBounds.isEmpty()) {
            placementBounds = pinnedContentGeometry;
        }
        const ScreenshotAnchoredToolbarPlacement placement =
            ScreenshotGeometryMapper::anchoredToolbarPlacement(
                QPoint(pinnedContentGeometry.left() + pinnedContentGeometry.width(),
                       pinnedContentGeometry.top() + pinnedContentGeometry.height()),
                QPoint(pinnedContentGeometry.left() + pinnedContentGeometry.width(),
                       pinnedContentGeometry.top()),
                toolbarGeometry.bottom, toolbarGeometry.top, placementBounds, kToolbarGap);
        toolbar->setStyleToolbarAboveMain(placement.usesTopRightPlacement);
        if (guard == nullptr || toolbar == nullptr || toolbar != m_toolbarWindow) {
            return;
        }
        m_globalContentPosition = placement.contentPosition;
        m_toolbarWindow->resetPhysicalSizeInvariant();
    }

    m_toolbarWindow->moveContentTo(m_globalContentPosition);
    if (guard == nullptr || toolbar == nullptr || toolbar != m_toolbarWindow) {
        return;
    }
    if (m_toolbarWindow->isVisible()) {
        raiseToolbar();
    }
}

void ScreenshotPinnedEditController::updateAfterPinnedWindowMove(const QPoint& logicalDelta) {
    if (m_nativeWindowInteractionActive) {
        return;
    }
    if (m_manuallyPlaced) {
        m_globalContentPosition += logicalDelta;
    }
    updatePlacement();
}

void ScreenshotPinnedEditController::raiseToolbar() {
    if (m_toolbarWindow != nullptr && m_toolbarWindow->isVisible()) {
        m_toolbarWindow->raise();
    }
}

void ScreenshotPinnedEditController::destroyToolbar() {
    cancelCanvasColorSampling();
    if (m_toolbarWindow == nullptr) {
        return;
    }

    ScreenshotFloatingToolPaletteWindow* toolbarWindow = m_toolbarWindow;
    m_toolbarWindow = nullptr;
    m_nativeWindowInteractionActive = false;
    m_toolBeforeWindowResize.reset();
    m_canvas.endTextStylePopupInteraction(toolbarWindow);
    toolbarWindow->cancelDrag();
    toolbarWindow->setTransientOwnerWindow(nullptr);
    toolbarWindow->hide();
    delete toolbarWindow;
}

void ScreenshotPinnedEditController::activateCanvasTool(SnowCanvasTool tool) {
    if (!m_editMode) {
        return;
    }
    // Complete recognition teardown before committing the requested drawing tool.
    // Its exit callback may restore Resize window, but cannot override this command.
    m_pinnedWindow.deactivateRecognition();
    m_toolBeforeWindowResize.reset();
    m_resizeWindowToolActive = false;
    m_recognitionToolActivationPending = false;
    syncCanvasInteractionState();
    m_canvas.setCanvasTool(tool);
    // The canvas may already use this tool, so activeToolChanged is not guaranteed.
    syncPaletteFromCanvasTool();
    m_pinnedWindow.updateWindowDragCursor(m_pinnedWindow.mapFromGlobal(QCursor::pos()));
}

bool ScreenshotPinnedEditController::automationSetTool(SnowCanvasTool tool) {
    setEditMode(true);
    if (!m_editMode)
        return false;
    activateCanvasTool(tool);
    return m_canvas.canvasTool() == tool;
}

void ScreenshotPinnedEditController::prepareRecognitionToolActivation() {
    m_toolBeforeWindowResize.reset();
    m_resizeWindowToolActive = false;
    m_recognitionToolActivationPending = true;
    syncCanvasInteractionState();
    m_pinnedWindow.clearWindowDragCursor();
}

bool ScreenshotPinnedEditController::canvasInteractionAllowed() const {
    return m_editMode && !m_resizeWindowToolActive && !m_recognitionToolActivationPending &&
           !m_pinnedWindow.m_ocrMode;
}

void ScreenshotPinnedEditController::syncCanvasInteractionState() {
    m_canvas.setInteractionEnabled(canvasInteractionAllowed());
}

QScreen* ScreenshotPinnedEditController::placementScreen() const {
    if (QWindow* pinnedHandle = m_pinnedWindow.windowHandle()) {
        if (pinnedHandle->screen() != nullptr) {
            return pinnedHandle->screen();
        }
    }
    return m_pinnedWindow.screen();
}

QRect ScreenshotPinnedEditController::placementLogicalBounds() const {
    if (QScreen* screen = placementScreen()) {
        const QRect screenGeometry = screen->geometry();
        if (screenGeometry.isValid() && !screenGeometry.isEmpty()) {
            return screenGeometry;
        }
    }

    QRect logicalBounds = m_pinnedWindow.frameGeometry();
    if (!logicalBounds.isValid() || logicalBounds.isEmpty()) {
        logicalBounds = m_pinnedWindow.geometry();
    }
    return logicalBounds;
}

QRect ScreenshotPinnedEditController::placementPhysicalBounds() const {
    if (QScreen* screen = placementScreen()) {
        const QRect screenPhysicalBounds = snow_shot::presentation::pinnedScreenGeometry(*screen);
        if (screenPhysicalBounds.isValid() && !screenPhysicalBounds.isEmpty()) {
            return screenPhysicalBounds;
        }
    }

    QRect physicalBounds = m_pinnedWindow.currentNativeGeometry();
    if (physicalBounds.isValid() && !physicalBounds.isEmpty()) {
        return physicalBounds;
    }
    return placementLogicalBounds();
}

void ScreenshotPinnedEditController::syncPaletteFromCanvasTool() {
    ScreenshotToolPaletteHost* host = toolbarHost();
    if (host == nullptr) {
        return;
    }

    if (const auto tool = snow_shot::presentation::paletteToolForCanvasTool(m_canvas.canvasTool()))
        host->setActiveTool(*tool);
    else
        host->clearActiveTool();
}

void ScreenshotPinnedEditController::syncPaletteFromCanvasStyle() {
    ScreenshotToolPalette* toolbar =
        m_toolbarWindow != nullptr ? m_toolbarWindow->palette() : nullptr;
    if (toolbar == nullptr) {
        return;
    }

    toolbar->setStyleToolbarState(m_canvas.canvasStyleToolbarState());
    toolbar->setWatermarkConfig(m_canvas.canvasWatermarkConfig());
    toolbar->setSpotlightConfig(m_canvas.canvasSpotlightConfig());
}

void ScreenshotPinnedEditController::markToolbarManuallyPlaced() {
    if (m_toolbarWindow == nullptr) {
        return;
    }

    m_manuallyPlaced = true;
    m_globalContentPosition = m_toolbarWindow->contentPosition();
}

void ScreenshotPinnedEditController::beginCanvasColorSampling(
    adqt::widgets::AdColorPicker* picker) {
    if (picker == nullptr || !m_editMode) {
        return;
    }

    cancelCanvasColorSampling();
    if (!m_canvasColorSampling)
        m_canvasColorSampling = std::make_unique<ScreenshotCanvasColorSamplingSession>(*this);
    m_canvasColorSampling->begin(picker, [this]() { cancelCanvasColorSampling(); });
    m_canvasColorSampler.reset();
    if (m_toolbarWindow != nullptr) {
        m_shortcutManager.addScopeWindow(m_toolbarWindow);
    }
    setCanvasColorSamplingCursor(true);

    const std::optional<QPoint> physicalPosition = m_pinnedWindow.physicalCursorPosition();
    if (physicalPosition.has_value() &&
        m_pinnedWindow.currentContentNativeGeometry().contains(*physicalPosition)) {
        updateCanvasColorSamplingPreviewAtPhysicalPoint(
            *physicalPosition, canvasColorGlobalPositionAt(*physicalPosition));
        return;
    }
    const QPointF localPosition = m_canvas.mapFromGlobal(QCursor::pos());
    if (m_pinnedWindow.contentViewRect().contains(localPosition)) {
        updateCanvasColorSamplingPreviewAtPhysicalPoint(
            canvasColorPhysicalPositionAt(localPosition), QCursor::pos());
    }
}

void ScreenshotPinnedEditController::cancelCanvasColorSampling() {
    m_canvasColorSampler.reset();
    if (m_toolbarWindow != nullptr) {
        m_shortcutManager.removeScopeWindow(m_toolbarWindow);
    }
    if (m_canvasColorSampling)
        m_canvasColorSampling->end();
    setCanvasColorSamplingCursor(false);
}

QPoint
ScreenshotPinnedEditController::canvasColorPhysicalPositionAt(const QPointF& localPosition) const {
    return ScreenshotCanvasColorSampler::physicalPointForLocalPosition(
        localPosition, m_canvas.size(), m_pinnedWindow.currentNativeGeometry());
}

QPoint
ScreenshotPinnedEditController::canvasColorGlobalPositionAt(const QPoint& physicalPosition) const {
    const QRect physicalBounds = m_pinnedWindow.currentNativeGeometry();
    if (!physicalBounds.isValid() || physicalBounds.isEmpty()) {
        return QCursor::pos();
    }
    const QPoint localPosition(
        qRound((physicalPosition.x() - physicalBounds.left()) *
               static_cast<qreal>(m_canvas.width()) / physicalBounds.width()),
        qRound((physicalPosition.y() - physicalBounds.top()) *
               static_cast<qreal>(m_canvas.height()) / physicalBounds.height()));
    return m_canvas.mapToGlobal(localPosition);
}

QImage
ScreenshotPinnedEditController::canvasColorPreviewAtPhysicalPoint(const QPoint& physicalPosition) {
    const QRect physicalBounds = m_pinnedWindow.currentNativeGeometry();
    if (!m_pinnedWindow.currentContentNativeGeometry().contains(physicalPosition) ||
        !m_canvasColorSampler.ensureSnapshot(m_canvas, physicalBounds)) {
        return {};
    }
    return m_canvasColorSampler.previewAtPhysicalPoint(physicalPosition);
}

void ScreenshotPinnedEditController::updateCanvasColorSamplingPreviewAtPhysicalPoint(
    const QPoint& physicalPosition, const QPoint& globalPosition) {
    if (!canvasColorSamplingActive()) {
        return;
    }
    const QImage preview = canvasColorPreviewAtPhysicalPoint(physicalPosition);
    if (!preview.isNull()) {
        m_canvasColorSampling->updatePreview(preview, globalPosition);
    }
}

bool ScreenshotPinnedEditController::commitCanvasColorSampleAtPhysicalPoint(
    const QPoint& physicalPosition) {
    const QImage preview = canvasColorPreviewAtPhysicalPoint(physicalPosition);
    return m_canvasColorSampling &&
           m_canvasColorSampling->commit(preview, [this]() { cancelCanvasColorSampling(); });
}

bool ScreenshotPinnedEditController::automationAutoFilter(const QStringList& categories) {
    if (!m_autoFilterController || m_autoFilterController->detecting() || categories.isEmpty())
        return false;
    const QStringList allowed{QStringLiteral("text"),      QStringLiteral("text_in_box"),
                              QStringLiteral("image"),     QStringLiteral("avatar"),
                              QStringLiteral("icon"),      QStringLiteral("message_box"),
                              QStringLiteral("text_block")};
    for (const auto& category : categories)
        if (!allowed.contains(category))
            return false;
    m_automationFilterError.clear();
    m_automationFilterCategories = categories;
    if (!m_canvas.setCanvasTool(SnowCanvasTool::AutoFilter)) {
        m_automationFilterCategories.clear();
        emit automationAutoFilterStateChanged();
        return false;
    }
    m_autoFilterController->validate();
    if (m_autoFilterController->available() && !m_automationFilterCategories.isEmpty()) {
        const auto pending = std::exchange(m_automationFilterCategories, {});
        for (const auto& category : pending)
            m_autoFilterController->fillCategory(category);
    }
    emit automationAutoFilterStateChanged();
    return true;
}

QJsonObject ScreenshotPinnedEditController::automationAutoFilterState() const {
    return {{QStringLiteral("busy"), !m_automationFilterCategories.isEmpty()},
            {QStringLiteral("error"), m_automationFilterError}};
}

void ScreenshotPinnedEditController::cancelAutomationAutoFilter() {
    if (m_automationFilterCategories.isEmpty())
        return;
    resetAutoFilterSession();
}

void ScreenshotPinnedEditController::resetAutoFilterSession() {
    m_automationFilterCategories.clear();
    m_autoFilterController->resetSession();
    emit automationAutoFilterStateChanged();
}

void ScreenshotPinnedEditController::setCanvasColorSamplingCursor(bool enabled) {
    if (enabled && !m_canvasColorSamplingCursorOverridden) {
        QApplication::setOverrideCursor(ScreenshotCanvasColorSamplerWindow::samplingCursor());
        m_canvasColorSamplingCursorOverridden = true;
    } else if (!enabled && m_canvasColorSamplingCursorOverridden) {
        QApplication::restoreOverrideCursor();
        m_canvasColorSamplingCursorOverridden = false;
    }
}
