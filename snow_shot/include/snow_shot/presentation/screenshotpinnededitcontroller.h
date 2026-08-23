#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDEDITCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDEDITCONTROLLER_H

#include <QObject>
#include <QPointer>
#include <QPoint>
#include <QPointF>
#include <QMap>
#include <QRect>

#include <memory>

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
    [[nodiscard]] bool canvasColorSamplingActive() const;
    ScreenshotFloatingToolPaletteWindow* toolbarWindow() const;
    ScreenshotToolPaletteHost* toolbarHost() const;
    void setEditMode(bool enabled);
    void restoreDrawingToolState();
    void updatePlacement();
    void updateAfterPinnedWindowMove(const QPoint& logicalDelta);
    void updateCanvasColorSamplingAfterCursorMove(const QPoint& physicalPosition);
    void raiseToolbar();
    void hideToolbar();
    void destroyToolbar();

  signals:
    void editModeChanged(bool enabled);
    void textRecognitionRequested();
    void tableRecognitionRequested();
    void qrRecognitionRequested();
    void textTranslationRequested();

  private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void registerDrawingShortcuts();
    void reloadDrawingShortcuts();
    void registerRecognitionShortcuts();
    void reloadRecognitionShortcuts();
    QScreen* placementScreen() const;
    QRect placementLogicalBounds() const;
    QRect placementPhysicalBounds() const;
    void syncPaletteFromCanvasTool();
    void syncPaletteFromCanvasStyle();
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
    bool m_editMode = false;
    bool m_manuallyPlaced = false;
    bool m_updatingPlacement = false;
    bool m_canvasColorSamplingCursorOverridden = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDEDITCONTROLLER_H
