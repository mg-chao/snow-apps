#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARWINDOW_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARWINDOW_H

#include "snow_shot/presentation/screenshotfloatingtoolpalettewindow.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QRect>

class QEnterEvent;
class QScreen;
class ScreenshotToolbarCommandSink;

class ScreenshotToolbarWindow final : public ScreenshotFloatingToolPaletteWindow {
    Q_OBJECT

  public:
    explicit ScreenshotToolbarWindow(ScreenshotToolbarCommandSink& commands,
                                     QWidget* parent = nullptr);
    void resetForNewCapture();
    void setToolbarSize(const QString& size);
    void setToolbarLayout(const snow_shot::storage::ScreenshotToolbarLayout& layout);
    void setScrollingScreenshotMode(bool enabled);
    void setActiveTool(ScreenshotToolPalette::Tool tool);
    [[nodiscard]] bool activateDrawingShortcut(const QString& toolId);
    void setHistoryState(const SnowCanvasHistoryState& state);
    void setStyleToolbarState(const SnowCanvasStyleToolbarState& state);
    void setWatermarkConfig(const SnowCanvasWatermarkConfig& config);
    void setSpotlightConfig(const SnowCanvasSpotlightConfig& config);
    void setOcrEnabled(bool enabled);
    void setOcrBusy(bool busy);
    void setTableEnabled(bool enabled);
    void setTableBusy(bool busy);
    void setQrEnabled(bool enabled);
    void setQrBusy(bool busy);
    void setTableEditingState(bool available, bool canUndo, bool canRedo, bool canMerge,
                              bool canSplit, bool canReset);
    void setTextEditingState(bool available, bool editing, bool canUndo = false,
                             bool canRedo = false);
    void setTextTranslationState(bool available, bool translating, bool streaming,
                                 bool canUndo = false, bool canRedo = false,
                                 bool canReset = false);
    void setTextTransformSelections(const QString& formatting, const QString& punctuation);
    void clearTextTransformSelections();
    void setPlacementContext(QScreen* screen, const QRect& logicalBounds,
                             const QRect& physicalBounds = QRect());
    void setPlacementScreen(QScreen* screen);
    void setMovementBounds(const QRect& logicalBounds, const QRect& physicalBounds = QRect());
    void resetPositionForSelection(const QPoint& position, const QSize& windowSize);

  protected:
    void enterEvent(QEnterEvent* event) override;

  private:
    void initializePalette();
    void connectToolCommands(ScreenshotToolPalette& toolPalette);
    void connectActionCommands(ScreenshotToolPalette& toolPalette);
    void connectStyleCommands(ScreenshotToolPalette& toolPalette);
    void connectSerialNumberCommands(ScreenshotToolPalette& toolPalette);
    void connectScrollingScreenshotCommands(ScreenshotToolPalette& toolPalette);
    void setActiveToolAndReposition(ScreenshotToolPalette::Tool tool);

    ScreenshotToolbarCommandSink& m_commands;
    QRect m_movementLogicalBounds;
    QRect m_movementPhysicalBounds;
    QScreen* m_placementScreen = nullptr;
    bool m_manuallyDragged = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARWINDOW_H
