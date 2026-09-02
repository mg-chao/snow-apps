#ifndef SNOW_SHOT_PRESENTATION_STYLES_MAINWINDOWCOMPONENTTOKEN_H
#define SNOW_SHOT_PRESENTATION_STYLES_MAINWINDOWCOMPONENTTOKEN_H

#include "snow_shot/presentation/styles/themecolorscheme.h"

namespace snow_shot::presentation::styles {
struct MainWindowComponentMetricToken {
    int cardRadius = 10;
};

MainWindowComponentMetricToken
buildMainWindowComponentMetricToken(const ThemeColorScheme& colorScheme);
} // namespace snow_shot::presentation::styles

#endif // SNOW_SHOT_PRESENTATION_STYLES_MAINWINDOWCOMPONENTTOKEN_H
