#ifndef SNOW_SHOT_PRESENTATION_LIVEWINDOWSOURCE_H
#define SNOW_SHOT_PRESENTATION_LIVEWINDOWSOURCE_H

#include <QObject>
#include <QColor>
#include <QImage>
#include <QMargins>
#include <QPointer>
#include <QRect>
#include <QTransform>
#include <QVector>
#include <functional>
#include <memory>
#include <optional>

class QWidget;

namespace snow_shot::presentation {

enum class LiveWindowState { Initializing, Live, Minimized, Closed, Unavailable };

// Desktop coordinates are physical pixels on Windows and desktop points on macOS.
// Native identities are session-scoped and must never be persisted.
struct LiveWindowTarget {
    quintptr nativeId = 0;
    qint64 processId = 0;
    QRect desktopBounds;
    QPoint hitPoint;
    // Keep the selected title available after the native window has closed.
    QString title = {};
};

struct LiveWindowPresentation {
    QPointer<QWidget> owner;
    // Insets from the owner's native client rectangle, in physical pixels on Windows.
    // Placement and size belong to the owner; source refreshes never supply desktop geometry.
    QMargins nativeContentMargins;
    qreal effectiveOpacity = 1;
    QColor backgroundColor = Qt::white;
    bool alwaysOnTop = true;
    bool clickThrough = false;
    bool visible = true;
    QTransform imageTransform;
    // Untransformed normalized region of the initially captured content.
    QRectF sourceViewport = QRectF(0, 0, 1, 1);
};

class LiveWindowSource : public QObject {
    Q_OBJECT
  public:
    using InitializationCompletion = std::function<void(bool, const QString&)>;
    using FrameCompletion = std::function<void(const QImage&, const QString&)>;

    using QObject::QObject;
    ~LiveWindowSource() override = default;
    virtual void initialize(InitializationCompletion completion) = 0;
    virtual QImage initialFrame() const = 0;
    // The shell owns its first painted image after successful presentation.
    virtual void releaseInitialFrame() = 0;
    virtual QRect initialDesktopRegion() const = 0;
    virtual LiveWindowState state() const = 0;
    virtual QString stateMessage() const = 0;
    virtual QString windowTitle() const = 0;
    virtual void requestFrame(FrameCompletion completion) = 0;
    virtual bool setPresentation(const LiveWindowPresentation& presentation,
                                 QString* error = nullptr) = 0;
    virtual bool usesNativePresentation() const = 0;
    virtual void shutdown() = 0;

  signals:
    void frameChanged(const QImage& frame);
    void stateChanged(snow_shot::presentation::LiveWindowState state, const QString& message);
    void nativePresentationChanged(bool native);
    void stopped();
};

std::optional<LiveWindowTarget> liveWindowTargetAtPoint(const QPoint& nativeDesktopPoint,
                                                        const QVector<quintptr>& excluded);
std::shared_ptr<LiveWindowSource> createLiveWindowSource(const LiveWindowTarget& target,
                                                         const QRect& nativeDesktopSelection);

} // namespace snow_shot::presentation

Q_DECLARE_METATYPE(snow_shot::presentation::LiveWindowState)

#endif
