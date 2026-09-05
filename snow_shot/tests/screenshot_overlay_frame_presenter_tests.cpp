#include "screenshotoverlayframepresenter.h"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QPaintEvent>
#include <QPainter>
#include <QScreen>
#include <QtNumeric>
#include <QVBoxLayout>
#include <QWidget>

#if defined(Q_OS_WIN)
#include <dwmapi.h>
#include <qt_windows.h>
#endif

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void revealStrategiesHaveExplicitCommitPlans() {
    const auto fallback = ScreenshotOverlayRevealStrategy::NativeUpdate;
    require(
        ScreenshotOverlayFramePresenter::strategyForName(QByteArrayLiteral("repaint"), fallback) ==
            ScreenshotOverlayRevealStrategy::SingleRepaint,
        "the repaint alias should select the single-repaint reveal strategy");
    require(ScreenshotOverlayFramePresenter::strategyForName(QByteArrayLiteral("invalid"),
                                                             fallback) == fallback,
            "an unknown reveal strategy should preserve the requested fallback");

    const ScreenshotOverlayRevealPlan singleRepaint =
        ScreenshotOverlayFramePresenter::planFor(ScreenshotOverlayRevealStrategy::SingleRepaint);
    require(!singleRepaint.suppressShowPaint && singleRepaint.repaint &&
                !singleRepaint.sendPostedUpdate && !singleRepaint.nativeUpdate &&
                !singleRepaint.nativeInvalidate,
            "the single-repaint reveal should issue exactly one explicit commit request");

    require(ScreenshotOverlayFramePresenter::strategyForName(
                QByteArrayLiteral("native-invalidate-suppressed"), fallback) ==
                ScreenshotOverlayRevealStrategy::NativeInvalidateSuppressed,
            "the redraw-suppressed native reveal strategy should be selectable by name");
    const ScreenshotOverlayRevealPlan suppressedNativeInvalidate =
        ScreenshotOverlayFramePresenter::planFor(
            ScreenshotOverlayRevealStrategy::NativeInvalidateSuppressed);
    require(suppressedNativeInvalidate.suppressShowPaint && !suppressedNativeInvalidate.repaint &&
                !suppressedNativeInvalidate.sendPostedUpdate &&
                !suppressedNativeInvalidate.nativeUpdate &&
                suppressedNativeInvalidate.nativeInvalidate,
            "the redraw-suppressed native reveal should suppress show and commit once");

    const ScreenshotOverlayRevealPlan nativeUpdate =
        ScreenshotOverlayFramePresenter::planFor(ScreenshotOverlayRevealStrategy::NativeUpdate);
    require(!nativeUpdate.suppressShowPaint && !nativeUpdate.repaint &&
                !nativeUpdate.sendPostedUpdate && nativeUpdate.nativeUpdate &&
                !nativeUpdate.nativeInvalidate,
            "the native-update reveal should issue exactly one explicit commit request");
}

QImage patternedOpaqueBgra(const QSize& size) {
    QImage image(size, QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            row[x] = qRgba((x * 3 + y) & 0xff, (y * 5) & 0xff, (x + y * 7) & 0xff, 255);
        }
    }
    return image;
}

QImage blitImage(const QImage& source) {
    QImage destination(source.size(), QImage::Format_ARGB32_Premultiplied);
    destination.fill(Qt::transparent);
    QPainter painter(&destination);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.drawImage(QPoint(0, 0), source);
    painter.end();
    return destination;
}

void opaqueRgb32BlitMatchesArgb32() {
    const QImage argb = patternedOpaqueBgra(QSize(960, 540));
    require(argb.hasAlphaChannel(), "ARGB32 fixtures must report an alpha channel");
    const QImage rgb32(argb.constBits(), argb.width(), argb.height(), argb.bytesPerLine(),
                       QImage::Format_RGB32);
    require(!rgb32.hasAlphaChannel(), "RGB32 must report no alpha channel");

    const QImage fromArgb = blitImage(argb);
    const QImage fromRgb32 = blitImage(rgb32);
    require(fromArgb == fromRgb32,
            "opaque RGB32 and ARGB32 BGRA buffers must blit to the same pixels");

    constexpr int iterations = 4;
    QElapsedTimer timer;
    timer.start();
    for (int iteration = 0; iteration < iterations; ++iteration) {
        static_cast<void>(blitImage(argb));
    }
    const qint64 argbNanoseconds = timer.nsecsElapsed();
    timer.restart();
    for (int iteration = 0; iteration < iterations; ++iteration) {
        static_cast<void>(blitImage(rgb32));
    }
    const qint64 rgb32Nanoseconds = timer.nsecsElapsed();
    std::cerr << "blit-format mean_ms argb32="
              << (static_cast<double>(argbNanoseconds) / iterations / 1e6)
              << " rgb32=" << (static_cast<double>(rgb32Nanoseconds) / iterations / 1e6) << '\n';
}

#if defined(Q_OS_WIN)
class RevealProbeCanvas final : public QWidget {
  public:
    explicit RevealProbeCanvas(QWidget* parent) : QWidget(parent) {}

    void setFrameColor(const QColor& color) {
        m_color = color;
        update();
    }

    void resetPaintCount() {
        m_paintCount = 0;
    }

    [[nodiscard]] int paintCount() const {
        return m_paintCount;
    }

  protected:
    void paintEvent(QPaintEvent* event) override {
        ++m_paintCount;
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(event->rect(), m_color);
    }

  private:
    QColor m_color = Qt::transparent;
    int m_paintCount = 0;
};

class RevealProbeWindow final : public QWidget {
  public:
    RevealProbeWindow() : m_canvas(new RevealProbeCanvas(this)) {
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(m_canvas);
    }

    void setFrameColor(const QColor& color) {
        m_canvas->setFrameColor(color);
    }

    void resetPaintCount() {
        m_canvas->resetPaintCount();
    }

    [[nodiscard]] int paintCount() const {
        return m_canvas->paintCount();
    }

  private:
    RevealProbeCanvas* m_canvas = nullptr;
};

QPoint nativeGlobalPosition(QWidget& window, const QPoint& localPosition) {
    const HWND hwnd = reinterpret_cast<HWND>(window.winId());
    require(hwnd != nullptr, "native reveal test could not access the probe HWND");
    POINT native{localPosition.x(), localPosition.y()};
    require(ClientToScreen(hwnd, &native) != FALSE,
            "native reveal test could not map the probe point to the desktop");
    return QPoint(native.x, native.y);
}

COLORREF desktopPixel(const QPoint& position) {
    HDC screen = GetDC(nullptr);
    require(screen != nullptr, "native reveal test could not access the desktop DC");
    const COLORREF pixel = GetPixel(screen, position.x(), position.y());
    ReleaseDC(nullptr, screen);
    require(pixel != CLR_INVALID, "native reveal test could not read the desktop pixel");
    return pixel;
}

bool colorNear(COLORREF actual, const QColor& expected, int tolerance = 12) {
    return std::abs(GetRValue(actual) - expected.red()) <= tolerance &&
           std::abs(GetGValue(actual) - expected.green()) <= tolerance &&
           std::abs(GetBValue(actual) - expected.blue()) <= tolerance;
}

void preparedRevealPublishesExactlyOneFreshFrame() {
    require(QGuiApplication::platformName() == QStringLiteral("windows"),
            "native reveal test requires the Windows Qt platform");
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "native reveal test requires a primary screen");

    constexpr QSize probeSize(192, 128);
    const QRect available = screen->availableGeometry();
    require(available.width() >= probeSize.width() + 64 &&
                available.height() >= probeSize.height() + 64,
            "native reveal test requires a 256 by 192 pixel desktop area");

    RevealProbeWindow window;
    window.setGeometry(QRect(available.topLeft() + QPoint(32, 32), probeSize));
    static_cast<void>(window.winId());

    const QColor staleColor(194, 33, 71);
    const QColor preparedColor(20, 173, 109);
    window.setFrameColor(staleColor);
    window.show();
    window.repaint();
    QApplication::processEvents();
    require(SUCCEEDED(DwmFlush()), "native reveal test could not flush the initial DWM frame");

    const QPoint samplePosition = nativeGlobalPosition(window, window.rect().center());
    require(colorNear(desktopPixel(samplePosition), staleColor),
            "native reveal test could not observe the stale frame fixture");

    window.hide();
    QApplication::processEvents();
    window.setFrameColor(preparedColor);
    window.resetPaintCount();

    ScreenshotOverlayFramePresenter presenter(window);
    presenter.setStrategyForTesting(ScreenshotOverlayRevealStrategy::NativeInvalidateSuppressed);
    presenter.presentPreparedFrame();
    const int immediatePaintCount = window.paintCount();
    require(SUCCEEDED(DwmFlush()), "native reveal test could not flush the prepared DWM frame");
    QApplication::processEvents();
    require(SUCCEEDED(DwmFlush()), "native reveal test could not flush deferred presentation work");
    const int settledPaintCount = window.paintCount();

    require(window.isVisible(), "prepared reveal left the probe logically hidden");
    require(IsWindowVisible(reinterpret_cast<HWND>(window.winId())) != FALSE,
            "prepared reveal left the native probe hidden");

    const COLORREF revealedPixel = desktopPixel(samplePosition);
    if (!colorNear(revealedPixel, preparedColor)) {
        std::cerr << "native reveal pixel: rgb(" << static_cast<int>(GetRValue(revealedPixel))
                  << ',' << static_cast<int>(GetGValue(revealedPixel)) << ','
                  << static_cast<int>(GetBValue(revealedPixel)) << ")\n";
    }
    require(colorNear(revealedPixel, preparedColor),
            "prepared reveal exposed a stale composited frame");
    if (immediatePaintCount != 1 || settledPaintCount != 1) {
        std::cerr << "native reveal paint counts: immediate=" << immediatePaintCount
                  << ", settled=" << settledPaintCount << '\n';
    }
    require(immediatePaintCount == 1 && settledPaintCount == 1,
            "prepared reveal must publish the frame with exactly one paint event");

    const QColor subsequentColor(47, 91, 213);
    window.setFrameColor(subsequentColor);
    QApplication::processEvents();
    require(SUCCEEDED(DwmFlush()), "native reveal test could not flush a subsequent DWM frame");
    require(colorNear(desktopPixel(samplePosition), subsequentColor),
            "prepared reveal left subsequent widget updates unable to publish");
    require(window.paintCount() == 2,
            "the first post-reveal update should add exactly one paint event");
    window.hide();
}

void warmedSurfaceRevealSkipsFirstShowAndRestoresOpacity() {
    require(QGuiApplication::platformName() == QStringLiteral("windows"),
            "native reveal test requires the Windows Qt platform");
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "native reveal test requires a primary screen");

    constexpr QSize probeSize(192, 128);
    const QRect available = screen->availableGeometry();
    require(available.width() >= probeSize.width() + 64 &&
                available.height() >= probeSize.height() + 64,
            "native reveal test requires a 256 by 192 pixel desktop area");

    RevealProbeWindow window;
    window.setGeometry(QRect(available.topLeft() + QPoint(32, 32), probeSize));
    static_cast<void>(window.winId());

    const QColor preparedColor(20, 173, 109);
    window.setFrameColor(preparedColor);
    window.resetPaintCount();

    ScreenshotOverlayFramePresenter presenter(window);
    presenter.setStrategyForTesting(ScreenshotOverlayRevealStrategy::NativeInvalidateSuppressed);
    presenter.warmPresentationSurface();
    require(SUCCEEDED(DwmFlush()), "native warm test could not flush the concealed DWM surface");

    require(window.isVisible(), "surface warmup left the probe logically hidden");
    require(IsWindowVisible(reinterpret_cast<HWND>(window.winId())) != FALSE,
            "surface warmup left the native probe hidden");
    require(qFuzzyIsNull(window.windowOpacity()),
            "surface warmup must keep the probe at opacity 0");
    require(window.paintCount() == 1,
            "surface warmup must create the layered bitmap with one paint");

    const QPoint samplePosition = nativeGlobalPosition(window, window.rect().center());
    require(!colorNear(desktopPixel(samplePosition), preparedColor),
            "a warmed overlay must stay visually concealed until reveal");

    window.resetPaintCount();
    presenter.presentPreparedFrame();
    require(SUCCEEDED(DwmFlush()), "native warm test could not flush the revealed DWM frame");
    QApplication::processEvents();
    require(SUCCEEDED(DwmFlush()), "native warm test could not flush deferred presentation work");

    require(window.isVisible(), "warmed reveal left the probe logically hidden");
    require(qAbs(window.windowOpacity() - 1.0) < 0.001,
            "warmed reveal must restore full window opacity");
    if (window.paintCount() != 1) {
        std::cerr << "warmed reveal paint count=" << window.paintCount() << '\n';
    }
    require(window.paintCount() == 1,
            "warmed reveal must publish the prepared frame with exactly one paint event");
    require(colorNear(desktopPixel(samplePosition), preparedColor),
            "warmed reveal exposed a stale or concealed composited frame");
    window.hide();
}
#endif
} // namespace

int main(int argc, char** argv) {
#if defined(Q_OS_WIN)
    for (int index = 1; index < argc; ++index) {
        if (QByteArray(argv[index]) != QByteArrayLiteral("--native-reveal")) {
            continue;
        }
        QApplication application(argc, argv);
        revealStrategiesHaveExplicitCommitPlans();
        opaqueRgb32BlitMatchesArgb32();
        preparedRevealPublishesExactlyOneFreshFrame();
        warmedSurfaceRevealSkipsFirstShowAndRestoresOpacity();
        return 0;
    }
#endif
    QCoreApplication application(argc, argv);
    revealStrategiesHaveExplicitCommitPlans();
    opaqueRgb32BlitMatchesArgb32();
    return 0;
}
