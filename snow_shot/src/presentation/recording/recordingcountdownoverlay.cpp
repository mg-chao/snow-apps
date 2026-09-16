#include "recordingcountdownoverlay.h"

#include <QEasingCurve>
#include <QFont>
#include <QPainter>
#include <QPaintEvent>

#include <algorithm>

namespace snow_shot::presentation::recording {
namespace {
constexpr QColor kIndicatorBackground(0, 0, 0, 184);
constexpr qreal kBackdropInset = 1.0;
constexpr QColor kRingTrack(255, 255, 255, 44);
// Matches the paused colour the area border uses while the countdown waits.
constexpr QColor kRingProgress(0xfa, 0xad, 0x14);
constexpr qreal kRingMargin = 5.0;
constexpr qreal kRingWidth = 3.0;
// 12 o'clock in Qt's sixteenths of a degree; arcs sweep clockwise from there.
constexpr int kRingStartAngle = 90 * 16;
constexpr qreal kDigitPopScale = 0.25;
} // namespace

int screenRecordingCountdownRemainingSeconds(qint64 remainingMs) {
    return static_cast<int>(std::max<qint64>(1, (remainingMs + 999) / 1000));
}

qreal screenRecordingCountdownProgress(qint64 totalMs, qint64 remainingMs) {
    if (totalMs <= 0) {
        return 0.0;
    }
    return std::clamp<qreal>(static_cast<qreal>(remainingMs) / static_cast<qreal>(totalMs), 0.0,
                             1.0);
}

qreal screenRecordingCountdownDigitEntrance(qint64 totalMs, qint64 remainingMs) {
    const qint64 elapsed = std::clamp<qint64>(totalMs - remainingMs, 0, totalMs);
    const qint64 digitAge = elapsed % 1000;
    return std::clamp<qreal>(static_cast<qreal>(digitAge) /
                                 static_cast<qreal>(screenRecordingCountdownDigitPopDurationMs),
                             0.0, 1.0);
}

RecordingCountdownOverlay::RecordingCountdownOverlay(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setFixedSize(screenRecordingCountdownIndicatorSize, screenRecordingCountdownIndicatorSize);
    hide();
}

RecordingCountdownOverlay::~RecordingCountdownOverlay() = default;

void RecordingCountdownOverlay::start(int seconds) {
    m_totalMilliseconds = std::max(1, seconds) * 1000;
    m_remainingMilliseconds = m_totalMilliseconds;
    if (!isVisible()) {
        show();
        raise();
    }
    update();
}

void RecordingCountdownOverlay::setRemainingMilliseconds(qint64 remainingMs) {
    if (!active()) {
        return;
    }
    const qint64 clamped = std::clamp<qint64>(remainingMs, 1, m_totalMilliseconds);
    if (clamped == m_remainingMilliseconds) {
        return;
    }
    m_remainingMilliseconds = clamped;
    update();
}

void RecordingCountdownOverlay::clear() {
    m_totalMilliseconds = 0;
    m_remainingMilliseconds = 0;
    hide();
}

int RecordingCountdownOverlay::remainingSeconds() const {
    if (!active()) {
        return 0;
    }
    return screenRecordingCountdownRemainingSeconds(m_remainingMilliseconds);
}

void RecordingCountdownOverlay::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    if (!active()) {
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // The backdrop holds still for the whole countdown; only the ring and the
    // incoming digit animate. A small inset keeps the antialiased edge inside
    // the widget.
    painter.setPen(Qt::NoPen);
    painter.setBrush(kIndicatorBackground);
    painter.drawEllipse(
        QRectF(rect()).adjusted(kBackdropInset, kBackdropInset, -kBackdropInset, -kBackdropInset));

    // The ring drains clockwise over the whole delay, so progress stays
    // visible even between digit changes.
    const QRectF ringRect =
        QRectF(rect()).adjusted(kRingMargin, kRingMargin, -kRingMargin, -kRingMargin);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(kRingTrack, kRingWidth, Qt::SolidLine, Qt::RoundCap));
    painter.drawEllipse(ringRect);
    const qreal progress =
        screenRecordingCountdownProgress(m_totalMilliseconds, m_remainingMilliseconds);
    if (progress > 0.0) {
        painter.setPen(QPen(kRingProgress, kRingWidth, Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(ringRect, kRingStartAngle, -qRound(progress * 360.0 * 16.0));
    }

    // Each digit pops in once at its second boundary and then holds still.
    const QEasingCurve entranceEasing(QEasingCurve::OutCubic);
    const qreal entrance = entranceEasing.valueForProgress(
        screenRecordingCountdownDigitEntrance(m_totalMilliseconds, m_remainingMilliseconds));
    const qreal digitScale = 1.0 + kDigitPopScale * (1.0 - entrance);
    QFont digitFont = font();
    digitFont.setPixelSize(qMax(12, qRound(height() * 0.44)));
    digitFont.setWeight(QFont::Bold);
    painter.setFont(digitFont);
    painter.setPen(Qt::white);
    painter.setOpacity(entrance);
    painter.translate(QPointF(rect().center()));
    painter.scale(digitScale, digitScale);
    painter.drawText(QRectF(-width() / 2.0, -height() / 2.0, width(), height()), Qt::AlignCenter,
                     QString::number(remainingSeconds()));
}

} // namespace snow_shot::presentation::recording
