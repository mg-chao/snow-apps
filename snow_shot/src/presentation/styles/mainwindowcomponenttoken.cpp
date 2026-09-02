#include "snow_shot/presentation/styles/mainwindowcomponenttoken.h"

namespace snow_shot::presentation::styles {
MainWindowComponentMetricToken
buildMainWindowComponentMetricToken(const ThemeColorScheme& colorScheme) {
    const ThemeMetricMapToken& metricMap = colorScheme.metricMap;

    MainWindowComponentMetricToken token;
    token.cardRadius = metricMap.radius.borderRadiusLG + metricMap.radius.borderRadiusXS;
    return token;
}
} // namespace snow_shot::presentation::styles
