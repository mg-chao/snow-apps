#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDEDITCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDEDITCONTROLLER_H

#include <QObject>
#include <QPointer>
#include <QPoint>
#include <QPointF>
#include <QMap>
#include <QRect>
#include <QJsonObject>

#include <memory>
#include <optional>

#include "snow_shot/presentation/screenshotcanvascolorsampler.h"
#include "snow_draw_engine_qt/snow_canvas_types.h"

class QScreen;
class QEvent;
class QImage;
class ScreenshotCanvasColorSamplerWindow;
class ScreenshotFloatingToolPaletteWindow;
class ScreenshotPinnedWindow;
class ScreenshotToolPaletteHost;
class SnowCanvasWidget;
class ScreenshotAutoFilterController;
namespace adqt::widgets {
class AdColorPicker;
}
namespace snow_shot::presentation {
class WindowShortcutManager;
}

class ScreenshotPinnedEditController final : public QObject {
    Q_OBJECT

  public:
    ScreenshotPinnedEditController(ScreenshotPinnedWindow& pinnedWindow, SnowCanvasWidget& canvas,
                                   snow_shot::presentation::WindowShortcutManager& shortcutManager,
                                   QObject* parent = nullptr);
    ~ScreenshotPinnedEditController() override;

    bool editMode() const;
    [[nodiscard]] bool resizeWindowToolActive() const;
    [[nodiscard]] bool canvasColorSamplingActive() const;
    ScreenshotFloatingToolPaletteWindow* toolbarWindow() const;
    ScreenshotToolPaletteHost* toolbarHost() const;
    void setEditMode(bool enabled);
    bool automationSetTool(SnowCanvasTool tool);
    void activateResizeWindowTool();
    [[nodiscard]] bool beginTemporaryResizeWindowTool();
    void endTemporaryResizeWindowTool();
    void prepareRecognitionToolActivation();
    void beginNativeWindowInteraction();
    void endNativeWindowInteraction();
    void recognitionDeactivated();
    void syncCanvasInteractionState();
    void updatePlacement();
    void updateAfterPinnedWindowMove(const QPoint& logicalDelta);
    void updateCanvasColorSamplingAfterCursorMove(const QPoint& physicalPosition);
    void raiseToolbar();
    [[nodiscard]] bool automationAutoFilter(const QStringList& categories);
    [[nodiscard]] QJsonObject automationAutoFilterState() const;
    void cancelAutomationAutoFilter();

  signals:
    void toolbarCreated(ScreenshotFloatingToolPaletteWindow* toolbarWindow);
    void editModeChanged(bool enabled);
    void textRecognitionRequested();
    void tableRecognitionRequested();
    void qrRecognitionRequested();
    void textTranslationRequested();

  private:
    QStringList m_automationFilterCategories;
    QString m_automationFilterError;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void ensureToolbar();
    void destroyToolbar();
    void registerDrawingShortcuts();
    void reloadDrawingShortcuts();
    void registerRecognitionShortcuts();
    void reloadRecognitionShortcuts();
    QScreen* placementScreen() const;
    QRect placementLogicalBounds() const;
    QRect placementPhysicalBounds() const;
    void syncPaletteFromCanvasTool();
    void syncPaletteFromCanvasStyle();
    void activateCanvasTool(SnowCanvasTool tool);
    void applyResizeWindowTool();
    void applyShapeStyleFromPalette(const SnowCanvasShapeStyle& style, quint32 properties,
                                    SnowCanvasShapeKind kind);
    void applyTextStyleFromPalette(const SnowCanvasTextStyle& style);
    void applySerialNumberStyleFromPalette(const SnowCanvasSerialNumberStyle& style);
    void markToolbarManuallyPlaced();
    void beginCanvasColorSampling(adqt::widgets::AdColorPicker* picker);
    void cancelCanvasColorSampling();
    [[nodiscard]] QPoint canvasColorPhysicalPositionAt(const QPointF& localPosition) const;
    [[nodiscard]] QPoint canvasColorGlobalPositionAt(const QPoint& physicalPosition) const;
    [[nodiscard]] QImage canvasColorPreviewAtPhysicalPoint(const QPoint& physicalPosition);
    void updateCanvasColorSamplingPreviewAtPhysicalPoint(const QPoint& physicalPosition,
                                                         const QPoint& globalPosition);
    bool commitCanvasColorSampleAtPhysicalPoint(const QPoint& physicalPosition);
    void setCanvasColorSamplingCursor(bool enabled);
    [[nodiscard]] bool canvasInteractionAllowed() const;

    ScreenshotPinnedWindow& m_pinnedWindow;
    SnowCanvasWidget& m_canvas;
    snow_shot::presentation::WindowShortcutManager& m_shortcutManager;
    QMap<QString, quint64> m_drawingShortcutBindings;
    QMap<QString, quint64> m_recognitionShortcutBindings;
    ScreenshotFloatingToolPaletteWindow* m_toolbarWindow = nullptr;
    std::unique_ptr<ScreenshotCanvasColorSamplerWindow> m_canvasColorSamplerWindow;
    ScreenshotCanvasColorSampler m_canvasColorSampler;
    QPointer<adqt::widgets::AdColorPicker> m_canvasColorSamplingTarget;
    QMetaObject::Connection m_canvasColorSamplingDestroyedConnection;
    QPoint m_globalContentPosition;
    std::unique_ptr<ScreenshotAutoFilterController> m_autoFilterController;
    bool m_editMode = false;
    bool m_manuallyPlaced = false;
    bool m_updatingPlacement = false;
    bool m_resizeWindowToolActive = false;
    bool m_nativeWindowInteractionActive = false;
    bool m_recognitionToolActivationPending = false;
    std::optional<int> m_toolBeforeWindowResize;
    bool m_canvasColorSamplingCursorOverridden = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDEDITCONTROLLER_H
