#include "locale/locale.h"

#include <QApplication>
#include <QCoreApplication>

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
  auto& manager = adqt::locale::LocaleManager::instance();
  manager.applyTo(app);

  int changeCount = 0;
  QObject::connect(&manager, &adqt::locale::LocaleManager::localeChanged, &app,
                   [&changeCount](const QLocale&) { ++changeCount; });

  manager.setLocale(QLocale(QLocale::Chinese, QLocale::China));
  require(manager.locale().name() == QStringLiteral("zh_CN"),
          "Chinese locale should become active");
  require(changeCount == 1, "localeChanged should be emitted once");
  require(QCoreApplication::translate("adqt::widgets::AdPagination", "Previous page") !=
              QStringLiteral("Previous page"),
          "the bundled Ant Design catalog should translate pagination text");
  require(QCoreApplication::translate("adqt::widgets::AdPagination", "%1 / page")
                  .arg(50) == QStringLiteral("50 条/页"),
          "the bundled catalog should translate pagination page-size labels");
  require(QCoreApplication::translate("adqt::widgets::AdImage", "Preview") ==
              QStringLiteral("预览"),
          "the bundled catalog should translate the image preview label");

  manager.setLocale(QLocale(QLocale::English, QLocale::UnitedStates));
  require(manager.locale().name() == QStringLiteral("en_US"),
          "English locale should become active");
  require(changeCount == 2, "switching back should emit localeChanged");
  require(QCoreApplication::translate("adqt::widgets::AdPagination", "Previous page") ==
              QStringLiteral("Previous page"),
          "English should use source strings");

  return 0;
}
