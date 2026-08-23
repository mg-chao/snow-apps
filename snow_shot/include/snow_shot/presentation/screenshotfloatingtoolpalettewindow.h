#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTFLOATINGTOOLPALETTEWINDOW_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTFLOATINGTOOLPALETTEWINDOW_H

#include "snow_shot/presentation/screenshottoolpalette.h"

#include <QByteArray>
#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QRect>
#include <QSize>
#include <QWidget>

class QEvent;
class QHideEvent;
class QPaintEvent;
class QScreen;
class QShowEvent;
class QWheelEvent;
class ScreenshotToolPaletteHost;
class ScreenshotFloatingToolPaletteWindowTestAccess;

namespace adqt::widgets {
struct AdDpiStableWindowDiagnostics;
class AdControlScaleScope;
class AdDpiStableWindowController;
} // namespace adqt::widgets

class ScreenshotFloatingToolPaletteWindow : public QWidget {
    Q_OBJECT

  public:
    explicit ScreenshotFloatingToolPaletteWindow(const ScreenshotToolPalette::Options& options,
                                                 QWidget* parent = nullptr);
    ~ScreenshotFloatingToolPaletteWindow() override;

    ScreenshotToolPalette* palette() const;
    ScreenshotToolPaletteHost* paletteHost() const;
    void setOwnerWindow(QWidget* owner);
    void setTransientOwnerWindow(QWidget* owner);
    // Releases the platform window while leaving this QObject alive for safe
    // deferred deletion from toolbar-triggered commands.
    void releaseNativeSurface();
    void setPlacementContext(QScreen* screen, const QRect& logicalBounds,
                             const QRect& physicalBounds = QRect());
    void setStyleToolbarAboveMain(bool above);
    void prepareForDisplay();
    void resetPhysicalSizeInvariant();
    void moveContentTo(const QPoint& position);
    void moveContentTo(const QPoint& position, const QSize& windowSize);
    QPoint contentPosition() const;
    QRect occupiedContentRect() const;
    QRect visualContentRect() const;
    QRect fullContentRect() const;
    QRect bottomPlacementContentRect() const;
    QRect topPlacementContentRect() const;
    QRect topRightMainToolbarContentRect() const;
    QSize contentSizeHint() const;
    QSize windowSizeHint() const;
    bool containsInteractiveGlobalPoint(const QPoint& globalPosition) const;
    bool stepStrokeWidth(int direction);
    bool stepSelectionOpacity(int direction);
    bool stepSpotlightOpacity(int direction);
    bool stepFilterIntensity(int direction);
    bool stepPenFilterStrokeWidth(int direction);
    bool stepWatermarkFontSize(int direction);
    void setMovementClampingEnabled(bool enabled);
    bool movementClampingEnabled() const;
    void cancelDrag();
    bool physicalDragActive() const;
    adqt::widgets::AdDpiStableWindowDiagnostics dpiTransitionDiagnostics() const;

  signals:
    void visibleContentChanged();
    void dragFinished();
    void dpiScaleCommitCompleted();

  protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

    QPoint constrainedContentPosition(const QPoint& position) const;
    QPointF constrainedContentPosition(const QPointF& position) const;
    void beginKeyboardFocusInteraction(QWidget* editor);
    void endKeyboardFocusInteraction(QWidget* editor = nullptr);
    void setPaletteScaleMultiplier(qreal multiplier);
    [[nodiscard]] qreal paletteScaleMultiplier() const;

  private:
    class GeometryUpdateTransaction final {
      public:
        explicit GeometryUpdateTransaction(ScreenshotFloatingToolPaletteWindow& owner);
        ~GeometryUpdateTransaction();

        GeometryUpdateTransaction(const GeometryUpdateTransaction&) = delete;
        GeometryUpdateTransaction& operator=(const GeometryUpdateTransaction&) = delete;

      private:
        ScreenshotFloatingToolPaletteWindow& m_owner;
    };

    struct GeometrySnapshot {
        QScreen* screen = nullptr;
        qreal devicePixelRatio = 0.0;
        quint64 paletteRevision = 0;
        QSize hostSize;
        QSize windowSize;
        QPoint contentOffset;
        QRect mainToolbarRect;
    };

    void updatePaletteGeometryForVisibleContent();
    void bindDynamicKeyboardEditors();
    void refreshPaletteWindow(bool forceRepaint = false);
    bool handleNativeHitTest(void* message, qintptr* result) const;
    bool isPointInInteractiveContent(const QPoint& localPosition) const;
    void handlePaletteContentChange();
    bool applyPlacementScreen();
    void ensureReferenceDevicePixelRatio();
    void syncPalettePhysicalScale();
    void refreshStablePhysicalWindowSize();
    void refreshGeometryForVisibleContent(bool preserveContentPosition, bool forceRepaint = false);
    void beginGeometryUpdate();
    void endGeometryUpdate();
    void requestGeometryUpdate(bool preserveContentPosition, bool forceRepaint);
    void drainGeometryUpdates();
    bool commitGeometryUpdate(bool preserveContentPosition);
    GeometrySnapshot geometrySnapshot() const;
    static bool geometrySnapshotsEqual(const GeometrySnapshot& lhs, const GeometrySnapshot& rhs);
    static QRect nativeWindowGeometryForPhysicalDrag(const QPointF& physicalCursorPosition,
                                                     const QPointF& physicalCursorToWindowOffset,
                                                     const QSize& stablePhysicalWindowSize);
    QRect mainToolbarContentRect() const;
    void updateMainToolbarPositionSnapshot();
    qreal currentWindowDevicePixelRatio() const;
    qreal targetDevicePixelRatio() const;
    void beginPaletteDrag(const QPoint& globalPosition);
    void updatePaletteDrag(const QPoint& globalPosition);
    void moveContentDuringDrag(const QPoint& position);
    void finishPaletteDrag(bool emitFinished);
    bool handleToolbarWheel(QWheelEvent* event);
    void applyWindowAttributes();
    QSize fixedWindowSizeHint() const;
    QPoint contentOffset() const;
    QPointF dragPositionForEvent(const QPoint& globalPosition) const;

    QWidget* m_panel = nullptr;
    ScreenshotToolPaletteHost* m_paletteHost = nullptr;
    adqt::widgets::AdControlScaleScope* m_scaleScope = nullptr;
    adqt::widgets::AdDpiStableWindowController* m_dpiController = nullptr;
    QRect m_movementLogicalBounds;
    QRect m_movementPhysicalBounds;
    QPoint m_lastRequestedContentPosition;
    QPointF m_lastDragPosition;
    QPointF m_dragContentPosition;
    QPointF m_dragPhysicalCursorToWindowOffset;
    QPointF m_lastPhysicalDragCursorPosition;
    QPoint m_lastMainToolbarGlobalTopLeft;
    QPointer<QScreen> m_placementScreen;
    QPointer<QWidget> m_transientOwnerWindow;
    QPointer<QWidget> m_keyboardFocusEditor;
    QPointer<QWidget> m_watermarkTextEditor;
    QSize m_stablePhysicalWindowSize;
    qreal m_referenceDevicePixelRatio = 0.0;
    qreal m_paletteScaleMultiplier = 1.0;
    qreal m_lastAppliedWindowDevicePixelRatio = 0.0;
    QSize m_lastAppliedHostSize;
    QPoint m_lastAppliedContentOffset;
    QRect m_lastAppliedMainToolbarContentRect;
    bool m_draggingPalette = false;
    bool m_dragPhysicalAnchorValid = false;
    bool m_lastRequestedContentPositionValid = false;
    bool m_lastMainToolbarGlobalTopLeftValid = false;
    bool m_movementClampingEnabled = false;
    bool m_processingNativeDpiChange = false;
    bool m_keyboardFocusInteractionActive = false;
    int m_geometryUpdateDepth = 0;
    bool m_drainingGeometryUpdates = false;
    bool m_geometryUpdatePending = false;
    bool m_pendingPreserveContentPosition = false;
    bool m_pendingForceRepaint = false;
    bool m_geometrySnapshotValid = false;
    GeometrySnapshot m_lastGeometrySnapshot;

#if defined(SNOW_SHOT_TEST_HOOKS)
    quint64 m_paletteGeometryRefreshCount = 0;
    quint64 m_windowResizeOrReanchorCount = 0;
    quint64 m_coalescedGeometryRequestCount = 0;
    quint64 m_committedGeometryPassCount = 0;
#endif

    friend class ScreenshotFloatingToolPaletteWindowTestAccess;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTFLOATINGTOOLPALETTEWINDOW_H
