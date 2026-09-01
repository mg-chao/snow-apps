#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYUIHOST_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYUIHOST_H

#include "snow_shot/presentation/screenshotshortcuthints.h"

#include <QColor>
#include <QImage>
#include <QMetaObject>
#include <QObjectCleanupHandler>
#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QRect>
#include <QRectF>

class ScreenshotColorPickerWidget;
class ScreenshotOverlayWindow;
class ScreenshotSelectionToolbarCommandSink;
class ScreenshotSelectionToolbarWidget;
class ScreenshotToolbarCommandSink;
class ScreenshotToolbarWindow;
class SnowCanvasWidget;

class ScreenshotOverlayUiHost final {
  public:
    ScreenshotOverlayUiHost();
    ~ScreenshotOverlayUiHost();

    void setToolbarCommandSinks(ScreenshotToolbarCommandSink& toolbarCommands,
                                ScreenshotSelectionToolbarCommandSink& selectionToolbarCommands);

    ScreenshotToolbarWindow* ensureToolbar();
    ScreenshotToolbarWindow* toolbar() const;
    void attachToolbarToOverlay(ScreenshotOverlayWindow* overlay);
    void undoCanvasEdit();
    void redoCanvasEdit();
    ScreenshotSelectionToolbarWidget* selectionToolbar() const;
    void attachSelectionToolbarToOverlay(ScreenshotOverlayWindow* overlay);
    ScreenshotColorPickerWidget* ensureColorPicker();
    ScreenshotColorPickerWidget* colorPicker() const;
    void updateColorPicker(ScreenshotOverlayWindow* overlay, const QImage& image,
                           const QRect& physicalRect, const QPoint& physicalPoint,
                           const QPointF& localPosition, qreal opacity);
    void hideColorPicker();
    void setColorPickerCenterGuideLineColor(const QColor& color);
    void resetColorPickerForNewCapture();
    void hideColorPickerForOverlay(ScreenshotOverlayWindow* overlay) const;
    [[nodiscard]] bool colorPickerBelongsToOverlay(const ScreenshotOverlayWindow* overlay) const;
    [[nodiscard]] bool screenshotUiContainsGlobalCursor() const;
    void updateShortcutHints(ScreenshotOverlayWindow* overlay,
                             ScreenshotShortcutHintMode mode, qreal opacity,
                             const QRectF& selectionGlobal = {});
    void updateShortcutHints(ScreenshotOverlayWindow* overlay,
                             const ScreenshotShortcutHintContext& context, qreal opacity,
                             const QRectF& selectionGlobal = {});
    void hideShortcutHints();
    [[nodiscard]] bool stepToolbarStrokeWidth(int direction);
    [[nodiscard]] bool stepToolbarSelectionOpacity(int direction);
    [[nodiscard]] bool stepToolbarSpotlightOpacity(int direction);
    [[nodiscard]] bool stepToolbarFilterIntensity(int direction);
    [[nodiscard]] bool stepToolbarPenFilterStrokeWidth(int direction);
    [[nodiscard]] bool stepToolbarWatermarkFontSize(int direction);
    void resetToolbarForNewCapture();
    void hideToolbar();
    void showToolbar();
    void raiseToolbar();
    void hideSelectionToolbar();
    void showSelectionToolbar();
    void raiseSelectionToolbar();
    void detachOverlayTransientUi(ScreenshotOverlayWindow* overlay);
    void destroyUiResources();

  private:
    ScreenshotToolbarCommandSink* m_toolbarCommands = nullptr;
    ScreenshotSelectionToolbarCommandSink* m_selectionToolbarCommands = nullptr;
    QObjectCleanupHandler m_ownedWidgets;
    QPointer<ScreenshotToolbarWindow> m_toolbar;
    QPointer<SnowCanvasWidget> m_toolbarStyleCanvas;
    QMetaObject::Connection m_toolbarStyleConnection;
    QMetaObject::Connection m_toolbarHistoryConnection;
    QMetaObject::Connection m_toolbarStylePopupBeginConnection;
    QMetaObject::Connection m_toolbarStylePopupEndConnection;
    QPointer<ScreenshotSelectionToolbarWidget> m_selectionToolbar;
    QPointer<ScreenshotColorPickerWidget> m_colorPicker;
    QPointer<QWidget> m_shortcutHints;
    QColor m_colorPickerCenterGuideLineColor = QColor(0, 0, 0, 0);
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYUIHOST_H
