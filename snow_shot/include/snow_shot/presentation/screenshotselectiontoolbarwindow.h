#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONTOOLBARWINDOW_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONTOOLBARWINDOW_H

#include <QList>
#include <QMargins>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QWidget>

class QLabel;
class QByteArray;
class QCursor;
class QEvent;
class QHideEvent;
class QPaintEvent;
class QShowEvent;
class ScreenshotSelectionToolbarCommandSink;

class ScreenshotSelectionToolbarWindow final : public QWidget {
    Q_OBJECT

  public:
    enum class DisplayMode {
        Full,
        SizeOnly,
    };

    explicit ScreenshotSelectionToolbarWindow(ScreenshotSelectionToolbarCommandSink& commands,
                                              QWidget* parent = nullptr);

    void resetForNewCapture();
    void prepareForDisplay();
    void setSelectionState(const QRect& selection, bool aspectRatioLocked, int cornerRadius,
                           int shadowWidth, DisplayMode displayMode = DisplayMode::Full);
    QSize contentSizeHint() const;
    QRect visualContentRect() const;
    QPoint contentPosition() const;
    bool containsInteractiveGlobalPoint(const QPoint& globalPosition) const;
    void moveContentTo(const QPoint& position);

  private:
    enum class Field {
        PositionX,
        PositionY,
        Width,
        Height,
        Radius,
        Shadow,
    };

    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void showEvent(QShowEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

    QLabel* addValueLabel(const QString& tooltip, Field field, const QCursor& cursor);
    QLabel* addStaticLabel(const QString& text, const QString& tooltip = QString(),
                           const QMargins& margins = QMargins());
    QLabel* addIconLabel(const QString& tooltip);
    QWidget* addSeparator();
    void setToolbarHovered(bool hovered);
    void scheduleToolbarHoverSync();
    void refreshHoverVisuals();
    bool fieldForObject(QObject* object, Field* outField) const;
    void handleFieldWheel(Field field, int deltaY);
    bool shouldForwardPointerEventToOverlayCanvas(const QEvent* event) const;
    bool forwardPointerEventToOverlayCanvas(QEvent* event) const;
    bool isPointInInteractiveContent(const QPoint& localPosition) const;
    bool updateLabels(bool refreshGeometry = false);
    void retranslateUi();
    void updateLockIconPixmap();
    void updateIconPixmaps();
    void updateDisplayMode();
    void updateMouseEventTransparency();
    void updateWindowSize();
    QPoint contentOffset() const;

    ScreenshotSelectionToolbarCommandSink& m_commands;
    QWidget* m_panel = nullptr;
    QLabel* m_xLabel = nullptr;
    QLabel* m_yLabel = nullptr;
    QLabel* m_widthLabel = nullptr;
    QLabel* m_heightLabel = nullptr;
    QLabel* m_radiusLabel = nullptr;
    QLabel* m_shadowLabel = nullptr;
    QLabel* m_lockIconLabel = nullptr;
    QList<QWidget*> m_positionWidgets;
    QList<QWidget*> m_sizeWidgets;
    QList<QWidget*> m_editingWidgets;
    QRect m_selection;
    bool m_aspectRatioLocked = false;
    bool m_toolbarHovered = false;
    DisplayMode m_displayMode = DisplayMode::Full;
    int m_cornerRadius = 0;
    int m_shadowWidth = 0;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONTOOLBARWINDOW_H
