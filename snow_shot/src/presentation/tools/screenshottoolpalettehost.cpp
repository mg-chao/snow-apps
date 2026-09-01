#include "snow_shot/presentation/screenshottoolpalettehost.h"

#include "snow_shot/presentation/screenshottoolbarmainpanel.h"

#include "screenshottoolbarperfinstrumentation.h"

#include <QEvent>
#include <QMargins>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QRegion>
#include <QWheelEvent>

#include <algorithm>

namespace {
QPoint globalMousePosition(const QMouseEvent* event) {
    if (event == nullptr) {
        return {};
    }

    return event->globalPosition().toPoint();
}

QRect expandedForShadow(const QRect& rect, const QMargins& margins) {
    return rect.adjusted(-margins.left(), -margins.top(), margins.right(), margins.bottom());
}
} // namespace

ScreenshotToolPaletteHost::ScreenshotToolPaletteHost(const ScreenshotToolPalette::Options& options,
                                                     QWidget* parent)
    : QWidget(parent) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    m_palette = new ScreenshotToolPalette(options, this);
    m_palette->setShadowMargins(defaultShadowMargins());
    m_palette->move(0, 0);
    if (m_palette->dragHandle() != nullptr) {
        m_palette->dragHandle()->installEventFilter(this);
    }
    if (m_palette->trailingDragHandle() != nullptr) {
        m_palette->trailingDragHandle()->installEventFilter(this);
    }
    m_palette->installWheelFilters(this);

    connect(m_palette, &ScreenshotToolPalette::visibleContentChanged, this,
            &ScreenshotToolPaletteHost::handlePaletteVisibleContentChanged);
    connect(m_palette, &ScreenshotToolPalette::materializedScope, this,
            [this](QWidget* scope) {
                if (m_palette != nullptr) {
                    m_palette->installWheelFilters(this, scope);
                }
            });

    applyHostSize();
}

QMargins ScreenshotToolPaletteHost::defaultShadowMargins() {
    return ScreenshotToolbarMainPanel::shadowMargins();
}

ScreenshotToolPalette* ScreenshotToolPaletteHost::palette() const {
    return m_palette;
}

QSize ScreenshotToolPaletteHost::contentSizeHint() const {
    return m_palette != nullptr ? m_palette->contentSizeHint() : QSize();
}

QRect ScreenshotToolPaletteHost::occupiedContentRect() const {
    return m_palette != nullptr ? m_palette->occupiedContentRect()
                                : QRect(QPoint(0, 0), contentSizeHint());
}

QRect ScreenshotToolPaletteHost::visualContentRect() const {
    return m_palette != nullptr ? m_palette->visualContentRect()
                                : QRect(QPoint(0, 0), contentSizeHint());
}

QRect ScreenshotToolPaletteHost::fullContentRect() const {
    return m_palette != nullptr ? m_palette->fullContentRect()
                                : QRect(QPoint(0, 0), contentSizeHint());
}

QRect ScreenshotToolPaletteHost::bottomPlacementContentRect() const {
    return m_palette != nullptr ? m_palette->bottomPlacementContentRect()
                                : QRect(QPoint(0, 0), contentSizeHint());
}

QRect ScreenshotToolPaletteHost::topPlacementContentRect() const {
    return m_palette != nullptr ? m_palette->topPlacementContentRect()
                                : QRect(QPoint(0, 0), contentSizeHint());
}

QRect ScreenshotToolPaletteHost::topRightMainToolbarContentRect() const {
    return m_palette != nullptr ? m_palette->topRightMainToolbarContentRect()
                                : QRect(QPoint(0, 0), contentSizeHint());
}

QRect ScreenshotToolPaletteHost::mainToolbarContentRect() const {
    return m_palette != nullptr ? m_palette->mainToolbarContentRect()
                                : QRect(QPoint(0, 0), contentSizeHint());
}

ScreenshotToolbarPlacementSnapshot ScreenshotToolPaletteHost::placementSnapshot() const {
    ScreenshotToolbarPlacementSnapshot snapshot =
        m_palette != nullptr ? m_palette->placementSnapshot()
                             : ScreenshotToolbarPlacementSnapshot{};
    snapshot.contentOffset = contentOffset();
    return snapshot;
}

QRect ScreenshotToolPaletteHost::occupiedHostRect() const {
    if (m_palette == nullptr) {
        return QRect(QPoint(0, 0), size());
    }

    return expandedForShadow(m_palette->occupiedContentRect().translated(contentOffset()),
                             currentShadowMargins())
        .intersected(QRect(QPoint(0, 0), size()));
}

QRegion ScreenshotToolPaletteHost::interactiveHostRegion() const {
    if (m_palette == nullptr) {
        return QRegion(QRect(QPoint(0, 0), size()));
    }

    const QRect hostBounds(QPoint(0, 0), size());
    QRegion region;
    const auto appendPanel = [&](const QWidget* panel, bool contributes) {
        if (panel == nullptr || !contributes) {
            return;
        }

        const QRect panelRect =
            panel->geometry().translated(m_palette->pos()).intersected(hostBounds);
        if (!panelRect.isEmpty()) {
            region += QRegion(panelRect);
        }
    };

    appendPanel(m_palette->mainPanel(), true);
    appendPanel(m_palette->actionPanel(), m_palette->actionToolbarVisible());
    appendPanel(m_palette->stylePanel(), m_palette->styleToolbarVisible());
    return region;
}

QPoint ScreenshotToolPaletteHost::contentOffset() const {
    return m_palette != nullptr ? m_palette->pos() + m_palette->contentOffset() : QPoint();
}

QPoint ScreenshotToolPaletteHost::contentPosition() const {
    return pos() + contentOffset();
}

void ScreenshotToolPaletteHost::moveContentTo(const QPoint& position) {
    // Once the host has been assigned the native frame, it is the full-frame
    // surface and must remain at the origin. The outer floating window owns
    // movement in that mode; retain the old behavior for standalone hosts.
    if (m_frameSize.isValid() && !m_frameSize.isEmpty()) {
        if (pos() != QPoint(0, 0)) {
            move(0, 0);
        }
        return;
    }
    move(position - contentOffset());
}

void ScreenshotToolPaletteHost::prepareForDisplay() {
    applyHostSize();
}

void ScreenshotToolPaletteHost::resetStyleState() {
    if (m_palette != nullptr) {
        m_palette->resetStyleState();
    }
}

void ScreenshotToolPaletteHost::setCreationStyleDefaults(
    const SnowCanvasStyleDefaults& defaults) {
    if (m_palette != nullptr) {
        m_palette->setCreationStyleDefaults(defaults);
    }
}

SnowCanvasStyleDefaults ScreenshotToolPaletteHost::creationStyleDefaults() const {
    return m_palette != nullptr ? m_palette->creationStyleDefaults()
                                : snow_shot::presentation::screenshotCanvasStyleDefaults();
}

bool ScreenshotToolPaletteHost::stepStrokeWidth(int direction) {
    return m_palette != nullptr && m_palette->stepStrokeWidth(direction);
}

bool ScreenshotToolPaletteHost::stepSelectionOpacity(int direction) {
    return m_palette != nullptr && m_palette->stepSelectionOpacity(direction);
}

bool ScreenshotToolPaletteHost::stepSpotlightOpacity(int direction) {
    return m_palette != nullptr && m_palette->stepSpotlightOpacity(direction);
}

bool ScreenshotToolPaletteHost::stepFilterIntensity(int direction) {
    return m_palette != nullptr && m_palette->stepFilterIntensity(direction);
}

bool ScreenshotToolPaletteHost::stepPenFilterStrokeWidth(int direction) {
    return m_palette != nullptr && m_palette->stepPenFilterStrokeWidth(direction);
}

bool ScreenshotToolPaletteHost::stepWatermarkFontSize(int direction) {
    return m_palette != nullptr && m_palette->stepWatermarkFontSize(direction);
}

void ScreenshotToolPaletteHost::setShadowMargins(const QMargins& margins) {
    if (m_palette == nullptr) {
        return;
    }

    if (m_palette->setShadowMargins(margins)) {
        syncHostSize();
    }
}

void ScreenshotToolPaletteHost::setPhysicalScale(qreal scale) {
    if (m_palette == nullptr) {
        return;
    }

    if (m_palette->setPhysicalScale(scale)) {
        syncHostSize();
    }
}

qreal ScreenshotToolPaletteHost::physicalScale() const {
    return m_palette != nullptr ? m_palette->physicalScale() : 1.0;
}

void ScreenshotToolPaletteHost::commitDpiScale(qreal scale, const QMargins& shadowMargins) {
    if (m_palette == nullptr) {
        return;
    }

    m_palette->setShadowMargins(shadowMargins);
    m_palette->setPhysicalScale(scale);
    m_palette->prepareForDisplay();
    syncHostSize();
}

void ScreenshotToolPaletteHost::setFrameSize(const QSize& frameSize, bool anchorToBottom) {
    if (!frameSize.isValid() || frameSize.isEmpty()) {
        return;
    }

    m_frameSize = frameSize;
    m_anchorPaletteToBottom = anchorToBottom;
    syncHostSize();
}

void ScreenshotToolPaletteHost::setStyleToolbarAboveMain(bool above) {
    if (m_palette == nullptr) {
        return;
    }

    m_anchorPaletteToBottom = above;
    m_palette->setStyleToolbarAboveMain(above);
    applyHostSize();
}

void ScreenshotToolPaletteHost::setStyleToolbarVisible(bool visible) {
    if (m_palette == nullptr) {
        return;
    }

    m_palette->setStyleToolbarVisible(visible);
    applyHostSize();
}

void ScreenshotToolPaletteHost::setActiveTool(ScreenshotToolPalette::Tool tool) {
    if (m_palette != nullptr) {
        m_palette->setActiveTool(tool);
    }
}

void ScreenshotToolPaletteHost::setScrollingScreenshotMode(bool enabled) {
    if (m_palette != nullptr) {
        m_palette->setScrollingScreenshotMode(enabled);
        applyHostSize();
    }
}

void ScreenshotToolPaletteHost::clearActiveTool() {
    if (m_palette != nullptr) {
        m_palette->clearActiveTool();
    }
}

bool ScreenshotToolPaletteHost::handleToolbarWheel(QWheelEvent* event) {
    return m_palette != nullptr && m_palette->handleToolbarWheel(event);
}

void ScreenshotToolPaletteHost::cancelDrag() {
    if (!m_dragging) {
        return;
    }

    m_dragging = false;
    if (m_palette != nullptr) {
        if (m_palette->dragHandle() != nullptr) {
            m_palette->dragHandle()->releaseMouse();
        }
        if (m_palette->trailingDragHandle() != nullptr) {
            m_palette->trailingDragHandle()->releaseMouse();
        }
    }
}

bool ScreenshotToolPaletteHost::eventFilter(QObject* watched, QEvent* event) {
    if (event != nullptr && event->type() == QEvent::Wheel &&
        handleToolbarWheel(static_cast<QWheelEvent*>(event))) {
        return true;
    }

    if (m_palette != nullptr &&
        (watched == m_palette->dragHandle() || watched == m_palette->trailingDragHandle()) &&
        event != nullptr) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                m_dragging = true;
                raise();
                if (auto* dragHandle = qobject_cast<QWidget*>(watched)) {
                    dragHandle->grabMouse(Qt::SizeAllCursor);
                }
                emit dragStarted(globalMousePosition(mouseEvent));
                mouseEvent->accept();
                return true;
            }
        }

        if (event->type() == QEvent::MouseMove && m_dragging) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            emit dragMoved(globalMousePosition(mouseEvent));
            mouseEvent->accept();
            return true;
        }

        if (event->type() == QEvent::MouseButtonRelease) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton && m_dragging) {
                emit dragMoved(globalMousePosition(mouseEvent));
                m_dragging = false;
                if (auto* dragHandle = qobject_cast<QWidget*>(watched)) {
                    dragHandle->releaseMouse();
                }
                emit dragFinished(globalMousePosition(mouseEvent));
                mouseEvent->accept();
                return true;
            }
        }

        if (event->type() == QEvent::MouseButtonDblClick) {
            event->accept();
            return true;
        }
    }

    return QWidget::eventFilter(watched, event);
}

void ScreenshotToolPaletteHost::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
}

void ScreenshotToolPaletteHost::handlePaletteVisibleContentChanged() {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("host.visible_content_changed");
    // The palette has already committed its synchronous layout before this
    // signal is emitted.  Re-entering prepareForDisplay here used to turn one
    // state change into a second full layout pass.
    syncHostSize();
    emit visibleContentChanged();
}

void ScreenshotToolPaletteHost::applyHostSize() {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("host.apply_size");
    if (m_palette == nullptr) {
        return;
    }

    m_palette->prepareForDisplay();
    syncHostSize();
}

void ScreenshotToolPaletteHost::syncHostSize() {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("host.sync_size");
    if (m_palette == nullptr) {
        return;
    }

    const QSize targetSize = m_frameSize.isValid() && !m_frameSize.isEmpty()
                                 ? m_frameSize
                                 : m_palette->size();
    if (size() != targetSize) {
        setFixedSize(targetSize);
        SNOW_SHOT_TOOLBAR_PERF_COUNTER("host.size_sync");
#if defined(SNOW_SHOT_TEST_HOOKS)
        ++m_sizeSynchronizationCount;
#endif
        update();
    }
    if (m_frameSize.isValid() && !m_frameSize.isEmpty()) {
        syncPalettePosition();
    }
}

void ScreenshotToolPaletteHost::syncPalettePosition() {
    if (m_palette == nullptr || !m_frameSize.isValid() || m_frameSize.isEmpty()) {
        return;
    }

    const QSize paletteSize = m_palette->size();
    if (paletteSize.isEmpty()) {
        return;
    }

    const QPoint palettePosition(
        m_frameSize.width() - paletteSize.width(),
        m_anchorPaletteToBottom ? m_frameSize.height() - paletteSize.height() : 0);
    if (m_palette->pos() != palettePosition) {
        m_palette->move(palettePosition);
    }
}

quint64 ScreenshotToolPaletteHost::layoutRevision() const {
    return m_palette != nullptr ? m_palette->layoutRevision() : 0;
}

QMargins ScreenshotToolPaletteHost::currentShadowMargins() const {
    const qreal scale = m_palette != nullptr ? m_palette->physicalScale() : 1.0;
    const QMargins baseMargins = defaultShadowMargins();
    const auto scaled = [scale](int value) {
        return value > 0 ? std::max(1, qRound(static_cast<qreal>(value) * scale)) : 0;
    };
    return QMargins(scaled(baseMargins.left()), scaled(baseMargins.top()),
                    scaled(baseMargins.right()), scaled(baseMargins.bottom()));
}
