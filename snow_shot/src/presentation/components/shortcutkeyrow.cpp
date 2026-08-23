#include "snow_shot/presentation/components/shortcutkeyrow.h"

#include "snow_shot/presentation/components/infotooltipicon.h"
#include "snow_shot/presentation/components/icons/iconrenderutils.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/styles/buttonborder.h"
#include "snow_shot/presentation/styles/mainwindowcomponenttoken.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"

#include "antd_icons.h"
#include "theme/theme.h"
#include "widgets/button.h"
#include "widgets/button_style.h"
#include "widgets/detail/button_rendering.h"
#include "widgets/input_line_edit.h"
#include "widgets/modal.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
#include <QAbstractButton>
#include <QEvent>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QFrame>
#include <QHash>
#include <QHBoxLayout>
#include <QKeyCombination>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLayoutItem>
#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPointer>
#include <QRect>
#include <QRegularExpression>
#include <QObject>
#include <QOperatingSystemVersion>
#include <QSize>
#include <QSizePolicy>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace {
namespace outlined_icons = adqt::icons::antd::outlined;
namespace custom_outlined_icons = snow_shot::presentation::icons::custom::outlined;

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

QColor borderColorForShortcutRow(const QString& rowState, bool isPressed, bool isHovered,
                                 const snow_shot::presentation::styles::ThemeMapColorToken& map) {
    if (isPressed) {
        return map.colorPrimaryActive;
    }

    if (isHovered) {
        return map.colorPrimaryHover;
    }

    if (rowState == QStringLiteral("focus")) {
        return map.colorPrimary;
    }

    if (rowState == QStringLiteral("highlight")) {
        return map.colorPrimaryBorderHover;
    }

    return map.colorBorder;
}

adqt::widgets::AdButton::AccentRole
shortcutAccentRole(snow_shot::presentation::GlobalShortcutStatus status) {
    // Match KeyButton in E:\snow-shot: green for registered, orange for an
    // interrupted registration, and danger for a failed or unset shortcut.
    switch (status) {
    case snow_shot::presentation::GlobalShortcutStatus::Registered:
        return adqt::widgets::AdButton::AccentRole::Green;
    case snow_shot::presentation::GlobalShortcutStatus::PartiallyRegistered:
        return adqt::widgets::AdButton::AccentRole::Orange;
    case snow_shot::presentation::GlobalShortcutStatus::Failed:
        return adqt::widgets::AdButton::AccentRole::Danger;
    case snow_shot::presentation::GlobalShortcutStatus::Unset:
        return adqt::widgets::AdButton::AccentRole::Neutral;
    }
    return adqt::widgets::AdButton::AccentRole::Danger;
}

QColor shortcutStatusColor(snow_shot::presentation::GlobalShortcutStatus status,
                           const snow_shot::presentation::styles::ThemeMapColorToken& map) {
    switch (status) {
    case snow_shot::presentation::GlobalShortcutStatus::Registered:
        return map.presetColorHover.value(QStringLiteral("green"), map.colorSuccess);
    case snow_shot::presentation::GlobalShortcutStatus::PartiallyRegistered:
        return map.presetColorHover.value(QStringLiteral("orange"), map.colorWarning);
    case snow_shot::presentation::GlobalShortcutStatus::Failed:
        return map.colorError;
    case snow_shot::presentation::GlobalShortcutStatus::Unset:
        return map.colorTextTertiary;
    }
    return map.colorError;
}

QColor titleColorForShortcutRow(const QString& rowState, bool isPressed, bool isHovered,
                                const snow_shot::presentation::styles::ThemeMapColorToken& map) {
    if (isPressed) {
        return map.colorPrimaryActive;
    }

    if (isHovered) {
        return map.colorPrimaryHover;
    }

    if (rowState == QStringLiteral("focus")) {
        return map.colorPrimary;
    }

    if (rowState == QStringLiteral("highlight")) {
        return map.colorPrimaryHover;
    }

    return map.colorText;
}

QColor rowBackgroundColor(const QString& rowState,
                          const snow_shot::presentation::styles::ThemeMapColorToken& map) {
    if (rowState == QStringLiteral("highlight")) {
        return map.colorPrimaryBgHover;
    }

    return map.colorBgContainer;
}

QColor registrationStatusColor(snow_shot::presentation::GlobalShortcutStatus status,
                               const snow_shot::presentation::styles::ThemeMapColorToken& map) {
    return shortcutStatusColor(status, map);
}

constexpr int SHORTCUT_CONFIG_MODAL_WIDTH = 520;
constexpr int SHORTCUT_CONFIG_INPUT_MIN_WIDTH = 248;
constexpr int SHORTCUT_CONFIG_APPLY_BUTTON_WIDTH = 76;
constexpr int SHORTCUT_KEY_TEXT_MAX_WIDTH = 200;
constexpr int COMPACT_SHORTCUT_KEY_TEXT_MAX_WIDTH = 100;

bool isModifierOnlyKey(int key) {
    return key == Qt::Key_Control || key == Qt::Key_Alt ||
           key == Qt::Key_Meta || key == Qt::Key_AltGr || key == Qt::Key_Super_L ||
           key == Qt::Key_Super_R;
}

QString normalizeShortcutText(const QString& shortcut) {
    QString normalized = shortcut;
    normalized.replace(QStringLiteral("+"), QStringLiteral(" + "));
    normalized.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return normalized.trimmed();
}

QString compactShortcutText(const QString& shortcut) {
    QString compact = normalizeShortcutText(shortcut);
    compact.replace(QStringLiteral(" + "), QStringLiteral("+"));
    return compact;
}

QString formatShortcutDisplayText(const QString& shortcut) {
    QString displayText = compactShortcutText(shortcut);
    if (displayText.isEmpty()) {
        return displayText;
    }

    if (displayText.compare(QStringLiteral("Shift+Shift"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("Shift");
    }

    const bool hasPlusKey =
        displayText == QStringLiteral("+") || displayText.endsWith(QStringLiteral("++"));

    displayText.replace(QStringLiteral("Period"), QStringLiteral("."));
    displayText.replace(QStringLiteral("Comma"), QStringLiteral(","));
    // PortableText encodes Qt::KeypadModifier as "Num+"; it is not a chord.
    displayText.replace(QStringLiteral("Num+"), QStringLiteral("Num "));

    if (hasPlusKey) {
        displayText.chop(1);
        displayText.append(QStringLiteral("Plus"));
    }

    if (QOperatingSystemVersion::currentType() == QOperatingSystemVersion::MacOS) {
        displayText.replace(QStringLiteral("Meta"), QStringLiteral("Command"));
        displayText.replace(QStringLiteral("Alt"), QStringLiteral("Option"));
        displayText.replace(QStringLiteral("Ctrl"), QStringLiteral("Control"));
    } else {
        displayText.replace(QStringLiteral("Meta"), QStringLiteral("Win"));
        displayText.replace(QStringLiteral("Super"), QStringLiteral("Win"));
    }

    return displayText;
}

QString formatShortcutListDisplayText(const QStringList& shortcuts) {
    QStringList displayShortcuts;
    displayShortcuts.reserve(shortcuts.size());
    for (const QString& shortcut : shortcuts) {
        const QString displayText = formatShortcutDisplayText(shortcut);
        if (!displayText.isEmpty()) {
            displayShortcuts.push_back(displayText);
        }
    }
    return displayShortcuts.join(QStringLiteral(" / "));
}

QString shortcutTextForKey(const QKeyEvent& event) {
    const auto key = static_cast<Qt::Key>(event.key());
    if (key == Qt::Key_Shift) {
        return QStringLiteral("Shift");
    }
    if (key == Qt::Key_unknown || isModifierOnlyKey(key)) {
        return {};
    }

    const Qt::KeyboardModifiers modifiers =
        event.modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier |
                             Qt::MetaModifier | Qt::KeypadModifier);
    const QKeyCombination combination(modifiers, key);
    return QKeySequence(combination).toString(QKeySequence::PortableText).trimmed();
}

QString
shortcutValidationMessage(const snow_shot::presentation::GlobalShortcutValidationResult& validation,
                          const QString& attemptedShortcut,
                          ShortcutKeyRowConfig::ValidationScope validationScope) {
    const QString displayShortcut = formatShortcutDisplayText(attemptedShortcut);
    if (validationScope == ShortcutKeyRowConfig::ValidationScope::ScreenshotShortcut) {
        if (validation.failureReason ==
            snow_shot::presentation::GlobalShortcutFailureReason::AlreadyInUse) {
            return displayShortcut.isEmpty()
                       ? QObject::tr(
                             "This key is already assigned to another shortcut, try another key")
                       : QObject::tr(
                             "%1 is already assigned to another shortcut, try another key")
                             .arg(displayShortcut);
        }
        return displayShortcut.isEmpty()
                   ? QObject::tr("This key cannot be used as a screenshot shortcut, try another key")
                   : QObject::tr("%1 cannot be used as a screenshot shortcut, try another key")
                         .arg(displayShortcut);
    }
    if (validationScope == ShortcutKeyRowConfig::ValidationScope::DrawingShortcut) {
        if (validation.failureReason ==
            snow_shot::presentation::GlobalShortcutFailureReason::AlreadyInUse) {
            return displayShortcut.isEmpty()
                       ? QObject::tr(
                             "This key is already assigned to another drawing tool, try another key")
                       : QObject::tr(
                             "%1 is already assigned to another drawing tool, try another key")
                             .arg(displayShortcut);
        }
        return displayShortcut.isEmpty()
                   ? QObject::tr("This key cannot be used as a drawing shortcut, try another key")
                   : QObject::tr("%1 cannot be used as a drawing shortcut, try another key")
                         .arg(displayShortcut);
    }
    if (validationScope == ShortcutKeyRowConfig::ValidationScope::PinnedWindowShortcut) {
        if (validation.failureReason ==
            snow_shot::presentation::GlobalShortcutFailureReason::AlreadyInUse) {
            return displayShortcut.isEmpty()
                       ? QObject::tr(
                             "This key is already assigned to another pinned window action, try another key")
                       : QObject::tr(
                             "%1 is already assigned to another pinned window action, try another key")
                             .arg(displayShortcut);
        }
        return displayShortcut.isEmpty()
                   ? QObject::tr("This key cannot be used as a pinned window shortcut, try another key")
                   : QObject::tr("%1 cannot be used as a pinned window shortcut, try another key")
                         .arg(displayShortcut);
    }

    if (validation.failureReason ==
        snow_shot::presentation::GlobalShortcutFailureReason::UnsupportedPlatform) {
        return QObject::tr("Global shortcuts are not supported on this platform");
    }

    if (!displayShortcut.isEmpty()) {
        return QObject::tr("%1 cannot be registered as a Windows global shortcut, try another key")
            .arg(displayShortcut);
    }
    return QObject::tr(
        "This key cannot be registered as a Windows global shortcut, try another key");
}

class LegacyShortcutKeyConfigContent final : public QWidget {
  public:
    explicit LegacyShortcutKeyConfigContent(
        const QString& currentShortcut,
        const snow_shot::presentation::styles::ThemeColorScheme& colorScheme,
        QWidget* parent = nullptr)
        : QWidget(parent), m_colorScheme(colorScheme) {
        setObjectName(QStringLiteral("shortcutConfigContent"));

        const auto& metric = m_colorScheme.metricAlias;
        auto* rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(metric.marginSM);

        auto* inputRowLayout = new QHBoxLayout;
        inputRowLayout->setContentsMargins(0, 0, 0, 0);
        inputRowLayout->setSpacing(metric.marginSM);

        m_captureInput = new adqt::widgets::AdLineEdit(this);
        m_captureInput->setObjectName(QStringLiteral("shortcutConfigInput"));
        m_captureInput->setReadOnly(true);
        m_captureInput->setPlaceholderText(QObject::tr("Please press a key"));
        m_captureInput->setControlSize(adqt::widgets::AdLineEdit::ControlSize::Medium);
        m_captureInput->setVariant(adqt::widgets::AdLineEdit::Variant::Outlined);
        m_captureInput->setMinimumWidth(SHORTCUT_CONFIG_INPUT_MIN_WIDTH);
        m_captureInput->installEventFilter(this);
        inputRowLayout->addWidget(m_captureInput, 1);

        m_applyPendingButton = new adqt::widgets::AdButton(QObject::tr("Apply"), this);
        m_applyPendingButton->setObjectName(QStringLiteral("shortcutConfigApplyButton"));
        m_applyPendingButton->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Outline);
        m_applyPendingButton->setAccentRole(adqt::widgets::AdButton::AccentRole::Primary);
        m_applyPendingButton->setSizeClass(adqt::widgets::AdButton::SizeClass::Medium);
        m_applyPendingButton->setCursor(Qt::PointingHandCursor);
        m_applyPendingButton->setFixedHeight(metric.controlHeight);
        m_applyPendingButton->setFixedWidth(SHORTCUT_CONFIG_APPLY_BUTTON_WIDTH);
        inputRowLayout->addWidget(m_applyPendingButton, 0, Qt::AlignRight);
        rootLayout->addLayout(inputRowLayout);

        auto* chipRowLayout = new QHBoxLayout;
        chipRowLayout->setContentsMargins(0, 0, 0, 0);
        chipRowLayout->setSpacing(metric.marginXS);

        auto* chipWrap = new QFrame(this);
        chipWrap->setObjectName(QStringLiteral("shortcutConfigChip"));
        auto* chipLayout = new QHBoxLayout(chipWrap);
        chipLayout->setContentsMargins(metric.paddingXS, metric.paddingXXS, metric.paddingXS,
                                       metric.paddingXXS);
        chipLayout->setSpacing(0);

        m_shortcutChipLabel = new QLabel(chipWrap);
        m_shortcutChipLabel->setObjectName(QStringLiteral("shortcutConfigChipLabel"));
        chipLayout->addWidget(m_shortcutChipLabel, 0, Qt::AlignCenter);
        chipRowLayout->addWidget(chipWrap, 0, Qt::AlignLeft);
        chipRowLayout->addStretch(1);
        rootLayout->addLayout(chipRowLayout);
        m_shortcutChip = chipWrap;

        m_selectedShortcut = normalizeShortcutText(currentShortcut);
        m_pendingShortcut = m_selectedShortcut;
        m_captureInput->setText(m_pendingShortcut);
        m_applyPendingButton->setEnabled(false);

        connect(m_applyPendingButton, &QAbstractButton::clicked, this,
                [this]() { applyPendingShortcut(); });

        updateShortcutChip();
        applyTheme(m_colorScheme);
    }

    QString selectedShortcut() const {
        return m_selectedShortcut;
    }

    bool hasSelectedShortcut() const {
        return !normalizeShortcutText(m_selectedShortcut).isEmpty();
    }

    void focusCaptureInput() {
        if (m_captureInput == nullptr) {
            return;
        }

        m_captureInput->setFocus(Qt::OtherFocusReason);
        m_captureInput->selectAll();
    }

    std::function<void(bool)> selectedShortcutAvailabilityChanged;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched != m_captureInput) {
            return QWidget::eventFilter(watched, event);
        }

        if (event->type() == QEvent::KeyRelease) {
            return true;
        }

        if (event->type() != QEvent::KeyPress) {
            return QWidget::eventFilter(watched, event);
        }

        auto* keyEvent = static_cast<QKeyEvent*>(event);
        const int key = keyEvent->key();
        if (key == Qt::Key_Escape) {
            return QWidget::eventFilter(watched, event);
        }

        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            applyPendingShortcut();
            return true;
        }

        if (key == Qt::Key_Backspace || key == Qt::Key_Delete) {
            m_pendingShortcut.clear();
            m_selectedShortcut.clear();
            m_captureInput->clear();
            m_applyPendingButton->setEnabled(false);
            updateShortcutChip();
            return true;
        }

        const QString detectedShortcut = shortcutTextForKey(*keyEvent);
        if (detectedShortcut.isEmpty()) {
            return true;
        }

        m_pendingShortcut = detectedShortcut;
        m_captureInput->setText(m_pendingShortcut);
        m_captureInput->selectAll();
        m_applyPendingButton->setEnabled(m_pendingShortcut != m_selectedShortcut);
        return true;
    }

  private:
    void applyPendingShortcut() {
        if (m_pendingShortcut.trimmed().isEmpty()) {
            return;
        }

        m_selectedShortcut = m_pendingShortcut;
        m_applyPendingButton->setEnabled(false);
        updateShortcutChip();
    }

    void updateShortcutChip() {
        const QString normalizedShortcut = normalizeShortcutText(m_selectedShortcut);
        const bool hasShortcut = !normalizedShortcut.isEmpty();

        if (m_shortcutChip != nullptr) {
            m_shortcutChip->setVisible(hasShortcut);
        }
        if (m_shortcutChipLabel != nullptr) {
            m_shortcutChipLabel->setText(normalizedShortcut);
        }
        if (selectedShortcutAvailabilityChanged) {
            selectedShortcutAvailabilityChanged(hasShortcut);
        }
    }

    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
        m_colorScheme = scheme;
        const auto& metric = m_colorScheme.metricAlias;
        const auto& map = m_colorScheme.map;

        setStyleSheet(QStringLiteral("adqt--widgets--AdButton#shortcutConfigApplyButton {"
                                     "  min-width: %1px;"
                                     "  max-width: %1px;"
                                     "}"
                                     "QFrame#shortcutConfigChip {"
                                     "  border: %2px solid %3;"
                                     "  border-radius: %4px;"
                                     "  background-color: %5;"
                                     "}"
                                     "QLabel#shortcutConfigChipLabel {"
                                     "  color: %3;"
                                     "  font-size: %6px;"
                                     "  font-weight: 500;"
                                     "}")
                          .arg(SHORTCUT_CONFIG_APPLY_BUTTON_WIDTH)
                          .arg(metric.lineWidth)
                          .arg(cssColor(map.colorPrimary))
                          .arg(metric.borderRadiusSM + metric.borderRadiusXS)
                          .arg(cssColor(map.colorPrimaryBg))
                          .arg(metric.fontSizeLG));
    }

    snow_shot::presentation::styles::ThemeColorScheme m_colorScheme;
    adqt::widgets::AdLineEdit* m_captureInput = nullptr;
    adqt::widgets::AdButton* m_applyPendingButton = nullptr;
    QFrame* m_shortcutChip = nullptr;
    QLabel* m_shortcutChipLabel = nullptr;
    QString m_pendingShortcut;
    QString m_selectedShortcut;
};

class ShortcutConfigValidationButton final : public adqt::widgets::AdButton {
  public:
    explicit ShortcutConfigValidationButton(
        const snow_shot::presentation::styles::ThemeAliasMetricToken& metric,
        ShortcutKeyRowConfig::ValidationScope validationScope,
        QWidget* parent = nullptr)
        : adqt::widgets::AdButton(parent), m_infoGap(metric.marginXS),
          m_info(new InfoTooltipIcon(metric.fontSize, this)) {
        m_info->setObjectName(QStringLiteral("shortcutConfigValidationTooltipTrigger"));
        m_info->setProperty("inlineGap", m_infoGap);
        if (validationScope == ShortcutKeyRowConfig::ValidationScope::ScreenshotShortcut) {
            m_info->setAccessibleName(QObject::tr("Invalid screenshot shortcut"));
        } else if (validationScope == ShortcutKeyRowConfig::ValidationScope::DrawingShortcut) {
            m_info->setAccessibleName(QObject::tr("Invalid drawing shortcut"));
        } else if (validationScope ==
                   ShortcutKeyRowConfig::ValidationScope::PinnedWindowShortcut) {
            m_info->setAccessibleName(QObject::tr("Invalid pinned window shortcut"));
        } else {
            m_info->setAccessibleName(QObject::tr("Invalid global shortcut"));
        }
    }

    void setValidationTooltipText(const QString& text) {
        m_info->setTooltipText(text);
    }

    QSize sizeHint() const override {
        const adqt::widgets::detail::ButtonVisualStyle style = buttonVisualStyle();
        const QFontMetrics fontMetrics(style.metrics.font);
        const int textWidth = fontMetrics.horizontalAdvance(text());
        const int horizontalFrameWidth =
            (style.metrics.horizontalPadding + style.metrics.borderWidth) * 2;
        return QSize(horizontalFrameWidth + textWidth + m_infoGap + m_info->width(),
                     style.metrics.height);
    }

  protected:
    bool event(QEvent* event) override {
        const bool handled = adqt::widgets::AdButton::event(event);
        const QEvent::Type type = event->type();
        if (type == QEvent::Enter) {
            m_hovered = true;
        } else if (type == QEvent::Leave) {
            m_hovered = false;
        }
        if (type == QEvent::Enter || type == QEvent::Leave || type == QEvent::MouseButtonPress ||
            type == QEvent::MouseButtonRelease || type == QEvent::EnabledChange) {
            syncInfoColor();
        }
        return handled;
    }

    void resizeEvent(QResizeEvent* event) override {
        adqt::widgets::AdButton::resizeEvent(event);
        syncInfoGeometry();
    }

    void paintEvent(QPaintEvent* event) override {
        (void)event;

        const adqt::widgets::detail::ButtonVisualStyle style = buttonVisualStyle();
        const adqt::widgets::detail::ButtonStateStyle& state = buttonState(style);
        const auto& metrics = style.metrics;

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const bool joinedLeft = false;
        const bool joinedRight = false;
        const QRectF rawBorderRect = adqt::widgets::detail::joinedButtonBorderRect(
            rect(), metrics.borderWidth, joinedLeft, joinedRight);
        const QRectF shapeRect =
            adqt::widgets::detail::resolveButtonShapeRect(rawBorderRect, shape());
        const adqt::widgets::detail::ButtonCornerRadii corners =
            adqt::widgets::detail::resolveButtonCorners(shape(), shapeRect, metrics.borderRadius,
                                                        joinedLeft, joinedRight);
        const QPainterPath buttonPath = adqt::widgets::detail::roundedButtonPath(
            shapeRect, corners.topLeft, corners.topRight, corners.bottomRight, corners.bottomLeft);
        painter.fillPath(buttonPath, state.background);

        if (metrics.borderWidth > 0 && state.border.alpha() > 0) {
            painter.setPen(QPen(state.border, metrics.borderWidth, state.borderStyle));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(buttonPath);
        }

        const int contentInset = metrics.horizontalPadding + metrics.borderWidth;
        const QRect contentRect =
            rect().adjusted(contentInset, metrics.borderWidth, -contentInset, -metrics.borderWidth);
        const int availableTextWidth =
            std::max(0, contentRect.width() - m_infoGap - m_info->width());
        painter.setFont(metrics.font);
        const QFontMetrics fontMetrics(metrics.font);
        const QString displayText =
            fontMetrics.elidedText(text(), Qt::ElideRight, availableTextWidth);
        const int textWidth = fontMetrics.horizontalAdvance(displayText);
        const int contentWidth = textWidth + m_infoGap + m_info->width();
        const int textX =
            contentRect.left() + std::max(0, (contentRect.width() - contentWidth) / 2);

        painter.setPen(state.text);
        painter.drawText(QRect(textX, contentRect.top(), textWidth, contentRect.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, displayText);

        m_info->setGeometry(textX + textWidth + m_infoGap, (height() - m_info->height()) / 2,
                            m_info->width(), m_info->height());
        m_info->setIconColor(state.text);
        m_info->raise();
    }

  private:
    adqt::widgets::detail::ButtonVisualStyle buttonVisualStyle() const {
        adqt::widgets::detail::ButtonStyleInput input;
        input.buttonStyle = buttonStyle();
        input.accentRole = accentRole();
        input.sizeClass = sizeClass();
        input.flat = isFlat();
        input.defaultButton = isDefault();
        input.hasMenu = menu() != nullptr;
        input.baseFont = font();
        return adqt::widgets::detail::resolveButtonVisualStyle(
            input, adqt::theme::ThemeManager::instance().resolve(this));
    }

    const adqt::widgets::detail::ButtonStateStyle&
    buttonState(const adqt::widgets::detail::ButtonVisualStyle& style) const {
        if (!isEnabled()) {
            return style.disabled;
        }
        if (isDown()) {
            return style.active;
        }
        if (isChecked()) {
            return style.checked;
        }
        return m_hovered ? style.hover : style.normal;
    }

    void syncInfoColor() {
        const adqt::widgets::detail::ButtonVisualStyle style = buttonVisualStyle();
        m_info->setIconColor(buttonState(style).text);
    }

    void syncInfoGeometry() {
        const adqt::widgets::detail::ButtonVisualStyle style = buttonVisualStyle();
        const auto& metrics = style.metrics;
        const int contentInset = metrics.horizontalPadding + metrics.borderWidth;
        const QRect contentRect =
            rect().adjusted(contentInset, metrics.borderWidth, -contentInset, -metrics.borderWidth);
        const int availableTextWidth =
            std::max(0, contentRect.width() - m_infoGap - m_info->width());
        const QFontMetrics fontMetrics(metrics.font);
        const QString displayText =
            fontMetrics.elidedText(text(), Qt::ElideRight, availableTextWidth);
        const int textWidth = fontMetrics.horizontalAdvance(displayText);
        const int contentWidth = textWidth + m_infoGap + m_info->width();
        const int textX =
            contentRect.left() + std::max(0, (contentRect.width() - contentWidth) / 2);
        m_info->setGeometry(textX + textWidth + m_infoGap, (height() - m_info->height()) / 2,
                            m_info->width(), m_info->height());
        m_info->raise();
    }

    int m_infoGap = 6;
    InfoTooltipIcon* m_info = nullptr;
    bool m_hovered = false;
};

class ShortcutKeyConfigContent final : public QWidget {
  public:
    struct KeyConfig {
        QString recordKeys;
        int index = 0;
    };

    explicit ShortcutKeyConfigContent(
        const QStringList& currentShortcuts,
        const snow_shot::presentation::styles::ThemeColorScheme& colorScheme, int maxShortcutCount,
        std::function<snow_shot::presentation::GlobalShortcutValidationResult(const QString&)>
            shortcutValidator,
        ShortcutKeyRowConfig::ValidationScope validationScope,
        QWidget* parent = nullptr)
        : QWidget(parent), m_colorScheme(colorScheme),
          m_maxShortcutCount(std::max(1, maxShortcutCount)),
          m_shortcutValidator(std::move(shortcutValidator)),
          m_validationScope(validationScope) {
        setObjectName(QStringLiteral("shortcutConfigContent"));
        setFocusPolicy(Qt::StrongFocus);

        const auto& metric = m_colorScheme.metricAlias;
        auto* rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(metric.margin);

        m_keyListLayout = new QVBoxLayout;
        m_keyListLayout->setContentsMargins(0, 0, 0, 0);
        m_keyListLayout->setSpacing(metric.margin);
        rootLayout->addLayout(m_keyListLayout);

        m_addButton = new adqt::widgets::AdButton(QObject::tr("Add key config"), this);
        m_addButton->setObjectName(QStringLiteral("shortcutConfigAddButton"));
        m_addButton->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Dashed);
        m_addButton->setAccentRole(adqt::widgets::AdButton::AccentRole::Primary);
        m_addButton->setShape(adqt::widgets::AdButton::Shape::Rounded);
        m_addButton->setIconRef(outlined_icons::Plus());
        m_addButton->setCursor(Qt::PointingHandCursor);
        m_addButton->setFixedHeight(metric.controlHeight);
        m_addButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        rootLayout->addWidget(m_addButton);

        connect(m_addButton, &QAbstractButton::clicked, this, [this]() { addKeyConfig(); });

        for (const QString& shortcutPart : currentShortcuts) {
            if (m_keyConfigs.size() >= m_maxShortcutCount) {
                break;
            }

            const QString recordKeys = normalizeShortcutText(shortcutPart);
            if (!recordKeys.isEmpty()) {
                m_keyConfigs.push_back({recordKeys, m_nextConfigIndex++});
            }
        }
        if (m_keyConfigs.isEmpty()) {
            m_keyConfigs.push_back({QString(), m_nextConfigIndex++});
        }

        if (m_keyConfigs.size() == 1 && m_keyConfigs.first().recordKeys.trimmed().isEmpty()) {
            m_recordingConfigIndex = m_keyConfigs.first().index;
        }

        rebuildKeyConfigRows();
        applyTheme(m_colorScheme);
    }

    ~ShortcutKeyConfigContent() override {
        stopRecording();
    }

    QStringList selectedShortcuts() const {
        QStringList recordKeysList;
        for (const KeyConfig& keyConfig : m_keyConfigs) {
            const QString recordKeys = compactShortcutText(keyConfig.recordKeys);
            if (!recordKeys.isEmpty() && !recordKeysList.contains(recordKeys)) {
                recordKeysList.push_back(recordKeys);
            }
        }

        return recordKeysList;
    }

    bool hasSelectedShortcut() const {
        return !selectedShortcuts().isEmpty();
    }

    bool canAcceptDialog() const {
        return m_recordingConfigIndex < 0 || !m_pendingShortcut.trimmed().isEmpty();
    }

    void commitPendingShortcut() {
        if (m_recordingConfigIndex >= 0 && !m_pendingShortcut.trimmed().isEmpty()) {
            applyPendingShortcut();
        }
    }

    void focusInitialControl() {
        setFocus(Qt::OtherFocusReason);
        ensureKeyboardGrabbed();
    }

    std::function<void(bool)> selectedShortcutAvailabilityChanged;
    std::function<void(bool)> acceptanceAvailabilityChanged;

  protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (m_recordingConfigIndex < 0) {
            QWidget::keyPressEvent(event);
            return;
        }

        recordKeyEvent(*event);
        rebuildKeyConfigRows();
        event->accept();
    }

    void keyReleaseEvent(QKeyEvent* event) override {
        if (m_recordingConfigIndex >= 0) {
            event->accept();
            return;
        }

        QWidget::keyReleaseEvent(event);
    }

  private:
    void rebuildKeyConfigRows() {
        clearLayout(m_keyListLayout);

        const auto& metric = m_colorScheme.metricAlias;
        const int rowHeight = metric.controlHeight;
        const int actionButtonWidth = metric.controlHeight;

        for (const KeyConfig& keyConfig : m_keyConfigs) {
            auto* rowWidget = new QWidget(this);
            rowWidget->setObjectName(QStringLiteral("shortcutConfigRow"));
            auto* rowLayout = new QHBoxLayout(rowWidget);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(metric.marginSM);

            const bool isRecording = keyConfig.index == m_recordingConfigIndex;
            const bool hasValidationError = isRecording && !m_validationMessage.isEmpty();

            ShortcutConfigValidationButton* validationButton = nullptr;
            adqt::widgets::AdButton* keyButton = nullptr;
            if (hasValidationError) {
                validationButton =
                    new ShortcutConfigValidationButton(metric, m_validationScope, rowWidget);
                keyButton = validationButton;
            } else {
                keyButton = new adqt::widgets::AdButton(rowWidget);
            }
            keyButton->setObjectName(QStringLiteral("shortcutConfigKeyButton"));
            keyButton->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Outline);
            keyButton->setAccentRole(
                hasValidationError ? adqt::widgets::AdButton::AccentRole::Danger
                                   : (isRecording ? adqt::widgets::AdButton::AccentRole::Primary
                                                  : adqt::widgets::AdButton::AccentRole::Neutral));
            keyButton->setShape(adqt::widgets::AdButton::Shape::Rounded);
            keyButton->setIconRef(isRecording ? adqt::icons::IconRef()
                                              : outlined_icons::MacCommand());
            keyButton->setCursor(Qt::PointingHandCursor);
            keyButton->setFixedHeight(rowHeight);
            keyButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

            if (isRecording) {
                keyButton->setProperty("shortcutValidationState",
                                       hasValidationError ? QStringLiteral("invalid")
                                                          : (m_pendingShortcut.trimmed().isEmpty()
                                                                 ? QStringLiteral("waiting")
                                                                 : QStringLiteral("valid")));
                keyButton->setProperty("shortcutValidationMessage", m_validationMessage);
                keyButton->setAccessibleDescription(m_validationMessage);
                if (hasValidationError) {
                    keyButton->setText(m_rejectedShortcut.trimmed().isEmpty()
                                           ? QObject::tr("Unsupported key")
                                           : formatShortcutDisplayText(m_rejectedShortcut));
                    validationButton->setValidationTooltipText(m_validationMessage);
                } else {
                    keyButton->setBusy(true);
                    keyButton->setText(m_pendingShortcut.trimmed().isEmpty()
                                           ? QObject::tr("Please press a key")
                                           : formatShortcutDisplayText(m_pendingShortcut));
                }
            } else {
                keyButton->setText(formatShortcutDisplayText(keyConfig.recordKeys));
            }

            connect(keyButton, &QAbstractButton::clicked, this,
                    [this, configIndex = keyConfig.index]() { startRecording(configIndex); });
            rowLayout->addWidget(keyButton, 0);
            rowLayout->addStretch(1);

            auto* actionButton = new adqt::widgets::AdButton(rowWidget);
            actionButton->setObjectName(QStringLiteral("shortcutConfigActionButton"));
            actionButton->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Outline);
            actionButton->setShape(adqt::widgets::AdButton::Shape::Rounded);
            actionButton->setCursor(Qt::PointingHandCursor);
            actionButton->setFixedSize(actionButtonWidth, rowHeight);

            if (isRecording) {
                actionButton->setAccentRole(adqt::widgets::AdButton::AccentRole::Green);
                actionButton->setIconRef(outlined_icons::Check());
                actionButton->setEnabled(!m_pendingShortcut.trimmed().isEmpty());
                connect(actionButton, &QAbstractButton::clicked, this,
                        [this]() { applyPendingShortcut(); });
            } else {
                actionButton->setAccentRole(adqt::widgets::AdButton::AccentRole::Danger);
                actionButton->setIconRef(outlined_icons::IconDelete());
                connect(actionButton, &QAbstractButton::clicked, this,
                        [this, configIndex = keyConfig.index]() { deleteKeyConfig(configIndex); });
            }

            rowLayout->addWidget(actionButton, 0, Qt::AlignRight);
            m_keyListLayout->addWidget(rowWidget);
        }

        syncAddButtonState();
        notifyShortcutAvailabilityChanged();
    }

    void startRecording(int configIndex) {
        for (KeyConfig& keyConfig : m_keyConfigs) {
            if (keyConfig.index == configIndex) {
                keyConfig.recordKeys.clear();
                break;
            }
        }

        m_recordingConfigIndex = configIndex;
        m_pendingShortcut.clear();
        m_rejectedShortcut.clear();
        m_validationMessage.clear();
        ensureKeyboardGrabbed();
        setFocus(Qt::OtherFocusReason);
        rebuildKeyConfigRows();
    }

    void applyPendingShortcut() {
        if (m_recordingConfigIndex < 0 || m_pendingShortcut.trimmed().isEmpty()) {
            return;
        }

        for (KeyConfig& keyConfig : m_keyConfigs) {
            if (keyConfig.index == m_recordingConfigIndex) {
                keyConfig.recordKeys = normalizeShortcutText(m_pendingShortcut);
                break;
            }
        }

        stopRecording();
        rebuildKeyConfigRows();
    }

    void stopRecording() {
        if (m_keyboardGrabbed) {
            releaseKeyboard();
            m_keyboardGrabbed = false;
        }

        m_recordingConfigIndex = -1;
        m_pendingShortcut.clear();
        m_rejectedShortcut.clear();
        m_validationMessage.clear();
    }

    void ensureKeyboardGrabbed() {
        if (m_recordingConfigIndex < 0 || m_keyboardGrabbed) {
            return;
        }

        grabKeyboard();
        m_keyboardGrabbed = true;
    }

    void recordKeyEvent(const QKeyEvent& event) {
        if (isModifierOnlyKey(event.key())) {
            m_pendingShortcut.clear();
            m_rejectedShortcut.clear();
            m_validationMessage.clear();
            return;
        }

        const QString shortcut = shortcutTextForKey(event);
        snow_shot::presentation::GlobalShortcutValidationResult validation{
            shortcut,
            !shortcut.isEmpty(),
            shortcut.isEmpty()
                ? snow_shot::presentation::GlobalShortcutFailureReason::InvalidShortcut
                : snow_shot::presentation::GlobalShortcutFailureReason::None,
        };
        if (!shortcut.isEmpty() && m_shortcutValidator) {
            validation = m_shortcutValidator(shortcut);
        }

        if (validation.supported) {
            m_pendingShortcut =
                validation.shortcut.trimmed().isEmpty() ? shortcut : validation.shortcut;
            m_rejectedShortcut.clear();
            m_validationMessage.clear();
            return;
        }

        m_pendingShortcut.clear();
        m_rejectedShortcut = shortcut;
        m_validationMessage =
            shortcutValidationMessage(validation, shortcut, m_validationScope);
    }

    void deleteKeyConfig(int configIndex) {
        if (m_recordingConfigIndex == configIndex) {
            stopRecording();
        }

        m_keyConfigs.erase(std::remove_if(m_keyConfigs.begin(), m_keyConfigs.end(),
                                          [configIndex](const KeyConfig& keyConfig) {
                                              return keyConfig.index == configIndex;
                                          }),
                           m_keyConfigs.end());
        rebuildKeyConfigRows();
    }

    void addKeyConfig() {
        if (m_recordingConfigIndex >= 0 || m_keyConfigs.size() >= m_maxShortcutCount) {
            return;
        }

        const int configIndex = m_nextConfigIndex++;
        m_keyConfigs.push_back({QString(), configIndex});
        startRecording(configIndex);
    }

    void syncAddButtonState() {
        if (m_addButton == nullptr) {
            return;
        }

        const bool canAddMore = m_maxShortcutCount > 1 && m_keyConfigs.size() < m_maxShortcutCount;
        m_addButton->setVisible(canAddMore);
        m_addButton->setEnabled(canAddMore && m_recordingConfigIndex < 0);
    }

    void notifyShortcutAvailabilityChanged() {
        if (selectedShortcutAvailabilityChanged) {
            selectedShortcutAvailabilityChanged(hasSelectedShortcut());
        }
        if (acceptanceAvailabilityChanged) {
            acceptanceAvailabilityChanged(canAcceptDialog());
        }
    }

    static void clearLayout(QLayout* layout) {
        if (layout == nullptr) {
            return;
        }

        while (QLayoutItem* item = layout->takeAt(0)) {
            if (QWidget* widget = item->widget(); widget != nullptr) {
                widget->deleteLater();
            }
            delete item;
        }
    }

    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
        m_colorScheme = scheme;
        const auto& metric = m_colorScheme.metricAlias;

        setStyleSheet(QStringLiteral("QWidget#shortcutConfigContent {"
                                     "  background-color: transparent;"
                                     "}"
                                     "QWidget#shortcutConfigRow {"
                                     "  background-color: transparent;"
                                     "}"));

        if (m_addButton != nullptr) {
            m_addButton->setFixedHeight(metric.controlHeight);
        }
    }

    snow_shot::presentation::styles::ThemeColorScheme m_colorScheme;
    QVBoxLayout* m_keyListLayout = nullptr;
    adqt::widgets::AdButton* m_addButton = nullptr;
    QVector<KeyConfig> m_keyConfigs;
    QString m_pendingShortcut;
    QString m_rejectedShortcut;
    QString m_validationMessage;
    int m_maxShortcutCount = 2;
    int m_recordingConfigIndex = -1;
    int m_nextConfigIndex = 0;
    bool m_keyboardGrabbed = false;
    std::function<snow_shot::presentation::GlobalShortcutValidationResult(const QString&)>
        m_shortcutValidator;
    ShortcutKeyRowConfig::ValidationScope m_validationScope =
        ShortcutKeyRowConfig::ValidationScope::GlobalShortcut;
};

class ShortcutKeyButton final : public adqt::widgets::AdButton {
  public:
    explicit ShortcutKeyButton(const snow_shot::presentation::styles::ThemeAliasMetricToken& metric,
                               int textMaxWidth,
                               QWidget* parent = nullptr)
        : adqt::widgets::AdButton(parent), m_iconTextSpacing(metric.marginXS),
          m_textMaxWidth(std::max(0, textMaxWidth)) {
        setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Dashed);
        setAccentRole(adqt::widgets::AdButton::AccentRole::Danger);
        setShape(adqt::widgets::AdButton::Shape::Rounded);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);
        setMinimumWidth(0);

        QFont buttonFont = font();
        buttonFont.setPixelSize(metric.fontSize);
        buttonFont.setWeight(QFont::Normal);
        setFont(buttonFont);

        m_statusTooltipTrigger = new InfoTooltipIcon(metric.fontSize, this);
        m_statusTooltipTrigger->setObjectName(
            QStringLiteral("shortcutRegistrationStatusTooltipTrigger"));
        m_statusTooltipTrigger->setProperty("inlineGap", m_iconTextSpacing);
        m_statusTooltipTrigger->hide();
    }

    QSize sizeHint() const override {
        const adqt::widgets::detail::ButtonVisualStyle style = buttonVisualStyle();
        const QFontMetricsF fontMetrics(style.metrics.font);
        const int textWidth = static_cast<int>(std::ceil(fontMetrics.horizontalAdvance(text())));
        const int cappedTextWidth = std::min(textWidth, m_textMaxWidth);
        const int iconSize = std::max(10, style.metrics.font.pixelSize());
        const int iconGap = contentTextGap(style.metrics);
        const int horizontalFrameWidth =
            (style.metrics.horizontalPadding + style.metrics.borderWidth) * 2;
        return QSize(horizontalFrameWidth + iconSize + iconGap + cappedTextWidth +
                         statusTooltipReservationWidth(),
                     style.metrics.height);
    }

    QSize minimumSizeHint() const override {
        return QSize(0, buttonVisualStyle().metrics.height);
    }

    void setRegistrationStatus(snow_shot::presentation::GlobalShortcutStatus status) {
        m_status = status;
        setProperty("registrationStatus", static_cast<int>(status));
        setAccentRole(shortcutAccentRole(status));
        syncStatusTooltipTrigger();
        update();
    }

    void setRegistrationStatusTooltipVisible(bool visible) {
        if (m_statusTooltipVisible == visible) {
            return;
        }

        m_statusTooltipVisible = visible;
        syncStatusTooltipTrigger();
        updateGeometry();
        update();
    }

    InfoTooltipIcon* registrationStatusTooltipTrigger() const {
        return m_statusTooltipTrigger;
    }

    void setTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
        m_colorScheme = scheme;
        syncStatusTooltipTrigger();
        update();
    }

  protected:
    bool event(QEvent* event) override {
        const bool handled = adqt::widgets::AdButton::event(event);

        const QEvent::Type type = event->type();
        if (type == QEvent::Enter) {
            m_hovered = true;
        } else if (type == QEvent::Leave) {
            m_hovered = false;
        }
        if (type == QEvent::Enter || type == QEvent::Leave || type == QEvent::MouseButtonPress ||
            type == QEvent::MouseButtonRelease) {
            syncStatusTooltipTrigger();
            update();
        }

        return handled;
    }

    void paintEvent(QPaintEvent* event) override {
        (void)event;

        const adqt::widgets::detail::ButtonVisualStyle style = buttonVisualStyle();
        const adqt::widgets::detail::ButtonStateStyle& state = buttonState(style);
        const auto& metrics = style.metrics;

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const bool joinedLeft = false;
        const bool joinedRight = false;
        const QRectF rawBorderRect = adqt::widgets::detail::joinedButtonBorderRect(
            rect(), metrics.borderWidth, joinedLeft, joinedRight);
        const QRectF shapeRect =
            adqt::widgets::detail::resolveButtonShapeRect(rawBorderRect, shape());
        const adqt::widgets::detail::ButtonCornerRadii corners =
            adqt::widgets::detail::resolveButtonCorners(shape(), shapeRect, metrics.borderRadius,
                                                        joinedLeft, joinedRight);
        const QPainterPath buttonPath = adqt::widgets::detail::roundedButtonPath(
            shapeRect, corners.topLeft, corners.topRight, corners.bottomRight, corners.bottomLeft);
        painter.fillPath(buttonPath, state.background);

        if (metrics.borderWidth > 0 && state.border.alpha() > 0) {
            const bool dashed = buttonStyle() == adqt::widgets::AdButton::ButtonStyle::Dashed ||
                                buttonStyle() == adqt::widgets::AdButton::ButtonStyle::GhostDashed;
            const Qt::PenStyle borderStyle = dashed ? Qt::DashLine : state.borderStyle;
            QPen borderPen = adqt::widgets::detail::makeButtonBorderPen(
                state.border, metrics.borderWidth, borderStyle);
            painter.setPen(borderPen);
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(buttonPath);
        }

        const int contentInset = metrics.horizontalPadding + metrics.borderWidth;
        const QRect contentRect =
            rect().adjusted(contentInset, metrics.borderWidth, -contentInset, -metrics.borderWidth);
        const int iconSize = std::max(10, metrics.font.pixelSize());
        const bool hasStatusTrigger = m_statusTooltipVisible && m_statusTooltipTrigger != nullptr;
        const int statusTriggerWidth = hasStatusTrigger ? m_statusTooltipTrigger->width() : 0;
        const int iconTextGap = contentTextGap(metrics);
        const int statusTriggerGap = hasStatusTrigger && !text().isEmpty() ? metrics.iconGap : 0;
        const int availableTextWidth = std::max(0, contentRect.width() - iconSize - iconTextGap -
                                                       statusTriggerGap - statusTriggerWidth);

        painter.setFont(metrics.font);
        const QFontMetrics fontMetrics(metrics.font);
        const QString displayText =
            text().isEmpty() ? QString()
                             : fontMetrics.elidedText(text(), Qt::ElideRight, availableTextWidth);
        const int textWidth =
            displayText.isEmpty() ? 0 : fontMetrics.horizontalAdvance(displayText);
        const int displayIconTextGap = displayText.isEmpty() ? 0 : contentTextGap(metrics);
        const int displayStatusTriggerGap =
            hasStatusTrigger && !displayText.isEmpty() ? metrics.iconGap : 0;
        const int contentWidth = iconSize + displayIconTextGap + textWidth +
                                 displayStatusTriggerGap + statusTriggerWidth;
        const int startX =
            contentRect.left() + std::max(0, (contentRect.width() - contentWidth) / 2);

        QColor keyboardColor = state.text;
        if (!m_hovered && !isDown()) {
            keyboardColor.setAlpha(
                static_cast<int>(std::lround(static_cast<double>(keyboardColor.alpha()) * 0.42)));
        }
        const QPixmap keyboardIcon = snow_shot::presentation::icons::renderTintedIconPixmap(
            custom_outlined_icons::Keyboard(), QSize(iconSize, iconSize), devicePixelRatioF(),
            keyboardColor);
        if (!keyboardIcon.isNull()) {
            painter.drawPixmap(startX, (height() - iconSize) / 2, keyboardIcon);
        }

        const int textX = startX + iconSize + displayIconTextGap;
        if (!displayText.isEmpty()) {
            painter.setPen(contentTextColor(state));
            painter.drawText(QRect(textX, contentRect.top(), textWidth, contentRect.height()),
                             Qt::AlignLeft | Qt::AlignVCenter, displayText);
        }

        if (hasStatusTrigger) {
            const int triggerX = textX + textWidth + displayStatusTriggerGap;
            m_statusTooltipTrigger->setGeometry(
                triggerX, (height() - m_statusTooltipTrigger->height()) / 2,
                m_statusTooltipTrigger->width(), m_statusTooltipTrigger->height());
            m_statusTooltipTrigger->raise();
        }
    }

  private:
    adqt::widgets::detail::ButtonVisualStyle buttonVisualStyle() const {
        adqt::widgets::detail::ButtonStyleInput input;
        input.buttonStyle = buttonStyle();
        input.accentRole = accentRole();
        input.sizeClass = sizeClass();
        input.flat = isFlat();
        input.defaultButton = isDefault();
        input.hasMenu = menu() != nullptr;
        input.baseFont = font();
        return adqt::widgets::detail::resolveButtonVisualStyle(
            input, adqt::theme::ThemeManager::instance().resolve(this));
    }

    const adqt::widgets::detail::ButtonStateStyle&
    buttonState(const adqt::widgets::detail::ButtonVisualStyle& style) const {
        if (!isEnabled()) {
            return style.disabled;
        }
        if (isDown()) {
            return style.active;
        }
        if (isChecked()) {
            return style.checked;
        }
        return m_hovered ? style.hover : style.normal;
    }

    int contentTextGap(const adqt::widgets::detail::ButtonMetrics& metrics) const {
        if (text().isEmpty()) {
            return 0;
        }

        // The reference KeyButton keeps the empty shortcut-value element in
        // its flex layout, so the Unset description follows two Button gaps.
        return m_status == snow_shot::presentation::GlobalShortcutStatus::Unset
                   ? metrics.iconGap * 2
                   : metrics.iconGap;
    }

    QColor contentTextColor(const adqt::widgets::detail::ButtonStateStyle& state) const {
        return m_status == snow_shot::presentation::GlobalShortcutStatus::Unset
                   ? m_colorScheme.map.colorTextTertiary
                   : state.text;
    }

    int statusTooltipReservationWidth() const {
        return m_statusTooltipVisible && m_statusTooltipTrigger != nullptr
                   ? m_iconTextSpacing + m_statusTooltipTrigger->width()
                   : 0;
    }

    void syncStatusTooltipTrigger() {
        if (m_statusTooltipTrigger == nullptr) {
            return;
        }

        m_statusTooltipTrigger->setVisible(m_statusTooltipVisible);
        if (!m_statusTooltipVisible) {
            return;
        }

        m_statusTooltipTrigger->setIconColor(buttonState(buttonVisualStyle()).text);
    }

    int m_iconTextSpacing = 6;
    int m_textMaxWidth = SHORTCUT_KEY_TEXT_MAX_WIDTH;
    InfoTooltipIcon* m_statusTooltipTrigger = nullptr;
    bool m_statusTooltipVisible = false;
    bool m_hovered = false;
    snow_shot::presentation::GlobalShortcutStatus m_status =
        snow_shot::presentation::GlobalShortcutStatus::Unset;
    snow_shot::presentation::styles::ThemeColorScheme m_colorScheme;
};
} // namespace

ShortcutKeyRow::ShortcutKeyRow(
    const ShortcutKeyRowConfig& config,
    const snow_shot::presentation::styles::ThemeAliasMetricToken& metric,
    const snow_shot::presentation::styles::MainWindowComponentMetricToken& mainWindowMetric,
    QWidget* parent)
    : adqt::widgets::AdButton(parent), m_rowState(config.rowState), m_baseTitle(config.title),
      m_registrationState(config.registrationState),
      m_maxShortcutCount(std::max(1, config.maxShortcutCount)),
      m_shortcutValidator(config.shortcutValidator),
      m_adjustableDelay(config.adjustableDelay),
      m_delaySeconds(std::clamp(config.delaySeconds, 1, 10)),
      m_delaySetter(config.delaySetter),
      m_colorScheme(snow_shot::presentation::styles::ThemeManager::instance().themeColorScheme()) {
    m_showRegistrationStatus = config.showRegistrationStatus;
    m_compactPresentation =
        config.presentation == ShortcutKeyRowConfig::Presentation::CompactFormField;
    m_validationScope = config.validationScope;
    if (m_registrationState.shortcuts.isEmpty() && !config.shortcuts.isEmpty()) {
        m_registrationState.shortcuts = config.shortcuts;
    }

    setAccessibleName(config.title);
    setButtonStyle(m_compactPresentation ? adqt::widgets::AdButton::ButtonStyle::Text
                                         : adqt::widgets::AdButton::ButtonStyle::Outline);
    setAccentRole(adqt::widgets::AdButton::AccentRole::Primary);
    setShape(adqt::widgets::AdButton::Shape::Rounded);
    setFixedHeight(m_compactPresentation ? metric.controlHeight
                                         : metric.controlHeightLG + metric.paddingXXS);
    setCursor(m_compactPresentation ? Qt::ArrowCursor : Qt::PointingHandCursor);
    if (m_adjustableDelay) {
        setToolTip(tr("Delay: %1 seconds").arg(m_delaySeconds));
    }
    setFocusPolicy(Qt::NoFocus);
    setCheckable(false);
    setAttribute(Qt::WA_Hover, true);

    m_useStableBorder = config.useStableBorder;
    m_rowBorderWidth = metric.lineWidth;
    m_rowBorderRadius = mainWindowMetric.cardRadius;
    m_titleIconRef = config.iconRef;
    m_titleIconSize = metric.fontSizeLG + metric.borderRadiusXS;

    auto* rowLayout = new QHBoxLayout(this);
    if (m_compactPresentation) {
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(metric.marginXS);
    } else {
        rowLayout->setContentsMargins(metric.padding, metric.paddingXXS, metric.padding,
                                      metric.paddingXXS);
        rowLayout->setSpacing(metric.marginXS + metric.borderRadiusXS);
    }

    auto* titleWrap = new QWidget(this);
    titleWrap->setAttribute(Qt::WA_TransparentForMouseEvents, !m_adjustableDelay);
    auto* titleLayout = new QHBoxLayout(titleWrap);
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(metric.marginXS);

    const QString initialTitle = delayDisplayTitle();
    m_titleLabel = new QLabel(titleLabelText(), titleWrap);
    m_titleLabel->setObjectName(m_adjustableDelay ? QStringLiteral("delayTitleLabel")
                                                  : QStringLiteral("shortcutTitleLabel"));
    setAccessibleName(initialTitle);
    m_titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents, !m_adjustableDelay);
    if (m_adjustableDelay) {
        m_titleLabel->setCursor(Qt::SplitVCursor);
        m_titleLabel->installEventFilter(this);
        m_delayUnderline = new QWidget(m_titleLabel);
        m_delayUnderline->setObjectName(QStringLiteral("delaySecondsHoverUnderline"));
        m_delayUnderline->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_delayUnderline->hide();
    }
    titleLayout->addWidget(m_titleLabel, 0, Qt::AlignVCenter);

    if (!m_compactPresentation && adqt::icons::isValid(m_titleIconRef)) {
        m_titleIcon = new QLabel(titleWrap);
        m_titleIcon->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_titleIcon->setFixedSize(m_titleIconSize, m_titleIconSize);
        titleLayout->addWidget(m_titleIcon, 0, Qt::AlignVCenter);
    }

    if (!m_compactPresentation) {
        titleLayout->addStretch(1);
    }
    rowLayout->addWidget(titleWrap, m_compactPresentation ? 0 : 1);

    auto* shortcutButton = new ShortcutKeyButton(
        metric, m_compactPresentation ? COMPACT_SHORTCUT_KEY_TEXT_MAX_WIDTH
                                      : SHORTCUT_KEY_TEXT_MAX_WIDTH,
        this);
    shortcutButton->setObjectName(QStringLiteral("shortcutKeyButton"));
    shortcutButton->setFixedHeight(metric.controlHeight);
    if (m_compactPresentation) {
        shortcutButton->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Outline);
    }
    shortcutButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    shortcutButton->installEventFilter(this);
    connect(shortcutButton, &QAbstractButton::clicked, this,
            &ShortcutKeyRow::openShortcutConfigDialog);
    rowLayout->addWidget(shortcutButton, 0,
                         m_compactPresentation ? Qt::AlignVCenter : Qt::AlignRight);
    if (m_compactPresentation) {
        rowLayout->addStretch(1);
    }
    m_shortcutButton = shortcutButton;

    const auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    connect(&themeManager, &snow_shot::presentation::styles::ThemeManager::themeChanged, this,
            &ShortcutKeyRow::applyTheme);

    applyTheme(m_colorScheme);
}

void ShortcutKeyRow::applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
    m_colorScheme = scheme;

    if (m_shortcutButton != nullptr) {
        auto* button = static_cast<ShortcutKeyButton*>(m_shortcutButton);
        button->setTheme(scheme);
    }

    syncTitle();
    syncDelayUnderline();
    syncRegistrationStatus();
    update();
}

void ShortcutKeyRow::setTitle(const QString& title) {
    m_baseTitle = title;
    const QString displayTitle = delayDisplayTitle();
    if (m_titleLabel != nullptr) {
        m_titleLabel->setText(titleLabelText());
    }
    setAccessibleName(displayTitle);
    syncDelayUnderline();
}

void ShortcutKeyRow::retranslateUi() {
    if (m_adjustableDelay) {
        setToolTip(tr("Delay: %1 seconds").arg(m_delaySeconds));
    }
    syncRegistrationStatus();
    updateGeometry();
    update();
}

void ShortcutKeyRow::setRegistrationState(
    const snow_shot::presentation::GlobalShortcutRegistrationState& state) {
    m_registrationState = state;
    syncRegistrationStatus();
    updateGeometry();
    update();
}

void ShortcutKeyRow::setDelaySeconds(int seconds) {
    if (!m_adjustableDelay) {
        return;
    }
    const int clamped = std::clamp(seconds, 1, 10);
    if (m_delaySeconds == clamped) {
        return;
    }
    m_delaySeconds = clamped;
    setTitle(m_baseTitle);
    setToolTip(tr("Delay: %1 seconds").arg(m_delaySeconds));
    updateGeometry();
    update();
}

int ShortcutKeyRow::delaySeconds() const {
    return m_delaySeconds;
}

void ShortcutKeyRow::paintEvent(QPaintEvent* event) {
    (void)event;

    if (m_compactPresentation) {
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const auto& map = m_colorScheme.map;
    const QColor bgColor = rowBackgroundColor(m_rowState, map);

    painter.setPen(Qt::NoPen);
    painter.setBrush(bgColor);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                            static_cast<qreal>(m_rowBorderRadius),
                            static_cast<qreal>(m_rowBorderRadius));

    if (!m_useStableBorder) {
        return;
    }

    const bool shortcutButtonActive = isShortcutButtonActive();
    const QColor borderColor = borderColorForShortcutRow(
        m_rowState, isDown() && !shortcutButtonActive, underMouse() && !shortcutButtonActive, map);

    snow_shot::presentation::styles::ButtonBorderSpec spec;
    spec.color = borderColor;
    spec.width = m_rowBorderWidth;
    spec.radius = m_rowBorderRadius;
    spec.pattern = snow_shot::presentation::styles::BorderPattern::Solid;
    spec.widthRounding = snow_shot::presentation::styles::BorderWidthRounding::Floor;
    snow_shot::presentation::styles::drawButtonBorder(&painter, size(), spec);
}

bool ShortcutKeyRow::event(QEvent* event) {
    const bool handled = adqt::widgets::AdButton::event(event);

    const QEvent::Type type = event->type();
    if (type == QEvent::LanguageChange) {
        retranslateUi();
    }
    if (type == QEvent::Enter || type == QEvent::Leave || type == QEvent::MouseButtonPress ||
        type == QEvent::MouseButtonRelease) {
        syncTitle();
        update();
    }

    return handled;
}

bool ShortcutKeyRow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_titleLabel && m_adjustableDelay) {
        const QEvent::Type type = event->type();
        if (type == QEvent::Wheel && adjustDelayFromWheel(event)) {
            return true;
        }
        if (type == QEvent::Enter || type == QEvent::Leave) {
            m_delayTitleHovered = type == QEvent::Enter;
            syncDelayUnderline();
        } else if (type == QEvent::Resize || type == QEvent::FontChange) {
            syncDelayUnderline();
        }
    }

    if (watched == m_shortcutButton) {
        const QEvent::Type type = event->type();
        if (type == QEvent::Enter || type == QEvent::Leave || type == QEvent::MouseButtonPress ||
            type == QEvent::MouseButtonRelease) {
            syncTitle();
            update();
        }
    }

    return adqt::widgets::AdButton::eventFilter(watched, event);
}

QString ShortcutKeyRow::delayDisplayTitle() const {
    if (!m_adjustableDelay) {
        return m_baseTitle;
    }
    return m_baseTitle.contains(QStringLiteral("%1"))
               ? m_baseTitle.arg(m_delaySeconds)
               : tr("%1 (%2 s)").arg(m_baseTitle).arg(m_delaySeconds);
}

QString ShortcutKeyRow::titleLabelText() const {
    const QString title = delayDisplayTitle();
    return m_compactPresentation ? title + QStringLiteral(":") : title;
}

bool ShortcutKeyRow::adjustDelayFromWheel(QEvent* event) {
    auto* wheel = static_cast<QWheelEvent*>(event);
    const int delta = wheel->angleDelta().y() != 0 ? wheel->angleDelta().y()
                                                    : wheel->pixelDelta().y();
    if (delta == 0) {
        return false;
    }

    const int next = std::clamp(m_delaySeconds + (delta > 0 ? 1 : -1), 1, 10);
    if (next != m_delaySeconds && (!m_delaySetter || m_delaySetter(next))) {
        setDelaySeconds(next);
        emit delaySecondsChanged(next);
    }
    wheel->accept();
    return true;
}

void ShortcutKeyRow::syncDelayUnderline() {
    if (!m_adjustableDelay || m_titleLabel == nullptr || m_delayUnderline == nullptr) {
        return;
    }

    const QString secondsText = QString::number(m_delaySeconds);
    const QString displayTitle = m_titleLabel->text();
    const int delayTextStart = m_baseTitle.contains(QStringLiteral("%1"))
                                   ? m_baseTitle.indexOf(QStringLiteral("%1"))
                                   : displayTitle.lastIndexOf(secondsText);
    if (delayTextStart < 0 || secondsText.isEmpty()) {
        m_delayUnderline->hide();
        return;
    }

    const QFontMetrics metrics(m_titleLabel->font());
    const int underlineX = metrics.horizontalAdvance(displayTitle.left(delayTextStart));
    const int underlineWidth = std::max(1, metrics.horizontalAdvance(secondsText));
    m_delayUnderline->setGeometry(underlineX, m_titleLabel->height() - 2, underlineWidth, 2);

    const QColor highlightColor = m_colorScheme.map.presetColorHover.value(
        QStringLiteral("blue"), m_colorScheme.map.colorPrimaryHover);
    m_delayUnderline->setProperty("highlightColor", highlightColor);
    m_delayUnderline->setStyleSheet(
        QStringLiteral("background-color: %1;").arg(cssColor(highlightColor)));
    m_delayUnderline->setVisible(m_delayTitleHovered);
}

void ShortcutKeyRow::openShortcutConfigDialog() {
    QWidget* const hostWindow = window();
    auto* modal = new adqt::widgets::AdModal(this);
    auto* content = new ShortcutKeyConfigContent(m_registrationState.shortcuts, m_colorScheme,
                                                 m_maxShortcutCount, m_shortcutValidator,
                                                 m_validationScope);
    const QPointer<ShortcutKeyConfigContent> contentGuard(content);

    modal->setOwnerWindow(hostWindow);
    modal->setWindowTitle(tr("Key Configuration for \"%1\"")
                              .arg(m_titleLabel != nullptr ? m_titleLabel->text() : QString()));
    modal->setCentered(true);
    modal->setPreferredWidth(SHORTCUT_CONFIG_MODAL_WIDTH);
    modal->setCloseOnMaskClick(false);
    modal->setClosePolicy(adqt::widgets::AdModal::ClosePolicy::Manual);
    modal->setAcceptText(tr("OK"));
    modal->setRejectText(tr("Cancel"));
    modal->setStandardButtons(adqt::widgets::AdModal::StandardButton::Ok |
                              adqt::widgets::AdModal::StandardButton::Cancel);
    modal->setContentWidget(content);
    content->acceptanceAvailabilityChanged = [modal](bool available) {
        if (modal->acceptButton() != nullptr) {
            modal->acceptButton()->setEnabled(available);
        }
    };

    connect(modal, &adqt::widgets::AdModal::closeRequested, modal,
            [modal, contentGuard](adqt::widgets::AdModal::CloseReason reason) {
                if (reason != adqt::widgets::AdModal::CloseReason::OkAction) {
                    modal->reject();
                    return;
                }

                if (auto* contentPtr = contentGuard.data(); contentPtr != nullptr) {
                    if (!contentPtr->canAcceptDialog()) {
                        return;
                    }
                    contentPtr->commitPendingShortcut();
                    modal->accept();
                }
            });

    connect(modal, &adqt::widgets::AdModal::accepted, this, [this, contentGuard]() {
        auto* const contentPtr = contentGuard.data();
        if (contentPtr == nullptr) {
            return;
        }

        emit shortcutsChanged(contentPtr->selectedShortcuts());
    });
    connect(modal, &adqt::widgets::AdModal::finished, modal, &QObject::deleteLater);

    modal->open();
    if (modal->acceptButton() != nullptr) {
        modal->acceptButton()->setEnabled(content->canAcceptDialog());
    }

    QTimer::singleShot(0, content, [contentGuard]() {
        if (auto* contentPtr = contentGuard.data(); contentPtr != nullptr) {
            contentPtr->focusInitialControl();
        }
    });
}

void ShortcutKeyRow::syncTitle() {
    const auto& map = m_colorScheme.map;
    const bool shortcutButtonActive = isShortcutButtonActive();
    const QColor textColor =
        m_compactPresentation
            ? map.colorText
            : titleColorForShortcutRow(m_rowState, isDown() && !shortcutButtonActive,
                                       underMouse() && !shortcutButtonActive, map);

    syncTitleLabelColor(textColor);
    syncTitleIcon(textColor);
}

void ShortcutKeyRow::syncTitleLabelColor(const QColor& textColor) {
    if (m_titleLabel == nullptr) {
        return;
    }

    m_titleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(cssColor(textColor)));

    QFont labelFont = m_titleLabel->font();
    labelFont.setPixelSize(m_compactPresentation ? m_colorScheme.metricAlias.fontSize
                                                 : m_colorScheme.metricAlias.fontSizeLG);
    labelFont.setWeight(m_compactPresentation ? QFont::Normal : QFont::Medium);
    m_titleLabel->setFont(labelFont);
}

void ShortcutKeyRow::syncTitleIcon(const QColor& iconColor) {
    if (m_titleIcon == nullptr) {
        return;
    }

    const QPixmap iconPixmap = snow_shot::presentation::icons::renderTintedIconPixmap(
        m_titleIconRef, QSize(m_titleIconSize, m_titleIconSize), devicePixelRatioF(), iconColor);
    if (iconPixmap.isNull()) {
        m_titleIcon->clear();
        return;
    }

    m_titleIcon->setPixmap(iconPixmap);
}

void ShortcutKeyRow::syncRegistrationStatus() {
    const QString shortcutText = formatShortcutListDisplayText(m_registrationState.shortcuts);
    const auto status =
        m_showRegistrationStatus
            ? m_registrationState.status
            : (shortcutText.isEmpty()
                   ? snow_shot::presentation::GlobalShortcutStatus::Unset
                   : snow_shot::presentation::GlobalShortcutStatus::Registered);
    const QColor statusColor = registrationStatusColor(status, m_colorScheme.map);

    if (m_shortcutButton != nullptr) {
        auto* const button = static_cast<ShortcutKeyButton*>(m_shortcutButton);
        button->setText(m_compactPresentation
                            ? shortcutText
                            : (shortcutText.isEmpty() ? tr("Unset") : shortcutText));
        button->setRegistrationStatus(status);
        if (!m_showRegistrationStatus) {
            button->setAccentRole(shortcutText.isEmpty()
                                      ? adqt::widgets::AdButton::AccentRole::Danger
                                      : adqt::widgets::AdButton::AccentRole::Neutral);
        }
        button->setTheme(m_colorScheme);
    }

    QString statusName;
    switch (status) {
    case snow_shot::presentation::GlobalShortcutStatus::Registered:
        statusName = tr("Registered");
        break;
    case snow_shot::presentation::GlobalShortcutStatus::PartiallyRegistered:
        statusName = tr("Partially registered");
        break;
    case snow_shot::presentation::GlobalShortcutStatus::Failed:
        statusName = tr("Registration failed");
        break;
    case snow_shot::presentation::GlobalShortcutStatus::Unset:
        statusName = tr("Not configured");
        break;
    }

    const QString tooltipText = m_showRegistrationStatus ? registrationTooltipText() : QString();
    const bool showTooltip = !tooltipText.trimmed().isEmpty();
    if (m_shortcutButton != nullptr) {
        auto* const button = static_cast<ShortcutKeyButton*>(m_shortcutButton);
        button->setRegistrationStatusTooltipVisible(showTooltip);
        if (InfoTooltipIcon* const trigger = button->registrationStatusTooltipTrigger();
            trigger != nullptr) {
            trigger->setProperty("registrationStatus", static_cast<int>(status));
            trigger->setProperty("statusColor", statusColor);
            trigger->setAccessibleName(tr("Global shortcut status: %1").arg(statusName));
            trigger->setTooltipText(showTooltip ? tooltipText : QString());
        }
    }
}

QString ShortcutKeyRow::registrationTooltipText() const {
    if (m_registrationState.status == snow_shot::presentation::GlobalShortcutStatus::Registered ||
        m_registrationState.status == snow_shot::presentation::GlobalShortcutStatus::Unset) {
        return {};
    }

    QStringList registeredShortcuts;
    QStringList failedShortcuts;
    for (const snow_shot::presentation::GlobalShortcutBindingResult& binding :
         m_registrationState.bindings) {
        const QString displayShortcut = formatShortcutDisplayText(binding.shortcut);
        if (binding.registered) {
            registeredShortcuts.push_back(displayShortcut);
            continue;
        }

        QString reason;
        switch (binding.failureReason) {
        case snow_shot::presentation::GlobalShortcutFailureReason::AlreadyInUse:
            reason = tr("already used by another application or action");
            break;
        case snow_shot::presentation::GlobalShortcutFailureReason::InvalidShortcut:
            reason = tr("not supported as a Windows global shortcut");
            break;
        case snow_shot::presentation::GlobalShortcutFailureReason::UnsupportedPlatform:
            reason = tr("global shortcuts are not supported on this platform");
            break;
        case snow_shot::presentation::GlobalShortcutFailureReason::SystemError:
            reason = binding.nativeErrorCode == 0
                         ? tr("the system rejected this shortcut")
                         : tr("the system rejected this shortcut (error %1)")
                               .arg(binding.nativeErrorCode);
            break;
        case snow_shot::presentation::GlobalShortcutFailureReason::None:
            reason = tr("registration did not complete");
            break;
        }
        failedShortcuts.push_back(tr("%1: %2").arg(displayShortcut, reason));
    }

    if (m_registrationState.status ==
        snow_shot::presentation::GlobalShortcutStatus::PartiallyRegistered) {
        return tr("Some shortcuts are unavailable\nAvailable: %1\nUnavailable: %2")
            .arg(registeredShortcuts.join(QStringLiteral(", ")),
                 failedShortcuts.join(QStringLiteral("\n")));
    }

    return tr("No configured shortcut is available\n%1\nChange the shortcut and try again")
        .arg(failedShortcuts.join(QStringLiteral("\n")));
}

bool ShortcutKeyRow::isShortcutButtonActive() const {
    return m_shortcutButton != nullptr &&
           (m_shortcutButton->underMouse() || m_shortcutButton->isDown());
}
