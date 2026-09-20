#include "snow_shot/platform/screenshotnative.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"

#import <AppKit/AppKit.h>
#include <QApplication>
#include <QEvent>
#include <QScreen>
#include <QWidget>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
class OverlayFixture final : public QWidget {
  public:
    OverlayFixture()
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {}
    void recreateSurface() {
        destroy();
    }

  protected:
    bool event(QEvent* event) override {
        const bool handled = QWidget::event(event);
        if (event->type() == QEvent::Show)
            snow_shot::platform::configureScreenshotOverlayWindow(this);
        return handled;
    }
};
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const bool cocoa = QGuiApplication::platformName() == QStringLiteral("cocoa");
    for (QScreen* screen : QGuiApplication::screens()) {
        auto display = ScreenshotGeometryMapper::preCaptureDisplayModel(*screen);
        require(display.active && display.image.isNull(), "preparation must not require an image");
        require(display.logicalRect == screen->geometry(),
                "preparation must cover the full screen");
        if (cocoa) {
            require(display.nativeDisplayId != 0 &&
                        display.stableId ==
                            QStringLiteral("display:%1").arg(display.nativeDisplayId),
                    "initial selector results must have a matching native display identity");
            require(display.canvasUsesPoints && display.capturedLogicalRect == screen->geometry(),
                    "the initial canvas must use the captured frame's point coordinate space");
        }
        ScreenshotDisplaySession session;
        session.appendDisplay(display);
        ScreenshotGeometryMapper geometry;
        geometry.rebuild(session);
        const QRectF physical = display.physicalRect;
        const QRectF expected = session.displayAt(0).canvasRect;
        require(geometry.canvasRectForPhysicalRect(session, physical, display.stableId) == expected,
                "a selection received before the image must map onto the initial canvas");
        require(!expected.isEmpty(),
                "the initial selection must be visible without mouse movement");
        if (cocoa)
            require(expected.size() == QSizeF(screen->geometry().size()),
                    "initial Retina selection must not be enlarged by the backing scale");
    }
    OverlayFixture overlay;
    overlay.resize(32, 32);
    for (int attempt = 0; attempt != 2; ++attempt) {
        overlay.show();
        QCoreApplication::processEvents();
        if (cocoa) {
            NSWindow* window = reinterpret_cast<NSView*>(overlay.winId()).window;
            require(window.level > CGWindowLevelForKey(kCGMainMenuWindowLevelKey) &&
                        window.level > CGWindowLevelForKey(kCGDockWindowLevelKey),
                    "the overlay must stay above the menu bar and Dock on each reveal");
            require(!window.hidesOnDeactivate, "the overlay must remain visible when deactivated");
            require((window.collectionBehavior & NSWindowCollectionBehaviorFullScreenAuxiliary) !=
                        0,
                    "the overlay must support full-screen application spaces");
        }
        overlay.hide();
        overlay.recreateSurface();
    }
    return 0;
}
