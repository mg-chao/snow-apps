#include "snow_shot/presentation/screenshotselectiontoolbarwidget.h"

#include "screenshotselectiontoolbarwidgets.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/screenshotselectionlimits.h"
#include "snow_shot/presentation/screenshottoolbarcommands.h"
#include "widgets/select.h"

#include <QCoreApplication>
#include <QApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QMargins>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPoint>
#include <QRegion>
#include <QSizePolicy>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QVariant>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <array>

namespace {
namespace custom_outlined_icons = snow_shot::presentation::icons::custom::outlined;
namespace toolbar_widgets = screenshot_selection_toolbar;
using snow_shot::presentation::kScreenshotSelectionCornerRadiusMax;
using snow_shot::presentation::kScreenshotSelectionShadowWidthMax;

constexpr auto kAlwaysMouseTransparentProperty = "selectionToolbarAlwaysMouseTransparent";
constexpr auto kTranslationSourceProperty = "selectionToolbarTranslationSource";
constexpr int kAspectRatioSelectGap = 8;
constexpr std::array kAspectRatioPresets = {
    ScreenshotSelectionAspectRatioPreset::Free,
    ScreenshotSelectionAspectRatioPreset::Square,
    ScreenshotSelectionAspectRatioPreset::Landscape3x2,
    ScreenshotSelectionAspectRatioPreset::Landscape4x3,
    ScreenshotSelectionAspectRatioPreset::Landscape16x9,
    ScreenshotSelectionAspectRatioPreset::Portrait2x3,
    ScreenshotSelectionAspectRatioPreset::Portrait3x4,
    ScreenshotSelectionAspectRatioPreset::Portrait9x16,
};
constexpr std::array kAspectRatioLabelSources = {
    QT_TRANSLATE_NOOP("ScreenshotSelectionToolbarWidget", "Free"),
    QT_TRANSLATE_NOOP("ScreenshotSelectionToolbarWidget", "1:1"),
    QT_TRANSLATE_NOOP("ScreenshotSelectionToolbarWidget", "3:2"),
    QT_TRANSLATE_NOOP("ScreenshotSelectionToolbarWidget", "4:3"),
    QT_TRANSLATE_NOOP("ScreenshotSelectionToolbarWidget", "16:9"),
    QT_TRANSLATE_NOOP("ScreenshotSelectionToolbarWidget", "2:3"),
    QT_TRANSLATE_NOOP("ScreenshotSelectionToolbarWidget", "3:4"),
    QT_TRANSLATE_NOOP("ScreenshotSelectionToolbarWidget", "9:16"),
};

void setTranslationSource(QLabel* label, const char* source) {
    if (label != nullptr && source != nullptr && source[0] != '\0') {
        label->setProperty(kTranslationSourceProperty, QString::fromUtf8(source));
    }
}

QString translateToolbarText(const char* source) {
    return QCoreApplication::translate("ScreenshotSelectionToolbarWidget", source);
}

QString valueText(int value) {
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
    setFocusPolicy(Qt::NoFocus);
    setAutoFillBackground(false);

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
    auto* positionUnitLabel =
        addStaticLabel(tr("px"), tr("Pixels"), QMargins(toolbar_widgets::UnitLeftMargin, 0, 0, 0));
    m_positionUnitLabel = positionUnitLabel;
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
    auto* sizeUnitLabel =
        addStaticLabel(tr("px"), tr("Pixels"), QMargins(toolbar_widgets::UnitLeftMargin, 0, 0, 0));
    m_sizeUnitLabel = sizeUnitLabel;
    panelLayout->addWidget(m_widthLabel);
    panelLayout->addWidget(sizeSeparatorLabel);
    panelLayout->addWidget(m_heightLabel);
    panelLayout->addWidget(sizeUnitLabel);
    m_sizeWidgets << m_widthLabel << sizeSeparatorLabel << m_heightLabel << sizeUnitLabel;

    auto* aspectRatioGap = new QWidget(m_panel);
    aspectRatioGap->setFixedSize(kAspectRatioSelectGap, 1);
    aspectRatioGap->setProperty(kAlwaysMouseTransparentProperty, true);
    aspectRatioGap->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    panelLayout->addWidget(aspectRatioGap);

    m_aspectRatioSelect = new adqt::widgets::AdSelect(m_panel);
    m_aspectRatioSelect->setObjectName(QStringLiteral("screenshotSelectionAspectRatioSelect"));
    m_aspectRatioSelect->setFocusPolicy(Qt::NoFocus);
    m_aspectRatioSelect->setMode(adqt::widgets::AdSelect::Mode::Single);
    m_aspectRatioSelect->setSizeAdjustPolicy(
        adqt::widgets::AdSelect::SizeAdjustPolicy::AdjustToCurrentText);
    m_aspectRatioSelect->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_aspectRatioSelect->setControlSize(adqt::widgets::AdSelect::ControlSize::Small);
    m_aspectRatioSelect->setVariant(adqt::widgets::AdSelect::Variant::Borderless);
    m_aspectRatioSelect->setSearchEnabled(false);
    m_aspectRatioSelect->setAllowClear(false);
    m_aspectRatioSelect->setPopupLayerMode(adqt::widgets::AdSelect::PopupLayerMode::QtTool);
    m_aspectRatioSelect->setPopupMatchSelectWidth(false);
    m_aspectRatioSelect->setPopupWidth(112);
    m_aspectRatioSelect->setPlacement(adqt::widgets::AdSelect::Placement::BottomLeft);
    QFont ratioFont = m_aspectRatioSelect->font();
    ratioFont.setPixelSize(14);
    ratioFont.setWeight(QFont::Normal);
    m_aspectRatioSelect->setFont(ratioFont);
    adqt::widgets::AdSelect::ComponentTokens ratioTokens;
    ratioTokens.metrics.controlHeight =
        toolbar_widgets::PanelHeight - toolbar_widgets::PanelVerticalPadding * 2;
    ratioTokens.metrics.borderRadius = toolbar_widgets::PanelRadius;
    ratioTokens.metrics.borderWidth = 0;
    ratioTokens.metrics.horizontalPadding = 4;
    ratioTokens.metrics.iconSize = 12;
    ratioTokens.metrics.selectorFontSize = 14;
    ratioTokens.metrics.optionFontSize = 14;
    ratioTokens.metrics.optionHeight = 28;
    ratioTokens.metrics.popupMaxHeight = 240;
    ratioTokens.colors.selectorText = toolbar_widgets::panelTextColor();
    ratioTokens.colors.placeholderText = toolbar_widgets::panelTextColor();
    ratioTokens.colors.suffix = toolbar_widgets::panelTextColor();
    ratioTokens.colors.disabledText = toolbar_widgets::panelTextColor();
    m_aspectRatioSelect->setComponentTokens(ratioTokens);
    m_aspectRatioSelect->setFixedHeight(*ratioTokens.metrics.controlHeight);
    m_aspectRatioSelect->installEventFilter(this);
    updateAspectRatioOptions();
    updateAspectRatioSelectStyle();
    panelLayout->addWidget(m_aspectRatioSelect);
    m_editingWidgets << aspectRatioGap << m_aspectRatioSelect;
    connect(m_aspectRatioSelect, &adqt::widgets::AdSelect::selected, this,
            [this](const QVariant& value, const QString&) {
                if (pointerInteractionEnabled() && m_selectionResizable) {
                    m_commands.setSelectionAspectRatioPresetFromToolbar(
                        screenshotSelectionAspectRatioPresetFromId(value.toString()));
                }
                const QSignalBlocker blocker(m_aspectRatioSelect);
                m_aspectRatioSelect->setCurrentValue(
                    screenshotSelectionAspectRatioPresetId(m_aspectRatioPreset));
            });
    connect(m_aspectRatioSelect, &adqt::widgets::AdSelect::popupOpening, this, [this]() {
        prepareAspectRatioPopupInput();
        m_focusBeforeAspectRatioPopup = QApplication::focusWidget();
        m_commands.hideColorPickersForScreenshotUi();
    });
    connect(m_aspectRatioSelect, &adqt::widgets::AdSelect::popupVisibleChanged, this,
            [this](bool visible) {
                m_commands.setSelectionToolbarPopupVisible(visible);
                updateAspectRatioSelectStyle();
                const auto* panel = static_cast<SelectionToolbarPanel*>(m_panel);
                setToolbarHovered(visible || panel->pointerHovered());
                if (!visible) {
                    scheduleToolbarHoverSync();
                    const QPointer<QWidget> previousFocus = m_focusBeforeAspectRatioPopup;
                    m_focusBeforeAspectRatioPopup = nullptr;
                    QTimer::singleShot(0, this, [this, previousFocus]() {
                        if (isVisible() && pointerInteractionEnabled() &&
                            previousFocus != nullptr && previousFocus->isVisible() &&
                            window()->isActiveWindow() && !m_aspectRatioSelect->popupVisible()) {
                            QWidget* currentFocus = QApplication::focusWidget();
                            const bool popupOwnsFocus =
                                currentFocus != nullptr && m_aspectRatioPopupView != nullptr &&
                                m_aspectRatioPopupView->window()->isAncestorOf(currentFocus);
                            if (currentFocus != nullptr && currentFocus != previousFocus &&
                                !popupOwnsFocus) {
                                return;
                            }
                            previousFocus->setFocus(Qt::PopupFocusReason);
                        }
                    });
                }
            });

    QWidget* selectionSettingsSeparator = addSeparator();
    panelLayout->addWidget(selectionSettingsSeparator);

    m_radiusLabel = addValueLabel(tr("Corner radius"), Field::Radius);
    setTranslationSource(m_radiusLabel, "Corner radius");
    auto* radiusUnitLabel =
        addStaticLabel(tr("px"), tr("Pixels"), QMargins(toolbar_widgets::UnitLeftMargin, 0, 0, 0));
    m_canvasUnitLabels << radiusUnitLabel;
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
    auto* shadowUnitLabel =
        addStaticLabel(tr("px"), tr("Pixels"), QMargins(toolbar_widgets::UnitLeftMargin, 0, 0, 0));
    m_canvasUnitLabels << shadowUnitLabel;
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

ScreenshotSelectionToolbarWidget::~ScreenshotSelectionToolbarWidget() {
    closeAspectRatioPopup();
    setToolbarHovered(false);
}

void ScreenshotSelectionToolbarWidget::resetForNewCapture() {
    closeAspectRatioPopup();
    m_selection = QRect();
    m_displayValues = {};
    m_aspectRatioLocked = false;
    m_aspectRatioPreset = ScreenshotSelectionAspectRatioPreset::Free;
    {
        const QSignalBlocker blocker(m_aspectRatioSelect);
        m_aspectRatioSelect->setCurrentValue(
            screenshotSelectionAspectRatioPresetId(m_aspectRatioPreset));
    }
    m_displayMode = DisplayMode::Full;
    m_pointerInteractionEnabled = true;
    m_cornerRadius = 0;
    m_shadowWidth = 0;
    m_canvasUsesPoints = false;
    updateLabels();
    updateLockIconPixmap();
    updateDisplayMode();
    updateWindowSize();
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

void ScreenshotSelectionToolbarWidget::setSelectionState(
    const QRect& selection, bool aspectRatioLocked, int cornerRadius, int shadowWidth,
    DisplayMode displayMode, bool canvasUsesPoints,
    std::optional<ScreenshotSelectionDisplayValues> displayValues,
    ScreenshotSelectionAspectRatioPreset aspectRatioPreset) {
    const QRect normalized = selection.normalized();
    const int clampedRadius = std::clamp(cornerRadius, 0, kScreenshotSelectionCornerRadiusMax);
    const int clampedShadowWidth = std::clamp(shadowWidth, 0, kScreenshotSelectionShadowWidthMax);
    const bool selectionChanged = m_selection != normalized;
    const auto values = displayValues.value_or(ScreenshotSelectionDisplayValues{
        QPointF(normalized.topLeft()), QSizeF(normalized.size()),
        canvasUsesPoints ? ScreenshotSelectionDisplayUnit::LogicalPixels
                         : ScreenshotSelectionDisplayUnit::PhysicalPixels,
        canvasUsesPoints});
    const bool unitsChanged = m_canvasUsesPoints != canvasUsesPoints || m_displayValues != values;
    const bool aspectRatioChanged = m_aspectRatioLocked != aspectRatioLocked;
    const bool aspectRatioPresetChanged = m_aspectRatioPreset != aspectRatioPreset;
    const bool cornerRadiusChanged = m_cornerRadius != clampedRadius;
    const bool shadowWidthChanged = m_shadowWidth != clampedShadowWidth;
    const bool displayModeChanged = m_displayMode != displayMode;
    if (!selectionChanged && !unitsChanged && !aspectRatioChanged && !cornerRadiusChanged &&
        !shadowWidthChanged && !displayModeChanged && !aspectRatioPresetChanged) {
        return;
    }

    m_selection = normalized;
    m_displayValues = values;
    m_canvasUsesPoints = canvasUsesPoints;
    m_aspectRatioLocked = aspectRatioLocked;
    m_aspectRatioPreset = aspectRatioPreset;
    m_cornerRadius = clampedRadius;
    m_shadowWidth = clampedShadowWidth;
    m_displayMode = displayMode;

    bool labelGeometryChanged = false;
    if (selectionChanged || unitsChanged || cornerRadiusChanged || shadowWidthChanged ||
        displayModeChanged) {
        labelGeometryChanged = updateLabels();
    }
    if (aspectRatioChanged) {
        updateLockIconPixmap();
    }
    if (aspectRatioPresetChanged) {
        const QSignalBlocker blocker(m_aspectRatioSelect);
        m_aspectRatioSelect->setCurrentValue(
            screenshotSelectionAspectRatioPresetId(m_aspectRatioPreset));
    }
    if (displayModeChanged) {
        updateDisplayMode();
    }
    if (labelGeometryChanged || displayModeChanged || aspectRatioPresetChanged) {
        updateWindowSize();
    }
}

QSize ScreenshotSelectionToolbarWidget::contentSizeHint() const {
    return m_panel != nullptr ? m_panel->size() : size();
}

bool ScreenshotSelectionToolbarWidget::containsInteractiveGlobalPoint(
    const QPoint& globalPosition) const {
    if (!pointerInteractionEnabled()) {
        return false;
    }
    if (m_aspectRatioSelect->popupVisible()) {
        const auto* view = m_aspectRatioSelect->view();
        const QWidget* popup = view != nullptr ? view->window() : nullptr;
        if (popup != nullptr && popup->isVisible() &&
            popup->rect().contains(popup->mapFromGlobal(globalPosition))) {
            return true;
        }
    }
    return isPointInInteractiveContent(mapFromGlobal(globalPosition));
}

void ScreenshotSelectionToolbarWidget::moveContentTo(const QPoint& position) {
    move(position - contentOffset());
}

bool ScreenshotSelectionToolbarWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_aspectRatioPopupView && event != nullptr &&
        event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        const bool confirmsOption =
            keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter;
        if (confirmsOption && pointerInteractionEnabled() && m_selectionResizable &&
            m_aspectRatioSelect->currentValue() == QStringLiteral("free") &&
            m_aspectRatioPopupView->currentIndex().row() == 0) {
            // Explicit Free also clears a remembered preset when the active value is already Free.
            m_commands.setSelectionAspectRatioPresetFromToolbar(
                ScreenshotSelectionAspectRatioPreset::Free);
            m_aspectRatioSelect->hidePopup();
            keyEvent->accept();
            return true;
        }
    }
    if (watched == m_aspectRatioSelect) {
        if (event != nullptr &&
            (event->type() == QEvent::Enter || event->type() == QEvent::Leave)) {
            m_aspectRatioSelectHovered =
                event->type() == QEvent::Enter && pointerInteractionEnabled();
            updateAspectRatioSelectStyle();
        }
        // Select owns presses and keyboard input. The remaining panel controls open the resize
        // modal.
        return QWidget::eventFilter(watched, event);
    }
    if (!pointerInteractionEnabled()) {
        return QWidget::eventFilter(watched, event);
    }
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

void ScreenshotSelectionToolbarWidget::changeEvent(QEvent* event) {
    if (event != nullptr && event->type() == QEvent::LanguageChange) {
        retranslateUi();
        m_radiusLabel->setToolTip(m_cornerRadiusApplicable
                                      ? tr("Corner radius")
                                      : tr("Corner radius is unavailable for custom regions"));
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
    updateAspectRatioOptions();
    updateWindowSize();
}

void ScreenshotSelectionToolbarWidget::hideEvent(QHideEvent* event) {
    closeAspectRatioPopup();
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
    hovered = hovered || (m_aspectRatioSelect != nullptr && m_aspectRatioSelect->popupVisible());
    hovered = hovered && pointerInteractionEnabled();
    if (m_toolbarHovered == hovered) {
        return;
    }

    m_toolbarHovered = hovered;
    updateInputRegion();
    m_commands.setSelectionToolbarHovered(hovered);
    update();
    if (hovered) {
        m_commands.hideColorPickersForScreenshotUi();
    }
}

void ScreenshotSelectionToolbarWidget::scheduleToolbarHoverSync() {
    QTimer::singleShot(0, this, [this]() {
        if (isVisible()) {
            if (auto* panel = qobject_cast<SelectionToolbarPanel*>(m_panel)) {
                panel->synchronizePointerHover();
                setToolbarHovered(panel->pointerHovered());
            } else {
                setToolbarHovered(false);
            }
        }
    });
}

void ScreenshotSelectionToolbarWidget::updateAspectRatioOptions() {
    const QSignalBlocker blocker(m_aspectRatioSelect);
    QVector<adqt::widgets::AdSelect::Option> options;
    options.reserve(static_cast<qsizetype>(kAspectRatioPresets.size()));
    int widestLabel = 0;
    const QFontMetrics metrics(m_aspectRatioSelect->font());
    for (std::size_t index = 0; index < kAspectRatioPresets.size(); ++index) {
        const auto preset = kAspectRatioPresets[index];
        const QString id = screenshotSelectionAspectRatioPresetId(preset);
        const QString label = translateToolbarText(kAspectRatioLabelSources[index]);
        options.push_back({id, label});
        widestLabel = std::max(widestLabel, metrics.horizontalAdvance(label));
    }
    m_aspectRatioSelect->setOptions(options);
    m_aspectRatioSelect->setCurrentValue(
        screenshotSelectionAspectRatioPresetId(m_aspectRatioPreset));
    m_aspectRatioSelect->setToolTip(tr("Selection aspect ratio"));
    m_aspectRatioSelect->setAccessibleName(tr("Selection aspect ratio"));
    m_aspectRatioSelect->setPopupWidth(std::max(112, widestLabel + 40));
}

void ScreenshotSelectionToolbarWidget::prepareAspectRatioPopupInput() {
    m_aspectRatioValueBeforePopupActivation = m_aspectRatioSelect->currentValue().toString();
    auto* view = m_aspectRatioSelect->view();
    if (view == nullptr || m_aspectRatioPopupView == view) {
        return;
    }
    m_aspectRatioPopupView = view;
    view->installEventFilter(this);
    connect(view, &QListView::pressed, this, [this](const QModelIndex&) {
        m_aspectRatioValueBeforePopupActivation = m_aspectRatioSelect->currentValue().toString();
    });
    connect(view, &QListView::clicked, this, [this](const QModelIndex& index) {
        if (index.row() == 0 && pointerInteractionEnabled() && m_selectionResizable &&
            m_aspectRatioValueBeforePopupActivation == QStringLiteral("free") &&
            m_aspectRatioSelect->currentValue() == QStringLiteral("free")) {
            m_commands.setSelectionAspectRatioPresetFromToolbar(
                ScreenshotSelectionAspectRatioPreset::Free);
        }
    });
}

void ScreenshotSelectionToolbarWidget::updateAspectRatioSelectStyle() {
    if (m_aspectRatioSelect == nullptr) {
        return;
    }
    const bool highlighted = !m_aspectRatioSelect->disabled() &&
                             (m_aspectRatioSelectHovered || m_aspectRatioSelect->popupVisible());
    if (m_aspectRatioSelectHighlight == highlighted) {
        return;
    }
    m_aspectRatioSelectHighlight = highlighted;
    adqt::widgets::AdSelect::SemanticStyles styles;
    styles.root.backgroundColor = Qt::transparent;
    styles.selector.backgroundColor =
        highlighted ? QColor(22, 119, 255, 107) : QColor(Qt::transparent);
    styles.selector.textColor = toolbar_widgets::panelTextColor();
    styles.selector.borderColor = Qt::transparent;
    styles.suffix.textColor = toolbar_widgets::panelTextColor();
    m_aspectRatioSelect->setSemanticStyles(styles);
}

void ScreenshotSelectionToolbarWidget::updateAspectRatioSelectAvailability() {
    if (m_aspectRatioSelect == nullptr) {
        return;
    }
    const bool enabled = pointerInteractionEnabled() && m_selectionResizable;
    if (!enabled) {
        closeAspectRatioPopup();
    }
    m_aspectRatioSelect->setDisabled(!enabled);
    updateAspectRatioSelectStyle();
}

void ScreenshotSelectionToolbarWidget::closeAspectRatioPopup() {
    if (m_aspectRatioSelect == nullptr) {
        return;
    }
    m_aspectRatioSelectHovered = false;
    m_aspectRatioSelect->hidePopup();
    updateAspectRatioSelectStyle();
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

void ScreenshotSelectionToolbarWidget::setPointerInteractionEnabled(bool enabled) {
    if (m_pointerInteractionEnabled == enabled) {
        return;
    }
    m_pointerInteractionEnabled = enabled;
    updateAspectRatioSelectAvailability();
    // A selection drag owns the pointer even when this moving toolbar passes
    // underneath it. Clear preview hover before the next selection frame.
    updateMouseEventTransparency();
    if (!pointerInteractionEnabled()) {
        setToolbarHovered(false);
    } else if (isVisible()) {
        scheduleToolbarHoverSync();
    }
}

void ScreenshotSelectionToolbarWidget::setSelectionResizable(bool enabled) {
    if (m_selectionResizable == enabled) {
        return;
    }
    m_selectionResizable = enabled;
    updateAspectRatioSelectAvailability();
    for (auto* label : {m_widthLabel, m_heightLabel, m_lockIconLabel}) {
        label->setCursor(enabled ? Qt::SizeHorCursor : Qt::ArrowCursor);
    }
}

void ScreenshotSelectionToolbarWidget::handleFieldWheel(Field field, int deltaY) {
    if (field == Field::Radius && !m_cornerRadiusApplicable)
        return;
    if (!m_selectionResizable && (field == Field::Width || field == Field::Height))
        return;
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

void ScreenshotSelectionToolbarWidget::updateInputRegion() {
    // The margin around the panel exists only to paint the hover glow. Align the native
    // input surface with the interactive panel (plus the visible glow while hovered) so
    // pointer events over the margin fall through to the overlay canvas below. That keeps
    // the selection border running under the toolbar directly grabbable with native hover
    // cursors, real grabs, and double-clicks instead of synthesized forwarded events.
    if (m_panel == nullptr) {
        setMask(QRegion());
        return;
    }
    if (layout() != nullptr) {
        layout()->activate();
    }
    setMask(toolbar_widgets::interactiveInputRegion(m_panel->geometry(), m_toolbarHovered));
}

bool ScreenshotSelectionToolbarWidget::updateLabels(bool refreshGeometry) {
    bool geometryChanged = false;
    const auto updateUnit = [&](QLabel* label, ScreenshotSelectionDisplayUnit unit) {
        geometryChanged |=
            updateLabelText(label, screenshotSelectionDisplayUnitText(unit), refreshGeometry);
        const QString description = screenshotSelectionDisplayUnitDescription(unit);
        if (label->toolTip() != description) {
            label->setToolTip(description);
        }
        if (label->accessibleName() != description) {
            label->setAccessibleName(description);
        }
    };
    for (QLabel* label : m_canvasUnitLabels) {
        updateUnit(label, m_canvasUsesPoints ? ScreenshotSelectionDisplayUnit::LogicalPixels
                                             : ScreenshotSelectionDisplayUnit::PhysicalPixels);
    }
    for (auto* label : {m_positionUnitLabel, m_sizeUnitLabel}) {
        updateUnit(label, m_displayValues.unit);
    }
    geometryChanged |= updateLabelText(
        m_xLabel, screenshotSelectionDisplayValue(m_displayValues.position.x()), refreshGeometry);
    geometryChanged |= updateLabelText(
        m_yLabel, screenshotSelectionDisplayValue(m_displayValues.position.y()), refreshGeometry);
    geometryChanged |=
        updateLabelText(m_widthLabel, screenshotSelectionDisplayValue(m_displayValues.size.width()),
                        refreshGeometry);
    geometryChanged |= updateLabelText(
        m_heightLabel, screenshotSelectionDisplayValue(m_displayValues.size.height()),
        refreshGeometry);
    geometryChanged |= updateLabelText(m_radiusLabel, valueText(m_cornerRadius), refreshGeometry);
    geometryChanged |= updateLabelText(m_shadowLabel, valueText(m_shadowWidth), refreshGeometry);
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
    updateAspectRatioSelectAvailability();
    updateMouseEventTransparency();
    updateInputRegion();
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

bool ScreenshotSelectionToolbarWidget::pointerInteractionEnabled() const {
    return m_pointerInteractionEnabled && m_displayMode == DisplayMode::Full;
}

void ScreenshotSelectionToolbarWidget::updateMouseEventTransparency() {
    const bool transparent = !pointerInteractionEnabled();
    if (transparent) {
        // WA_TransparentForMouseEvents and the input mask only redirect Qt-internal
        // hit testing for alien widgets. A native child HWND always wins OS-level
        // hit testing, which would leave an arrow cursor over the toolbar and starve
        // the overlay canvas of the mouse moves that drive the color picker. Siblings
        // can be forced native at any time (Qt nativizes every sibling of an embedded
        // native window-type child, e.g. the floating tool palette), so shed that
        // surface whenever the click-through display mode is applied.
        releaseNativeInputSurface();
    }
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

void ScreenshotSelectionToolbarWidget::releaseNativeInputSurface() {
    if (!testAttribute(Qt::WA_NativeWindow) && internalWinId() == 0) {
        return;
    }

    const bool wasVisible = isVisible();
    if (wasVisible) {
        hide();
    }
    setAttribute(Qt::WA_NativeWindow, false);
    destroy(true, false);
    if (wasVisible) {
        show();
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
    updateInputRegion();
}

QPoint ScreenshotSelectionToolbarWidget::contentOffset() const {
    return QPoint(toolbar_widgets::ShadowMargin, toolbar_widgets::ShadowMargin);
}

void ScreenshotSelectionToolbarWidget::setCornerRadiusApplicable(bool enabled) {
    if (m_cornerRadiusApplicable == enabled) {
        return;
    }
    m_cornerRadiusApplicable = enabled;
    m_radiusLabel->setEnabled(enabled);
    m_radiusLabel->setToolTip(enabled ? tr("Corner radius")
                                      : tr("Corner radius is unavailable for custom regions"));
}
