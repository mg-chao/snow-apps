#include "../../presentation/pinned/pinnedwindowstackingplatform.h"

#import <AppKit/AppKit.h>
#include <QHash>
#include <map>

namespace snow_shot::presentation {
namespace {
NSWindow* nativeWindow(QWidget* widget) {
    if (!widget || !widget->internalWinId())
        return nil;
    return reinterpret_cast<NSView*>(widget->internalWinId()).window;
}
std::optional<PinnedStackingWindows> capture(const PinnedStackingWindows& windows,
                                             PinnedWindowStackingMetrics* metrics) {
    if (metrics)
        ++metrics->snapshots;
    QHash<NSInteger, QPointer<QWidget>> wanted;
    for (const auto& widget : windows) {
        NSWindow* window = nativeWindow(widget);
        if (window && window.visible && !window.miniaturized)
            wanted.insert(window.windowNumber, widget);
    }
    PinnedStackingWindows result;
    if (wanted.isEmpty())
        return result;
    NSArray<NSNumber*>* numbers = [NSWindow windowNumbersWithOptions:0];
    if (!numbers)
        return std::nullopt;
    for (NSNumber* number in numbers) {
        const auto found = wanted.constFind(number.integerValue);
        if (found != wanted.cend() && *found &&
            nativeWindow(*found).windowNumber == number.integerValue)
            result.append(*found);
    }
    return result;
}
qint64 level(QWidget* widget) {
    return nativeWindow(widget).level;
}
bool place(QWidget* widget, QWidget* above, QWidget* below, PinnedWindowStackingMetrics* metrics) {
    NSWindow* window = nativeWindow(widget);
    if (!window)
        return false;
    NSWindow* preceding = nativeWindow(above);
    NSWindow* following = nativeWindow(below);
    if (preceding && preceding.level == window.level) {
        if (metrics)
            ++metrics->placements;
        [window orderWindow:NSWindowBelow relativeTo:preceding.windowNumber];
    } else if (following && following.level == window.level) {
        if (metrics)
            ++metrics->placements;
        [window orderWindow:NSWindowAbove relativeTo:following.windowNumber];
    }
    return true;
}
bool apply(const PinnedStackingWindows& windows, PinnedWindowStackingMetrics* metrics) {
    std::map<qint64, PinnedStackingWindows> expected;
    for (const auto& widget : windows) {
        NSWindow* window = nativeWindow(widget);
        if (!window || !window.visible || window.miniaturized)
            continue;
        expected[window.level].append(widget);
    }
    if (windows.size() < 2)
        return true;
    const auto matches = [&expected](const PinnedStackingWindows& actual) {
        std::map<qint64, PinnedStackingWindows> observed;
        for (const auto& widget : actual)
            observed[level(widget)].append(widget);
        return observed == expected;
    };
    if (const auto current = capture(windows, metrics); current && matches(*current))
        return true;
    for (const auto& [band, peers] : expected) {
        Q_UNUSED(band);
        for (qsizetype i = 1; i < peers.size(); ++i)
            if (!place(peers.at(i), peers.at(i - 1), nullptr, metrics))
                return false;
    }
    const auto actual = capture(windows, metrics);
    if (!actual)
        return false;
    return matches(*actual);
}
} // namespace
PinnedWindowStackingPlatform
createCocoaPinnedWindowStackingPlatform(PinnedWindowStackingMetrics* metrics) {
    return {[metrics](const auto& windows) { return capture(windows, metrics); }, level,
            [metrics](auto* window, auto* above, auto* below) {
                return place(window, above, below, metrics);
            },
            [metrics](const auto& windows) { return apply(windows, metrics); }};
}
} // namespace snow_shot::presentation
