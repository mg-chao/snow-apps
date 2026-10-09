#include "snow_shot/presentation/screenshotsmartselectiontransition.h"

#include <algorithm>
#include <utility>

ScreenshotSmartSelectionTransition::ScreenshotSmartSelectionTransition(UpdateCallback update)
    : m_update(std::move(update)) {}

void ScreenshotSmartSelectionTransition::setEnabled(bool enabled) {
    if (m_enabled == enabled) {
        return;
    }
    m_enabled = enabled;
    if (!m_enabled && m_running) {
        static_cast<void>(presentDirectly(m_targetSelection));
    }
}

bool ScreenshotSmartSelectionTransition::enabled() const {
    return m_enabled;
}

bool ScreenshotSmartSelectionTransition::update(const QRectF& selection, bool smartFraming,
                                                qint64 nowMs) {
    const bool hasSelection = selection.isValid() && !selection.isEmpty();
    if (!smartFraming || !hasSelection) {
        m_hasPresentedSmartSelection = false;
        return presentDirectly(selection);
    }

    if (!m_enabled || !m_hasPresentedSmartSelection) {
        m_hasPresentedSmartSelection = true;
        return presentDirectly(selection);
    }

    if (selection == m_targetSelection) {
        return false;
    }

    // Retarget from the geometry at this frame's timestamp, including when an earlier frame
    // was delayed. Presentation owns the clock and decides when frames are committed.
    static_cast<void>(advance(nowMs));
    nowMs = std::max(nowMs, m_lastAdvancedAtMs);
    m_startSelection = m_displayedSelection;
    m_targetSelection = selection;
    m_startedAtMs = nowMs;
    m_lastAdvancedAtMs = nowMs;
    m_running = m_displayedSelection != m_targetSelection;
    return true;
}

bool ScreenshotSmartSelectionTransition::advance(qint64 nowMs) {
    if (!m_running || nowMs <= m_lastAdvancedAtMs) {
        return false;
    }
    m_lastAdvancedAtMs = nowMs;
    const qint64 elapsedMs = nowMs - m_startedAtMs;
    QRectF selection;
    if (elapsedMs >= kDurationMs) {
        m_running = false;
        selection = m_targetSelection;
    } else {
        const qreal progress = static_cast<qreal>(elapsedMs) / kDurationMs;
        const qreal easedProgress = progress * (2.0 - progress);
        selection = QRectF(
            m_startSelection.x() + (m_targetSelection.x() - m_startSelection.x()) * easedProgress,
            m_startSelection.y() + (m_targetSelection.y() - m_startSelection.y()) * easedProgress,
            m_startSelection.width() +
                (m_targetSelection.width() - m_startSelection.width()) * easedProgress,
            m_startSelection.height() +
                (m_targetSelection.height() - m_startSelection.height()) * easedProgress);
    }
    if (selection == m_displayedSelection) {
        return false;
    }
    m_displayedSelection = selection;
    notifyUpdate();
    return true;
}

bool ScreenshotSmartSelectionTransition::isRunning() const {
    return m_running;
}

QRectF ScreenshotSmartSelectionTransition::displayedSelection() const {
    return m_displayedSelection;
}

bool ScreenshotSmartSelectionTransition::presentDirectly(const QRectF& selection) {
    m_running = false;
    m_targetSelection = selection;
    if (m_displayedSelection == selection) {
        return false;
    }
    m_displayedSelection = selection;
    notifyUpdate();
    return true;
}

void ScreenshotSmartSelectionTransition::notifyUpdate() {
    if (m_update) {
        m_update(m_displayedSelection);
    }
}
