#include "snow_shot/presentation/screenshotsmartselectiontransition.h"

#include <QObject>
#include <QVariant>

#include <utility>

ScreenshotSmartSelectionTransition::ScreenshotSmartSelectionTransition(UpdateCallback update)
    : m_update(std::move(update)) {
    m_animation.setDuration(kDurationMs);
    m_animation.setEasingCurve(kEasingCurve);
    QObject::connect(&m_animation, &QVariantAnimation::valueChanged, &m_animation,
                     [this](const QVariant& value) {
                         m_displayedSelection = value.toRectF();
                         notifyUpdate();
                     });
}

void ScreenshotSmartSelectionTransition::setEnabled(bool enabled) {
    if (m_enabled == enabled) {
        return;
    }
    m_enabled = enabled;
    if (!m_enabled && m_animation.state() == QAbstractAnimation::Running) {
        presentDirectly(m_targetSelection);
    }
}

bool ScreenshotSmartSelectionTransition::enabled() const {
    return m_enabled;
}

bool ScreenshotSmartSelectionTransition::update(const QRectF& selection, bool smartFraming) {
    const bool hasSelection = selection.isValid() && !selection.isEmpty();
    if (!smartFraming || !hasSelection) {
        m_hasPresentedSmartSelection = false;
        presentDirectly(selection);
        return true;
    }

    if (!m_enabled) {
        m_hasPresentedSmartSelection = true;
        presentDirectly(selection);
        return true;
    }

    if (!m_hasPresentedSmartSelection) {
        m_hasPresentedSmartSelection = true;
        presentDirectly(selection);
        return true;
    }

    if (selection == m_targetSelection) {
        return false;
    }

    m_animation.stop();
    m_targetSelection = selection;
    m_animation.setStartValue(m_displayedSelection);
    m_animation.setEndValue(m_targetSelection);
    m_animation.start();
    return true;
}


bool ScreenshotSmartSelectionTransition::isRunning() const {
    return m_animation.state() == QAbstractAnimation::Running;
}

QRectF ScreenshotSmartSelectionTransition::displayedSelection() const {
    return m_displayedSelection;
}

void ScreenshotSmartSelectionTransition::presentDirectly(const QRectF& selection) {
    m_animation.stop();
    m_displayedSelection = selection;
    m_targetSelection = selection;
    notifyUpdate();
}

void ScreenshotSmartSelectionTransition::notifyUpdate() {
    if (m_update) {
        m_update(m_displayedSelection);
    }
}
