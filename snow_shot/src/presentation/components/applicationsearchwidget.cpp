#include "snow_shot/presentation/components/applicationsearchwidget.h"

#include "snow_shot/presentation/styles/themecolorscheme.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include "antd_icons.h"
#include "icons/widget_icons.h"
#include "widgets/select.h"

#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QPainter>
#include <QPixmap>
#include <QScopedValueRollback>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVariant>

#include <algorithm>
#include <cmath>

namespace {
namespace outlined_icons = adqt::icons::antd::outlined;

constexpr int kDescriptionRole = Qt::UserRole + 101;
constexpr int kCategoryRole = Qt::UserRole + 102;
constexpr auto kScreenshotDelayKey = "screenshot/delay_seconds";

snow_shot::presentation::settings::SettingsSearchRuntimeValues searchRuntimeValues() {
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    if (!storage.isInitialized()) {
        static_cast<void>(storage.initialize());
    }
    return {snow_shot::storage::ScreenshotSettings().delaySeconds()};
}

QString metadataRoleKey(int role) {
    return QStringLiteral("__role_%1").arg(role);
}

QColor colorOnBackground(const QColor& foreground, const QColor& background) {
    if (!foreground.isValid()) {
        return background;
    }
    if (!background.isValid() || foreground.alpha() >= 255) {
        return foreground;
    }
    const qreal alpha = foreground.alphaF();
    return QColor::fromRgbF(foreground.redF() * alpha + background.redF() * (1.0 - alpha),
                            foreground.greenF() * alpha + background.greenF() * (1.0 - alpha),
                            foreground.blueF() * alpha + background.blueF() * (1.0 - alpha));
}

class SearchResultItemDelegate final : public QStyledItemDelegate {
  public:
    explicit SearchResultItemDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
        m_popupBackground = scheme.map.colorBgElevated;
        m_hoverBackground = scheme.map.colorFillTertiary;
        m_activeBackground = scheme.map.colorPrimaryBg;
        m_titleColor = scheme.map.colorText;
        m_descriptionColor = scheme.map.colorTextTertiary;
        m_categoryColor = scheme.map.colorTextSecondary;
        m_horizontalPadding = scheme.metricAlias.paddingSM;
        m_verticalPadding = scheme.metricAlias.paddingXS;
        m_columnGap = scheme.metricAlias.marginSM;
        m_radius = scheme.metricAlias.borderRadiusSM;
        m_titleFontSize = scheme.metricAlias.fontSize;
        m_supportingFontSize = scheme.metricAlias.fontSizeSM;
        m_rowHeight = std::max(scheme.metricAlias.controlHeightLG +
                                   2 * scheme.metricAlias.paddingXS,
                               52);

        // Keep the empty row aligned with Ant Design's Select small empty state.
        m_emptyTextColor = scheme.map.colorTextTertiary;
        m_emptyBorderColor = colorOnBackground(scheme.map.colorFill, m_popupBackground);
        m_emptyShadowColor =
            colorOnBackground(scheme.map.colorFillTertiary, m_popupBackground);
        m_emptyContentColor =
            colorOnBackground(scheme.map.colorFillQuaternary, m_popupBackground);
        m_emptyMarginBlock = std::max(4, scheme.metricAlias.paddingXS);
        m_emptyMarginInline = std::max(4, scheme.metricAlias.paddingXS);
        m_emptyImageMarginBottom = std::max(4, scheme.metricAlias.paddingXS);
        m_emptyIconHeight = std::max(
            20, static_cast<int>(std::lround(scheme.metricAlias.controlHeightLG * 0.875)));
        m_emptyIconWidth = std::max(
            30, static_cast<int>(std::lround(m_emptyIconHeight * (64.0 / 41.0))));
        m_emptyDescriptionFontSize = std::max(12, scheme.metricAlias.fontSize);
        m_emptyDescriptionLineHeight = std::max(
            m_emptyDescriptionFontSize + 2,
            static_cast<int>(std::lround(m_emptyDescriptionFontSize *
                                          scheme.metricMap.font.lineHeight)));
        const int optionTextHeight = std::max(
            1, static_cast<int>(std::lround(m_emptyDescriptionFontSize *
                                            scheme.metricMap.font.lineHeight)));
        m_optionPaddingVertical = std::max(0, (m_rowHeight - optionTextHeight + 1) / 2);
        m_emptyStateHeight = std::max(
            m_rowHeight,
            m_optionPaddingVertical * 2 + m_emptyMarginBlock * 2 + m_emptyIconHeight +
                m_emptyImageMarginBottom + m_emptyDescriptionLineHeight);
    }

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override {
        if (!index.data(adqt::widgets::AdSelect::DefaultLabelRole).toString().isEmpty()) {
            return QSize(option.rect.width(), m_rowHeight);
        }
        return QSize(option.rect.width(), m_emptyStateHeight);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        if (painter == nullptr || !index.isValid()) {
            return;
        }

        const QString title =
            index.data(adqt::widgets::AdSelect::DefaultLabelRole).toString();
        if (title.isEmpty()) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->fillRect(option.rect, m_popupBackground);

            QRect contentRect = option.rect.adjusted(m_horizontalPadding, m_optionPaddingVertical,
                                                      -m_horizontalPadding,
                                                      -m_optionPaddingVertical);
            contentRect.adjust(m_emptyMarginInline, 0, -m_emptyMarginInline, 0);
            const int top = contentRect.top() + m_emptyMarginBlock;
            const QRectF iconRect(contentRect.left() +
                                      (contentRect.width() - m_emptyIconWidth) / 2.0,
                                  top, m_emptyIconWidth, m_emptyIconHeight);
            const auto emptyIconColors = adqt::icons::IconColors::threeTone(
                m_emptyBorderColor, m_emptyContentColor, m_emptyShadowColor);
            const auto emptyIcon = adqt::widgets::icons::twotone::EmptySimple(emptyIconColors);
            adqt::icons::IconRenderRequest request;
            request.logicalSize = QSize(m_emptyIconWidth, m_emptyIconHeight);
            request.devicePixelRatio = option.widget != nullptr
                                           ? option.widget->devicePixelRatioF()
                                           : 1.0;
            const QPixmap iconPixmap = adqt::icons::renderIconPixmap(emptyIcon, request);
            if (!iconPixmap.isNull()) {
                painter->drawPixmap(iconRect.topLeft(), iconPixmap);
            }

            QFont emptyFont = option.font;
            emptyFont.setPixelSize(m_emptyDescriptionFontSize);
            emptyFont.setWeight(QFont::Normal);
            painter->setFont(emptyFont);
            painter->setPen(m_emptyTextColor);
            const QFontMetrics emptyMetrics(emptyFont);
            const QString emptyText = emptyMetrics.elidedText(
                index.data(Qt::DisplayRole).toString(), Qt::ElideRight,
                std::max(0, contentRect.width()));
            const QRect textRect(contentRect.left(), top + m_emptyIconHeight +
                                                           m_emptyImageMarginBottom,
                                 contentRect.width(), m_emptyDescriptionLineHeight);
            painter->drawText(textRect, Qt::AlignHCenter | Qt::AlignTop, emptyText);
            painter->restore();
            return;
        }

        const QString description = index.data(kDescriptionRole).toString();
        const QString category = index.data(kCategoryRole).toString();
        const bool active = (option.state & QStyle::State_Selected) != 0;
        const bool hovered = (option.state & QStyle::State_MouseOver) != 0;

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->fillRect(option.rect, m_popupBackground);

        const QRect backgroundRect = option.rect.adjusted(4, 2, -4, -2);
        if (active || hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(active ? m_activeBackground : m_hoverBackground);
            painter->drawRoundedRect(backgroundRect, m_radius, m_radius);
        }

        const QRect contentRect =
            backgroundRect.adjusted(m_horizontalPadding, m_verticalPadding,
                                    -m_horizontalPadding, -m_verticalPadding);
        const int categoryWidth = category.isEmpty()
                                      ? 0
                                      : std::clamp(contentRect.width() * 2 / 5, 96, 180);
        const int leftWidth =
            std::max(0, contentRect.width() - categoryWidth -
                            (categoryWidth > 0 ? m_columnGap : 0));
        const QRect leftRect(contentRect.left(), contentRect.top(), leftWidth,
                             contentRect.height());
        const QRect categoryRect(contentRect.right() - categoryWidth + 1, contentRect.top(),
                                 categoryWidth, contentRect.height());

        QFont titleFont = option.font;
        titleFont.setPixelSize(m_titleFontSize);
        titleFont.setWeight(QFont::DemiBold);
        painter->setFont(titleFont);
        painter->setPen(m_titleColor);
        const QFontMetrics titleMetrics(titleFont);
        const int titleHeight = titleMetrics.height();
        painter->drawText(
            QRect(leftRect.left(), leftRect.top(), leftRect.width(), titleHeight),
            Qt::AlignLeft | Qt::AlignVCenter,
            titleMetrics.elidedText(title, Qt::ElideRight, leftRect.width()));

        QFont supportingFont = option.font;
        supportingFont.setPixelSize(m_supportingFontSize);
        supportingFont.setWeight(QFont::Normal);
        painter->setFont(supportingFont);
        const QFontMetrics supportingMetrics(supportingFont);
        const int descriptionTop = leftRect.bottom() - supportingMetrics.height() + 1;
        painter->setPen(m_descriptionColor);
        painter->drawText(
            QRect(leftRect.left(), descriptionTop, leftRect.width(), supportingMetrics.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            supportingMetrics.elidedText(description, Qt::ElideRight, leftRect.width()));

        if (categoryWidth > 0) {
            painter->setPen(m_categoryColor);
            painter->drawText(
                categoryRect, Qt::AlignRight | Qt::AlignVCenter,
                supportingMetrics.elidedText(category, Qt::ElideLeft, categoryRect.width()));
        }
        painter->restore();
    }

  private:
    QColor m_popupBackground;
    QColor m_hoverBackground;
    QColor m_activeBackground;
    QColor m_titleColor;
    QColor m_descriptionColor;
    QColor m_categoryColor;
    int m_horizontalPadding = 12;
    int m_verticalPadding = 8;
    int m_columnGap = 12;
    int m_radius = 4;
    int m_titleFontSize = 14;
    int m_supportingFontSize = 12;
    int m_rowHeight = 56;
    QColor m_emptyTextColor;
    QColor m_emptyBorderColor;
    QColor m_emptyShadowColor;
    QColor m_emptyContentColor;
    int m_emptyMarginBlock = 8;
    int m_emptyMarginInline = 8;
    int m_emptyImageMarginBottom = 8;
    int m_emptyIconWidth = 55;
    int m_emptyIconHeight = 35;
    int m_emptyDescriptionFontSize = 14;
    int m_emptyDescriptionLineHeight = 22;
    int m_optionPaddingVertical = 17;
    int m_emptyStateHeight = 115;
};
} // namespace

ApplicationSearchWidget::ApplicationSearchWidget(
    const snow_shot::presentation::settings::SettingsRegistry& registry,
    const snow_shot::presentation::styles::ThemeAliasMetricToken& metric, QWidget* parent)
    : QWidget(parent), m_index(registry, searchRuntimeValues()),
      m_select(new adqt::widgets::AdSelect(this)) {
    auto* rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    m_select->setMode(adqt::widgets::AdSelect::Mode::Single);
    m_select->setVariant(adqt::widgets::AdSelect::Variant::Filled);
    m_select->setControlSize(adqt::widgets::AdSelect::ControlSize::Middle);
    m_select->setSearchEnabled(true);
    m_select->setSearchPolicy(adqt::widgets::AdSelect::SearchPolicy::External);
    m_select->setAllowClear(true);
    m_select->setAutoClearSearchValue(true);
    m_select->setPopupMatchSelectWidth(true);
    m_select->setPlacement(adqt::widgets::AdSelect::Placement::BottomCenter);
    m_select->setPrefixIconRef(outlined_icons::Search());
    m_select->setSearchRoles({
        adqt::widgets::AdSelect::DefaultLabelRole,
        adqt::widgets::AdSelect::DefaultValueRole,
        kDescriptionRole,
        kCategoryRole,
    });
    m_resultDelegate = new SearchResultItemDelegate(m_select);
    m_select->setItemDelegate(m_resultDelegate);
    m_select->setMinimumHeight(metric.controlHeight);
    m_select->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    rootLayout->addWidget(m_select, 1);

    connect(m_select, &adqt::widgets::AdSelect::searchTextChanged, this,
            [this](const QString& rawText) {
                if (m_clearingSelection) {
                    return;
                }
                populateResults(rawText);
            });

    connect(m_select, &adqt::widgets::AdSelect::selected, this,
            &ApplicationSearchWidget::handleSelectedValue);
    auto& applicationStorage = snow_shot::storage::ApplicationStorage::instance();
    if (!applicationStorage.isInitialized()) {
        static_cast<void>(applicationStorage.initialize());
    }
    auto& configuration = applicationStorage.configuration();
    connect(&configuration, &snow_shot::storage::ConfigurationStore::valueChanged, this,
            [this](const QString& key, const QJsonValue& value) {
                if (key != QLatin1String(kScreenshotDelayKey)) {
                    return;
                }
                m_index.setRuntimeValues({value.toInt()});
                populateResults(query());
            });
    populateResults(QString());
}

ApplicationSearchWidget::~ApplicationSearchWidget() = default;

void ApplicationSearchWidget::setPlaceholderText(const QString& text) {
    if (m_select != nullptr) {
        m_select->setPlaceholder(text);
    }
}

QString ApplicationSearchWidget::query() const {
    return m_select != nullptr ? m_select->searchText() : QString();
}

void ApplicationSearchWidget::applyTheme(
    const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
    if (m_select == nullptr) {
        return;
    }

    adqt::widgets::AdSelect::ComponentTokens tokens;
    tokens.metrics.controlHeight = scheme.metricAlias.controlHeight;
    tokens.metrics.borderRadius = scheme.metricAlias.borderRadius;
    tokens.metrics.borderWidth = std::max(1, scheme.metricAlias.lineWidth);
    tokens.metrics.horizontalPadding = scheme.metricAlias.controlPaddingHorizontal;
    tokens.metrics.popupMaxHeight = 420;
    tokens.metrics.optionHeight =
        std::max(scheme.metricAlias.controlHeightLG + 2 * scheme.metricAlias.paddingXS, 52);
    tokens.metrics.selectorFontSize = scheme.metricAlias.fontSize;
    tokens.metrics.optionFontSize = scheme.metricAlias.fontSize;
    tokens.metrics.iconSize = scheme.metricAlias.fontSizeIcon;

    tokens.colors.selectorBackground = scheme.map.colorFillTertiary;
    tokens.colors.selectorBorder = Qt::transparent;
    tokens.colors.selectorHoverBorder = Qt::transparent;
    tokens.colors.selectorActiveBorder = scheme.map.colorPrimary;
    tokens.colors.selectorText = scheme.map.colorText;
    tokens.colors.placeholderText = scheme.map.colorTextTertiary;
    tokens.colors.popupBackground = scheme.map.colorBgElevated;
    tokens.colors.popupBorder = scheme.map.colorBorderSecondary;
    tokens.colors.optionText = scheme.map.colorText;
    tokens.colors.optionHoverBackground = scheme.map.colorFillTertiary;
    tokens.colors.optionSelectedBackground = scheme.map.colorPrimaryBg;
    tokens.colors.optionSelectedText = scheme.map.colorText;
    tokens.colors.clear = scheme.map.colorTextQuaternary;
    tokens.colors.prefix = scheme.map.colorTextTertiary;
    tokens.colors.suffix = scheme.map.colorTextQuaternary;
    m_select->setComponentTokens(tokens);

    static_cast<SearchResultItemDelegate*>(m_resultDelegate)->applyTheme(scheme);

    m_select->setSemanticStyleResolver(
        [scheme](const adqt::widgets::AdSelect::StyleContext& context) {
            adqt::widgets::AdSelect::SemanticStyles styles;
            styles.root.backgroundColor = Qt::transparent;
            styles.selector.backgroundColor =
                context.popupVisible ? scheme.map.colorBgContainer : scheme.map.colorFillTertiary;
            styles.selector.borderColor =
                context.popupVisible ? scheme.map.colorPrimary : QColor(Qt::transparent);
            styles.placeholder.textColor = scheme.map.colorTextTertiary;
            styles.popup.backgroundColor = scheme.map.colorBgElevated;
            styles.popup.borderColor = scheme.map.colorBorderSecondary;
            styles.option.textColor = scheme.map.colorText;
            styles.optionHover.backgroundColor = scheme.map.colorFillTertiary;
            styles.optionSelected.backgroundColor = scheme.map.colorPrimaryBg;
            styles.optionSelected.textColor = scheme.map.colorText;
            styles.prefix.textColor = scheme.map.colorTextTertiary;
            styles.suffix.textColor = scheme.map.colorTextQuaternary;
            return styles;
        });

    QPalette selectPalette = m_select->palette();
    selectPalette.setColor(QPalette::Window, Qt::transparent);
    selectPalette.setColor(QPalette::Base, scheme.map.colorFillTertiary);
    selectPalette.setColor(QPalette::Text, scheme.map.colorText);
    selectPalette.setColor(QPalette::PlaceholderText, scheme.map.colorTextTertiary);
    m_select->setPalette(selectPalette);
    m_select->update();
}

void ApplicationSearchWidget::rebuildIndex() {
    m_index.rebuild();
    populateResults(query());
}

void ApplicationSearchWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        rebuildIndex();
    }
}

void ApplicationSearchWidget::populateResults(const QString& queryText) {
    if (m_select == nullptr) {
        return;
    }

    QVector<snow_shot::presentation::settings::SettingsSearchEntry> results =
        m_index.search(queryText);
    if (queryText.trimmed().isEmpty()) {
        results.erase(
            std::remove_if(
                results.begin(), results.end(),
                [](const auto& entry) {
                    return entry.kind != snow_shot::presentation::settings::
                                             SettingsSearchNodeKind::Page;
                }),
            results.end());
    }
    QVector<adqt::widgets::AdSelect::Option> options;
    options.reserve(results.size());
    m_locationsByValue.clear();

    for (const auto& entry : results) {
        if (entry.id.isEmpty()) {
            continue;
        }

        adqt::widgets::AdSelect::Option option;
        option.value = entry.id;
        option.label = entry.title;
        option.metadata.insert(metadataRoleKey(kDescriptionRole), entry.description);
        option.metadata.insert(metadataRoleKey(kCategoryRole), entry.path);
        option.metadata.insert(QStringLiteral("description"), entry.description);
        option.metadata.insert(QStringLiteral("entryId"), entry.id);
        options.push_back(option);
        m_locationsByValue.insert(entry.id, entry.location);
    }

    const QScopedValueRollback<bool> clearingGuard(m_clearingSelection, true);
    m_select->setOptions(options);
    m_select->setCurrentValue(QVariant());
}

void ApplicationSearchWidget::handleSelectedValue(const QVariant& value, const QString& label) {
    Q_UNUSED(label)
    if (m_select == nullptr || m_clearingSelection) {
        return;
    }

    const QString valueKey = value.toString();
    const auto location = m_locationsByValue.value(valueKey);
    if (location.isEmpty()) {
        return;
    }

    emit locationActivated(location);

    QTimer::singleShot(0, this, [this]() {
        if (m_select == nullptr) {
            return;
        }

        const QScopedValueRollback<bool> clearingGuard(m_clearingSelection, true);
        m_select->setCurrentValue(QVariant());
        m_select->setSearchText(QString());
        populateResults(QString());
    });
}
