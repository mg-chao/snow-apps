#include "recordingcountdownoverlay.h"

#include <QFont>
#include <QPainter>
#include <QPaintEvent>
#include <QTimer>

#include <algorithm>

namespace snow_shot::presentation::recording {
namespace {
constexpr int kAnimationIntervalMs = 33;
constexpr QColor kIndicatorBackground(0, 0, 0, 184);
} // namespace

int screenRecordingCountdownRemainingSeconds(qint64 totalMs, qint64 elapsedMs) {
    const qint64 remaining = totalMs - std::clamp<qint64>(elapsedMs, 0, totalMs);
    return static_cast<int>(std::max<qint64>(1, (remaining + 999) / 1000));
}

qreal screenRecordingCountdownOpacity(qint64 elapsedWithinSecondMs) {
    const qreal fraction =
        std::clamp<qreal>(static_cast<qreal>(elapsedWithinSecondMs) / 1000.0, 0.0, 1.0);
    return qAbs(1.0 - 2.0 * fraction);
}

RecordingCountdownOverlay::RecordingCountdownOverlay(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setFixedSize(screenRecordingCountdownIndicatorSize, screenRecordingCountdownIndicatorSize);
    m_animationTimer = new QTimer(this);
    m_animationTimer->setInterval(kAnimationIntervalMs);
    QObject::connect(m_animationTimer, &QTimer::timeout, this, [this]() { update(); });
    hide();
}

RecordingCountdownOverlay::~RecordingCountdownOverlay() = default;

void RecordingCountdownOverlay::start(int seconds) {
    m_totalMilliseconds = std::max(1, seconds) * 1000;
    m_elapsed.restart();
    if (!isVisible()) {
        show();
        raise();
    }
    m_animationTimer->start();
    update();
}

void RecordingCountdownOverlay::clear() {
    m_totalMilliseconds = 0;
    m_animationTimer->stop();
    hide();
}

int RecordingCountdownOverlay::remainingSeconds() const {
    if (m_totalMilliseconds <= 0) {
        return 0;
    }
    const qint64 elapsed = m_elapsed.isValid() ? m_elapsed.elapsed() : 0;
    return screenRecordingCountdownRemainingSeconds(m_totalMilliseconds, elapsed);
}

void RecordingCountdownOverlay::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    if (!active()) {
        return;
    }
    const qint64 elapsed = m_elapsed.isValid() ? m_elapsed.elapsed() : 0;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // The whole indicator pulses: full at each second boundary, invisible at
    // the second's midpoint.
    painter.setOpacity(screenRecordingCountdownOpacity(elapsed % 1000));
    painter.setPen(Qt::NoPen);
    painter.setBrush(kIndicatorBackground);
    painter.drawRoundedRect(rect(), screenRecordingCountdownIndicatorRadius,
                            screenRecordingCountdownIndicatorRadius);
    QFont numberFont = font();
    numberFont.setPixelSize(qMax(12, qRound(height() * 0.44)));
    numberFont.setWeight(QFont::Bold);
    painter.setFont(numberFont);
    painter.setPen(Qt::white);
    painter.drawText(
        rect(), Qt::AlignCenter,
        QString::number(screenRecordingCountdownRemainingSeconds(m_totalMilliseconds, elapsed)));
}

} // namespace snow_shot::presentation::recording
