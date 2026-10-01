#include "snow_shot/presentation/mainwindowskinwidget.h"

#include "snow_shot/presentation/mainwindowskincontroller.h"

#include <QEvent>
#include <QPainter>
#include <QResizeEvent>

namespace snow_shot::presentation {
MainWindowSkinWidget::MainWindowSkinWidget(QWidget* parent, MainWindowSkinController* controller)
    : QWidget(parent),
      m_controller(controller ? controller : &MainWindowSkinController::instance()) {
    setObjectName(QStringLiteral("mainWindowSkinBackground"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    if (parent != nullptr) {
        parent->installEventFilter(this);
    }
    connect(m_controller, &MainWindowSkinController::frameChanged, this,
            &MainWindowSkinWidget::syncFrame);
    connect(m_controller, &MainWindowSkinController::appearanceChanged, this, [this] {
        update();
        emit skinAppearanceChanged();
    });
    syncFrame();
}

MainWindowSkinWidget::~MainWindowSkinWidget() {
    if (m_controller) {
        m_controller->detach(this);
    }
}

bool MainWindowSkinWidget::skinActive() const {
    return !m_frame.isNull();
}

qreal MainWindowSkinWidget::maskOpacity() const {
    return skinActive() && m_controller ? m_controller->maskOpacity() : 1.0;
}

void MainWindowSkinWidget::setBaseColor(const QColor& color) {
    if (m_baseColor != color) {
        m_baseColor = color;
        m_baseColor.setAlpha(255);
        update();
    }
}

void MainWindowSkinWidget::syncFrame() {
    const auto frame = m_controller ? m_controller->frame() : MainWindowSkinFrame{};
    m_frame = QPixmap::fromImage(frame.image);
    m_placement = frame.normalizedPlacement;
    update();
    emit skinAppearanceChanged();
}

void MainWindowSkinWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), m_baseColor);
    if (!m_frame.isNull() && m_controller && m_controller->opacity() > 0.0) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setOpacity(m_controller->opacity());
        const QRectF target(m_placement.x() * width(), m_placement.y() * height(),
                            m_placement.width() * width(), m_placement.height() * height());
        painter.drawPixmap(target, m_frame, QRectF(m_frame.rect()));
    }
}

void MainWindowSkinWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_attached && m_controller) {
        m_controller->setViewport(this, size(), devicePixelRatioF());
    }
}

bool MainWindowSkinWidget::event(QEvent* event) {
    const bool handled = QWidget::event(event);
    if (m_controller && event->type() == QEvent::Show && !m_attached) {
        m_attached = true;
        m_controller->attach(this, size(), devicePixelRatioF());
    } else if (m_controller && m_attached &&
               (event->type() == QEvent::DevicePixelRatioChange || event->type() == QEvent::Show)) {
        m_controller->setViewport(this, size(), devicePixelRatioF(), true);
    }
    return handled;
}

bool MainWindowSkinWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(watched, event);
}
} // namespace snow_shot::presentation
