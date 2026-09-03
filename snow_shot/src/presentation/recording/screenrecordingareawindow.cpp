#include "snow_shot/presentation/screenrecordingareawindow.h"

#include "snow_shot/presentation/screenshotgeometry.h"
#include "screenrecordinggeometry.h"

#include <QPainter>
#include <QPaintEvent>
#include <QScreen>

namespace {
constexpr QColor kIdleColor(0x40, 0x96, 0xff);
constexpr QColor kRecordingColor(0xf5, 0x22, 0x2d);
constexpr QColor kPausedColor(0xfa, 0xad, 0x14);
} // namespace

ScreenRecordingAreaWindow::ScreenRecordingAreaWindow(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                          Qt::WindowDoesNotAcceptFocus | Qt::MSWindowsFixedSizeDialogHint |
                          Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setFocusPolicy(Qt::NoFocus);
}

void ScreenRecordingAreaWindow::setPhysicalRegion(const QRect& region) {
    if (!region.isValid() || region.isEmpty()) {
        return;
    }
    QScreen* screen = ScreenshotGeometryMapper::screenForPhysicalRect(region);
    const QRectF logicalRegion =
        ScreenshotGeometryMapper::logicalRectFForPhysicalRect(region, screen);
    const qreal scale = screen != nullptr ? screen->devicePixelRatio() : 1.0;
    const auto frameGeometry =
        snow_shot::presentation::recording::screenRecordingAreaFrameGeometry(logicalRegion, scale);
    m_frameRect = frameGeometry.frameRect;
    m_selectionRect = frameGeometry.selectionRect;
    m_paddingWidth = frameGeometry.paddingWidth;
    setGeometry(frameGeometry.windowGeometry);
    update();
}

void ScreenRecordingAreaWindow::setRecordingState(ScreenshotToolPalette::RecordingState state) {
    if (m_state == state) {
        return;
    }
    m_state = state;
    update();
}

void ScreenRecordingAreaWindow::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QColor color = kIdleColor;
    if (m_state == ScreenshotToolPalette::RecordingState::Recording) {
        color = kRecordingColor;
    } else if (m_state == ScreenshotToolPalette::RecordingState::Paused) {
        color = kPausedColor;
    }

    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const auto border = snow_shot::presentation::recording::screenRecordingAreaBorderGeometry(
        m_frameRect, m_selectionRect, m_paddingWidth);
    painter.fillRect(border.top, color);
    painter.fillRect(border.bottom, color);
    painter.fillRect(border.left, color);
    painter.fillRect(border.right, color);
}
