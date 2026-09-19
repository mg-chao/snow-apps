#include "theme/theme_manager.h"
#include "theme/theme_types.h"

#include <QApplication>
#include <QFont>
#include <QLabel>
#include <QMenu>
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

void requireSmoothOutlines(const QFont& font, const char* message) {
  require(font.hintingPreference() == QFont::PreferNoHinting, message);
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  auto& manager = adqt::theme::ThemeManager::instance();

  manager.applyTo(app);

  // The theme owns application typography: every surface resolving from the application
  // font renders smooth outlines at fractional device scale factors.
  requireSmoothOutlines(QApplication::font(),
                        "the themed application font must render unhinted outlines");
  for (const char* popupClass : {"QTipLabel", "QMessageBox", "QMenu"}) {
    requireSmoothOutlines(QApplication::font(popupClass),
                          "popup class fonts must render unhinted outlines");
  }

  QWidget standalone;  // models overlay, palette, and tray windows without owners
  requireSmoothOutlines(standalone.font(),
                        "parentless top-level widgets must inherit unhinted outlines");

  auto* anchored = new QWidget(&standalone);
  QFont sizeOnly;
  sizeOnly.setPixelSize(20);  // partially configured fonts keep merging with the chain
  anchored->setFont(sizeOnly);
  requireSmoothOutlines(anchored->font(),
                        "partially configured fonts must inherit unhinted outlines");
  auto* label = new QLabel(QStringLiteral("Preview"), anchored);
  QFont weightOnly;
  weightOnly.setWeight(QFont::DemiBold);
  label->setFont(weightOnly);
  requireSmoothOutlines(label->font(), "child labels must inherit unhinted outlines");
  delete anchored;

  QMenu menu;  // seeds its font from the QMenu class font, not from an owner chain
  requireSmoothOutlines(menu.font(), "menus must render unhinted outlines");

  // A configured theme font keeps its family and size while gaining the outline policy,
  // and the resolved token matches what the application actually renders with.
  adqt::theme::ThemeConfig config = manager.config();
  config.appFont = QFont(QStringLiteral("theme-typography-probe"));
  config.appFont.setPixelSize(15);
  manager.setConfig(config);
  require(QApplication::font().family() == QStringLiteral("theme-typography-probe") &&
              QApplication::font().pixelSize() == 15,
          "configured theme fonts must keep their family and size");
  requireSmoothOutlines(QApplication::font(),
                        "configured theme fonts must render unhinted outlines");
  QWidget resolver;
  requireSmoothOutlines(manager.resolveTheme(&resolver).appFont,
                        "resolved theme tokens must carry the unhinted outline policy");

  // Returning to the unconfigured state restores the captured platform font, still with
  // the design system's outline policy.
  config.appFont = QFont();
  manager.setConfig(config);
  requireSmoothOutlines(QApplication::font(),
                        "restored platform fonts must keep unhinted outlines");
  for (const char* popupClass : {"QTipLabel", "QMessageBox", "QMenu"}) {
    requireSmoothOutlines(QApplication::font(popupClass),
                          "popup class fonts must stay unhinted after config changes");
  }

  return 0;
}
