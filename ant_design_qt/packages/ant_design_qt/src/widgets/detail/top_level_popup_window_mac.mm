#include "top_level_popup_window.h"

#include <QEvent>
#include <QGuiApplication>
#include <QPointer>
#include <QWidget>
#include <QWindow>
#import <AppKit/AppKit.h>

namespace adqt::widgets::detail {
namespace {
// Qt's transient parent sets a minimum window level on Cocoa, but does not
// establish NSWindow child ordering for tools. Equal-level popups can therefore
// disappear behind an owner that is raised during layout or activation.
class MacPopupOwnership final : public QObject {
  public:
    explicit MacPopupOwnership(QWidget* popup) : QObject(popup), popup_(popup) {
        popup->installEventFilter(this);
    }
    ~MacPopupOwnership() override {
        detach();
    }

    void sync() {
        QWindow* handle = popup_->windowHandle();
        if (handle != observedHandle_) {
            QObject::disconnect(ownerChanged_);
            observedHandle_ = handle;
            if (handle) {
                ownerChanged_ =
                    connect(handle, &QWindow::transientParentChanged, this, [this] { sync(); });
            }
        }
        NSWindow* child = popup_->internalWinId()
                              ? reinterpret_cast<NSView*>(popup_->internalWinId()).window
                              : nil;
        QWindow* ownerHandle = handle ? handle->transientParent() : nullptr;
        NSWindow* owner = ownerHandle && ownerHandle->handle()
                              ? reinterpret_cast<NSView*>(ownerHandle->winId()).window
                              : nil;
        if (!popup_->isVisible() || !child || !owner || child == owner) {
            detach();
            return;
        }
        if (child_ != child) {
            detach();
            child_ = [child retain];
        }
        if (child_.parentWindow != owner) {
            [child_.parentWindow removeChildWindow:child_];
            [owner addChildWindow:child_ ordered:NSWindowAbove];
        }
    }

  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        switch (event->type()) {
        case QEvent::ShowToParent:
        case QEvent::HideToParent:
        case QEvent::WinIdChange:
            sync();
            break;
        default:
            break;
        }
        return false;
    }

  private:
    void detach() {
        if (child_) {
            [child_.parentWindow removeChildWindow:child_];
            [child_ release];
            child_ = nil;
        }
    }
    QWidget* popup_;
    QPointer<QWindow> observedHandle_;
    QMetaObject::Connection ownerChanged_;
    NSWindow* child_ = nil;
};
} // namespace

void syncMacTopLevelPopupOwnership(QWidget* popup) {
    if (QGuiApplication::platformName() != QStringLiteral("cocoa")) {
        return;
    }
    MacPopupOwnership* ownership = nullptr;
    for (QObject* child : popup->children()) {
        if ((ownership = dynamic_cast<MacPopupOwnership*>(child))) {
            break;
        }
    }
    if (!ownership) {
        ownership = new MacPopupOwnership(popup);
    }
    ownership->sync();
}
} // namespace adqt::widgets::detail
