#include "snow_shot/presentation/styles/mainwindowcomponenttoken.h"

#include "theme/theme_color_utils.h"
#include "theme/theme_manager.h"

namespace snow_shot::presentation::styles {
MainWindowComponentMetricToken
buildMainWindowComponentMetricToken(const ThemeColorScheme& colorScheme) {
    const ThemeMetricMapToken& metricMap = colorScheme.metricMap;

    MainWindowComponentMetricToken token;
    token.cardRadius = metricMap.radius.borderRadiusLG + metricMap.radius.borderRadiusXS;
    return token;
}

qreal mainWindowBackgroundOpacity(const QWidget* widget) {
    return adqt::theme::ThemeManager::instance().backgroundOpacity(widget);
}

QColor mainWindowBackgroundColor(const QWidget* widget, const QColor& color) {
    return adqt::theme::applyBackgroundOpacity(color, mainWindowBackgroundOpacity(widget));
}
} // namespace snow_shot::presentation::styles
