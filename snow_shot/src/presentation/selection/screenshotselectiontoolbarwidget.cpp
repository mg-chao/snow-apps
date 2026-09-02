#include "snow_shot/presentation/screenshotselectiontoolbarwidget.h"

#include "screenshotselectiontoolbarwidgets.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/screenshotselectionlimits.h"
#include "snow_shot/presentation/screenshottoolbarcommands.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QHoverEvent>
#include <QLabel>
#include <QMargins>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPoint>
#include <QSizePolicy>
#include <QShowEvent>
#include <QTimer>
#include <QVariant>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>

namespace {
namespace custom_outlined_icons = snow_shot::presentation::icons::custom::outlined;
namespace toolbar_widgets = screenshot_selection_toolbar;
using snow_shot::presentation::kScreenshotSelectionCornerRadiusMax;
using snow_shot::presentation::kScreenshotSelectionShadowWidthMax;

constexpr auto kAlwaysMouseTransparentProperty = "selectionToolbarAlwaysMouseTransparent";
constexpr auto kTranslationSourceProperty = "selectionToolbarTranslationSource";

void setTranslationSource(QLabel* label, const char* source) {
    if (label != nullptr && source != nullptr && source[0] != '\0') {
        label->setProperty(kTranslationSourceProperty, QString::fromUtf8(source));
    }
}

QString translateToolbarText(const char* source) {
    return QCoreApplication::translate("ScreenshotSelectionToolbarWidget", source);
}

QString pxText(int value) {
    return QStringLiteral("%1").arg(value);
}

bool updateLabelText(QLabel* label, const QString& text, bool refreshGeometry) {
    if (label == nullptr) {
        return false;
    }

    const bool textChanged = label->text() != text;
    if (!textChanged && !refreshGeometry) {
        return false;
    }
    if (textChanged) {
        label->setText(text);
    }

    const QSize nextSize = label->sizeHint();
    const bool geometryChanged = label->size() != nextSize;
    if (geometryChanged || refreshGeometry) {
        label->setFixedSize(nextSize);
    }
    return geometryChanged;
}

int wheelVerticalDelta(const QWheelEvent* event) {
    if (event == nullptr) {
        return 0;
    }

    const QPoint pixelDelta = event->pixelDelta();
    if (!pixelDelta.isNull()) {
        return pixelDelta.y();
    }
    return event->angleDelta().y();
}

bool objectInsidePanel(QObject* object, const QWidget* panel) {
    if (object == nullptr || panel == nullptr) {
        return false;
    }
    if (object == panel) {
        return true;
    }

    auto* widget = qobject_cast<QWidget*>(object);
    while (widget != nullptr) {
        if (widget == panel) {
            return true;
        }
        widget = widget->parentWidget();
    }
    return false;
}

void setMouseTransparentForWidget(QWidget* widget, bool transparent) {
    if (widget == nullptr) {
        return;
    }

    const bool alwaysTransparent = widget->property(kAlwaysMouseTransparentProperty).toBool();
    const bool interactionEnabled = !transparent && !alwaysTransparent;
    if (auto* panel = dynamic_cast<SelectionToolbarPanel*>(widget)) {
        panel->setPointerInteractionEnabled(interactionEnabled);
        return;
    }
    if (auto* valueLabel = dynamic_cast<SelectionToolbarValueLabel*>(widget)) {
        valueLabel->setPointerInteractionEnabled(interactionEnabled);
        return;
    }
    widget->setAttribute(Qt::WA_TransparentForMouseEvents, !interactionEnabled);
}

QPointF globalPositionForPointerEvent(const QObject* watched, const QEvent* event) {
    if (event == nullptr) {
        return {};
    }

    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseMove:
    case QEvent::MouseButtonRelease:
        return static_cast<const QMouseEvent*>(event)->globalPosition();
    case QEvent::Wheel:
        return static_cast<const QWheelEvent*>(event)->globalPosition();
    case QEvent::Enter:
        return static_cast<const QEnterEvent*>(event)->globalPosition();
    case QEvent::HoverEnter:
    case QEvent::HoverMove: {
        const auto* widget = qobject_cast<const QWidget*>(watched);
        const auto* hoverEvent = static_cast<const QHoverEvent*>(event);
        return widget != nullptr ? QPointF(widget->mapToGlobal(hoverEvent->position().toPoint()))
                                 : QPointF();
    }
    default:
        return {};
    }
}

ScreenshotOverlayWindow* overlayParentForObject(const QObject* object) {
    for (const QObject* parent = object != nullptr ? object->parent() : nullptr; parent != nullptr;
         parent = parent->parent()) {
        auto* overlay = qobject_cast<ScreenshotOverlayWindow*>(const_cast<QObject*>(parent));
        if (overlay != nullptr) {
            return overlay;
        }
    }

    return nullptr;
}
} // namespace

Qt::CursorShape ScreenshotSelectionToolbarWidget::cursorShapeForField(Field field) {
    switch (field) {
    case Field::PositionX:
    case Field::Width:
        return Qt::SplitHCursor;
    case Field::PositionY:
    case Field::Height:
    case Field::Radius:
    case Field::Shadow:
        return Qt::SplitVCursor;
    }

    return Qt::ArrowCursor;
}

ScreenshotSelectionToolbarWidget::ScreenshotSelectionToolbarWidget(
    ScreenshotSelectionToolbarCommandSink& commands, QWidget* parent)
    : QWidget(parent), m_commands(commands) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_Hover, true);
    setFocusPolicy(Qt::NoFocus);
    setAutoFillBackground(false);
    setMouseTracking(true);

    auto* rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(toolbar_widgets::ShadowMargin, toolbar_widgets::ShadowMargin,
                                   toolbar_widgets::ShadowMargin, toolbar_widgets::ShadowMargin);
    rootLayout->setSpacing(0);

    auto* panel = new SelectionToolbarPanel(this);
    m_panel = panel;
    panel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    panel->setCursor(Qt::PointingHandCursor);
    connect(panel, &SelectionToolbarPanel::hoverChanged, this,
            &ScreenshotSelectionToolbarWidget::setToolbarHovered);
    panel->installEventFilter(this);

    auto* panelLayout = new QHBoxLayout(panel);
    panelLayout->setContentsMargins(
        toolbar_widgets::PanelHorizontalPadding, toolbar_widgets::PanelVerticalPadding,
        toolbar_widgets::PanelHorizontalPadding, toolbar_widgets::PanelVerticalPadding);
    panelLayout->setSpacing(toolbar_widgets::PanelItemSpacing);
    panelLayout->setAlignment(Qt::AlignVCenter);

    m_xLabel = addValueLabel(tr("X coordinate"), Field::PositionX);
    setTranslationSource(m_xLabel, "X coordinate");
    m_yLabel = addValueLabel(tr("Y coordinate"), Field::PositionY);
    setTranslationSource(m_yLabel, "Y coordinate");
    auto* positionCommaLabel = addStaticLabel(QStringLiteral(","), QString(),
                                              QMargins(toolbar_widgets::SymbolHorizontalMargin, 0,
                                                       toolbar_widgets::SymbolHorizontalMargin, 0));
    auto* positionUnitLabel = addStaticLabel(QStringLiteral("px"), tr("Pixels"),
                                             QMargins(toolbar_widgets::UnitLeftMargin, 0, 0, 0));
    setTranslationSource(positionUnitLabel, "Pixels");
    panelLayout->addWidget(m_xLabel);
    panelLayout->addWidget(positionCommaLabel);
    panelLayout->addWidget(m_yLabel);
    panelLayout->addWidget(positionUnitLabel);
    m_positionWidgets << m_xLabel << positionCommaLabel << m_yLabel << positionUnitLabel;

    m_lockIconLabel = addIconLabel(tr("Lock selection aspect ratio"));
    setTranslationSource(m_lockIconLabel, "Lock selection aspect ratio");
    m_lockIconLabel->setCursor(Qt::PointingHandCursor);
    m_lockIconLabel->installEventFilter(this);
    panelLayout->addWidget(m_lockIconLabel);

    m_widthLabel = addValueLabel(tr("Width"), Field::Width);
    setTranslationSource(m_widthLabel, "Width");
    m_heightLabel = addValueLabel(tr("Height"), Field::Height);
    setTranslationSource(m_heightLabel, "Height");
    auto* sizeSeparatorLabel = addStaticLabel(QStringLiteral("x"), QString(),
                                              QMargins(toolbar_widgets::SymbolHorizontalMargin, 0,
                                                       toolbar_widgets::SymbolHorizontalMargin, 0));
    auto* sizeUnitLabel = addStaticLabel(QStringLiteral("px"), tr("Pixels"),
                                         QMargins(toolbar_widgets::UnitLeftMargin, 0, 0, 0));
    setTranslationSource(sizeUnitLabel, "Pixels");
    panelLayout->addWidget(m_widthLabel);
    panelLayout->addWidget(sizeSeparatorLabel);
    panelLayout->addWidget(m_heightLabel);
    panelLayout->addWidget(sizeUnitLabel);
    m_sizeWidgets << m_widthLabel << sizeSeparatorLabel << m_heightLabel << sizeUnitLabel;

    QWidget* selectionSettingsSeparator = addSeparator();
    panelLayout->addWidget(selectionSettingsSeparator);

    m_radiusLabel = addValueLabel(tr("Corner radius"), Field::Radius);
    setTranslationSource(m_radiusLabel, "Corner radius");
    auto* radiusUnitLabel = addStaticLabel(QStringLiteral("px"), tr("Pixels"),
                                           QMargins(toolbar_widgets::UnitLeftMargin, 0, 0, 0));
    setTranslationSource(radiusUnitLabel, "Pixels");
    panelLayout->addWidget(m_radiusLabel);
    panelLayout->addWidget(radiusUnitLabel);
    auto* radiusShadowSpacer = new QWidget(m_panel);
    radiusShadowSpacer->setFixedSize(toolbar_widgets::RadiusShadowSettingGap, 1);
    radiusShadowSpacer->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    radiusShadowSpacer->setProperty(kAlwaysMouseTransparentProperty, true);
    radiusShadowSpacer->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    panelLayout->addWidget(radiusShadowSpacer);

    m_shadowLabel = addValueLabel(tr("Shadow width"), Field::Shadow);
    setTranslationSource(m_shadowLabel, "Shadow width");
    auto* shadowUnitLabel = addStaticLabel(QStringLiteral("px"), tr("Pixels"),
                                           QMargins(toolbar_widgets::UnitLeftMargin, 0, 0, 0));
    setTranslationSource(shadowUnitLabel, "Pixels");
    panelLayout->addWidget(m_shadowLabel);
    panelLayout->addWidget(shadowUnitLabel);
    m_editingWidgets << m_lockIconLabel << selectionSettingsSeparator << m_radiusLabel
                     << radiusUnitLabel << radiusShadowSpacer << m_shadowLabel << shadowUnitLabel;

    rootLayout->addWidget(panel);

    updateLabels();
    updateIconPixmaps();
    updateDisplayMode();
    updateWindowSize();
}

void ScreenshotSelectionToolbarWidget::resetForNewCapture() {
    m_selection = QRect();
    m_aspectRatioLocked = false;
    m_displayMode = DisplayMode::Full;
    m_cornerRadius = 0;
    m_shadowWidth = 0;
    updateLabels();
    updateDisplayMode();
}

void ScreenshotSelectionToolbarWidget::prepareForDisplay() {
    updateLabels(true);
    updateIconPixmaps();
    updateDisplayMode();
    updateWindowSize();
}

void ScreenshotSelectionToolbarWidget::prewarm() {
    if (isVisible()) {
        return;
    }

    ensurePolished();
    if (m_panel != nullptr && m_panel->layout() != nullptr) {
        m_panel->layout()->activate();
    }
    if (layout() != nullptr) {
        layout()->activate();
    }
    prepareForDisplay();

    const qreal dpr = std::max<qreal>(1.0, devicePixelRatioF());
    QPixmap warmSurface(std::max(1, qCeil(width() * dpr)), std::max(1, qCeil(height() * dpr)));
    warmSurface.setDevicePixelRatio(dpr);
    warmSurface.fill(Qt::transparent);
    render(&warmSurface);

    hide();
    resetForNewCapture();
}

void ScreenshotSelectionToolbarWidget::setSelectionState(const QRect& selection,
                                                         bool aspectRatioLocked, int cornerRadius,
                                                         int shadowWidth, DisplayMode displayMode) {
    const QRect normalized = selection.normalized();
    const int clampedRadius = std::clamp(cornerRadius, 0, kScreenshotSelectionCornerRadiusMax);
    const int clampedShadowWidth = std::clamp(shadowWidth, 0, kScreenshotSelectionShadowWidthMax);
    const bool selectionChanged = m_selection != normalized;
    const bool aspectRatioChanged = m_aspectRatioLocked != aspectRatioLocked;
    const bool cornerRadiusChanged = m_cornerRadius != clampedRadius;
    const bool shadowWidthChanged = m_shadowWidth != clampedShadowWidth;
    const bool displayModeChanged = m_displayMode != displayMode;
    if (!selectionChanged && !aspectRatioChanged && !cornerRadiusChanged && !shadowWidthChanged &&
        !displayModeChanged) {
        return;
    }

    m_selection = normalized;
    m_aspectRatioLocked = aspectRatioLocked;
    m_cornerRadius = clampedRadius;
    m_shadowWidth = clampedShadowWidth;
    m_displayMode = displayMode;

    bool labelGeometryChanged = false;
    if (selectionChanged || cornerRadiusChanged || shadowWidthChanged) {
        labelGeometryChanged = updateLabels();
    }
    if (aspectRatioChanged) {
        updateLockIconPixmap();
    }
    if (displayModeChanged) {
        updateDisplayMode();
    }
    if (labelGeometryChanged || displayModeChanged) {
        updateWindowSize();
    }
}

QSize ScreenshotSelectionToolbarWidget::contentSizeHint() const {
    return m_panel != nullptr ? m_panel->size() : size();
}

bool ScreenshotSelectionToolbarWidget::containsInteractiveGlobalPoint(
    const QPoint& globalPosition) const {
    return isPointInInteractiveContent(mapFromGlobal(globalPosition));
}

void ScreenshotSelectionToolbarWidget::moveContentTo(const QPoint& position) {
    move(position - contentOffset());
}

bool ScreenshotSelectionToolbarWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_lockIconLabel && event != nullptr) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton && m_displayMode == DisplayMode::Full) {
                m_commands.toggleSelectionAspectRatioLockFromToolbar();
                mouseEvent->accept();
                return true;
            }
        }
    }

    if (event != nullptr && event->type() == QEvent::MouseButtonPress) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton && m_displayMode == DisplayMode::Full &&
            objectInsidePanel(watched, m_panel)) {
            m_commands.openSelectionResizeModalFromToolbar();
            mouseEvent->accept();
            return true;
        }
    }

    if (event != nullptr && event->type() == QEvent::Wheel) {
        Field field = Field::Width;
        if (m_displayMode == DisplayMode::Full && fieldForObject(watched, &field)) {
            auto* wheelEvent = static_cast<QWheelEvent*>(event);
            const int deltaY = wheelVerticalDelta(wheelEvent);
            if (deltaY != 0) {
                handleFieldWheel(field, deltaY);
                wheelEvent->accept();
                return true;
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

bool ScreenshotSelectionToolbarWidget::event(QEvent* event) {
    // Only the transparent shadow margin belongs to the toolbar surface. Child controls must
    // receive their normal Qt enter/leave events so they can maintain their own hover visuals.
    if (shouldForwardPointerEventToOverlayCanvas(this, event) &&
        forwardPointerEventToOverlayCanvas(event)) {
        return true;
    }

    return QWidget::event(event);
}

void ScreenshotSelectionToolbarWidget::changeEvent(QEvent* event) {
    if (event != nullptr && event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    QWidget::changeEvent(event);
}

void ScreenshotSelectionToolbarWidget::retranslateUi() {
    const auto updateLabel = [](QLabel* label) {
        if (label == nullptr) {
            return;
        }
        const QString source = label->property(kTranslationSourceProperty).toString();
        if (source.isEmpty()) {
            return;
        }
        const QByteArray sourceUtf8 = source.toUtf8();
        const QString translated = translateToolbarText(sourceUtf8.constData());
        label->setToolTip(translated);
        label->setAccessibleName(translated);
    };

    for (QLabel* label : findChildren<QLabel*>()) {
        updateLabel(label);
    }
    updateLabels(true);
    updateWindowSize();
}

void ScreenshotSelectionToolbarWidget::hideEvent(QHideEvent* event) {
    setToolbarHovered(false);
    QWidget::hideEvent(event);
}

void ScreenshotSelectionToolbarWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // The toolbar is pooled with its overlay. Paint the prepared state during
    // showEvent so a layered child surface cannot expose its previous frame
    // while an asynchronous update is still pending.
    repaint();
    scheduleToolbarHoverSync();
}

void ScreenshotSelectionToolbarWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (m_panel != nullptr) {
        toolbar_widgets::paintToolbarShadow(&painter, QRectF(m_panel->geometry()),
                                            m_toolbarHovered);
    }
}

QLabel* ScreenshotSelectionToolbarWidget::addValueLabel(const QString& tooltip, Field field) {
    auto* label = new SelectionToolbarValueLabel(m_panel);
    label->setToolTip(tooltip);
    label->setAccessibleName(tooltip);
    label->setProperty("selectionToolbarField", static_cast<int>(field));
    label->setCursor(cursorShapeForField(field));
    label->installEventFilter(this);

    QFont valueFont = label->font();
    valueFont.setFamily(QStringLiteral("Open Sans"));
    valueFont.setPixelSize(14);
    valueFont.setWeight(QFont::Normal);
    label->setFont(valueFont);
    return label;
}

QLabel* ScreenshotSelectionToolbarWidget::addStaticLabel(const QString& text,
                                                         const QString& tooltip,
                                                         const QMargins& margins) {
    auto* label = new QLabel(text, m_panel);
    label->setAlignment(Qt::AlignCenter);
    label->setFocusPolicy(Qt::NoFocus);
    label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    label->setFixedHeight(toolbar_widgets::PanelHeight - toolbar_widgets::PanelVerticalPadding * 2);
    label->setContentsMargins(margins);
    label->installEventFilter(this);
    if (!tooltip.isEmpty()) {
        label->setToolTip(tooltip);
        label->setAccessibleName(tooltip);
    }
    QFont textFont = label->font();
    textFont.setFamily(QStringLiteral("Open Sans"));
    textFont.setPixelSize(14);
    textFont.setWeight(QFont::Normal);
    label->setFont(textFont);
    label->setStyleSheet(QStringLiteral("QLabel { color: #ffffff; }"));
    return label;
}

QLabel* ScreenshotSelectionToolbarWidget::addIconLabel(const QString& tooltip) {
    auto* label = new SelectionToolbarValueLabel(m_panel);
    label->setFocusPolicy(Qt::NoFocus);
    label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    label->setToolTip(tooltip);
    label->setAccessibleName(tooltip);
    label->installEventFilter(this);
    return label;
}

QWidget* ScreenshotSelectionToolbarWidget::addSeparator() {
    auto* separator = new SelectionToolbarSeparator(m_panel);
    separator->installEventFilter(this);
    return separator;
}

void ScreenshotSelectionToolbarWidget::setToolbarHovered(bool hovered) {
    hovered = hovered && m_displayMode == DisplayMode::Full;
    if (m_toolbarHovered == hovered) {
        return;
    }

    m_toolbarHovered = hovered;
    m_commands.setSelectionToolbarHovered(hovered);
    refreshHoverVisuals();
    if (hovered) {
        m_commands.hideColorPickersForScreenshotUi();
    }
}

void ScreenshotSelectionToolbarWidget::scheduleToolbarHoverSync() {
    QTimer::singleShot(0, this, [this]() {
        if (isVisible()) {
            setToolbarHovered(m_panel != nullptr && m_panel->underMouse());
        }
    });
}

void ScreenshotSelectionToolbarWidget::refreshHoverVisuals() {
    update();
    if (m_panel == nullptr) {
        return;
    }

    m_panel->update();
}

bool ScreenshotSelectionToolbarWidget::fieldForObject(QObject* object, Field* outField) const {
    if (object == nullptr || outField == nullptr) {
        return false;
    }

    const QVariant rawField = object->property("selectionToolbarField");
    if (!rawField.isValid()) {
        return false;
    }

    bool ok = false;
    const int fieldValue = rawField.toInt(&ok);
    if (!ok) {
        return false;
    }

    *outField = static_cast<Field>(fieldValue);
    return true;
}

void ScreenshotSelectionToolbarWidget::handleFieldWheel(Field field, int deltaY) {
    const int direction = deltaY > 0 ? 1 : -1;
    switch (field) {
    case Field::PositionX:
        m_commands.adjustSelectionFromToolbar(direction, 0, direction, 0);
        break;
    case Field::PositionY:
        m_commands.adjustSelectionFromToolbar(0, direction, 0, direction);
        break;
    case Field::Width:
        m_commands.adjustSelectionFromToolbar(0, 0, direction, 0);
        break;
    case Field::Height:
        m_commands.adjustSelectionFromToolbar(0, 0, 0, direction);
        break;
    case Field::Radius:
        m_commands.setSelectionCornerRadiusFromToolbar(m_cornerRadius + direction);
        break;
    case Field::Shadow:
        m_commands.setSelectionShadowWidthFromToolbar(m_shadowWidth + direction);
        break;
    default:
        break;
    }
}

bool ScreenshotSelectionToolbarWidget::shouldForwardPointerEventToOverlayCanvas(
    QObject* watched, const QEvent* event) const {
    if (event == nullptr) {
        return false;
    }

    const QEvent::Type eventType = event->type();
    const bool mouseEvent = eventType == QEvent::MouseButtonPress ||
                            eventType == QEvent::MouseMove ||
                            eventType == QEvent::MouseButtonRelease;
    const bool rootHoverEvent =
        watched == this && (eventType == QEvent::Enter || eventType == QEvent::HoverEnter ||
                            eventType == QEvent::HoverMove);
    const bool pointerEvent = mouseEvent || rootHoverEvent || eventType == QEvent::Wheel;
    if (!pointerEvent) {
        return false;
    }
    if (m_displayMode == DisplayMode::SizeOnly) {
        return true;
    }
    if (m_displayMode != DisplayMode::Full) {
        return false;
    }

    const QPoint localPosition =
        mapFromGlobal(globalPositionForPointerEvent(watched, event).toPoint());
    return !isPointInInteractiveContent(localPosition);
}

bool ScreenshotSelectionToolbarWidget::forwardPointerEventToOverlayCanvas(QEvent* event) const {
    if (event == nullptr) {
        return false;
    }

    const QEvent::Type eventType = event->type();
    const bool mouseEvent = eventType == QEvent::MouseButtonPress ||
                            eventType == QEvent::MouseMove ||
                            eventType == QEvent::MouseButtonRelease;
    const bool hoverEvent = eventType == QEvent::Enter || eventType == QEvent::HoverEnter ||
                            eventType == QEvent::HoverMove;
    const bool wheelEvent = eventType == QEvent::Wheel;
    if (!mouseEvent && !hoverEvent && !wheelEvent) {
        return false;
    }

    ScreenshotOverlayWindow* overlay = overlayParentForObject(this);
    QWidget* canvas = overlay != nullptr ? static_cast<QWidget*>(overlay->canvas()) : nullptr;
    if (canvas == nullptr) {
        return false;
    }

    const QPointF globalPosition = globalPositionForPointerEvent(this, event);
    const QPointF canvasPosition = QPointF(canvas->mapFromGlobal(globalPosition.toPoint()));

    if (mouseEvent) {
        const auto* mouseEventData = static_cast<const QMouseEvent*>(event);
        QMouseEvent forwardedEvent(eventType, canvasPosition, canvasPosition, globalPosition,
                                   mouseEventData->button(), mouseEventData->buttons(),
                                   mouseEventData->modifiers());
        QCoreApplication::sendEvent(canvas, &forwardedEvent);
        return true;
    }

    if (hoverEvent) {
        QMouseEvent forwardedEvent(QEvent::MouseMove, canvasPosition, canvasPosition,
                                   globalPosition, Qt::NoButton, QApplication::mouseButtons(),
                                   QApplication::keyboardModifiers());
        QCoreApplication::sendEvent(canvas, &forwardedEvent);
        return true;
    }

    const auto* wheelEventData = static_cast<const QWheelEvent*>(event);
    QWheelEvent forwardedEvent(canvasPosition, globalPosition, wheelEventData->pixelDelta(),
                               wheelEventData->angleDelta(), wheelEventData->buttons(),
                               wheelEventData->modifiers(), wheelEventData->phase(),
                               wheelEventData->inverted());
    QCoreApplication::sendEvent(canvas, &forwardedEvent);
    return true;
}

bool ScreenshotSelectionToolbarWidget::isPointInInteractiveContent(
    const QPoint& localPosition) const {
    if (!rect().contains(localPosition)) {
        return false;
    }
    if (m_panel == nullptr) {
        return true;
    }

    return m_panel->geometry().contains(localPosition);
}

bool ScreenshotSelectionToolbarWidget::updateLabels(bool refreshGeometry) {
    bool geometryChanged = false;
    geometryChanged |= updateLabelText(m_xLabel, pxText(m_selection.left()), refreshGeometry);
    geometryChanged |= updateLabelText(m_yLabel, pxText(m_selection.top()), refreshGeometry);
    geometryChanged |= updateLabelText(m_widthLabel, pxText(m_selection.width()), refreshGeometry);
    geometryChanged |=
        updateLabelText(m_heightLabel, pxText(m_selection.height()), refreshGeometry);
    geometryChanged |= updateLabelText(m_radiusLabel, pxText(m_cornerRadius), refreshGeometry);
    geometryChanged |= updateLabelText(m_shadowLabel, pxText(m_shadowWidth), refreshGeometry);
    return geometryChanged;
}

void ScreenshotSelectionToolbarWidget::updateLockIconPixmap() {
    if (m_lockIconLabel != nullptr) {
        auto* valueLabel = static_cast<SelectionToolbarValueLabel*>(m_lockIconLabel);
        valueLabel->setIconOnlyPixmap(toolbar_widgets::renderToolbarIcon(
            m_lockIconLabel, custom_outlined_icons::SelectionLockAspect(),
            m_aspectRatioLocked ? toolbar_widgets::panelPrimaryColor()
                                : toolbar_widgets::panelTextColor()));
        valueLabel->setLockAspectRatioControl(true);
        valueLabel->setFixedSize(valueLabel->sizeHint());
    }
}

void ScreenshotSelectionToolbarWidget::updateIconPixmaps() {
    updateLockIconPixmap();

    if (m_radiusLabel != nullptr) {
        auto* valueLabel = static_cast<SelectionToolbarValueLabel*>(m_radiusLabel);
        valueLabel->setLeadingIcon(toolbar_widgets::renderToolbarIcon(
            m_radiusLabel, custom_outlined_icons::SelectionRadius(),
            toolbar_widgets::panelTextColor()));
        valueLabel->setFixedSize(valueLabel->sizeHint());
    }

    if (m_shadowLabel != nullptr) {
        auto* valueLabel = static_cast<SelectionToolbarValueLabel*>(m_shadowLabel);
        valueLabel->setLeadingIcon(toolbar_widgets::renderToolbarIcon(
            m_shadowLabel, custom_outlined_icons::SelectionShadow(),
            toolbar_widgets::panelTextColor()));
        valueLabel->setFixedSize(valueLabel->sizeHint());
    }
}

void ScreenshotSelectionToolbarWidget::updateDisplayMode() {
    const bool fullMode = m_displayMode == DisplayMode::Full;
    updateMouseEventTransparency();
    if (!fullMode) {
        setToolbarHovered(false);
    }

    for (QWidget* widget : m_positionWidgets) {
        if (widget != nullptr) {
            widget->setVisible(fullMode);
        }
    }
    for (QWidget* widget : m_sizeWidgets) {
        if (widget != nullptr) {
            widget->setVisible(true);
        }
    }
    for (QWidget* widget : m_editingWidgets) {
        if (widget != nullptr) {
            widget->setVisible(fullMode);
        }
    }
    // SizeOnly keeps dimensions visible as read-only click-through content, so they must not
    // advertise the directional cursor used by the editable controls in Full mode.
    if (m_widthLabel != nullptr) {
        m_widthLabel->setCursor(fullMode ? cursorShapeForField(Field::Width) : Qt::ArrowCursor);
    }
    if (m_heightLabel != nullptr) {
        m_heightLabel->setCursor(fullMode ? cursorShapeForField(Field::Height) : Qt::ArrowCursor);
    }
}

void ScreenshotSelectionToolbarWidget::updateMouseEventTransparency() {
    const bool transparent = m_displayMode == DisplayMode::SizeOnly;
    setMouseTransparentForWidget(this, transparent);
    setMouseTransparentForWidget(m_panel, transparent);

    const QList<QWidget*> childWidgets = findChildren<QWidget*>();
    for (QWidget* child : childWidgets) {
        if (child == m_panel) {
            continue;
        }
        setMouseTransparentForWidget(child, transparent);
    }
}

void ScreenshotSelectionToolbarWidget::updateWindowSize() {
    if (m_panel == nullptr) {
        adjustSize();
        return;
    }

    m_panel->ensurePolished();
    if (m_panel->layout() != nullptr) {
        m_panel->layout()->activate();
    }

    const QSize panelSize = m_panel->sizeHint().expandedTo(QSize(1, toolbar_widgets::PanelHeight));
    m_panel->setFixedSize(panelSize);
    setFixedSize(panelSize +
                 QSize(toolbar_widgets::ShadowMargin * 2, toolbar_widgets::ShadowMargin * 2));
}

QPoint ScreenshotSelectionToolbarWidget::contentOffset() const {
    return QPoint(toolbar_widgets::ShadowMargin, toolbar_widgets::ShadowMargin);
}
