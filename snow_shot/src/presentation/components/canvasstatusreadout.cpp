#include "snow_shot/presentation/canvasstatusreadout.h"

#include <QEvent>
#include <QPainter>
#include <QScopedValueRollback>
#include <QTimer>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr int kInset = 8;
constexpr int kHorizontalPadding = 6;
constexpr int kTransientDurationMs = 1000;
} // namespace

CanvasStatusReadout::CanvasStatusReadout(QWidget* parent) : QLabel(parent) {
    setAttribute(Qt::WA_NativeWindow, false);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setFocusPolicy(Qt::NoFocus);
    setAlignment(Qt::AlignCenter);
    setTextFormat(Qt::PlainText);
    setStyleSheet(QStringLiteral("QLabel { color: white; background-color: rgba(0, 0, 0, 150); "
                                 "padding: 3px 6px; border-radius: 4px; }"));
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_timer->setTimerType(Qt::PreciseTimer);
    m_timer->setInterval(kTransientDurationMs);
    connect(m_timer, &QTimer::timeout, this, &CanvasStatusReadout::dismiss);
    hide();
    m_initializing = false;
}

void CanvasStatusReadout::setMessage(Message message, qreal value, int decimals) {
    m_message = message;
    m_value = value;
    m_decimals = std::max(0, decimals);
    m_customText = false;
    refreshPresentation();
}

CanvasStatusReadout::Message CanvasStatusReadout::message() const {
    return m_message;
}

void CanvasStatusReadout::setText(const QString& text) {
    m_fullText = text;
    m_customText = true;
    refreshPresentation();
}

void CanvasStatusReadout::layoutIn(const QRectF& anchor, Corner corner) {
    m_anchor = anchor;
    m_corner = corner;
    m_hasAnchor = true;
    refreshPresentation();
}

void CanvasStatusReadout::showTransient() {
    if (!m_validLayout) {
        dismiss();
        return;
    }
    m_mode = DisplayMode::Transient;
    m_deadline.setRemainingTime(kTransientDurationMs, Qt::PreciseTimer);
    m_timer->start(kTransientDurationMs);
    show();
    raise();
    emit presentationChanged();
}

void CanvasStatusReadout::showPersistent() {
    if (!m_validLayout) {
        dismiss();
        return;
    }
    m_mode = DisplayMode::Persistent;
    m_deadline = QDeadlineTimer(QDeadlineTimer::Forever, Qt::PreciseTimer);
    m_timer->stop();
    show();
    raise();
    emit presentationChanged();
}

void CanvasStatusReadout::dismiss() {
    const bool changed = !isHidden() || m_mode != DisplayMode::Hidden || m_timer->isActive();
    m_mode = DisplayMode::Hidden;
    m_deadline = QDeadlineTimer(0, Qt::PreciseTimer);
    m_timer->stop();
    hide();
    if (changed)
        emit presentationChanged();
}

QTimer* CanvasStatusReadout::dismissalTimer() const {
    return m_timer;
}

CanvasStatusReadout::State CanvasStatusReadout::captureState() const {
    return {m_message,    m_value,
            m_decimals,   m_hasAnchor ? m_anchor : QRectF(parentWidget()->rect()),
            m_corner,     !isHidden(),
            m_mode,       m_deadline,
            m_customText, m_fullText};
}

void CanvasStatusReadout::restoreState(const State& state) {
    m_timer->stop();
    m_message = state.message;
    m_value = state.value;
    m_decimals = state.decimals;
    m_anchor = state.anchor;
    m_corner = state.corner;
    m_hasAnchor = true;
    m_customText = state.customText;
    m_fullText = state.customText ? state.fullText : formattedMessage();
    m_mode = state.mode;
    m_deadline = state.deadline;
    setToolTip(m_fullText);
    setAccessibleName(m_fullText);
    updateLayout();
    const qint64 remaining = m_deadline.remainingTime();
    const bool expired = m_mode == DisplayMode::Transient && remaining <= 0;
    setVisible(state.visible && m_validLayout && !expired);
    if (expired || !m_validLayout) {
        m_mode = DisplayMode::Hidden;
        m_deadline = QDeadlineTimer(0, Qt::PreciseTimer);
    } else if (state.visible && m_mode == DisplayMode::Transient) {
        m_timer->start(
            static_cast<int>(std::min<qint64>(remaining, std::numeric_limits<int>::max())));
    }
    if (!isHidden())
        raise();
    emit presentationChanged();
}

void CanvasStatusReadout::paintOverlay(QPainter& painter) {
    if (isHidden() || !m_validLayout)
        return;
    // DrawWindowBackground adds a rectangular palette pass beneath the rounded QSS background.
    render(&painter, pos(), QRegion(), QWidget::DrawChildren);
}

void CanvasStatusReadout::changeEvent(QEvent* event) {
    QLabel::changeEvent(event);
    if (m_initializing)
        return;
    switch (event->type()) {
    case QEvent::LanguageChange:
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
    case QEvent::StyleChange:
        refreshPresentation();
        break;
    default:
        break;
    }
}

QString CanvasStatusReadout::formattedMessage() const {
    switch (m_message) {
    case Message::Scale:
        return tr("Scale: %1%").arg(m_value, 0, 'f', m_decimals);
    case Message::Opacity:
        return tr("Opacity: %1%").arg(m_value, 0, 'f', m_decimals);
    case Message::Locked:
        return tr("Locked");
    case Message::MotionPreview:
        return tr("Motion preview in progress");
    case Message::ResultPreview:
        return tr("Result preview in progress");
    case Message::RenderingPreview:
        return tr("Rendering preview");
    }
    return {};
}

void CanvasStatusReadout::refreshPresentation() {
    if (m_refreshing)
        return;
    const QScopedValueRollback<bool> guard(m_refreshing, true);
    if (!m_customText)
        m_fullText = formattedMessage();
    setToolTip(m_fullText);
    setAccessibleName(m_fullText);
    updateLayout();
    emit presentationChanged();
}

void CanvasStatusReadout::updateLayout() {
    const QRectF anchor = m_hasAnchor ? m_anchor : QRectF(parentWidget()->rect());
    m_validLayout = std::isfinite(anchor.x()) && std::isfinite(anchor.y()) &&
                    std::isfinite(anchor.width()) && std::isfinite(anchor.height()) &&
                    anchor.isValid();
    QRect bounds;
    if (m_validLayout) {
        const QRectF clipped = anchor.intersected(QRectF(parentWidget()->rect()));
        const int left = qCeil(clipped.left());
        const int top = qCeil(clipped.top());
        const int right = qFloor(clipped.left() + clipped.width());
        const int bottom = qFloor(clipped.top() + clipped.height());
        bounds = QRect(left, top, std::max(0, right - left), std::max(0, bottom - top));
        m_validLayout = clipped.isValid() && !bounds.isEmpty();
    }
    if (!m_validLayout) {
        resize(0, 0);
        m_mode = DisplayMode::Hidden;
        m_deadline = QDeadlineTimer(0, Qt::PreciseTimer);
        m_timer->stop();
        hide();
        return;
    }
    const int insetX = std::min(kInset, std::max(0, (bounds.width() - 1) / 2));
    const int insetY = std::min(kInset, std::max(0, (bounds.height() - 1) / 2));
    const int availableWidth = std::max(0, bounds.width() - insetX * 2);
    QLabel::setText(fontMetrics().elidedText(m_fullText, Qt::ElideRight,
                                             std::max(0, availableWidth - kHorizontalPadding * 2)));
    const QSize desired = sizeHint().boundedTo(QSize(availableWidth, bounds.height() - insetY));
    resize(desired);
    const int y = m_corner == Corner::TopLeft ? bounds.top() + insetY
                                              : bounds.top() + bounds.height() - height() - insetY;
    move(bounds.left() + insetX, y);
    raise();
}
