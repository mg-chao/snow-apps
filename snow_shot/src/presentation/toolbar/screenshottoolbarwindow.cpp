#include "snow_shot/presentation/screenshottoolbarwindow.h"

#include "../tools/screenshottoolbarperfinstrumentation.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshottoolbarcommands.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/screenshottoolpalettehost.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QCursor>
#include <QJsonValue>
#include <QScreen>
#include <QSignalBlocker>

namespace {
ScreenshotToolPalette::Options screenshotToolbarOptions() {
    ScreenshotToolPalette::Options options;
    options.showDragHandle = true;
    options.showHistoryActions = true;
    options.showMoveTool = true;
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
    options.showScreenRecordButton = true;
    options.showScrollingScreenshotTool = true;
    options.showSaveButton = true;
    options.separatorBeforeShape = true;
    options.actions = ScreenshotToolPalette::PinAction | ScreenshotToolPalette::CancelAction |
                      ScreenshotToolPalette::CopyAction;
    options.styleDefaults = snow_shot::presentation::screenshotCanvasStyleDefaults();
    return options;
}
} // namespace
ScreenshotToolbarWindow::ScreenshotToolbarWindow(ScreenshotToolbarCommandSink& commands,
                                                 QWidget* parent)
    : ScreenshotFloatingToolPaletteWindow(screenshotToolbarOptions(), parent),
      m_commands(commands) {
    setToolbarSize(snow_shot::storage::ScreenshotUiSettings().toolbarSize());
    setToolbarLayout(snow_shot::storage::ScreenshotToolbarSettings().layout());
    initializePalette();

    auto& configuration =
        snow_shot::storage::ApplicationStorage::instance().configuration();
    connect(&configuration, &snow_shot::storage::ConfigurationStore::valueChanged, this,
            [this](const QString& key, const QJsonValue&) {
                if (key == QStringLiteral("screenshot_ui/toolbar_size")) {
                    setToolbarSize(snow_shot::storage::ScreenshotUiSettings().toolbarSize());
                } else if (key == QStringLiteral("screenshot_toolbar/layout")) {
                    setToolbarLayout(snow_shot::storage::ScreenshotToolbarSettings().layout());
                }
            });
}

void ScreenshotToolbarWindow::setToolbarSize(const QString& size) {
    setPaletteScaleMultiplier(size == QStringLiteral("small") ? 0.8 : 1.0);
}

void ScreenshotToolbarWindow::setToolbarLayout(
    const snow_shot::storage::ScreenshotToolbarLayout& layout) {
    if (ScreenshotToolPalette* toolPalette = palette()) {
        toolPalette->setToolbarLayout(layout);
    }
}

void ScreenshotToolbarWindow::enterEvent(QEnterEvent* event) {
    m_commands.hideColorPickersForScreenshotUi();
    ScreenshotFloatingToolPaletteWindow::enterEvent(event);
}

void ScreenshotToolbarWindow::initializePalette() {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("window.initialize_palette");
    ScreenshotToolPalette* toolPalette = palette();
    ScreenshotToolPaletteHost* host = paletteHost();
    if (toolPalette == nullptr || host == nullptr) {
        return;
    }

    resetForNewCapture();

    connect(toolPalette, &ScreenshotToolPalette::undoRequested, this,
            [this]() { m_commands.undoCanvasEdit(); });
    connect(toolPalette, &ScreenshotToolPalette::redoRequested, this,
            [this]() { m_commands.redoCanvasEdit(); });
    connectToolCommands(*toolPalette);
    connectActionCommands(*toolPalette);
    connectStyleCommands(*toolPalette);
    connectSerialNumberCommands(*toolPalette);
    connectScrollingScreenshotCommands(*toolPalette);
    connect(host, &ScreenshotToolPaletteHost::dragStarted, this,
            [this](const QPoint&) { m_manuallyDragged = true; });
}

void ScreenshotToolbarWindow::connectToolCommands(ScreenshotToolPalette& toolPalette) {
    connect(&toolPalette, &ScreenshotToolPalette::moveRequested, this, [this]() {
        m_commands.setMoveTool();
        setActiveToolAndReposition(ScreenshotToolPalette::Tool::Move);
    });
    connect(&toolPalette, &ScreenshotToolPalette::selectRequested, this, [this]() {
        m_commands.setSelectTool();
        setActiveToolAndReposition(ScreenshotToolPalette::Tool::Select);
    });
    connect(&toolPalette, &ScreenshotToolPalette::shapeRequested, this, [this]() {
        m_commands.setShapeTool();
        setActiveToolAndReposition(ScreenshotToolPalette::Tool::Shape);
    });
    connect(&toolPalette, &ScreenshotToolPalette::arrowRequested, this, [this]() {
        m_commands.setArrowTool();
        setActiveToolAndReposition(ScreenshotToolPalette::Tool::Arrow);
    });
    connect(&toolPalette, &ScreenshotToolPalette::textRequested, this, [this]() {
        m_commands.setTextTool();
        setActiveToolAndReposition(ScreenshotToolPalette::Tool::Text);
    });
    connect(&toolPalette, &ScreenshotToolPalette::serialNumberRequested, this, [this]() {
        m_commands.setSerialNumberTool();
        setActiveToolAndReposition(ScreenshotToolPalette::Tool::SerialNumber);
    });
    connect(&toolPalette, &ScreenshotToolPalette::ocrRequested, this, [this]() {
        m_commands.setOcrTool();
        setActiveToolAndReposition(ScreenshotToolPalette::Tool::Ocr);
    });
    connect(&toolPalette, &ScreenshotToolPalette::textTranslationRequested, this, [this]() {
        m_commands.setTextTranslationTool();
        setActiveToolAndReposition(ScreenshotToolPalette::Tool::TextTranslation);
    });
    connect(&toolPalette, &ScreenshotToolPalette::tableRequested, this, [this]() {
        m_commands.setTableTool();
        setActiveToolAndReposition(ScreenshotToolPalette::Tool::Table);
    });
    connect(&toolPalette, &ScreenshotToolPalette::qrRequested, this, [this]() {
        m_commands.setQrTool();
        setActiveToolAndReposition(ScreenshotToolPalette::Tool::Qr);
    });
    connect(&toolPalette, &ScreenshotToolPalette::textEditRequested, this,
            [this]() { m_commands.toggleTextEditing(); });
    connect(&toolPalette, &ScreenshotToolPalette::textTranslateRequested, this,
            [this]() { m_commands.toggleTextTranslation(); });
    connect(&toolPalette, &ScreenshotToolPalette::textResetRequested, this,
            [this]() { m_commands.resetTextEditing(); });
    connect(&toolPalette, &ScreenshotToolPalette::textSettingsRequested, this,
            [this]() { m_commands.openTextTranslationSettings(); });
    connect(&toolPalette, &ScreenshotToolPalette::textFormattingRequested, this,
            [this](const QString& value) { m_commands.applyTextFormatting(value); });
    connect(&toolPalette, &ScreenshotToolPalette::textPunctuationRequested, this,
            [this](const QString& value) { m_commands.applyTextPunctuation(value); });
    connect(&toolPalette, &ScreenshotToolPalette::tableMergeRequested, this,
            [this]() { m_commands.mergeTableSelection(); });
    connect(&toolPalette, &ScreenshotToolPalette::tableSplitRequested, this,
            [this]() { m_commands.splitTableSelection(); });
    connect(&toolPalette, &ScreenshotToolPalette::tableResetRequested, this,
            [this]() { m_commands.resetTable(); });
}

void ScreenshotToolbarWindow::connectActionCommands(ScreenshotToolPalette& toolPalette) {
    connect(&toolPalette, &ScreenshotToolPalette::screenRecordRequested, this,
            [this]() { m_commands.startScreenRecording(); });
    connect(&toolPalette, &ScreenshotToolPalette::pinRequested, this,
            [this]() { m_commands.pinSelectionToScreen(); });
    connect(&toolPalette, &ScreenshotToolPalette::saveRequested, this,
            [this]() { m_commands.saveSelectionToFile(); });
    connect(&toolPalette, &ScreenshotToolPalette::cancelRequested, this,
            [this, palette = &toolPalette]() {
                palette->clearActiveTool();
                m_commands.cancelCapture();
            });
    connect(&toolPalette, &ScreenshotToolPalette::copyRequested, this,
            [this]() { m_commands.copySelectionToClipboard(); });
}

void ScreenshotToolbarWindow::connectStyleCommands(ScreenshotToolPalette& toolPalette) {
    connect(&toolPalette, &ScreenshotToolPalette::canvasColorSamplingRequested, this,
            [this](adqt::widgets::AdColorPicker* picker) {
                m_commands.beginCanvasColorSampling(picker);
            });
    connect(&toolPalette, &ScreenshotToolPalette::sendSelectionToBackRequested, this,
            [this]() { m_commands.reorderSelectedElements(SnowCanvasSelectionOrder::SendToBack); });
    connect(&toolPalette, &ScreenshotToolPalette::sendSelectionBackwardRequested, this, [this]() {
        m_commands.reorderSelectedElements(SnowCanvasSelectionOrder::SendBackward);
    });
    connect(&toolPalette, &ScreenshotToolPalette::bringSelectionForwardRequested, this, [this]() {
        m_commands.reorderSelectedElements(SnowCanvasSelectionOrder::BringForward);
    });
    connect(&toolPalette, &ScreenshotToolPalette::bringSelectionToFrontRequested, this, [this]() {
        m_commands.reorderSelectedElements(SnowCanvasSelectionOrder::BringToFront);
    });
    connect(&toolPalette, &ScreenshotToolPalette::selectionOpacityChanged, this,
            [this](qreal opacity) { m_commands.setSelectedElementsOpacity(opacity); });
    connect(&toolPalette, &ScreenshotToolPalette::duplicateSelectionRequested, this,
            [this]() { m_commands.duplicateSelectedElements(); });
    connect(&toolPalette, &ScreenshotToolPalette::deleteSelectionRequested, this,
            [this]() { m_commands.deleteSelectedElements(); });
    connect(
        &toolPalette, &ScreenshotToolPalette::shapeStyleChanged, this,
        [this](const SnowCanvasShapeStyle& style, quint32 properties, SnowCanvasShapeKind kind) {
            m_commands.setShapeStyleFromToolbar(style, properties, kind);
            if (ScreenshotToolPalette* palette = this->palette()) {
                static_cast<void>(snow_shot::presentation::persistScreenshotCanvasToolStyles(
                    palette->creationStyleDefaults()));
            }
        });
    connect(
        &toolPalette, &ScreenshotToolPalette::textStyleChanged, this,
        [this](const SnowCanvasTextStyle& style) {
            m_commands.setTextStyleFromToolbar(style);
            if (ScreenshotToolPalette* palette = this->palette()) {
                static_cast<void>(snow_shot::presentation::persistScreenshotCanvasToolStyles(
                    palette->creationStyleDefaults()));
            }
        });
    connect(&toolPalette, &ScreenshotToolPalette::lineRequested, this, [this]() {
        m_commands.setLineTool();
        setActiveToolAndReposition(ScreenshotToolPalette::Tool::Line);
    });
    connect(&toolPalette, &ScreenshotToolPalette::freeDrawRequested, this, [this]() {
        m_commands.setFreeDrawTool();
        setActiveToolAndReposition(ScreenshotToolPalette::Tool::FreeDraw);
    });
    connect(&toolPalette, &ScreenshotToolPalette::highlightRequested, this, [this]() {
        m_commands.setHighlightTool();
        setActiveToolAndReposition(ScreenshotToolPalette::Tool::RectangleHighlight);
    });
    connect(&toolPalette, &ScreenshotToolPalette::penHighlightRequested, this, [this]() {
        m_commands.setPenHighlightTool();
        setActiveToolAndReposition(ScreenshotToolPalette::Tool::PenHighlight);
    });
    connect(&toolPalette, &ScreenshotToolPalette::eraserRequested, this, [this]() {
        m_commands.setEraserTool();
        setActiveToolAndReposition(ScreenshotToolPalette::Tool::Eraser);
    });
    connect(&toolPalette, &ScreenshotToolPalette::filterRequested, this, [this]() {
        m_commands.setFilterTool();
        setActiveToolAndReposition(ScreenshotToolPalette::Tool::Filter);
    });
    connect(&toolPalette, &ScreenshotToolPalette::spotlightRequested, this, [this]() {
        m_commands.setSpotlightTool();
        setActiveToolAndReposition(ScreenshotToolPalette::Tool::Spotlight);
    });
    connect(&toolPalette, &ScreenshotToolPalette::rectangleFilterRequested, this, [this]() {
        m_commands.setRectangleFilterTool();
        setActiveToolAndReposition(ScreenshotToolPalette::Tool::RectangleFilter);
    });
    connect(&toolPalette, &ScreenshotToolPalette::penFilterRequested, this, [this]() {
        m_commands.setPenFilterTool();
        setActiveToolAndReposition(ScreenshotToolPalette::Tool::PenFilter);
    });
    connect(&toolPalette, &ScreenshotToolPalette::filterStyleChanged, this,
            [this](const SnowCanvasFilterStyle& style, quint32 properties) {
                m_commands.setFilterStyleFromToolbar(style, properties);
                if (ScreenshotToolPalette* palette = this->palette()) {
                    static_cast<void>(snow_shot::presentation::persistScreenshotCanvasToolStyles(
                        palette->creationStyleDefaults()));
                }
            });
    connect(&toolPalette, &ScreenshotToolPalette::watermarkRequested, this, [this]() {
        m_commands.setWatermarkTool();
        setActiveToolAndReposition(ScreenshotToolPalette::Tool::Watermark);
    });
    connect(&toolPalette, &ScreenshotToolPalette::watermarkConfigChanged, this,
            [this](const SnowCanvasWatermarkConfig& config) {
                m_commands.setWatermarkConfigFromToolbar(config);
            });
    connect(&toolPalette, &ScreenshotToolPalette::watermarkPreviewChanged, this,
            [this](const SnowCanvasWatermarkConfig& config) {
                m_commands.previewWatermarkFromToolbar(config);
            });
    connect(&toolPalette, &ScreenshotToolPalette::serialNumberStyleChanged, this,
            [this](const SnowCanvasSerialNumberStyle& style) {
                m_commands.setSerialNumberStyleFromToolbar(style);
                if (ScreenshotToolPalette* palette = this->palette()) {
                    static_cast<void>(snow_shot::presentation::persistScreenshotCanvasToolStyles(
                        palette->creationStyleDefaults()));
                }
            });
    connect(&toolPalette, &ScreenshotToolPalette::spotlightConfigChanged, this,
            [this](const SnowCanvasSpotlightConfig& config) {
                m_commands.setSpotlightConfigFromToolbar(config);
            });
    connect(&toolPalette, &ScreenshotToolPalette::spotlightPreviewChanged, this,
            [this](const SnowCanvasSpotlightConfig& config) {
                m_commands.previewSpotlightFromToolbar(config);
            });
}

void ScreenshotToolbarWindow::setActiveToolAndReposition(ScreenshotToolPalette::Tool tool) {
    ScreenshotToolPalette* toolPalette = palette();
    if (toolPalette == nullptr) {
        return;
    }

    toolPalette->setActiveTool(tool);
    if (!m_manuallyDragged) {
        m_commands.repositionToolbarForContentChange();
    }
}

void ScreenshotToolbarWindow::connectSerialNumberCommands(ScreenshotToolPalette& toolPalette) {
    connect(&toolPalette, &ScreenshotToolPalette::serialNumberDecrementRequested, this,
            [this]() { m_commands.decrementSelectedSerialNumbers(); });
    connect(&toolPalette, &ScreenshotToolPalette::serialNumberIncrementRequested, this,
            [this]() { m_commands.incrementSelectedSerialNumbers(); });
    connect(&toolPalette, &ScreenshotToolPalette::serialNumberCreateTextRequested, this,
            [this]() { m_commands.createTextForSelectedSerialNumber(); });
}

void ScreenshotToolbarWindow::connectScrollingScreenshotCommands(
    ScreenshotToolPalette& toolPalette) {
    connect(&toolPalette, &ScreenshotToolPalette::scrollingScreenshotRequested, this,
            [this]() { m_commands.startScrollingScreenshot(); });
    connect(&toolPalette, &ScreenshotToolPalette::scrollingRecognitionModeChanged, this,
            [this](ScreenshotScrollingRecognitionMode mode) {
                m_commands.setScrollingScreenshotRecognitionMode(mode);
            });
}

void ScreenshotToolbarWindow::resetForNewCapture() {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("window.reset_for_new_capture");
    cancelDrag();
    m_manuallyDragged = false;
    resetPhysicalSizeInvariant();
    if (ScreenshotToolPaletteHost* host = paletteHost()) {
        const QSignalBlocker blocker(host);
        host->setPhysicalScale(paletteScaleMultiplier());
        host->setShadowMargins(ScreenshotToolPaletteHost::defaultShadowMargins());
        setStyleToolbarAboveMain(false);
        host->setStyleToolbarVisible(false);
        host->resetStyleState();
        host->setCreationStyleDefaults(
            snow_shot::presentation::screenshotCanvasToolStyleDefaults());
        host->setScrollingScreenshotMode(false);
        host->setActiveTool(ScreenshotToolPalette::Tool::Move);
    }
    setHistoryState(SnowCanvasHistoryState{});
    prepareForDisplay();
}

void ScreenshotToolbarWindow::setScrollingScreenshotMode(bool enabled) {
    if (ScreenshotToolPaletteHost* host = paletteHost()) {
        host->setScrollingScreenshotMode(enabled);
    }
    prepareForDisplay();
}

void ScreenshotToolbarWindow::setActiveTool(ScreenshotToolPalette::Tool tool) {
    setActiveToolAndReposition(tool);
}

bool ScreenshotToolbarWindow::activateDrawingShortcut(const QString& toolId) {
    ScreenshotToolPalette* toolPalette = palette();
    return toolPalette != nullptr && toolPalette->activateDrawingShortcut(toolId);
}

void ScreenshotToolbarWindow::setHistoryState(const SnowCanvasHistoryState& state) {
    if (ScreenshotToolPalette* toolPalette = palette()) {
        toolPalette->setHistoryState(state);
    }
}

void ScreenshotToolbarWindow::setStyleToolbarState(const SnowCanvasStyleToolbarState& state) {
    if (ScreenshotToolPalette* toolPalette = palette()) {
        toolPalette->setStyleToolbarState(state);
    }
}

void ScreenshotToolbarWindow::setWatermarkConfig(const SnowCanvasWatermarkConfig& config) {
    if (ScreenshotToolPalette* toolPalette = palette()) {
        toolPalette->setWatermarkConfig(config);
    }
}

void ScreenshotToolbarWindow::setSpotlightConfig(const SnowCanvasSpotlightConfig& config) {
    if (ScreenshotToolPalette* toolPalette = palette()) {
        toolPalette->setSpotlightConfig(config);
    }
}

void ScreenshotToolbarWindow::setOcrBusy(bool busy) {
    if (ScreenshotToolPalette* toolPalette = palette()) {
        toolPalette->setOcrBusy(busy);
    }
}

void ScreenshotToolbarWindow::setTableBusy(bool busy) {
    if (ScreenshotToolPalette* toolPalette = palette()) {
        toolPalette->setTableBusy(busy);
    }
}

void ScreenshotToolbarWindow::setTableEditingState(bool available, bool canUndo, bool canRedo,
                                                   bool canMerge, bool canSplit, bool canReset) {
    if (ScreenshotToolPalette* toolPalette = palette()) {
        toolPalette->setTableEditingState(available, canUndo, canRedo, canMerge, canSplit,
                                          canReset);
    }
}

void ScreenshotToolbarWindow::setTextEditingState(bool available, bool editing, bool canUndo,
                                                   bool canRedo) {
    if (ScreenshotToolPalette* toolPalette = palette()) {
        toolPalette->setTextEditingState(available, editing, canUndo, canRedo);
    }
}

void ScreenshotToolbarWindow::setTextTranslationState(bool available, bool translating,
                                                       bool streaming, bool canUndo,
                                                       bool canRedo, bool canReset) {
    if (ScreenshotToolPalette* toolPalette = palette()) {
        toolPalette->setTextTranslationState(available, translating, streaming, canUndo, canRedo,
                                             canReset);
    }
}

void ScreenshotToolbarWindow::setTextTransformSelections(const QString& formatting,
                                                          const QString& punctuation) {
    if (ScreenshotToolPalette* toolPalette = palette()) {
        toolPalette->setTextTransformSelections(formatting, punctuation);
    }
}

void ScreenshotToolbarWindow::setQrBusy(bool busy) {
    if (ScreenshotToolPalette* toolPalette = palette()) {
        toolPalette->setQrBusy(busy);
    }
}

void ScreenshotToolbarWindow::clearTextTransformSelections() {
    setTextTransformSelections({}, {});
}

void ScreenshotToolbarWindow::setOcrEnabled(bool enabled) {
    if (ScreenshotToolPalette* toolPalette = palette()) {
        toolPalette->setOcrEnabled(enabled);
    }
}

void ScreenshotToolbarWindow::setTableEnabled(bool enabled) {
    if (ScreenshotToolPalette* toolPalette = palette()) {
        toolPalette->setTableEnabled(enabled);
    }
}

void ScreenshotToolbarWindow::setQrEnabled(bool enabled) {
    if (ScreenshotToolPalette* toolPalette = palette()) {
        toolPalette->setQrEnabled(enabled);
    }
}

void ScreenshotToolbarWindow::setPlacementContext(QScreen* screen, const QRect& logicalBounds,
                                                  const QRect& physicalBounds) {
    m_placementScreen = screen;
    m_movementLogicalBounds = logicalBounds;
    m_movementPhysicalBounds = physicalBounds;
    ScreenshotFloatingToolPaletteWindow::setPlacementContext(screen, logicalBounds, physicalBounds);
}

void ScreenshotToolbarWindow::setPlacementScreen(QScreen* screen) {
    setPlacementContext(screen, m_movementLogicalBounds, m_movementPhysicalBounds);
}

void ScreenshotToolbarWindow::setMovementBounds(const QRect& logicalBounds,
                                                const QRect& physicalBounds) {
    setPlacementContext(m_placementScreen, logicalBounds, physicalBounds);
    if (isVisible()) {
        const QPoint constrainedPosition = constrainedContentPosition(contentPosition());
        moveContentTo(constrainedPosition);
    }
}

void ScreenshotToolbarWindow::resetPositionForSelection(const QPoint& position) {
    moveContentTo(position);
    m_manuallyDragged = false;
}
