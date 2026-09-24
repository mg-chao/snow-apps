#include "snow_shot/platform/windows/windowchrome.h"

#include <QApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QEvent>
#include <QToolButton>
#include <QWidget>
#include <QVariant>

#include <cstdlib>
#include <iostream>

#include <qt_windows.h>

namespace {
constexpr DWORD WINDOWS_10_2004_BUILD = 19041;

void require(bool condition, const char* message);

HWND toNativeHwnd(WId windowId) {
    return reinterpret_cast<HWND>(windowId); // NOLINT(performance-no-int-to-ptr)
}

bool currentWindowsSupportsCaptureExclusion() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    require(ntdll != nullptr, "ntdll must be loaded for the Windows version query");
    const auto rtlGetVersion =
        reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    require(rtlGetVersion != nullptr, "RtlGetVersion must be available");

    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    require(rtlGetVersion(&version) == 0, "RtlGetVersion must report the native OS version");
    return version.dwMajorVersion > 10 ||
           (version.dwMajorVersion == 10 && version.dwMinorVersion == 0 &&
            version.dwBuildNumber >= WINDOWS_10_2004_BUILD);
}

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void flushEvents() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::PolishRequest);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QCoreApplication::processEvents();
}

qintptr hitTestAt(QWidget& window, QWidget& titleBar, const QPoint& globalPosition) {
    QCursor::setPos(globalPosition);
    flushEvents();

    POINT nativePosition{};
    require(GetCursorPos(&nativePosition) != 0, "the native cursor position should be available");

    MSG message{};
    message.hwnd = toNativeHwnd(window.winId());
    message.message = WM_NCHITTEST;
    message.lParam =
        MAKELPARAM(static_cast<WORD>(nativePosition.x), static_cast<WORD>(nativePosition.y));

    qintptr result = HTERROR;
    require(snow_shot::platform::windows::handleNativeWindowEvent(&titleBar, &message, &result),
            "the window chrome should handle WM_NCHITTEST");
    return result;
}

void raisedOverlayPreventsTitleBarDragging() {
    const QPoint originalCursorPosition = QCursor::pos();

    QWidget window(nullptr, Qt::Window | Qt::FramelessWindowHint);
    window.resize(420, 280);

    QWidget titleBar(&window);
    titleBar.setGeometry(0, 0, window.width(), 48);

    QToolButton windowControl(&titleBar);
    windowControl.setGeometry(376, 8, 32, 32);

    window.show();
    flushEvents();

    const QPoint dragPosition = titleBar.mapToGlobal(QPoint(180, 24));
    require(hitTestAt(window, titleBar, dragPosition) == HTCAPTION,
            "an uncovered blank title-bar point should remain draggable");

    const QPoint windowControlPosition = windowControl.mapToGlobal(windowControl.rect().center());
    require(hitTestAt(window, titleBar, windowControlPosition) == HTCLIENT,
            "a title-bar control should remain in the client area");

    windowControl.setProperty("snowWindowCaptionHit", HTMAXBUTTON);
    require(hitTestAt(window, titleBar, windowControlPosition) == HTMAXBUTTON,
            "the custom maximize button must expose native Windows Snap hit testing");
    windowControl.setProperty("snowWindowCaptionHit", HTSYSMENU);
    require(hitTestAt(window, titleBar, windowControlPosition) == HTSYSMENU,
            "the application icon must expose the native system menu hit target");
    windowControl.setEnabled(false);
    require(hitTestAt(window, titleBar, windowControlPosition) == HTCLIENT,
            "disabled caption controls must not invoke native commands");
    windowControl.setEnabled(true);

    QWidget previewOverlay(&window);
    previewOverlay.setGeometry(window.rect());
    QToolButton previewClose(&previewOverlay);
    previewClose.setGeometry(320, 8, 40, 40);
    previewOverlay.show();
    previewOverlay.raise();
    flushEvents();

    const QPoint previewClosePosition = previewClose.mapToGlobal(previewClose.rect().center());
    require(hitTestAt(window, titleBar, previewClosePosition) == HTCLIENT,
            "a raised preview control over the title bar must not start a window drag");
    require(hitTestAt(window, titleBar, dragPosition) == HTCLIENT,
            "a raised preview surface must occlude the title-bar drag region");

    require(hitTestAt(window, titleBar, windowControlPosition) == HTCLIENT,
            "an overlay must occlude native caption control hit targets too");

    previewOverlay.hide();
    flushEvents();
    require(hitTestAt(window, titleBar, dragPosition) == HTCAPTION,
            "hiding the preview should restore the title-bar drag region");

    window.hide();
    QCursor::setPos(originalCursorPosition);
}

void nativeCaptionControlHitsAreSuppressed() {
    using snow_shot::platform::windows::detail::isNativeCaptionControlHit;
    for (const qintptr nativeControl : {HTSYSMENU, HTMINBUTTON, HTMAXBUTTON, HTCLOSE, HTHELP}) {
        require(isNativeCaptionControlHit(nativeControl),
                "DWM caption controls must not override SnowShot title-bar controls");
    }
    for (const qintptr customChrome : {HTCLIENT, HTCAPTION, HTLEFT, HTRIGHT, HTTOP, HTBOTTOM}) {
        require(!isNativeCaptionControlHit(customChrome),
                "dragging and resizing must remain owned by the custom window chrome");
    }
}

void customMaximizePressUsesVisibleButtonGeometry() {
    QWidget window(nullptr, Qt::Window | Qt::FramelessWindowHint);
    window.resize(420, 280);
    QWidget titleBar(&window);
    titleBar.setGeometry(0, 0, 420, 48);
    QToolButton maximize(&titleBar);
    // Deliberately far from the native caption-button rectangle.
    maximize.setGeometry(80, 8, 46, 32);
    maximize.setProperty("snowWindowCaptionHit", HTMAXBUTTON);
    int clicks = 0;
    QObject::connect(&maximize, &QToolButton::clicked, &window, [&] {
        ++clicks;
        window.isMaximized() ? window.showNormal() : window.showMaximized();
    });
    window.show();
    flushEvents();
    const HWND hwnd = toNativeHwnd(window.winId());
    for (const UINT type : {WM_NCMOUSEMOVE, WM_NCMOUSELEAVE}) {
        MSG message{};
        message.hwnd = hwnd;
        message.message = type;
        message.wParam = HTMAXBUTTON;
        qintptr result = -1;
        require(snow_shot::platform::windows::handleNativeWindowEvent(&titleBar, &message, &result),
                "non-client hover must never fall through to USER32 caption painting");
        require(maximize.property("snowNativeCaptionHover").toBool() == (type == WM_NCMOUSEMOVE),
                "native hover and leave must update the custom button highlight");
    }
    const auto press = [&](QPoint local, UINT type = WM_NCLBUTTONDOWN) {
        const QPoint inWindow = maximize.mapTo(&window, local);
        const qreal scale = window.devicePixelRatioF();
        POINT position{qRound(inWindow.x() * scale), qRound(inWindow.y() * scale)};
        ClientToScreen(hwnd, &position);
        MSG message{};
        message.hwnd = hwnd;
        message.message = type;
        message.wParam = HTMAXBUTTON;
        message.lParam = MAKELPARAM(position.x, position.y);
        qintptr result = -1;
        require(snow_shot::platform::windows::handleNativeWindowEvent(&titleBar, &message, &result),
                "custom maximize presses must be consumed before default caption processing");
        flushEvents();
    };
    const auto release = [&](QPoint local) {
        const QPoint position = maximize.mapTo(&window, local);
        const qreal scale = window.devicePixelRatioF();
        SendMessageW(hwnd, WM_LBUTTONUP, 0,
                     MAKELPARAM(qRound(position.x() * scale), qRound(position.y() * scale)));
        flushEvents();
    };
    for (const QPoint point : {QPoint(2, 16), QPoint(43, 16)}) {
        const int before = clicks;
        press(point);
        require(maximize.isDown() && clicks == before,
                "pressing either edge must arm the Qt button without activating it");
        require(GetCapture() == hwnd,
                "Qt must capture the mouse so real movement and release use the client path");
        release(point);
        require(clicks == before + 1 && !maximize.isDown(),
                "release inside the visible button must activate it exactly once");
        require(window.isMaximized() == (clicks == 1),
                "the same custom button must maximize and restore the window");
    }
    press(QPoint(2, 16));
    release(QPoint(-20, 60));
    require(clicks == 2 && !maximize.isDown(),
            "release outside the custom button must cancel the click");
    press(QPoint(2, 16), WM_NCLBUTTONDBLCLK);
    release(QPoint(2, 16));
    require(clicks == 3, "a double-click press must also stay on the Qt button path");
    window.hide();
}

void captureExclusionCapabilityAndNativeVisibilityAreReported() {
    require(snow_shot::platform::windows::supportsWindowCaptureExclusion() ==
                currentWindowsSupportsCaptureExclusion(),
            "capture exclusion support must follow the native Windows build capability");

    QWidget window(nullptr, Qt::Tool);
    window.resize(80, 60);
    window.show();
    flushEvents();
    require(window.isVisible() && snow_shot::platform::windows::isNativeWindowVisible(&window),
            "shown capture windows must be visible to both Qt and Windows");

    window.hide();
    flushEvents();
    require(!window.isVisible() && !snow_shot::platform::windows::isNativeWindowVisible(&window),
            "hidden capture windows must be hidden from both Qt and Windows");
    require(snow_shot::platform::windows::flushWindowComposition(),
            "DWM composition must flush before fallback capture begins");
}
void layeredWindowInputTransparencyPreservesNativeState() {
    using snow_shot::platform::windows::setWindowInputTransparent;
    require(!setWindowInputTransparent(nullptr, true).has_value(),
            "null windows must reject native input transparency");
    QWidget window(nullptr, Qt::Tool | Qt::FramelessWindowHint);
    window.setAttribute(Qt::WA_TranslucentBackground);
    window.resize(80, 60);
    window.show();
    flushEvents();
    const HWND hwnd = toNativeHwnd(window.winId());
    const LONG_PTR original = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    require((original & WS_EX_LAYERED) != 0 && (original & WS_EX_TRANSPARENT) == 0,
            "capture window fixture must be layered and accept input");
    SetCapture(hwnd);
    require(GetCapture() == hwnd, "fixture must hold native mouse capture");
    require(setWindowInputTransparent(&window, true) == std::optional<bool>(false),
            "enabling pass-through must report the original input state");
    require(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) == (original | WS_EX_TRANSPARENT) &&
                GetCapture() != hwnd && window.isVisible() && toNativeHwnd(window.winId()) == hwnd,
            "pass-through must release capture without changing visibility, HWND, or other styles");
    require(setWindowInputTransparent(&window, true) == std::optional<bool>(true),
            "already-transparent windows must report their existing state");
    require(setWindowInputTransparent(&window, false) == std::optional<bool>(true) &&
                GetWindowLongPtrW(hwnd, GWL_EXSTYLE) == original,
            "restoration must restore the original native input state");
    window.hide();

    QWidget opaque;
    const HWND opaqueHwnd = toNativeHwnd(opaque.winId());
    const LONG_PTR opaqueStyle = GetWindowLongPtrW(opaqueHwnd, GWL_EXSTYLE);
    require(!setWindowInputTransparent(&opaque, true).has_value() &&
                GetWindowLongPtrW(opaqueHwnd, GWL_EXSTYLE) == opaqueStyle,
            "unsupported non-layered windows must remain unchanged for hidden capture fallback");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    {
        QWidget titleBar;
        QToolButton maximize(&titleBar);
        maximize.setProperty("snowWindowCaptionHit", HTMAXBUTTON);
        for (const UINT type : {WM_ACTIVATE, WM_SHOWWINDOW, WM_ENABLE, WM_CANCELMODE, WM_DESTROY}) {
            maximize.setProperty("snowNativeCaptionHover", true);
            MSG message{};
            message.message = type;
            message.wParam = 0;
            qintptr result = 0;
            snow_shot::platform::windows::handleNativeWindowEvent(&titleBar, &message, &result);
            require(!maximize.property("snowNativeCaptionHover").toBool(),
                    "native lifecycle changes must clear caption hover without a mouse move");
        }
    }
    nativeCaptionControlHitsAreSuppressed();
    customMaximizePressUsesVisibleButtonGeometry();
    layeredWindowInputTransparencyPreservesNativeState();
    captureExclusionCapabilityAndNativeVisibilityAreReported();
    raisedOverlayPreventsTitleBarDragging();
    return 0;
}
