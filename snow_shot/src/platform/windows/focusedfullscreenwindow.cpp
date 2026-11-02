#include "snow_shot/platform/windows/focusedfullscreenwindow.h"

#if defined(Q_OS_WIN) || defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dwmapi.h>
#include <qt_windows.h>
#include <QtGui/qscreen_platform.h>
#endif

#include <cstdlib>
#include <QScreen>
#include <QScopeGuard>
#include <QUuid>
#include <vector>
#include "snow_shot/platform/focusedfullscreenwindow.h"
#include "snow_shot/platform/gamewindowclassification.h"

namespace snow_shot::platform::windows {
namespace {
#if defined(Q_OS_WIN) || defined(_WIN32)
bool coordinatesMatch(LONG lhs, LONG rhs) {
    constexpr LONG tolerance = 1;
    return std::abs(lhs - rhs) <= tolerance;
}

QStringList registryStrings(HKEY key, const wchar_t* valueName, DWORD type) {
    DWORD bytes = 0;
    constexpr DWORD maximumBytes = 128 * 1024;
    if (RegGetValueW(key, nullptr, valueName, type, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes == 0 || bytes > maximumBytes || bytes % sizeof(wchar_t) != 0)
        return {};
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 2, L'\0');
    if (RegGetValueW(key, nullptr, valueName, type, nullptr, buffer.data(), &bytes) !=
        ERROR_SUCCESS)
        return {};
    return QString::fromWCharArray(buffer.data(), static_cast<qsizetype>(bytes / sizeof(wchar_t)))
        .split(QChar(u'\0'), Qt::SkipEmptyParts);
}

QStringList registeredGamePaths(const QString& parentKey) {
    // Undocumented, read-only best effort. Use the filename index rather than enumerate all games.
    // Format reference: SpecialK/src/wgc/wgc_registrar.cpp (GetSha1Utf16Le/Parents/Children).
    const QString parentPath = QStringLiteral("System\\GameConfigStore\\Parents\\") + parentKey;
    HKEY parent = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, reinterpret_cast<const wchar_t*>(parentPath.utf16()), 0,
                      KEY_QUERY_VALUE, &parent) != ERROR_SUCCESS)
        return {};
    const auto closeParent = qScopeGuard([parent] { RegCloseKey(parent); });
    const auto children = registryStrings(parent, L"Children", RRF_RT_REG_MULTI_SZ);
    QStringList paths;
    for (const QString& child : children) {
        if (QUuid(child).isNull())
            continue;
        const QString childPath = QStringLiteral("System\\GameConfigStore\\Children\\") + child;
        HKEY record = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, reinterpret_cast<const wchar_t*>(childPath.utf16()), 0,
                          KEY_QUERY_VALUE, &record) != ERROR_SUCCESS)
            continue;
        const auto closeRecord = qScopeGuard([record] { RegCloseKey(record); });
        const auto gameIds = registryStrings(record, L"GameDVR_GameGUID", RRF_RT_REG_SZ);
        if (gameIds.size() == 1 && !QUuid(gameIds.first()).isNull()) {
            const auto matched = registryStrings(record, L"MatchedExeFullPath", RRF_RT_REG_SZ);
            if (matched.size() == 1)
                paths.append(matched.first());
        }
    }
    return paths;
}

bool isGameWindow(HWND window, DWORD processId) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr)
        return false;
    const auto closeProcess = qScopeGuard([process] { CloseHandle(process); });
    std::vector<wchar_t> path(32768);
    DWORD pathSize = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &pathSize))
        return false;
    wchar_t windowClass[256]{};
    const int classSize = GetClassNameW(window, windowClass, 256);
    return isWindowsGameWindow(QString::fromWCharArray(path.data(), pathSize),
                               QString::fromWCharArray(windowClass, classSize),
                               registeredGamePaths);
}
#endif
} // namespace

bool focusedFullscreenWindowMatches(const QScreen* screen, bool gamesOnly = false) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND foreground = GetForegroundWindow();
    HWND window = foreground;
    if (window == nullptr || IsWindow(window) == 0 || IsWindowVisible(window) == 0 ||
        IsIconic(window) != 0) {
        return false;
    }
    if (HWND root = GetAncestor(window, GA_ROOT); root != nullptr) {
        window = root;
    }
    if (window == GetDesktopWindow() || window == GetShellWindow()) {
        return false;
    }

    DWORD processId = 0;
    if (GetWindowThreadProcessId(window, &processId) == 0 ||
        !isExternalFocusedWindowProcess(processId)) {
        return false;
    }

    DWORD cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) ||
        cloaked != 0) {
        return false;
    }

    RECT frame{};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
        return false;
    }
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    if (monitor == nullptr) {
        return false;
    }
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&monitorInfo)) == 0) {
        return false;
    }

    if (screen) {
        const auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
        if (!native || native->handle() != monitor)
            return false;
    }
    const RECT& bounds = monitorInfo.rcMonitor;
    if (!coordinatesMatch(frame.left, bounds.left) || !coordinatesMatch(frame.top, bounds.top) ||
        !coordinatesMatch(frame.right, bounds.right) ||
        !coordinatesMatch(frame.bottom, bounds.bottom) ||
        (gamesOnly && !isGameWindow(window, processId)))
        return false;
    DWORD finalProcessId = 0;
    return GetForegroundWindow() == foreground &&
           GetWindowThreadProcessId(window, &finalProcessId) != 0 && finalProcessId == processId;
#else
    Q_UNUSED(screen);
    Q_UNUSED(gamesOnly);
    return false;
#endif
}

bool focusedFullscreenWindowExists() {
    return focusedFullscreenWindowMatches(nullptr);
}

} // namespace snow_shot::platform::windows

#if defined(Q_OS_WIN) || defined(_WIN32)
namespace snow_shot::platform {
bool focusedFullscreenWindowOnScreen(QScreen* screen) {
    return screen && windows::focusedFullscreenWindowMatches(screen);
}
bool focusedFullscreenWindowExists() {
    return windows::focusedFullscreenWindowExists();
}
bool focusedFullscreenGameWindowExists() {
    return windows::focusedFullscreenWindowMatches(nullptr, true);
}
} // namespace snow_shot::platform
#endif
