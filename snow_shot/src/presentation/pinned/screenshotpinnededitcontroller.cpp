#include "snow_shot/presentation/screenshotpinnededitcontroller.h"

#include "snow_shot/presentation/screenshotcanvascolorsamplerwindow.h"
#include "snow_shot/presentation/screenshotfloatingtoolpalettewindow.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/screenshottoolpalettehost.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/settingsadapters.h"

#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include "widgets/color_picker.h"

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>

namespace {
constexpr int kToolbarGap = 4;
ScreenshotToolPalette::Options pinnedEditToolbarOptions() {
    ScreenshotToolPalette::Options options;
    options.showDragHandle = true;
    options.showHistoryActions = true;
    options.showSelectTool = true;
    options.showShapeTool = true;
    options.showArrowTool = true;
    options.showLineTool = true;
    options.showFreeDrawTool = true;
    options.showHighlightTool = true;
    options.showSpotlightTool = true;
    options.showEraserTool = true;
    options.showFilterTool = true;
    options.showWatermarkTool = true;
    options.showTextTool = true;
    options.showSerialNumberTool = true;
    options.showOcrTool = true;
    options.showTextTranslationTool = true;
    options.showTableTool = true;
    options.showQrTool = true;
    options.separatorAfterSelect = true;
    options.separatorBeforeConfirm = true;
    options.actions = ScreenshotToolPalette::ConfirmAction;
    options.styleDefaults = snow_shot::presentation::screenshotCanvasStyleDefaults();
    return options;
}

bool wheelAdjustsStrokeWidth(SnowCanvasTool tool) {
    switch (tool) {
    case SnowCanvasTool::Shape:
    case SnowCanvasTool::Arrow:
    case SnowCanvasTool::Line:
    case SnowCanvasTool::FreeDraw:
    case SnowCanvasTool::RectangleHighlight:
    case SnowCanvasTool::PenHighlight:
        return true;
    default:
        return false;
    }
}
} // namespace

ScreenshotPinnedEditController::ScreenshotPinnedEditController(
    ScreenshotPinnedWindow& pinnedWindow, SnowCanvasWidget& canvas,
    snow_shot::presentation::WindowShortcutManager& shortcutManager, QObject* parent)
    : QObject(parent), m_pinnedWindow(pinnedWindow), m_canvas(canvas),
      m_shortcutManager(shortcutManager) {
    m_canvas.installEventFilter(this);
    m_canvasColorSamplerWindow = std::make_unique<ScreenshotCanvasColorSamplerWindow>();
    m_toolbarWindow = new ScreenshotFloatingToolPaletteWindow(pinnedEditToolbarOptions());
    m_toolbarWindow->setObjectName(QStringLiteral("screenshotPinnedDrawingToolbar"));
    m_toolbarWindow->setAttribute(Qt::WA_DeleteOnClose, false);
    m_toolbarWindow->setTransientOwnerWindow(&m_pinnedWindow);
    m_toolbarWindow->setStyleToolbarAboveMain(false);

    if (ScreenshotToolPalette* toolbar = m_toolbarWindow->palette()) {
        toolbar->setHistoryState(m_canvas.canvasHistoryState());
        connect(toolbar, &ScreenshotToolPalette::undoRequested, this,
                [this]() { static_cast<void>(m_canvas.undo()); });
        connect(toolbar, &ScreenshotToolPalette::redoRequested, this,
                [this]() { static_cast<void>(m_canvas.redo()); });
        connect(toolbar, &ScreenshotToolPalette::selectRequested, this,
                [this]() { m_canvas.setCanvasTool(SnowCanvasTool::Select); });
        connect(toolbar, &ScreenshotToolPalette::shapeRequested, this,
                [this]() { m_canvas.setCanvasTool(SnowCanvasTool::Shape); });
        connect(toolbar, &ScreenshotToolPalette::arrowRequested, this,
                [this]() { m_canvas.setCanvasTool(SnowCanvasTool::Arrow); });
        connect(toolbar, &ScreenshotToolPalette::lineRequested, this,
                [this]() { m_canvas.setCanvasTool(SnowCanvasTool::Line); });
        connect(toolbar, &ScreenshotToolPalette::freeDrawRequested, this,
                [this]() { m_canvas.setCanvasTool(SnowCanvasTool::FreeDraw); });
        connect(toolbar, &ScreenshotToolPalette::highlightRequested, this,
                [this]() { m_canvas.setCanvasTool(SnowCanvasTool::RectangleHighlight); });
        connect(toolbar, &ScreenshotToolPalette::penHighlightRequested, this,
                [this]() { m_canvas.setCanvasTool(SnowCanvasTool::PenHighlight); });
        connect(toolbar, &ScreenshotToolPalette::spotlightRequested, this,
                [this]() { m_canvas.setCanvasTool(SnowCanvasTool::Spotlight); });
        connect(toolbar, &ScreenshotToolPalette::eraserRequested, this,
                [this]() { m_canvas.setCanvasTool(SnowCanvasTool::Eraser); });
        connect(toolbar, &ScreenshotToolPalette::filterRequested, this,
                [this]() { m_canvas.setCanvasTool(SnowCanvasTool::Filter); });
        connect(toolbar, &ScreenshotToolPalette::rectangleFilterRequested, this,
                [this]() { m_canvas.setCanvasTool(SnowCanvasTool::RectangleFilter); });
        connect(toolbar, &ScreenshotToolPalette::penFilterRequested, this,
                [this]() { m_canvas.setCanvasTool(SnowCanvasTool::PenFilter); });
        connect(toolbar, &ScreenshotToolPalette::filterStyleChanged, this,
                [this](const SnowCanvasFilterStyle& style, quint32 properties) {
                    m_canvas.setCanvasFilterStyle(style, properties);
                });
        toolbar->setWatermarkConfig(m_canvas.canvasWatermarkConfig());
        toolbar->setSpotlightConfig(m_canvas.canvasSpotlightConfig());
        connect(toolbar, &ScreenshotToolPalette::watermarkRequested, this,
                [this]() { m_canvas.setCanvasTool(SnowCanvasTool::Watermark); });
        connect(toolbar, &ScreenshotToolPalette::watermarkConfigChanged, this,
                [this](const SnowCanvasWatermarkConfig& config) {
                    m_canvas.setCanvasWatermarkConfig(config);
                });
        connect(toolbar, &ScreenshotToolPalette::watermarkPreviewChanged, this,
                [this](const SnowCanvasWatermarkConfig& config) {
                    m_canvas.previewCanvasWatermarkConfig(config);
                });
        connect(toolbar, &ScreenshotToolPalette::spotlightConfigChanged, this,
                [this](const SnowCanvasSpotlightConfig& config) {
                    m_canvas.setCanvasSpotlightConfig(config);
                });
        connect(toolbar, &ScreenshotToolPalette::spotlightPreviewChanged, this,
                [this](const SnowCanvasSpotlightConfig& config) {
                    m_canvas.previewCanvasSpotlightConfig(config);
                });
        connect(toolbar, &ScreenshotToolPalette::textRequested, this,
                [this]() { m_canvas.setCanvasTool(SnowCanvasTool::Text); });
        connect(toolbar, &ScreenshotToolPalette::serialNumberRequested, this,
                [this]() { m_canvas.setCanvasTool(SnowCanvasTool::SerialNumber); });
        connect(toolbar, &ScreenshotToolPalette::ocrRequested, this,
                &ScreenshotPinnedEditController::textRecognitionRequested);
        connect(toolbar, &ScreenshotToolPalette::tableRequested, this,
                &ScreenshotPinnedEditController::tableRecognitionRequested);
        connect(toolbar, &ScreenshotToolPalette::qrRequested, this,
                &ScreenshotPinnedEditController::qrRecognitionRequested);
        connect(toolbar, &ScreenshotToolPalette::textTranslationRequested, this,
                &ScreenshotPinnedEditController::textTranslationRequested);
        connect(toolbar, &ScreenshotToolPalette::serialNumberDecrementRequested, this,
                [this]() { m_canvas.adjustSelectedSerialNumbers(-1); });
        connect(toolbar, &ScreenshotToolPalette::serialNumberIncrementRequested, this,
                [this]() { m_canvas.adjustSelectedSerialNumbers(1); });
        connect(toolbar, &ScreenshotToolPalette::serialNumberCreateTextRequested, this,
                [this]() { m_canvas.createSerialNumberText(); });
        connect(toolbar, &ScreenshotToolPalette::sendSelectionToBackRequested, this,
                [this]() { m_canvas.reorderSelected(SnowCanvasSelectionOrder::SendToBack); });
        connect(toolbar, &ScreenshotToolPalette::sendSelectionBackwardRequested, this,
                [this]() { m_canvas.reorderSelected(SnowCanvasSelectionOrder::SendBackward); });
        connect(toolbar, &ScreenshotToolPalette::bringSelectionForwardRequested, this,
                [this]() { m_canvas.reorderSelected(SnowCanvasSelectionOrder::BringForward); });
        connect(toolbar, &ScreenshotToolPalette::bringSelectionToFrontRequested, this,
                [this]() { m_canvas.reorderSelected(SnowCanvasSelectionOrder::BringToFront); });
        connect(toolbar, &ScreenshotToolPalette::selectionOpacityChanged, this,
                [this](qreal opacity) { m_canvas.setSelectedOpacity(opacity); });
        connect(toolbar, &ScreenshotToolPalette::duplicateSelectionRequested, this,
                [this]() { m_canvas.duplicateSelected(); });
        connect(toolbar, &ScreenshotToolPalette::deleteSelectionRequested, this,
                [this]() { m_canvas.deleteSelected(); });
        connect(toolbar, &ScreenshotToolPalette::shapeStyleChanged, this,
                &ScreenshotPinnedEditController::applyShapeStyleFromPalette);
        connect(toolbar, &ScreenshotToolPalette::textStyleChanged, this,
                &ScreenshotPinnedEditController::applyTextStyleFromPalette);
        connect(toolbar, &ScreenshotToolPalette::textStylePopupInteractionBegan, this,
                [this]() { m_canvas.beginTextStylePopupInteraction(); });
        connect(toolbar, &ScreenshotToolPalette::textStylePopupInteractionEnded, this,
                [this]() { m_canvas.endTextStylePopupInteraction(m_toolbarWindow); });
        connect(toolbar, &ScreenshotToolPalette::serialNumberStyleChanged, this,
                &ScreenshotPinnedEditController::applySerialNumberStyleFromPalette);
        connect(toolbar, &ScreenshotToolPalette::canvasColorSamplingRequested, this,
                &ScreenshotPinnedEditController::beginCanvasColorSampling);
        connect(toolbar, &ScreenshotToolPalette::confirmRequested, this,
                [this]() { setEditMode(false); });
    }

    connect(m_toolbarWindow, &ScreenshotFloatingToolPaletteWindow::dragFinished, this,
            &ScreenshotPinnedEditController::markToolbarManuallyPlaced);
    if (ScreenshotToolPaletteHost* host = m_toolbarWindow->paletteHost()) {
        connect(host, &ScreenshotToolPaletteHost::dragStarted, this,
                [this](const QPoint&) { markToolbarManuallyPlaced(); });
    }
    connect(&m_canvas, &SnowCanvasWidget::activeToolChanged, this,
            &ScreenshotPinnedEditController::syncPaletteFromCanvasTool);
    connect(&m_canvas, &SnowCanvasWidget::styleToolbarStateChanged, this,
            &ScreenshotPinnedEditController::syncPaletteFromCanvasStyle);
    connect(&m_canvas, &SnowCanvasWidget::historyStateChanged, this, [this]() {
        if (m_toolbarWindow != nullptr) {
            if (ScreenshotToolPalette* toolbar = m_toolbarWindow->palette()) {
                toolbar->setHistoryState(m_canvas.canvasHistoryState());
            }
        }
    });

    registerDrawingShortcuts();
    reloadDrawingShortcuts();
    registerRecognitionShortcuts();
    reloadRecognitionShortcuts();
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    if (storage.isInitialized()) {
        connect(&storage.configuration(), &snow_shot::storage::ConfigurationStore::valueChanged,
                this, [this](const QString& key, const QJsonValue&) {
                    if (key.startsWith(QStringLiteral("drawing_shortcuts/"))) {
                        reloadDrawingShortcuts();
                    } else if (key.startsWith(QStringLiteral("screenshot_shortcuts/"))) {
                        reloadRecognitionShortcuts();
                    }
                });
    }

    updatePlacement();
}

ScreenshotPinnedEditController::~ScreenshotPinnedEditController() {
    destroyToolbar();
}

bool ScreenshotPinnedEditController::editMode() const {
    return m_editMode;
}

bool ScreenshotPinnedEditController::canvasColorSamplingActive() const {
    return !m_canvasColorSamplingTarget.isNull();
}

void ScreenshotPinnedEditController::updateCanvasColorSamplingAfterCursorMove(
    const QPoint& physicalPosition) {
    if (!canvasColorSamplingActive()) {
        return;
    }
    if (m_pinnedWindow.currentNativeGeometry().contains(physicalPosition)) {
        updateCanvasColorSamplingPreviewAtPhysicalPoint(
            physicalPosition, canvasColorGlobalPositionAt(physicalPosition));
    }
}

bool ScreenshotPinnedEditController::eventFilter(QObject* watched, QEvent* event) {
    if (watched != &m_canvas || event == nullptr || !m_editMode) {
        return QObject::eventFilter(watched, event);
    }

    if (!m_canvasColorSamplingTarget.isNull()) {
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
            if (keyEvent->key() == Qt::Key_Escape) {
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

    if (event->type() == QEvent::Wheel) {
        auto* wheelEvent = static_cast<QWheelEvent*>(event);
        const int deltaY = !wheelEvent->pixelDelta().isNull() ? wheelEvent->pixelDelta().y()
                                                              : wheelEvent->angleDelta().y();
        ScreenshotToolPaletteHost* host = toolbarHost();
        const int direction = deltaY > 0 ? 1 : -1;
        bool handled = false;
        if (deltaY != 0 && host != nullptr) {
            const SnowCanvasTool activeTool = m_canvas.canvasTool();
            if (wheelAdjustsStrokeWidth(activeTool)) {
                handled = host->stepStrokeWidth(direction);
            } else {
                switch (activeTool) {
                case SnowCanvasTool::Select:
                    handled = host->stepSelectionOpacity(direction);
                    break;
                case SnowCanvasTool::Spotlight:
                    handled = host->stepSpotlightOpacity(direction);
                    break;
                case SnowCanvasTool::RectangleFilter:
                    handled = host->stepFilterIntensity(direction);
                    break;
                case SnowCanvasTool::PenFilter:
                    handled = host->stepPenFilterStrokeWidth(direction);
                    break;
                case SnowCanvasTool::Watermark:
                    handled = host->stepWatermarkFontSize(direction);
                    break;
                default:
                    break;
                }
            }
        }
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
        binding.canActivate = [this](const auto&) {
            return m_editMode && !m_canvas.hasActiveTextEditing() && m_toolbarWindow != nullptr &&
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
        static_cast<void>(m_shortcutManager.setKeyCombinations(
            binding.value(),
            snow_shot::presentation::WindowShortcutManager::keyCombinationsFromPortableText(
                settings.shortcuts(binding.key()))));
    }
}

void ScreenshotPinnedEditController::registerRecognitionShortcuts() {
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
        binding.canActivate = [this](const auto&) {
            return m_editMode && !m_canvas.hasActiveTextEditing() && m_toolbarWindow != nullptr &&
                   m_toolbarWindow->palette() != nullptr;
        };
        binding.activate = [this, actionId](const auto&) {
            if (actionId == QStringLiteral("table_recognition")) {
                emit tableRecognitionRequested();
            } else if (actionId == QStringLiteral("qr_code_recognition")) {
                emit qrRecognitionRequested();
            } else if (actionId == QStringLiteral("text_recognition")) {
                emit textRecognitionRequested();
            } else if (actionId == QStringLiteral("text_translation")) {
                emit textTranslationRequested();
            } else {
                return false;
            }
            return true;
        };
        m_recognitionShortcutBindings.insert(
            actionId, m_shortcutManager.addBinding(this, std::move(binding)));
    }
}

void ScreenshotPinnedEditController::reloadRecognitionShortcuts() {
    const snow_shot::storage::ScreenshotShortcutSettings settings;
    for (auto binding = m_recognitionShortcutBindings.cbegin();
         binding != m_recognitionShortcutBindings.cend(); ++binding) {
        static_cast<void>(m_shortcutManager.setKeyCombinations(
            binding.value(),
            snow_shot::presentation::WindowShortcutManager::keyCombinationsFromPortableText(
                settings.shortcuts(binding.key()))));
    }
}

ScreenshotFloatingToolPaletteWindow* ScreenshotPinnedEditController::toolbarWindow() const {
    return m_toolbarWindow;
}

ScreenshotToolPaletteHost* ScreenshotPinnedEditController::toolbarHost() const {
    return m_toolbarWindow != nullptr ? m_toolbarWindow->paletteHost() : nullptr;
}

void ScreenshotPinnedEditController::setEditMode(bool enabled) {
    if (m_editMode == enabled && m_canvas.interactionEnabled() == enabled) {
        return;
    }

    m_editMode = enabled;
    if (enabled) {
        m_canvas.setInteractionEnabled(true);
        m_canvas.setFocus(Qt::OtherFocusReason);
        m_canvas.setCanvasTool(SnowCanvasTool::Select);
        syncPaletteFromCanvasStyle();
        m_manuallyPlaced = false;
        if (m_toolbarWindow != nullptr) {
            m_toolbarWindow->cancelDrag();
            if (ScreenshotToolPaletteHost* host = m_toolbarWindow->paletteHost()) {
                host->setActiveTool(ScreenshotToolPalette::Tool::Select);
            }
            updatePlacement();
            m_toolbarWindow->prepareForDisplay();
            m_toolbarWindow->show();
            raiseToolbar();
        }
        emit editModeChanged(true);
        return;
    }

    cancelCanvasColorSampling();
    static_cast<void>(m_canvas.resetEditingState());
    m_canvas.setInteractionEnabled(false);
    m_canvas.clearFocus();
    if (m_toolbarWindow != nullptr) {
        m_toolbarWindow->cancelDrag();
        if (ScreenshotToolPaletteHost* host = m_toolbarWindow->paletteHost()) {
            host->clearActiveTool();
        }
        hideToolbar();
    }
    updatePlacement();
    emit editModeChanged(false);
}

void ScreenshotPinnedEditController::restoreDrawingToolState() {
    syncPaletteFromCanvasTool();
}

void ScreenshotPinnedEditController::updatePlacement() {
    if (m_toolbarWindow == nullptr || m_updatingPlacement) {
        return;
    }

    const QRect logicalBounds = placementLogicalBounds();
    const QRect physicalBounds = placementPhysicalBounds();
    m_toolbarWindow->setPlacementContext(placementScreen(), logicalBounds, physicalBounds);
    m_toolbarWindow->prepareForDisplay();

    const QRect bottomToolbarRect = m_toolbarWindow->bottomPlacementContentRect();
    if (bottomToolbarRect.isEmpty()) {
        return;
    }

    if (!m_manuallyPlaced) {
        const QRect pinnedGeometry =
            m_pinnedWindow.frameGeometry().isValid() && !m_pinnedWindow.frameGeometry().isEmpty()
                ? m_pinnedWindow.frameGeometry()
                : m_pinnedWindow.geometry();
        QRect placementBounds;
        if (const QScreen* screen = placementScreen()) {
            placementBounds = screen->geometry();
        }
        if (!placementBounds.isValid() || placementBounds.isEmpty()) {
            placementBounds = pinnedGeometry;
        }
        const ScreenshotAnchoredToolbarPlacement placement =
            ScreenshotGeometryMapper::anchoredToolbarPlacement(
                QPoint(pinnedGeometry.left() + pinnedGeometry.width(),
                       pinnedGeometry.top() + pinnedGeometry.height()),
                QPoint(pinnedGeometry.left() + pinnedGeometry.width(), pinnedGeometry.top()),
                bottomToolbarRect, placementBounds, kToolbarGap,
                m_toolbarWindow->topRightMainToolbarContentRect(),
                m_toolbarWindow->topPlacementContentRect());
        m_toolbarWindow->setStyleToolbarAboveMain(placement.usesTopRightPlacement);
        m_globalContentPosition = placement.contentPosition;
        m_toolbarWindow->resetPhysicalSizeInvariant();
    }

    m_updatingPlacement = true;
    m_toolbarWindow->moveContentTo(m_globalContentPosition);
    m_updatingPlacement = false;
    if (m_toolbarWindow->isVisible()) {
        raiseToolbar();
    }
}

void ScreenshotPinnedEditController::updateAfterPinnedWindowMove(const QPoint& logicalDelta) {
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

void ScreenshotPinnedEditController::hideToolbar() {
    if (m_toolbarWindow == nullptr) {
        return;
    }

    m_toolbarWindow->cancelDrag();
    m_toolbarWindow->hide();
}

void ScreenshotPinnedEditController::destroyToolbar() {
    cancelCanvasColorSampling();
    if (m_toolbarWindow == nullptr) {
        return;
    }

    ScreenshotFloatingToolPaletteWindow* toolbarWindow = m_toolbarWindow;
    m_toolbarWindow = nullptr;
    toolbarWindow->cancelDrag();
    toolbarWindow->setTransientOwnerWindow(nullptr);
    toolbarWindow->hide();
    delete toolbarWindow;
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
        const QRect screenPhysicalBounds = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
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

    switch (m_canvas.canvasTool()) {
    case SnowCanvasTool::Select:
        host->setActiveTool(ScreenshotToolPalette::Tool::Select);
        break;
    case SnowCanvasTool::Shape:
        host->setActiveTool(ScreenshotToolPalette::Tool::Shape);
        break;
    case SnowCanvasTool::Arrow:
        host->setActiveTool(ScreenshotToolPalette::Tool::Arrow);
        break;
    case SnowCanvasTool::Line:
        host->setActiveTool(ScreenshotToolPalette::Tool::Line);
        break;
    case SnowCanvasTool::FreeDraw:
        host->setActiveTool(ScreenshotToolPalette::Tool::FreeDraw);
        break;
    case SnowCanvasTool::RectangleHighlight:
        host->setActiveTool(ScreenshotToolPalette::Tool::RectangleHighlight);
        break;
    case SnowCanvasTool::PenHighlight:
        host->setActiveTool(ScreenshotToolPalette::Tool::PenHighlight);
        break;
    case SnowCanvasTool::Spotlight:
        host->setActiveTool(ScreenshotToolPalette::Tool::Spotlight);
        break;
    case SnowCanvasTool::Eraser:
        host->setActiveTool(ScreenshotToolPalette::Tool::Eraser);
        break;
    case SnowCanvasTool::RectangleFilter:
        host->setActiveTool(ScreenshotToolPalette::Tool::RectangleFilter);
        break;
    case SnowCanvasTool::PenFilter:
        host->setActiveTool(ScreenshotToolPalette::Tool::PenFilter);
        break;
    case SnowCanvasTool::Watermark:
        host->setActiveTool(ScreenshotToolPalette::Tool::Watermark);
        break;
    case SnowCanvasTool::Text:
        host->setActiveTool(ScreenshotToolPalette::Tool::Text);
        break;
    case SnowCanvasTool::SerialNumber:
        host->setActiveTool(ScreenshotToolPalette::Tool::SerialNumber);
        break;
    default:
        host->clearActiveTool();
        break;
    }
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

void ScreenshotPinnedEditController::applyShapeStyleFromPalette(const SnowCanvasShapeStyle& style,
                                                                quint32 properties,
                                                                SnowCanvasShapeKind kind) {
    m_canvas.setCanvasShapeStylePatch(style, properties, kind);
}

void ScreenshotPinnedEditController::applyTextStyleFromPalette(const SnowCanvasTextStyle& style) {
    static_cast<void>(m_canvas.setCanvasTextStyle(style));
}

void ScreenshotPinnedEditController::applySerialNumberStyleFromPalette(
    const SnowCanvasSerialNumberStyle& style) {
    static_cast<void>(m_canvas.setCanvasSerialNumberStyle(style));
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
    m_canvasColorSamplingTarget = picker;
    m_canvasColorSamplingDestroyedConnection =
        connect(picker, &QObject::destroyed, this, [this]() { cancelCanvasColorSampling(); });
    if (m_canvasColorSamplerWindow != nullptr) {
        m_canvasColorSamplerWindow->beginSampling();
    }
    m_canvasColorSampler.reset();
    if (m_toolbarWindow != nullptr) {
        m_shortcutManager.addScopeWindow(m_toolbarWindow);
    }
    setCanvasColorSamplingCursor(true);

    const std::optional<QPoint> physicalPosition = m_pinnedWindow.physicalCursorPosition();
    if (physicalPosition.has_value() &&
        m_pinnedWindow.currentNativeGeometry().contains(*physicalPosition)) {
        updateCanvasColorSamplingPreviewAtPhysicalPoint(
            *physicalPosition, canvasColorGlobalPositionAt(*physicalPosition));
        return;
    }
    const QPointF localPosition = m_canvas.mapFromGlobal(QCursor::pos());
    if (m_canvas.rect().contains(localPosition.toPoint())) {
        updateCanvasColorSamplingPreviewAtPhysicalPoint(
            canvasColorPhysicalPositionAt(localPosition), QCursor::pos());
    }
}

void ScreenshotPinnedEditController::cancelCanvasColorSampling() {
    m_canvasColorSamplingTarget.clear();
    disconnect(m_canvasColorSamplingDestroyedConnection);
    m_canvasColorSamplingDestroyedConnection = {};
    m_canvasColorSampler.reset();
    if (m_toolbarWindow != nullptr) {
        m_shortcutManager.removeScopeWindow(m_toolbarWindow);
    }
    if (m_canvasColorSamplerWindow != nullptr) {
        m_canvasColorSamplerWindow->endSampling();
    }
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
    if (!physicalBounds.contains(physicalPosition) ||
        !m_canvasColorSampler.ensureSnapshot(m_canvas, physicalBounds)) {
        return {};
    }
    return m_canvasColorSampler.previewAtPhysicalPoint(physicalPosition);
}

void ScreenshotPinnedEditController::updateCanvasColorSamplingPreviewAtPhysicalPoint(
    const QPoint& physicalPosition, const QPoint& globalPosition) {
    if (m_canvasColorSamplerWindow == nullptr || m_canvasColorSamplingTarget.isNull()) {
        return;
    }
    const QImage preview = canvasColorPreviewAtPhysicalPoint(physicalPosition);
    if (!preview.isNull()) {
        m_canvasColorSamplerWindow->updateSample(preview, globalPosition);
    }
}

bool ScreenshotPinnedEditController::commitCanvasColorSampleAtPhysicalPoint(
    const QPoint& physicalPosition) {
    QPointer<adqt::widgets::AdColorPicker> picker = m_canvasColorSamplingTarget;
    const QImage preview = canvasColorPreviewAtPhysicalPoint(physicalPosition);
    cancelCanvasColorSampling();
    if (picker.isNull() || preview.isNull()) {
        return false;
    }

    const QColor sampled = preview.pixelColor(preview.width() / 2, preview.height() / 2);
    if (!sampled.isValid()) {
        return false;
    }
    picker->commitValue(adqt::widgets::AdColorValue::solid(sampled));
    return true;
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
