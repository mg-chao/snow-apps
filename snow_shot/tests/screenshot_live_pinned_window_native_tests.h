// SPDX-License-Identifier: GPL-3.0-or-later
// Included after screenshot_live_pinned_window_tests.h and the shared native helpers.
#ifdef Q_OS_WIN
namespace live_pinned_native_tests {
using Access = ScreenshotLivePinnedWindowTestAccess;
using Source = snow_shot::presentation::LiveWindowSource;

class SourceWindow final : public QWidget {
  public:
    QColor color{32, 96, 192};

    SourceWindow() {
        setWindowFlags(Qt::Window | Qt::WindowStaysOnTopHint);
        setWindowTitle(QStringLiteral("Snow Shot live pin shell integration source"));
        resize(400, 260);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), color);
    }

    void paintColor(const QColor& next) {
        color = next;
        repaint();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        DwmFlush();
    }
};

HWND shell(const ScreenshotPinnedWindow& window) {
    return reinterpret_cast<HWND>(window.internalWinId());
}

HWND owner(HWND window) {
    return reinterpret_cast<HWND>(GetWindowLongPtrW(window, GWLP_HWNDPARENT));
}

bool isDwmHost(HWND window) {
    if (!window || !IsWindow(window))
        return false;
    wchar_t name[128]{};
    return GetClassNameW(window, name, int(std::size(name))) > 0 &&
           QString::fromWCharArray(name) == QStringLiteral("SnowShotLiveWindowHost");
}

QRect nativeGeometry(HWND window) {
    RECT rect{};
    require(GetWindowRect(window, &rect), "read live shell integration native geometry");
    return QRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
}

bool topmost(HWND window) {
    return (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

bool clickThrough(HWND window) {
    return (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0;
}

bool hostOpacity(HWND host, int percent) {
    BYTE alpha = 0;
    DWORD flags = 0;
    return GetLayeredWindowAttributes(host, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) &&
           std::abs(int(alpha) - qRound(percent * 255. / 100.)) <= 1;
}

bool colorMatches(const QImage& image, const QColor& color, int alpha = 255) {
    if (image.isNull())
        return false;
    const QColor actual = image.pixelColor(image.rect().center());
    return std::abs(actual.red() - color.red()) <= 2 &&
           std::abs(actual.green() - color.green()) <= 2 &&
           std::abs(actual.blue() - color.blue()) <= 2 && std::abs(actual.alpha() - alpha) <= 2;
}

struct ClipboardImageRead {
    QImage image;
    bool opened = false;
    bool pngAvailable = false;
    DWORD openError = ERROR_SUCCESS;
    HWND blockingWindow = nullptr;
    DWORD blockingProcess = 0;
};

ClipboardImageRead readClipboardImage() {
    ClipboardImageRead result;
    result.opened = OpenClipboard(nullptr) != FALSE;
    if (!result.opened) {
        result.openError = GetLastError();
        result.blockingWindow = GetOpenClipboardWindow();
        if (result.blockingWindow)
            GetWindowThreadProcessId(result.blockingWindow, &result.blockingProcess);
        return result;
    }
    const auto close = qScopeGuard([] { CloseClipboard(); });
    const UINT png = RegisterClipboardFormatW(L"PNG");
    result.pngAvailable = IsClipboardFormatAvailable(png) != FALSE;
    const auto handle = static_cast<HGLOBAL>(GetClipboardData(png));
    const auto* bytes = handle ? static_cast<const char*>(GlobalLock(handle)) : nullptr;
    if (bytes) {
        result.image =
            QImage::fromData(QByteArray(bytes, static_cast<qsizetype>(GlobalSize(handle))), "PNG");
        GlobalUnlock(handle);
    }
    return result;
}

void diagnoseClipboardCopy(const ClipboardImageRead& read, const QSize& expectedSize,
                           const QColor& expectedColor, const ScreenshotPinnedWindow& window,
                           const Source& source) {
    const QColor center =
        read.image.isNull() ? QColor() : read.image.pixelColor(read.image.rect().center());
    const HWND blocking = GetOpenClipboardWindow();
    DWORD blockingProcess = 0;
    if (blocking)
        GetWindowThreadProcessId(blocking, &blockingProcess);
    std::cerr << "Native live Copy failure: opened=" << read.opened
              << " lastOpenError=" << read.openError
              << " lastBlockingWindow=" << reinterpret_cast<quintptr>(read.blockingWindow)
              << " lastBlockingPid=" << read.blockingProcess
              << " currentBlockingWindow=" << reinterpret_cast<quintptr>(blocking)
              << " currentBlockingPid=" << blockingProcess << " png=" << read.pngAvailable
              << " null=" << read.image.isNull() << " size=" << read.image.width() << 'x'
              << read.image.height() << " expectedSize=" << expectedSize.width() << 'x'
              << expectedSize.height()
              << " centerRGBA=" << center.name(QColor::HexArgb).toStdString()
              << " expectedRGBA=" << expectedColor.name(QColor::HexArgb).toStdString()
              << " live=" << window.isLiveWindow() << " sourceState=" << int(source.state())
              << " nativePresentation=" << source.usesNativePresentation()
              << " shell=" << reinterpret_cast<quintptr>(shell(window))
              << " host=" << reinterpret_cast<quintptr>(owner(shell(window))) << '\n';
}

std::shared_ptr<Source> initializeSource(const snow_shot::presentation::LiveWindowTarget& target,
                                         const QRect& selection) {
    auto source = snow_shot::presentation::createLiveWindowSource(target, selection);
    require(bool(source), "native source for actual pinned shell must be created");
    struct Initialization {
        bool finished = false;
        bool success = false;
        QString error;
    };
    const auto initialization = std::make_shared<Initialization>();
    source->initialize([initialization](bool initialized, const QString& message) {
        initialization->success = initialized;
        initialization->error = message;
        initialization->finished = true;
    });
    live_pinned_tests::wait([&] { return initialization->finished; },
                            "native pinned source initializes");
    if (!initialization->success)
        std::cerr << "Native shell source initialization: " << initialization->error.toStdString()
                  << '\n';
    require(initialization->success && source->initialDesktopRegion() == selection,
            "actual native pin retains the selected source intersection");
    return source;
}

void present(ScreenshotPinnedWindow& window, const std::shared_ptr<Source>& source) {
    ScreenshotPinnedWindow::Config config;
    config.screen = QGuiApplication::primaryScreen();
    config.nativeGeometry = source->initialDesktopRegion();
    config.initialWindowSize = config.nativeGeometry.size();
    config.canvasSourceRect = QRectF(QPointF(), QSizeF(config.initialWindowSize));
    config.imageSource =
        ScreenshotImageSource::fromImage(source->initialFrame(), config.canvasSourceRect);
    config.liveSource = source;
    config.automaticTextRecognition = false;
    config.enableEditing = true;
    config.checkerboardEnabled = false;
    config.initialBorderVisible = false;
    config.initialShadowVisible = false;
    live_pinned_tests::present(window, config);
    live_pinned_tests::wait(
        [&] { return source->usesNativePresentation() && isDwmHost(owner(shell(window))); },
        "actual ScreenshotPinnedWindow establishes its DWM host");
}

void run() {
    require(QGuiApplication::platformName() == QStringLiteral("windows"),
            "native live pin shell integration requires the Windows platform");
    const live_pinned_tests::ClipboardRestorer restoreClipboard;
    const snow_shot::storage::ScreenshotSettings settings;
    const auto previousAutoSave = settings.autoSaveAfterExecutionActions();
    const bool previousCopyFile = settings.copyImageFileToClipboard();
    const auto restoreSettings = qScopeGuard([&] {
        static_cast<void>(settings.setAutoSaveAfterExecutionActions(previousAutoSave));
        static_cast<void>(settings.setCopyImageFileToClipboard(previousCopyFile));
    });
    require(settings.setAutoSaveAfterExecutionActions({}) &&
                settings.setCopyImageFileToClipboard(false),
            "native shell fixture copies image pixels without automatic file saving");

    SourceWindow fixture;
    fixture.move(QGuiApplication::primaryScreen()->availableGeometry().topLeft() + QPoint(80, 80));
    fixture.show();
    fixture.raise();
    const HWND sourceHwnd = reinterpret_cast<HWND>(fixture.winId());
    require(SetWindowPos(sourceHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW),
            "native source fixture enters the topmost band before target lookup");
    fixture.paintColor(fixture.color);
    RECT client{};
    require(GetClientRect(sourceHwnd, &client), "native source fixture has client bounds");
    POINT center{(client.left + client.right) / 2, (client.top + client.bottom) / 2};
    require(ClientToScreen(sourceHwnd, &center), "map native source center to desktop pixels");
    const QPoint hit(center.x, center.y);
    std::optional<snow_shot::presentation::LiveWindowTarget> target;
    live_pinned_tests::wait(
        [&] {
            target = snow_shot::presentation::liveWindowTargetAtPoint(hit, {});
            return target && target->nativeId == reinterpret_cast<quintptr>(sourceHwnd);
        },
        "native shell fixture resolves exactly one real source top-level window");
    const QRect selection(hit - QPoint(64, 40), QSize(128, 80));
    auto source = initializeSource(*target, selection);
    require(colorMatches(source->initialFrame(), fixture.color),
            "native pin initialization receives actual unedited source pixels");
    int stopped = 0;
    QObject::connect(source.get(), &Source::stopped, [&] { ++stopped; });
    ScreenshotPinnedWindow window;
    present(window, source);
    const HWND initialShell = shell(window);
    HWND host = owner(initialShell);
    require(topmost(initialShell) && topmost(host) &&
                nativeGeometry(host) == Access::contentGeometry(window),
            "actual DWM destination matches the topmost pin's displayed content bounds");
    const QRect originalGeometry = window.currentNativeGeometry();

    fixture.paintColor(QColor(48, 160, 80));
    const auto current = live_pinned_tests::image(Access::artifact(window), &window);
    require(current.size() == live_pinned_tests::exportSize(window) &&
                colorMatches(current, fixture.color) && source->usesNativePresentation(),
            "DWM-mode snapshots use fresh source pixels at actual displayed dimensions");
    live_pinned_tests::ClipboardPublicationObserver clipboard;
    Access::copy(window);
    live_pinned_tests::wait(
        [&] {
            return clipboard.publications > 0 &&
                   !ScreenshotPinnedWindowTestAccess::exportArtifact(window);
        },
        "actual native live pin Copy publishes its current source frame");
    ClipboardImageRead copied;
    QElapsedTimer clipboardAccess;
    clipboardAccess.start();
    do {
        const auto read = readClipboardImage();
        if (read.opened) {
            copied.image = read.image;
            copied.opened = true;
            copied.pngAvailable = read.pngAvailable;
            break;
        }
        copied = read;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    } while (clipboardAccess.elapsed() < 10000);
    // Clipboard publication is complete above. Retry access contention only; validate the first
    // readable publication even if its pixels or dimensions are wrong.
    const bool copyCorrect = copied.image.size() == current.size() &&
                             colorMatches(copied.image, fixture.color) && window.isLiveWindow() &&
                             source->usesNativePresentation();
    if (!copyCorrect)
        diagnoseClipboardCopy(copied, current.size(), fixture.color, window, *source);
    require(copyCorrect, "native live Copy preserves dimensions, pixels, and DWM association");

    Access::command(window, QStringLiteral("rotate_clockwise"));
    live_pinned_tests::wait(
        [&] { return !source->usesNativePresentation() && !isDwmHost(owner(shell(window))); },
        "rotating the actual pin switches DWM display to continuous WGC frames");
    const HWND originalOwner = owner(shell(window));
    require(!IsWindow(host), "rotation releases the old DWM host");
    fixture.paintColor(QColor(192, 96, 32));
    live_pinned_tests::wait(
        [&] {
            return !Access::frameProcessing(window) &&
                   colorMatches(Access::original(window), fixture.color) &&
                   Access::transformed(window).size() ==
                       QSize(selection.height(), selection.width());
        },
        "the rotated actual pin continuously receives and transforms WGC source updates");
    const QTransform rotated = Access::transform(window);
    Access::command(window, QStringLiteral("flip_horizontal"));
    require(Access::transform(window) != rotated,
            "the actual horizontal flip changes the live transformation");
    const QTransform horizontallyFlipped = Access::transform(window);
    Access::command(window, QStringLiteral("flip_vertical"));
    require(Access::transform(window) != horizontallyFlipped,
            "the actual vertical flip changes the live transformation");
    fixture.paintColor(QColor(144, 48, 176));
    live_pinned_tests::wait(
        [&] {
            return !source->usesNativePresentation() && !Access::frameProcessing(window) &&
                   colorMatches(Access::original(window), fixture.color);
        },
        "both actual flip operations retain continuous WGC updates");
    Access::command(window, QStringLiteral("reset_transform"));
    live_pinned_tests::wait(
        [&] { return source->usesNativePresentation() && isDwmHost(owner(shell(window))); },
        "resetting actual pin rotation and flips returns to DWM presentation");
    host = owner(shell(window));
    require(Access::transform(window).isIdentity() &&
                window.currentNativeGeometry() == originalGeometry,
            "actual transform reset restores the original displayed orientation and dimensions");

    Access::appearance(window);
    live_pinned_tests::wait(
        [&] {
            return !topmost(shell(window)) && !topmost(owner(shell(window))) &&
                   hostOpacity(owner(shell(window)), 70);
        },
        "native DWM and Qt shell synchronize always-on-top and opacity changes");
    auto* decoration = ScreenshotPinnedWindowTestAccess::decoration(window);
    require(decoration && !decoration->property("showBorder").toBool() &&
                decoration->property("showShadow").toBool(),
            "actual live pin applies the ordinary border and shadow controls");
    require(Access::clickThrough(window, true), "actual live pin enables click-through");
    live_pinned_tests::wait(
        [&] {
            const int effective = window.automationState()
                                      .value(QStringLiteral("click_through_opacity_percent"))
                                      .toInt();
            return clickThrough(shell(window)) && clickThrough(owner(shell(window))) &&
                   hostOpacity(owner(shell(window)), effective);
        },
        "actual native host and shell share click-through styles and effective opacity");
    Access::lock(window, true);
    const QRect lockedGeometry = window.currentNativeGeometry();
    fixture.paintColor(QColor(208, 184, 32));
    const auto lockedImage = live_pinned_tests::image(Access::artifact(window), &window);
    require(colorMatches(lockedImage, fixture.color, qRound(70 * 255. / 100.)) &&
                window.currentNativeGeometry() == lockedGeometry && window.liveFrameAvailable() &&
                source->usesNativePresentation(),
            "locking the actual pin preserves geometry while source capture and export continue");

    const auto before = window.automationState();
    const HWND conversionShell = shell(window);
    const HWND conversionHost = owner(conversionShell);
    const QRect conversionGeometry = nativeGeometry(conversionShell);
    Access::convert(window);
    live_pinned_tests::wait([&] { return !window.isLiveWindow(); },
                            "actual live pin converts to an ordinary image in place");
    const auto after = window.automationState();
    require(
        shell(window) == conversionShell && nativeGeometry(conversionShell) == conversionGeometry &&
            owner(conversionShell) == originalOwner && !IsWindow(conversionHost) && stopped == 1 &&
            !source->usesNativePresentation() && window.isVisible(),
        "actual conversion preserves HWND and geometry while releasing native capture ownership");
    for (const auto& key :
         {QStringLiteral("always_on_top"), QStringLiteral("show_border"),
          QStringLiteral("show_shadow"), QStringLiteral("click_through"), QStringLiteral("locked"),
          QStringLiteral("opacity_percent"), QStringLiteral("click_through_opacity_percent")})
        require(before.value(key) == after.value(key),
                "actual conversion preserves every native pin appearance and interaction mode");
    require(
        colorMatches(Access::original(window), fixture.color) &&
            Access::original(window).size() == lockedImage.size(),
        "converted native pixels preserve their displayed dimensions without baking opacity twice");
    Access::lock(window, false);
    require(Access::clickThrough(window, false),
            "converted native image retains click-through controls");
    auto* menu = ScreenshotPinnedWindowTestAccess::contextMenu(window);
    require(menu && menu->findChild<QAction*>(QStringLiteral("screenshotPinnedDrawingAction")),
            "conversion enables the actual standard pinned-image menu");

    auto peerSource = initializeSource(*target, selection);
    int peerStopped = 0;
    QObject::connect(peerSource.get(), &Source::stopped, [&] { ++peerStopped; });
    ScreenshotPinnedWindow peer;
    present(peer, peerSource);
    window.close();
    require(
        !window.isVisible() && peer.isVisible() && peer.liveFrameAvailable() &&
            peerSource->usesNativePresentation() && peerStopped == 0 && fixture.isVisible(),
        "closing the converted pin affects neither another actual live pin nor its source window");
    const HWND peerHost = owner(shell(peer));
    peer.closeLiveWindow();
    require(!peer.isVisible() && peerStopped == 1 && !IsWindow(peerHost) && fixture.isVisible(),
            "closing an actual live pin releases only its own DWM and capture resources");
    require(IsWindow(sourceHwnd) && stopped == 1,
            "pin conversion and closure leave the original source window open");
}
} // namespace live_pinned_native_tests
#endif
