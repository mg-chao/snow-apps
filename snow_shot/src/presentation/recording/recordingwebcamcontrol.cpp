#include "recordingwebcamcontrol.h"
#include "snow_shot/presentation/components/canvasactiontoolbar.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "antd_icons.h"

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QScreen>
#include <QtMath>
#include <algorithm>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
class ResizeButton final : public QToolButton {
  public:
    explicit ResizeButton(QWidget* parent) : QToolButton(parent) {
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::SizeBDiagCursor);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, isDown() ? 210 : underMouse() ? 180 : 150));
        painter.drawEllipse(rect());
        painter.translate(width() / 2.0, height() / 2.0);
        painter.scale(width() / 32.0, height() / 32.0);
        painter.setPen(QPen(Qt::white, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(QPointF(-6, 6), QPointF(6, -6));
        painter.drawPolyline(QPolygonF{QPointF(0, -6), QPointF(6, -6), QPointF(6, 0)});
        painter.drawPolyline(QPolygonF{QPointF(-6, 0), QPointF(-6, 6), QPointF(0, 6)});
    }
};
} // namespace

RecordingWebcamControl::RecordingWebcamControl(QWidget* parent)
    : QWidget(parent), m_resize(new ResizeButton(this)),
      m_toolbar(new snow_shot::presentation::components::CanvasActionToolbar(this)) {
    setObjectName(QStringLiteral("screenRecordingWebcamControl"));
    m_resize->setObjectName(QStringLiteral("screenRecordingWebcamResize"));
    setAttribute(Qt::WA_NoSystemBackground);
    setCursor(Qt::SizeAllCursor);
    setFocusPolicy(Qt::NoFocus);
    m_toolbar->setObjectName(QStringLiteral("screenRecordingWebcamOrientationToolbar"));
    m_toolbar->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                              Qt::NoDropShadowWindowHint);
    m_toolbar->setAttribute(Qt::WA_TranslucentBackground);
    m_toolbar->setAttribute(Qt::WA_ShowWithoutActivating);
    m_toolbar->setCursor(Qt::ArrowCursor);
    auto* toolbar =
        static_cast<snow_shot::presentation::components::CanvasActionToolbar*>(m_toolbar);
    const std::array<adqt::icons::IconRef, 4> icons{
        adqt::icons::antd::outlined::RotateRight(), adqt::icons::antd::outlined::RotateLeft(),
        snow_shot::presentation::icons::custom::outlined::FlipHorizontal(),
        snow_shot::presentation::icons::custom::outlined::FlipVertical()};
    const std::array<const char*, 4> names{
        "screenRecordingWebcamRotateClockwise", "screenRecordingWebcamRotateCounterclockwise",
        "screenRecordingWebcamFlipHorizontal", "screenRecordingWebcamFlipVertical"};
    for (std::size_t i = 0; i < icons.size(); ++i) {
        auto* button = toolbar->addAction(icons[i], names[i]);
        button->setCursor(Qt::PointingHandCursor);
        m_orientationButtons[i] = button;
        connect(button, &QAbstractButton::clicked, this, [this, i] {
            if (isVisible() && isEnabled() && !m_interacting && m_toolbar->isVisible())
                emit orientationRequested(static_cast<int>(i));
        });
    }
    m_toolbar->adjustSize();
    m_toolbar->hide();
    const auto observeScreen = [this](QScreen* screen) {
        connect(screen, &QScreen::geometryChanged, this, [this] { updateToolbar(); });
        connect(screen, &QScreen::availableGeometryChanged, this, [this] { updateToolbar(); });
    };
    for (auto* screen : QGuiApplication::screens())
        observeScreen(screen);
    connect(qApp, &QGuiApplication::screenAdded, this, [this, observeScreen](QScreen* screen) {
        observeScreen(screen);
        updateToolbar();
    });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this] { updateToolbar(); });
    qApp->installEventFilter(this);
    retranslateUi();
    hide();
}

void RecordingWebcamControl::retranslateUi() {
    setAccessibleName(tr("Move webcam preview"));
    m_resize->setToolTip(tr("Resize webcam preview"));
    m_resize->setAccessibleName(tr("Resize webcam preview"));
    const std::array<QString, 4> labels{tr("Rotate clockwise"), tr("Rotate counterclockwise"),
                                        tr("Flip horizontally"), tr("Flip vertically")};
    for (std::size_t i = 0; i < labels.size(); ++i) {
        m_orientationButtons[i]->setToolTip(labels[i]);
        m_orientationButtons[i]->setAccessibleName(labels[i]);
    }
}

void RecordingWebcamControl::updateToolbar() {
    if (!isVisible()) {
        m_toolbar->hide();
        return;
    }
    m_toolbar->setEnabled(isEnabled() && !m_interacting);
    const QRectF body(mapToGlobal(m_bounds.topLeft()), m_bounds.size());
    QScreen* screen = QGuiApplication::screenAt(body.center().toPoint());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen) {
        m_toolbar->hide();
        return;
    }
    const QRect available = screen->availableGeometry();
    const QSize toolbarSize = m_toolbar->sizeHint();
    const int x =
        std::clamp(qRound(body.center().x() - toolbarSize.width() / 2.0), available.left(),
                   std::max(available.left(), available.right() + 1 - toolbarSize.width()));
    int y = qFloor(body.top()) - toolbarSize.height() - 6;
    if (y < available.top())
        y = qCeil(body.bottom()) + 6;
    y = std::clamp(y, available.top(),
                   std::max(available.top(), available.bottom() + 1 - toolbarSize.height()));
    m_toolbar->setGeometry(QRect(QPoint(x, y), toolbarSize));
    m_toolbar->show();
}

void RecordingWebcamControl::present(const QRectF& bounds, const QSize& region, int size,
                                     const QPoint& offset, const QPoint& fittedOffset,
                                     bool circle) {
    const QRect enclosing = bounds.toAlignedRect();
    m_bounds = bounds.translated(-enclosing.topLeft());
    m_region = region;
    m_size = size;
    m_offset = offset;
    m_fittedOffset = fittedOffset;
    m_circle = circle;
    m_scale = std::min({size, region.width(), region.height()}) / bounds.width();
    setGeometry(enclosing);
    const int side = std::max(1, std::min({32, qFloor(m_bounds.right()) - qCeil(m_bounds.left()),
                                           qFloor(m_bounds.bottom()) - qCeil(m_bounds.top())}));
    m_resize->setGeometry(qFloor(m_bounds.right()) - side, qCeil(m_bounds.top()), side, side);
    const QRegion body(m_bounds.toAlignedRect(), circle ? QRegion::Ellipse : QRegion::Rectangle);
    setMask(body | QRegion(m_resize->geometry()));
    updateToolbar();
    update();
}

void RecordingWebcamControl::paintEvent(QPaintEvent*) {
    // Nonzero alpha retains input on Windows layered windows, including the circle edge.
    QPainter painter(this);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 2));
    if (m_circle)
        painter.drawEllipse(m_bounds);
    else
        painter.drawRect(m_bounds);
}

void RecordingWebcamControl::beginInteraction(QMouseEvent* event, bool resize) {
    if (event->button() != Qt::LeftButton || m_interacting || !isVisible() || !isEnabled()) {
        event->ignore();
        return;
    }
    m_interacting = true;
    updateToolbar();
    m_resizing = resize;
    m_changed = false;
    m_initialOffset = m_nextOffset = m_offset;
    m_initialFittedOffset = m_fittedOffset;
    m_initialSize = m_nextSize = m_size;
    m_initialSide = std::min({m_size, m_region.width(), m_region.height()});
    m_initialScale = m_scale;
    m_origin = pointerPosition(event->globalPosition());
    m_resize->setDown(resize);
    grabMouse(resize ? Qt::SizeBDiagCursor : Qt::SizeAllCursor);
    emit interactionChanged();
    event->accept();
}

QPointF RecordingWebcamControl::pointerPosition(const QPointF& globalPosition) const {
#ifdef Q_OS_WIN
    // Qt desktop coordinates change scale across displays. Native pixels keep the
    // gesture continuous even when capture carries the pointer onto another monitor.
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        POINT point{};
        if (GetCursorPos(&point))
            return QPointF(point.x, point.y);
    }
#endif
    // macOS desktop points and offscreen events use the preview's initial transform.
    return globalPosition * m_initialScale;
}

void RecordingWebcamControl::updateInteraction(const QPointF& point) {
    const QPointF delta = pointerPosition(point) - m_origin;
    const qreal resizeDelta = (delta.x() - delta.y()) / 2;
    if (delta.isNull() || (m_resizing && qFuzzyIsNull(resizeDelta))) {
        if (!m_changed)
            return;
        m_nextSize = m_initialSize;
        m_nextOffset = m_initialOffset;
    } else if (m_resizing) {
        m_nextSize = std::clamp(qRound(m_initialSide + resizeDelta), 50, 500);
        m_nextOffset = m_initialFittedOffset;
    } else {
        m_nextOffset = QPoint(std::clamp(qRound(m_initialFittedOffset.x() + delta.x()), 0,
                                         m_region.width() - m_initialSide),
                              std::clamp(qRound(m_initialFittedOffset.y() - delta.y()), 0,
                                         m_region.height() - m_initialSide));
    }
    m_changed = true;
    emit layoutPreviewed(m_nextSize, m_nextOffset);
}

void RecordingWebcamControl::releaseInteraction() {
    m_resize->setDown(false);
    if (QWidget::mouseGrabber() == this)
        releaseMouse();
    updateToolbar();
    emit interactionChanged();
}

void RecordingWebcamControl::cancelInteraction() {
    if (!m_interacting)
        return;
    m_interacting = false;
    emit layoutPreviewed(m_initialSize, m_initialOffset);
    releaseInteraction();
}

bool RecordingWebcamControl::event(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    if (event->type() == QEvent::Hide || event->type() == QEvent::UngrabMouse ||
        (event->type() == QEvent::EnabledChange && !isEnabled()))
        cancelInteraction();
    const bool result = QWidget::event(event);
    if (event->type() == QEvent::Show || event->type() == QEvent::Hide ||
        event->type() == QEvent::EnabledChange || event->type() == QEvent::Move ||
        event->type() == QEvent::Resize)
        updateToolbar();
    return result;
}

bool RecordingWebcamControl::eventFilter(QObject* watched, QEvent* event) {
    if (watched == window() && (event->type() == QEvent::Move || event->type() == QEvent::Resize ||
                                event->type() == QEvent::ScreenChangeInternal ||
                                event->type() == QEvent::DevicePixelRatioChange))
        updateToolbar();
    if (m_escapeRelease && event->type() == QEvent::KeyRelease &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        m_escapeRelease = false;
        return true;
    }
    if (m_interacting &&
        (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        if (event->type() == QEvent::KeyPress) {
            m_escapeRelease = true;
            cancelInteraction();
        }
        event->accept();
        return true;
    }
    if (m_interacting &&
        (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::WindowBlocked) &&
        watched == window())
        cancelInteraction();
    if (watched == m_resize && event->type() == QEvent::MouseButtonPress) {
        beginInteraction(static_cast<QMouseEvent*>(event), true);
        return event->isAccepted();
    }
    return QWidget::eventFilter(watched, event);
}

void RecordingWebcamControl::mousePressEvent(QMouseEvent* event) {
    beginInteraction(event, false);
}
void RecordingWebcamControl::mouseMoveEvent(QMouseEvent* event) {
    if (!m_interacting)
        return;
    if (event->buttons().testFlag(Qt::LeftButton))
        updateInteraction(event->globalPosition());
    else
        cancelInteraction();
    event->accept();
}
void RecordingWebcamControl::mouseReleaseEvent(QMouseEvent* event) {
    if (!m_interacting || event->button() != Qt::LeftButton)
        return;
    updateInteraction(event->globalPosition());
    m_interacting = false;
    if (m_changed)
        emit layoutCommitted(m_nextSize, m_nextOffset);
    releaseInteraction();
    event->accept();
}
