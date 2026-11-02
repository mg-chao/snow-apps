#include "pinnedwindowstackingplatform.h"

#include <QGuiApplication>
#include <QHash>
#include <array>
#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace snow_shot::presentation {
#if defined(Q_OS_MACOS)
PinnedWindowStackingPlatform
createCocoaPinnedWindowStackingPlatform(PinnedWindowStackingMetrics* metrics);
#endif
#if defined(Q_OS_WIN)
namespace {
HWND handle(QWidget* window) {
    return window ? reinterpret_cast<HWND>(window->internalWinId()) : nullptr;
}
qint64 level(QWidget* window) {
    const auto hwnd = handle(window);
    return hwnd && (GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) ? 1 : 0;
}
constexpr UINT kFlags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
std::optional<PinnedStackingWindows> capture(const PinnedStackingWindows& windows,
                                             PinnedWindowStackingMetrics* metrics) {
    if (metrics)
        ++metrics->snapshots;
    struct Context {
        QHash<HWND, QPointer<QWidget>> wanted;
        PinnedStackingWindows ordered;
    } context;
    for (const auto& window : windows) {
        const auto hwnd = handle(window);
        if (hwnd && IsWindowVisible(hwnd) && !IsIconic(hwnd))
            context.wanted.insert(hwnd, window);
    }
    if (context.wanted.isEmpty())
        return context.ordered;
    const auto enumerate = [](HWND hwnd, LPARAM data) -> BOOL {
        auto& state = *reinterpret_cast<Context*>(data);
        const auto found = state.wanted.constFind(hwnd);
        if (found != state.wanted.cend() && *found && handle(*found) == hwnd)
            state.ordered.append(*found);
        return TRUE;
    };
    if (!EnumWindows(enumerate, reinterpret_cast<LPARAM>(&context)))
        return std::nullopt;
    return context.ordered;
}
bool place(QWidget* window, QWidget* above, QWidget* below, PinnedWindowStackingMetrics* metrics) {
    const auto hwnd = handle(window);
    if (!hwnd)
        return false;
    HWND after = nullptr;
    if (above && handle(above) && level(above) == level(window)) {
        after = handle(above);
    } else if (below && handle(below) && level(below) == level(window)) {
        after = GetWindow(handle(below), GW_HWNDPREV);
        if (after == hwnd)
            return true;
        // Crossing the topmost boundary with a sibling handle can change bands.
        if (!after || (!level(window) && (GetWindowLongPtr(after, GWL_EXSTYLE) & WS_EX_TOPMOST)))
            after = level(window) ? HWND_TOPMOST : HWND_TOP;
    } else {
        return true;
    }
    if (metrics)
        ++metrics->placements;
    return SetWindowPos(hwnd, after, 0, 0, 0, 0, kFlags) != FALSE;
}
bool apply(const PinnedStackingWindows& windows, PinnedWindowStackingMetrics* metrics) {
    struct Position {
        QPointer<QWidget> window;
        QPointer<QWidget> above;
        HWND hwnd;
        HWND after;
    };
    QVector<Position> positions;
    std::array<QPointer<QWidget>, 2> previous;
    std::array<PinnedStackingWindows, 2> expected;
    for (const auto& window : windows) {
        const auto hwnd = handle(window);
        if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd))
            continue;
        const auto band = static_cast<size_t>(level(window));
        expected[band].append(window);
        if (previous[band])
            positions.append({window, previous[band], hwnd, handle(previous[band])});
        previous[band] = window;
    }
    if (positions.isEmpty())
        return true;
    const auto matches = [&expected](const PinnedStackingWindows& actual) {
        std::array<PinnedStackingWindows, 2> observed;
        for (const auto& window : actual)
            observed[static_cast<size_t>(level(window))].append(window);
        return observed == expected;
    };
    // Progressive insertion usually already established the final stack. Reconciliation
    // needs only one scan in that case, with no redundant native placement messages.
    if (const auto current = capture(windows, metrics); current && matches(*current))
        return true;
    if (metrics)
        ++metrics->deferredBatches;
    HDWP batch = BeginDeferWindowPos(static_cast<int>(positions.size()));
    for (const auto& position : positions) {
        if (!batch)
            break;
        if (metrics)
            ++metrics->placements;
        batch = DeferWindowPos(batch, position.hwnd, position.after, 0, 0, 0, 0, kFlags);
    }
    bool success = batch && EndDeferWindowPos(batch);
    if (!success) {
        success = true;
        // A failed batch may have applied a prefix. Apply the complete intended chain once.
        for (const auto& position : positions) {
            if (metrics)
                ++metrics->placements;
            if (!position.window || !position.above || handle(position.window) != position.hwnd ||
                handle(position.above) != position.after ||
                !SetWindowPos(position.hwnd, position.after, 0, 0, 0, 0, kFlags))
                success = false;
        }
    }
    const auto actual = capture(windows, metrics);
    if (!actual)
        return false;
    return success && matches(*actual);
}
} // namespace
#endif

PinnedWindowStackingPlatform
createPinnedWindowStackingPlatform(PinnedWindowStackingMetrics* metrics) {
#if defined(Q_OS_WIN)
    if (QGuiApplication::platformName() == QStringLiteral("windows"))
        return {[metrics](const auto& windows) { return capture(windows, metrics); }, level,
                [metrics](auto* window, auto* above, auto* below) {
                    return place(window, above, below, metrics);
                },
                [metrics](const auto& windows) { return apply(windows, metrics); }};
#elif defined(Q_OS_MACOS)
    if (QGuiApplication::platformName() == QStringLiteral("cocoa"))
        return createCocoaPinnedWindowStackingPlatform(metrics);
#endif
    Q_UNUSED(metrics);
    return {};
}
} // namespace snow_shot::presentation
