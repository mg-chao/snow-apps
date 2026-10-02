#include "recordingtrimtoolbar.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "widgets/button.h"
#include <QCoreApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QSlider>
#include <algorithm>

namespace {
using namespace screenshot_action_toolbar;
constexpr int HandleWidth = ControlSize / 2;
constexpr int TrackGap = 8;
constexpr qreal RailHeight = 4;
constexpr qreal GripWidth = 10;
constexpr qreal GripHeight = 20;
QString trimText(const char* text) {
    return QCoreApplication::translate("RecordingTrimToolbar", text);
}
QString timecode(quint64 microseconds) {
    const quint64 milliseconds = microseconds / 1000;
    return QStringLiteral("%1:%2.%3")
        .arg(milliseconds / 60000, 2, 10, QLatin1Char('0'))
        .arg(milliseconds / 1000 % 60, 2, 10, QLatin1Char('0'))
        .arg(milliseconds % 1000, 3, 10, QLatin1Char('0'));
}
class TrimHandle final : public QSlider {
  public:
    explicit TrimHandle(QWidget* parent) : QSlider(Qt::Horizontal, parent) {
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::SizeHorCursor);
        setSingleStep(1);
        setPageStep(10);
    }

  protected:
    void paintEvent(QPaintEvent*) override {}
};
} // namespace

RecordingTrimToolbar::RecordingTrimToolbar(QWidget* parent) : ScreenshotToolbarPanel(parent) {
    setObjectName(QStringLiteral("screenRecordingTrimToolbar"));
    setFixedSize(sizeHint());
    setAttribute(Qt::WA_TranslucentBackground);
    m_replay = new adqt::widgets::AdButton(this);
    m_replay->setObjectName(QStringLiteral("screenRecordingReplay"));
    m_replay->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
    m_replay->setShape(adqt::widgets::AdButton::Shape::Rounded);
    m_replay->setFocusPolicy(Qt::StrongFocus);
    m_replay->setIconRef(snow_shot::presentation::icons::custom::outlined::RecordingReplay());
    m_replay->setReferenceIconSize({IconSize, IconSize});
    m_replay->setCursor(Qt::PointingHandCursor);
    m_first = new TrimHandle(this);
    m_end = new TrimHandle(this);
    m_first->setObjectName(QStringLiteral("screenRecordingTrimStart"));
    m_end->setObjectName(QStringLiteral("screenRecordingTrimEnd"));
    for (auto* handle : {m_first, m_end}) {
        handle->installEventFilter(this);
        connect(handle, &QSlider::valueChanged, this, [this, handle] {
            const QSignalBlocker firstBlock(m_first), endBlock(m_end);
            if (handle == m_first)
                m_first->setValue(std::min(firstFrame(), endFrame() - 1));
            else
                m_end->setValue(std::max(endFrame(), firstFrame() + 1));
            layoutHandles();
            if (rangeChanged)
                rangeChanged(firstFrame(), endFrame(),
                             handle == m_first ? firstFrame() : endFrame() - 1);
        });
    }
    connect(m_replay, &adqt::widgets::AdButton::clicked, this, [this] {
        if (replayRequested)
            replayRequested();
    });
    setTimeline(1, 1, [](int frame) { return static_cast<quint64>(frame); });
    retranslate();
    connect(&snow_shot::presentation::styles::ThemeManager::instance(),
            &snow_shot::presentation::styles::ThemeManager::themeChanged, this, [this] {
                retranslate();
                update();
            });
}

QSize RecordingTrimToolbar::sizeHint() const {
    return {qRound(360 * m_scale), qRound(PanelHeight * m_scale)};
}
void RecordingTrimToolbar::commitControlScale(const adqt::widgets::AdControlScaleContext& context) {
    m_scale = context.logicalScale;
    setPanelScale(m_scale);
    m_replay->commitControlScale(context);
    setFixedSize(sizeHint());
    layoutHandles();
    retranslate();
}
int RecordingTrimToolbar::firstFrame() const {
    return m_first->value();
}
int RecordingTrimToolbar::endFrame() const {
    return m_end->value();
}
void RecordingTrimToolbar::setTimeline(int frames, quint64 duration,
                                       std::function<quint64(int)> boundary) {
    m_frames = std::max(1, frames);
    m_duration = std::max<quint64>(1, duration);
    m_boundary = std::move(boundary);
    const QSignalBlocker firstBlock(m_first), endBlock(m_end);
    m_first->setRange(0, m_frames - 1);
    m_end->setRange(1, m_frames);
    m_first->setValue(0);
    m_end->setValue(m_frames);
    layoutHandles();
}
void RecordingTrimToolbar::setPosition(quint64 position) {
    if (m_position == position)
        return;
    m_position = std::min(position, m_duration);
    update();
}
QRectF RecordingTrimToolbar::trackRect() const {
    const qreal left = HorizontalMargin + ControlSize + TrackGap + HandleWidth;
    const qreal right = HorizontalMargin + HandleWidth;
    return {left, (height() / m_scale - RailHeight) / 2.0, width() / m_scale - left - right,
            RailHeight};
}
qreal RecordingTrimToolbar::xForFrame(int frame) const {
    const QRectF track = trackRect();
    return track.left() +
           track.width() * static_cast<qreal>(m_boundary(frame)) / static_cast<qreal>(m_duration);
}
int RecordingTrimToolbar::frameAt(qreal x) const {
    const auto track = trackRect();
    const qreal fraction = std::clamp((x / m_scale - track.left()) / track.width(), 0.0, 1.0);
    const quint64 at = static_cast<quint64>(fraction * static_cast<qreal>(m_duration));
    int low = 0, high = m_frames;
    while (low < high) {
        const int middle = low + (high - low) / 2;
        if (m_boundary(middle) < at)
            low = middle + 1;
        else
            high = middle;
    }
    if (low > 0 && at - m_boundary(low - 1) < m_boundary(low) - at)
        --low;
    return low;
}
void RecordingTrimToolbar::layoutHandles() {
    if (!m_replay)
        return;
    const int controlSize = qRound(ControlSize * m_scale);
    const int top = (height() - controlSize + 1) / 2;
    m_replay->setGeometry(qRound(HorizontalMargin * m_scale), top, controlSize, controlSize);
    if (!m_boundary)
        return;
    for (auto* handle : {m_first, m_end}) {
        handle->setGeometry(
            qRound((xForFrame(handle->value()) - (handle == m_first ? HandleWidth : 0)) * m_scale),
            top, qRound(HandleWidth * m_scale), controlSize);
        handle->setToolTip(handle->accessibleName() + QLatin1Char('\n') +
                           timecode(m_boundary(handle->value())));
    }
    update();
}
void RecordingTrimToolbar::resizeEvent(QResizeEvent*) {
    layoutHandles();
}
void RecordingTrimToolbar::retranslate() {
    m_replay->setToolTip(trimText(QT_TRANSLATE_NOOP("RecordingTrimToolbar", "Replay")));
    m_replay->setAccessibleName(m_replay->toolTip());
    m_first->setAccessibleName(trimText(QT_TRANSLATE_NOOP("RecordingTrimToolbar", "Trim start")));
    m_end->setAccessibleName(trimText(QT_TRANSLATE_NOOP("RecordingTrimToolbar", "Trim end")));
    layoutHandles();
}
void RecordingTrimToolbar::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange || event->type() == QEvent::PaletteChange)
        retranslate();
    if (event->type() == QEvent::EnabledChange && !isEnabled()) {
        m_dragging = nullptr;
        m_hovered = nullptr;
    }
    QWidget::changeEvent(event);
}
void RecordingTrimToolbar::paintEvent(QPaintEvent* event) {
    ScreenshotToolbarPanel::paintEvent(event);
    QPainter painter(this);
    painter.scale(m_scale, m_scale);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    const auto track = trackRect();
    const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
    const auto& colors = scheme.map;
    const bool active = isEnabled() && (m_hovered || m_dragging);
    painter.setBrush(active ? colors.colorFillSecondary : colors.colorFillTertiary);
    painter.drawRoundedRect(track, RailHeight / 2, RailHeight / 2);
    const qreal first = xForFrame(firstFrame()), end = xForFrame(endFrame());
    painter.setBrush(!isEnabled() ? colors.colorFillSecondary
                     : active     ? colors.colorPrimaryBorderHover
                                  : colors.colorPrimaryBorder);
    painter.drawRect(QRectF(first, track.top(), end - first, track.height()));
    const qreal position =
        std::clamp(track.left() + track.width() * static_cast<qreal>(m_position) /
                                      static_cast<qreal>(m_duration),
                   first, end);
    const QColor progressColor = isEnabled() ? colors.colorPrimary : colors.colorTextQuaternary;
    painter.setBrush(progressColor);
    painter.drawRect(QRectF(first, track.top(), position - first, track.height()));
    const qreal center = track.center().y();
    for (auto* handle : {m_first, m_end}) {
        const qreal x = xForFrame(handle->value());
        const bool focused = isEnabled() && (handle->hasFocus() || m_dragging == handle);
        const bool hovered = isEnabled() && m_hovered == handle;
        const QColor border = !isEnabled() ? colors.colorBorderDisabled
                              : focused    ? colors.colorPrimary
                              : hovered    ? colors.colorPrimaryBorderHover
                                           : colors.colorPrimaryBorder;
        const QRectF grip(x + (handle == m_first ? -GripWidth - 1 : 1), center - GripHeight / 2,
                          GripWidth, GripHeight);
        if (focused) {
            QColor outline = colors.colorPrimary;
            outline.setAlphaF(0.2F);
            painter.setPen(Qt::NoPen);
            painter.setBrush(outline);
            painter.drawRoundedRect(grip.adjusted(-3, -3, 3, 3), 6, 6);
        }
        painter.setPen(QPen(border, hovered || focused ? 2.5 : 2));
        painter.setBrush(isEnabled() ? colors.colorBgElevated : colors.colorBgContainer);
        painter.drawRoundedRect(grip, 3, 3);
        painter.setPen(QPen(isEnabled() ? colors.colorPrimary : colors.colorTextQuaternary, 1,
                            Qt::SolidLine, Qt::RoundCap));
        for (qreal offset : {-1.5, 1.5})
            painter.drawLine(QPointF(grip.center().x() + offset, center - 3),
                             QPointF(grip.center().x() + offset, center + 3));
    }
}
void RecordingTrimToolbar::moveHandle(QSlider* handle, const QPointF& position) {
    const int frame = frameAt(position.x() - m_dragOffset);
    handle->setValue(handle == m_first ? std::clamp(frame, 0, endFrame() - 1)
                                       : std::clamp(frame, firstFrame() + 1, m_frames));
}
bool RecordingTrimToolbar::eventFilter(QObject* watched, QEvent* event) {
    auto* handle = qobject_cast<QSlider*>(watched);
    if (!handle || !isEnabled())
        return QWidget::eventFilter(watched, event);
    if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)
        update();
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
        m_hovered = event->type() == QEvent::Enter ? handle : nullptr;
        update();
    }
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove ||
        event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton) {
            m_dragging = handle;
            m_dragOffset =
                mapFromGlobal(mouse->globalPosition()).x() - xForFrame(handle->value()) * m_scale;
            handle->setFocus(Qt::MouseFocusReason);
            if (seekRequested)
                seekRequested(handle == m_first ? firstFrame() : endFrame() - 1);
        }
        if (m_dragging == handle) {
            if (event->type() != QEvent::MouseButtonPress)
                moveHandle(handle, mapFromGlobal(mouse->globalPosition()));
            if (event->type() == QEvent::MouseButtonRelease)
                m_dragging = nullptr;
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
void RecordingTrimToolbar::mousePressEvent(QMouseEvent* event) {
    if (isEnabled() && event->button() == Qt::LeftButton && seekRequested)
        seekRequested(std::clamp(frameAt(event->position().x()), firstFrame(), endFrame() - 1));
}
