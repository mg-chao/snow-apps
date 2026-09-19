#include "snow_shot/platform/windows/windowchrome.h"

#include <QApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QEvent>
#include <QToolButton>
#include <QWidget>

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

    previewOverlay.hide();
    flushEvents();
    require(hitTestAt(window, titleBar, dragPosition) == HTCAPTION,
            "hiding the preview should restore the title-bar drag region");

    window.hide();
    QCursor::setPos(originalCursorPosition);
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
    layeredWindowInputTransparencyPreservesNativeState();
    captureExclusionCapabilityAndNativeVisibilityAreReported();
    raisedOverlayPreventsTitleBarDragging();
    return 0;
}
