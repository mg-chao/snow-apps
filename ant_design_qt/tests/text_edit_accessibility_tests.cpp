#include "theme/theme_manager.h"
#include "widgets/input_text_edit.h"

#include <QApplication>
#include <QFocusEvent>
#include <QScopeGuard>
#include <QTranslator>

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

class InputTranslator final : public QTranslator {
 public:
  bool isEmpty() const override { return false; }

  QString translate(const char* context, const char* source, const char*, int) const override {
    if (qstrcmp(context, "adqt::widgets::AdTextEdit") == 0 &&
        qstrcmp(source, "Multiline input") == 0)
      return QStringLiteral("Translated multiline input");
    return {};
  }
};

void callerNamesSurviveRefreshesAndDefaultsRetranslate() {
  auto& themes = adqt::theme::ThemeManager::instance();
  const auto originalTheme = themes.theme();
  const auto restoreTheme = qScopeGuard([&] { themes.setTheme(originalTheme); });
  themes.setPreset(adqt::theme::ThemeScheme::Light, adqt::theme::ThemeDensity::Compact);
  InputTranslator translator;
  QWidget owner;
  adqt::widgets::AdTextEdit custom(&owner);
  adqt::widgets::AdTextEdit generic(&owner);
  owner.resize(320, 220);
  custom.setGeometry(0, 0, 320, 100);
  generic.setGeometry(0, 110, 320, 100);
  owner.show();
  QApplication::processEvents();
  require(generic.accessibleName() == QStringLiteral("Multiline input"),
          "a text input starts with the translated generic accessible name");
  const QString callerName = QStringLiteral("Formula source");
  custom.setAccessibleName(callerName);
  custom.setPlainText(QStringLiteral("x"));
  require(custom.accessibleName() == callerName,
          "programmatic text changes preserve a caller's accessible name");
  QTextCursor cursor = custom.textCursor();
  cursor.insertText(QStringLiteral("y"));
  require(custom.accessibleName() == callerName,
          "document edits preserve a caller's accessible name");
  themes.setPreset(adqt::theme::ThemeScheme::Dark, adqt::theme::ThemeDensity::Compact);
  require(custom.accessibleName() == callerName,
          "theme changes preserve a caller's accessible name");
  QFocusEvent focus(QEvent::FocusIn, Qt::TabFocusReason);
  QFocusEvent blur(QEvent::FocusOut, Qt::OtherFocusReason);
  QApplication::sendEvent(&custom, &focus);
  require(custom.accessibleName() == callerName,
          "focus changes preserve a caller's accessible name");
  QApplication::sendEvent(&custom, &blur);
  require(custom.accessibleName() == callerName,
          "blur changes preserve a caller's accessible name");
  QFont font = custom.font();
  font.setPointSizeF(14.0);
  custom.setFont(font);
  custom.setEnabled(false);
  custom.setEnabled(true);
  require(custom.accessibleName() == callerName,
          "font and enabled-state changes preserve a caller's accessible name");

  QEvent languageChange(QEvent::LanguageChange);
  {
    require(QCoreApplication::installTranslator(&translator), "install the input translator");
    const auto uninstallTranslator =
        qScopeGuard([&] { QCoreApplication::removeTranslator(&translator); });
    QApplication::sendEvent(&owner, &languageChange);
    QApplication::processEvents();
    require(custom.accessibleName() == callerName &&
                generic.accessibleName() == QStringLiteral("Translated multiline input"),
            "language changes preserve custom names and retranslate component defaults");
    custom.setAccessibleName({});
    custom.setStatus(adqt::widgets::AdTextEdit::Status::Warning);
    require(custom.accessibleName() == QStringLiteral("Translated multiline input"),
            "clearing a caller name restores the translated generic name on refresh");
  }
  QApplication::sendEvent(&owner, &languageChange);
  QApplication::processEvents();
  require(custom.accessibleName() == QStringLiteral("Multiline input") &&
              generic.accessibleName() == QStringLiteral("Multiline input"),
          "restored generic names continue to follow subsequent language changes");
}

}  // namespace

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  try {
    callerNamesSurviveRefreshesAndDefaultsRetranslate();
    std::cout << "Text edit accessibility tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
