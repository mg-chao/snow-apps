#include "snow_shot/presentation/screenshottoolbarmainpanel.h"

#include "screenshottoolpalettebuttons.h"
#include "snow_shot/presentation/components/icons/iconrenderutils.h"
#include "snow_shot/presentation/mainwindowskincontroller.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"

#include "antd_icons.h"
#include "theme/theme_manager.h"
#include "widgets/control_scale.h"

#include <QBoxLayout>
#include <QEvent>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPaintEvent>
#include <QPixmap>
#include <QSizePolicy>
#include <QSpacerItem>
#include <QSet>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

namespace {
namespace outlined_icons = adqt::icons::antd::outlined;

constexpr int kButtonSize = 32;
constexpr int kIconSize = 24;
constexpr int kPanelHorizontalMargin = 12;
constexpr int kPanelMarginTop = 4;
constexpr int kPanelMarginBottom = 4;
constexpr int kDragHandleWidth = 18;
constexpr int kDragHandleIconSize = 18;
constexpr int kDragHandleTrailingSpacing = 4;
constexpr int kSeparatorHeight = 16;
constexpr int kSeparatorWidth = 1;
constexpr int kSeparatorSideSpacing = 12;
constexpr int kPanelRadius = 8;
#if !defined(Q_OS_MACOS)
constexpr qreal kShadowBlurRadius = 18.0;
constexpr qreal kShadowOffsetX = 0.0;
constexpr qreal kShadowOffsetY = 3.0;
constexpr QColor kShadowColor(0, 0, 0, 90);
#endif

QColor toolbarSurfaceColor() {
    const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
    return scheme.map.colorBgContainer.isValid() ? scheme.map.colorBgContainer : QColor(Qt::white);
}

QColor toolbarSeparatorColor() {
    const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
    return scheme.map.colorBorder.isValid() ? scheme.map.colorBorder : QColor(0xd9, 0xd9, 0xd9);
}

QColor toolbarDragHandleColor() {
    const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
    return scheme.map.colorTextQuaternary.isValid() ? scheme.map.colorTextQuaternary
                                                    : QColor(0xbf, 0xbf, 0xbf);
}

QString cssColor(const QColor& color) {
    if (color.alpha() == 255) {
        return color.name(QColor::HexRgb);
    }

    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

int scaledMetric(int value, qreal scale) {
    return adqt::widgets::scaleControlMetric(value, scale);
}

ScreenshotToolPaletteButtonMetrics buttonMetrics(qreal scale) {
    return ScreenshotToolPaletteButtonMetrics{
        kButtonSize,
        kIconSize,
        scale,
    };
}
} // namespace

ScreenshotToolbarPanel::ScreenshotToolbarPanel(QWidget* parent) : QFrame(parent) {
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setPanelScale(1.0);
    const auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    connect(&themeManager, &snow_shot::presentation::styles::ThemeManager::themeChanged, this,
            [this](const snow_shot::presentation::styles::ThemeColorScheme&) { update(); });
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    connect(&storage, &snow_shot::storage::ApplicationStorage::storageStatusChanged, this,
            [this] { syncSkinConfiguration(); });
    syncSkinConfiguration();
}

ScreenshotToolbarPanel::~ScreenshotToolbarPanel() {
    releaseSkin();
}

void ScreenshotToolbarPanel::syncSkinConfiguration() {
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    if (!storage.isInitialized() || m_skinConfiguration == &storage.configuration()) {
        return;
    }
    if (m_skinConfiguration) {
        disconnect(m_skinConfiguration, nullptr, this, nullptr);
    }
    m_skinConfiguration = &storage.configuration();
    connect(m_skinConfiguration, &snow_shot::storage::ConfigurationStore::valueChanged, this,
            [this](const QString& key) {
                if (key == QStringLiteral("interface/toolbar_skin_path") ||
                    key == QStringLiteral("interface/skin_opacity")) {
                    syncSkin();
                }
            });
    syncSkin();
}

bool ScreenshotToolbarPanel::releaseSkin() {
    const auto controller = m_skinController;
    const bool attached = m_skinAttached;
    const bool frameChanged = !m_skinFrame.isNull();
    if (controller)
        disconnect(controller, nullptr, this, nullptr);
    m_skinAttached = false;
    m_skinController.clear();
    m_skinFrame = {};
    m_skinPlacement = {};
    // Detachment can synchronously notify settings listeners that close this row.
    if (controller && attached)
        controller->detach(this);
    return frameChanged;
}

void ScreenshotToolbarPanel::syncSkin() {
    const bool enabled =
        isVisible() && m_skinConfiguration &&
        m_skinConfiguration->value(QStringLiteral("interface/skin_opacity")).toInt(100) > 0 &&
        !m_skinConfiguration->value(QStringLiteral("interface/toolbar_skin_path"))
             .toString()
             .isEmpty();
    if (!enabled) {
        const QPointer<ScreenshotToolbarPanel> lifetime(this);
        const bool frameChanged = releaseSkin();
        if (lifetime) {
            syncSkinAppearance(frameChanged);
        }
        return;
    }
    using snow_shot::presentation::MainWindowSkinController;
    if (!m_skinController) {
        m_skinController = &MainWindowSkinController::instance();
        connect(m_skinController, &MainWindowSkinController::viewFrameChanged, this,
                [this](QObject* view) {
                    if (view == this) {
                        syncSkinFrame();
                    }
                });
        connect(m_skinController, &MainWindowSkinController::appearanceChanged, this,
                [this] { syncSkinAppearance(); });
    }
    if (!m_skinAttached) {
        m_skinAttached = true;
        const QPointer<ScreenshotToolbarPanel> lifetime(this);
        m_skinController->attach(this, snow_shot::presentation::SkinSurface::Toolbar, size(),
                                 devicePixelRatioF());
        if (!lifetime) {
            return;
        }
    }
    syncSkinFrame();
}

void ScreenshotToolbarPanel::syncSkinFrame() {
    const auto frame = m_skinController ? m_skinController->pixmap(this) : QPixmap{};
    const auto placement =
        m_skinController ? m_skinController->frame(this).normalizedPlacement : QRectF{};
    const bool frameChanged =
        m_skinFrame.cacheKey() != frame.cacheKey() || m_skinPlacement != placement;
    m_skinFrame = frame;
    m_skinPlacement = placement;
    syncSkinAppearance(frameChanged);
}

void ScreenshotToolbarPanel::syncSkinAppearance(bool frameChanged) {
    const bool active = !m_skinFrame.isNull() && m_skinController;
    const qreal imageOpacity = active ? m_skinController->opacity() : 1.0;
    const qreal maskOpacity = active ? m_skinController->maskOpacity() : 1.0;
    const std::optional<qreal> mask = maskOpacity < 1.0 ? std::optional(maskOpacity) : std::nullopt;
    const bool maskChanged = mask != m_skinMaskOpacity;
    const bool paintChanged = frameChanged || imageOpacity != m_skinImageOpacity || maskChanged;
    m_skinImageOpacity = imageOpacity;
    m_skinMaskOpacity = mask;
    if (paintChanged) {
        update();
    }
    if (!maskChanged) {
        return;
    }
    // The row supplies the backdrop for its controls. Only a loaded skin can
    // replace their theme fills; text, icons, borders and preview colors stay intact.
    // Publish our state before a scope update can synchronously close this row.
    auto& controlTheme = adqt::theme::ThemeManager::instance();
    auto overrideValue = controlTheme.scopeOverride(this);
    overrideValue.backgroundOpacity = mask;
    controlTheme.setScopeOverride(this, overrideValue);
}

bool ScreenshotToolbarPanel::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::Show:
    case QEvent::Hide:
        break;
    case QEvent::Resize:
    case QEvent::DevicePixelRatioChange:
        if (!m_skinAttached) {
            return QFrame::event(event);
        }
        break;
    default:
        return QFrame::event(event);
    }
    const QPointer<ScreenshotToolbarPanel> lifetime(this);
    const bool handled = QFrame::event(event);
    if (!lifetime) {
        return handled;
    }
    if (event->type() == QEvent::Show) {
        syncSkinConfiguration();
        if (lifetime) {
            syncSkin();
        }
    } else if (event->type() == QEvent::Hide) {
        const bool frameChanged = releaseSkin();
        if (lifetime) {
            syncSkinAppearance(frameChanged);
        }
    } else if (m_skinAttached && m_skinController &&
               (event->type() == QEvent::Resize ||
                event->type() == QEvent::DevicePixelRatioChange)) {
        m_skinController->setViewport(this, size(), devicePixelRatioF(),
                                      event->type() == QEvent::DevicePixelRatioChange);
    }
    return handled;
}

void ScreenshotToolbarPanel::setPanelScale(qreal scale) {
    if (qFuzzyCompare(m_panelScale + 1.0, scale + 1.0)) {
        return;
    }
    m_panelScale = scale;
    m_panelRadius = scaledMetric(kPanelRadius, scale);
#if !defined(Q_OS_MACOS)
    auto* shadow = qobject_cast<QGraphicsDropShadowEffect*>(graphicsEffect());
    if (shadow == nullptr) {
        shadow = new QGraphicsDropShadowEffect(this);
        setGraphicsEffect(shadow);
    }
    shadow->setBlurRadius(kShadowBlurRadius * scale);
    shadow->setOffset(kShadowOffsetX * scale, kShadowOffsetY * scale);
    shadow->setColor(kShadowColor);
#endif
    update();
}

QString ScreenshotToolbarPanel::separatorStyleSheet() {
    return QStringLiteral("QFrame { background: %1; border: 0px; }")
        .arg(cssColor(toolbarSeparatorColor()));
}

QPainterPath ScreenshotToolbarPanel::surfacePath() const {
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()), m_panelRadius, m_panelRadius);
    return path;
}

void ScreenshotToolbarPanel::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(toolbarSurfaceColor());
    painter.drawPath(surfacePath());
    if (!m_skinFrame.isNull() && m_skinController) {
        painter.setClipPath(surfacePath());
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setOpacity(m_skinImageOpacity);
        const QRectF target(m_skinPlacement.x() * width(), m_skinPlacement.y() * height(),
                            m_skinPlacement.width() * width(), m_skinPlacement.height() * height());
        painter.drawPixmap(target, m_skinFrame, QRectF(m_skinFrame.rect()));
        painter.setOpacity(m_skinMaskOpacity.value_or(1.0));
        painter.fillPath(surfacePath(), toolbarSurfaceColor());
    }
}

ScreenshotToolbarMainPanel::ScreenshotToolbarMainPanel(const Options& options, QWidget* parent)
    : ScreenshotToolbarPanel(parent) {
    setObjectName(QStringLiteral("screenshotToolbarMainPanel"));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    auto* layout = new QHBoxLayout(this);
    m_layout = layout;

    if (options.showDragHandle) {
        auto* handle = new QLabel(this);
        handle->setObjectName(QStringLiteral("screenshotToolbarDragHandle"));
        configureScreenshotToolPaletteTooltip(handle, "Drag toolbar");
        handle->setFocusPolicy(Qt::NoFocus);
        handle->setCursor(Qt::SizeAllCursor);
        handle->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        handle->setAttribute(Qt::WA_Hover, true);
        handle->setAlignment(Qt::AlignCenter);
        m_dragHandle = handle;
        layout->addWidget(handle);
        addSpacing(kDragHandleTrailingSpacing);
    }

    applyMetrics();

    const auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    connect(&themeManager, &snow_shot::presentation::styles::ThemeManager::themeChanged, this,
            [this](const snow_shot::presentation::styles::ThemeColorScheme&) {
                updatePanelStyle();
                updateDragHandle(m_dragHandle);
                updateDragHandle(m_trailingDragHandle);
            });
}

QBoxLayout* ScreenshotToolbarMainPanel::contentLayout() const {
    return m_layout;
}

QWidget* ScreenshotToolbarMainPanel::dragHandle() const {
    return m_dragHandle;
}

QWidget* ScreenshotToolbarMainPanel::trailingDragHandle() const {
    return m_trailingDragHandle;
}

int ScreenshotToolbarMainPanel::buttonSize() const {
    return scaledMetric(kButtonSize, m_physicalScale);
}

QSize ScreenshotToolbarMainPanel::sizeHint() const {
    const QSize intrinsicHint = QFrame::sizeHint();
    // Readouts and other custom widgets own their sizes. Their dimensions can
    // change independently of the panel's reference metrics.
    if (hasCallerSizedWidgets()) {
        return intrinsicHint;
    }
    if (qFuzzyCompare(m_physicalScale + 1.0, 2.0)) {
        m_referenceSizeHint = intrinsicHint;
        return intrinsicHint;
    }
    if (!m_referenceSizeHint.isValid() || m_referenceSizeHint.isEmpty()) {
        return intrinsicHint;
    }
    return QSize(qMax(1, qRound(m_referenceSizeHint.width() * m_physicalScale)),
                 qMax(1, qRound(m_referenceSizeHint.height() * m_physicalScale)));
}

QMargins ScreenshotToolbarMainPanel::shadowMargins() {
#if defined(Q_OS_MACOS)
    return {};
#else
    return QMargins(24, 24, 24, 28);
#endif
}

adqt::widgets::AdButton*
ScreenshotToolbarMainPanel::createToolButton(const char* tooltip,
                                             const adqt::icons::IconRef& iconRef) {
    auto* button = createScreenshotToolPaletteToolButton(this, tooltip, iconRef,
                                                         buttonMetrics(m_physicalScale));
    m_buttons.push_back(button);
    return button;
}

adqt::widgets::AdButton* ScreenshotToolbarMainPanel::createActionButton(
    const char* tooltip, const adqt::icons::IconRef& iconRef, bool danger, bool primary) {
    auto* button = createScreenshotToolPaletteActionButton(this, tooltip, iconRef, danger, primary,
                                                           buttonMetrics(m_physicalScale));
    m_buttons.push_back(button);
    return button;
}

void ScreenshotToolbarMainPanel::addSpacing(int baseSpacing) {
    if (m_layout == nullptr) {
        return;
    }

    auto* spacer = new QSpacerItem(scaledMetric(baseSpacing, m_physicalScale), 0,
                                   QSizePolicy::Fixed, QSizePolicy::Minimum);
    m_layout->addSpacerItem(spacer);
    m_spacingItems.push_back(SpacingItem{spacer, baseSpacing});
}

void ScreenshotToolbarMainPanel::addSeparator() {
    if (m_layout == nullptr) {
        return;
    }

    addSpacing(kSeparatorSideSpacing);
    auto* separator = new QFrame(this);
    separator->setFrameShape(QFrame::NoFrame);
    separator->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    updateSeparatorStyle(separator);
    m_layout->addWidget(separator);
    m_separatorFrames.push_back(separator);
    addSpacing(kSeparatorSideSpacing);
    applyMetrics();
}

void ScreenshotToolbarMainPanel::resetContentLayout() {
    if (m_layout == nullptr) {
        return;
    }

    const QSet<QFrame*> separators(m_separatorFrames.cbegin(), m_separatorFrames.cend());
    while (QLayoutItem* item = m_layout->takeAt(0)) {
        QWidget* widget = item->widget();
        if (widget != nullptr) {
            widget->hide();
            if (QFrame* separator = qobject_cast<QFrame*>(widget);
                separator != nullptr && separators.contains(separator)) {
                delete separator;
            }
        }
        delete item;
    }
    m_separatorFrames.clear();
    m_spacingItems.clear();
    m_referenceSizeHint = QSize();

    if (m_dragHandle != nullptr) {
        m_dragHandle->show();
        m_layout->addWidget(m_dragHandle);
        addSpacing(kDragHandleTrailingSpacing);
    }
    applyMetrics();
}

void ScreenshotToolbarMainPanel::addTrailingDragHandle() {
    if (m_layout == nullptr) {
        return;
    }

    if (m_trailingDragHandle != nullptr) {
        if (m_layout->indexOf(m_trailingDragHandle) < 0) {
            m_layout->addWidget(m_trailingDragHandle);
        }
        m_trailingDragHandle->show();
        return;
    }

    auto* handle = new QLabel(this);
    handle->setObjectName(QStringLiteral("screenshotToolbarTrailingDragHandle"));
    configureScreenshotToolPaletteTooltip(handle, "Drag toolbar");
    handle->setFocusPolicy(Qt::NoFocus);
    handle->setCursor(Qt::SizeAllCursor);
    handle->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    handle->setAttribute(Qt::WA_Hover, true);
    handle->setAlignment(Qt::AlignCenter);
    m_trailingDragHandle = handle;
    m_layout->addWidget(handle);
    updateDragHandle(handle);
}

void ScreenshotToolbarMainPanel::setPhysicalScale(qreal scale) {
    const auto context =
        adqt::widgets::AdControlScaleContext::fromDprsAndContentScale(1.0, 1.0, scale);
    if (qFuzzyCompare(m_physicalScale, context.logicalScale))
        return;
    adqt::widgets::AdControlScaleScope scope(this);
    if (!scope.publishScale(context))
        scope.applyCurrentScaleToSubtree(this);
}

void ScreenshotToolbarMainPanel::commitControlScale(
    const adqt::widgets::AdControlScaleContext& context) {
    if (qFuzzyCompare(m_physicalScale, context.logicalScale))
        return;
    m_physicalScale = context.logicalScale;
    applyMetrics();
}

void ScreenshotToolbarMainPanel::changeEvent(QEvent* event) {
    if (event != nullptr && event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    QFrame::changeEvent(event);
}

void ScreenshotToolbarMainPanel::retranslateUi() {
    retranslateScreenshotToolPalette(this);
}

void ScreenshotToolbarMainPanel::applyMetrics() {
    if (m_layout == nullptr) {
        return;
    }

    m_layout->setSpacing(0);

    const ScreenshotToolPaletteButtonMetrics metrics = buttonMetrics(m_physicalScale);
    for (adqt::widgets::AdButton* button : m_buttons) {
        configureScreenshotToolPaletteBaseButton(button, nullptr, metrics);
    }

    for (QFrame* separator : m_separatorFrames) {
        if (separator != nullptr) {
            separator->setFixedSize(scaledMetric(kSeparatorWidth, m_physicalScale),
                                    scaledMetric(kSeparatorHeight, m_physicalScale));
        }
    }

    for (const SpacingItem& item : std::as_const(m_spacingItems)) {
        if (item.item != nullptr) {
            item.item->changeSize(scaledMetric(item.baseSpacing, m_physicalScale), 0,
                                  QSizePolicy::Fixed, QSizePolicy::Minimum);
        }
    }

    updateDragHandle(m_dragHandle);
    updateDragHandle(m_trailingDragHandle);

    QVector<int> referenceWidths{kPanelHorizontalMargin};
    referenceWidths.reserve(m_layout->count() + 2);
    for (int index = 0; index < m_layout->count(); ++index) {
        QLayoutItem* layoutItem = m_layout->itemAt(index);
        QWidget* widget = layoutItem != nullptr ? layoutItem->widget() : nullptr;
        int referenceWidth = 0;
        if (widget != nullptr && widget->isHidden()) {
            // Stateful slots keep their alternate source in the row. Only
            // the visible phase contributes to its reference-size budget.
        } else if (qobject_cast<adqt::widgets::AdButton*>(widget) != nullptr) {
            referenceWidth = kButtonSize;
        } else if (widget != nullptr &&
                   (widget == m_dragHandle || widget == m_trailingDragHandle)) {
            referenceWidth = kDragHandleWidth;
        } else if (m_separatorFrames.contains(qobject_cast<QFrame*>(widget))) {
            referenceWidth = kSeparatorWidth;
        } else if (layoutItem != nullptr && layoutItem->spacerItem() != nullptr) {
            for (const SpacingItem& spacing : std::as_const(m_spacingItems)) {
                if (spacing.item == layoutItem->spacerItem()) {
                    referenceWidth = spacing.baseSpacing;
                    break;
                }
            }
        }
        referenceWidths.append(referenceWidth);
    }
    referenceWidths.append(kPanelHorizontalMargin);

    const int referenceWidth = std::accumulate(referenceWidths.cbegin(), referenceWidths.cend(), 0);
    const int targetWidth =
        !hasCallerSizedWidgets() && m_referenceSizeHint.isValid() && !m_referenceSizeHint.isEmpty()
            ? qMax(1, qRound(m_referenceSizeHint.width() * m_physicalScale))
            : qMax(1, qRound(referenceWidth * m_physicalScale));
    const qreal cumulativeScale =
        referenceWidth > 0 ? static_cast<qreal>(targetWidth) / referenceWidth : m_physicalScale;
    const QVector<int> scaledEdges =
        adqt::widgets::scaleCumulativeWidths(referenceWidths, cumulativeScale, targetWidth);
    const auto scaledWidthAt = [&scaledEdges](int index) {
        return qMax(0, scaledEdges.at(index + 1) - scaledEdges.at(index));
    };
    m_layout->setContentsMargins(scaledWidthAt(0), scaledMetric(kPanelMarginTop, m_physicalScale),
                                 scaledWidthAt(static_cast<int>(referenceWidths.size()) - 1),
                                 scaledMetric(kPanelMarginBottom, m_physicalScale));
    for (int index = 0; index < m_layout->count(); ++index) {
        QLayoutItem* layoutItem = m_layout->itemAt(index);
        if (layoutItem == nullptr) {
            continue;
        }
        QWidget* widget = layoutItem->widget();
        const int roundedWidth = scaledWidthAt(index + 1);
        const int width = m_separatorFrames.contains(qobject_cast<QFrame*>(widget))
                              ? qMax(1, roundedWidth)
                              : roundedWidth;
        if (widget != nullptr) {
            // A zero reference width means that this widget's caller owns its
            // metrics. Never overwrite a custom readout with a zero width.
            if (referenceWidths.at(index + 1) > 0 &&
                widget->sizePolicy().horizontalPolicy() == QSizePolicy::Fixed) {
                widget->setFixedWidth(width);
            }
        } else if (QSpacerItem* spacer = layoutItem->spacerItem()) {
            spacer->changeSize(width, spacer->sizeHint().height(), QSizePolicy::Fixed,
                               QSizePolicy::Minimum);
        }
    }

    m_layout->invalidate();
    updatePanelStyle();
}

bool ScreenshotToolbarMainPanel::hasCallerSizedWidgets() const {
    if (m_layout == nullptr) {
        return false;
    }
    for (int index = 0; index < m_layout->count(); ++index) {
        auto* widget = m_layout->itemAt(index)->widget();
        if (widget != nullptr && qobject_cast<adqt::widgets::AdButton*>(widget) == nullptr &&
            widget != m_dragHandle && widget != m_trailingDragHandle &&
            !m_separatorFrames.contains(qobject_cast<QFrame*>(widget))) {
            return true;
        }
    }
    return false;
}

void ScreenshotToolbarMainPanel::updatePanelStyle() {
    for (QFrame* separator : std::as_const(m_separatorFrames)) {
        updateSeparatorStyle(separator);
    }

    setPanelScale(m_physicalScale);
}

void ScreenshotToolbarMainPanel::updateSeparatorStyle(QFrame* separator) {
    if (separator == nullptr) {
        return;
    }

    separator->setAttribute(Qt::WA_StyledBackground, true);
    separator->setStyleSheet(ScreenshotToolbarPanel::separatorStyleSheet());
}

void ScreenshotToolbarMainPanel::updateDragHandle(QWidget* handle) {
    if (handle == nullptr) {
        return;
    }

    handle->setFixedSize(scaledMetric(kDragHandleWidth, m_physicalScale), buttonSize());
    const QPixmap icon = snow_shot::presentation::icons::renderTintedIconPixmap(
        outlined_icons::Holder(),
        QSize(scaledMetric(kDragHandleIconSize, m_physicalScale),
              scaledMetric(kDragHandleIconSize, m_physicalScale)),
        handle->devicePixelRatioF(), toolbarDragHandleColor());
    if (!icon.isNull()) {
        if (auto* label = qobject_cast<QLabel*>(handle)) {
            label->setPixmap(icon);
        }
    }
}
