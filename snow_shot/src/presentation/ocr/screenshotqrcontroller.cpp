#include "snow_shot/shortcuts/shortcutbinding.h"
#include "snow_shot/presentation/screenshotqrcontroller.h"

#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_shot/presentation/components/icons/iconrenderutils.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "theme/theme_manager.h"
#include "widgets/button.h"
#include "widgets/scroll_area.h"
#include "widgets/detail/top_level_popup_window.h"

#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QScreen>
#include <QTextEdit>
#include <QTextDocument>
#include <QThread>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <optional>

using adqt::widgets::AdButton;

class ScreenshotQrMarker final : public QAbstractButton {
  public:
    ScreenshotQrMarker(SnowCanvasWidget* canvas, qsizetype detection)
        : QAbstractButton(canvas), canvas(canvas), detection(detection) {
        setObjectName(QStringLiteral("screenshotQrMarker"));
        setProperty("qrDetectionIndex", int(detection));
        setFixedSize(24, 24);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
    }
    QPointer<SnowCanvasWidget> canvas;
    qsizetype detection;
    std::optional<QTransform> lastTransform;

  protected:
    void paintEvent(QPaintEvent*) override {
        const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(underMouse() || hasFocus() ? theme.colorPrimaryHover : theme.colorPrimary);
        painter.setPen(QPen(theme.colorBgElevated, hasFocus() ? 3.0 : 2.0));
        painter.drawEllipse(QRectF(rect()).adjusted(1, 1, -1, -1));
        const auto glyph = snow_shot::presentation::icons::renderTintedIconPixmap(
            snow_shot::presentation::icons::custom::outlined::ScanQrcode(), QSize(14, 14),
            devicePixelRatioF(), Qt::white);
        painter.drawPixmap(QPoint(5, 5), glyph);
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (snow_shot::shortcuts::commandKey(*event) == Qt::Key_Return ||
            snow_shot::shortcuts::commandKey(*event) == Qt::Key_Enter) {
            click();
            event->accept();
            return;
        }
        QAbstractButton::keyPressEvent(event);
    }
    bool hitButton(const QPoint& point) const override {
        const QPointF delta = QPointF(point) - QRectF(rect()).center();
        return delta.x() * delta.x() + delta.y() * delta.y() <= 144;
    }
};

// Paint the bubble and arrow as one antialiased surface, including their shared shadow.
class ScreenshotQrBubble final : public QWidget {
  public:
    using QWidget::QWidget;
    bool arrowOnLeft = true;
    qreal arrowY = 40;

  protected:
    void paintEvent(QPaintEvent*) override {
        const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
        const QRectF box = QRectF(rect()).adjusted(12, 12, -12, -12);
        const qreal y = std::clamp(arrowY, box.top() + 16, box.bottom() - 16);
        const qreal x = arrowOnLeft ? box.left() : box.right();
        QPainterPath shape;
        shape.addRoundedRect(box, 8, 8);
        QPainterPath arrow;
        arrow.moveTo(x, y - 8);
        arrow.lineTo(x + (arrowOnLeft ? -8 : 8), y);
        arrow.lineTo(x, y + 8);
        arrow.closeSubpath();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(theme.colorBorderSecondary, 1));
        painter.setBrush(theme.colorBgElevated);
        painter.drawPath(shape.united(arrow));
    }
};

class ScreenshotQrPopover final : public QWidget {
  public:
    explicit ScreenshotQrPopover(QWidget* parent)
        : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
        setObjectName(QStringLiteral("screenshotQrPopover"));
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TranslucentBackground);
        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        body = new ScreenshotQrBubble(this);
        body->setObjectName(QStringLiteral("screenshotQrPopoverBody"));
        outer->addWidget(body);
        auto* shadow = new QGraphicsDropShadowEffect(body);
        shadow->setBlurRadius(18);
        shadow->setOffset(0, 3);
        shadow->setColor(QColor(0, 0, 0, 55));
        body->setGraphicsEffect(shadow);
        auto* layout = new QVBoxLayout(body);
        layout->setContentsMargins(24, 24, 24, 24);
        layout->setSpacing(12);
        text = new QTextEdit(body);
        text->setObjectName(QStringLiteral("screenshotQrPopoverText"));
        text->setReadOnly(true);
        text->setAcceptRichText(false);
        text->setFrameStyle(QFrame::NoFrame);
        text->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        text->setVerticalScrollBar(new adqt::widgets::AdScrollBar(Qt::Vertical, text));
        text->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout->addWidget(text);
        status = new QLabel(body);
        status->setObjectName(QStringLiteral("screenshotQrPopoverStatus"));
        status->setWordWrap(true);
        status->setTextFormat(Qt::PlainText);
        status->hide();
        layout->addWidget(status);
        auto* buttons = new QHBoxLayout;
        buttons->setSpacing(8);
        buttons->addStretch();
        copy = new AdButton(body);
        copy->setObjectName(QStringLiteral("screenshotQrCopyTextButton"));
        open = new AdButton(body);
        open->setObjectName(QStringLiteral("screenshotQrOpenUrlButton"));
        open->setButtonStyle(AdButton::ButtonStyle::Solid);
        open->setAccentRole(AdButton::AccentRole::Primary);
        buttons->addWidget(copy);
        buttons->addWidget(open);
        layout->addLayout(buttons);
        retranslate();
        applyTheme();
        connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged,
                this, [this] { applyTheme(); });
    }
    void retranslate() {
        setAccessibleName(ScreenshotQrController::tr("QR Code"));
        text->setAccessibleName(ScreenshotQrController::tr("QR code text"));
        copy->setText(ScreenshotQrController::tr("Copy Text"));
        open->setText(ScreenshotQrController::tr("Open URL"));
        if (status->isVisible())
            status->setText(ScreenshotQrController::tr("Unable to open the recognized link"));
    }
    void applyTheme() {
        const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
        body->update();
        QPalette colors = palette();
        colors.setColor(QPalette::Base, theme.colorBgElevated);
        colors.setColor(QPalette::Text, theme.colorText);
        colors.setColor(QPalette::WindowText, theme.colorText);
        setPalette(colors);
        text->setPalette(colors);
        status->setPalette(colors);
        text->setStyleSheet(
            QStringLiteral("QTextEdit { background: transparent; color: %1; border: none; }")
                .arg(theme.colorText.name()));
        QFont font = theme.appFont.family().isEmpty() ? QApplication::font() : theme.appFont;
        font.setPixelSize(qRound(theme.fontSize));
        setFont(font);
    }
    void setDetection(const QString& payload) {
        status->hide();
        text->setPlainText(payload);
        url = ScreenshotQrController::webUrl(payload);
        open->setVisible(!url.isEmpty());
        copy->setButtonStyle(url.isEmpty() ? AdButton::ButtonStyle::Solid
                                           : AdButton::ButtonStyle::Outline);
        copy->setAccentRole(url.isEmpty() ? AdButton::AccentRole::Primary
                                          : AdButton::AccentRole::Neutral);
    }
    void place(const QRect& anchor, const QRect& available) {
        lastAnchor = anchor;
        lastAvailable = available;
        const int width = std::min(344, available.width());
        setFixedWidth(std::max(1, width));
        QTextDocument measurement;
        measurement.setDefaultFont(text->font());
        measurement.setDefaultTextOption(text->document()->defaultTextOption());
        measurement.setDocumentMargin(text->document()->documentMargin());
        measurement.setPlainText(text->toPlainText());
        measurement.setTextWidth(std::max(1, width - 52));
        const int textHeight = std::clamp(qCeil(measurement.size().height()) + 8, 48, 240);
        text->setFixedHeight(std::min(textHeight, std::max(24, available.height() - 140)));
        body->layout()->invalidate();
        body->layout()->activate();
        layout()->invalidate();
        layout()->activate();
        setFixedHeight(layout()->sizeHint().height());
        int x = anchor.right() + 1 + 8 - 12;
        body->arrowOnLeft = x + this->width() <= available.right() + 1;
        if (!body->arrowOnLeft)
            x = anchor.left() - 8 - this->width() + 12;
        const int maxX = std::max(available.left(), available.right() + 1 - this->width());
        const int maxY = std::max(available.top(), available.bottom() + 1 - height());
        move(std::clamp(x, available.left(), maxX),
             std::clamp(anchor.center().y() - height() / 2, available.top(), maxY));
        body->arrowY = anchor.center().y() - y();
        body->update();
    }
    void prepareNativeSurface() {
        QWidget* owner = parentWidget()->window();
        setWindowFlag(Qt::WindowStaysOnTopHint,
                      owner->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        if (!owner->windowHandle())
            static_cast<void>(owner->winId());
        // winId() enforces native siblings on the canvas, disrupting marker hover.
        // Create only this tool while retaining its QObject and transient ownership.
        create();
        adqt::widgets::detail::setTopLevelToolTransientParent(this, owner);
#if defined(Q_OS_MACOS)
        adqt::widgets::detail::syncMacTopLevelPopupOwnership(this);
#endif
    }
    void showOpenError() {
        status->setText(ScreenshotQrController::tr("Unable to open the recognized link"));
        status->show();
        place(lastAnchor, lastAvailable);
    }
    ScreenshotQrBubble* body;
    QTextEdit* text;
    QLabel* status;
    AdButton* copy;
    AdButton* open;
    QUrl url;
    QRect lastAnchor;
    QRect lastAvailable;

  protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override {
        adqt::widgets::detail::constrainTopLevelToolStackingToOwner(this, message);
        return QWidget::nativeEvent(eventType, message, result);
    }
};

ScreenshotQrController::ScreenshotQrController(ScreenshotQrRecognitionPort& recognition,
                                               Actions actions, QObject* parent)
    : QObject(parent), m_recognition(&recognition), m_actions(std::move(actions)) {
    if (!m_actions.openUrl)
        m_actions.openUrl = QDesktopServices::openUrl;
    m_hoverTimer.setSingleShot(true);
    m_hoverTimer.setInterval(150);
    m_dismissTimer.setSingleShot(true);
    m_dismissTimer.setInterval(250);
    connect(&m_hoverTimer, &QTimer::timeout, this, [this] {
        if (m_hoverMarker)
            showPopover(m_hoverMarker, false);
    });
    connect(&m_dismissTimer, &QTimer::timeout, this, [this] {
        QWidget* focused = QApplication::focusWidget();
        if (m_popover && m_popover->isVisible() &&
            (m_popover->underMouse() || (focused && m_popover->isAncestorOf(focused))))
            return;
        if (!m_hoverMarker)
            dismissPopover();
    });
    connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
            [this] {
                for (auto marker : m_markers)
                    if (marker)
                        marker->update();
            });
    qApp->installEventFilter(this);
}

ScreenshotQrController::~ScreenshotQrController() {
    invalidate();
    delete m_popover.data();
}

QUrl ScreenshotQrController::webUrl(const QString& text) {
    const QString candidate = text.trimmed();
    // A payload must be one complete URL, not prose containing a link.
    for (const QChar character : candidate)
        if (character.isSpace() || character.category() == QChar::Other_Control)
            return {};
    const QUrl url(candidate, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    return url.isValid() && !url.isRelative() && !url.host().isEmpty() &&
                   (scheme == QStringLiteral("http") || scheme == QStringLiteral("https"))
               ? url
               : QUrl();
}

QImage ScreenshotQrController::prepareImage(const Snapshot& snapshot,
                                            const std::atomic_bool& cancelled) {
    if (snapshot.bounds.isEmpty() || snapshot.pixelSize.isEmpty() || cancelled.load())
        return {};
    const double width = snapshot.pixelSize.width();
    const double height = snapshot.pixelSize.height();
    const double scale = std::min(
        {1.0, std::sqrt(1920.0 * 1080.0 / (width * height)), 2560.0 / std::max(width, height)});
    const QSize pixels(std::max(1, int(std::floor(width * scale))),
                       std::max(1, int(std::floor(height * scale))));
    QImage image(pixels, QImage::Format_RGB32);
    if (image.isNull())
        return {};
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.scale(pixels.width() / snapshot.bounds.width(),
                  pixels.height() / snapshot.bounds.height());
    painter.translate(-snapshot.bounds.topLeft());
    painter.setClipPath(snapshot.selection);
    for (const auto& layer : snapshot.layers) {
        if (cancelled.load())
            return {};
        if (!layer.isValid())
            continue;
        painter.save();
        painter.setClipRect(layer.destinationCanvasRect, Qt::IntersectClip);
        painter.drawImage(layer.imageCanvasRect, layer.image);
        painter.restore();
    }
    return image;
}

void ScreenshotQrController::attachCanvas(SnowCanvasWidget* canvas) {
    if (!canvas || m_canvases.contains(canvas))
        return;
    m_canvases.append(canvas);
    connect(canvas, &SnowCanvasWidget::activeToolChanged, this, [this] { refreshMarkers(); });
    refreshMarkers();
}

void ScreenshotQrController::recognize(Snapshot snapshot) {
    invalidate();
    if (!m_enabled)
        return;
    m_visible = true;
    m_bounds = snapshot.bounds;
    m_sourceSelection = snapshot.selection;
    m_selection = snapshot.selection;
    m_busy = true;
    emit stateChanged();
    const quint64 generation = m_generation;
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    auto image = std::make_shared<QImage>();
    QThread* worker = QThread::create([snapshot = std::move(snapshot), cancelled = m_cancelled,
                                       image] { *image = prepareImage(snapshot, *cancelled); });
    worker->setObjectName(QStringLiteral("ScreenshotQrImageWorker"));
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, generation, image] {
        if (generation != m_generation)
            return;
        if (image->isNull() || !m_recognition) {
            finish(generation, {}, {{}, tr("QR code recognition failed"), {}});
            return;
        }
        const QSize pixels = image->size();
        // A test port may complete synchronously before recognize returns its token.
        auto completed = std::make_shared<bool>(false);
        const auto token = m_recognition->recognize(
            std::move(*image), this,
            [this, generation, pixels, completed](ScreenshotQrRecognitionResult result) {
                *completed = true;
                finish(generation, pixels, std::move(result));
            },
            ScreenshotQrRecognitionMode::QrOnly);
        if (!*completed && generation == m_generation) {
            m_request = token;
            if (!token)
                finish(generation, {}, {{}, tr("QR code recognition failed"), {}});
        }
    });
    worker->start();
}

void ScreenshotQrController::finish(quint64 generation, QSize pixels,
                                    ScreenshotQrRecognitionResult result) {
    if (generation != m_generation)
        return;
    m_request = 0;
    m_busy = false;
    m_cancelled.reset();
    if (!result.error.isEmpty() || pixels.isEmpty()) {
        m_error = tr("QR code recognition failed");
        qWarning("Automatic QR recognition failed");
    } else {
        QTransform toCanvas;
        toCanvas.translate(m_bounds.x(), m_bounds.y());
        toCanvas.scale(m_bounds.width() / pixels.width(), m_bounds.height() / pixels.height());
        for (auto& detection : result.detections) {
            if (detection.text.isEmpty() || detection.corners.size() != 4)
                continue;
            bool valid = true;
            for (const auto& point : detection.corners)
                valid = valid && std::isfinite(point.x()) && std::isfinite(point.y()) &&
                        point.x() >= 0 && point.y() >= 0 && point.x() <= pixels.width() &&
                        point.y() <= pixels.height();
            if (!valid)
                continue;
            detection.corners = toCanvas.map(detection.corners);
            QPainterPath code;
            code.addPolygon(detection.corners);
            code.closeSubpath();
            code = code.simplified();
            if (!code.isEmpty() && !code.boundingRect().isEmpty() &&
                code.subtracted(m_sourceSelection).isEmpty())
                m_detections.append(std::move(detection));
        }
    }
    refreshMarkers();
    emit stateChanged();
}

void ScreenshotQrController::synchronize(const QPainterPath& selection, bool editing,
                                         bool moveActive) {
    // Selection drags temporarily leave Editing. Only the capture lifecycle invalidates results.
    m_selection = selection;
    m_editing = editing;
    m_moveActive = moveActive;
    refreshMarkers();
}

void ScreenshotQrController::setEnabled(bool enabled) {
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    if (!enabled)
        invalidate();
}

void ScreenshotQrController::invalidate() {
    ++m_generation;
    if (m_cancelled)
        m_cancelled->store(true);
    m_cancelled.reset();
    if (m_request && m_recognition)
        m_recognition->cancel(m_request);
    m_request = 0;
    m_busy = false;
    m_error.clear();
    m_detections.clear();
    dismissPopover();
    clearMarkers();
    emit stateChanged();
}

bool ScreenshotQrController::available() const {
    return !m_busy && !m_detections.isEmpty();
}

bool ScreenshotQrController::ownsInput(const QWidget* widget) const {
    return m_popover && m_popover->isVisible() && widget &&
           (widget == m_popover || m_popover->isAncestorOf(widget));
}

void ScreenshotQrController::setMarkersVisible(bool visible) {
    m_visible = visible;
    refreshMarkers();
    emit stateChanged();
}

void ScreenshotQrController::setSuspended(bool suspended) {
    m_suspended = suspended;
    refreshMarkers();
}

void ScreenshotQrController::clearMarkers() {
    for (auto marker : m_markers)
        delete marker.data();
    m_markers.clear();
}

void ScreenshotQrController::refreshMarkers() {
    const bool visible = m_editing && m_moveActive && m_visible && !m_suspended && available();
    if (!visible)
        dismissPopover();
    // Only rebuild when results/canvas membership changes, never on pointer motion.
    m_canvases.removeIf([](const auto& canvas) { return !canvas; });
    m_markers.removeIf([](const auto& marker) { return !marker; });
    if (m_markers.size() != m_canvases.size() * m_detections.size()) {
        dismissPopover();
        clearMarkers();
        for (auto canvas : m_canvases) {
            for (qsizetype index = 0; index < m_detections.size(); ++index) {
                auto* marker = new ScreenshotQrMarker(canvas, index);
                marker->setAccessibleName(tr("QR Code %1").arg(index + 1));
                connect(marker, &QAbstractButton::clicked, this,
                        [this, marker] { showPopover(marker, true); });
                m_markers.append(marker);
            }
        }
    }
    for (auto marker : m_markers) {
        if (!marker || !marker->canvas)
            continue;
        const auto& detection = m_detections[marker->detection];
        QPointF center;
        for (const auto& point : detection.corners)
            center += point / 4.0;
        const QTransform transform = marker->canvas->canvasToViewTransform();
        const QPoint point = transform.map(center).toPoint();
        if (!marker->lastTransform || *marker->lastTransform != transform) {
            marker->lastTransform = transform;
            marker->move(point - QPoint(12, 12));
        }
        marker->setVisible(visible && m_selection.contains(center) &&
                           marker->canvas->rect().contains(point));
        if (!marker->isVisible() && m_popoverMarker == marker)
            dismissPopover();
        if (marker->isVisible())
            marker->raise();
    }
}

void ScreenshotQrController::showPopover(ScreenshotQrMarker* marker, bool focus) {
    if (!marker || !marker->isVisible() || marker->detection >= m_detections.size())
        return;
    m_hoverTimer.stop();
    m_dismissTimer.stop();
    if (!m_popover) {
        m_popover = new ScreenshotQrPopover(marker->window());
        connect(m_popover->copy, &AdButton::clicked, this, [this] {
            if (!m_popoverMarker)
                return;
            if (m_popover->text->textCursor().hasSelection())
                m_popover->text->copy();
            else
                QApplication::clipboard()->setText(m_detections[m_popoverMarker->detection].text);
            dismissPopover();
            if (m_actions.closeEditor)
                m_actions.closeEditor();
        });
        connect(m_popover->open, &AdButton::clicked, this, [this] {
            if (!m_popoverMarker || m_popover->url.isEmpty())
                return;
            if (m_actions.openUrl(m_popover->url)) {
                dismissPopover();
                if (m_actions.closeEditor)
                    m_actions.closeEditor();
            } else {
                m_popover->showOpenError();
            }
        });
    }
    if (m_popover->parentWidget() != marker->window())
        m_popover->setParent(marker->window(), m_popover->windowFlags());
    if (m_popoverMarker != marker || !m_popover->isVisible())
        m_popover->setDetection(m_detections[marker->detection].text);
    m_popoverMarker = marker;
    updatePopoverGeometry();
    const bool opening = !m_popover->isVisible();
    if (opening) {
        m_popover->prepareNativeSurface();
        m_popover->show();
    }
    if (opening || focus)
        m_popover->raise();
    if (focus) {
        m_popover->activateWindow();
        m_popover->text->setFocus(Qt::OtherFocusReason);
    }
}

void ScreenshotQrController::updatePopoverGeometry() {
    if (!m_popover || !m_popoverMarker)
        return;
    const QRect anchor(m_popoverMarker->mapToGlobal(QPoint()), m_popoverMarker->size());
    QScreen* screen = QGuiApplication::screenAt(anchor.center());
    if (!screen)
        screen = m_popoverMarker->screen();
    // Following the canvas must not alter pending hover/dismissal or native stacking.
    m_popover->place(anchor, screen->availableGeometry());
}

void ScreenshotQrController::dismissPopover() {
    m_hoverTimer.stop();
    m_dismissTimer.stop();
    m_hoverMarker.clear();
    m_popoverMarker.clear();
    if (m_popover)
        m_popover->hide();
}

void ScreenshotQrController::scheduleDismiss() {
    m_hoverTimer.stop();
    m_hoverMarker.clear();
    m_dismissTimer.start();
}

bool ScreenshotQrController::eventFilter(QObject* watched, QEvent* event) {
    auto* widget = qobject_cast<QWidget*>(watched);
    const bool inPopover =
        m_popover && widget && (widget == m_popover || m_popover->isAncestorOf(widget));
    if (inPopover && event->type() == QEvent::ShortcutOverride) {
        event->accept();
        return true;
    }
    if (m_popover && m_popover->isVisible() && event->type() == QEvent::KeyPress &&
        snow_shot::shortcuts::commandKey(*static_cast<QKeyEvent*>(event)) == Qt::Key_Escape) {
        QPointer<ScreenshotQrMarker> marker = m_popoverMarker;
        dismissPopover();
        if (marker) {
            marker->window()->activateWindow();
            marker->setFocus(Qt::OtherFocusReason);
        }
        return true;
    }
    if (inPopover) {
        if (event->type() == QEvent::Enter)
            m_dismissTimer.stop();
        else if (event->type() == QEvent::Leave || event->type() == QEvent::FocusOut)
            scheduleDismiss();
        else if (event->type() == QEvent::LanguageChange) {
            m_popover->retranslate();
            if (!m_error.isEmpty()) {
                m_error = tr("QR code recognition failed");
                emit stateChanged();
            }
        }
    }
    for (auto marker : m_markers) {
        if (marker != watched)
            continue;
        if (event->type() == QEvent::Enter) {
            m_dismissTimer.stop();
            m_hoverMarker = marker;
            m_hoverTimer.start();
        } else if (event->type() == QEvent::Leave) {
            scheduleDismiss();
        } else if (event->type() == QEvent::LanguageChange) {
            marker->setAccessibleName(tr("QR Code %1").arg(marker->detection + 1));
        }
        break;
    }
    // Native presses reach QWindow before Qt dispatches them to the target QWidget.
    // Only classify the widget delivery; a non-widget receiver is not an outside click.
    if (widget && event->type() == QEvent::MouseButtonPress && !inPopover &&
        widget != m_popoverMarker && m_popover && m_popover->isVisible())
        dismissPopover();
    if (widget && event->type() == QEvent::Paint) {
        for (auto marker : m_markers) {
            if (marker && marker->canvas == widget && marker->lastTransform &&
                *marker->lastTransform != marker->canvas->canvasToViewTransform()) {
                refreshMarkers();
                if (m_popoverMarker && m_popover && m_popover->isVisible())
                    updatePopoverGeometry();
                break;
            }
        }
    }
    if (widget && event->type() == QEvent::LanguageChange && !m_error.isEmpty()) {
        for (auto canvas : m_canvases)
            if (canvas == widget) {
                m_error = tr("QR code recognition failed");
                emit stateChanged();
                break;
            }
    }
    if (widget && (event->type() == QEvent::Resize || event->type() == QEvent::Show ||
                   event->type() == QEvent::Move)) {
        for (auto canvas : m_canvases) {
            if (canvas && (widget == canvas || widget == canvas->window())) {
                refreshMarkers();
                if (m_popoverMarker && m_popover && m_popover->isVisible())
                    updatePopoverGeometry();
                break;
            }
        }
    }
    if (widget && event->type() == QEvent::Hide) {
        for (auto canvas : m_canvases)
            if (canvas && widget == canvas->window()) {
                dismissPopover();
                break;
            }
    }
    return QObject::eventFilter(watched, event);
}
