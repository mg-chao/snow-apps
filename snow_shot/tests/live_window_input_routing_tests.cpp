// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/windows/livewindowinputrouting.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
using snow_shot::presentation::live_window_detail::forwardPointerMessage;
using snow_shot::presentation::live_window_detail::suppressInternalPointerLeave;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct PointerProbe {
    UINT message = 0;
    WPARAM wParam = 0;
    LPARAM lParam = 0;
    LRESULT hit = HTCLIENT;
    int deliveries = 0;
    std::vector<UINT> messages;
};

LRESULT CALLBACK probeProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* probe = reinterpret_cast<PointerProbe*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (probe) {
        probe->message = message;
        probe->wParam = wParam;
        probe->lParam = lParam;
        ++probe->deliveries;
        probe->messages.push_back(message);
        return message == WM_NCHITTEST ? probe->hit : 1;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void requireForwarded(HWND host, HWND shell, PointerProbe& probe, UINT message, WPARAM wParam,
                      LPARAM lParam, LPARAM expectedPosition) {
    probe.deliveries = 0;
    const auto result = forwardPointerMessage(host, shell, message, wParam, lParam);
    require(result && *result == 1 && probe.deliveries == 1 && probe.message == message &&
                probe.wParam == wParam && probe.lParam == expectedPosition,
            "Pointer input must reach the shell once with the correct coordinate space.");
}
} // namespace

int main() {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = probeProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"SnowShotLivePointerRoutingTest";
    require(RegisterClassW(&windowClass) != 0, "Register hidden native input fixtures.");
    // Hidden HWNDs exercise native routing without a display, DWM, or desktop input.
    const HWND shell = CreateWindowExW(0, windowClass.lpszClassName, L"", WS_POPUP, -300, -200, 240,
                                       180, nullptr, nullptr, windowClass.hInstance, nullptr);
    const HWND host = CreateWindowExW(0, windowClass.lpszClassName, L"", WS_POPUP, -284, -184, 208,
                                      148, nullptr, nullptr, windowClass.hInstance, nullptr);
    require(shell && host, "Create hidden native input fixtures.");
    PointerProbe probe;
    SetWindowLongPtrW(shell, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&probe));

    const LPARAM contentPosition = MAKELPARAM(-280, -180);
    for (LRESULT hit : {HTCAPTION, HTLEFT, HTRIGHT, HTTOP, HTBOTTOM, HTTOPLEFT, HTTOPRIGHT,
                        HTBOTTOMLEFT, HTBOTTOMRIGHT}) {
        probe.hit = hit;
        require(!suppressInternalPointerLeave(shell, WM_MOUSELEAVE, contentPosition) &&
                    probe.message == WM_NCHITTEST && probe.lParam == contentPosition,
                "Leaving a Qt control for native caption/resize content must clear widget hover.");
        require(suppressInternalPointerLeave(shell, WM_NCMOUSELEAVE, contentPosition),
                "Internal non-client transitions must retain the composite surface presence.");
    }
    probe.hit = HTCLIENT;
    require(suppressInternalPointerLeave(shell, WM_MOUSELEAVE, contentPosition) &&
                suppressInternalPointerLeave(shell, WM_NCMOUSELEAVE, contentPosition),
            "Client content and controls must retain tracking across internal native leaves.");
    probe.deliveries = 0;

    const auto activationResult =
        forwardPointerMessage(host, shell, WM_MOUSEACTIVATE, reinterpret_cast<WPARAM>(host),
                              MAKELPARAM(HTLEFT, WM_LBUTTONDOWN));
    require(activationResult && *activationResult == MA_NOACTIVATE && probe.deliveries == 0,
            "The content host must deliver presses without taking activation from the shell.");

    for (LRESULT hit : {HTTOPLEFT, HTTOP, HTTOPRIGHT, HTRIGHT, HTBOTTOMRIGHT, HTBOTTOM,
                        HTBOTTOMLEFT, HTLEFT, HTCAPTION, HTCLIENT, HTTRANSPARENT}) {
        probe.hit = hit;
        probe.deliveries = 0;
        const LPARAM screenPoint = MAKELPARAM(-280, -180);
        const auto result = forwardPointerMessage(host, shell, WM_NCHITTEST, 0, screenPoint);
        require(
            result && *result == hit && probe.deliveries == 1 && probe.message == WM_NCHITTEST &&
                probe.lParam == screenPoint,
            "The content host must use the shell's resize, move, lock, and click-through hits.");
    }
    probe.messages.clear();
    const auto nonClientMove =
        forwardPointerMessage(host, shell, WM_NCMOUSEMOVE, HTCAPTION, contentPosition);
    require(nonClientMove && *nonClientMove == 1 &&
                probe.messages == std::vector<UINT>{WM_NCMOUSEMOVE, WM_MOUSELEAVE},
            "Native content must establish presence and clear client hover even without a "
            "tracked shell leave after a tooltip transition.");

    const std::array nonClientMessages{WM_NCLBUTTONDOWN, WM_NCLBUTTONUP, WM_NCLBUTTONDBLCLK,
                                       WM_NCRBUTTONDOWN, WM_NCRBUTTONUP, WM_NCRBUTTONDBLCLK,
                                       WM_NCMBUTTONDOWN, WM_NCMBUTTONUP, WM_NCMBUTTONDBLCLK,
                                       WM_NCXBUTTONDOWN, WM_NCXBUTTONUP, WM_NCXBUTTONDBLCLK};
    for (const UINT message : nonClientMessages)
        requireForwarded(host, shell, probe, message, HTLEFT, MAKELPARAM(-280, -180),
                         MAKELPARAM(-280, -180));

    probe.deliveries = 0;
    const LPARAM cursorInfo = MAKELPARAM(HTLEFT, WM_MOUSEMOVE);
    const auto cursorResult = forwardPointerMessage(host, shell, WM_SETCURSOR,
                                                    reinterpret_cast<WPARAM>(host), cursorInfo);
    require(cursorResult && *cursorResult == 1 && probe.deliveries == 1 &&
                probe.message == WM_SETCURSOR && probe.wParam == reinterpret_cast<WPARAM>(shell) &&
                probe.lParam == cursorInfo,
            "The shell must resolve the resize cursor for its own HWND.");
    probe.deliveries = 0;
    require(!forwardPointerMessage(host, shell, WM_SETCURSOR, reinterpret_cast<WPARAM>(shell),
                                   cursorInfo) &&
                probe.deliveries == 0,
            "Cursor requests delegated back to the owner must not recurse into the shell.");

    const std::array clientMessages{
        WM_MOUSEMOVE,   WM_LBUTTONDOWN,   WM_LBUTTONUP,    WM_LBUTTONDBLCLK, WM_RBUTTONDOWN,
        WM_RBUTTONUP,   WM_RBUTTONDBLCLK, WM_MBUTTONDOWN,  WM_MBUTTONUP,     WM_MBUTTONDBLCLK,
        WM_XBUTTONDOWN, WM_XBUTTONUP,     WM_XBUTTONDBLCLK};
    for (const UINT message : clientMessages)
        requireForwarded(host, shell, probe, message, MK_LBUTTON, MAKELPARAM(4, 5),
                         MAKELPARAM(20, 21));
    for (const UINT message : {WM_MOUSEWHEEL, WM_MOUSEHWHEEL, WM_CONTEXTMENU})
        requireForwarded(host, shell, probe, message, 0, MAKELPARAM(-280, -180),
                         MAKELPARAM(-280, -180));
    requireForwarded(host, shell, probe, WM_CONTEXTMENU, 0, -1, -1);
    probe.deliveries = 0;
    require(!forwardPointerMessage(host, shell, WM_PAINT, 0, 0) && probe.deliveries == 0 &&
                !forwardPointerMessage(host, nullptr, WM_NCHITTEST, 0, 0),
            "Unrelated messages and detached surfaces must not route input.");

    SetWindowLongPtrW(shell, GWLP_USERDATA, 0);
    DestroyWindow(host);
    DestroyWindow(shell);
    UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
    return EXIT_SUCCESS;
}
