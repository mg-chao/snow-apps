#include "recordingregiondraghandle.h"

#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include <QEvent>
#include <QMouseEvent>

RecordingRegionDragHandle::RecordingRegionDragHandle(QWidget* parent)
    : OverlayControlButton(Intent::Primary, parent) {
    setObjectName(QStringLiteral("screenRecordingRegionDragHandle"));
    setIconRef(snow_shot::presentation::icons::custom::outlined::ToolMove().withColors(
        adqt::icons::IconColors::primary(QColor(Qt::white))));
    setCursor(Qt::SizeAllCursor);
    setMouseTracking(true);
    retranslateUi();
}

void RecordingRegionDragHandle::stopDragging() {
    m_dragging = false;
    setDown(false);
    if (QWidget::mouseGrabber() == this)
        releaseMouse();
}

void RecordingRegionDragHandle::cancelDrag() {
    if (!m_dragging)
        return;
    m_dragging = false;
    setDown(false);
    emit dragCancelled();
    stopDragging();
}

void RecordingRegionDragHandle::retranslateUi() {
    const QString text = tr("Move recording area");
    setToolTip(text);
    setAccessibleName(text);
}

bool RecordingRegionDragHandle::event(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    if (event->type() == QEvent::Hide || event->type() == QEvent::UngrabMouse ||
        event->type() == QEvent::WindowDeactivate ||
        (event->type() == QEvent::EnabledChange && !isEnabled()))
        cancelDrag();
    return OverlayControlButton::event(event);
}

void RecordingRegionDragHandle::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !rect().contains(event->position().toPoint()) ||
        m_dragging) {
        event->ignore();
        return;
    }
    m_dragging = true;
    setDown(true);
    grabMouse();
    event->accept();
    emit dragStarted(event->globalPosition());
}

void RecordingRegionDragHandle::mouseMoveEvent(QMouseEvent* event) {
    if (!m_dragging) {
        OverlayControlButton::mouseMoveEvent(event);
        return;
    }
    if (!event->buttons().testFlag(Qt::LeftButton))
        cancelDrag();
    else
        emit dragMoved(event->globalPosition());
    event->accept();
}

void RecordingRegionDragHandle::mouseReleaseEvent(QMouseEvent* event) {
    if (!m_dragging || event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    m_dragging = false;
    setDown(false);
    event->accept();
    emit dragFinished(event->globalPosition());
    stopDragging();
}
