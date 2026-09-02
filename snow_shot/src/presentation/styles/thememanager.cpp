#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/settingsadapters.h"

#include "theme/theme_manager.h"
#include "theme/theme_types.h"

#include <QApplication>
#include <QStyleHints>
#include <QtGlobal>

namespace snow_shot::presentation::styles {
namespace {
ThemeMode decodeThemeMode(const QString& value) {
    const QString key = value.trimmed().toLower();
    if (key == QStringLiteral("light")) {
        return ThemeMode::Light;
    }
    if (key == QStringLiteral("dark")) {
        return ThemeMode::Dark;
    }
    return ThemeMode::FollowSystem;
}

QString encodeThemeMode(ThemeMode mode) {
    switch (mode) {
    case ThemeMode::Light:
        return QStringLiteral("light");
    case ThemeMode::Dark:
        return QStringLiteral("dark");
    case ThemeMode::FollowSystem:
    default:
        return QStringLiteral("system");
    }
}

ThemeAppearance systemThemeAppearance() {
    const QStyleHints* styleHints = QApplication::styleHints();
    if (styleHints != nullptr && styleHints->colorScheme() == Qt::ColorScheme::Dark) {
        return ThemeAppearance::Dark;
    }
    return ThemeAppearance::Light;
}

ThemeAppearance resolvedAppearance(ThemeMode mode) {
    switch (mode) {
    case ThemeMode::Dark:
        return ThemeAppearance::Dark;
    case ThemeMode::Light:
        return ThemeAppearance::Light;
    case ThemeMode::FollowSystem:
    default:
        return systemThemeAppearance();
    }
}

ThemeAppearance toThemeAppearance(adqt::theme::ThemeScheme scheme) {
    return scheme == adqt::theme::ThemeScheme::Dark ? ThemeAppearance::Dark
                                                    : ThemeAppearance::Light;
}

ThemePreset toThemePreset(adqt::theme::ThemeDensity density) {
    return density == adqt::theme::ThemeDensity::Compact ? ThemePreset::Compact
                                                         : ThemePreset::Default;
}

adqt::theme::ThemeScheme toAdqtScheme(ThemeAppearance appearance) {
    return appearance == ThemeAppearance::Dark ? adqt::theme::ThemeScheme::Dark
                                               : adqt::theme::ThemeScheme::Light;
}

adqt::theme::ThemeDensity toAdqtDensity(ThemePreset preset) {
    return preset == ThemePreset::Compact ? adqt::theme::ThemeDensity::Compact
                                          : adqt::theme::ThemeDensity::Comfortable;
}

ThemePresetColorMap normalizePresetColorMap(const ThemePresetColorMap& presetColors) {
    ThemePresetColorMap normalized;
    for (auto it = presetColors.cbegin(); it != presetColors.cend(); ++it) {
        normalized.insert(it.key().trimmed().toLower(), it.value());
    }
    return normalized;
}

QColor presetColorValue(const ThemePresetColorMap& normalizedPresetColors, const QString& key) {
    const auto it = normalizedPresetColors.constFind(key);
    return it != normalizedPresetColors.cend() ? it.value() : QColor();
}

void applyPresetColors(adqt::theme::ThemeConfig* target, const ThemePresetColorMap& presetColors) {
    if (target == nullptr) {
        return;
    }

    const ThemePresetColorMap normalizedPresetColors = normalizePresetColorMap(presetColors);

    const QColor blue = presetColorValue(normalizedPresetColors, QStringLiteral("blue"));
    const QColor purple = presetColorValue(normalizedPresetColors, QStringLiteral("purple"));
    const QColor cyan = presetColorValue(normalizedPresetColors, QStringLiteral("cyan"));
    const QColor green = presetColorValue(normalizedPresetColors, QStringLiteral("green"));
    const QColor magenta = presetColorValue(normalizedPresetColors, QStringLiteral("magenta"));
    const QColor pink = presetColorValue(normalizedPresetColors, QStringLiteral("pink"));
    const QColor red = presetColorValue(normalizedPresetColors, QStringLiteral("red"));
    const QColor orange = presetColorValue(normalizedPresetColors, QStringLiteral("orange"));
    const QColor yellow = presetColorValue(normalizedPresetColors, QStringLiteral("yellow"));
    const QColor volcano = presetColorValue(normalizedPresetColors, QStringLiteral("volcano"));
    const QColor geekblue = presetColorValue(normalizedPresetColors, QStringLiteral("geekblue"));
    const QColor gold = presetColorValue(normalizedPresetColors, QStringLiteral("gold"));
    const QColor lime = presetColorValue(normalizedPresetColors, QStringLiteral("lime"));

    if (blue.isValid()) {
        target->blue = blue;
    }
    if (purple.isValid()) {
        target->purple = purple;
    }
    if (cyan.isValid()) {
        target->cyan = cyan;
    }
    if (green.isValid()) {
        target->green = green;
    }
    if (magenta.isValid()) {
        target->magenta = magenta;
    }
    if (pink.isValid()) {
        target->pink = pink;
    }
    if (red.isValid()) {
        target->red = red;
    }
    if (orange.isValid()) {
        target->orange = orange;
    }
    if (yellow.isValid()) {
        target->yellow = yellow;
    }
    if (volcano.isValid()) {
        target->volcano = volcano;
    }
    if (geekblue.isValid()) {
        target->geekblue = geekblue;
    }
    if (gold.isValid()) {
        target->gold = gold;
    }
    if (lime.isValid()) {
        target->lime = lime;
    }
}

adqt::theme::ThemeConfig toAdqtThemeConfig(const ThemeStyleConfig& config) {
    adqt::theme::ThemeConfig adqtConfig = adqt::theme::defaultThemeConfig(
        toAdqtScheme(config.appearance), toAdqtDensity(config.preset));

    if (config.colorPrimary.isValid()) {
        adqtConfig.primary = config.colorPrimary;
    }
    if (config.colorSuccess.isValid()) {
        adqtConfig.success = config.colorSuccess;
    }
    if (config.colorWarning.isValid()) {
        adqtConfig.warning = config.colorWarning;
    }
    if (config.colorError.isValid()) {
        adqtConfig.error = config.colorError;
    }
    if (config.colorInfo.isValid()) {
        adqtConfig.info = config.colorInfo;
    }
    if (config.colorLink.isValid()) {
        adqtConfig.link = config.colorLink;
    }

    adqtConfig.fontSize = static_cast<double>(config.fontSize);
    adqtConfig.lineWidth = static_cast<double>(config.lineWidth);
    adqtConfig.borderRadius = static_cast<double>(config.borderRadius);
    adqtConfig.sizeUnit = static_cast<double>(config.sizeUnit);
    adqtConfig.sizeStep = static_cast<double>(config.sizeStep);
    adqtConfig.controlHeight = static_cast<double>(config.controlHeight);
    adqtConfig.motion = config.motionUnit > 0.0;

    applyPresetColors(&adqtConfig, config.presetColors);
    return adqtConfig;
}

ThemeStyleConfig toThemeStyleConfig(const adqt::theme::ResolvedTheme& resolvedTheme) {
    ThemeStyleConfig config;
    config.preset = toThemePreset(resolvedTheme.config.density);
    config.appearance = toThemeAppearance(resolvedTheme.config.scheme);
    config.colorPrimary = resolvedTheme.config.primary;
    config.colorSuccess = resolvedTheme.config.success;
    config.colorWarning = resolvedTheme.config.warning;
    config.colorError = resolvedTheme.config.error;
    config.colorInfo = resolvedTheme.config.info;
    config.colorLink = resolvedTheme.config.link;
    config.colorTextBase = resolvedTheme.values.colorTextBase;
    config.colorBgBase = resolvedTheme.values.colorBgBase;
    config.fontSize = qRound(resolvedTheme.config.fontSize);
    config.lineWidth = qRound(resolvedTheme.config.lineWidth);
    config.borderRadius = qRound(resolvedTheme.config.borderRadius);
    config.sizeUnit = qRound(resolvedTheme.config.sizeUnit);
    config.sizeStep = qRound(resolvedTheme.config.sizeStep);
    config.controlHeight = qRound(resolvedTheme.config.controlHeight);
    config.motionUnit = resolvedTheme.config.motion ? 0.1 : 0.0;
    return config;
}
} // namespace

ThemeManager::ThemeManager(QObject* parent)
    : QObject(parent), m_config(toThemeStyleConfig(adqt::theme::makeResolvedTheme(
                           adqt::theme::ThemeManager::instance().config()))),
      m_scheme(generateThemeColorScheme(m_config)),
      m_mode(decodeThemeMode(snow_shot::storage::InterfaceSettings().themeMode())) {
    auto& adqtThemeManager = adqt::theme::ThemeManager::instance();
    adqtThemeManager.setConfig(toAdqtThemeConfig(m_config));
    m_config = toThemeStyleConfig(adqt::theme::makeResolvedTheme(adqtThemeManager.config()));
    m_scheme = generateThemeColorScheme(m_config);
}

void ThemeManager::initialize(QApplication& application) {
    if (m_initialized) {
        return;
    }

    m_initialized = true;
    applyThemeMode();
    adqt::theme::ThemeManager::instance().applyTo(application);

    if (QStyleHints* styleHints = QApplication::styleHints(); styleHints != nullptr) {
        m_systemColorSchemeConnection =
            connect(styleHints, &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme) {
                if (m_mode == ThemeMode::FollowSystem) {
                    applyThemeMode();
                }
            });
    }
}

ThemeManager& ThemeManager::instance() {
    static ThemeManager manager;
    return manager;
}

ThemeMode ThemeManager::themeMode() const {
    return m_mode;
}

ThemeColorScheme ThemeManager::themeColorScheme() const {
    return m_scheme;
}

void ThemeManager::setThemeStyleConfig(const ThemeStyleConfig& config) {
    auto& adqtThemeManager = adqt::theme::ThemeManager::instance();
    adqtThemeManager.setConfig(toAdqtThemeConfig(config));
    m_config = toThemeStyleConfig(adqt::theme::makeResolvedTheme(adqtThemeManager.config()));
    rebuildScheme();
}

void ThemeManager::setThemeMode(ThemeMode mode) {
    if (m_mode == mode) {
        return;
    }

    m_mode = mode;
    snow_shot::storage::InterfaceSettings().setThemeMode(encodeThemeMode(m_mode));
    applyThemeMode();
    emit themeModeChanged(m_mode);
}

void ThemeManager::setThemeAppearance(ThemeAppearance appearance) {
    setThemeMode(appearance == ThemeAppearance::Dark ? ThemeMode::Dark : ThemeMode::Light);
}

void ThemeManager::setThemePreset(ThemePreset preset) {
    if (m_config.preset == preset) {
        return;
    }

    auto& adqtThemeManager = adqt::theme::ThemeManager::instance();
    adqtThemeManager.setDensity(toAdqtDensity(preset));
    m_config = toThemeStyleConfig(adqt::theme::makeResolvedTheme(adqtThemeManager.config()));
    rebuildScheme();
}

void ThemeManager::rebuildScheme() {
    m_scheme = generateThemeColorScheme(m_config);
    emit themeChanged(m_scheme);
}

void ThemeManager::applyThemeMode() {
    const ThemeAppearance appearance = resolvedAppearance(m_mode);
    if (m_config.appearance == appearance) {
        return;
    }

    auto& adqtThemeManager = adqt::theme::ThemeManager::instance();
    adqtThemeManager.setColorScheme(toAdqtScheme(appearance));
    m_config = toThemeStyleConfig(adqt::theme::makeResolvedTheme(adqtThemeManager.config()));
    rebuildScheme();
}
} // namespace snow_shot::presentation::styles
