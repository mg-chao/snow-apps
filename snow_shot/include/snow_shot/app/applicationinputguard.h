#pragma once

#include <QEvent>
#include <QObject>

namespace snow_shot::app {

// Keep painting and IPC responsive while application mutations are suspended.
class ApplicationInputGuard final : public QObject {
  public:
    explicit ApplicationInputGuard(QObject* parent) : QObject(parent) {}
    bool active = false;

    bool eventFilter(QObject*, QEvent* event) override {
        if (!active)
            return false;
        switch (event->type()) {
        case QEvent::Close:
        case QEvent::Quit:
            event->ignore();
            return true;
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
        case QEvent::Shortcut:
        case QEvent::ShortcutOverride:
        case QEvent::InputMethod:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
        case QEvent::Wheel:
        case QEvent::TabletPress:
        case QEvent::TabletMove:
        case QEvent::TabletRelease:
        case QEvent::ContextMenu:
        case QEvent::DragEnter:
        case QEvent::DragMove:
        case QEvent::Drop:
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
        case QEvent::TouchCancel:
        case QEvent::NativeGesture:
        case QEvent::Gesture:
        case QEvent::GestureOverride:
            return true;
        default:
            return false;
        }
    }
};

} // namespace snow_shot::app
