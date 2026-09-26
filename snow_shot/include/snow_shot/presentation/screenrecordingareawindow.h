#ifndef SNOW_SHOT_PRESENTATION_SCREENRECORDINGAREAWINDOW_H
#define SNOW_SHOT_PRESENTATION_SCREENRECORDINGAREAWINDOW_H

#include "snow_shot/presentation/screenshottoolpalette.h"

#include <QRect>
#include <QMarginsF>
#include <QRectF>
#include <QWidget>

#include <memory>

class QEvent;
class QShowEvent;
class SnowCanvasRuntime;
class SnowCanvasWidget;

namespace snow_shot::presentation::recording {
class RecordingCountdownOverlay;
}

class ScreenRecordingAreaWindow final : public QWidget {
    Q_OBJECT

  public:
    enum class InputMode {
        PassThrough,
        Drawing,
        RegionEditing,
    };

    explicit ScreenRecordingAreaWindow(QWidget* parent = nullptr);
    ~ScreenRecordingAreaWindow() override;

    // Desktop points on macOS; physical desktop pixels on Windows.
    void setRecordingRegion(const QRect& region);
    [[nodiscard]] QRect recordingRegion() const;
    void setRecordingState(ScreenshotToolPalette::RecordingState state);
    void setInputMode(InputMode mode);
    [[nodiscard]] InputMode inputMode() const;
    // Focus the effective input owner; pass-through and blocked areas cannot activate.
    bool activateInput();
    void setDrawingBlocked(bool blocked);
    [[nodiscard]] bool drawingBlocked() const;
    void startCountdown(int seconds);
    void updateCountdown(qint64 remainingMilliseconds);
    void clearCountdown();
    [[nodiscard]] bool countdownActive() const;
    [[nodiscard]] QColor inputSurfaceColor() const;
    [[nodiscard]] SnowCanvasWidget* canvas() const;
    [[nodiscard]] SnowCanvasRuntime& canvasRuntime() {
        return *m_canvasRuntime;
    }
    [[nodiscard]] QRect canvasGeometry() const;
    [[nodiscard]] QRectF selectionRect() const {
        return m_selectionRect;
    }

  signals:
    void recordingRegionChanged(const QRect& region);
    void regionInteractionStarted();
    void regionInteractionFinished();
    void closeRequested();
    void drawingDeactivationRequested();
    void drawingWheelRequested(int direction);

  protected:
    bool event(QEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    friend class ScreenRecordingAreaWindowTestAccess;

    void applyInputMode();
    void updateRegionCursor(const QPointF& position);
    void applyQuickSelectionPreferences();
    void applyNativePassThrough(bool enabled);
    [[nodiscard]] bool regionEditingEnabled() const;
    [[nodiscard]] Qt::Edges resizeEdgesAt(const QPointF& position) const;
    void cancelRegionInteraction();
    void finishRegionInteraction();
    void beginRegionInteraction();
    void synchronizeWindowGeometry();
    void scheduleGeometrySynchronization();
    void layoutSelection();
    void layoutCountdownOverlay();

    QRectF m_frameRect;
    QRectF m_selectionRect;
    QRect m_recordingRegion;
    qreal m_paddingWidth = 0.0;
    ScreenshotToolPalette::RecordingState m_state = ScreenshotToolPalette::RecordingState::Idle;
    InputMode m_inputMode = InputMode::PassThrough;
    bool m_drawingBlocked = false;
    bool m_gestureInProgress = false;
    bool m_regionInteractionActive = false;
    bool m_settingRegion = false;
    bool m_geometrySyncPending = false;
    QMarginsF m_physicalInsets;
#ifdef Q_OS_MACOS
    QPoint m_regionDragOrigin;
    QRect m_regionDragRect;
    Qt::Edges m_regionDragEdges;
#endif
    std::unique_ptr<SnowCanvasRuntime> m_canvasRuntime;
    SnowCanvasWidget* m_canvas = nullptr;
    snow_shot::presentation::recording::RecordingCountdownOverlay* m_countdownOverlay = nullptr;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENRECORDINGAREAWINDOW_H
