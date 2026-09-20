#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "icons/draw_engine_icons.h"
#include "icon_renderer.h"

#include "snow_draw_engine_qt/snow_canvas_view.h"

#include <QBitmap>
#include <QApplication>
#include <QEnterEvent>
#include <QFocusEvent>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPointer>
#include <QResizeEvent>
#include <QToolButton>
#include <QWheelEvent>
#include <QWindow>
#include <utility>

namespace {
constexpr int kSerialToolbarButtonSize = 24;
constexpr int kSerialToolbarIconSize = 14;
constexpr int kSerialToolbarWidth = kSerialToolbarButtonSize * 3;
constexpr int kSerialToolbarHeight = kSerialToolbarButtonSize;
constexpr int kSerialToolbarRadius = 10;

enum class SerialToolbarIcon : std::uint8_t {
    Decrease,
    Increase,
    TextFields,
};

QIcon serialToolbarIcon(SerialToolbarIcon icon) {
    adqt::icons::IconRef ref;
    switch (icon) {
    case SerialToolbarIcon::Decrease:
        ref = snow::draw_engine::icons::toolbar::SerialDecrease();
        break;
    case SerialToolbarIcon::Increase:
        ref = snow::draw_engine::icons::toolbar::SerialIncrease();
        break;
    case SerialToolbarIcon::TextFields:
        ref = snow::draw_engine::icons::toolbar::SerialTextFields();
        break;
    }
    adqt::icons::IconStatePalette palette;
    palette.set(QIcon::Normal, QIcon::Off, adqt::icons::IconColors::primary(QColor(29, 27, 32)));
    palette.set(QIcon::Disabled, QIcon::Off,
                adqt::icons::IconColors::primary(QColor(73, 69, 79, 128)));
    return adqt::icons::makeIcon(ref, palette);
}

void applySerialToolbarMask(QWidget* toolbar) {
    if (toolbar == nullptr || toolbar->size().isEmpty()) {
        return;
    }

    QBitmap mask(toolbar->size());
    mask.fill(Qt::color0);

    QPainter painter(&mask);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::color1);
    painter.drawRoundedRect(QRectF(0.0, 0.0, toolbar->width(), toolbar->height()),
                            kSerialToolbarRadius, kSerialToolbarRadius);
    toolbar->setMask(mask);
}

QToolButton* createSerialToolbarButton(QWidget* parent, const QString& objectName,
                                       SerialToolbarIcon icon, const QString& toolTip) {
    QToolButton* button = new QToolButton(parent);
    button->setObjectName(objectName);
    button->setText(QString());
    button->setIcon(serialToolbarIcon(icon));
    button->setIconSize(QSize(kSerialToolbarIconSize, kSerialToolbarIconSize));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setToolTip(toolTip);
    button->setToolTipDuration(-1);
    button->setAccessibleName(toolTip);
    button->setAutoRaise(true);
    button->setCursor(Qt::ArrowCursor);
    button->setFixedSize(kSerialToolbarButtonSize, kSerialToolbarButtonSize);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

} // namespace

struct SnowCanvasWidget::Impl {
    SnowCanvasWidget& widget;
    std::unique_ptr<SnowCanvasView> view;
    explicit Impl(SnowCanvasWidget& owner, SnowCanvasRuntime* runtime = nullptr) : widget(owner) {
        SnowCanvasHostCallbacks host;
        host.repaint = [&owner](const QRegion& region) { owner.update(region); };
        host.focus = [&owner](Qt::FocusReason reason) { owner.setFocus(reason); };
        host.clearFocus = [&owner]() { owner.clearFocus(); };
        host.hasFocus = [&owner]() { return owner.hasFocus(); };
        host.capture = [&owner](bool capture) {
            if (capture)
                owner.grabMouse();
            else
                owner.releaseMouse();
        };
        host.cursor = [&owner](const std::optional<QCursor>& cursor) {
            if (cursor)
                owner.setCursor(*cursor);
            else
                owner.unsetCursor();
        };
        host.inputMethodEnabled = [&owner](bool enabled) {
            owner.setAttribute(Qt::WA_InputMethodEnabled, enabled);
        };
        host.inputMethodQuery = [&owner](Qt::InputMethodQuery query) {
            return owner.QWidget::inputMethodQuery(query);
        };
        host.font = [&owner]() { return owner.font(); };
        host.window = [&owner]() { return owner.window()->windowHandle(); };
        host.mapFromGlobal = [&owner](const QPointF& point) { return owner.mapFromGlobal(point); };
        host.mapToGlobal = [&owner](const QPointF& point) { return owner.mapToGlobal(point); };
        view = runtime ? std::make_unique<SnowCanvasView>(*runtime, std::move(host), &owner)
                       : std::make_unique<SnowCanvasView>(std::move(host), &owner);
        view->setLogicalSurfaceSize(owner.size(), owner.devicePixelRatioF());
    }
};

SnowCanvasWidget::SnowCanvasWidget(QWidget* parent)
    : QWidget(parent), m_impl(std::make_unique<Impl>(*this)) {
    initializeAdapter();
}
SnowCanvasWidget::SnowCanvasWidget(SnowCanvasRuntime& runtime, QWidget* parent)
    : QWidget(parent), m_impl(std::make_unique<Impl>(*this, &runtime)) {
    initializeAdapter();
}
SnowCanvasWidget::~SnowCanvasWidget() = default;
SnowCanvasView& SnowCanvasWidget::view() const {
    return *m_impl->view;
}

void SnowCanvasWidget::initializeAdapter() {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    connect(m_impl->view.get(), &SnowCanvasView::autoFilterRegionsChanged, this,
            &SnowCanvasWidget::autoFilterRegionsChanged);
    connect(m_impl->view.get(), &SnowCanvasView::autoFilterInteractionStarting, this,
            &SnowCanvasWidget::autoFilterInteractionStarting);
    connect(m_impl->view.get(), &SnowCanvasView::activeToolChanged, this,
            &SnowCanvasWidget::activeToolChanged);
    connect(m_impl->view.get(), &SnowCanvasView::styleToolbarStateChanged, this,
            &SnowCanvasWidget::styleToolbarStateChanged);
    connect(m_impl->view.get(), &SnowCanvasView::historyStateChanged, this,
            &SnowCanvasWidget::historyStateChanged);
    connect(m_impl->view.get(), &SnowCanvasView::snapConfigChanged, this,
            &SnowCanvasWidget::snapConfigChanged);
    connect(m_impl->view.get(), &SnowCanvasView::gridConfigChanged, this,
            &SnowCanvasWidget::gridConfigChanged);
    connect(m_impl->view.get(), &SnowCanvasView::watermarkPreviewApplied, this,
            &SnowCanvasWidget::watermarkPreviewApplied);
    connect(m_impl->view.get(), &SnowCanvasView::spotlightPreviewApplied, this,
            &SnowCanvasWidget::spotlightPreviewApplied);
    connect(m_impl->view.get(), &SnowCanvasView::freeDrawMoveBatchProcessed, this,
            &SnowCanvasWidget::freeDrawMoveBatchProcessed);
    connect(m_impl->view.get(), &SnowCanvasView::eraserMoveFrameProcessed, this,
            &SnowCanvasWidget::eraserMoveFrameProcessed);
    connect(m_impl->view.get(), &SnowCanvasView::unhandledLeftDoubleClick, this,
            &SnowCanvasWidget::unhandledLeftDoubleClick);
    connect(m_impl->view.get(), &SnowCanvasView::unhandledMiddleClick, this,
            &SnowCanvasWidget::unhandledMiddleClick);
    connect(m_impl->view.get(), &SnowCanvasView::showDirtyRectsChanged, this,
            &SnowCanvasWidget::showDirtyRectsChanged);
    createSnowCanvasSerialNumberToolbar(*m_impl->view, this);
}

SnowCanvasTool SnowCanvasWidget::canvasTool() const {
    return m_impl->view->canvasTool();
}

bool SnowCanvasWidget::setCanvasTool(SnowCanvasTool tool) {
    return m_impl->view->setCanvasTool(tool);
}

void SnowCanvasWidget::setCursorForLayer(SnowCanvasCursorLayer layer, const QCursor& cursor) {
    m_impl->view->setCursorForLayer(layer, cursor);
}

void SnowCanvasWidget::clearCursorForLayer(SnowCanvasCursorLayer layer) {
    m_impl->view->clearCursorForLayer(layer);
}

SnowCanvasStyleToolbarState SnowCanvasWidget::canvasStyleToolbarState() const {
    return m_impl->view->canvasStyleToolbarState();
}

SnowCanvasSerialNumberToolbarState SnowCanvasWidget::serialNumberToolbarState() const {
    return m_impl->view->serialNumberToolbarState();
}

SnowCanvasWatermarkConfig SnowCanvasWidget::canvasWatermarkConfig() const {
    return m_impl->view->canvasWatermarkConfig();
}

bool SnowCanvasWidget::setCanvasWatermarkConfig(const SnowCanvasWatermarkConfig& config) {
    return m_impl->view->setCanvasWatermarkConfig(config);
}

void SnowCanvasWidget::previewCanvasWatermarkConfig(const SnowCanvasWatermarkConfig& config) {
    m_impl->view->previewCanvasWatermarkConfig(config);
}

SnowCanvasSpotlightConfig SnowCanvasWidget::canvasSpotlightConfig() const {
    return m_impl->view->canvasSpotlightConfig();
}

bool SnowCanvasWidget::setCanvasSpotlightConfig(const SnowCanvasSpotlightConfig& config) {
    return m_impl->view->setCanvasSpotlightConfig(config);
}

void SnowCanvasWidget::previewCanvasSpotlightConfig(const SnowCanvasSpotlightConfig& config) {
    m_impl->view->previewCanvasSpotlightConfig(config);
}

bool SnowCanvasWidget::setCanvasShapeStylePatch(const SnowCanvasShapeStyle& style,
                                                quint32 properties, SnowCanvasShapeKind kind) {
    return m_impl->view->setCanvasShapeStylePatch(style, properties, kind);
}

bool SnowCanvasWidget::setCanvasFilterStyle(const SnowCanvasFilterStyle& style,
                                            quint32 properties) {
    return m_impl->view->setCanvasFilterStyle(style, properties);
}

bool SnowCanvasWidget::setCanvasTextStyle(const SnowCanvasTextStyle& style) {
    return m_impl->view->setCanvasTextStyle(style);
}

bool SnowCanvasWidget::setCanvasSerialNumberStyle(const SnowCanvasSerialNumberStyle& style) {
    return m_impl->view->setCanvasSerialNumberStyle(style);
}

SnowCanvasHistoryState SnowCanvasWidget::canvasHistoryState() const {
    return m_impl->view->canvasHistoryState();
}

SnowCanvasSnapConfig SnowCanvasWidget::canvasSnapConfig() const {
    return m_impl->view->canvasSnapConfig();
}

bool SnowCanvasWidget::setCanvasSnapConfig(const SnowCanvasSnapConfig& config) {
    return m_impl->view->setCanvasSnapConfig(config);
}

SnowCanvasGridConfig SnowCanvasWidget::canvasGridConfig() const {
    return m_impl->view->canvasGridConfig();
}

bool SnowCanvasWidget::setCanvasGridConfig(const SnowCanvasGridConfig& config) {
    return m_impl->view->setCanvasGridConfig(config);
}

bool SnowCanvasWidget::interactionEnabled() const {
    return m_impl->view->interactionEnabled();
}

void SnowCanvasWidget::setInteractionEnabled(bool enabled) {
    m_impl->view->setInteractionEnabled(enabled);
}

bool SnowCanvasWidget::wheelZoomEnabled() const {
    return m_impl->view->wheelZoomEnabled();
}

void SnowCanvasWidget::setWheelZoomEnabled(bool enabled) {
    m_impl->view->setWheelZoomEnabled(enabled);
}

bool SnowCanvasWidget::canvasContentVisible() const {
    return m_impl->view->canvasContentVisible();
}

void SnowCanvasWidget::setCanvasContentVisible(bool visible) {
    m_impl->view->setCanvasContentVisible(visible);
}

bool SnowCanvasWidget::clearBackgroundEnabled() const {
    return m_impl->view->clearBackgroundEnabled();
}

void SnowCanvasWidget::setClearBackgroundEnabled(bool enabled) {
    m_impl->view->setClearBackgroundEnabled(enabled);
}

bool SnowCanvasWidget::showDirtyRects() const {
    return m_impl->view->showDirtyRects();
}

std::uint64_t SnowCanvasWidget::viewportId() const {
    return m_impl->view->viewportId();
}

bool SnowCanvasWidget::setViewportCamera(double centerX, double centerY, double zoom) {
    return m_impl->view->setViewportCamera(centerX, centerY, zoom);
}

bool SnowCanvasWidget::hasWatermarkRenderArea() const {
    return m_impl->view->hasWatermarkRenderArea();
}

QRectF SnowCanvasWidget::watermarkRenderArea() const {
    return m_impl->view->watermarkRenderArea();
}

void SnowCanvasWidget::setWatermarkRenderArea(const QRectF& canvasRect) {
    m_impl->view->setWatermarkRenderArea(canvasRect);
}

void SnowCanvasWidget::setDecorationRenderAreas(const SnowCanvasDecorationRenderAreas& areas) {
    m_impl->view->setDecorationRenderAreas(areas);
}

void SnowCanvasWidget::clearWatermarkRenderArea() {
    m_impl->view->clearWatermarkRenderArea();
}

bool SnowCanvasWidget::hasSpotlightRenderArea() const {
    return m_impl->view->hasSpotlightRenderArea();
}

QRectF SnowCanvasWidget::spotlightRenderArea() const {
    return m_impl->view->spotlightRenderArea();
}

void SnowCanvasWidget::setSpotlightRenderArea(const QRectF& canvasRect) {
    m_impl->view->setSpotlightRenderArea(canvasRect);
}

void SnowCanvasWidget::clearSpotlightRenderArea() {
    m_impl->view->clearSpotlightRenderArea();
}

SnowCanvasCustomRenderer* SnowCanvasWidget::customRenderer() const {
    return m_impl->view->customRenderer();
}

void SnowCanvasWidget::setCustomRenderer(SnowCanvasCustomRenderer* renderer) {
    m_impl->view->setCustomRenderer(renderer);
}

QTransform SnowCanvasWidget::canvasToViewTransform() const {
    return m_impl->view->canvasToViewTransform();
}

QRect SnowCanvasWidget::viewRectForCanvasRect(const QRectF& canvasRect, int paddingPx) const {
    return m_impl->view->viewRectForCanvasRect(canvasRect, paddingPx);
}

bool SnowCanvasWidget::undo() {
    return m_impl->view->undo();
}

bool SnowCanvasWidget::redo() {
    return m_impl->view->redo();
}

bool SnowCanvasWidget::deleteSelected() {
    return m_impl->view->deleteSelected();
}

bool SnowCanvasWidget::deleteAllElements() {
    return m_impl->view->deleteAllElements();
}

bool SnowCanvasWidget::clearDocument() {
    return m_impl->view->clearDocument();
}

bool SnowCanvasWidget::duplicateSelected(const QPointF& offset) {
    return m_impl->view->duplicateSelected(offset);
}

bool SnowCanvasWidget::reorderSelected(SnowCanvasSelectionOrder order) {
    return m_impl->view->reorderSelected(order);
}

bool SnowCanvasWidget::alignSelected(SnowCanvasSelectionAlignment alignment) {
    return m_impl->view->alignSelected(alignment);
}

bool SnowCanvasWidget::setSelectedOpacity(double opacity) {
    return m_impl->view->setSelectedOpacity(opacity);
}

bool SnowCanvasWidget::adjustSelectedSerialNumbers(qint64 delta) {
    return m_impl->view->adjustSelectedSerialNumbers(delta);
}

bool SnowCanvasWidget::editSelectedArrowText() {
    return m_impl->view->editSelectedArrowText();
}

bool SnowCanvasWidget::createSerialNumberText() {
    return m_impl->view->createSerialNumberText();
}

bool SnowCanvasWidget::resetEditingState() {
    return m_impl->view->resetEditingState();
}

bool SnowCanvasWidget::resetEditingStatePreservingTool() {
    return m_impl->view->resetEditingStatePreservingTool();
}

void SnowCanvasWidget::clearRenderState() {
    m_impl->view->clearRenderState();
}

bool SnowCanvasWidget::cancelActiveTextEditing() {
    return m_impl->view->cancelActiveTextEditing();
}

bool SnowCanvasWidget::hasActiveTextEditing() const {
    return m_impl->view->hasActiveTextEditing();
}

void SnowCanvasWidget::beginTextStylePopupInteraction() {
    m_impl->view->beginTextStylePopupInteraction();
}

void SnowCanvasWidget::endTextStylePopupInteraction(QWidget* focusScope) {
    m_impl->view->endTextStylePopupInteraction(focusScope);
}

QVariant SnowCanvasWidget::inputMethodQuery(Qt::InputMethodQuery query) const {
    return m_impl->view->inputMethodQuery(query);
}

void SnowCanvasWidget::setShowDirtyRects(bool show) {
    m_impl->view->setShowDirtyRects(show);
}

quint64 SnowCanvasWidget::autoFilterGeneration() const {
    return m_impl->view->autoFilterGeneration();
}

std::optional<SnowCanvasAutoFilterRecord> SnowCanvasWidget::autoFilterRegions() const {
    return m_impl->view->autoFilterRegions();
}

bool SnowCanvasWidget::setAutoFilterRegions(
    const std::optional<SnowCanvasAutoFilterRecord>& record) {
    return m_impl->view->setAutoFilterRegions(record);
}

bool SnowCanvasWidget::fillAutoFilterCategory(const QString& category) {
    return m_impl->view->fillAutoFilterCategory(category);
}

void SnowCanvasWidget::setBaseImageSources(const QList<SnowCanvasBaseImageSource>& sources) {
    m_impl->view->setBaseImageSources(sources);
}

void SnowCanvasWidget::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    m_impl->view->render(painter, event ? event->region() : QRegion(rect()));
}
bool SnowCanvasWidget::event(QEvent* event) {
    if (m_impl && event->type() == QEvent::DevicePixelRatioChange)
        m_impl->view->setLogicalSurfaceSize(size(), devicePixelRatioF());
    return QWidget::event(event);
}
bool SnowCanvasWidget::eventFilter(QObject* watched, QEvent* event) {
    return QWidget::eventFilter(watched, event);
}
void SnowCanvasWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    m_impl->view->setLogicalSurfaceSize(event->size(), devicePixelRatioF());
    QCoreApplication::sendEvent(m_impl->view.get(), event);
}

void SnowCanvasWidget::mousePressEvent(QMouseEvent* event) {
    if (!QCoreApplication::sendEvent(m_impl->view.get(), event))
        QWidget::mousePressEvent(event);
}

void SnowCanvasWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (!QCoreApplication::sendEvent(m_impl->view.get(), event))
        QWidget::mouseDoubleClickEvent(event);
}

void SnowCanvasWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!QCoreApplication::sendEvent(m_impl->view.get(), event))
        QWidget::mouseMoveEvent(event);
}

void SnowCanvasWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (!QCoreApplication::sendEvent(m_impl->view.get(), event))
        QWidget::mouseReleaseEvent(event);
}

void SnowCanvasWidget::enterEvent(QEnterEvent* event) {
    if (!QCoreApplication::sendEvent(m_impl->view.get(), event))
        QWidget::enterEvent(event);
}

void SnowCanvasWidget::leaveEvent(QEvent* event) {
    if (!QCoreApplication::sendEvent(m_impl->view.get(), event))
        QWidget::leaveEvent(event);
}

void SnowCanvasWidget::wheelEvent(QWheelEvent* event) {
    if (!QCoreApplication::sendEvent(m_impl->view.get(), event))
        QWidget::wheelEvent(event);
}

void SnowCanvasWidget::keyPressEvent(QKeyEvent* event) {
    if (!QCoreApplication::sendEvent(m_impl->view.get(), event))
        QWidget::keyPressEvent(event);
}

void SnowCanvasWidget::keyReleaseEvent(QKeyEvent* event) {
    if (!QCoreApplication::sendEvent(m_impl->view.get(), event))
        QWidget::keyReleaseEvent(event);
}

void SnowCanvasWidget::inputMethodEvent(QInputMethodEvent* event) {
    if (!QCoreApplication::sendEvent(m_impl->view.get(), event))
        QWidget::inputMethodEvent(event);
}

void SnowCanvasWidget::focusOutEvent(QFocusEvent* event) {
    if (!QCoreApplication::sendEvent(m_impl->view.get(), event))
        QWidget::focusOutEvent(event);
}

namespace {
class SerialToolbarController final : public QObject {
  public:
    SerialToolbarController(SnowCanvasView& canvas, QWidget* parent, QWindow* surface)
        : QObject(parent), widget(*parent), view(&canvas), owner(surface) {
        initialize();
        if (owner) {
            serialNumberToolbar->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint |
                                                Qt::WindowStaysOnTopHint);
            serialNumberToolbar->winId();
            serialNumberToolbar->windowHandle()->setTransientParent(owner);
            connect(owner, &QWindow::visibleChanged, this, [this] { refresh(); });
            connect(owner, &QWindow::xChanged, this, [this] { refresh(); });
            connect(owner, &QWindow::yChanged, this, [this] { refresh(); });
        }
        connect(&canvas, &SnowCanvasView::serialNumberToolbarStateChanged, this,
                [this] { refresh(); });
        connect(&canvas, &QObject::destroyed, serialNumberToolbar, &QWidget::hide);
        refresh();
    }
    QWidget* toolbar() const {
        return serialNumberToolbar;
    }

  private:
    void initialize();
    void refresh();
    QWidget& widget;
    QPointer<SnowCanvasView> view;
    QPointer<QWindow> owner;
    QWidget* serialNumberToolbar = nullptr;
    QToolButton* serialNumberDecreaseButton = nullptr;
    QToolButton* serialNumberIncreaseButton = nullptr;
    QToolButton* serialNumberCreateTextButton = nullptr;
};
void SerialToolbarController::initialize() {
    serialNumberToolbar = new QWidget(&widget);
    serialNumberToolbar->setObjectName(QStringLiteral("snowSerialNumberToolbar"));
    serialNumberToolbar->setAttribute(Qt::WA_StyledBackground, true);
    serialNumberToolbar->setFixedSize(kSerialToolbarWidth, kSerialToolbarHeight);
    applySerialToolbarMask(serialNumberToolbar);
    serialNumberToolbar->setStyleSheet(
        QStringLiteral("#snowSerialNumberToolbar {"
                       "background: white;"
                       "border: none;"
                       "border-radius: 10px;"
                       "}"
                       "#snowSerialNumberToolbar QToolButton {"
                       "border: 0;"
                       "border-radius: 0;"
                       "padding: 0;"
                       "}"
                       "#snowSerialNumberToolbarDecreaseButton {"
                       "border-top-left-radius: 10px;"
                       "border-bottom-left-radius: 10px;"
                       "}"
                       "#snowSerialNumberToolbarCreateTextButton {"
                       "border-top-right-radius: 10px;"
                       "border-bottom-right-radius: 10px;"
                       "}"
                       "#snowSerialNumberToolbar QToolButton:hover:enabled {"
                       "background: rgba(30, 30, 30, 18);"
                       "}"
                       "#snowSerialNumberToolbar QToolButton:pressed:enabled {"
                       "background: rgba(30, 30, 30, 32);"
                       "}"
                       "#snowSerialNumberToolbar QToolButton:disabled {"
                       "color: rgba(30, 30, 30, 76);"
                       "}"));

    auto* shadow = new QGraphicsDropShadowEffect(serialNumberToolbar);
    shadow->setBlurRadius(10.0);
    shadow->setOffset(0.0, 2.0);
    shadow->setColor(QColor(0, 0, 0, 48));
    serialNumberToolbar->setGraphicsEffect(shadow);

    auto* layout = new QHBoxLayout(serialNumberToolbar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    serialNumberDecreaseButton = createSerialToolbarButton(
        serialNumberToolbar, QStringLiteral("snowSerialNumberToolbarDecreaseButton"),
        SerialToolbarIcon::Decrease, QStringLiteral("Decrease"));
    serialNumberIncreaseButton = createSerialToolbarButton(
        serialNumberToolbar, QStringLiteral("snowSerialNumberToolbarIncreaseButton"),
        SerialToolbarIcon::Increase, QStringLiteral("Increase"));
    serialNumberCreateTextButton = createSerialToolbarButton(
        serialNumberToolbar, QStringLiteral("snowSerialNumberToolbarCreateTextButton"),
        SerialToolbarIcon::TextFields, QStringLiteral("Create Text"));

    layout->addWidget(serialNumberDecreaseButton);
    layout->addWidget(serialNumberIncreaseButton);
    layout->addWidget(serialNumberCreateTextButton);

    QObject::connect(serialNumberDecreaseButton, &QToolButton::clicked, this, [this]() {
        view->adjustSelectedSerialNumbers(-1);
        refresh();
    });
    QObject::connect(serialNumberIncreaseButton, &QToolButton::clicked, this, [this]() {
        view->adjustSelectedSerialNumbers(1);
        refresh();
    });
    QObject::connect(serialNumberCreateTextButton, &QToolButton::clicked, this, [this]() {
        view->createSerialNumberText();
        refresh();
    });

    serialNumberToolbar->hide();
}
void SerialToolbarController::refresh() {
    if (!view) {
        serialNumberToolbar->hide();
        return;
    }
    if (serialNumberToolbar == nullptr) {
        return;
    }

    const SnowCanvasSerialNumberToolbarState state = view->serialNumberToolbarState();
    if (!view || !view->canvasContentVisible() || !state.visible ||
        (owner && !owner->isVisible())) {
        serialNumberToolbar->hide();
        return;
    }

    const bool enabled = view->interactionEnabled();
    serialNumberDecreaseButton->setEnabled(enabled && state.canDecrease);
    serialNumberIncreaseButton->setEnabled(enabled && state.canIncrease);
    serialNumberCreateTextButton->setEnabled(enabled && state.canCreateText);
    const QRect geometry = state.geometry.toAlignedRect();
    serialNumberToolbar->setGeometry(
        owner ? QRect(owner->mapToGlobal(geometry.topLeft()), geometry.size()) : geometry);
    applySerialToolbarMask(serialNumberToolbar);
    serialNumberToolbar->raise();
    serialNumberToolbar->show();
}
} // namespace
QWidget* createSnowCanvasSerialNumberToolbar(SnowCanvasView& view, QWidget* parent,
                                             QWindow* owner) {
    if (!parent)
        return nullptr;
    return (new SerialToolbarController(view, parent, owner))->toolbar();
}
