// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/imageeditingwindow.h"
#include "snow_shot/presentation/imageeditingcanvas.h"
#include "snow_shot/presentation/screenshotfloatingtoolpalettewindow.h"
#include "widgets/modal.h"

#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QEventLoop>
#include <QTimer>
#include <QWindow>
#include <stdexcept>
#include <utility>
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

bool cocoa() {
    return QGuiApplication::platformName() == QStringLiteral("cocoa");
}

NSWindow* native(QWidget& widget) {
    return reinterpret_cast<NSView*>(widget.winId()).window;
}

void settle() {
    QEventLoop loop;
    QObject::connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock,
                     &loop, &QEventLoop::quit, Qt::QueuedConnection);
    QTimer::singleShot(100, &loop, &QEventLoop::quit);
    loop.exec();
}

void requireAbove(QWidget& toolbar, QWidget& preview) {
    settle();
    require(toolbar.isVisible() && preview.isVisible(),
            "the editor preview and toolbar must remain visible");
    require(!toolbar.parentWidget() &&
                toolbar.windowHandle()->transientParent() == preview.windowHandle(),
            "the independent toolbar must retain the preview as its transient owner");
    if (!cocoa())
        return;
    CFArrayRef windows =
        CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    require(windows, "WindowServer must provide the visible window order");
    const auto list = reinterpret_cast<NSArray*>(windows);
    NSUInteger toolbarIndex = NSNotFound;
    NSUInteger previewIndex = NSNotFound;
    for (NSUInteger index = 0; index < list.count; ++index) {
        NSDictionary* info = list[index];
        const NSInteger number = [info[reinterpret_cast<NSString*>(kCGWindowNumber)] integerValue];
        if (number == native(toolbar).windowNumber)
            toolbarIndex = index;
        if (number == native(preview).windowNumber)
            previewIndex = index;
    }
    CFRelease(windows);
    require(toolbarIndex != NSNotFound && previewIndex != NSNotFound,
            "the preview and toolbar must both exist in WindowServer order");
    require(toolbarIndex < previewIndex,
            "raising the image preview must not cover its floating editing toolbar");
    require(native(toolbar).parentWindow == native(preview),
            "Cocoa must maintain child-window ordering above the image preview");
}

void requireDetached(QWidget& toolbar) {
    if (cocoa())
        require(native(toolbar).parentWindow == nil,
                "a hidden or unowned editing toolbar must release its Cocoa owner");
}
} // namespace

void runMacImageEditingToolbarStackingTests() {
    using namespace snow_shot::presentation;
    @autoreleasepool {
        ImageEditingSource source;
        source.image = QImage(320, 240, QImage::Format_ARGB32_Premultiplied);
        source.image.fill(Qt::white);
        source.canvasRect = QRectF(QPointF(), QSizeF(source.image.size()));
        ImageEditingWindow window;
        window.present(std::move(source));
        auto* preview = window.canvas()->window();
        auto* toolbar =
            qobject_cast<ScreenshotFloatingToolPaletteWindow*>(window.palette()->window());
        require(toolbar, "the editor must use the shared floating toolbar");
        // Overlap the image as a user can by dragging the floating toolbar.
        toolbar->moveContentTo(preview->geometry().center());
        toolbar->dragFinished();
        preview->raise();
        preview->activateWindow();
        requireAbove(*toolbar, *preview);

        for (const bool staysOnTop : {false, true}) {
            window.modal()->setWindowAlwaysOnTop(staysOnTop);
            preview->raise();
            requireAbove(*toolbar, *preview);

            window.modal()->windowInteractionStarted();
            require(!toolbar->isVisible(), "moving the preview must hide the toolbar");
            requireDetached(*toolbar);
            preview->move(preview->pos() + QPoint(5, 5));
            window.modal()->windowInteractionFinished();
            preview->raise();
            requireAbove(*toolbar, *preview);

            preview->hide();
            require(!toolbar->isVisible(), "hiding the preview must hide its toolbar");
            requireDetached(*toolbar);
            preview->show();
            preview->raise();
            requireAbove(*toolbar, *preview);

            toolbar->releaseNativeSurface();
            require(toolbar->internalWinId() == 0,
                    "the regression must exercise toolbar native-surface recreation");
            toolbar->restoreNativeSurface();
            toolbar->show();
            preview->raise();
            requireAbove(*toolbar, *preview);
        }

        toolbar->setTransientOwnerWindow(nullptr);
        require(!toolbar->windowHandle()->transientParent(),
                "clearing the toolbar owner must clear Qt ownership");
        requireDetached(*toolbar);
        toolbar->setTransientOwnerWindow(preview);
        preview->raise();
        requireAbove(*toolbar, *preview);
    }
}
