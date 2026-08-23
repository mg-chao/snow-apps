#include "icon_theme_adapter.h"

#include "antd_icons.h"
#include "theme/theme_manager.h"

#include <QColor>

namespace demo {

namespace {

adqt::icons::IconPalette buildSnapshot() {
  const adqt::theme::ThemeManager& manager = adqt::theme::ThemeManager::instance();
  const adqt::theme::AdThemePalette& colors = manager.theme().palette;

  adqt::icons::IconPalette snapshot;
  snapshot.text = QColor(colors.colorText);
  snapshot.textDisabled = QColor(colors.colorTextQuaternary);
  snapshot.primary = QColor(colors.colorPrimary);
  snapshot.twoToneSecondary = QColor(colors.colorPrimaryBg);
  snapshot.tertiary = QColor(colors.colorPrimaryBorder);
  snapshot.revision = manager.themeRevision();
  return snapshot;
}

}  // namespace

void installIconThemeResolver() {
  adqt::icons::setPaletteResolver([]() { return buildSnapshot(); });
}

}  // namespace demo
