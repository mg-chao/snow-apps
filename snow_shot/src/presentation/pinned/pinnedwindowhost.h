#ifndef SNOW_SHOT_PRESENTATION_PINNEDWINDOWHOST_H
#define SNOW_SHOT_PRESENTATION_PINNEDWINDOWHOST_H

#include <QObject>
#include <QPointer>
#include <QWidget>
#include <QWindow>
#include <QRegion>
#include <memory>

class SnowCanvasView;
class QPainter;
namespace snow_shot::presentation {
class PinnedImagePresenter;
class PinnedNativeWindow;
class PinnedWidgetWindow;

// Lifecycle and event host shared by the native Windows surface and QWidget
// platforms. The controller never derives its physical geometry from this
// host's rounded Qt geometry.
class PinnedWindowHost : public QObject {
    Q_OBJECT
  public:
    explicit PinnedWindowHost(QWidget* parent = nullptr);
    ~PinnedWindowHost() override;
    bool usesNativeImagePresentation() const;
    QWidget* widgetHost() const;
    QWindow* windowHandle() const;
    QScreen* screen() const;
    void setScreen(QScreen* screen);
    WId winId();
    WId internalWinId() const;
    QRect geometry() const;
    QRect frameGeometry() const;
    QRect rect() const;
    QSize size() const;
    int width() const;
    int height() const;
    QPoint pos() const;
    qreal devicePixelRatioF() const;
    void resize(const QSize& size);
    void resize(int width, int height);
    void move(const QPoint& point);
    void setGeometry(const QRect& geometry);
    void setWindowFlags(Qt::WindowFlags flags);
    Qt::WindowFlags windowFlags() const;
    void overrideWindowFlags(Qt::WindowFlags flags);
    void setAttribute(Qt::WidgetAttribute attribute, bool enabled = true);
    bool testAttribute(Qt::WidgetAttribute attribute) const;
    void setMinimumSize(int width, int height);
    void setFocusPolicy(Qt::FocusPolicy policy);
    void setMouseTracking(bool enabled);
    void setAcceptDrops(bool enabled);
    void setWindowTitle(const QString& title);
    void setWindowOpacity(qreal opacity);
    qreal windowOpacity() const;
    void ensurePolished();
    QLayout* layout() const;
    QFont font() const;
    QPalette palette() const;
    QPoint mapFromGlobal(const QPoint& point) const;
    QPointF mapFromGlobal(const QPointF& point) const;
    QPoint mapToGlobal(const QPoint& point) const;
    QPointF mapToGlobal(const QPointF& point) const;
    void setFocus(Qt::FocusReason reason = Qt::OtherFocusReason);
    void clearFocus();
    bool hasFocus() const;
    bool isActiveWindow() const;
    bool isVisible() const;
    bool isHidden() const;
    void setVisible(bool visible);
    void activateWindow();
    void raise();
    void setCursor(const QCursor& cursor);
    void unsetCursor();
    QCursor cursor() const;
    void grabMouse();
    void releaseMouse();
    void update();
    void update(const QRegion& dirty);
    void repaint();
    bool publishNativeFrame();
    bool applyNativeGeometry(const QRect& pixels);
    bool nativePresentationInProgress() const {
        return m_publishing || m_applyingGeometry;
    }
    void setCanvasView(SnowCanvasView* view);
    void attachAuxiliary(QWidget* widget, bool sharesOpacity = true);

  public slots:
    void show();
    void hide();
    bool close();

  protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    virtual bool nativeEvent(const QByteArray&, void*, qintptr*) {
        return false;
    }
    virtual void changeEvent(QEvent*) {}
    virtual void closeEvent(QCloseEvent* event);
    virtual void contextMenuEvent(QContextMenuEvent*) {}
    virtual void paintEvent(QPaintEvent*) {}
    virtual void resizeEvent(QResizeEvent*) {}
    virtual void moveEvent(QMoveEvent*) {}
    virtual void showEvent(QShowEvent*) {}
    virtual void mousePressEvent(QMouseEvent*) {}
    virtual void mouseDoubleClickEvent(QMouseEvent*) {}
    virtual void mouseReleaseEvent(QMouseEvent*) {}
    virtual void wheelEvent(QWheelEvent*) {}
    virtual void dragEnterEvent(QDragEnterEvent*) {}
    virtual void dragMoveEvent(QDragMoveEvent*) {}
    virtual void dragLeaveEvent(QDragLeaveEvent*) {}
    virtual void dropEvent(QDropEvent*) {}
    virtual bool paintNativeFrame(QPainter&, const QRegion&) {
        return false;
    }
    virtual void nativeFramePublished() {}
    virtual void nativeFrameRejected() {}

  private:
    friend class PinnedNativeWindow;
    friend class PinnedWidgetWindow;
    friend class PinnedWindowHostTestAccess;
    QPointer<QWidget> m_widget;
    std::unique_ptr<QWindow> m_native;
    std::unique_ptr<PinnedImagePresenter> m_presenter;
    QPointer<SnowCanvasView> m_view;
    QList<QPointer<QWidget>> m_hiddenAuxiliaries;
    QList<QPointer<QWidget>> m_frameAuxiliaries;
    QRegion m_dirty;
    bool m_frameScheduled = false;
    bool m_publishing = false;
    bool m_applyingGeometry = false;
    bool m_deleteOnClose = false;
    qreal m_opacity = 1.0;
    qreal m_committedOpacity = 1.0;
};
} // namespace snow_shot::presentation
#endif
