#include "app/application_setup.h"

#include "ui/app_icons.h"
#include "ui/icon_theme_adapter.h"

#include "icon_renderer.h"
#include "theme/theme_manager.h"
#include "widgets/tooltip.h"

#include <QApplication>
#include <QStyleFactory>
#include <QStyleHints>

namespace snow::image_viewer {
namespace {

adqt::theme::ThemeScheme initialThemeScheme() {
    return QApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark
               ? adqt::theme::ThemeScheme::Dark
               : adqt::theme::ThemeScheme::Light;
}

} // namespace

void configureViewerApplicationIdentity(QApplication& app) {
    Q_UNUSED(app)
    QApplication::setOrganizationName(QStringLiteral("Snow Shot"));
    QApplication::setOrganizationDomain(QStringLiteral("snow-shot.app"));
    QApplication::setApplicationName(QStringLiteral("snow_image_viewer"));
    QApplication::setApplicationDisplayName(QStringLiteral("Snow Image Viewer"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
}

void configureViewerApplicationAppearance(QApplication& app) {
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        QApplication::setStyle(fusion);
    }

    auto& themeManager = adqt::theme::ThemeManager::instance();
    themeManager.setPreset(initialThemeScheme(), adqt::theme::ThemeDensity::Comfortable);
    themeManager.applyTo(app);
    installIconThemeResolver();
    adqt::widgets::AdTooltip::installApplicationTooltips();
    QApplication::setWindowIcon(adqt::icons::makeIcon(icons::app::ApplicationIcon()));
}

} // namespace snow::image_viewer
