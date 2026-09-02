#include "snow_shot/presentation/screenshotcapturedisplaymodelreconciler.h"

#include <QScreen>

#include "snow_shot/presentation/screenshotdisplaysession.h"

#include <QHash>

#include <algorithm>

namespace {
constexpr qsizetype kIndexedLookupProbeThreshold = 16;

qsizetype findDisplaySlotForSnapshot(ScreenshotDisplaySession& displaySession,
                                     const CapturedDisplayModel& snapshot) {
    if (!snapshot.stableId.isEmpty()) {
        qsizetype matchedIndex = -1;
        displaySession.forEachDisplay([&](qsizetype index, const CapturedDisplayModel& display) {
            if (matchedIndex < 0 && display.stableId == snapshot.stableId) {
                matchedIndex = index;
            }
        });
        if (matchedIndex >= 0) {
            return matchedIndex;
        }
    }

    qsizetype reusableOverlayIndex = -1;
    displaySession.forEachDisplayWithOverlay([&](qsizetype index,
                                                 const CapturedDisplayModel& display,
                                                 ScreenshotOverlayWindow* overlay) {
        if (reusableOverlayIndex < 0 && !display.active && overlay != nullptr) {
            reusableOverlayIndex = index;
        }
    });
    if (reusableOverlayIndex >= 0) {
        return reusableOverlayIndex;
    }

    displaySession.appendDisplay();
    return displaySession.size() - 1;
}

bool shouldUseIndexedLookup(qsizetype displayCount, qsizetype snapshotCount) {
    return displayCount > 0 && snapshotCount > 0 &&
           displayCount > kIndexedLookupProbeThreshold / snapshotCount;
}

class DisplaySlotLookup final {
  public:
    explicit DisplaySlotLookup(ScreenshotDisplaySession& displaySession)
        : m_displaySession(displaySession) {
        rebuildStableIdIndex();
    }

    [[nodiscard]] qsizetype findSlotForSnapshot(const CapturedDisplayModel& snapshot) {
        if (!snapshot.stableId.isEmpty()) {
            const auto match = m_stableIdToIndex.constFind(snapshot.stableId);
            if (match != m_stableIdToIndex.constEnd()) {
                return match.value();
            }
        }

        while (m_nextReusableOverlayIndex < m_displaySession.size()) {
            const qsizetype index = m_nextReusableOverlayIndex;
            ++m_nextReusableOverlayIndex;
            const CapturedDisplayModel& display = m_displaySession.displayAt(index);
            if (!display.active && m_displaySession.overlayAt(index) != nullptr) {
                return index;
            }
        }

        m_displaySession.appendDisplay();
        return m_displaySession.size() - 1;
    }

    void recordStableIdChange(qsizetype index, const QString& nextStableId) {
        if (index < 0 || index >= m_displaySession.size()) {
            return;
        }

        const QString previousStableId = m_displaySession.displayAt(index).stableId;
        if (previousStableId == nextStableId) {
            return;
        }

        removeStableIdIndex(index, previousStableId);
        insertStableIdIndex(index, nextStableId);
    }

  private:
    void rebuildStableIdIndex() {
        m_stableIdToIndex.clear();
        m_stableIdToIndex.reserve(m_displaySession.size());
        m_displaySession.forEachDisplay(
            [this](qsizetype index, const CapturedDisplayModel& display) {
                insertStableIdIndex(index, display.stableId);
            });
    }

    void insertStableIdIndex(qsizetype index, const QString& stableId) {
        if (stableId.isEmpty()) {
            return;
        }

        const auto match = m_stableIdToIndex.constFind(stableId);
        if (match == m_stableIdToIndex.constEnd() || index < match.value()) {
            m_stableIdToIndex.insert(stableId, index);
        }
    }

    void removeStableIdIndex(qsizetype index, const QString& stableId) {
        if (stableId.isEmpty()) {
            return;
        }

        auto match = m_stableIdToIndex.find(stableId);
        if (match == m_stableIdToIndex.end() || match.value() != index) {
            return;
        }

        m_stableIdToIndex.erase(match);
        const qsizetype replacement = firstSlotWithStableId(stableId, index);
        if (replacement >= 0) {
            m_stableIdToIndex.insert(stableId, replacement);
        }
    }

    [[nodiscard]] qsizetype firstSlotWithStableId(const QString& stableId,
                                                  qsizetype skippedIndex) const {
        qsizetype matchedIndex = -1;
        m_displaySession.forEachDisplay([&](qsizetype index, const CapturedDisplayModel& display) {
            if (matchedIndex < 0 && index != skippedIndex && display.stableId == stableId) {
                matchedIndex = index;
            }
        });
        return matchedIndex;
    }

    ScreenshotDisplaySession& m_displaySession;
    QHash<QString, qsizetype> m_stableIdToIndex;
    qsizetype m_nextReusableOverlayIndex = 0;
};

void applySnapshotToDisplay(CapturedDisplayModel& display, const CapturedDisplayModel& snapshot) {
    display.stableId = snapshot.stableId;
    display.name = snapshot.name;
    display.physicalRect = snapshot.physicalRect;
    display.canvasRect = snapshot.physicalRect;
    display.imageSourceCanvasRect = QRect();
    display.logicalRect = QRect();
    display.screen = nullptr;
    display.image = snapshot.image;
    display.active = true;
}

void applySnapshotsToDisplaySession(ScreenshotDisplaySession& displaySession,
                                    const QVector<CapturedDisplayModel>& snapshots) {
    displaySession.forEachMutableDisplay([](qsizetype, CapturedDisplayModel& display) {
        ScreenshotCaptureDisplayModelReconciler::clearCaptureMetadata(display);
    });

    displaySession.reserve(std::max(displaySession.size(), snapshots.size()));
    if (shouldUseIndexedLookup(displaySession.size(), snapshots.size())) {
        DisplaySlotLookup lookup(displaySession);
        for (const CapturedDisplayModel& snapshot : snapshots) {
            if (snapshot.image.isNull()) {
                continue;
            }

            const qsizetype slotIndex = lookup.findSlotForSnapshot(snapshot);
            lookup.recordStableIdChange(slotIndex, snapshot.stableId);
            applySnapshotToDisplay(displaySession.displayAt(slotIndex), snapshot);
        }
        return;
    }

    for (const CapturedDisplayModel& snapshot : snapshots) {
        if (snapshot.image.isNull()) {
            continue;
        }

        applySnapshotToDisplay(
            displaySession.displayAt(findDisplaySlotForSnapshot(displaySession, snapshot)),
            snapshot);
    }
}
} // namespace

void ScreenshotCaptureDisplayModelReconciler::applySnapshots(
    ScreenshotDisplaySession& displaySession, const QVector<CapturedDisplayModel>& snapshots) {
    applySnapshotsToDisplaySession(displaySession, snapshots);
}

void ScreenshotCaptureDisplayModelReconciler::clearCaptureMetadata(CapturedDisplayModel& display) {
    display.name.clear();
    display.physicalRect = QRect();
    display.canvasRect = QRect();
    display.imageSourceCanvasRect = QRect();
    display.logicalRect = QRect();
    display.screen = nullptr;
    display.image = QImage();
    display.active = false;
}
