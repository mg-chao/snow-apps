#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPRINTINTERACTIONGUARD_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPRINTINTERACTIONGUARD_H

#include <QApplication>
#include <QEvent>
#include <QPointer>
#include <QWidget>

// Native print UI does not participate in Qt's modal-window dispatch. Freeze only
// the capture's existing widgets, including the asynchronous snapshot preparation.
class ScreenshotPrintInteractionGuard final : public QObject {
  public:
    explicit ScreenshotPrintInteractionGuard(const QList<QWidget*>& windows) {
        for (auto* window : windows)
            if (window)
                m_windows.append(window);
        qApp->installEventFilter(this);
    }
    ~ScreenshotPrintInteractionGuard() override {
        release();
    }
    void release() {
        if (qApp)
            qApp->removeEventFilter(this);
        m_windows.clear();
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
        case QEvent::Wheel:
        case QEvent::ContextMenu:
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
        case QEvent::ShortcutOverride:
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
        case QEvent::Gesture:
        case QEvent::NativeGesture:
        case QEvent::DragEnter:
        case QEvent::DragMove:
        case QEvent::Drop:
            break;
        default:
            return false;
        }
        auto* widget = qobject_cast<QWidget*>(watched);
        for (const auto& window : m_windows) {
            if (window && widget && (window == widget || window->isAncestorOf(widget))) {
                event->accept();
                return true;
            }
        }
        return false;
    }

  private:
    QList<QPointer<QWidget>> m_windows;
};

#endif
