#include "ui/icon_theme_adapter.h"

#include "antd_icons.h"
#include "theme/theme_manager.h"

#include <QColor>

namespace snow::image_viewer {
namespace {

adqt::icons::IconPalette buildPaletteSnapshot() {
    const auto& manager = adqt::theme::ThemeManager::instance();
    const auto& colors = manager.theme().palette;

    adqt::icons::IconPalette palette;
    palette.text = QColor(colors.colorText);
    palette.textDisabled = QColor(colors.colorTextQuaternary);
    palette.primary = QColor(colors.colorPrimary);
    palette.twoToneSecondary = QColor(colors.colorPrimaryBg);
    palette.tertiary = QColor(colors.colorPrimaryBorder);
    palette.revision = manager.themeRevision();
    return palette;
}

} // namespace

void installIconThemeResolver() {
    adqt::icons::setPaletteResolver([]() { return buildPaletteSnapshot(); });
}

} // namespace snow::image_viewer
