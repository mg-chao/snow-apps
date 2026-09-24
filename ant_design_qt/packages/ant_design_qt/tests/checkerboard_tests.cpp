#include "theme/theme_manager.h"
#include "widgets/checkerboard.h"

#include <QApplication>
#include <QPainter>
#include <QWidget>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);

  const QImage custom = adqt::widgets::checkerboardTile(6, QColor(Qt::white), QColor(0, 0, 0, 15));
  require(custom.size() == QSize(12, 12) && custom.pixelColor(1, 1).alpha() == 255 &&
              custom.pixelColor(1, 1) == custom.pixelColor(7, 7) &&
              custom.pixelColor(1, 1) != custom.pixelColor(7, 1),
          "checkerboard cells must alternate and remain opaque");

  auto& manager = adqt::theme::ThemeManager::instance();
  QWidget widget;
  manager.setColorScheme(adqt::theme::ThemeScheme::Light);
  const QImage light = adqt::widgets::themedCheckerboardTile(&widget);
  manager.setColorScheme(adqt::theme::ThemeScheme::Dark);
  const QImage dark = adqt::widgets::themedCheckerboardTile(&widget);
  require(light.pixelColor(1, 1) != light.pixelColor(7, 1) &&
              dark.pixelColor(1, 1) != dark.pixelColor(7, 1),
          "both color schemes must show alternating cells");
  require(dark.pixelColor(1, 1).lightness() < light.pixelColor(1, 1).lightness() &&
              dark.pixelColor(7, 1).lightness() < light.pixelColor(7, 1).lightness(),
          "dark checkerboard cells must be darker than light cells");

  manager.setColorScheme(adqt::theme::ThemeScheme::Light);
  adqt::theme::ThemeOverride darkScope;
  darkScope.scheme = adqt::theme::ThemeScheme::Dark;
  manager.setScopeOverride(&widget, darkScope);
  require(adqt::widgets::themedCheckerboardTile(&widget) == dark,
          "a scoped dark theme must update the checkerboard without changing the global theme");
  manager.clearScopeOverride(&widget);
  require(adqt::widgets::themedCheckerboardTile(&widget) == light,
          "clearing a scoped theme must restore the light checkerboard");

  manager.setColorScheme(adqt::theme::ThemeScheme::Dark);

  QImage rendered(12, 12, QImage::Format_ARGB32_Premultiplied);
  rendered.fill(Qt::transparent);
  QPainter painter(&rendered);
  painter.fillRect(rendered.rect(), adqt::widgets::themedCheckerboardBrush(&widget));
  painter.end();
  require(rendered == dark, "the themed brush and tile must render the same pattern");
  return 0;
}
