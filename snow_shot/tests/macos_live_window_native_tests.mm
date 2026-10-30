#include "snow_shot/presentation/livewindowsource.h"
#include "snow_shot/platform/macos/rastercolorspace.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QPainter>
#include <QScreen>
#include <QThread>
#include <QWidget>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#ifdef SNOW_SHOT_MACOS_LIVE_WINDOW_BENCHMARK
#include "../src/presentation/pinned/livewindowframerenderer_p.h"
#include <QTimer>
#include <sys/resource.h>
#include <mach/mach.h>
#endif

namespace {
QSize lastSampleSize;
QColor lastSampleColor;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <typename Predicate> bool waitFor(Predicate predicate, int timeout = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    QApplication::processEvents();
    return predicate();
}
class SourceWindow final : public QWidget {
  public:
    QColor color{32, 96, 192};
    bool patterned = false;
    SourceWindow() {
        setWindowTitle(QStringLiteral("Snow Shot live-window macOS native fixture"));
        setWindowFlag(Qt::WindowStaysOnTopHint);
        resize(320, 240);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), color);
        if (patterned) {
            for (int y = 0; y < height(); y += 8)
                for (int x = 0; x < width(); x += 8)
                    painter.fillRect(QRect(x, y, 8, 8),
                                     QColor((x * 7) % 256, (y * 11) % 256, (x + y) % 256));
        }
    }
};
CGWindowID nativeId(QWidget& window) {
    NSWindow* native = reinterpret_cast<NSView*>(window.winId()).window;
    return native ? static_cast<CGWindowID>(native.windowNumber) : 0;
}
QRect desktopBounds(CGWindowID id) {
    CFArrayRef windows = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, id);
    if (!windows)
        return {};
    QRect result;
    if (CFArrayGetCount(windows) == 1) {
        const auto info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windows, 0));
        const auto value =
            static_cast<CFDictionaryRef>(CFDictionaryGetValue(info, kCGWindowBounds));
        CGRect rectangle{};
        if (value && CGRectMakeWithDictionaryRepresentation(value, &rectangle))
            result = QRect(qRound(rectangle.origin.x), qRound(rectangle.origin.y),
                           qRound(rectangle.size.width), qRound(rectangle.size.height));
    }
    CFRelease(windows);
    return result;
}
bool hasColor(const QImage& image, const QColor& expected) {
    if (image.isNull())
        return false;
    const QColor actual = image.pixelColor(image.width() / 2, image.height() / 2);
    lastSampleSize = image.size();
    lastSampleColor = actual;
    return actual.alpha() >= 250 && qAbs(actual.red() - expected.red()) <= 3 &&
           qAbs(actual.green() - expected.green()) <= 3 &&
           qAbs(actual.blue() - expected.blue()) <= 3;
}
#ifdef SNOW_SHOT_MACOS_LIVE_WINDOW_BENCHMARK
#include "macos_live_window_performance_benchmark.h"
#endif
int runFixture() {
    using namespace snow_shot::presentation;
    if (QGuiApplication::platformName() != QStringLiteral("cocoa")) {
        std::cout << "A native Cocoa session is required.\n";
        return 77;
    }
    if (!CGPreflightScreenCaptureAccess()) {
        std::cout << "Screen Recording permission is required for native live capture.\n";
        return 77;
    }
    auto window = std::make_unique<SourceWindow>();
    window->move(100, 100);
    window->show();
    window->raise();
    window->activateWindow();
    [NSApp activate];
    require(waitFor([&] { return !desktopBounds(nativeId(*window)).isEmpty(); }),
            "The native source window must appear.");
    const auto id = nativeId(*window);
    const QPoint point = desktopBounds(id).center();
    std::optional<LiveWindowTarget> target;
    require(waitFor([&] {
                target = liveWindowTargetAtPoint(point, {});
                return target && target->nativeId == id;
            }),
            "Native lookup must retain exactly the selected top-level window.");
    const QRect selection(point - QPoint(20, 15), QSize(40, 30));
    auto source = createLiveWindowSource(*target, selection);
    require(bool(source), "A valid source must create a live source object.");
    struct Initialization {
        bool done = false;
        bool succeeded = false;
        QString error;
    };
    const auto initialization = std::make_shared<Initialization>();
    source->initialize([initialization](bool success, const QString& error) {
        initialization->succeeded = success;
        initialization->done = true;
        initialization->error = error;
    });
    require(waitFor([&] { return initialization->done; }), "Native capture must initialize.");
    if (!initialization->succeeded)
        throw std::runtime_error(initialization->error.toStdString());
    if (source->initialDesktopRegion() != selection ||
        !hasColor(source->initialFrame(), window->color)) {
        const QRect region = source->initialDesktopRegion();
        const QImage frame = source->initialFrame();
        const QColor actual = frame.pixelColor(frame.width() / 2, frame.height() / 2);
        std::cerr << "Initial capture: selected " << selection.x() << ',' << selection.y() << ' '
                  << selection.width() << 'x' << selection.height() << ", region " << region.x()
                  << ',' << region.y() << ' ' << region.width() << 'x' << region.height()
                  << ", frame " << frame.width() << 'x' << frame.height() << ", center RGBA "
                  << actual.red() << ',' << actual.green() << ',' << actual.blue() << ','
                  << actual.alpha() << '\n';
    }
    require(source->initialDesktopRegion() == selection &&
                hasColor(source->initialFrame(), window->color),
            "Initial capture must contain the selected unedited source-window pixels.");
    const QSize cropPixels = source->initialFrame().size();
    require(cropPixels.width() >= selection.width() && cropPixels.height() >= selection.height(),
            "Desktop points and the captured backing raster must stay separate.");
    const auto sample = [&](bool allowUnavailable = false) {
        struct Snapshot {
            bool done = false;
            QImage image;
            QString error;
        };
        const auto result = std::make_shared<Snapshot>();
        source->requestFrame([result](const QImage& frame, const QString& message) {
            result->image = frame;
            result->error = message;
            result->done = true;
        });
        require(waitFor([&] { return result->done; }), "A current-frame request must complete.");
        if (!allowUnavailable && !result->error.isEmpty())
            throw std::runtime_error(result->error.toStdString());
        return result->image;
    };
    require(hasColor(sample(), window->color),
            "An idle window must still provide an action-time snapshot.");

    QWidget shell;
    shell.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    shell.setAttribute(Qt::WA_TranslucentBackground);
    shell.setGeometry(550, 120, 160, 120);
    shell.show();
    QImage latest;
    const auto connection = QObject::connect(source.get(), &LiveWindowSource::frameChanged, &shell,
                                             [&](const QImage& image) { latest = image; });
    LiveWindowPresentation presentation;
    presentation.owner = &shell;
    QString error;
    require(source->setPresentation(presentation, &error) && source->usesNativePresentation(),
            "macOS display must present native GPU frames.");
    NSWindow* nativeShell = reinterpret_cast<NSView*>(shell.winId()).window;
    require(nativeShell.childWindows.count == 1, "The GPU surface is a child below the Qt shell.");
    NSWindow* host = nativeShell.childWindows.firstObject;
    require(host.ignoresMouseEvents && !host.canBecomeKeyWindow,
            "The native content surface never steals input from the shell.");
    QRect hostBounds;
    require(waitFor([&] {
                hostBounds = desktopBounds(static_cast<CGWindowID>(host.windowNumber));
                return !hostBounds.isEmpty();
            }),
            "WindowServer publishes the GPU surface before capture.");
    const LiveWindowTarget displayTarget{static_cast<quintptr>(host.windowNumber),
                                         NSProcessInfo.processInfo.processIdentifier, hostBounds,
                                         hostBounds.center()};
    auto displaySource = createLiveWindowSource(displayTarget, hostBounds);
    require(bool(displaySource), "The displayed surface has a valid capture target.");
    bool displayInitialized = false;
    displaySource->initialize([&](bool ok, const QString& message) {
        require(ok, qPrintable(message));
        displayInitialized = true;
    });
    require(waitFor([&] { return displayInitialized; }),
            "The displayed GPU surface can be sampled.");
    const auto displaySample = [&] {
        bool done = false;
        QImage image;
        displaySource->requestFrame([&](const QImage& frame, const QString&) {
            image = frame;
            done = true;
        });
        require(waitFor([&] { return done; }), "The displayed surface snapshot completes.");
        return image;
    };
    latest = {};
    window->color = QColor(192, 64, 32);
    window->repaint();
    require(waitFor([&] { return hasColor(displaySample(), window->color); }),
            "GPU surface pixels must update after the source changes.");
    require(latest.isNull(), "Continuous GPU display must not emit CPU image frames.");
    presentation.imageTransform.rotate(90);
    presentation.imageTransform.scale(-1, 1);
    require(source->setPresentation(presentation, &error),
            "Transforms must preserve the native source association.");
    window->color = QColor(48, 160, 96);
    window->repaint();
    require(waitFor([&] { return hasColor(displaySample(), window->color); }),
            "GPU presentation must continue while rotation and flipping are active.");
    require(latest.isNull(), "Rotated GPU display must not read back CPU pixels.");
    require(hasColor(sample(), window->color),
            "Transformed display must preserve raw source snapshots for processing.");
    window->patterned = true;
    window->repaint();
    QImage pattern;
    require(waitFor([&] {
                pattern = sample();
                return !pattern.isNull() && !hasColor(pattern, window->color);
            }),
            "The asymmetric source pattern reaches capture.");
    for (int angle : {0, 90, 180, 270}) {
        for (bool flip : {false, true}) {
            for (bool cropped : {false, true}) {
                presentation.imageTransform = QTransform().rotate(angle).scale(flip ? -1 : 1, 1);
                presentation.sourceViewport =
                    cropped ? QRectF(0.25, 0.25, 0.5, 0.5) : QRectF(0, 0, 1, 1);
                require(source->setPresentation(presentation), "GPU geometry changes succeed.");
                const QImage selected =
                    cropped ? pattern.copy(QRect(pattern.width() / 4, pattern.height() / 4,
                                                 pattern.width() / 2, pattern.height() / 2))
                            : pattern;
                const QImage oriented = selected.transformed(presentation.imageTransform);
                const bool matches = waitFor([&] {
                    const QImage actual = displaySample();
                    const QImage expected = oriented.scaled(actual.size(), Qt::IgnoreAspectRatio,
                                                            Qt::SmoothTransformation);
                    if (actual.isNull())
                        return false;
                    for (const QPointF point :
                         {QPointF(0.17, 0.19), QPointF(0.73, 0.23), QPointF(0.29, 0.81)}) {
                        const int x = qRound(point.x() * (actual.width() - 1));
                        const int y = qRound(point.y() * (actual.height() - 1));
                        const QColor a = actual.pixelColor(x, y), b = expected.pixelColor(x, y);
                        if (qAbs(a.red() - b.red()) > 5 || qAbs(a.green() - b.green()) > 5 ||
                            qAbs(a.blue() - b.blue()) > 5)
                            return false;
                    }
                    return true;
                });
                if (!matches) {
                    std::cerr << "GPU pattern mismatch: angle=" << angle << " flip=" << flip
                              << " cropped=" << cropped << '\n';
                }
                require(
                    matches,
                    "Displayed GPU pixels preserve crop, rotation, reflection, and backing scale.");
            }
        }
    }
    window->patterned = false;
    window->repaint();
    presentation.imageTransform.reset();
    presentation.sourceViewport = QRectF(0, 0, 1, 1);
    require(source->setPresentation(presentation, &error),
            "Resetting transforms must retain the live macOS stream.");
    displaySource->shutdown();
    displaySource.reset();

    QWidget occluder;
    occluder.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    occluder.setAutoFillBackground(true);
    QPalette palette = occluder.palette();
    palette.setColor(QPalette::Window, Qt::red);
    occluder.setPalette(palette);
    occluder.setGeometry(window->frameGeometry());
    occluder.show();
    occluder.raise();
    QApplication::processEvents();
    require(hasColor(sample(), window->color),
            "Independent window capture must exclude a covering window.");
    const auto throughHelper = liveWindowTargetAtPoint(point, {nativeId(occluder)});
    require(throughHelper && throughHelper->nativeId == id,
            "Target lookup must step below explicitly excluded helper windows.");
    occluder.hide();
    const QRect pinnedGeometry = shell.geometry();
    window->move(window->pos() + QPoint(100, 80));
    QApplication::processEvents();
    require(hasColor(sample(), window->color) && shell.geometry() == pinnedGeometry,
            "Moving the source must preserve its local crop and pin placement.");
    const QSize originalSize = window->size();
    window->resize(30, 30);
    QApplication::processEvents();
    require(waitFor([&] {
                const QImage image = sample(true);
                return !image.isNull() && image.size() == cropPixels &&
                       image.pixelColor(image.width() / 2, image.height() / 2).alpha() == 0;
            }),
            "Shrinking beyond the crop must leave a fixed-size blank region.");
    window->resize(originalSize);
    window->repaint();
    require(waitFor([&] { return hasColor(sample(true), window->color); }),
            "Regrowing the source must restore the corresponding crop content.");

    QRect desktop;
    for (const auto* screen : QGuiApplication::screens())
        desktop = desktop.united(screen->geometry());
    const QPoint visiblePosition = window->pos();
    window->move(desktop.bottomRight() + QPoint(400, 400));
    const QPoint cropOffset = selection.topLeft() - target->desktopBounds.topLeft();
    QRect offscreenBounds;
    QRect offscreenCrop;
    // AppKit constrains titled windows to leave a small draggable edge visible.
    // The selected central crop must nevertheless be wholly beyond the desktop.
    const bool cropIsOffscreen = waitFor([&] {
        offscreenBounds = desktopBounds(id);
        offscreenCrop = QRect(offscreenBounds.topLeft() + cropOffset, selection.size());
        return !offscreenBounds.isEmpty() && !offscreenCrop.intersects(desktop);
    });
    if (!cropIsOffscreen || nativeId(*window) != id)
        std::cerr << "Offscreen metadata: original ID " << id << ", current ID "
                  << nativeId(*window) << ", bounds " << offscreenBounds.x() << ','
                  << offscreenBounds.y() << ' ' << offscreenBounds.width() << 'x'
                  << offscreenBounds.height() << ", crop " << offscreenCrop.x() << ','
                  << offscreenCrop.y() << ' ' << offscreenCrop.width() << 'x'
                  << offscreenCrop.height() << '\n';
    require(
        cropIsOffscreen && nativeId(*window) == id,
        "The captured source region must move fully beyond the desktop with unchanged identity.");
    window->color = QColor(144, 48, 128);
    window->repaint();
    const bool updatedOffscreen = waitFor([&] { return hasColor(sample(true), window->color); });
    if (!updatedOffscreen)
        std::cerr << "Offscreen capture state " << static_cast<int>(source->state()) << ", "
                  << source->stateMessage().toStdString() << '\n';
    require(updatedOffscreen,
            "An offscreen crop must keep updating its pixels through native window capture.");
    window->move(visiblePosition);
    require(waitFor([&] { return desktopBounds(id).topLeft() == visiblePosition; }),
            "The source must return to its original visible desktop position.");
    require(hasColor(sample(), window->color),
            "Returning the source onscreen must retain its source-local crop.");

    window->showMinimized();
    const bool minimized = waitFor([&] { return source->state() == LiveWindowState::Minimized; });
    if (!minimized) {
        NSWindow* native = reinterpret_cast<NSView*>(window->winId()).window;
        const QRect bounds = desktopBounds(id);
        std::cerr << "Minimize state: Qt " << window->isMinimized() << ", native "
                  << (native ? native.miniaturized : false) << ", original ID " << id
                  << ", current ID " << nativeId(*window) << ", CG bounds " << bounds.width() << 'x'
                  << bounds.height() << ", source " << static_cast<int>(source->state()) << ", "
                  << source->stateMessage().toStdString() << '\n';
    }
    require(minimized, "Minimization must keep the live pin in the minimized state.");
    require(source->stateMessage() ==
                QStringLiteral("The window is minimized. The latest content cannot be displayed."),
            "The minimized message must match the specified text.");
    bool rejected = false;
    source->requestFrame([&](const QImage& image, const QString& message) {
        rejected = image.isNull() && !message.isEmpty();
    });
    require(rejected, "Minimized content must not be exported as a stale image.");
    window->showNormal();
    window->raise();
    require(waitFor([&] { return source->state() == LiveWindowState::Live; }),
            "Restoring the source must automatically resume live capture.");
    require(hasColor(sample(), window->color), "Restored capture must provide current pixels.");
    window.reset();
    require(waitFor([&] { return source->state() == LiveWindowState::Closed; }),
            "Closing the source must retain the pin with a terminal closed state.");
    require(source->stateMessage() ==
                QStringLiteral("The window is closed. The latest content cannot be displayed."),
            "The closed message must match the specified text.");
    QObject::disconnect(connection);
    source->shutdown();
    shell.close();
    std::cout << "Native macOS live-window capture, crop, and lifecycle checks passed.\n";
    return EXIT_SUCCESS;
}
} // namespace

int main(int argc, char** argv) {
    snow_shot::platform::macos::configureRasterColorSpace();
    QApplication application(argc, argv);
#ifdef SNOW_SHOT_MACOS_LIVE_WINDOW_BENCHMARK
    if (application.arguments().contains(QStringLiteral("--performance")))
        return runPerformance();
#endif
    if (application.arguments().contains(QStringLiteral("--request-permission"))) {
        if (CGPreflightScreenCaptureAccess() || CGRequestScreenCaptureAccess())
            return EXIT_SUCCESS;
        std::cout << "Enable Screen Recording for Snow Shot Live Window Tests in System Settings, "
                     "then rerun the fixture.\n";
        return 77;
    }
    try {
        return runFixture();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        if (!lastSampleSize.isEmpty())
            std::cerr << "Last sampled frame " << lastSampleSize.width() << 'x'
                      << lastSampleSize.height() << ", center RGBA " << lastSampleColor.red() << ','
                      << lastSampleColor.green() << ',' << lastSampleColor.blue() << ','
                      << lastSampleColor.alpha() << '\n';
        return EXIT_FAILURE;
    }
}
