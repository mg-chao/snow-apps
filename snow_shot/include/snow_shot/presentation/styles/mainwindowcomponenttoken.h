#ifndef SNOW_SHOT_PRESENTATION_STYLES_MAINWINDOWCOMPONENTTOKEN_H
#define SNOW_SHOT_PRESENTATION_STYLES_MAINWINDOWCOMPONENTTOKEN_H

#include "snow_shot/presentation/styles/themecolorscheme.h"

class QWidget;

namespace snow_shot::presentation::styles {
struct MainWindowComponentMetricToken {
    int cardRadius = 10;
};

MainWindowComponentMetricToken
buildMainWindowComponentMetricToken(const ThemeColorScheme& colorScheme);

// Custom surfaces use the same inherited opacity as Ant Design controls.
qreal mainWindowBackgroundOpacity(const QWidget* widget);
QColor mainWindowBackgroundColor(const QWidget* widget, const QColor& color);
} // namespace snow_shot::presentation::styles

#endif // SNOW_SHOT_PRESENTATION_STYLES_MAINWINDOWCOMPONENTTOKEN_H
