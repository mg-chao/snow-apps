#pragma once

#include <QLocale>

namespace adqt::locale {

struct LocaleConfig {
  QLocale locale = QLocale(QLocale::English, QLocale::UnitedStates);

  friend bool operator==(const LocaleConfig& lhs, const LocaleConfig& rhs) {
    return lhs.locale == rhs.locale;
  }

  friend bool operator!=(const LocaleConfig& lhs, const LocaleConfig& rhs) { return !(lhs == rhs); }
};

}  // namespace adqt::locale
