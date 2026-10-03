#include "snow_shot/presentation/components/applicationsearchwidget.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"

#include "widgets/select.h"

#include <QAbstractItemDelegate>
#include <QApplication>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QStandardItemModel>
#include <QStringList>
#include <QStyleOptionViewItem>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

const snow_shot::presentation::settings::SettingsRegistry& registry() {
    return snow_shot::presentation::settings::builtInSettingsRegistry();
}

void searchTagsUseSpaceLeftByShortTitles() {
    using snow_shot::presentation::styles::ThemeAppearance;
    auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    ApplicationSearchWidget search(registry(), themeManager.themeColorScheme().metricAlias);
    auto* select = search.findChild<adqt::widgets::AdSelect*>();
    require(select != nullptr && select->itemDelegate() != nullptr,
            "global search should expose its result renderer");
    select->setFont(QFont(QStringLiteral("Snow Recording Test Sans")));

    QStandardItemModel model(1, 1);
    const QModelIndex index = model.index(0, 0);
    const QStringList titles{QStringLiteral("DirectML"), QStringLiteral("\u6309\u94ae")};
    const QStringList categories{
        QStringLiteral("Recognition & translation / OCR models & performance"),
        QStringLiteral("\u9f20\u6807\u5de6\u952e / "
                       "\u6c49\u5b57\u6309\u94ae\u6d4b\u8bd5\u9f20\u6807\u5de6\u952e")};
    bool coversRoundedDownAdvance = false;

    for (const auto appearance : {ThemeAppearance::Light, ThemeAppearance::Dark}) {
        themeManager.setThemeAppearance(appearance);
        for (const int fontSize : {14, 20}) {
            auto scheme = themeManager.themeColorScheme();
            scheme.metricAlias.fontSize = fontSize;
            scheme.metricAlias.fontSizeSM = fontSize - 2;
            search.applyTheme(scheme);
            QFont titleFont = select->font();
            titleFont.setPixelSize(fontSize);
            titleFont.setWeight(QFont::DemiBold);
            QFont supportingFont = select->font();
            supportingFont.setPixelSize(fontSize - 2);
            supportingFont.setWeight(QFont::Normal);
            const QFontMetrics titleMetrics(titleFont);
            const QFontMetrics supportingMetrics(supportingFont);
            const int sidePadding = 4 + scheme.metricAlias.paddingSM;
            const int gap = scheme.metricAlias.marginSM;

            const auto render = [&](int width) {
                QStyleOptionViewItem option;
                option.initFrom(select);
                option.state = QStyle::State_Enabled;
                option.font = select->font();
                option.rect = QRect(0, 0, width, 56);
                option.rect.setHeight(select->itemDelegate()->sizeHint(option, index).height());
                QImage image(option.rect.size(), QImage::Format_ARGB32_Premultiplied);
                image.fill(Qt::transparent);
                QPainter painter(&image);
                select->itemDelegate()->paint(&painter, option, index);
                return image;
            };

            for (qsizetype fixture = 0; fixture < titles.size(); ++fixture) {
                const QString& title = titles.at(fixture);
                const QString& category = categories.at(fixture);
                model.setData(index, title, adqt::widgets::AdSelect::DefaultLabelRole);
                model.setData(index, category, Qt::UserRole + 102);
                const int titleWidth =
                    static_cast<int>(std::ceil(QFontMetricsF(titleFont).horizontalAdvance(title)));
                const int tagWidth =
                    static_cast<int>(
                        std::ceil(QFontMetricsF(supportingFont).horizontalAdvance(category))) +
                    12;
                coversRoundedDownAdvance =
                    coversRoundedDownAdvance ||
                    titleMetrics.horizontalAdvance(title) < titleWidth ||
                    supportingMetrics.horizontalAdvance(category) < tagWidth - 12;
                require(titleMetrics.elidedText(title, Qt::ElideRight, titleWidth) == title &&
                            supportingMetrics.elidedText(category, Qt::ElideLeft, tagWidth - 12) ==
                                category,
                        "the measured reference widths should fit both complete texts");
                require(tagWidth > titleWidth + gap,
                        "the regression fixture should need more than half the line for its tag");
                const int fittingWidth = 2 * sidePadding + titleWidth + gap + tagWidth;
                const int referenceWidth = 2 * sidePadding + 2 * tagWidth + gap;
                const int topLineHeight =
                    2 + scheme.metricAlias.paddingXS +
                    std::max(titleMetrics.height(), supportingMetrics.height() + 4);
                const auto tagImage = [&](const QImage& image) {
                    return image.copy(image.width() - sidePadding - tagWidth, 0, tagWidth,
                                      topLineHeight);
                };

                for (const int width : {fittingWidth, fittingWidth + 1, fittingWidth + 40}) {
                    for (int kind = 0; kind < 3; ++kind) {
                        model.setData(index, kind, Qt::UserRole + 103);
                        const QImage fullWidthReference = render(referenceWidth);
                        const QImage fitted = render(width);
                        require(tagImage(fitted) == tagImage(fullWidthReference),
                                "a full category tag should render whenever both texts fit");
                        model.setData(index, QString(), Qt::UserRole + 102);
                        const QImage withoutTag = render(width);
                        require(fitted.copy(0, 0, sidePadding + titleWidth, topLineHeight) ==
                                    withoutTag.copy(0, 0, sidePadding + titleWidth, topLineHeight),
                                "expanding a category tag should preserve a fitting title");
                        model.setData(index, category, Qt::UserRole + 102);
                    }
                }

                model.setData(index, 0, Qt::UserRole + 103);
                const QImage fullWidthReference = render(referenceWidth);
                const QImage constrained = render(fittingWidth - 20);
                require(tagImage(constrained) != tagImage(fullWidthReference),
                        "category tags should still elide when the full texts cannot fit");
                const QRect titleRect(0, 0, sidePadding + titleWidth, topLineHeight);
                require(constrained.copy(titleRect) == fullWidthReference.copy(titleRect),
                        "a constrained category tag should leave a short title intact");

                model.setData(index, title + QStringLiteral(" / ") + category,
                              adqt::widgets::AdSelect::DefaultLabelRole);
                // Fonts without an ellipsis glyph render three periods instead.
                // Reserve enough space for the prefix and either representation.
                const int prefixWidth =
                    static_cast<int>(std::ceil(QFontMetricsF(titleFont).horizontalAdvance(
                        title + QStringLiteral(" / ...")))) +
                    gap;
                const int narrowWidth = 2 * sidePadding + 2 * prefixWidth + gap;
                const QImage longTitle = render(narrowWidth);
                require(longTitle.copy(titleRect) == fullWidthReference.copy(titleRect),
                        "long titles should retain a readable prefix beside long category tags");
            }
        }
    }
    require(coversRoundedDownAdvance,
            "the font fixtures should exercise advances that integer metrics round down");
    themeManager.setThemeAppearance(ThemeAppearance::Light);
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("application_search_tests"));

    QTemporaryDir storageDirectory;
    require(storageDirectory.isValid(), "temporary storage directory should be available");
    require(snow_shot::storage::ApplicationStorage::instance()
                .initialize({storageDirectory.path(), storageDirectory.path(), 8000})
                .success,
            "search test storage should initialize");
    // Use the repository's original font fixtures so offscreen rendering has
    // real Latin and Han glyphs without depending on system language packs.
    for (const auto* name : {"SnowRecordingTestSans-Regular.ttf", "SnowRecordingTestSans-Bold.ttf",
                             "SnowRecordingTestHan-Regular.ttf", "SnowRecordingTestHan-Bold.ttf"}) {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/recording-test-fonts/") +
                                                  QString::fromLatin1(name)) >= 0,
                "search test fonts should load");
    }
    QFontDatabase::setApplicationFallbackFontFamilies(QChar::Script_Han,
                                                      {QStringLiteral("Snow Recording Test Han")});
    snow_shot::presentation::styles::ThemeManager::instance().initialize(application);

    searchTagsUseSpaceLeftByShortTitles();

    snow_shot::storage::ApplicationStorage::instance().shutdown();
    return 0;
}
