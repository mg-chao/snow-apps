#include <QRhiWidget>
#include <QOperatingSystemVersion>
#include "toolbar_capture_exclusion_native_tests.h"
#include "../src/platform/windows/captureexclusioncomposition.h"
#include "snow_shot/platform/windowcaptureexclusion.h"
#include "snow_shot/presentation/screenshotfloatingtoolpalettewindow.h"
#include "snow_capture.h"

#include <QApplication>
#include <QBackingStore>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <QWindow>
#include <qt_windows.h>
#include <dwmapi.h>
#include <psapi.h>
#include <qpa/qplatformbackingstore.h>
#include <rhi/qrhi.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <thread>

namespace {
namespace composition = snow_shot::platform::windows::detail;
const QColor backgroundColor(37, 153, 81);
constexpr int backgroundAnimationStep = 32;

bool sampleMemory(const QString& phase) {
    std::array<SIZE_T, 9> privateBytes{};
    std::array<SIZE_T, 9> workingSetBytes{};
    for (size_t i = 0; i < privateBytes.size(); ++i) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        DwmFlush();
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (!GetProcessMemoryInfo(GetCurrentProcess(),
                                  reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                  sizeof(counters)))
            return false;
        privateBytes[i] = counters.PrivateUsage;
        workingSetBytes[i] = counters.WorkingSetSize;
        QThread::msleep(20);
    }
    std::sort(privateBytes.begin(), privateBytes.end());
    std::sort(workingSetBytes.begin(), workingSetBytes.end());
    DWORD handles = 0;
    if (!GetProcessHandleCount(GetCurrentProcess(), &handles))
        return false;
    std::cout << "memory," << phase.toStdString() << ',' << privateBytes[4] << ','
              << workingSetBytes[4] << ',' << handles << ','
              << GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) << std::endl;
    return true;
}

bool excludeToolbarAndCheckFallback(QWidget& toolbar) {
    if (composition::usesCaptureExclusionComposition(&toolbar)) {
        std::cerr << "Toolbar must start on its original presentation path\n";
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    const bool nativeSucceeded = SetWindowDisplayAffinity(reinterpret_cast<HWND>(toolbar.winId()),
                                                          WDA_EXCLUDEFROMCAPTURE) != 0;
    const DWORD error = nativeSucceeded ? ERROR_SUCCESS : GetLastError();
    if (!SetWindowDisplayAffinity(reinterpret_cast<HWND>(toolbar.winId()), WDA_NONE))
        return false;
    const auto version = QOperatingSystemVersion::current();
    const bool expectedFallback = !nativeSucceeded && error == ERROR_NOT_ENOUGH_MEMORY &&
                                  composition::supportsCaptureExclusionCompositionFallback(
                                      version.majorVersion(), version.microVersion());
    const bool excluded = snow_shot::platform::setWindowExcludedFromCapture(&toolbar, true);
    const bool applied = composition::usesCaptureExclusionComposition(&toolbar);
    std::cout << "native_result=" << nativeSucceeded << " native_error=" << error
              << " fallback=" << applied << " excluded=" << excluded << std::endl;
    return excluded && applied == expectedFallback;
}

bool hasAffinity(QWidget& window, DWORD expected) {
    DWORD actual = 0;
    return GetWindowDisplayAffinity(reinterpret_cast<HWND>(window.winId()), &actual) &&
           actual == expected;
}

class Background final : public QWidget {
  public:
    explicit Background(QColor color = backgroundColor)
        : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint), m_color(color) {
        // Desktop Duplication emits frames on damage. Animate the known backdrop
        // so every backend must supply several actual frames, even when excluded.
        m_animation.setInterval(40);
        connect(&m_animation, &QTimer::timeout, this, [this] {
            m_alternate = !m_alternate;
            update();
        });
        m_animation.start();
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        QColor color = m_color;
        // A one-level change can disappear in desktop color quantization and
        // leave damage-driven capture waiting on an effectively static frame.
        color.setRed(qMax(0, color.red() - (m_alternate ? backgroundAnimationStep : 0)));
        painter.fillRect(rect(), color);
    }

  private:
    QColor m_color;
    QTimer m_animation;
    bool m_alternate = false;
};

QRect nativeContentRect(QWidget& window) {
    const auto hwnd = reinterpret_cast<HWND>(window.winId());
    POINT origin{};
    if (!ClientToScreen(hwnd, &origin))
        return {};
    const auto* toolbar = dynamic_cast<ScreenshotFloatingToolPaletteWindow*>(&window);
    const QRect content =
        toolbar
            ? toolbar->occupiedContentRect().translated(toolbar->contentPosition() - toolbar->pos())
            : window.rect();
    const qreal dpr = window.devicePixelRatioF();
    return {origin.x + qRound(content.x() * dpr), origin.y + qRound(content.y() * dpr),
            qRound(content.width() * dpr), qRound(content.height() * dpr)};
}

bool matchesBackground(const QImage& image) {
    if (image.isNull() || image.format() != QImage::Format_RGBA8888)
        return false;
    // Require one uniform, known animation frame across the entire capture.
    const int firstRed = image.constScanLine(0)[0];
    const int expectedRed = qAbs(firstRed - backgroundColor.red()) <= 3
                                ? backgroundColor.red()
                                : backgroundColor.red() - backgroundAnimationStep;
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            const auto* pixel = row + x * 4;
            if (qAbs(static_cast<int>(pixel[0]) - expectedRed) > 3 ||
                qAbs(static_cast<int>(pixel[1]) - backgroundColor.green()) > 3 ||
                qAbs(static_cast<int>(pixel[2]) - backgroundColor.blue()) > 3)
                return false;
        }
    }
    return true;
}

bool verifyStream(QWidget& toolbar, const QString& directory, const QString& phase,
                  SnowCaptureBackendKind backend, bool excluded, bool backgroundOnly = false,
                  int inset = 3) {
    QCoreApplication::processEvents();
    toolbar.repaint();
    DwmFlush();
    const QRect bounds = nativeContentRect(toolbar).adjusted(inset, inset, -inset, -inset);
    SnowCaptureStreamConfig config{};
    config.version = SNOW_CAPTURE_STREAM_CONFIG_VERSION;
    config.struct_size = sizeof(config);
    config.x = bounds.x();
    config.y = bounds.y();
    config.width = static_cast<uint32_t>(bounds.width());
    config.height = static_cast<uint32_t>(bounds.height());
    config.target_fps = 30;
    config.min_fps = 30;
    config.buffer_depth = 3;
    config.max_consecutive_errors = 3;
    config.capture_retry_count = 3;
    config.capture_backend = static_cast<uint8_t>(backend);
    config.pixel_format = SNOW_CAPTURE_PIXEL_FORMAT_RGBA8;
    const std::unique_ptr<SnowCaptureStream, decltype(&snow_capture_stream_destroy)> stream(
        snow_capture_stream_create_region(&config), snow_capture_stream_destroy);
    if (!stream) {
        std::cerr << phase.toStdString() << " backend=" << static_cast<int>(backend)
                  << " stream creation failed: " << snow_capture_last_error_message() << '\n';
        return false;
    }
    QElapsedTimer deadline;
    deadline.start();
    int consecutive = 0;
    QImage last;
    while (deadline.elapsed() < 5000 && consecutive < 5) {
        QCoreApplication::processEvents();
        SnowCaptureStreamEvent event{};
        if (!snow_capture_stream_receive(stream.get(), 20, &event) ||
            event.kind == SNOW_CAPTURE_STREAM_EVENT_ERROR) {
            std::cerr << phase.toStdString() << " backend=" << static_cast<int>(backend)
                      << " capture failed: " << snow_capture_last_error_message() << '\n';
            return false;
        }
        if (event.kind != SNOW_CAPTURE_STREAM_EVENT_FRAME)
            continue;
        SnowCaptureStreamFrameInfo info{};
        info.version = SNOW_CAPTURE_STREAM_FRAME_INFO_VERSION;
        info.struct_size = sizeof(info);
        const bool valid = snow_capture_stream_frame_info(event.frame, &info) != 0;
        if (valid) {
            last =
                QImage(info.rgba_bytes, static_cast<int>(info.width), static_cast<int>(info.height),
                       static_cast<int>(info.stride_bytes), QImage::Format_RGBA8888)
                    .copy();
        }
        snow_capture_stream_frame_release(event.frame);
        if (!valid || info.backend_kind != backend) {
            std::cerr << "Requested capture backend was not exercised\n";
            return false;
        }
        consecutive = matchesBackground(last) == (excluded || backgroundOnly) ? consecutive + 1 : 0;
    }
    const QString label = phase + QStringLiteral("-%1").arg(static_cast<int>(backend));
    const bool saved = last.save(directory + QLatin1Char('/') + label + QStringLiteral(".png"));
    DWORD affinity = 0;
    const auto hwnd = reinterpret_cast<HWND>(toolbar.winId());
    const bool visible = toolbar.isVisible() && IsWindowVisible(hwnd);
    const DWORD expectedAffinity = excluded ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
    const bool affinityMatches =
        GetWindowDisplayAffinity(hwnd, &affinity) && affinity == expectedAffinity;
    std::cout << label.toStdString() << " visible=" << visible << " affinity=" << affinity
              << " clean_frames=" << consecutive << " bounds=" << bounds.width() << 'x'
              << bounds.height() << std::endl;
    return saved && consecutive == 5 && visible && affinityMatches;
}
} // namespace

int verifyFollowAreaCaptureExclusion(QWidget& area, const std::function<void()>& move) {
    const QString directory =
        QDir::current().filePath(QStringLiteral("capture-exclusion-evidence/follow-area"));
    if (!QDir().mkpath(directory))
        return 1;
    Background background;
    background.setGeometry(QApplication::primaryScreen()->geometry());
    background.show();
    background.raise();
    area.show();
    area.raise();
    const auto verify = [&](const QString& phase, bool excluded) {
        bool passed = true;
        for (const auto backend : {SNOW_CAPTURE_BACKEND_DXGI, SNOW_CAPTURE_BACKEND_WGC})
            passed = verifyStream(area, directory, phase, backend, excluded, false, 0) && passed;
        const HWND handle = reinterpret_cast<HWND>(area.winId());
        const QRect bounds = nativeContentRect(area);
        for (const auto point : {bounds.topLeft(), bounds.center()}) {
            passed = SendMessageW(handle, WM_NCHITTEST, 0, MAKELPARAM(point.x(), point.y())) ==
                         HTTRANSPARENT &&
                     passed;
        }
        return passed && area.isVisible() && area.windowOpacity() == 1.0;
    };
    bool passed = verify(QStringLiteral("visible"), false);
    passed = excludeToolbarAndCheckFallback(area) && passed;
    passed = verify(QStringLiteral("excluded"), true) && passed;
    move();
    QCoreApplication::processEvents();
    passed = verify(QStringLiteral("moved-excluded"), true) && passed;
    passed = snow_shot::platform::setWindowExcludedFromCapture(&area, false) && passed;
    passed = verify(QStringLiteral("restored"), false) && passed;
    area.hide();
    return passed ? 0 : 1;
}

int verifyToolbarCaptureExclusionAffinity(QWidget& window, const char* name, bool showWindow) {
    std::cout << "Affinity lifecycle: " << name << std::endl;
    const bool measureMemory =
        showWindow && QCoreApplication::arguments().contains(QStringLiteral("--measure-memory"));
    if (measureMemory)
        std::cout << "memory,phase,private_bytes,working_set_bytes,handles,gdi_objects\n";
    if (showWindow)
        window.show();
    QCoreApplication::processEvents();
    const bool visible = window.isVisible();
    bool passed = excludeToolbarAndCheckFallback(window);
    if (!composition::usesCaptureExclusionComposition(&window))
        passed = composition::applyCaptureExclusionCompositionFallback(&window) && passed;
    passed = snow_shot::platform::setWindowExcludedFromCapture(&window, true) && passed;
    const auto childCount = [&] {
        return window
            .findChildren<QRhiWidget*>(QStringLiteral("snowCaptureExclusionComposition"),
                                       Qt::FindDirectChildrenOnly)
            .size();
    };
    const auto compositionChildren = childCount();
    passed = passed && hasAffinity(window, WDA_EXCLUDEFROMCAPTURE) && window.isVisible() == visible;
    window.move(window.pos() + QPoint(20, 20));
    window.repaint();
    if (visible) {
        window.hide();
        window.show();
    }
    QCoreApplication::processEvents();
    passed = hasAffinity(window, WDA_EXCLUDEFROMCAPTURE) && passed;
    if (auto* toolbar = dynamic_cast<ScreenshotFloatingToolPaletteWindow*>(&window)) {
        const int sessions = measureMemory ? 100 : 1;
        for (int session = 1; session <= sessions; ++session) {
            const QString phase = QStringLiteral("session-%1").arg(session);
            if (measureMemory)
                passed = sampleMemory(phase + QStringLiteral("-active")) && passed;
            auto* store = toolbar->backingStore();
            auto* rhi =
                store && store->handle() ? store->handle()->rhi(toolbar->windowHandle()) : nullptr;
            if (!rhi)
                return 1;
            const auto graphicsReleased = std::make_shared<bool>(false);
            rhi->addCleanupCallback([graphicsReleased](QRhi*) { *graphicsReleased = true; });
            const QPointer<QWindow> nativeWindow(toolbar->windowHandle());
            toolbar->releaseNativeSurface();
            const bool retired = *graphicsReleased && !toolbar->backingStore() &&
                                 !toolbar->internalWinId() && nativeWindow.isNull();
            std::cout << phase.toStdString() << " graphics_released=" << *graphicsReleased
                      << " native_surface_retired=" << retired << std::endl;
            if (!retired)
                return 1;
            if (measureMemory)
                passed = sampleMemory(phase + QStringLiteral("-released")) && passed;
            passed = childCount() == compositionChildren && passed;
            toolbar->restoreNativeSurface();
            if (visible)
                toolbar->show();
            passed = snow_shot::platform::setWindowExcludedFromCapture(toolbar, true) && passed;
            passed = hasAffinity(window, WDA_EXCLUDEFROMCAPTURE) && passed;
            toolbar->repaint();
            QCoreApplication::processEvents();
        }
    }
    passed = snow_shot::platform::setWindowExcludedFromCapture(&window, false) && passed;
    passed = hasAffinity(window, WDA_NONE) && childCount() == compositionChildren &&
             window.isVisible() == visible && passed;
    window.hide();
    std::cout << "Affinity lifecycle passed=" << passed << std::endl;
    return passed ? 0 : 1;
}

int verifyToolbarCaptureExclusion(ScreenshotFloatingToolPaletteWindow& toolbar, const char* name) {
    const bool measureMemory =
        QCoreApplication::arguments().contains(QStringLiteral("--measure-memory"));
    if (measureMemory) {
        std::cout << "memory,phase,private_bytes,working_set_bytes,handles,gdi_objects\n";
        if (!sampleMemory(QStringLiteral("before-session")))
            return 1;
    }
    const QString directory = QDir::current().filePath(
        QStringLiteral("capture-exclusion-evidence/") + QString::fromLatin1(name));
    if (!QDir().mkpath(directory)) {
        return 1;
    }
    Background background;
    background.setGeometry(QApplication::primaryScreen()->availableGeometry());
    background.show();
    background.raise();
    bool passed = verifyStream(background, directory, QStringLiteral("background"),
                               SNOW_CAPTURE_BACKEND_GDI, false, true);
    toolbar.setOwnerWindow(&background);
    toolbar.moveContentTo(background.geometry().topLeft() + QPoint(50, 120));
    toolbar.show();
    toolbar.raise();
    QCoreApplication::processEvents();
    const auto verifyCoverage = [&] {
        QCoreApplication::processEvents();
        toolbar.repaint();
        QCoreApplication::processEvents();
        // Compare native coverage with a separately rendered widget image,
        // including real toolbar shadows and its currently visible rows.
        const QImage image = toolbar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
        const auto hwnd = reinterpret_cast<HWND>(toolbar.winId());
        const HRGN region = CreateRectRgn(0, 0, 0, 0);
        const int type = GetWindowRgn(hwnd, region);
        bool valid = type != ERROR && !image.isNull();
        POINT origin{};
        ClientToScreen(hwnd, &origin);
        POINT transparent{};
        bool foundTransparent = false;
        RECT bounds{};
        GetRgnBox(region, &bounds);
        for (int y = 0; y < image.height() && valid; ++y) {
            const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const bool painted = qAlpha(row[x]) != 0;
                if ((PtInRegion(region, x, y) != FALSE) != painted) {
                    std::cerr << "Native coverage differs from painted alpha at " << x << ',' << y
                              << " alpha=" << qAlpha(row[x]) << '\n';
                    valid = false;
                    break;
                }
                if (!painted && !foundTransparent && x >= bounds.left && x < bounds.right &&
                    y >= bounds.top && y < bounds.bottom) {
                    transparent = {origin.x + x, origin.y + y};
                    foundTransparent = true;
                }
            }
        }
        DeleteObject(region);
        if (!valid || !foundTransparent)
            return false;
        HWND hit = nullptr;
        std::thread query([&] { hit = WindowFromPoint(transparent); });
        query.join();
        if (hit == hwnd || IsChild(hwnd, hit)) {
            std::cerr << "Transparent toolbar pixels intercepted another thread's hit test\n";
            return false;
        }
        return true;
    };
    const auto verify = [&](const QString& phase, bool excluded) {
        // Keep the entire sampled toolbar on the known backdrop, including on
        // small VM displays. Pixels beyond the monitor are black, not backdrop.
        const QRect available = background.geometry().adjusted(4, 4, -4, -4);
        const QRect occupied = toolbar.occupiedContentRect().translated(toolbar.contentPosition());
        if (occupied.width() > available.width() || occupied.height() > available.height()) {
            std::cerr << "Test display is too small for the complete toolbar\n";
            return false;
        }
        const QPoint correction(
            std::clamp(0, available.left() - occupied.left(), available.right() - occupied.right()),
            std::clamp(0, available.top() - occupied.top(),
                       available.bottom() - occupied.bottom()));
        if (!correction.isNull())
            toolbar.moveContentTo(toolbar.contentPosition() + correction);
        for (const auto backend :
             {SNOW_CAPTURE_BACKEND_GDI, SNOW_CAPTURE_BACKEND_WGC, SNOW_CAPTURE_BACKEND_DXGI}) {
            if (!verifyStream(toolbar, directory, phase, backend, excluded))
                return false;
        }
        return true;
    };
    passed = verify(QStringLiteral("included"), false) && passed;
    QApplication::primaryScreen()->grabWindow(0).save(directory +
                                                      QStringLiteral("/desktop-included.png"));
    if (measureMemory)
        passed = sampleMemory(QStringLiteral("before-composition")) && passed;
    passed = excludeToolbarAndCheckFallback(toolbar) && passed;
    // Force the production composition path even on hosts where native affinity
    // already succeeds, then reapply affinity if Qt recreated the HWND.
    if (!composition::usesCaptureExclusionComposition(&toolbar))
        passed = composition::applyCaptureExclusionCompositionFallback(&toolbar) && passed;
    passed = snow_shot::platform::setWindowExcludedFromCapture(&toolbar, true) && passed;
    passed = verifyCoverage() && passed;
    passed = verify(QStringLiteral("excluded"), true) && passed;
    QApplication::primaryScreen()->grabWindow(0).save(directory +
                                                      QStringLiteral("/desktop-excluded.png"));
    toolbar.moveContentTo(toolbar.contentPosition() + QPoint(30, 30));
    toolbar.palette()->setActiveTool(ScreenshotToolPalette::Tool::Shape);
    toolbar.update();
    passed = verifyCoverage() && passed;
    passed = verify(QStringLiteral("moved-repainted"), true) && passed;
    toolbar.hide();
    toolbar.show();
    passed = verifyCoverage() && passed;
    passed = verify(QStringLiteral("shown-again"), true) && passed;
    // Exercise the actual pooled-toolbar boundary, including GPU teardown and
    // capture output after each HWND recreation. Memory counters are diagnostic;
    // allocator/driver caches make byte-for-byte return to baseline unreliable.
    const auto helperCount = toolbar.findChildren<QRhiWidget*>().size();
    const int sessions = measureMemory ? 20 : 3;
    for (int session = 1; session <= sessions; ++session) {
        const QString phase = QStringLiteral("session-%1").arg(session);
        if (measureMemory)
            passed = sampleMemory(phase + QStringLiteral("-active")) && passed;
        auto* store = toolbar.backingStore();
        auto* rhi =
            store && store->handle() ? store->handle()->rhi(toolbar.windowHandle()) : nullptr;
        if (!rhi) {
            std::cerr << "Session must allocate composition graphics resources\n";
            return 1;
        }
        const auto graphicsReleased = std::make_shared<bool>(false);
        rhi->addCleanupCallback([graphicsReleased](QRhi*) { *graphicsReleased = true; });
        const QPointer<QWindow> nativeWindow(toolbar.windowHandle());
        toolbar.releaseNativeSurface();
        const bool retired = *graphicsReleased && !toolbar.backingStore() &&
                             !toolbar.internalWinId() && nativeWindow.isNull();
        std::cout << phase.toStdString() << " graphics_released=" << *graphicsReleased
                  << " native_surface_retired=" << retired << std::endl;
        if (!retired)
            return 1;
        if (measureMemory)
            passed = sampleMemory(phase + QStringLiteral("-released")) && passed;
        toolbar.restoreNativeSurface();
        passed = snow_shot::platform::setWindowExcludedFromCapture(&toolbar, true) && passed;
        toolbar.show();
        toolbar.raise();
        passed = toolbar.findChildren<QRhiWidget*>().size() == helperCount && passed;
        passed = verifyCoverage() && passed;
        passed = verify(phase + QStringLiteral("-recreated"), true) && passed;
    }
    passed = snow_shot::platform::setWindowExcludedFromCapture(&toolbar, false) && passed;
    passed = verifyCoverage() && passed;
    passed = verify(QStringLiteral("restored"), false) && passed;
    toolbar.hide();
    // Unregistered popups retain the native API result and their raster surface.
    Background popup(Qt::magenta);
    popup.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    popup.setAttribute(Qt::WA_TranslucentBackground);
    popup.setGeometry(background.geometry().topLeft().x() + 80,
                      background.geometry().topLeft().y() + 240, 180, 90);
    popup.show();
    passed = verifyStream(popup, directory, QStringLiteral("popup-included"),
                          SNOW_CAPTURE_BACKEND_WGC, false) &&
             passed;
    const bool nativePopupExcluded = SetWindowDisplayAffinity(reinterpret_cast<HWND>(popup.winId()),
                                                              WDA_EXCLUDEFROMCAPTURE) != 0;
    const bool popupExcluded = snow_shot::platform::setWindowExcludedFromCapture(&popup, true);
    passed = popupExcluded == nativePopupExcluded &&
             !composition::usesCaptureExclusionComposition(&popup) && passed;
    popup.repaint();
    passed = verifyStream(popup, directory, QStringLiteral("popup-excluded"),
                          SNOW_CAPTURE_BACKEND_WGC, popupExcluded) &&
             passed;
    passed = snow_shot::platform::setWindowExcludedFromCapture(&popup, false) && passed;
    passed = verifyStream(popup, directory, QStringLiteral("popup-restored"),
                          SNOW_CAPTURE_BACKEND_WGC, false) &&
             passed;
    toolbar.setOwnerWindow(nullptr);
    return passed ? 0 : 1;
}
