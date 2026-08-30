#include "tooltip.h"

#include "detail/overlay_accessibility.h"
#include "detail/overlay_popup_controller.h"
#include "detail/overlay_popup_surface.h"
#include "detail/qt_tooltip_bridge.h"
#include "theme/theme.h"
#include "tooltip_style.h"

#include <QAccessible>
#include <QApplication>
#include <QEvent>
#include <QFontMetricsF>
#include <QLabel>
#include <QMetaObject>
#include <QPalette>
#include <QSet>
#include <QStyle>
#include <QStringList>
#include <QTextDocument>
#include <QVariant>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>
#endif

namespace adqt::widgets {

class AdTooltipPrivate;

namespace {

#if defined(Q_OS_WIN) || defined(_WIN32)
HWND hwndFromWId(WId id) {
  // Qt transports the native HWND through its integer-valued WId type.
  return reinterpret_cast<HWND>(id);  // NOLINT(performance-no-int-to-ptr)
}
#endif

bool isVisualRefreshEvent(QEvent::Type type) {
  return type == QEvent::FontChange || type == QEvent::PaletteChange ||
         type == QEvent::StyleChange || type == QEvent::ApplicationFontChange ||
         type == QEvent::ApplicationPaletteChange;
}

int measureSingleLineTextWidth(const QFontMetricsF& metrics, const QString& line) {
  if (line.isEmpty()) {
    return 0;
  }

  const qreal advance = metrics.horizontalAdvance(line);
  const qreal boundsWidth = metrics.boundingRect(line).width();
  return std::max(0, static_cast<int>(std::ceil(std::max(advance, boundsWidth))));
}

int measureTooltipNaturalTextWidth(const QString& text, const QFont& font) {
  if (text.isEmpty()) {
    return 0;
  }

  if (Qt::mightBeRichText(text)) {
    QTextDocument document;
    document.setDocumentMargin(0.0);
    document.setDefaultFont(font);
    document.setHtml(text);
    return std::max(0, static_cast<int>(std::ceil(document.idealWidth())));
  }

  const QFontMetricsF metrics(font);
  const QStringList lines = text.split(QChar::LineFeed);
  int maxWidth = 0;
  for (QString line : lines) {
    line.remove(QChar::CarriageReturn);
    maxWidth = std::max(maxWidth, measureSingleLineTextWidth(metrics, line));
  }
  return maxWidth;
}

QString accessibleTextForTooltip(const QString& text) {
  if (text.isEmpty()) {
    return QString();
  }
  if (!Qt::mightBeRichText(text)) {
    return text.trimmed();
  }

  QTextDocument document;
  document.setDocumentMargin(0.0);
  document.setHtml(text);
  return document.toPlainText().trimmed();
}

void mergeSemanticSlotStyle(AdTooltip::SemanticSlotStyle* target,
                            const AdTooltip::SemanticSlotStyle& extra) {
  if (!target) {
    return;
  }
  if (extra.textColor.has_value()) {
    target->textColor = extra.textColor;
  }
  if (extra.backgroundColor.has_value()) {
    target->backgroundColor = extra.backgroundColor;
  }
  if (extra.borderColor.has_value()) {
    target->borderColor = extra.borderColor;
  }
}

AdTooltip::SemanticStyles mergeSemanticStyles(const AdTooltip::SemanticStyles& base,
                                              const AdTooltip::SemanticStyles& extra) {
  AdTooltip::SemanticStyles merged = base;
  mergeSemanticSlotStyle(&merged.surface, extra.surface);
  mergeSemanticSlotStyle(&merged.content, extra.content);
  mergeSemanticSlotStyle(&merged.arrow, extra.arrow);
  return merged;
}

detail::OverlayPopupPlacement toOverlayPlacement(AdTooltip::Placement value) {
  switch (value) {
    case AdTooltip::Placement::Top:
      return detail::OverlayPopupPlacement::Top;
    case AdTooltip::Placement::TopLeft:
      return detail::OverlayPopupPlacement::TopLeft;
    case AdTooltip::Placement::TopRight:
      return detail::OverlayPopupPlacement::TopRight;
    case AdTooltip::Placement::Bottom:
      return detail::OverlayPopupPlacement::Bottom;
    case AdTooltip::Placement::BottomLeft:
      return detail::OverlayPopupPlacement::BottomLeft;
    case AdTooltip::Placement::BottomRight:
      return detail::OverlayPopupPlacement::BottomRight;
    case AdTooltip::Placement::Left:
      return detail::OverlayPopupPlacement::Left;
    case AdTooltip::Placement::LeftTop:
      return detail::OverlayPopupPlacement::LeftTop;
    case AdTooltip::Placement::LeftBottom:
      return detail::OverlayPopupPlacement::LeftBottom;
    case AdTooltip::Placement::Right:
      return detail::OverlayPopupPlacement::Right;
    case AdTooltip::Placement::RightTop:
      return detail::OverlayPopupPlacement::RightTop;
    case AdTooltip::Placement::RightBottom:
      return detail::OverlayPopupPlacement::RightBottom;
  }
  return detail::OverlayPopupPlacement::Top;
}

detail::OverlayPopupController::Triggers toOverlayTriggers(AdTooltip::Triggers value) {
  detail::OverlayPopupController::Triggers mapped;
  if (value.testFlag(AdTooltip::Trigger::Hover)) {
    mapped |= detail::OverlayPopupController::Trigger::Hover;
  }
  if (value.testFlag(AdTooltip::Trigger::Focus)) {
    mapped |= detail::OverlayPopupController::Trigger::Focus;
  }
  if (value.testFlag(AdTooltip::Trigger::Click)) {
    mapped |= detail::OverlayPopupController::Trigger::Click;
  }
  if (value.testFlag(AdTooltip::Trigger::ContextMenu)) {
    mapped |= detail::OverlayPopupController::Trigger::ContextMenu;
  }
  return mapped;
}

detail::OverlayPopupController::VisibilityMode toControllerActivationMode(
    AdTooltip::ActivationMode value) {
  return value == AdTooltip::ActivationMode::Manual
             ? detail::OverlayPopupController::VisibilityMode::External
             : detail::OverlayPopupController::VisibilityMode::Automatic;
}

int arrowOffsetHorizontalForRadius(int borderRadius) {
  if (borderRadius > 12) {
    return borderRadius + 2;
  }
  return 12;
}

int arrowOffsetVerticalForRadius(int borderRadius) {
  return std::min(8, arrowOffsetHorizontalForRadius(borderRadius));
}

int tooltipWakeUpDelay(const QWidget* widget) {
  QStyle* style = widget ? widget->style() : QApplication::style();
  if (!style) {
    return 700;
  }
  return std::max(0, style->styleHint(QStyle::SH_ToolTip_WakeUpDelay, nullptr, widget));
}

Qt::WindowFlags topLevelTooltipWindowFlags() {
  return Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint |
         Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput;
}

void clearTooltipManager(QWidget* widget, const AdTooltip* tooltip) {
  if (!widget || widget->property(detail::kTooltipManagerProperty).value<QObject*>() != tooltip) {
    return;
  }
  widget->setProperty(detail::kTooltipManagerProperty, QVariant());
}

void setTooltipManager(QWidget* widget, AdTooltip* tooltip) {
  if (widget) {
    widget->setProperty(detail::kTooltipManagerProperty,
                        QVariant::fromValue(static_cast<QObject*>(tooltip)));
  }
}

}  // namespace

class TooltipPopupView final {
 public:
  void ensureSurface(AdTooltipPrivate* owner, QWidget* scopeWindow);
  void releaseSurface(AdTooltipPrivate* owner);
  void syncContent(const QString& text, const QString& accessibleText) const;

  QPointer<QWidget> surface;
  QPointer<QWidget> bodyHost;
  QPointer<QVBoxLayout> bodyLayout;
  QPointer<QLabel> contentLabel;
};

struct TooltipStyleState {
  detail::TooltipVisualStyle visual;
  int popupOffset = 0;
  int borderRadius = 0;
};

class AdTooltipPrivate final : public QObject, private detail::OverlayPopupControllerDelegate {
  Q_DECLARE_PUBLIC(AdTooltip)

 public:
  explicit AdTooltipPrivate(AdTooltip* q);
  ~AdTooltipPrivate() override;

  bool eventFilter(QObject* watched, QEvent* event) override;

  QWidget* resolvedAnchorWidget() const;
  QWidget* scopeWidget() const;
  bool isEffectivelyDisabled() const;
  QFont effectiveBaseFont() const;
  QString accessibleText() const;
  bool hasRenderableContent() const;
  bool canShowByState() const;
  bool logicalVisible() const;
  bool actualVisible() const;
  void syncActualVisible();

  void ensurePopupSurface();
  void releasePopupSurface();
  void syncPopupContent();
  void updateStyle();
  void syncAccessibleState();
  void refreshVisiblePopup();
  void syncControllerConfiguration();
  void refreshDefaultHoverOpenDelay(bool emitSignal);
  void refreshObservedWidgets();
  void clearObservedWidgets();
  void handleObservedWidgetDestroyed(QObject* object);
  void ensureClosedWhenUnavailable();
  void applyInitiallyVisibleIfNeeded();
  void captureTopLevelTransientAnchor();
  void syncTransientOwner();
  void clearTransientOwner();
  AdTooltip::SemanticStyles effectiveSemanticStyles() const;
  void handleControllerPopupVisibleChanged(bool value);

  QObject* popupOwnerObject() const override;
  QWidget* popupTriggerWidget() const override;
  QWidget* popupAnchorWidget() const override;
  QWidget* popupScopeWindow() const override;
  QWidget* popupSurfaceWidget() const override;
  QWidget* popupEnsureSurface() override;
  void popupPrepareToShow() override;
  bool popupHasContent() const override;
  std::optional<QRect> popupTriggerGlobalRect() const override;
  std::optional<QRect> popupAnchorGlobalRect() const override;
  detail::OverlayPopupPlacement popupPlacement() const override;
  AdPopupLayerMode popupLayerMode() const override;
  bool popupAutoAdjustOverflow() const override;
  bool popupArrowVisible() const override;
  bool popupArrowPointAtCenter() const override;
  int popupOffset() const override;
  int popupArrowOffsetHorizontal() const override;
  int popupArrowOffsetVertical() const override;
  void popupApplyResolvedPlacement(detail::OverlayPopupPlacement placement,
                                   qreal arrowCenterCoord) override;
  bool popupReleaseOnHide() const override;
  void popupReleaseSurface() override;

  AdTooltip* const q_ptr;

  AdTooltip::Placement placement = AdTooltip::Placement::Top;
  AdTooltip::Triggers triggers = AdTooltip::Trigger::Hover;
  AdTooltip::ActivationMode activationMode = AdTooltip::ActivationMode::Automatic;
  AdTooltip::PopupLifetime popupLifetime = AdTooltip::PopupLifetime::Retained;
  AdTooltip::LayerMode layerMode = AdTooltip::LayerMode::InWindow;
  bool initiallyVisible = false;
  bool initiallyVisibleApplied = false;
  bool explicitVisibleSet = false;
  bool autoAdjustOverflow = true;
  bool arrowVisible = true;
  bool arrowPointAtCenter = false;
  int hoverOpenDelayMs = 700;
  int hoverCloseDelayMs = 300;
  bool hoverOpenDelayExplicit = false;
  bool enabled = true;
  QString text;
  AdTooltip::ComponentTokens componentTokens;
  AdTooltip::SemanticStyles semanticStyles;
  AdTooltip::SemanticStyleResolver semanticStyleResolver;

  QPointer<detail::OverlayPopupController> controller;
  QPointer<QWidget> targetWidget;
  QPointer<QWidget> anchorWidgetOverride;
  std::optional<QRect> triggerRect;
  std::optional<QRect> anchorRect;
  std::optional<QRect> topLevelTransientAnchorGlobalRect;

  TooltipPopupView popupView;
  TooltipStyleState styleState;
  bool visible = false;

  QHash<QObject*, QMetaObject::Connection> observedWidgetDestroyedConnections;
};

void TooltipPopupView::ensureSurface(AdTooltipPrivate* owner, QWidget* scopeWindow) {
  if (surface && bodyHost && bodyLayout && contentLabel) {
    return;
  }
  if (surface || bodyHost || bodyLayout || contentLabel) {
    releaseSurface(owner);
  }

  const bool topLevelTransient = owner->layerMode == AdTooltip::LayerMode::TopLevelTransient;
  auto* surfaceWidget = new detail::OverlayPopupSurface(topLevelTransient ? nullptr : scopeWindow);
  if (topLevelTransient) {
    surfaceWidget->setWindowFlags(topLevelTooltipWindowFlags());
    surfaceWidget->setAttribute(Qt::WA_ShowWithoutActivating, true);
    surfaceWidget->setAttribute(Qt::WA_TranslucentBackground, true);
    surfaceWidget->setProperty("adqt.tooltip.topLevelTransient", true);
  }
  surfaceWidget->setObjectName(QStringLiteral("adtooltip-surface"));
  surfaceWidget->setProperty("adqt.interaction.surface", true);
  surfaceWidget->setAttribute(Qt::WA_DeleteOnClose, false);
  surfaceWidget->setAttribute(Qt::WA_Hover, true);
  surfaceWidget->setMouseTracking(true);
  surfaceWidget->installEventFilter(owner);

  surface = surfaceWidget;
  bodyHost = surfaceWidget->bodyWidget();
  if (bodyHost) {
    bodyHost->setObjectName(QStringLiteral("adtooltip-popup"));
    bodyHost->setProperty("adqt.interaction.surface", true);
    bodyHost->setAttribute(Qt::WA_Hover, true);
    bodyHost->setMouseTracking(true);

    auto* layout = new QVBoxLayout(bodyHost);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    bodyLayout = layout;

    auto* label = new QLabel(bodyHost);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignLeading | Qt::AlignTop);
    bodyLayout->addWidget(label);
    contentLabel = label;
  }
}

void TooltipPopupView::releaseSurface(AdTooltipPrivate* owner) {
  if (surface) {
    surface->removeEventFilter(owner);
    surface->hide();
    surface->deleteLater();
  }

  surface.clear();
  bodyHost.clear();
  bodyLayout.clear();
  contentLabel.clear();
}

void TooltipPopupView::syncContent(const QString& text, const QString& accessibleText) const {
  if (contentLabel) {
    contentLabel->setText(text);
    contentLabel->setVisible(!text.trimmed().isEmpty());
    contentLabel->setAccessibleName(accessibleText);
    contentLabel->setAccessibleDescription(accessibleText);
  }
  if (bodyHost) {
    bodyHost->setAccessibleName(accessibleText);
    bodyHost->setAccessibleDescription(accessibleText);
  }
  if (surface) {
    surface->setAccessibleName(accessibleText);
    surface->setAccessibleDescription(accessibleText);
  }
}

AdTooltipPrivate::AdTooltipPrivate(AdTooltip* q) : QObject(q), q_ptr(q) {
  hoverOpenDelayMs = tooltipWakeUpDelay(scopeWidget());
  controller = new detail::OverlayPopupController(this, this);
  connect(controller, &detail::OverlayPopupController::popupVisibleChanged, this,
          &AdTooltipPrivate::handleControllerPopupVisibleChanged);
  connect(controller, &detail::OverlayPopupController::popupVisibilityRequested, q_ptr,
          &AdTooltip::visibilityRequested);
  syncControllerConfiguration();
}

AdTooltipPrivate::~AdTooltipPrivate() {
  clearTooltipManager(targetWidget, q_ptr);
  detail::clearDerivedAccessibleDescription(targetWidget);
  clearObservedWidgets();
  releasePopupSurface();
}

bool AdTooltipPrivate::eventFilter(QObject* watched, QEvent* event) {
  if (!watched || !event) {
    return QObject::eventFilter(watched, event);
  }

  if (watched == targetWidget || watched == anchorWidgetOverride) {
    if (event->type() == QEvent::EnabledChange) {
      syncControllerConfiguration();
      syncAccessibleState();
      ensureClosedWhenUnavailable();
      updateStyle();
      refreshVisiblePopup();
      syncActualVisible();
    } else if (isVisualRefreshEvent(event->type())) {
      refreshDefaultHoverOpenDelay(true);
      updateStyle();
      refreshVisiblePopup();
    }
  } else if (watched == popupView.surface) {
    if (event->type() == QEvent::Show ||
        (event->type() == QEvent::WinIdChange && logicalVisible())) {
      syncTransientOwner();
    }
    if (event->type() == QEvent::Show || event->type() == QEvent::Hide) {
      if (event->type() == QEvent::Hide) {
        clearTransientOwner();
      }
      syncActualVisible();
    }
  }

  return QObject::eventFilter(watched, event);
}

QWidget* AdTooltipPrivate::resolvedAnchorWidget() const {
  if (anchorWidgetOverride) {
    return anchorWidgetOverride;
  }
  if (targetWidget) {
    return targetWidget;
  }
  return qobject_cast<QWidget*>(q_ptr->parent());
}

QWidget* AdTooltipPrivate::scopeWidget() const {
  if (QWidget* anchor = resolvedAnchorWidget()) {
    return anchor;
  }
  return qobject_cast<QWidget*>(q_ptr->parent());
}

bool AdTooltipPrivate::isEffectivelyDisabled() const {
  if (!enabled) {
    return true;
  }
  if (targetWidget && !targetWidget->isEnabled()) {
    return true;
  }
  if (anchorWidgetOverride && !anchorWidgetOverride->isEnabled()) {
    return true;
  }
  if (!targetWidget && !anchorWidgetOverride) {
    if (QWidget* parentWidget = qobject_cast<QWidget*>(q_ptr->parent())) {
      return !parentWidget->isEnabled();
    }
  }
  return false;
}

QFont AdTooltipPrivate::effectiveBaseFont() const {
  if (componentTokens.textFont.has_value()) {
    return componentTokens.textFont.value();
  }
  if (targetWidget) {
    return targetWidget->font();
  }
  if (anchorWidgetOverride) {
    return anchorWidgetOverride->font();
  }
  if (QWidget* parentWidget = qobject_cast<QWidget*>(q_ptr->parent())) {
    return parentWidget->font();
  }
  return QFont();
}

QString AdTooltipPrivate::accessibleText() const { return accessibleTextForTooltip(text); }

bool AdTooltipPrivate::hasRenderableContent() const { return !text.trimmed().isEmpty(); }

bool AdTooltipPrivate::canShowByState() const {
  return hasRenderableContent() && resolvedAnchorWidget() && !isEffectivelyDisabled();
}

bool AdTooltipPrivate::logicalVisible() const { return controller && controller->popupVisible(); }

bool AdTooltipPrivate::actualVisible() const {
  return popupView.surface && popupView.surface->isVisible();
}

void AdTooltipPrivate::syncActualVisible() {
  const bool nextVisible = actualVisible();
  if (visible == nextVisible) {
    return;
  }
  visible = nextVisible;
  emit q_ptr->visibleChanged(visible);
}

void AdTooltipPrivate::ensurePopupSurface() {
  popupView.ensureSurface(this, popupScopeWindow());
  syncTransientOwner();
  if (controller) {
    controller->popupSurfaceChanged();
  }
  syncPopupContent();
  updateStyle();
}

void AdTooltipPrivate::releasePopupSurface() {
  clearTransientOwner();
  popupView.releaseSurface(this);
  if (controller) {
    controller->popupSurfaceChanged();
    controller->invalidatePopupGeometry();
  }
  syncActualVisible();
}

void AdTooltipPrivate::syncPopupContent() { popupView.syncContent(text, accessibleText()); }

AdTooltip::SemanticStyles AdTooltipPrivate::effectiveSemanticStyles() const {
  if (!semanticStyleResolver) {
    return semanticStyles;
  }

  AdTooltip::StyleContext context;
  context.placement = placement;
  context.triggers = triggers;
  context.visible = logicalVisible();
  context.enabled = !isEffectivelyDisabled();
  context.arrowVisible = arrowVisible;
  return mergeSemanticStyles(semanticStyles, semanticStyleResolver(context));
}

void AdTooltipPrivate::updateStyle() {
  detail::TooltipStyleInput styleInput;
  styleInput.placement = placement;
  styleInput.visible = logicalVisible();
  styleInput.disabled = isEffectivelyDisabled();
  styleInput.arrowVisible = arrowVisible;
  styleInput.baseFont = effectiveBaseFont();
  styleInput.componentTokens = componentTokens;
  styleInput.semanticStyles = effectiveSemanticStyles();

  const QWidget* themeOwner = resolvedAnchorWidget();
  if (!themeOwner) {
    themeOwner = qobject_cast<QWidget*>(q_ptr->parent());
  }

  if (themeOwner) {
    const adqt::theme::ResolvedTheme resolvedTheme =
        adqt::theme::ThemeManager::instance().resolve(themeOwner, themeOwner);
    styleState.visual = detail::resolveTooltipVisualStyle(styleInput, resolvedTheme);
  } else {
    styleState.visual = detail::resolveTooltipVisualStyle(styleInput);
  }
  styleState.popupOffset = std::max(0, styleState.visual.metrics.popupOffset);
  styleState.borderRadius = std::max(0, styleState.visual.metrics.borderRadius);

  auto* surface = static_cast<detail::OverlayPopupSurface*>(popupView.surface.data());
  if (!surface) {
    return;
  }

  detail::OverlayPopupSurfaceStyle surfaceStyle;
  surfaceStyle.background = styleState.visual.surfaceBackground;
  surfaceStyle.borderColor = styleState.visual.surfaceBorderColor;
  surfaceStyle.arrowBackground = styleState.visual.arrowBackground;
  surfaceStyle.arrowBorderColor = styleState.visual.arrowBorderColor;
  surfaceStyle.metrics.borderRadius = std::max(0, styleState.visual.metrics.borderRadius);
  surfaceStyle.metrics.borderWidth = std::max(0, styleState.visual.metrics.borderWidth);
  surfaceStyle.metrics.arrowSize = std::max(0, styleState.visual.metrics.arrowSize);
  surface->setSurfaceStyle(surfaceStyle);
  surface->setArrowVisible(arrowVisible);
  surface->setPlacement(toOverlayPlacement(placement));

  const int leftPadding = std::max(0, styleState.visual.metrics.padding.left());
  const int topPadding = std::max(0, styleState.visual.metrics.padding.top());
  const int rightPadding = std::max(0, styleState.visual.metrics.padding.right());
  const int bottomPadding = std::max(0, styleState.visual.metrics.padding.bottom());
  const int horizontalPaddingSpace = leftPadding + rightPadding;
  const int verticalPaddingSpace = topPadding + bottomPadding;
  const int arrowWidth = std::max(0, styleState.visual.metrics.arrowSize * 2);
  const int centerAlignedMinWidth =
      std::max(1, styleState.visual.metrics.borderRadius * 2 + arrowWidth);
  const int edgeOffsetHorizontal = (styleState.visual.metrics.borderRadius > 12)
                                       ? (styleState.visual.metrics.borderRadius + 2)
                                       : 12;
  const int edgeAlignedMinWidth =
      std::max(centerAlignedMinWidth,
               styleState.visual.metrics.borderRadius + arrowWidth + edgeOffsetHorizontal);
  const bool edgeAlignedPlacement = placement == AdTooltip::Placement::TopLeft ||
                                    placement == AdTooltip::Placement::TopRight ||
                                    placement == AdTooltip::Placement::BottomLeft ||
                                    placement == AdTooltip::Placement::BottomRight;
  const int bodyMinWidth = edgeAlignedPlacement ? edgeAlignedMinWidth : centerAlignedMinWidth;
  const int contentMinWidth = std::max(1, bodyMinWidth - horizontalPaddingSpace);
  const int contentMinHeight =
      std::max(0, styleState.visual.metrics.popupMinimumHeight - verticalPaddingSpace);
  const int contentMaxWidth =
      (styleState.visual.metrics.popupMaximumWidth > 0)
          ? std::max(1, styleState.visual.metrics.popupMaximumWidth - horizontalPaddingSpace)
          : QWIDGETSIZE_MAX;
  const int resolvedContentMaxWidth = std::max(contentMinWidth, contentMaxWidth);
  const int naturalContentWidth =
      measureTooltipNaturalTextWidth(text, styleState.visual.metrics.textFont);
  const int preferredContentWidth =
      std::clamp(std::max(0, naturalContentWidth), contentMinWidth, resolvedContentMaxWidth);
  const int preferredBodyWidth = preferredContentWidth + horizontalPaddingSpace;
  const bool shouldWrapText = naturalContentWidth > preferredContentWidth;

  if (popupView.bodyLayout) {
    popupView.bodyLayout->setContentsMargins(leftPadding, topPadding, rightPadding, bottomPadding);
  }
  if (popupView.bodyHost) {
    popupView.bodyHost->setMinimumWidth(preferredBodyWidth);
    popupView.bodyHost->setMinimumHeight(std::max(0, styleState.visual.metrics.popupMinimumHeight));
    popupView.bodyHost->setMaximumWidth(preferredBodyWidth);
  }
  if (popupView.contentLabel) {
    popupView.contentLabel->setWordWrap(shouldWrapText);
    popupView.contentLabel->setMinimumWidth(preferredContentWidth);
    popupView.contentLabel->setMinimumHeight(contentMinHeight);
    popupView.contentLabel->setMaximumWidth(preferredContentWidth);
    popupView.contentLabel->setFont(styleState.visual.metrics.textFont);
    QPalette palette = popupView.contentLabel->palette();
    palette.setColor(QPalette::WindowText, styleState.visual.contentColor);
    popupView.contentLabel->setPalette(palette);
  }

  if (controller) {
    controller->invalidatePopupGeometry();
  }
}

void AdTooltipPrivate::syncAccessibleState() {
  detail::syncDerivedAccessibleDescription(targetWidget, accessibleText());
}

void AdTooltipPrivate::refreshVisiblePopup() {
  if (controller && controller->popupVisible()) {
    controller->refreshVisiblePopup();
  }
}

void AdTooltipPrivate::syncControllerConfiguration() {
  if (!controller) {
    return;
  }
  controller->setTriggerModes(toOverlayTriggers(triggers));
  controller->setVisibilityMode(toControllerActivationMode(activationMode));
  controller->setDisabled(isEffectivelyDisabled());
  controller->setMouseEnterDelayMs(hoverOpenDelayMs);
  controller->setMouseLeaveDelayMs(hoverCloseDelayMs);
  controller->anchorWidgetChanged();
}

void AdTooltipPrivate::refreshDefaultHoverOpenDelay(bool emitSignal) {
  if (hoverOpenDelayExplicit) {
    return;
  }
  const int delay = tooltipWakeUpDelay(resolvedAnchorWidget());
  if (hoverOpenDelayMs == delay) {
    return;
  }
  hoverOpenDelayMs = delay;
  if (controller) {
    controller->setMouseEnterDelayMs(hoverOpenDelayMs);
  }
  if (emitSignal) {
    emit q_ptr->hoverOpenDelayMsChanged(hoverOpenDelayMs);
  }
}

void AdTooltipPrivate::refreshObservedWidgets() {
  QSet<QObject*> nextWidgets;
  if (targetWidget) {
    nextWidgets.insert(targetWidget);
  }
  if (anchorWidgetOverride) {
    nextWidgets.insert(anchorWidgetOverride);
  }

  for (auto it = observedWidgetDestroyedConnections.begin();
       it != observedWidgetDestroyedConnections.end();) {
    QObject* object = it.key();
    if (!object || !nextWidgets.contains(object)) {
      if (object) {
        object->removeEventFilter(this);
      }
      disconnect(it.value());
      it = observedWidgetDestroyedConnections.erase(it);
    } else {
      ++it;
    }
  }

  for (QObject* object : nextWidgets) {
    if (!object || observedWidgetDestroyedConnections.contains(object)) {
      continue;
    }
    object->installEventFilter(this);
    observedWidgetDestroyedConnections.insert(
        object, connect(object, &QObject::destroyed, this,
                        &AdTooltipPrivate::handleObservedWidgetDestroyed));
  }
}

void AdTooltipPrivate::clearObservedWidgets() {
  for (auto it = observedWidgetDestroyedConnections.begin();
       it != observedWidgetDestroyedConnections.end(); ++it) {
    if (QObject* object = it.key()) {
      object->removeEventFilter(this);
    }
    disconnect(it.value());
  }
  observedWidgetDestroyedConnections.clear();
}

void AdTooltipPrivate::handleObservedWidgetDestroyed(QObject* object) {
  const QWidget* previousResolvedAnchor = resolvedAnchorWidget();
  const bool anchorChanged = previousResolvedAnchor && previousResolvedAnchor == object;

  if (object == targetWidget) {
    detail::clearDerivedAccessibleDescription(targetWidget);
    targetWidget.clear();
    if (triggerRect.has_value()) {
      triggerRect.reset();
      emit q_ptr->triggerRectChanged();
    }
    emit q_ptr->targetWidgetChanged(nullptr);
  }
  if (object == anchorWidgetOverride) {
    anchorWidgetOverride.clear();
    emit q_ptr->anchorWidgetChanged(nullptr);
  }

  auto it = observedWidgetDestroyedConnections.find(object);
  if (it != observedWidgetDestroyedConnections.end()) {
    disconnect(it.value());
    observedWidgetDestroyedConnections.erase(it);
  }

  if (anchorChanged && anchorRect.has_value()) {
    anchorRect.reset();
    emit q_ptr->anchorRectChanged();
  }

  syncControllerConfiguration();
  syncAccessibleState();
  ensureClosedWhenUnavailable();
  updateStyle();
  applyInitiallyVisibleIfNeeded();
  refreshVisiblePopup();
  syncActualVisible();
}

void AdTooltipPrivate::ensureClosedWhenUnavailable() {
  if (controller && controller->popupVisible() && !canShowByState()) {
    controller->setPopupVisible(false);
  }
}

void AdTooltipPrivate::applyInitiallyVisibleIfNeeded() {
  if (!controller || initiallyVisibleApplied || explicitVisibleSet || !initiallyVisible ||
      activationMode == AdTooltip::ActivationMode::Manual || !canShowByState()) {
    return;
  }
  initiallyVisibleApplied = true;
  controller->setPopupVisible(true);
}

void AdTooltipPrivate::captureTopLevelTransientAnchor() {
  topLevelTransientAnchorGlobalRect.reset();
  if (layerMode != AdTooltip::LayerMode::TopLevelTransient) {
    return;
  }

  QWidget* coordinateWidget = resolvedAnchorWidget();
  if (!coordinateWidget || !coordinateWidget->isVisible()) {
    return;
  }

  const QRect localRect = anchorRect.value_or(coordinateWidget->rect());
  if (!localRect.isValid()) {
    return;
  }
  topLevelTransientAnchorGlobalRect =
      QRect(coordinateWidget->mapToGlobal(localRect.topLeft()), localRect.size());
}

void AdTooltipPrivate::syncTransientOwner() {
  if (layerMode != AdTooltip::LayerMode::TopLevelTransient || !popupView.surface) {
    return;
  }

  QWidget* anchor = resolvedAnchorWidget();
  QWidget* ownerWidget = anchor ? anchor->window() : nullptr;
  if (!ownerWidget || ownerWidget == popupView.surface) {
    return;
  }

  ownerWidget->winId();
  popupView.surface->winId();
  QWindow* ownerWindow = ownerWidget->windowHandle();
  QWindow* tooltipWindow = popupView.surface->windowHandle();
  if (!ownerWindow || !tooltipWindow) {
    return;
  }

  if (tooltipWindow->transientParent() != ownerWindow) {
    tooltipWindow->setTransientParent(ownerWindow);
  }

#if defined(Q_OS_WIN) || defined(_WIN32)
  const HWND tooltipHwnd = hwndFromWId(popupView.surface->winId());
  const HWND ownerHwnd = hwndFromWId(ownerWidget->winId());
  if (tooltipHwnd && ownerHwnd) {
    if (GetWindow(tooltipHwnd, GW_OWNER) != ownerHwnd) {
      SetWindowLongPtr(tooltipHwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(ownerHwnd));
    }
    SetWindowPos(tooltipHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
  }
#endif
}

void AdTooltipPrivate::clearTransientOwner() {
  if (!popupView.surface ||
      !popupView.surface->property("adqt.tooltip.topLevelTransient").toBool()) {
    return;
  }

  if (QWindow* tooltipWindow = popupView.surface->windowHandle()) {
    tooltipWindow->setTransientParent(nullptr);
  }

#if defined(Q_OS_WIN) || defined(_WIN32)
  const HWND tooltipHwnd = hwndFromWId(popupView.surface->effectiveWinId());
  if (tooltipHwnd && GetWindow(tooltipHwnd, GW_OWNER)) {
    SetWindowLongPtr(tooltipHwnd, GWLP_HWNDPARENT, 0);
  }
#endif
}

void AdTooltipPrivate::handleControllerPopupVisibleChanged(bool value) {
  updateStyle();
  if (popupView.surface) {
    detail::notifyAccessibilityEvent(popupView.surface,
                                     value ? QAccessible::ObjectShow : QAccessible::ObjectHide);
  }
  if (!value) {
    topLevelTransientAnchorGlobalRect.reset();
    clearTransientOwner();
    syncActualVisible();
  }
}

QObject* AdTooltipPrivate::popupOwnerObject() const { return q_ptr; }

QWidget* AdTooltipPrivate::popupTriggerWidget() const {
  if (targetWidget) {
    return targetWidget;
  }
  return resolvedAnchorWidget();
}

QWidget* AdTooltipPrivate::popupAnchorWidget() const { return resolvedAnchorWidget(); }

QWidget* AdTooltipPrivate::popupScopeWindow() const {
  return detail::resolvePopupScopeWindow(scopeWidget());
}

QWidget* AdTooltipPrivate::popupSurfaceWidget() const { return popupView.surface; }

QWidget* AdTooltipPrivate::popupEnsureSurface() {
  ensurePopupSurface();
  return popupView.surface;
}

void AdTooltipPrivate::popupPrepareToShow() {
  if (!topLevelTransientAnchorGlobalRect.has_value()) {
    captureTopLevelTransientAnchor();
  }
  ensurePopupSurface();
  syncTransientOwner();
  syncPopupContent();
  updateStyle();
}

bool AdTooltipPrivate::popupHasContent() const { return hasRenderableContent(); }

std::optional<QRect> AdTooltipPrivate::popupTriggerGlobalRect() const {
  if (!triggerRect.has_value() || !targetWidget || !targetWidget->isVisible()) {
    return std::nullopt;
  }
  const QRect localRect = triggerRect.value();
  if (!localRect.isValid()) {
    return std::nullopt;
  }
  return QRect(targetWidget->mapToGlobal(localRect.topLeft()), localRect.size());
}

std::optional<QRect> AdTooltipPrivate::popupAnchorGlobalRect() const {
  if (layerMode == AdTooltip::LayerMode::TopLevelTransient &&
      topLevelTransientAnchorGlobalRect.has_value()) {
    return topLevelTransientAnchorGlobalRect;
  }
  if (!anchorRect.has_value()) {
    return std::nullopt;
  }
  QWidget* coordinateWidget = resolvedAnchorWidget();
  if (!coordinateWidget || !coordinateWidget->isVisible()) {
    return std::nullopt;
  }
  return QRect(coordinateWidget->mapToGlobal(anchorRect.value().topLeft()),
               anchorRect.value().size());
}

detail::OverlayPopupPlacement AdTooltipPrivate::popupPlacement() const {
  return toOverlayPlacement(placement);
}

AdPopupLayerMode AdTooltipPrivate::popupLayerMode() const {
  return layerMode == AdTooltip::LayerMode::TopLevelTransient ? AdPopupLayerMode::QtTool
                                                              : AdPopupLayerMode::InWindow;
}

bool AdTooltipPrivate::popupAutoAdjustOverflow() const { return autoAdjustOverflow; }

bool AdTooltipPrivate::popupArrowVisible() const { return arrowVisible; }

bool AdTooltipPrivate::popupArrowPointAtCenter() const { return arrowPointAtCenter; }

int AdTooltipPrivate::popupOffset() const { return styleState.popupOffset; }

int AdTooltipPrivate::popupArrowOffsetHorizontal() const {
  return arrowOffsetHorizontalForRadius(std::max(0, styleState.borderRadius));
}

int AdTooltipPrivate::popupArrowOffsetVertical() const {
  return arrowOffsetVerticalForRadius(std::max(0, styleState.borderRadius));
}

void AdTooltipPrivate::popupApplyResolvedPlacement(detail::OverlayPopupPlacement placementValue,
                                                   qreal arrowCenterCoord) {
  auto* surface = static_cast<detail::OverlayPopupSurface*>(popupView.surface.data());
  if (!surface) {
    return;
  }
  surface->setPlacement(placementValue);
  surface->setArrowCenter(arrowCenterCoord);
}

bool AdTooltipPrivate::popupReleaseOnHide() const {
  return popupLifetime == AdTooltip::PopupLifetime::RecreateOnOpen;
}

void AdTooltipPrivate::popupReleaseSurface() { releasePopupSurface(); }

AdTooltip::AdTooltip(QObject* parent) : QObject(parent), d_ptr(new AdTooltipPrivate(this)) {}

AdTooltip::~AdTooltip() = default;

void AdTooltip::installApplicationTooltips() { detail::installQtTooltipBridge(); }

void AdTooltip::showText(QWidget* target, const QString& text, int displayTimeMs) {
  detail::showQtTooltip(target, text, displayTimeMs);
}

void AdTooltip::resetSyncPopupGeometryCountersForTesting() {
  detail::OverlayPopupController::resetSyncPopupGeometryCountersForTesting();
}

qint64 AdTooltip::syncPopupGeometryCallCountForTesting() {
  return detail::OverlayPopupController::syncPopupGeometryCallCountForTesting();
}

qint64 AdTooltip::syncPopupGeometryShortCircuitCountForTesting() {
  return detail::OverlayPopupController::syncPopupGeometryShortCircuitCountForTesting();
}

AdTooltip::Placement AdTooltip::placement() const {
  Q_D(const AdTooltip);
  return d->placement;
}

void AdTooltip::setPlacement(Placement value) {
  Q_D(AdTooltip);
  if (d->placement == value) {
    return;
  }
  d->placement = value;
  emit placementChanged(d->placement);
  d->updateStyle();
  d->refreshVisiblePopup();
}

AdTooltip::Triggers AdTooltip::triggers() const {
  Q_D(const AdTooltip);
  return d->triggers;
}

void AdTooltip::setTriggers(Triggers value) {
  Q_D(AdTooltip);
  if (d->triggers == value) {
    return;
  }
  d->triggers = value;
  if (d->controller) {
    d->controller->setTriggerModes(toOverlayTriggers(d->triggers));
  }
  emit triggersChanged(d->triggers);
}

bool AdTooltip::isVisible() const {
  Q_D(const AdTooltip);
  return d->visible;
}

void AdTooltip::setVisible(bool value) {
  Q_D(AdTooltip);
  if (!d->controller) {
    return;
  }
  d->explicitVisibleSet = true;
  if (value && !d->canShowByState()) {
    d->controller->setPopupVisible(false);
    d->syncActualVisible();
    return;
  }
  d->controller->setPopupVisible(value);
  d->syncActualVisible();
}

void AdTooltip::show() { setVisible(true); }

void AdTooltip::hide() { setVisible(false); }

void AdTooltip::toggle() { setVisible(!isVisible()); }

AdTooltip::ActivationMode AdTooltip::activationMode() const {
  Q_D(const AdTooltip);
  return d->activationMode;
}

void AdTooltip::setActivationMode(ActivationMode value) {
  Q_D(AdTooltip);
  if (d->activationMode == value) {
    return;
  }
  d->activationMode = value;
  if (d->controller) {
    d->controller->setVisibilityMode(toControllerActivationMode(d->activationMode));
  }
  emit activationModeChanged(d->activationMode);
  d->applyInitiallyVisibleIfNeeded();
}

AdTooltip::PopupLifetime AdTooltip::popupLifetime() const {
  Q_D(const AdTooltip);
  return d->popupLifetime;
}

void AdTooltip::setPopupLifetime(PopupLifetime value) {
  Q_D(AdTooltip);
  if (d->popupLifetime == value) {
    return;
  }
  d->popupLifetime = value;
  emit popupLifetimeChanged(d->popupLifetime);
  if (!d->actualVisible() && d->popupLifetime == PopupLifetime::RecreateOnOpen) {
    d->releasePopupSurface();
  }
}

AdTooltip::LayerMode AdTooltip::layerMode() const {
  Q_D(const AdTooltip);
  return d->layerMode;
}

void AdTooltip::setLayerMode(LayerMode value) {
  Q_D(AdTooltip);
  if (d->layerMode == value) {
    return;
  }

  const bool wasVisible = d->logicalVisible();
  d->layerMode = value;
  d->topLevelTransientAnchorGlobalRect.reset();
  d->releasePopupSurface();
  emit layerModeChanged(d->layerMode);
  if (wasVisible) {
    d->refreshVisiblePopup();
  }
}

bool AdTooltip::initiallyVisible() const {
  Q_D(const AdTooltip);
  return d->initiallyVisible;
}

void AdTooltip::setInitiallyVisible(bool value) {
  Q_D(AdTooltip);
  if (d->initiallyVisible == value) {
    return;
  }
  d->initiallyVisible = value;
  if (d->initiallyVisible) {
    d->initiallyVisibleApplied = false;
  }
  emit initiallyVisibleChanged(d->initiallyVisible);
  d->applyInitiallyVisibleIfNeeded();
}

bool AdTooltip::autoAdjustOverflow() const {
  Q_D(const AdTooltip);
  return d->autoAdjustOverflow;
}

void AdTooltip::setAutoAdjustOverflow(bool value) {
  Q_D(AdTooltip);
  if (d->autoAdjustOverflow == value) {
    return;
  }
  d->autoAdjustOverflow = value;
  emit autoAdjustOverflowChanged(d->autoAdjustOverflow);
  d->refreshVisiblePopup();
}

bool AdTooltip::arrowVisible() const {
  Q_D(const AdTooltip);
  return d->arrowVisible;
}

void AdTooltip::setArrowVisible(bool value) {
  Q_D(AdTooltip);
  if (d->arrowVisible == value) {
    return;
  }
  d->arrowVisible = value;
  emit arrowVisibleChanged(d->arrowVisible);
  d->updateStyle();
  d->refreshVisiblePopup();
}

bool AdTooltip::arrowPointAtCenter() const {
  Q_D(const AdTooltip);
  return d->arrowPointAtCenter;
}

void AdTooltip::setArrowPointAtCenter(bool value) {
  Q_D(AdTooltip);
  if (d->arrowPointAtCenter == value) {
    return;
  }
  d->arrowPointAtCenter = value;
  emit arrowPointAtCenterChanged(d->arrowPointAtCenter);
  d->refreshVisiblePopup();
}

int AdTooltip::hoverOpenDelayMs() const {
  Q_D(const AdTooltip);
  return d->hoverOpenDelayMs;
}

void AdTooltip::setHoverOpenDelayMs(int value) {
  Q_D(AdTooltip);
  const int normalized = std::max(0, value);
  d->hoverOpenDelayExplicit = true;
  if (d->hoverOpenDelayMs == normalized) {
    return;
  }
  d->hoverOpenDelayMs = normalized;
  if (d->controller) {
    d->controller->setMouseEnterDelayMs(d->hoverOpenDelayMs);
  }
  emit hoverOpenDelayMsChanged(d->hoverOpenDelayMs);
}

int AdTooltip::hoverCloseDelayMs() const {
  Q_D(const AdTooltip);
  return d->hoverCloseDelayMs;
}

void AdTooltip::setHoverCloseDelayMs(int value) {
  Q_D(AdTooltip);
  const int normalized = std::max(0, value);
  if (d->hoverCloseDelayMs == normalized) {
    return;
  }
  d->hoverCloseDelayMs = normalized;
  if (d->controller) {
    d->controller->setMouseLeaveDelayMs(d->hoverCloseDelayMs);
  }
  emit hoverCloseDelayMsChanged(d->hoverCloseDelayMs);
}

bool AdTooltip::isEnabled() const {
  Q_D(const AdTooltip);
  return d->enabled;
}

void AdTooltip::setEnabled(bool value) {
  Q_D(AdTooltip);
  if (d->enabled == value) {
    return;
  }
  d->enabled = value;
  emit enabledChanged(d->enabled);
  d->syncControllerConfiguration();
  d->syncAccessibleState();
  d->ensureClosedWhenUnavailable();
  d->updateStyle();
  d->applyInitiallyVisibleIfNeeded();
  d->refreshVisiblePopup();
  d->syncActualVisible();
}

QString AdTooltip::text() const {
  Q_D(const AdTooltip);
  return d->text;
}

void AdTooltip::setText(const QString& value) {
  Q_D(AdTooltip);
  if (d->text == value) {
    return;
  }
  d->text = value;
  emit textChanged(d->text);
  d->syncPopupContent();
  d->syncAccessibleState();
  if (d->controller) {
    d->controller->popupContentChanged(true);
  }
  d->ensureClosedWhenUnavailable();
  d->applyInitiallyVisibleIfNeeded();
  d->refreshVisiblePopup();
  d->syncActualVisible();
}

QWidget* AdTooltip::targetWidget() const {
  Q_D(const AdTooltip);
  return d->targetWidget;
}

void AdTooltip::setTargetWidget(QWidget* widget) {
  Q_D(AdTooltip);
  if (d->targetWidget == widget) {
    return;
  }

  const QWidget* previousResolvedAnchor = d->resolvedAnchorWidget();
  clearTooltipManager(d->targetWidget, this);
  detail::clearDerivedAccessibleDescription(d->targetWidget);
  d->targetWidget = widget;
  setTooltipManager(d->targetWidget, this);
  if (d->triggerRect.has_value()) {
    d->triggerRect.reset();
    emit triggerRectChanged();
  }
  d->refreshObservedWidgets();
  d->refreshDefaultHoverOpenDelay(true);
  d->syncControllerConfiguration();
  emit targetWidgetChanged(d->targetWidget);
  if (!d->anchorWidgetOverride && previousResolvedAnchor != d->resolvedAnchorWidget() &&
      d->anchorRect.has_value()) {
    d->anchorRect.reset();
    emit anchorRectChanged();
  }
  d->syncAccessibleState();
  d->ensureClosedWhenUnavailable();
  d->updateStyle();
  d->applyInitiallyVisibleIfNeeded();
  if (d->logicalVisible()) {
    d->captureTopLevelTransientAnchor();
  }
  d->refreshVisiblePopup();
  d->syncActualVisible();
}

QWidget* AdTooltip::anchorWidget() const {
  Q_D(const AdTooltip);
  return d->anchorWidgetOverride;
}

void AdTooltip::setAnchorWidget(QWidget* widget) {
  Q_D(AdTooltip);
  if (d->anchorWidgetOverride == widget) {
    return;
  }
  const QWidget* previousResolvedAnchor = d->resolvedAnchorWidget();
  d->anchorWidgetOverride = widget;
  d->refreshObservedWidgets();
  d->refreshDefaultHoverOpenDelay(true);
  d->syncControllerConfiguration();
  emit anchorWidgetChanged(d->anchorWidgetOverride);
  if (previousResolvedAnchor != d->resolvedAnchorWidget() && d->anchorRect.has_value()) {
    d->anchorRect.reset();
    emit anchorRectChanged();
  }
  d->ensureClosedWhenUnavailable();
  d->updateStyle();
  d->applyInitiallyVisibleIfNeeded();
  if (d->logicalVisible()) {
    d->captureTopLevelTransientAnchor();
  }
  d->refreshVisiblePopup();
  d->syncActualVisible();
}

bool AdTooltip::hasTriggerRect() const {
  Q_D(const AdTooltip);
  return d->triggerRect.has_value();
}

QRect AdTooltip::triggerRect() const {
  Q_D(const AdTooltip);
  return d->triggerRect.value_or(QRect());
}

void AdTooltip::setTriggerRect(const QRect& rect) {
  Q_D(AdTooltip);
  if (!rect.isValid()) {
    clearTriggerRect();
    return;
  }
  if (d->triggerRect == rect) {
    return;
  }

  d->triggerRect = rect;
  emit triggerRectChanged();
  d->syncControllerConfiguration();
}

void AdTooltip::clearTriggerRect() {
  Q_D(AdTooltip);
  if (!d->triggerRect.has_value()) {
    return;
  }
  d->triggerRect.reset();
  emit triggerRectChanged();
  d->syncControllerConfiguration();
}

bool AdTooltip::hasAnchorRect() const {
  Q_D(const AdTooltip);
  return d->anchorRect.has_value();
}

QRect AdTooltip::anchorRect() const {
  Q_D(const AdTooltip);
  return d->anchorRect.value_or(QRect());
}

void AdTooltip::setAnchorRect(const QRect& rect) {
  Q_D(AdTooltip);
  if (!rect.isValid()) {
    clearAnchorRect();
    return;
  }
  if (d->anchorRect == rect) {
    if (d->layerMode == LayerMode::TopLevelTransient && d->logicalVisible()) {
      const std::optional<QRect> previousGlobalRect = d->topLevelTransientAnchorGlobalRect;
      d->captureTopLevelTransientAnchor();
      if (d->topLevelTransientAnchorGlobalRect != previousGlobalRect) {
        d->refreshVisiblePopup();
      }
    }
    return;
  }

  d->anchorRect = rect;
  emit anchorRectChanged();
  if (d->logicalVisible()) {
    d->captureTopLevelTransientAnchor();
  }
  d->refreshVisiblePopup();
}

void AdTooltip::clearAnchorRect() {
  Q_D(AdTooltip);
  if (!d->anchorRect.has_value()) {
    return;
  }
  d->anchorRect.reset();
  emit anchorRectChanged();
  if (d->logicalVisible()) {
    d->captureTopLevelTransientAnchor();
  }
  d->refreshVisiblePopup();
}

AdTooltip::ComponentTokens AdTooltip::componentTokens() const {
  Q_D(const AdTooltip);
  return d->componentTokens;
}

void AdTooltip::setComponentTokens(const ComponentTokens& tokens) {
  Q_D(AdTooltip);
  d->componentTokens = tokens;
  emit componentTokensChanged();
  d->updateStyle();
  d->refreshVisiblePopup();
}

void AdTooltip::resetComponentTokens() {
  Q_D(AdTooltip);
  d->componentTokens = ComponentTokens{};
  emit componentTokensChanged();
  d->updateStyle();
  d->refreshVisiblePopup();
}

AdTooltip::SemanticStyles AdTooltip::semanticStyles() const {
  Q_D(const AdTooltip);
  return d->semanticStyles;
}

void AdTooltip::setSemanticStyles(const SemanticStyles& styles) {
  Q_D(AdTooltip);
  d->semanticStyles = styles;
  emit semanticStylesChanged();
  d->updateStyle();
  d->refreshVisiblePopup();
}

void AdTooltip::setSemanticStyleResolver(SemanticStyleResolver resolver) {
  Q_D(AdTooltip);
  d->semanticStyleResolver = std::move(resolver);
  emit semanticStylesChanged();
  d->updateStyle();
  d->refreshVisiblePopup();
}

}  // namespace adqt::widgets
