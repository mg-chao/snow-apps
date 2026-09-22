#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSTARTUPCONTEXT_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSTARTUPCONTEXT_H

#include "snow_shot/presentation/screenshottypes.h"

#include <QSet>
#include <QVector>

#include <memory>
#include <optional>

// The workflow owns this object and publishes the same shared_ptr on the display
// session. Reset the fields in place so session readers stay attached. Geometry is
// immutable once native and Qt observations agree. resumeLiveInput() is the only
// later phase change, and only the input gate calls it for the first genuine event.
struct ScreenshotStartupContext {
    enum class Phase { Inactive, Preparing, Revealed, Live };
    quint64 sessionId = 0;
    quint64 layoutGeneration = 0;
    QPoint invocationLogicalPosition;
    QPoint logicalPosition;
    QPoint physicalPosition;
    QString displayId;
    quint32 nativeDisplayId = 0;
    qsizetype displaySlot = -1;
    Phase phase = Phase::Inactive;
    bool anchorCursor = true;
    QVector<CapturedDisplayModel> qtDisplays;
    QVector<qsizetype> qtDisplaySlots;
    std::shared_ptr<const QVector<CapturedDisplayModel>> displays;

    [[nodiscard]] bool anchored() const {
        return anchorCursor && (phase == Phase::Preparing || phase == Phase::Revealed);
    }
    [[nodiscard]] std::optional<QPoint> anchoredLogicalCursor() const {
        return anchored() ? std::optional<QPoint>(logicalPosition) : std::nullopt;
    }
    [[nodiscard]] bool suppressesInput() const {
        return anchorCursor && (phase == Phase::Preparing || phase == Phase::Revealed);
    }
    void resumeLiveInput() {
        if (anchorCursor && phase == Phase::Revealed)
            phase = Phase::Live;
    }
};

// Join key for one startup generation. macOS binds by CGDirectDisplayID. Windows
// binds by device name, then by physical rect when the names differ.
struct StartupDisplayIdentity {
    bool usesNativeId = false;
    quint32 nativeDisplayId = 0;
    QString deviceName;
    QRect physicalRect;

    [[nodiscard]] static StartupDisplayIdentity fromDisplay(const CapturedDisplayModel& display) {
        StartupDisplayIdentity identity;
        identity.physicalRect = display.physicalRect;
        if (display.canvasUsesPoints || display.nativeDisplayId != 0) {
            identity.usesNativeId = true;
            identity.nativeDisplayId = display.nativeDisplayId;
            return identity;
        }
        identity.deviceName = display.name;
        return identity;
    }

    [[nodiscard]] bool sameNativeId(const StartupDisplayIdentity& qt) const {
        return usesNativeId && qt.usesNativeId && nativeDisplayId != 0 &&
               nativeDisplayId == qt.nativeDisplayId;
    }
    [[nodiscard]] bool sameDeviceName(const StartupDisplayIdentity& qt) const {
        return !usesNativeId && !qt.usesNativeId && !deviceName.isEmpty() &&
               !qt.deviceName.isEmpty() &&
               deviceName.compare(qt.deviceName, Qt::CaseInsensitive) == 0;
    }
    [[nodiscard]] bool samePhysicalRect(const StartupDisplayIdentity& qt) const {
        return !usesNativeId && !qt.usesNativeId && physicalRect.isValid() &&
               !physicalRect.isEmpty() && physicalRect == qt.physicalRect;
    }
};

struct StartupDisplayBinding {
    qsizetype qtIndex = -1;
    qsizetype slot = -1;
    StartupDisplayIdentity identity;
    CapturedDisplayModel display;
};

[[nodiscard]] inline bool startupFrameMatchesDisplay(const CapturedDisplayModel& bound,
                                                     const CapturedDisplayModel& frame) {
    return frame.stableId == bound.stableId && frame.physicalRect == bound.physicalRect &&
           frame.nativeDisplayId == bound.nativeDisplayId &&
           frame.canvasUsesPoints == bound.canvasUsesPoints &&
           (!bound.canvasUsesPoints || frame.capturedLogicalRect == bound.capturedLogicalRect);
}

template <typename QtStillCurrent>
[[nodiscard]] std::optional<QVector<StartupDisplayBinding>> matchStartupDisplays(
    const QVector<CapturedDisplayModel>& qtDisplays, const QVector<qsizetype>& qtSlots,
    const QVector<CapturedDisplayModel>& nativeDisplays, QtStillCurrent qtStillCurrent) {
    if (nativeDisplays.isEmpty() || nativeDisplays.size() != qtDisplays.size() ||
        qtDisplays.size() != qtSlots.size()) {
        return std::nullopt;
    }

    struct Match {
        qsizetype index = -1;
        bool ambiguous = false;
    };
    QSet<qsizetype> used;
    QSet<QString> identities;
    QVector<StartupDisplayBinding> bindings;
    bindings.reserve(nativeDisplays.size());
    for (const CapturedDisplayModel& native : nativeDisplays) {
        const StartupDisplayIdentity identity = StartupDisplayIdentity::fromDisplay(native);
        const auto findMatch = [&](const auto& predicate) {
            Match match;
            for (qsizetype index = 0; index < qtDisplays.size(); ++index) {
                if (used.contains(index) || !predicate(qtDisplays.at(index)))
                    continue;
                if (match.index >= 0) {
                    match.ambiguous = true;
                    match.index = -1;
                    return match;
                }
                match.index = index;
            }
            return match;
        };
        const auto identityOf = [](const CapturedDisplayModel& display) {
            return StartupDisplayIdentity::fromDisplay(display);
        };
        Match match = identity.usesNativeId ? findMatch([&](const CapturedDisplayModel& qt) {
            return identity.sameNativeId(identityOf(qt));
        })
                                            : findMatch([&](const CapturedDisplayModel& qt) {
                                                  return identity.sameDeviceName(identityOf(qt));
                                              });
        if (!identity.usesNativeId && !match.ambiguous && match.index < 0) {
            match = findMatch([&](const CapturedDisplayModel& qt) {
                return identity.samePhysicalRect(identityOf(qt));
            });
        }
        if (match.ambiguous || match.index < 0 || native.stableId.isEmpty() ||
            identities.contains(native.stableId)) {
            return std::nullopt;
        }
        const CapturedDisplayModel& qt = qtDisplays.at(match.index);
        if (native.physicalRect != qt.physicalRect ||
            (native.canvasUsesPoints && native.capturedLogicalRect != qt.logicalRect) ||
            !qtStillCurrent(qt)) {
            return std::nullopt;
        }
        auto display = native;
        display.screen = qt.screen;
        display.logicalRect = qt.logicalRect;
        display.geometryResolved = true;
        bindings.push_back(StartupDisplayBinding{match.index, qtSlots.at(match.index), identity,
                                                 std::move(display)});
        used.insert(match.index);
        identities.insert(native.stableId);
    }
    return bindings;
}

#endif
