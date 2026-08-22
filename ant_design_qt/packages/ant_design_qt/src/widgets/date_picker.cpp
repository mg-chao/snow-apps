#include "date_picker.h"

#include "antd_icons.h"
#include "date_picker_style.h"
#include "locale/locale.h"
#include "detail/themed_scrollbar.h"
#include "input_internal.h"
#include "input_style.h"
#include "detail/overlay_popup_surface.h"
#include "interaction_overlay_manager.h"
#include "popup_placement.h"
#include "theme/theme_manager.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCursor>
#include <QDateTime>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLayoutItem>
#include <QListWidget>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QStringList>
#include <QTimeEdit>
#include <QTimer>
#include <QToolButton>
#include <QVariant>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>

namespace adqt::widgets {

namespace {

namespace outlined_icons = adqt::icons::antd::outlined;
namespace filled_icons = adqt::icons::antd::filled;

QPoint mouseEventPos(const QMouseEvent* event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  return event ? event->position().toPoint() : QPoint();
#else
  return event ? event->pos() : QPoint();
#endif
}

bool widgetInTree(const QWidget* candidate, const QWidget* root) {
  if (!candidate || !root) {
    return false;
  }
  return candidate == root || root->isAncestorOf(const_cast<QWidget*>(candidate));
}

QString normalizedInputId(const QString& value) { return value.trimmed(); }

void applyAccessibleIdentifier(QWidget* widget, const QString& value) {
  if (!widget) {
    return;
  }
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
  widget->setAccessibleIdentifier(value);
#else
  Q_UNUSED(value)
#endif
}

detail::OverlayPopupPlacement toOverlayPopupPlacement(AdDatePicker::Placement placement) {
  switch (placement) {
    case AdDatePicker::Placement::BottomRight:
      return detail::OverlayPopupPlacement::BottomRight;
    case AdDatePicker::Placement::TopLeft:
      return detail::OverlayPopupPlacement::TopLeft;
    case AdDatePicker::Placement::TopRight:
      return detail::OverlayPopupPlacement::TopRight;
    case AdDatePicker::Placement::BottomLeft:
    default:
      return detail::OverlayPopupPlacement::BottomLeft;
  }
}

AdLineEdit::ControlSize toInputSize(AdDatePicker::Size size) {
  switch (size) {
    case AdDatePicker::Size::Large:
      return AdLineEdit::ControlSize::Large;
    case AdDatePicker::Size::Small:
      return AdLineEdit::ControlSize::Small;
    case AdDatePicker::Size::Middle:
    default:
      return AdLineEdit::ControlSize::Medium;
  }
}

AdLineEdit::Variant toInputVariant(AdDatePicker::Variant variant) {
  switch (variant) {
    case AdDatePicker::Variant::Filled:
      return AdLineEdit::Variant::Filled;
    case AdDatePicker::Variant::Borderless:
      return AdLineEdit::Variant::Borderless;
    case AdDatePicker::Variant::Underlined:
      return AdLineEdit::Variant::Underlined;
    case AdDatePicker::Variant::Outlined:
    default:
      return AdLineEdit::Variant::Outlined;
  }
}

AdLineEdit::Status toInputStatus(AdDatePicker::Status status) {
  switch (status) {
    case AdDatePicker::Status::Error:
      return AdLineEdit::Status::Error;
    case AdDatePicker::Status::Warning:
      return AdLineEdit::Status::Warning;
    case AdDatePicker::Status::None:
    default:
      return AdLineEdit::Status::None;
  }
}

bool isValidDayOfWeek(Qt::DayOfWeek value) { return value >= Qt::Monday && value <= Qt::Sunday; }

Qt::DayOfWeek normalizedFirstDayOfWeek(Qt::DayOfWeek value) {
  return isValidDayOfWeek(value) ? value : Qt::Monday;
}

Qt::DayOfWeek firstDayOfWeekForLocale(const QLocale& locale) {
  return normalizedFirstDayOfWeek(locale.firstDayOfWeek());
}

QDate todayDate() { return QDate::currentDate(); }

QDate startOfWeek(const QDate& value, Qt::DayOfWeek firstDayOfWeek) {
  if (!value.isValid()) {
    return {};
  }
  const int first = static_cast<int>(normalizedFirstDayOfWeek(firstDayOfWeek));
  const int delta = (value.dayOfWeek() - first + 7) % 7;
  return value.addDays(-delta);
}

int quarterForMonth(int month) { return ((std::max(1, month) - 1) / 3) + 1; }

QDate startOfQuarter(const QDate& value) {
  if (!value.isValid()) {
    return {};
  }
  const int month = (quarterForMonth(value.month()) - 1) * 3 + 1;
  return QDate(value.year(), month, 1);
}

QDate endOfMonth(const QDate& value) {
  if (!value.isValid()) {
    return {};
  }
  return QDate(value.year(), value.month(), value.daysInMonth());
}

int decadeStartForYear(int year) {
  if (year >= 0) {
    return (year / 10) * 10;
  }
  return ((year - 9) / 10) * 10;
}

int centuryStartForYear(int year) {
  if (year >= 0) {
    return (year / 100) * 100;
  }
  return ((year - 99) / 100) * 100;
}

QDate normalizeForPicker(AdDatePickerPanel::PickerMode mode, const QDate& value,
                         Qt::DayOfWeek firstDayOfWeek) {
  if (!value.isValid()) {
    return {};
  }

  switch (mode) {
    case AdDatePickerPanel::PickerMode::Week:
      return startOfWeek(value, firstDayOfWeek);
    case AdDatePickerPanel::PickerMode::Month:
      return QDate(value.year(), value.month(), 1);
    case AdDatePickerPanel::PickerMode::Quarter:
      return startOfQuarter(value);
    case AdDatePickerPanel::PickerMode::Year:
      return QDate(value.year(), 1, 1);
    case AdDatePickerPanel::PickerMode::Decade:
      return QDate(decadeStartForYear(value.year()), 1, 1);
    case AdDatePickerPanel::PickerMode::Time:
    case AdDatePickerPanel::PickerMode::Date:
    default:
      return value;
  }
}

void normalizedDateBounds(const QDate& minDate, const QDate& maxDate, QDate* lower, QDate* upper) {
  if (lower) {
    *lower = minDate;
  }
  if (upper) {
    *upper = maxDate;
  }
  if (lower && upper && lower->isValid() && upper->isValid() && *upper < *lower) {
    std::swap(*lower, *upper);
  }
}

QDate pickerRangeStart(AdDatePickerPanel::PickerMode mode, const QDate& value,
                       Qt::DayOfWeek firstDayOfWeek) {
  return normalizeForPicker(mode, value, firstDayOfWeek);
}

QDate pickerRangeEnd(AdDatePickerPanel::PickerMode mode, const QDate& value,
                     Qt::DayOfWeek firstDayOfWeek) {
  const QDate start = pickerRangeStart(mode, value, firstDayOfWeek);
  if (!start.isValid()) {
    return {};
  }

  switch (mode) {
    case AdDatePickerPanel::PickerMode::Week:
      return start.addDays(6);
    case AdDatePickerPanel::PickerMode::Month:
      return endOfMonth(start);
    case AdDatePickerPanel::PickerMode::Quarter:
      return start.addMonths(3).addDays(-1);
    case AdDatePickerPanel::PickerMode::Year:
      return QDate(start.year(), 12, 31);
    case AdDatePickerPanel::PickerMode::Decade:
      return QDate(start.year() + 9, 12, 31);
    case AdDatePickerPanel::PickerMode::Time:
    case AdDatePickerPanel::PickerMode::Date:
    default:
      return start;
  }
}

bool pickerValueWithinBounds(AdDatePickerPanel::PickerMode mode, const QDate& value,
                             Qt::DayOfWeek firstDayOfWeek, const QDate& minDate,
                             const QDate& maxDate) {
  const QDate start = pickerRangeStart(mode, value, firstDayOfWeek);
  const QDate end = pickerRangeEnd(mode, value, firstDayOfWeek);
  if (!start.isValid() || !end.isValid()) {
    return false;
  }

  QDate lower;
  QDate upper;
  normalizedDateBounds(minDate, maxDate, &lower, &upper);
  if (lower.isValid() && end < lower) {
    return false;
  }
  if (upper.isValid() && start > upper) {
    return false;
  }
  return true;
}

bool samePickerValue(AdDatePickerPanel::PickerMode mode, const QDate& lhs, const QDate& rhs,
                     Qt::DayOfWeek firstDayOfWeek) {
  if (!lhs.isValid() || !rhs.isValid()) {
    return false;
  }
  return normalizeForPicker(mode, lhs, firstDayOfWeek) ==
         normalizeForPicker(mode, rhs, firstDayOfWeek);
}

QString monthName(int month, const QLocale& locale, bool shortName = false) {
  QString name =
      locale.standaloneMonthName(month, shortName ? QLocale::ShortFormat : QLocale::LongFormat);
  if (name.isEmpty()) {
    name = locale.monthName(month, shortName ? QLocale::ShortFormat : QLocale::LongFormat);
  }
  return name.isEmpty() ? QString::number(month) : name;
}

QStringList normalizedFormats(const QStringList& formats) {
  QStringList out;
  for (const QString& format : formats) {
    const QString trimmed = format.trimmed();
    if (!trimmed.isEmpty() && !out.contains(trimmed)) {
      out.append(trimmed);
    }
  }
  return out;
}

QString normalizeDateFormatSyntax(QString format) {
  QString normalized;
  normalized.reserve(format.size() + 4);
  bool inQuote = false;
  const auto tokenAt = [&format](int index, const QString& token) {
    return index >= 0 && index + token.size() <= format.size() &&
           format.mid(index, token.size()) == token;
  };
  const auto appendQuotedLiteral = [&normalized](QString literal) {
    literal.replace(QStringLiteral("'"), QStringLiteral("''"));
    normalized += QLatin1Char('\'');
    normalized += literal;
    normalized += QLatin1Char('\'');
  };

  for (int i = 0; i < format.size();) {
    const QChar ch = format.at(i);
    if (ch == QLatin1Char('\'')) {
      normalized += ch;
      if (i + 1 < format.size() && format.at(i + 1) == QLatin1Char('\'')) {
        normalized += format.at(i + 1);
        i += 2;
        continue;
      }
      inQuote = !inQuote;
      ++i;
      continue;
    }
    if (ch == QLatin1Char('\\') && i + 1 < format.size()) {
      normalized += ch;
      normalized += format.at(i + 1);
      i += 2;
      continue;
    }
    if (inQuote) {
      normalized += ch;
      ++i;
      continue;
    }
    if (ch == QLatin1Char('[')) {
      QString literal;
      int j = i + 1;
      bool closed = false;
      for (; j < format.size(); ++j) {
        const QChar literalChar = format.at(j);
        if (literalChar == QLatin1Char('\\') && j + 1 < format.size()) {
          literal += format.at(++j);
          continue;
        }
        if (literalChar == QLatin1Char(']')) {
          closed = true;
          break;
        }
        literal += literalChar;
      }
      if (closed) {
        appendQuotedLiteral(literal);
        i = j + 1;
        continue;
      }
    }

    if (tokenAt(i, QStringLiteral("YYYY"))) {
      normalized += QStringLiteral("yyyy");
      i += 4;
      continue;
    }
    if (tokenAt(i, QStringLiteral("YY"))) {
      normalized += QStringLiteral("yy");
      i += 2;
      continue;
    }
    if (tokenAt(i, QStringLiteral("DD"))) {
      normalized += QStringLiteral("dd");
      i += 2;
      continue;
    }
    if (tokenAt(i, QStringLiteral("kk"))) {
      normalized += QStringLiteral("HH");
      i += 2;
      continue;
    }
    if (ch == QLatin1Char('k')) {
      normalized += QLatin1Char('H');
      ++i;
      continue;
    }
    if (ch == QLatin1Char('A')) {
      if (i + 1 < format.size() && format.at(i + 1) == QLatin1Char('P')) {
        normalized += QStringLiteral("AP");
        i += 2;
      } else {
        normalized += QStringLiteral("AP");
        ++i;
      }
      continue;
    }
    if (ch == QLatin1Char('a')) {
      if (i + 1 < format.size() && format.at(i + 1) == QLatin1Char('p')) {
        normalized += QStringLiteral("ap");
        i += 2;
      } else {
        normalized += QStringLiteral("ap");
        ++i;
      }
      continue;
    }

    normalized += ch;
    ++i;
  }

  return normalized;
}

QStringList normalizeDateFormatSyntax(const QStringList& formats) {
  QStringList out;
  out.reserve(formats.size());
  for (const QString& format : formats) {
    const QString normalized = normalizeDateFormatSyntax(format.trimmed());
    if (!normalized.isEmpty() && !out.contains(normalized)) {
      out.append(normalized);
    }
  }
  return out;
}

constexpr int kBuddhistEraYearOffset = 543;

struct FormatTokenReplacement {
  QString marker;
  QString value;
};

struct PreparedDateFormat {
  QString format;
  QVector<FormatTokenReplacement> replacements;
};

struct BuddhistEraYearCapture {
  int group = 0;
};

bool formatAt(const QString& format, int index, const QString& token) {
  return index >= 0 && index + token.size() <= format.size() &&
         format.mid(index, token.size()) == token;
}

QString quotedDateFormatLiteral(QString literal) {
  literal.replace(QStringLiteral("'"), QStringLiteral("''"));
  return QStringLiteral("'%1'").arg(literal);
}

QString buddhistEraYearText(const QDate& value, int width) {
  const QString year = QString::number(value.year() + kBuddhistEraYearOffset);
  if (width == 2) {
    return year.right(2).rightJustified(2, QLatin1Char('0'));
  }
  return year.rightJustified(width, QLatin1Char('0'));
}

PreparedDateFormat prepareBuddhistEraDisplayFormat(const QString& format, const QDate& date) {
  PreparedDateFormat prepared;
  prepared.format.reserve(format.size());
  bool inQuote = false;
  int replacementIndex = 0;

  for (int i = 0; i < format.size();) {
    const QChar ch = format.at(i);
    if (ch == QLatin1Char('\'')) {
      prepared.format += ch;
      if (i + 1 < format.size() && format.at(i + 1) == QLatin1Char('\'')) {
        prepared.format += format.at(i + 1);
        i += 2;
        continue;
      }
      inQuote = !inQuote;
      ++i;
      continue;
    }

    if (!inQuote && formatAt(format, i, QStringLiteral("BBBB"))) {
      const QString marker = QStringLiteral("__ADQT_BE_YEAR4_%1__").arg(++replacementIndex);
      prepared.format += quotedDateFormatLiteral(marker);
      prepared.replacements.append({marker, buddhistEraYearText(date, 4)});
      i += 4;
      continue;
    }

    if (!inQuote && formatAt(format, i, QStringLiteral("BB"))) {
      const QString marker = QStringLiteral("__ADQT_BE_YEAR2_%1__").arg(++replacementIndex);
      prepared.format += quotedDateFormatLiteral(marker);
      prepared.replacements.append({marker, buddhistEraYearText(date, 2)});
      i += 2;
      continue;
    }

    prepared.format += ch;
    ++i;
  }

  return prepared;
}

QString applyFormatTokenReplacements(QString text,
                                     const QVector<FormatTokenReplacement>& replacements) {
  for (const FormatTokenReplacement& replacement : replacements) {
    text.replace(replacement.marker, replacement.value);
  }
  return text;
}

enum class MeridiemTokenStyle : std::uint8_t { None, Lower, Upper };

MeridiemTokenStyle meridiemTokenStyleForFormat(const QString& format) {
  bool inQuote = false;
  for (int i = 0; i + 1 < format.size(); ++i) {
    const QChar ch = format.at(i);
    if (ch == QLatin1Char('\'')) {
      if (i + 1 < format.size() && format.at(i + 1) == QLatin1Char('\'')) {
        ++i;
        continue;
      }
      inQuote = !inQuote;
      continue;
    }
    if (ch == QLatin1Char('\\')) {
      ++i;
      continue;
    }
    if (inQuote) {
      continue;
    }
    if (ch == QLatin1Char('A') && format.at(i + 1) == QLatin1Char('P')) {
      return MeridiemTokenStyle::Upper;
    }
    if (ch == QLatin1Char('a') && format.at(i + 1) == QLatin1Char('p')) {
      return MeridiemTokenStyle::Lower;
    }
  }
  return MeridiemTokenStyle::None;
}

QString applyMeridiemTokenReplacements(QString text, const QString& format, const QLocale& locale) {
  const MeridiemTokenStyle style = meridiemTokenStyleForFormat(format);
  if (style == MeridiemTokenStyle::None) {
    return text;
  }
  const QString amText = locale.amText();
  const QString pmText = locale.pmText();
  if (style == MeridiemTokenStyle::Upper) {
    if (!amText.isEmpty()) {
      text.replace(amText, QStringLiteral("AM"));
    }
    if (!pmText.isEmpty()) {
      text.replace(pmText, QStringLiteral("PM"));
    }
    return text;
  }
  if (!amText.isEmpty()) {
    text.replace(amText, QStringLiteral("am"));
  }
  if (!pmText.isEmpty()) {
    text.replace(pmText, QStringLiteral("pm"));
  }
  return text;
}

QString formatDateWithTokens(const QDate& value, const QString& format, const QLocale& locale) {
  const PreparedDateFormat prepared = prepareBuddhistEraDisplayFormat(format, value);
  QString text =
      applyFormatTokenReplacements(locale.toString(value, prepared.format), prepared.replacements);
  return applyMeridiemTokenReplacements(std::move(text), prepared.format, locale);
}

QString formatDateTimeWithTokens(const QDateTime& value, const QString& format,
                                 const QLocale& locale) {
  const PreparedDateFormat prepared = prepareBuddhistEraDisplayFormat(format, value.date());
  QString text =
      applyFormatTokenReplacements(locale.toString(value, prepared.format), prepared.replacements);
  return applyMeridiemTokenReplacements(std::move(text), prepared.format, locale);
}

bool isDateFormatPatternChar(QChar ch) {
  return ch == QLatin1Char('d') || ch == QLatin1Char('M') || ch == QLatin1Char('y') ||
         ch == QLatin1Char('h') || ch == QLatin1Char('H') || ch == QLatin1Char('m') ||
         ch == QLatin1Char('s') || ch == QLatin1Char('z') || ch == QLatin1Char('a') ||
         ch == QLatin1Char('A') || ch == QLatin1Char('k') || ch == QLatin1Char('p') ||
         ch == QLatin1Char('P') || ch == QLatin1Char('t');
}

QString buddhistEraParseFormat(const QString& format) {
  QString out;
  out.reserve(format.size());
  bool inQuote = false;
  for (int i = 0; i < format.size();) {
    const QChar ch = format.at(i);
    if (ch == QLatin1Char('\'')) {
      out += ch;
      if (i + 1 < format.size() && format.at(i + 1) == QLatin1Char('\'')) {
        out += format.at(i + 1);
        i += 2;
        continue;
      }
      inQuote = !inQuote;
      ++i;
      continue;
    }
    if (!inQuote && formatAt(format, i, QStringLiteral("BBBB"))) {
      out += QStringLiteral("yyyy");
      i += 4;
      continue;
    }
    out += ch;
    ++i;
  }
  return out;
}

QString buddhistEraLocatorPattern(const QString& format,
                                  QVector<BuddhistEraYearCapture>* captures) {
  if (captures) {
    captures->clear();
  }

  QString pattern = QStringLiteral("^");
  int group = 1;
  bool inQuote = false;
  for (int i = 0; i < format.size();) {
    const QChar ch = format.at(i);
    if (ch == QLatin1Char('\'')) {
      if (i + 1 < format.size() && format.at(i + 1) == QLatin1Char('\'')) {
        pattern += QRegularExpression::escape(QString(ch));
        i += 2;
        continue;
      }
      inQuote = !inQuote;
      ++i;
      continue;
    }

    if (!inQuote && formatAt(format, i, QStringLiteral("BBBB"))) {
      pattern += QStringLiteral("(\\d{4})");
      if (captures) {
        BuddhistEraYearCapture capture;
        capture.group = group;
        captures->append(capture);
      }
      ++group;
      i += 4;
      continue;
    }

    if (!inQuote && isDateFormatPatternChar(ch)) {
      int width = 1;
      while (i + width < format.size() && format.at(i + width) == ch) {
        ++width;
      }
      pattern += QStringLiteral(".+?");
      i += width;
      continue;
    }

    pattern += QRegularExpression::escape(QString(ch));
    ++i;
  }
  pattern += QStringLiteral("$");
  return pattern;
}

bool prepareBuddhistEraParseText(const QString& text, const QString& format, QString* parsedText,
                                 QString* parsedFormat) {
  QVector<BuddhistEraYearCapture> captures;
  const QRegularExpression re(buddhistEraLocatorPattern(format, &captures));
  if (captures.isEmpty()) {
    if (parsedText) {
      *parsedText = text;
    }
    if (parsedFormat) {
      *parsedFormat = format;
    }
    return true;
  }

  const QRegularExpressionMatch match = re.match(text);
  if (!match.hasMatch()) {
    return false;
  }

  QString converted = text;
  for (qsizetype i = captures.size(); i > 0; --i) {
    const BuddhistEraYearCapture& capture = captures.at(i - 1);
    bool yearOk = false;
    const int buddhistYear = match.captured(capture.group).toInt(&yearOk);
    const int gregorianYear = buddhistYear - kBuddhistEraYearOffset;
    if (!yearOk || gregorianYear < 1 || gregorianYear > 9999) {
      return false;
    }
    converted.replace(match.capturedStart(capture.group), match.capturedLength(capture.group),
                      QString::number(gregorianYear).rightJustified(4, QLatin1Char('0')));
  }

  if (parsedText) {
    *parsedText = converted;
  }
  if (parsedFormat) {
    *parsedFormat = buddhistEraParseFormat(format);
  }
  return true;
}

QDate parseDateWithTokens(const QString& text, const QString& format, const QLocale& locale) {
  QString parsedText;
  QString parsedFormat;
  if (!prepareBuddhistEraParseText(text, format, &parsedText, &parsedFormat)) {
    return {};
  }
  QDate date = locale.toDate(parsedText, parsedFormat);
  if (!date.isValid()) {
    date = QDate::fromString(parsedText, parsedFormat);
  }
  return date;
}

QDateTime parseDateTimeWithTokens(const QString& text, const QString& format,
                                  const QLocale& locale) {
  QString parsedText;
  QString parsedFormat;
  if (!prepareBuddhistEraParseText(text, format, &parsedText, &parsedFormat)) {
    return {};
  }
  QDateTime dateTime = locale.toDateTime(parsedText, parsedFormat);
  if (!dateTime.isValid()) {
    dateTime = QDateTime::fromString(parsedText, parsedFormat);
  }
  return dateTime;
}

QTime defaultTimeValue() { return QTime(0, 0, 0); }

QTime normalizedTimeValue(const QTime& value) {
  return value.isValid() ? value : defaultTimeValue();
}

QString defaultTimeFormat(bool use12Hours) {
  return use12Hours ? QStringLiteral("h:mm:ss ap") : QStringLiteral("HH:mm:ss");
}

QString normalizedTimeFormat(const QString& format, bool use12Hours = false) {
  const QString trimmed = normalizeDateFormatSyntax(format.trimmed());
  return trimmed.isEmpty() ? defaultTimeFormat(use12Hours) : trimmed;
}

QString inferredTimeFormatFromDisplayFormat(const QString& displayFormat) {
  const QString normalized = normalizeDateFormatSyntax(displayFormat.trimmed());
  if (normalized.isEmpty()) {
    return QString();
  }

  bool inQuote = false;
  int firstTimeToken = -1;
  for (int i = 0; i < normalized.size(); ++i) {
    const QChar ch = normalized.at(i);
    if (ch == QLatin1Char('\'')) {
      if (i + 1 < normalized.size() && normalized.at(i + 1) == QLatin1Char('\'')) {
        ++i;
        continue;
      }
      inQuote = !inQuote;
      continue;
    }
    if (ch == QLatin1Char('\\') && i + 1 < normalized.size()) {
      ++i;
      continue;
    }
    if (inQuote) {
      continue;
    }
    if (ch == QLatin1Char('H') || ch == QLatin1Char('h') || ch == QLatin1Char('m') ||
        ch == QLatin1Char('s') || ch == QLatin1Char('z') || ch == QLatin1Char('A') ||
        ch == QLatin1Char('a')) {
      firstTimeToken = i;
      break;
    }
  }

  if (firstTimeToken < 0) {
    return QString();
  }

  while (firstTimeToken > 0) {
    const QChar previous = normalized.at(firstTimeToken - 1);
    if (previous.isSpace() || previous == QLatin1Char('T')) {
      break;
    }
    if (previous == QLatin1Char(':') || previous == QLatin1Char('.') ||
        previous == QLatin1Char('-') || previous == QLatin1Char('/')) {
      --firstTimeToken;
      continue;
    }
    break;
  }
  return normalized.mid(firstTimeToken).trimmed();
}

bool formatUses12HourClock(const QString& format) {
  const QString normalized = normalizeDateFormatSyntax(format);
  bool inQuote = false;
  for (int i = 0; i < normalized.size(); ++i) {
    const QChar ch = normalized.at(i);
    if (ch == QLatin1Char('\'')) {
      if (i + 1 < normalized.size() && normalized.at(i + 1) == QLatin1Char('\'')) {
        ++i;
        continue;
      }
      inQuote = !inQuote;
      continue;
    }
    if (ch == QLatin1Char('\\') && i + 1 < normalized.size()) {
      ++i;
      continue;
    }
    if (inQuote) {
      continue;
    }
    if (ch == QLatin1Char('h')) {
      return true;
    }
    if ((ch == QLatin1Char('A') || ch == QLatin1Char('a')) && i + 1 < normalized.size() &&
        (normalized.at(i + 1) == QLatin1Char('P') || normalized.at(i + 1) == QLatin1Char('p'))) {
      return true;
    }
  }
  return false;
}

void appendUniqueFormat(QStringList* formats, const QString& format) {
  if (!formats) {
    return;
  }
  const QString trimmed = format.trimmed();
  if (!trimmed.isEmpty() && !formats->contains(trimmed)) {
    formats->append(trimmed);
  }
}

QString defaultFormatForPicker(AdDatePickerPanel::PickerMode mode, bool showTime = false,
                               const QString& timeFormat = QString(), bool use12Hours = false) {
  switch (mode) {
    case AdDatePickerPanel::PickerMode::Month:
      return QStringLiteral("yyyy-MM");
    case AdDatePickerPanel::PickerMode::Year:
    case AdDatePickerPanel::PickerMode::Decade:
      return QStringLiteral("yyyy");
    case AdDatePickerPanel::PickerMode::Week:
      return QStringLiteral("yyyy-wo");
    case AdDatePickerPanel::PickerMode::Quarter:
      return QStringLiteral("yyyy-'Q'Q");
    case AdDatePickerPanel::PickerMode::Time:
      return normalizedTimeFormat(timeFormat, use12Hours);
    case AdDatePickerPanel::PickerMode::Date:
      if (showTime) {
        return QStringLiteral("yyyy-MM-dd %1").arg(normalizedTimeFormat(timeFormat, use12Hours));
      }
      return QStringLiteral("yyyy-MM-dd");
    default:
      return QString();
  }
}

QStringList effectiveFormatsForPicker(AdDatePickerPanel::PickerMode mode,
                                      const QString& displayFormat,
                                      const QStringList& displayFormats, bool showTime = false,
                                      const QString& timeFormat = QString(),
                                      bool use12Hours = false) {
  QStringList formats = normalizeDateFormatSyntax(normalizedFormats(displayFormats));
  const QString singleFormat = normalizeDateFormatSyntax(displayFormat.trimmed());
  if (formats.isEmpty() && !singleFormat.isEmpty()) {
    formats.append(singleFormat);
  }
  if (formats.isEmpty()) {
    const QString fallback = defaultFormatForPicker(mode, showTime, timeFormat, use12Hours);
    if (!fallback.isEmpty()) {
      formats.append(fallback);
    }
  }
  if (showTime && mode == AdDatePickerPanel::PickerMode::Date) {
    appendUniqueFormat(&formats, defaultFormatForPicker(mode, true, timeFormat, use12Hours));
    appendUniqueFormat(&formats, QStringLiteral("yyyy-MM-dd h:mm:ss ap"));
    appendUniqueFormat(&formats, QStringLiteral("yyyy-MM-dd h:mm ap"));
    appendUniqueFormat(&formats, QStringLiteral("yyyy-MM-dd h:mm:ss AP"));
    appendUniqueFormat(&formats, QStringLiteral("yyyy-MM-dd h:mm AP"));
    appendUniqueFormat(&formats, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    appendUniqueFormat(&formats, QStringLiteral("yyyy-MM-dd HH:mm"));
    appendUniqueFormat(&formats, QStringLiteral("yyyy-MM-dd"));
  }
  if (mode == AdDatePickerPanel::PickerMode::Time && use12Hours) {
    appendUniqueFormat(&formats, QStringLiteral("h:mm:ss ap"));
    appendUniqueFormat(&formats, QStringLiteral("h:mm ap"));
    appendUniqueFormat(&formats, QStringLiteral("h:mm:ss AP"));
    appendUniqueFormat(&formats, QStringLiteral("h:mm AP"));
  }
  return formats;
}

bool formatHasDayToken(const QString& format) { return format.contains(QLatin1Char('d')); }

bool formatHasTimeToken(const QString& format) {
  return format.contains(QLatin1Char('h')) || format.contains(QLatin1Char('H')) ||
         format.contains(QLatin1Char('k')) || format.contains(QLatin1Char('m')) ||
         format.contains(QLatin1Char('s')) || format.contains(QLatin1Char('z')) ||
         format.contains(QLatin1Char('a')) || format.contains(QLatin1Char('A'));
}

bool formatHasSecondToken(const QString& format) {
  return format.contains(QLatin1Char('s')) || format.contains(QLatin1Char('z'));
}

int normalizedTimeStep(int value, int maximum) { return std::clamp(value, 1, maximum); }

bool textContainsDigit(const QString& text) {
  return std::any_of(text.cbegin(), text.cend(), [](QChar ch) { return ch.isDigit(); });
}

bool isMaskFormatToken(QChar ch) {
  return ch == QLatin1Char('y') || ch == QLatin1Char('M') || ch == QLatin1Char('d') ||
         ch == QLatin1Char('H') || ch == QLatin1Char('h') || ch == QLatin1Char('k') ||
         ch == QLatin1Char('m') || ch == QLatin1Char('s') || ch == QLatin1Char('B');
}

QString escapedInputMaskLiteral(QChar ch) {
  static const QString special = QStringLiteral("\\AaNnXx90Dd#HhBb><!;");
  if (special.contains(ch)) {
    return QStringLiteral("\\%1").arg(ch);
  }
  return QString(ch);
}

QString inputMaskBodyForDateFormat(const QString& rawFormat) {
  const QString format = normalizeDateFormatSyntax(rawFormat);
  QString mask;
  bool hasEditable = false;
  bool inQuote = false;
  for (int i = 0; i < format.size(); ++i) {
    const QChar ch = format.at(i);
    if (ch == QLatin1Char('\'')) {
      if (i + 1 < format.size() && format.at(i + 1) == QLatin1Char('\'')) {
        mask += escapedInputMaskLiteral(format.at(++i));
        continue;
      }
      inQuote = !inQuote;
      continue;
    }
    if (ch == QLatin1Char('\\') && i + 1 < format.size()) {
      mask += escapedInputMaskLiteral(format.at(++i));
      continue;
    }
    if (inQuote) {
      mask += escapedInputMaskLiteral(ch);
      continue;
    }
    if (isMaskFormatToken(ch)) {
      mask += QLatin1Char('0');
      hasEditable = true;
      continue;
    }
    mask += escapedInputMaskLiteral(ch);
  }
  return hasEditable ? mask : QString();
}

enum class MaskDateToken : std::uint8_t {
  Year,
  BuddhistEraYear,
  Month,
  Day,
  Hour,
  Minute,
  Second,
};

MaskDateToken maskDateTokenForChar(QChar ch) {
  if (ch == QLatin1Char('y')) {
    return MaskDateToken::Year;
  }
  if (ch == QLatin1Char('B')) {
    return MaskDateToken::BuddhistEraYear;
  }
  if (ch == QLatin1Char('M')) {
    return MaskDateToken::Month;
  }
  if (ch == QLatin1Char('d')) {
    return MaskDateToken::Day;
  }
  if (ch == QLatin1Char('H') || ch == QLatin1Char('h') || ch == QLatin1Char('k')) {
    return MaskDateToken::Hour;
  }
  if (ch == QLatin1Char('m')) {
    return MaskDateToken::Minute;
  }
  return MaskDateToken::Second;
}

QRegularExpression maskedFormatExpression(const QString& rawFormat,
                                          QVector<MaskDateToken>* tokens) {
  if (tokens) {
    tokens->clear();
  }
  const QString format = normalizeDateFormatSyntax(rawFormat);
  QString pattern = QStringLiteral("^");
  bool inQuote = false;
  for (int i = 0; i < format.size();) {
    const QChar ch = format.at(i);
    if (ch == QLatin1Char('\'')) {
      if (i + 1 < format.size() && format.at(i + 1) == QLatin1Char('\'')) {
        pattern += QRegularExpression::escape(QString(format.at(i + 1)));
        i += 2;
        continue;
      }
      inQuote = !inQuote;
      ++i;
      continue;
    }
    if (ch == QLatin1Char('\\') && i + 1 < format.size()) {
      pattern += QRegularExpression::escape(QString(format.at(i + 1)));
      i += 2;
      continue;
    }
    if (inQuote) {
      pattern += QRegularExpression::escape(QString(ch));
      ++i;
      continue;
    }
    if (isMaskFormatToken(ch)) {
      int width = 1;
      while (i + width < format.size() && format.at(i + width) == ch) {
        ++width;
      }
      pattern += QStringLiteral("(\\d{%1})").arg(width);
      if (tokens) {
        tokens->append(maskDateTokenForChar(ch));
      }
      i += width;
      continue;
    }
    pattern += QRegularExpression::escape(QString(ch));
    ++i;
  }
  pattern += QStringLiteral("$");
  return QRegularExpression(pattern);
}

QDateTime alignMaskedDateTimeText(AdDatePickerPanel::PickerMode mode, const QString& text,
                                  const QString& format, Qt::DayOfWeek firstDayOfWeek,
                                  const QTime& fallbackTime, bool* ok) {
  if (ok) {
    *ok = false;
  }
  QVector<MaskDateToken> tokens;
  const QRegularExpression re = maskedFormatExpression(format, &tokens);
  const QRegularExpressionMatch match = re.match(text.trimmed());
  if (!match.hasMatch()) {
    return {};
  }

  bool hasYear = false;
  bool hasMonth = false;
  bool hasDay = false;
  bool hasHour = false;
  bool hasMinute = false;
  bool hasSecond = false;
  int year = 0;
  int month = 1;
  int day = 1;
  int hour = fallbackTime.isValid() ? fallbackTime.hour() : 0;
  int minute = fallbackTime.isValid() ? fallbackTime.minute() : 0;
  int second = fallbackTime.isValid() ? fallbackTime.second() : 0;

  for (int i = 0; i < tokens.size(); ++i) {
    bool numberOk = false;
    const int value = match.captured(i + 1).toInt(&numberOk);
    if (!numberOk) {
      return {};
    }
    switch (tokens.at(i)) {
      case MaskDateToken::Year:
        hasYear = true;
        year = match.captured(i + 1).size() <= 2 ? 2000 + value : value;
        break;
      case MaskDateToken::BuddhistEraYear:
        hasYear = true;
        year = value - kBuddhistEraYearOffset;
        break;
      case MaskDateToken::Month:
        hasMonth = true;
        month = value;
        break;
      case MaskDateToken::Day:
        hasDay = true;
        day = value;
        break;
      case MaskDateToken::Hour:
        hasHour = true;
        hour = value;
        break;
      case MaskDateToken::Minute:
        hasMinute = true;
        minute = value;
        break;
      case MaskDateToken::Second:
        hasSecond = true;
        second = value;
        break;
    }
  }

  if (!hasYear) {
    return {};
  }
  year = std::clamp(year, 1, 9999);
  month = hasMonth ? std::clamp(month, 1, 12) : 1;
  QDate monthStart(year, month, 1);
  if (!monthStart.isValid()) {
    return {};
  }
  day = hasDay ? std::clamp(day, 1, monthStart.daysInMonth()) : 1;
  QDate date(year, month, day);
  if (!date.isValid()) {
    return {};
  }

  hour = hasHour ? std::clamp(hour, 0, 23) : hour;
  minute = hasMinute ? std::clamp(minute, 0, 59) : minute;
  second = hasSecond ? std::clamp(second, 0, 59) : second;
  QTime time(hour, minute, second);
  if (!time.isValid()) {
    time = defaultTimeValue();
  }

  date = normalizeForPicker(mode, date, firstDayOfWeek);
  if (!date.isValid()) {
    return {};
  }
  if (ok) {
    *ok = true;
  }
  return QDateTime(date, time);
}

QString weekdayName(Qt::DayOfWeek day, const QLocale& locale) {
  QString name = locale.standaloneDayName(day, QLocale::ShortFormat);
  if (name.isEmpty()) {
    name = locale.dayName(day, QLocale::ShortFormat);
  }
  return name.left(2);
}

QDate weekOneStart(int year, Qt::DayOfWeek firstDayOfWeek) {
  const QDate januaryFourth(year, 1, 4);
  return januaryFourth.isValid() ? startOfWeek(januaryFourth, firstDayOfWeek) : QDate();
}

int weekNumberForDate(const QDate& value, Qt::DayOfWeek firstDayOfWeek, int* weekYear) {
  if (weekYear) {
    *weekYear = 0;
  }
  if (!value.isValid()) {
    return 0;
  }

  QDate weekStart = startOfWeek(value, firstDayOfWeek);
  int year = value.year();
  QDate currentWeekOne = weekOneStart(year, firstDayOfWeek);
  if (!currentWeekOne.isValid()) {
    return 0;
  }

  if (weekStart < currentWeekOne) {
    --year;
    currentWeekOne = weekOneStart(year, firstDayOfWeek);
  } else {
    const QDate nextWeekOne = weekOneStart(year + 1, firstDayOfWeek);
    if (nextWeekOne.isValid() && weekStart >= nextWeekOne) {
      ++year;
      currentWeekOne = nextWeekOne;
    }
  }

  if (!currentWeekOne.isValid()) {
    return 0;
  }
  if (weekYear) {
    *weekYear = year;
  }
  return static_cast<int>(currentWeekOne.daysTo(weekStart) / 7 + 1);
}

QString ordinalNumberText(int value) {
  const int mod100 = value % 100;
  QString suffix = QStringLiteral("th");
  if (mod100 < 11 || mod100 > 13) {
    switch (value % 10) {
      case 1:
        suffix = QStringLiteral("st");
        break;
      case 2:
        suffix = QStringLiteral("nd");
        break;
      case 3:
        suffix = QStringLiteral("rd");
        break;
      default:
        break;
    }
  }
  return QString::number(value) + suffix;
}

QDate dateFromWeek(int year, int week, Qt::DayOfWeek firstDayOfWeek) {
  if (week < 1 || week > 53) {
    return {};
  }

  const QDate candidate = weekOneStart(year, firstDayOfWeek).addDays((week - 1) * 7);
  int candidateYear = 0;
  const int candidateWeek = weekNumberForDate(candidate, firstDayOfWeek, &candidateYear);
  if (!candidate.isValid() || candidateWeek != week || candidateYear != year) {
    return {};
  }
  return candidate;
}

QString formatWeekOrQuarterDate(const QDate& value, AdDatePickerPanel::PickerMode mode,
                                const QString& format, Qt::DayOfWeek firstDayOfWeek) {
  int periodYear = value.year();
  int week = 0;
  int quarter = 0;
  if (mode == AdDatePickerPanel::PickerMode::Week) {
    week = weekNumberForDate(value, firstDayOfWeek, &periodYear);
  } else {
    quarter = quarterForMonth(value.month());
  }

  const QString pattern =
      format.isEmpty() ? (mode == AdDatePickerPanel::PickerMode::Week ? QStringLiteral("yyyy-wo")
                                                                      : QStringLiteral("yyyy-'Q'Q"))
                       : format;
  QString out;
  out.reserve(pattern.size() + 4);
  bool inQuote = false;
  for (int i = 0; i < pattern.size();) {
    const QChar ch = pattern.at(i);
    if (ch == QLatin1Char('\'')) {
      if (i + 1 < pattern.size() && pattern.at(i + 1) == QLatin1Char('\'')) {
        out += QLatin1Char('\'');
        i += 2;
        continue;
      }
      inQuote = !inQuote;
      ++i;
      continue;
    }
    if (ch == QLatin1Char('\\') && i + 1 < pattern.size()) {
      out += pattern.at(i + 1);
      i += 2;
      continue;
    }
    if (inQuote) {
      out += ch;
      ++i;
      continue;
    }
    if (formatAt(pattern, i, QStringLiteral("yyyy"))) {
      out += QString::number(periodYear).rightJustified(4, QLatin1Char('0'));
      i += 4;
      continue;
    }
    if (formatAt(pattern, i, QStringLiteral("yy"))) {
      out += QString::number(periodYear % 100).rightJustified(2, QLatin1Char('0'));
      i += 2;
      continue;
    }
    if (mode == AdDatePickerPanel::PickerMode::Week) {
      if (formatAt(pattern, i, QStringLiteral("wo"))) {
        out += ordinalNumberText(week);
        i += 2;
        continue;
      }
      if (formatAt(pattern, i, QStringLiteral("ww"))) {
        out += QString::number(week).rightJustified(2, QLatin1Char('0'));
        i += 2;
        continue;
      }
      if (ch == QLatin1Char('w')) {
        out += QString::number(week);
        ++i;
        continue;
      }
    } else {
      if (formatAt(pattern, i, QStringLiteral("Qo"))) {
        out += ordinalNumberText(quarter);
        i += 2;
        continue;
      }
      if (ch == QLatin1Char('Q')) {
        out += QString::number(quarter);
        ++i;
        continue;
      }
    }
    out += ch;
    ++i;
  }
  return out;
}

QString formatDefaultDate(const QDate& value, AdDatePickerPanel::PickerMode mode,
                          const QString& format, const QLocale& locale,
                          Qt::DayOfWeek firstDayOfWeek) {
  if (!value.isValid()) {
    return QString();
  }

  if (!format.isEmpty() && mode == AdDatePickerPanel::PickerMode::Date) {
    return formatDateWithTokens(value, format, locale);
  }

  if (!format.isEmpty() && mode != AdDatePickerPanel::PickerMode::Week &&
      mode != AdDatePickerPanel::PickerMode::Quarter) {
    return formatDateWithTokens(value, format, locale);
  }

  switch (mode) {
    case AdDatePickerPanel::PickerMode::Week:
      return formatWeekOrQuarterDate(value, mode, format, firstDayOfWeek);
    case AdDatePickerPanel::PickerMode::Month:
      return formatDateWithTokens(value, format.isEmpty() ? QStringLiteral("yyyy-MM") : format,
                                  locale);
    case AdDatePickerPanel::PickerMode::Quarter:
      return formatWeekOrQuarterDate(value, mode, format, firstDayOfWeek);
    case AdDatePickerPanel::PickerMode::Year:
    case AdDatePickerPanel::PickerMode::Decade:
      return formatDateWithTokens(value, format.isEmpty() ? QStringLiteral("yyyy") : format,
                                  locale);
    case AdDatePickerPanel::PickerMode::Time:
    case AdDatePickerPanel::PickerMode::Date:
    default:
      return formatDateWithTokens(value, format.isEmpty() ? QStringLiteral("yyyy-MM-dd") : format,
                                  locale);
  }
}

QDateTime dateTimeFromParts(const QDate& date, const QTime& time) {
  return date.isValid() ? QDateTime(date, normalizedTimeValue(time)) : QDateTime();
}

QString formatDefaultDateTime(const QDate& date, const QTime& time,
                              AdDatePickerPanel::PickerMode mode, const QString& format,
                              const QLocale& locale, Qt::DayOfWeek firstDayOfWeek, bool showTime) {
  if (mode == AdDatePickerPanel::PickerMode::Time) {
    Q_UNUSED(date)
    Q_UNUSED(locale)
    Q_UNUSED(firstDayOfWeek)
    return normalizedTimeValue(time).toString(format.isEmpty() ? QStringLiteral("HH:mm:ss")
                                                               : format);
  }
  if (!showTime || mode != AdDatePickerPanel::PickerMode::Date) {
    return formatDefaultDate(date, mode, format, locale, firstDayOfWeek);
  }
  const QDateTime value = dateTimeFromParts(date, time);
  return value.isValid()
             ? formatDateTimeWithTokens(
                   value, format.isEmpty() ? QStringLiteral("yyyy-MM-dd HH:mm:ss") : format, locale)
             : QString();
}

QDate parsePickerText(AdDatePickerPanel::PickerMode mode, const QString& text,
                      const QStringList& formats, const QLocale& locale,
                      Qt::DayOfWeek firstDayOfWeek, bool* ok) {
  if (ok) {
    *ok = false;
  }

  const QString trimmed = text.trimmed();
  if (trimmed.isEmpty()) {
    return {};
  }

  if (mode == AdDatePickerPanel::PickerMode::Week) {
    static const QRegularExpression re(
        QStringLiteral("^(\\d{4})\\s*[-/]?\\s*(?:[Ww]\\s*)?(\\d{1,2})(?:st|nd|rd|th)?$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = re.match(trimmed);
    if (!match.hasMatch()) {
      return {};
    }

    const QDate date =
        dateFromWeek(match.captured(1).toInt(), match.captured(2).toInt(), firstDayOfWeek);
    const QDate normalized = normalizeForPicker(mode, date, firstDayOfWeek);
    if (normalized.isValid() && ok) {
      *ok = true;
    }
    return normalized;
  }

  if (mode == AdDatePickerPanel::PickerMode::Quarter) {
    static const QRegularExpression re(QStringLiteral("^(\\d{4})\\s*[-/]?\\s*[Qq](\\d)$"));
    const QRegularExpressionMatch match = re.match(trimmed);
    if (match.hasMatch()) {
      const int year = match.captured(1).toInt();
      const int quarter = match.captured(2).toInt();
      if (quarter >= 1 && quarter <= 4) {
        const QDate normalized =
            normalizeForPicker(mode, QDate(year, (quarter - 1) * 3 + 1, 1), firstDayOfWeek);
        if (normalized.isValid() && ok) {
          *ok = true;
        }
        return normalized;
      }
    }
    return {};
  }

  QDate date;
  for (const QString& format : formats) {
    if (format.isEmpty()) {
      continue;
    }
    date = parseDateWithTokens(trimmed, format, locale);
    if (date.isValid()) {
      break;
    }
    if (mode == AdDatePickerPanel::PickerMode::Month && !formatHasDayToken(format)) {
      date = parseDateWithTokens(trimmed + QStringLiteral("-01"), format + QStringLiteral("-dd"),
                                 locale);
      if (date.isValid()) {
        break;
      }
    }
  }

  if (!date.isValid() && mode == AdDatePickerPanel::PickerMode::Date) {
    date = QDate::fromString(trimmed, QStringLiteral("yyyy-MM-dd"));
  }
  if (!date.isValid() && mode == AdDatePickerPanel::PickerMode::Month) {
    date = QDate::fromString(trimmed + QStringLiteral("-01"), QStringLiteral("yyyy-MM-dd"));
  }
  if (!date.isValid() && (mode == AdDatePickerPanel::PickerMode::Year ||
                          mode == AdDatePickerPanel::PickerMode::Decade)) {
    bool yearOk = false;
    const int year = trimmed.toInt(&yearOk);
    if (yearOk) {
      date = QDate(year, 1, 1);
    }
  }

  const QDate normalized = normalizeForPicker(mode, date, firstDayOfWeek);
  if (normalized.isValid() && ok) {
    *ok = true;
  }
  return normalized;
}

QDateTime parsePickerDateTimeText(AdDatePickerPanel::PickerMode mode, const QString& text,
                                  const QStringList& formats, const QLocale& locale,
                                  Qt::DayOfWeek firstDayOfWeek, const QTime& fallbackTime,
                                  bool showTime, bool* ok) {
  if (ok) {
    *ok = false;
  }

  const QString trimmed = text.trimmed();
  if (trimmed.isEmpty()) {
    return {};
  }

  if (showTime && mode == AdDatePickerPanel::PickerMode::Date) {
    for (const QString& format : formats) {
      if (format.isEmpty() || !formatHasTimeToken(format)) {
        continue;
      }
      QDateTime dateTime = parseDateTimeWithTokens(trimmed, format, locale);
      if (dateTime.isValid()) {
        if (ok) {
          *ok = true;
        }
        return dateTime;
      }
    }
  }

  if (mode == AdDatePickerPanel::PickerMode::Time) {
    QStringList timeFormats = formats;
    appendUniqueFormat(&timeFormats, normalizedTimeFormat(QString()));
    appendUniqueFormat(&timeFormats, QStringLiteral("HH:mm"));
    for (const QString& format : timeFormats) {
      if (format.isEmpty()) {
        continue;
      }
      const QTime time = QTime::fromString(trimmed, format);
      if (time.isValid()) {
        if (ok) {
          *ok = true;
        }
        return QDateTime(todayDate(), normalizedTimeValue(time));
      }
    }
    return {};
  }

  bool dateOk = false;
  const QDate date = parsePickerText(mode, trimmed, formats, locale, firstDayOfWeek, &dateOk);
  if (!dateOk || !date.isValid()) {
    return {};
  }
  if (ok) {
    *ok = true;
  }
  return dateTimeFromParts(date, fallbackTime);
}

detail::DatePickerVisualStyle resolveStyleForPanel(const AdDatePickerPanel* panel) {
  detail::DatePickerStyleInput input;
  input.componentTokens = panel ? panel->componentTokens() : AdDatePickerPanel::ComponentTokens();
  input.semanticStyles = panel ? panel->semanticStyles() : AdDatePickerPanel::SemanticStyles();
  input.baseFont = panel ? panel->font() : qApp->font();
  input.disabled = panel && panel->disabled();
  return detail::resolveDatePickerVisualStyle(input,
                                              adqt::theme::ThemeManager::instance().resolve(panel));
}

void setButtonPalette(QToolButton* button, const QColor& color) {
  if (!button) {
    return;
  }
  QPalette palette = button->palette();
  palette.setColor(QPalette::ButtonText, color);
  palette.setColor(QPalette::Text, color);
  palette.setColor(QPalette::WindowText, color);
  palette.setColor(QPalette::Disabled, QPalette::ButtonText, color);
  palette.setColor(QPalette::Disabled, QPalette::Text, color);
  palette.setColor(QPalette::Disabled, QPalette::WindowText, color);
  button->setPalette(palette);
}

QString cssColor(const QColor& color) {
  if (!color.isValid()) {
    return QStringLiteral("transparent");
  }
  const QColor rgb = color.toRgb();
  return QStringLiteral("rgba(%1,%2,%3,%4)")
      .arg(rgb.red())
      .arg(rgb.green())
      .arg(rgb.blue())
      .arg(rgb.alpha());
}

constexpr int kTimeColumnSpacerRole = Qt::UserRole + 100;

class DatePickerTimeColumnList final : public QListWidget {
 public:
  explicit DatePickerTimeColumnList(QWidget* parent = nullptr) : QListWidget(parent) {
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setMouseTracking(true);
    if (viewport()) {
      viewport()->setMouseTracking(true);
      viewport()->installEventFilter(this);
      overlayVerticalScrollBar_ = new QScrollBar(Qt::Vertical, viewport());
      overlayVerticalScrollBar_->setObjectName(
          QStringLiteral("addatepicker-panel-time-column-overlay-vbar"));
      overlayVerticalScrollBar_->setFocusPolicy(Qt::NoFocus);
      overlayVerticalScrollBar_->setAttribute(Qt::WA_Hover, true);
      overlayVerticalScrollBar_->setMouseTracking(true);
      overlayVerticalScrollBar_->installEventFilter(this);
      overlayVerticalScrollBar_->hide();
      overlayVerticalScrollBar_->raise();
    }

    if (QScrollBar* source = verticalScrollBar()) {
      connect(source, &QScrollBar::rangeChanged, this,
              [this](int, int) { syncOverlayScrollBar(); });
      connect(source, &QScrollBar::valueChanged, this, [this](int) { syncOverlayScrollBar(); });
    }
    if (overlayVerticalScrollBar_) {
      connect(overlayVerticalScrollBar_, &QScrollBar::valueChanged, this, [this](int value) {
        QScrollBar* source = verticalScrollBar();
        if (!source || source->value() == value) {
          return;
        }
        source->setValue(value);
      });
    }
  }

  void applyOverlayScrollBarStyle(const detail::DatePickerVisualStyle& style) {
    Q_UNUSED(style)
    applyOverlayScrollBarStyle();
  }

 protected:
  bool event(QEvent* event) override {
    handleOverlayEvent(event);
    return QListWidget::event(event);
  }

  bool eventFilter(QObject* watched, QEvent* event) override {
    if (watched == viewport() || watched == overlayVerticalScrollBar_) {
      handleOverlayEvent(event);
    }
    return QListWidget::eventFilter(watched, event);
  }

  void resizeEvent(QResizeEvent* event) override {
    QListWidget::resizeEvent(event);
    updateOverlayGeometry();
    syncOverlayScrollBar();
  }

  void showEvent(QShowEvent* event) override {
    QListWidget::showEvent(event);
    syncOverlayScrollBar();
  }

  void hideEvent(QHideEvent* event) override {
    overlayHovered_ = false;
    syncOverlayScrollBar();
    QListWidget::hideEvent(event);
  }

 private:
  static constexpr int kScrollBarThickness = 6;
  static constexpr int kScrollBarExpandedThickness = 8;
  static constexpr int kScrollBarCollapsedVisualThickness = 3;
  static constexpr int kScrollBarMargin = 2;

  void handleOverlayEvent(QEvent* event) {
    if (!event) {
      return;
    }
    switch (event->type()) {
      case QEvent::Enter:
      case QEvent::HoverEnter:
      case QEvent::MouseMove:
      case QEvent::HoverMove:
        setOverlayHovered(pointerInsideExpandTrigger());
        break;
      case QEvent::Leave:
      case QEvent::HoverLeave:
        scheduleOverlayHoverUpdate();
        break;
      case QEvent::Resize:
      case QEvent::LayoutRequest:
      case QEvent::Show:
        updateOverlayGeometry();
        syncOverlayScrollBar();
        break;
      case QEvent::Hide:
        setOverlayHovered(false);
        break;
      default:
        break;
    }
  }

  bool pointerInsideExpandTrigger() const {
    if (overlayVerticalScrollBar_ && overlayVerticalScrollBar_->underMouse()) {
      return true;
    }
    if (!viewport()) {
      return false;
    }
    const QPoint viewportPos = viewport()->mapFromGlobal(QCursor::pos());
    return scrollBarTriggerRect().contains(viewportPos);
  }

  QRect scrollBarTriggerRect() const {
    if (!viewport()) {
      return QRect();
    }
    const int width = overlayHovered_ ? kScrollBarExpandedThickness : kScrollBarThickness;
    const int height = std::max(0, viewport()->height() - kScrollBarMargin * 2);
    const int x = std::max(0, viewport()->width() - width - kScrollBarMargin);
    return QRect(x, kScrollBarMargin, width, height);
  }

  void scheduleOverlayHoverUpdate() {
    QPointer<DatePickerTimeColumnList> guard(this);
    QTimer::singleShot(0, this, [guard]() {
      if (!guard) {
        return;
      }
      guard->setOverlayHovered(guard->pointerInsideExpandTrigger());
    });
  }

  void setOverlayHovered(bool hovered) {
    if (overlayHovered_ == hovered) {
      syncOverlayScrollBar();
      return;
    }
    overlayHovered_ = hovered;
    applyOverlayScrollBarStyle();
  }

  void applyOverlayScrollBarStyle() {
    if (!overlayVerticalScrollBar_) {
      return;
    }
    const int thickness = kScrollBarThickness;
    const int extent = overlayHovered_ ? kScrollBarExpandedThickness : thickness;
    const int collapsedInset = std::max(0, thickness - kScrollBarCollapsedVisualThickness);
    const int inset = overlayHovered_ ? 0 : collapsedInset;
    const int visualWidth = std::max(1, extent - inset);
    const int radius = std::max(1, (visualWidth + 1) / 2);
    adqt::widgets::detail::applyThemedScrollBar(overlayVerticalScrollBar_, extent, radius, inset);
    updateOverlayGeometry();
    syncOverlayScrollBar();
  }

  void syncOverlayScrollBar() {
    if (!overlayVerticalScrollBar_) {
      return;
    }
    QScrollBar* source = verticalScrollBar();
    if (!source) {
      const bool wasHovered = overlayHovered_;
      overlayHovered_ = false;
      if (wasHovered) {
        applyOverlayScrollBarStyle();
      }
      overlayVerticalScrollBar_->hide();
      return;
    }

    {
      QSignalBlocker blocker(overlayVerticalScrollBar_);
      overlayVerticalScrollBar_->setRange(source->minimum(), source->maximum());
      overlayVerticalScrollBar_->setPageStep(source->pageStep());
      overlayVerticalScrollBar_->setSingleStep(source->singleStep());
      overlayVerticalScrollBar_->setValue(source->value());
    }

    const bool visible = source->maximum() > source->minimum() && isVisible();
    if (!visible && overlayHovered_) {
      overlayHovered_ = false;
      applyOverlayScrollBarStyle();
    }
    overlayVerticalScrollBar_->setVisible(visible);
    updateOverlayGeometry();
    if (visible) {
      overlayVerticalScrollBar_->raise();
    }
  }

  void updateOverlayGeometry() {
    if (!overlayVerticalScrollBar_ || !viewport()) {
      return;
    }
    overlayVerticalScrollBar_->setGeometry(scrollBarTriggerRect());
    overlayVerticalScrollBar_->raise();
  }

  QScrollBar* overlayVerticalScrollBar_ = nullptr;
  bool overlayHovered_ = false;
};

bool isTimeColumnSpacerItem(const QListWidgetItem* item) {
  return item && item->data(kTimeColumnSpacerRole).toBool();
}

void syncTimeColumnSpacer(QListWidget* list, const detail::DatePickerVisualStyle& style,
                          int columnHeight) {
  if (!list) {
    return;
  }

  QListWidgetItem* spacer = nullptr;
  for (int i = list->count() - 1; i >= 0; --i) {
    QListWidgetItem* item = list->item(i);
    if (!isTimeColumnSpacerItem(item)) {
      continue;
    }
    if (!spacer) {
      spacer = item;
    } else {
      delete list->takeItem(i);
    }
  }

  if (!spacer) {
    spacer = new QListWidgetItem();
    spacer->setData(kTimeColumnSpacerRole, true);
    list->addItem(spacer);
  }

  const int spacerHeight = std::max(0, columnHeight - style.metrics.timeCellHeight);
  spacer->setText(QString());
  spacer->setFlags(Qt::NoItemFlags);
  spacer->setSizeHint(QSize(style.metrics.timeColumnWidth, spacerHeight));
  if (list->row(spacer) != list->count() - 1) {
    list->takeItem(list->row(spacer));
    list->addItem(spacer);
  }
}

void applyPresetButtonStyle(QToolButton* button, const detail::DatePickerVisualStyle& style) {
  if (!button) {
    return;
  }
  button->setFont(style.metrics.smallFont);
  button->setToolButtonStyle(Qt::ToolButtonTextOnly);
  button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  button->setFixedHeight(std::max(18, style.metrics.cellHeight));
  setButtonPalette(button, style.textColor);
  button->setStyleSheet(
      QStringLiteral("QToolButton { background: transparent; border: none; border-radius: %1px; "
                     "padding: 0 8px; text-align: left; color: %2; }"
                     "QToolButton:hover { background: %3; color: %2; }"
                     "QToolButton:disabled { color: %4; background: transparent; }")
          .arg(std::max(0, style.metrics.cellRadius))
          .arg(cssColor(style.textColor), cssColor(style.hoverBackground),
               cssColor(style.disabledTextColor)));
}

QString timeColumnStyleSheet(const detail::DatePickerVisualStyle& style, bool leadingDivider) {
  const int timeCellPaddingStart =
      std::max(0, (style.metrics.timeColumnWidth - style.metrics.timeCellHeight) / 2);
  const QString borderStyle =
      leadingDivider ? QStringLiteral(
                           "border-left: %1px solid %2; border-top: none; border-right: none; "
                           "border-bottom: none;")
                           .arg(std::max(1, style.metrics.borderWidth))
                           .arg(cssColor(style.panelBorderColor))
                     : QStringLiteral("border: none;");
  return QStringLiteral(
             "QListWidget { background: transparent; %1 outline: 0; color: %2; }"
             "QListWidget::item { margin: 0 4px; padding: 0 0 0 %3px; "
             "border-radius: %4px; }"
             "QListWidget::item:hover { background: %5; }"
             "QListWidget::item:selected { background: %6; color: %2; }"
             "QListWidget::item:disabled { color: %7; background: transparent; }")
      .arg(borderStyle)
      .arg(cssColor(style.textColor))
      .arg(timeCellPaddingStart)
      .arg(std::max(0, style.metrics.cellRadius))
      .arg(cssColor(style.hoverBackground))
      .arg(cssColor(style.rangeBackground))
      .arg(cssColor(style.disabledTextColor));
}

int effectiveTimeColumnListHeight(const QListWidget* list,
                                  const detail::DatePickerVisualStyle& style) {
  int hostHeight = 0;
  const QWidget* host = list ? list->parentWidget() : nullptr;
  if (host) {
    hostHeight = host->height() > 0 ? host->height() : host->minimumHeight();
  }
  if (hostHeight <= 0) {
    hostHeight = style.metrics.timeColumnHeight;
  }

  QMargins margins(0, style.metrics.timeColumnMarginVertical, 0,
                   style.metrics.timeColumnMarginVertical);
  if (host && host->layout()) {
    margins = host->layout()->contentsMargins();
  }
  return std::max(1, hostHeight - margins.top() - margins.bottom());
}

void applyTimeColumnVisualStyle(QListWidget* list, const detail::DatePickerVisualStyle& style,
                                bool leadingDivider) {
  if (!list) {
    return;
  }
  const int columnHeight = effectiveTimeColumnListHeight(list, style);
  list->setFont(style.metrics.font);
  list->setFixedWidth(style.metrics.timeColumnWidth);
  list->setFixedHeight(columnHeight);
  list->setStyleSheet(timeColumnStyleSheet(style, leadingDivider));
  for (int i = 0; i < list->count(); ++i) {
    if (QListWidgetItem* item = list->item(i)) {
      if (isTimeColumnSpacerItem(item)) {
        continue;
      }
      item->setSizeHint(QSize(style.metrics.timeColumnWidth, style.metrics.timeCellHeight));
    }
  }
  syncTimeColumnSpacer(list, style, columnHeight);
  if (auto* timeList = dynamic_cast<DatePickerTimeColumnList*>(list)) {
    timeList->applyOverlayScrollBarStyle(style);
  }
}

bool presetItemsEqual(const AdDatePickerPanel::PresetItem& lhs,
                      const AdDatePickerPanel::PresetItem& rhs) {
  const auto hasProviders = [](const AdDatePickerPanel::PresetItem& item) {
    return static_cast<bool>(item.valueProvider) ||
           static_cast<bool>(item.rangeStartValueProvider) ||
           static_cast<bool>(item.rangeEndValueProvider) ||
           static_cast<bool>(item.rangeValueProvider);
  };
  if (hasProviders(lhs) || hasProviders(rhs)) {
    return false;
  }
  return lhs.label == rhs.label && lhs.value == rhs.value &&
         lhs.rangeStartValue == rhs.rangeStartValue && lhs.rangeEndValue == rhs.rangeEndValue;
}

bool presetsEqual(const QVector<AdDatePickerPanel::PresetItem>& lhs,
                  const QVector<AdDatePickerPanel::PresetItem>& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (int i = 0; i < lhs.size(); ++i) {
    if (!presetItemsEqual(lhs.at(i), rhs.at(i))) {
      return false;
    }
  }
  return true;
}

QDate resolvedPresetValue(const AdDatePickerPanel::PresetItem& preset) {
  return preset.valueProvider ? preset.valueProvider() : preset.value;
}

std::pair<QDate, QDate> resolvedPresetRange(const AdDatePickerPanel::PresetItem& preset) {
  if (preset.rangeValueProvider) {
    return preset.rangeValueProvider();
  }
  const QDate start =
      preset.rangeStartValueProvider ? preset.rangeStartValueProvider() : preset.rangeStartValue;
  const QDate end =
      preset.rangeEndValueProvider ? preset.rangeEndValueProvider() : preset.rangeEndValue;
  return {start, end};
}

bool iconRefsEqual(const adqt::icons::IconRef& lhs, const adqt::icons::IconRef& rhs) {
  return lhs == rhs;
}

bool semanticSlotStyleEqual(const AdDatePickerPanel::SemanticSlotStyle& lhs,
                            const AdDatePickerPanel::SemanticSlotStyle& rhs) {
  return lhs.textColor == rhs.textColor && lhs.backgroundColor == rhs.backgroundColor &&
         lhs.borderColor == rhs.borderColor;
}

bool panelSemanticStylesEqual(const AdDatePickerPanel::SemanticStyles& lhs,
                              const AdDatePickerPanel::SemanticStyles& rhs) {
  return semanticSlotStyleEqual(lhs.root, rhs.root) &&
         semanticSlotStyleEqual(lhs.header, rhs.header) &&
         semanticSlotStyleEqual(lhs.body, rhs.body) &&
         semanticSlotStyleEqual(lhs.content, rhs.content) &&
         semanticSlotStyleEqual(lhs.item, rhs.item) &&
         semanticSlotStyleEqual(lhs.footer, rhs.footer) &&
         semanticSlotStyleEqual(lhs.container, rhs.container);
}

bool panelComponentTokensEqual(const AdDatePickerPanel::ComponentTokens& lhs,
                               const AdDatePickerPanel::ComponentTokens& rhs) {
  return lhs.panelWidth == rhs.panelWidth && lhs.presetsWidth == rhs.presetsWidth &&
         lhs.presetsMaxWidth == rhs.presetsMaxWidth && lhs.zIndexPopup == rhs.zIndexPopup &&
         lhs.timeColumnWidth == rhs.timeColumnWidth &&
         lhs.timeColumnHeight == rhs.timeColumnHeight && lhs.timeCellHeight == rhs.timeCellHeight &&
         lhs.cellWidth == rhs.cellWidth && lhs.cellHeight == rhs.cellHeight &&
         lhs.textHeight == rhs.textHeight &&
         lhs.withoutTimeCellHeight == rhs.withoutTimeCellHeight &&
         lhs.multipleItemHeight == rhs.multipleItemHeight &&
         lhs.multipleItemHeightSmall == rhs.multipleItemHeightSmall &&
         lhs.multipleItemHeightLarge == rhs.multipleItemHeightLarge &&
         lhs.borderRadius == rhs.borderRadius && lhs.panelBackground == rhs.panelBackground &&
         lhs.panelBorderColor == rhs.panelBorderColor &&
         lhs.cellHoverBackground == rhs.cellHoverBackground &&
         lhs.cellSelectedBackground == rhs.cellSelectedBackground &&
         lhs.cellRangeBackground == rhs.cellRangeBackground &&
         lhs.cellRangeHoverBackground == rhs.cellRangeHoverBackground &&
         lhs.cellRangeBorderColor == rhs.cellRangeBorderColor &&
         lhs.multipleItemBackground == rhs.multipleItemBackground &&
         lhs.multipleItemBorderColor == rhs.multipleItemBorderColor &&
         lhs.multipleItemTextDisabledColor == rhs.multipleItemTextDisabledColor &&
         lhs.multipleItemBorderColorDisabled == rhs.multipleItemBorderColorDisabled &&
         lhs.textColor == rhs.textColor && lhs.textDisabledColor == rhs.textDisabledColor;
}

bool datePickerSemanticStylesEqual(const AdDatePicker::SemanticStyles& lhs,
                                   const AdDatePicker::SemanticStyles& rhs) {
  return semanticSlotStyleEqual(lhs.root, rhs.root) &&
         semanticSlotStyleEqual(lhs.prefix, rhs.prefix) &&
         semanticSlotStyleEqual(lhs.input, rhs.input) &&
         semanticSlotStyleEqual(lhs.suffix, rhs.suffix) &&
         panelSemanticStylesEqual(lhs.popup, rhs.popup);
}

void setSemanticSlot(QWidget* widget, const char* slotName, const QString& objectName = QString()) {
  if (!widget || !slotName) {
    return;
  }
  widget->setProperty("adqt.semanticSlot", QString::fromLatin1(slotName));
  widget->setProperty("adqt.datePicker.semanticSlot", QString::fromLatin1(slotName));
  if (!objectName.isEmpty() && widget->objectName().isEmpty()) {
    widget->setObjectName(objectName);
  }
}

void setOptionalColorProperty(QObject* object, const char* name,
                              const std::optional<QColor>& color) {
  if (!object || !name) {
    return;
  }
  object->setProperty(
      name, color.has_value() && color->isValid() ? QVariant::fromValue(*color) : QVariant());
}

std::optional<QColor> mergedColor(const std::optional<QColor>& preferred,
                                  const std::optional<QColor>& fallback) {
  return preferred.has_value() ? preferred : fallback;
}

void applyInputSemanticColors(AdLineEdit* lineEdit, const AdDatePicker::SemanticStyles& semantic) {
  if (!lineEdit) {
    return;
  }

  setOptionalColorProperty(
      lineEdit, "ad-input-background-color",
      mergedColor(semantic.input.backgroundColor, semantic.root.backgroundColor));
  setOptionalColorProperty(lineEdit, "ad-input-border-color",
                           mergedColor(semantic.input.borderColor, semantic.root.borderColor));
  setOptionalColorProperty(lineEdit, "ad-input-text-color",
                           mergedColor(semantic.input.textColor, semantic.root.textColor));
  setOptionalColorProperty(lineEdit, "ad-input-placeholder-color",
                           mergedColor(semantic.input.textColor, semantic.root.textColor));
  setOptionalColorProperty(lineEdit, "ad-input-prefix-color", semantic.prefix.textColor);
  setOptionalColorProperty(lineEdit, "ad-input-suffix-color", semantic.suffix.textColor);
  setOptionalColorProperty(lineEdit, "ad-input-suffix-action-color", semantic.suffix.textColor);
}

QColor pickerSuffixColor(const QWidget* widget) {
  const adqt::theme::ThemeMapToken map = adqt::theme::ThemeManager::instance().resolveTheme(widget);
  return map.colorTextQuaternary.isValid() ? map.colorTextQuaternary
                                           : QColor(QStringLiteral("#bfbfbf"));
}

void applyPickerSuffixTokenColors(AdLineEdit* lineEdit,
                                  const AdDatePicker::SemanticStyles& semantic) {
  if (!lineEdit || semantic.suffix.textColor.has_value()) {
    return;
  }

  const QColor suffixColor = pickerSuffixColor(lineEdit);
  lineEdit->setProperty("ad-input-suffix-color", QVariant::fromValue(suffixColor));
  lineEdit->setProperty("ad-input-suffix-action-color", QVariant::fromValue(suffixColor));
}

constexpr char kCompactHeightProperty[] = "ad-input-height";
constexpr char kCompactHorizontalPaddingProperty[] = "ad-input-horizontal-padding";
constexpr char kCompactFontPixelSizeProperty[] = "ad-input-font-pixel-size";
constexpr char kCompactIconSizeProperty[] = "ad-input-icon-size";
constexpr char kSemanticBackgroundColorProperty[] = "ad-input-background-color";
constexpr char kSemanticHoverBackgroundColorProperty[] = "ad-input-hover-background-color";
constexpr char kSemanticActiveBackgroundColorProperty[] = "ad-input-active-background-color";
constexpr char kSemanticBorderColorProperty[] = "ad-input-border-color";
constexpr char kSemanticHoverBorderColorProperty[] = "ad-input-hover-border-color";
constexpr char kSemanticActiveBorderColorProperty[] = "ad-input-active-border-color";
constexpr char kSemanticTextColorProperty[] = "ad-input-text-color";
constexpr char kSemanticPlaceholderColorProperty[] = "ad-input-placeholder-color";
constexpr char kSemanticPrefixColorProperty[] = "ad-input-prefix-color";
constexpr char kSemanticSuffixColorProperty[] = "ad-input-suffix-color";
constexpr char kSemanticSuffixActionColorProperty[] = "ad-input-suffix-action-color";

QVariant dynamicStyleProperty(const QObject* object, const char* name) {
  return object ? object->property(name) : QVariant();
}

QColor dynamicStyleColorProperty(const QObject* object, const char* name) {
  const QVariant value = dynamicStyleProperty(object, name);
  if (!value.isValid()) {
    return QColor();
  }
  if (value.canConvert<QColor>()) {
    const QColor color = qvariant_cast<QColor>(value);
    if (color.isValid()) {
      return color;
    }
  }
  if (value.canConvert<QString>()) {
    const QColor color(value.toString());
    if (color.isValid()) {
      return color;
    }
  }
  return QColor();
}

void applyDynamicStyleColor(QColor* target, const QObject* object, const char* name) {
  if (!target) {
    return;
  }
  const QColor color = dynamicStyleColorProperty(object, name);
  if (color.isValid()) {
    *target = color;
  }
}

detail::InputVisualStyle applyInputDynamicOverrides(detail::InputVisualStyle style,
                                                    const QObject* object) {
  const QVariant heightValue = dynamicStyleProperty(object, kCompactHeightProperty);
  if (heightValue.isValid()) {
    style.metrics.height = std::max(18, heightValue.toInt());
  }

  const QVariant paddingValue = dynamicStyleProperty(object, kCompactHorizontalPaddingProperty);
  if (paddingValue.isValid()) {
    style.metrics.horizontalPadding = std::max(2, paddingValue.toInt());
  }

  const QVariant fontPixelValue = dynamicStyleProperty(object, kCompactFontPixelSizeProperty);
  if (fontPixelValue.isValid()) {
    style.metrics.font.setPixelSize(std::max(8, fontPixelValue.toInt()));
  }

  const QVariant iconSizeValue = dynamicStyleProperty(object, kCompactIconSizeProperty);
  if (iconSizeValue.isValid()) {
    const int iconSize = std::max(8, iconSizeValue.toInt());
    style.metrics.affixIconSize = iconSize;
    style.metrics.clearIconSize = iconSize;
  } else if (style.metrics.font.pixelSize() > 0) {
    style.metrics.affixIconSize = std::max(8, style.metrics.font.pixelSize());
  }

  const QColor backgroundColor =
      dynamicStyleColorProperty(object, kSemanticBackgroundColorProperty);
  if (backgroundColor.isValid()) {
    style.selectorBg = backgroundColor;
    style.selectorHoverBg = backgroundColor;
    style.selectorActiveBg = backgroundColor;
  }
  applyDynamicStyleColor(&style.selectorHoverBg, object, kSemanticHoverBackgroundColorProperty);
  applyDynamicStyleColor(&style.selectorActiveBg, object, kSemanticActiveBackgroundColorProperty);

  const QColor borderColor = dynamicStyleColorProperty(object, kSemanticBorderColorProperty);
  if (borderColor.isValid()) {
    style.selectorBorderColor = borderColor;
    style.selectorHoverBorderColor = borderColor;
    style.selectorActiveBorderColor = borderColor;
  }
  applyDynamicStyleColor(&style.selectorHoverBorderColor, object,
                         kSemanticHoverBorderColorProperty);
  applyDynamicStyleColor(&style.selectorActiveBorderColor, object,
                         kSemanticActiveBorderColorProperty);

  applyDynamicStyleColor(&style.selectorTextColor, object, kSemanticTextColorProperty);
  applyDynamicStyleColor(&style.placeholderColor, object, kSemanticPlaceholderColorProperty);
  applyDynamicStyleColor(&style.prefixColor, object, kSemanticPrefixColorProperty);
  applyDynamicStyleColor(&style.suffixColor, object, kSemanticSuffixColorProperty);
  applyDynamicStyleColor(&style.suffixActionColor, object, kSemanticSuffixActionColorProperty);
  applyDynamicStyleColor(&style.suffixActionHoverColor, object, kSemanticSuffixActionColorProperty);

  return style;
}

QColor rangeInputBackgroundColor(const detail::InputVisualStyle& style, bool focused,
                                 bool hovered) {
  if (focused) {
    return style.selectorActiveBg;
  }
  if (hovered) {
    return style.selectorHoverBg;
  }
  return style.selectorBg;
}

QColor rangeInputBorderColor(const detail::InputVisualStyle& style, bool focused, bool hovered) {
  if (focused) {
    return style.selectorActiveBorderColor;
  }
  if (hovered) {
    return style.selectorHoverBorderColor;
  }
  return style.selectorBorderColor;
}

detail::InputVisualStyle resolveRangeLineEditStyle(const AdLineEdit* lineEdit, bool active) {
  detail::InputStyleInput input;
  if (lineEdit) {
    input.controlSize = lineEdit->controlSize();
    input.variant = lineEdit->variant();
    input.status = lineEdit->status();
    input.disabled = !lineEdit->isEnabled();
    input.focused = active;
    input.hovered = lineEdit->underMouse();
    input.baseFont = lineEdit->font();
  }
  return applyInputDynamicOverrides(
      detail::resolveInputVisualStyle(input,
                                      adqt::theme::ThemeManager::instance().resolve(lineEdit)),
      lineEdit);
}

QWidget* wrappedPopupContent(QWidget* popup, QWidget* originPanel,
                             const std::function<QWidget*(QWidget*, QWidget*)>& wrapperFactory) {
  if (!originPanel) {
    return nullptr;
  }
  QWidget* content = originPanel;
  if (wrapperFactory) {
    if (QWidget* rendered = wrapperFactory(originPanel, popup)) {
      content = rendered;
    }
  }
  if (content) {
    content->setProperty("adqt.popupContentWrapper", true);
    if (!content->parentWidget() && popup) {
      content->setParent(popup);
    }
  }
  return content;
}

enum class NavigationIconSlot : std::uint8_t {
  SuperPrev,
  Prev,
  Next,
  SuperNext,
};

adqt::icons::IconRef defaultPickerSuffixIcon(AdDatePickerPanel::PickerMode mode) {
  return mode == AdDatePickerPanel::PickerMode::Time ? outlined_icons::ClockCircle()
                                                     : outlined_icons::Calendar();
}

QString defaultPickerSuffixIconName(AdDatePickerPanel::PickerMode mode) {
  return mode == AdDatePickerPanel::PickerMode::Time ? QStringLiteral("clock-circle")
                                                     : QStringLiteral("calendar");
}

void setPanelToolButtonIcon(QToolButton* button, const adqt::icons::IconRef& icon,
                            const QString& fallbackText, int iconSide) {
  if (!button) {
    return;
  }

  if (adqt::icons::isValid(icon)) {
    const QSize iconSize(iconSide, iconSide);
    const QPixmap pixmap =
        adqt::icons::renderIconPixmap(icon, {iconSize, button->devicePixelRatioF()});
    if (!pixmap.isNull()) {
      button->setText(QString());
      button->setIcon(QIcon(pixmap));
      button->setIconSize(iconSize);
      button->setToolButtonStyle(Qt::ToolButtonIconOnly);
      button->setMinimumWidth(std::max(24, iconSide + 12));
      return;
    }
  }

  button->setIcon(QIcon());
  button->setText(fallbackText);
  button->setToolButtonStyle(Qt::ToolButtonTextOnly);
  button->setMinimumWidth(0);
}

void setPanelNavigationButtonIcon(QToolButton* button, const adqt::icons::IconRef& customIcon,
                                  NavigationIconSlot slot, const QColor& color, int iconSide) {
  if (!button) {
    return;
  }
  if (adqt::icons::isValid(customIcon)) {
    setPanelToolButtonIcon(button, customIcon, QString(), iconSide);
    return;
  }

  adqt::icons::IconRef fallback;
  switch (slot) {
    case NavigationIconSlot::SuperPrev:
      fallback = outlined_icons::DoubleLeft();
      break;
    case NavigationIconSlot::Prev:
      fallback = outlined_icons::Left();
      break;
    case NavigationIconSlot::Next:
      fallback = outlined_icons::Right();
      break;
    case NavigationIconSlot::SuperNext:
      fallback = outlined_icons::DoubleRight();
      break;
  }
  setPanelToolButtonIcon(button, fallback.withColors(adqt::icons::IconColors::primary(color)),
                         QString(), iconSide);
}

AdDatePickerPanel::PickerMode normalizedPanelPickerMode(AdDatePickerPanel::PickerMode value) {
  switch (value) {
    case AdDatePickerPanel::PickerMode::Month:
    case AdDatePickerPanel::PickerMode::Quarter:
    case AdDatePickerPanel::PickerMode::Year:
    case AdDatePickerPanel::PickerMode::Decade:
    case AdDatePickerPanel::PickerMode::Time:
      return value;
    case AdDatePickerPanel::PickerMode::Date:
    case AdDatePickerPanel::PickerMode::Week:
    default:
      return AdDatePickerPanel::PickerMode::Date;
  }
}

QString defaultRangeSeparator() { return QStringLiteral(" -> "); }

constexpr int kRangePopupArrowSize = 8;

int rangeSeparatorVisualSide(const QWidget* widget, const detail::InputVisualStyle& style) {
  const adqt::theme::ThemeMapToken map = adqt::theme::ThemeManager::instance().resolveTheme(widget);
  const int tokenSide = map.fontSizeLG > 0.0 ? qRound(map.fontSizeLG) : 0;
  const int fontSide = style.metrics.font.pixelSize() > 0 ? style.metrics.font.pixelSize() : 0;
  return std::max(10, std::max(tokenSide, fontSide));
}

QFont rangeSeparatorFont(const QWidget* widget, const detail::InputVisualStyle& style) {
  QFont font = style.metrics.font;
  font.setPixelSize(rangeSeparatorVisualSide(widget, style));
  return font;
}

QColor rangeSeparatorColor(const detail::InputVisualStyle& style, bool active, bool enabled) {
  QColor color = active ? style.suffixActionColor : style.clearColor;
  if (!enabled) {
    color = style.disabledTextColor;
  }
  if (!color.isValid() || color.alpha() == 0) {
    color = active ? QColor(QStringLiteral("#8c8c8c")) : QColor(QStringLiteral("#bfbfbf"));
  }
  return color;
}

bool rangeEndpointsAcceptable(const QDate& start, const QDate& end, bool allowEmptyStart,
                              bool allowEmptyEnd) {
  if (!start.isValid() && !end.isValid()) {
    return false;
  }
  return (start.isValid() || allowEmptyStart) && (end.isValid() || allowEmptyEnd);
}

QVector<QDate> normalizedDateVector(AdDatePickerPanel::PickerMode mode,
                                    const QVector<QDate>& values, Qt::DayOfWeek firstDayOfWeek,
                                    bool order) {
  QVector<QDate> out;
  QSet<qint64> seen;
  out.reserve(values.size());
  seen.reserve(values.size());
  for (const QDate& value : values) {
    const QDate normalized = normalizeForPicker(mode, value, firstDayOfWeek);
    if (normalized.isValid() && !seen.contains(normalized.toJulianDay())) {
      seen.insert(normalized.toJulianDay());
      out.append(normalized);
    }
  }
  if (order) {
    std::sort(out.begin(), out.end());
  }
  return out;
}

bool dateVectorContainsPickerValue(const QVector<QDate>& values, AdDatePickerPanel::PickerMode mode,
                                   const QDate& value, Qt::DayOfWeek firstDayOfWeek) {
  if (!value.isValid()) {
    return false;
  }
  const QDate normalized = normalizeForPicker(mode, value, firstDayOfWeek);
  return normalized.isValid() && values.contains(normalized);
}

QToolButton* createPanelToolButton(QWidget* parent, const QString& text) {
  auto* button = new QToolButton(parent);
  button->setText(text);
  button->setAutoRaise(true);
  button->setCursor(Qt::PointingHandCursor);
  button->setFocusPolicy(Qt::NoFocus);
  button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);
  return button;
}

void populateTimeColumn(QListWidget* list, int maximum, int step) {
  if (!list) {
    return;
  }
  const QSignalBlocker blocker(list);
  list->clear();
  const int normalizedStep = normalizedTimeStep(step, maximum + 1);
  for (int value = 0; value <= maximum; value += normalizedStep) {
    auto* item = new QListWidgetItem(QStringLiteral("%1").arg(value, 2, 10, QLatin1Char('0')));
    item->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    item->setData(Qt::UserRole, value);
    list->addItem(item);
  }
}

int hour24FromDisplayHour(int hour, bool pm) {
  const int displayHour = std::clamp(hour, 1, 12);
  if (displayHour == 12) {
    return pm ? 12 : 0;
  }
  return pm ? displayHour + 12 : displayHour;
}

int displayHourFromHour24(int hour) {
  const int normalized = std::clamp(hour, 0, 23);
  const int displayHour = normalized % 12;
  return displayHour == 0 ? 12 : displayHour;
}

void populateHourColumn(QListWidget* list, int step, bool use12Hours) {
  if (!list) {
    return;
  }
  if (!use12Hours) {
    populateTimeColumn(list, 23, step);
    return;
  }
  const QSignalBlocker blocker(list);
  list->clear();
  const int normalizedStep = normalizedTimeStep(step, 12);
  for (int offset = 0; offset < 12; offset += normalizedStep) {
    const int value = offset == 0 ? 12 : offset;
    auto* item = new QListWidgetItem(QStringLiteral("%1").arg(value, 2, 10, QLatin1Char('0')));
    item->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    item->setData(Qt::UserRole, value);
    list->addItem(item);
  }
}

void populateMeridiemColumn(QListWidget* list, const QLocale& locale) {
  if (!list) {
    return;
  }
  const QSignalBlocker blocker(list);
  list->clear();
  const QString amText = locale.amText().isEmpty() ? QStringLiteral("AM") : locale.amText();
  const QString pmText = locale.pmText().isEmpty() ? QStringLiteral("PM") : locale.pmText();
  const QStringList labels = {amText, pmText};
  for (int value = 0; value < labels.size(); ++value) {
    auto* item = new QListWidgetItem(labels.at(value));
    item->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    item->setData(Qt::UserRole, value);
    list->addItem(item);
  }
}

QListWidget* createTimeColumn(QWidget* parent, int maximum, int step = 1,
                              bool populate = true) {
  auto* list = new DatePickerTimeColumnList(parent);
  list->setFrameShape(QFrame::NoFrame);
  list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  list->setSelectionMode(QAbstractItemView::SingleSelection);
  list->setFocusPolicy(Qt::StrongFocus);
  list->setMouseTracking(true);
  if (list->viewport()) {
    list->viewport()->setMouseTracking(true);
  }
  list->setProperty("adqt.semantic.slot", QStringLiteral("popup.footer.time.column"));
  list->setProperty("adqt.semantic.class", QStringLiteral("addatepicker-panel-time-column"));
  if (populate) {
    populateTimeColumn(list, maximum, step);
  }
  return list;
}

QListWidget* createMeridiemColumn(QWidget* parent, const QLocale& locale, bool populate = true) {
  auto* list = createTimeColumn(parent, 1, 1, false);
  if (populate) {
    populateMeridiemColumn(list, locale);
  }
  return list;
}

}  // namespace

namespace detail {

class DatePickerLineEdit final : public AdLineEdit {
  struct RangeInputLayout {
    QRect contentRect;
    QRect startRect;
    QRect separatorRect;
    QRect endRect;
  };

 public:
  explicit DatePickerLineEdit(QWidget* parent = nullptr) : AdLineEdit(parent) {}

  void setRangeInputDisplay(const QString& startText, const QString& endText,
                            const QString& startPlaceholder, const QString& endPlaceholder,
                            const QString& separatorText, bool defaultSeparatorIcon,
                            AdDateRangePicker::RangePart activeRange, bool active) {
    if (rangeInputDisplayEnabled_ && rangeStartText_ == startText && rangeEndText_ == endText &&
        rangeStartPlaceholder_ == startPlaceholder && rangeEndPlaceholder_ == endPlaceholder &&
        rangeSeparatorText_ == separatorText &&
        rangeDefaultSeparatorIcon_ == defaultSeparatorIcon && rangeActivePart_ == activeRange &&
        rangeActive_ == active) {
      return;
    }
    rangeInputDisplayEnabled_ = true;
    rangeStartText_ = startText;
    rangeEndText_ = endText;
    rangeStartPlaceholder_ = startPlaceholder;
    rangeEndPlaceholder_ = endPlaceholder;
    rangeSeparatorText_ = separatorText;
    rangeDefaultSeparatorIcon_ = defaultSeparatorIcon;
    rangeActivePart_ = activeRange;
    rangeActive_ = active;
    syncRangeInteractionFocusOverlay();
    update();
  }

  void clearRangeInputDisplay() {
    if (!rangeInputDisplayEnabled_) {
      return;
    }
    rangeInputDisplayEnabled_ = false;
    rangeStartText_.clear();
    rangeEndText_.clear();
    rangeStartPlaceholder_.clear();
    rangeEndPlaceholder_.clear();
    rangeSeparatorText_.clear();
    rangeDefaultSeparatorIcon_ = false;
    rangeActivePart_ = AdDateRangePicker::RangePart::Start;
    rangeActive_ = false;
    syncRangeInteractionFocusOverlay();
    update();
  }

  AdDateRangePicker::RangePart rangePartAt(const QPoint& pos) const {
    if (!rangeInputDisplayEnabled_) {
      return AdDateRangePicker::RangePart::Start;
    }
    const InputVisualStyle style = resolveRangeLineEditStyle(this, rangeActive_ || hasFocus());
    const RangeInputLayout layout = rangeLayout(style);
    if (layout.endRect.contains(pos)) {
      return AdDateRangePicker::RangePart::End;
    }
    if (layout.startRect.contains(pos)) {
      return AdDateRangePicker::RangePart::Start;
    }
    return pos.x() > layout.separatorRect.center().x() ? AdDateRangePicker::RangePart::End
                                                       : AdDateRangePicker::RangePart::Start;
  }

  int rangeInputPartCenterX(AdDateRangePicker::RangePart part) const {
    if (!rangeInputDisplayEnabled_) {
      return width() / 2;
    }
    const InputVisualStyle style = resolveRangeLineEditStyle(this, rangeActive_ || hasFocus());
    const RangeInputLayout layout = rangeLayout(style);
    const QRect partRect =
        part == AdDateRangePicker::RangePart::End ? layout.endRect : layout.startRect;
    return partRect.isValid() ? partRect.center().x() : width() / 2;
  }

  QRect rangeInputPartRect(AdDateRangePicker::RangePart part) const {
    if (!rangeInputDisplayEnabled_) {
      return rect();
    }
    const InputVisualStyle style = resolveRangeLineEditStyle(this, rangeActive_ || hasFocus());
    const RangeInputLayout layout = rangeLayout(style);
    const QRect partRect =
        part == AdDateRangePicker::RangePart::End ? layout.endRect : layout.startRect;
    return partRect.isValid() ? partRect : rect();
  }

  void setDateTagTokens(const AdDatePickerPanel::ComponentTokens& tokens, AdDatePicker::Size size) {
    if (dateTagTokens_.multipleItemHeight == tokens.multipleItemHeight &&
        dateTagTokens_.multipleItemHeightSmall == tokens.multipleItemHeightSmall &&
        dateTagTokens_.multipleItemHeightLarge == tokens.multipleItemHeightLarge &&
        dateTagTokens_.multipleItemBackground == tokens.multipleItemBackground &&
        dateTagTokens_.multipleItemBorderColor == tokens.multipleItemBorderColor &&
        dateTagTokens_.multipleItemTextDisabledColor == tokens.multipleItemTextDisabledColor &&
        dateTagTokens_.multipleItemBorderColorDisabled == tokens.multipleItemBorderColorDisabled &&
        dateTagSize_ == size) {
      return;
    }
    dateTagTokens_ = tokens;
    dateTagSize_ = size;
    update();
  }

  void setDateTagTexts(const QStringList& values, int maxTagCount, bool responsiveMaxTagCount) {
    const int normalizedMaxTagCount = maxTagCount < 0 ? -1 : maxTagCount;
    if (dateTagTexts_ == values && maxTagCount_ == normalizedMaxTagCount &&
        responsiveMaxTagCount_ == responsiveMaxTagCount) {
      return;
    }
    dateTagTexts_ = values;
    maxTagCount_ = normalizedMaxTagCount;
    responsiveMaxTagCount_ = responsiveMaxTagCount;
    update();
  }

  void clearDateTagTexts() { setDateTagTexts({}, -1, false); }

  QSize sizeHint() const override {
    QSize hint = AdLineEdit::sizeHint();
    if (rangeInputDisplayEnabled_) {
      hint.setWidth(std::max(hint.width(), 240));
    }
    if (!dateTagTexts_.isEmpty()) {
      hint.setWidth(std::max(220, std::min(hint.width(), 320)));
    }
    return hint;
  }

  using AdLineEdit::setClearOverlaysTrailingAction;
  using AdLineEdit::setTrailingActionAccessibleName;
  using AdLineEdit::setTrailingActionIconRef;
  using AdLineEdit::setTrailingActionLeading;
  using AdLineEdit::setTrailingActionVisible;
  using AdLineEdit::trailingActionButton;

 protected:
  void resizeEvent(QResizeEvent* event) override {
    AdLineEdit::resizeEvent(event);
    syncRangeInteractionFocusOverlay();
  }

  void moveEvent(QMoveEvent* event) override {
    AdLineEdit::moveEvent(event);
    syncRangeInteractionFocusOverlay();
  }

  void showEvent(QShowEvent* event) override {
    AdLineEdit::showEvent(event);
    syncRangeInteractionFocusOverlay();
  }

  void hideEvent(QHideEvent* event) override {
    AdLineEdit::hideEvent(event);
    syncRangeInteractionFocusOverlay();
  }

  void focusInEvent(QFocusEvent* event) override {
    AdLineEdit::focusInEvent(event);
    syncRangeInteractionFocusOverlay();
  }

  void focusOutEvent(QFocusEvent* event) override {
    AdLineEdit::focusOutEvent(event);
    syncRangeInteractionFocusOverlay();
  }

  void changeEvent(QEvent* event) override {
    AdLineEdit::changeEvent(event);
    if (event &&
        (event->type() == QEvent::EnabledChange || event->type() == QEvent::PaletteChange ||
         event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::FontChange ||
         event->type() == QEvent::ApplicationFontChange || event->type() == QEvent::StyleChange)) {
      syncRangeInteractionFocusOverlay();
    }
  }

  void paintEvent(QPaintEvent* event) override {
    if (rangeInputDisplayEnabled_ && !(hasFocus() && isModified())) {
      paintRangeInputDisplay(event);
      return;
    }

    AdLineEdit::paintEvent(event);
    if (dateTagTexts_.isEmpty() || text().isEmpty()) {
      return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QMargins margins = textMargins();
    QRect content =
        rect().adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());
    if (!content.isValid()) {
      return;
    }
    content.adjust(0, 1, 0, -1);
    painter.fillRect(content.adjusted(-1, 0, 1, 0), palette().color(QPalette::Base));

    const QFontMetrics fm(font());
    const int tagHeight = resolvedTagHeight(height() - margins.top() - margins.bottom());
    const int tagTop = content.top() + (content.height() - tagHeight) / 2;
    const int radius = 4;
    const int horizontalPadding = 7;
    const int gap = 4;
    const QColor tagBackground = dateTagTokens_.multipleItemBackground.value_or(QColor("#f5f5f5"));
    const QColor enabledTagBorder =
        dateTagTokens_.multipleItemBorderColor.value_or(QColor(0, 0, 0, 0));
    const QColor disabledTagBorder =
        dateTagTokens_.multipleItemBorderColorDisabled.value_or(enabledTagBorder);
    const QColor tagBorder = isEnabled() ? enabledTagBorder : disabledTagBorder;
    const QColor tagText = isEnabled() ? palette().color(QPalette::Text)
                                       : dateTagTokens_.multipleItemTextDisabledColor.value_or(
                                             palette().color(QPalette::Disabled, QPalette::Text));

    QStringList labels = dateTagTexts_;
    int visibleCount = static_cast<int>(labels.size());
    if (maxTagCount_ >= 0) {
      visibleCount = std::min(visibleCount, maxTagCount_);
    }
    if (responsiveMaxTagCount_) {
      visibleCount = std::min(visibleCount, responsiveVisibleCount(labels, content.width()));
    }
    visibleCount = std::clamp(visibleCount, 0, static_cast<int>(labels.size()));
    const int hiddenCount = static_cast<int>(labels.size()) - visibleCount;

    int x = content.left();
    const auto drawTag = [&](const QString& label) {
      const int remaining = content.right() - x + 1;
      if (remaining <= 0) {
        return false;
      }
      const int desiredWidth = fm.horizontalAdvance(label) + horizontalPadding * 2;
      const int width = std::min(std::max(tagHeight, desiredWidth), remaining);
      if (width <= 0) {
        return false;
      }
      const QRect tagRect(x, tagTop, width, tagHeight);
      const QString displayText =
          fm.elidedText(label, Qt::ElideRight, std::max(1, width - horizontalPadding * 2));
      QPainterPath path;
      path.addRoundedRect(QRectF(tagRect), radius, radius);
      painter.fillPath(path, tagBackground);
      if (tagBorder.alpha() > 0) {
        painter.setPen(QPen(tagBorder, 1));
        painter.drawPath(path);
      }
      painter.setPen(tagText);
      painter.drawText(tagRect.adjusted(horizontalPadding, 0, -horizontalPadding, 0),
                       Qt::AlignVCenter | Qt::AlignLeft, displayText);
      x += width + gap;
      return true;
    };

    for (int i = 0; i < visibleCount && i < labels.size(); ++i) {
      if (!drawTag(labels.at(i))) {
        break;
      }
    }
    if (hiddenCount > 0) {
      drawTag(QStringLiteral("+%1...").arg(hiddenCount));
    }
  }

 private:
  void syncRangeInteractionFocusOverlay() {
    if (!rangeInputDisplayEnabled_) {
      if (!hasFocus()) {
        stopInteractionFocusForOwner(this);
      }
      return;
    }

    const bool active = rangeActive_ || hasFocus();
    if (!active) {
      stopInteractionFocusForOwner(this);
      return;
    }

    input_internal::updateInputFocusOverlay(this, rect(), resolveRangeLineEditStyle(this, true),
                                            joinedLeft(), joinedRight());
  }

  RangeInputLayout rangeLayout(const InputVisualStyle& style) const {
    const QMargins margins = textMargins();
    QRect content =
        rect().adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());
    if (!content.isValid()) {
      return {};
    }

    const QFont separatorFont = rangeSeparatorFont(this, style);
    const QFontMetrics fm(separatorFont);
    const int separatorPadding = std::max(0, style.metrics.affixItemGap);
    const int iconSide = rangeSeparatorVisualSide(this, style);
    const QString textSeparator = rangeSeparatorText_.trimmed().isEmpty()
                                      ? defaultRangeSeparator().trimmed()
                                      : rangeSeparatorText_.trimmed();
    const int separatorContentWidth =
        rangeDefaultSeparatorIcon_ ? iconSide : fm.horizontalAdvance(textSeparator);
    const int separatorWidth =
        std::min(content.width(), std::max(iconSide, separatorContentWidth) + separatorPadding * 2);
    const int endpointWidth = std::max(0, content.width() - separatorWidth);
    const int startWidth = endpointWidth / 2;
    const int endWidth = endpointWidth - startWidth;

    RangeInputLayout layout;
    layout.contentRect = content;
    if (layoutDirection() == Qt::RightToLeft) {
      layout.endRect = QRect(content.left(), content.top(), endWidth, content.height());
      layout.separatorRect =
          QRect(layout.endRect.right() + 1, content.top(), separatorWidth, content.height());
      layout.startRect =
          QRect(layout.separatorRect.right() + 1, content.top(), startWidth, content.height());
    } else {
      layout.startRect = QRect(content.left(), content.top(), startWidth, content.height());
      layout.separatorRect =
          QRect(layout.startRect.right() + 1, content.top(), separatorWidth, content.height());
      layout.endRect =
          QRect(layout.separatorRect.right() + 1, content.top(), endWidth, content.height());
    }
    return layout;
  }

  void paintRangeInputDisplay(QPaintEvent* event) {
    Q_UNUSED(event)

    const bool active = rangeActive_ || hasFocus();
    const InputVisualStyle style = resolveRangeLineEditStyle(this, active);
    const bool hovered = underMouse();

    input_internal::InputFramePaintStyle frameStyle;
    frameStyle.background = rangeInputBackgroundColor(style, active, hovered);
    frameStyle.border = QColor(0, 0, 0, 0);
    frameStyle.borderWidth = std::max<qreal>(0.0, style.metrics.borderWidth);
    frameStyle.underlined = style.underlined;
    const qreal radius = style.underlined ? 0.0 : std::max<qreal>(0.0, style.metrics.borderRadius);
    frameStyle.topLeftRadius = joinedLeft() ? 0.0 : radius;
    frameStyle.topRightRadius = joinedRight() ? 0.0 : radius;
    frameStyle.bottomRightRadius = joinedRight() ? 0.0 : radius;
    frameStyle.bottomLeftRadius = joinedLeft() ? 0.0 : radius;
    frameStyle.joinedLeft = joinedLeft();
    frameStyle.joinedRight = joinedRight();

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    input_internal::paintInputFrame(&painter, rect(), frameStyle);

    const RangeInputLayout layout = rangeLayout(style);
    if (layout.contentRect.isValid()) {
      painter.setFont(style.metrics.font);
      const QColor textColor = isEnabled() ? style.selectorTextColor : style.disabledTextColor;
      const QColor placeholderColor =
          isEnabled() ? style.placeholderColor : style.disabledTextColor;
      drawRangeEndpointText(&painter, layout.startRect, rangeStartText_, rangeStartPlaceholder_,
                            textColor, placeholderColor);
      drawRangeSeparator(&painter, layout.separatorRect, style, active);
      drawRangeEndpointText(&painter, layout.endRect, rangeEndText_, rangeEndPlaceholder_,
                            textColor, placeholderColor);
    }

    frameStyle.background = QColor(0, 0, 0, 0);
    frameStyle.border = rangeInputBorderColor(style, active, hovered);
    input_internal::paintInputFrame(&painter, rect(), frameStyle);

    if (active && isEnabled() && layout.contentRect.isValid()) {
      const QRect activeRect =
          rangeActivePart_ == AdDateRangePicker::RangePart::End ? layout.endRect : layout.startRect;
      const int barHeight = std::max(2, qRound(style.metrics.borderWidth * 2.0));
      QColor barColor = style.selectorActiveBorderColor;
      if (!barColor.isValid() || barColor.alpha() == 0) {
        barColor = QColor("#1677ff");
      }
      painter.setPen(Qt::NoPen);
      painter.setBrush(barColor);
      painter.drawRect(
          QRect(activeRect.left(), rect().bottom() - barHeight + 1, activeRect.width(), barHeight));
    }
  }

  void drawRangeEndpointText(QPainter* painter, const QRect& rect, const QString& text,
                             const QString& placeholder, const QColor& textColor,
                             const QColor& placeholderColor) const {
    if (!painter || rect.width() <= 0 || rect.height() <= 0) {
      return;
    }

    const bool hasText = !text.isEmpty();
    const QString source = hasText ? text : placeholder;
    if (source.isEmpty()) {
      return;
    }

    const QFontMetrics fm(painter->font());
    const QString displayText = fm.elidedText(source, Qt::ElideRight, std::max(1, rect.width()));
    painter->setPen(hasText ? textColor : placeholderColor);
    const Qt::Alignment horizontal =
        layoutDirection() == Qt::RightToLeft ? Qt::AlignRight : Qt::AlignLeft;
    painter->drawText(rect, Qt::AlignVCenter | horizontal, displayText);
  }

  void drawRangeSeparator(QPainter* painter, const QRect& rect, const InputVisualStyle& style,
                          bool active) const {
    if (!painter || rect.width() <= 0 || rect.height() <= 0) {
      return;
    }

    const QColor separatorColor = rangeSeparatorColor(style, active, isEnabled());

    if (rangeDefaultSeparatorIcon_) {
      const auto colors = adqt::icons::IconColors::primary(separatorColor);
      const int side = rangeSeparatorVisualSide(this, style);
      QRect iconRect(rect.left() + (rect.width() - side) / 2,
                     rect.top() + (rect.height() - side) / 2, side, side);
      adqt::icons::paintIcon(painter, outlined_icons::SwapRight(colors), QRectF(iconRect));
      return;
    }

    const QString separatorText = rangeSeparatorText_.trimmed().isEmpty()
                                      ? defaultRangeSeparator().trimmed()
                                      : rangeSeparatorText_.trimmed();
    const QFont previousFont = painter->font();
    painter->setFont(rangeSeparatorFont(this, style));
    painter->setPen(separatorColor);
    painter->drawText(rect, Qt::AlignCenter, separatorText);
    painter->setFont(previousFont);
  }

  int resolvedTagHeight(int contentHeight) const {
    const int defaultHeight = std::clamp(contentHeight - 4, 16, 22);
    std::optional<int> tokenHeight;
    if (dateTagSize_ == AdDatePicker::Size::Small) {
      tokenHeight = dateTagTokens_.multipleItemHeightSmall;
    } else if (dateTagSize_ == AdDatePicker::Size::Large) {
      tokenHeight = dateTagTokens_.multipleItemHeightLarge;
    }
    if (!tokenHeight.has_value()) {
      tokenHeight = dateTagTokens_.multipleItemHeight;
    }
    if (!tokenHeight.has_value()) {
      return defaultHeight;
    }
    return std::clamp(*tokenHeight, 8, std::max(8, contentHeight));
  }

  int responsiveVisibleCount(const QStringList& labels, int availableWidth) const {
    if (labels.isEmpty() || availableWidth <= 0) {
      return 0;
    }
    const QFontMetrics fm(font());
    constexpr int horizontalPadding = 14;
    constexpr int gap = 4;
    const auto tagWidth = [&fm](const QString& text) {
      return fm.horizontalAdvance(text) + horizontalPadding;
    };
    const auto restWidth = [&fm](int hiddenCount) {
      return fm.horizontalAdvance(QStringLiteral("+%1...").arg(hiddenCount)) + horizontalPadding;
    };

    int used = 0;
    const int labelCount = static_cast<int>(labels.size());
    for (int count = 0; count < labelCount; ++count) {
      const int hiddenCount = labelCount - count - 1;
      int next = used + (count > 0 ? gap : 0) + tagWidth(labels.at(count));
      if (hiddenCount > 0) {
        next += gap + restWidth(hiddenCount);
      }
      if (next > availableWidth) {
        return count;
      }
      used += (count > 0 ? gap : 0) + tagWidth(labels.at(count));
    }
    return labelCount;
  }

  QStringList dateTagTexts_;
  int maxTagCount_ = -1;
  bool responsiveMaxTagCount_ = false;
  AdDatePickerPanel::ComponentTokens dateTagTokens_;
  AdDatePicker::Size dateTagSize_ = AdDatePicker::Size::Middle;
  bool rangeInputDisplayEnabled_ = false;
  QString rangeStartText_;
  QString rangeEndText_;
  QString rangeStartPlaceholder_;
  QString rangeEndPlaceholder_;
  QString rangeSeparatorText_;
  bool rangeDefaultSeparatorIcon_ = false;
  AdDateRangePicker::RangePart rangeActivePart_ = AdDateRangePicker::RangePart::Start;
  bool rangeActive_ = false;
};

class DatePickerCalendarGrid final : public QWidget {
 public:
  explicit DatePickerCalendarGrid(AdDatePickerPanel* panel, QWidget* parent = nullptr)
      : QWidget(parent), panel_(panel) {
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
  }

  QSize sizeHint() const override {
    if (!panel_) {
      const DatePickerVisualStyle style = resolveStyleForPanel(nullptr);
      const int width = style.metrics.panelWidth;
      return QSize(width, 240);
    }
    const DatePickerVisualStyle style = panel_->resolvedStyle();
    const int width = style.metrics.panelWidth;
    switch (panel_->displayMode_) {
      case AdDatePickerPanel::DisplayMode::Time:
        return QSize(width, 0);
      case AdDatePickerPanel::DisplayMode::Month:
      case AdDatePickerPanel::DisplayMode::Decade:
      case AdDatePickerPanel::DisplayMode::Year:
        return QSize(width, style.metrics.monthCellHeight * 4);
      case AdDatePickerPanel::DisplayMode::Quarter:
        return QSize(width, style.metrics.quarterPanelContentHeight);
      case AdDatePickerPanel::DisplayMode::Date:
      default:
        return QSize(width, style.metrics.panelPaddingVertical * 2 + datePanelRowHeight(style) * 7);
    }
  }

  QSize minimumSizeHint() const override { return QSize(180, 160); }

  void clearHoverState() {
    hoveredIndex_ = -1;
    hoveredPreviewDate_ = QDate();
    invalidateCells();
    update();
  }

  void invalidateCells() const { cellsDirty_ = true; }

 protected:
  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)
    if (!panel_) {
      return;
    }

    const DatePickerVisualStyle style = panel_->resolvedStyle();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setFont(style.metrics.font);
    painter.fillRect(rect(), style.contentBackground);

    if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Time) {
      return;
    }
    if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Date) {
      paintDateGrid(painter, style);
      return;
    }
    paintUnitGrid(painter, style);
  }

  void mouseMoveEvent(QMouseEvent* event) override { setHoverFromPosition(mouseEventPos(event)); }

  void leaveEvent(QEvent* event) override {
    setHoverState(-1, QDate());
    QWidget::leaveEvent(event);
  }

  void mousePressEvent(QMouseEvent* event) override {
    if (!panel_ || panel_->disabled_ || !event || event->button() != Qt::LeftButton) {
      QWidget::mousePressEvent(event);
      return;
    }

    const QVector<Cell>& cells = buildCells();
    const QPoint pos = mouseEventPos(event);
    for (int i = 0; i < cells.size(); ++i) {
      const Cell& cell = cells.at(i);
      if (!cell.rect.contains(pos) || cell.disabled || !cell.date.isValid()) {
        continue;
      }

      activateCell(cell);
      event->accept();
      return;
    }

    if (useWeekRowPainting()) {
      const int row = dateGridRowFromPosition(pos);
      const int rowIndex = row * 7;
      if (row >= 0 && rowIndex >= 0 && rowIndex < cells.size()) {
        const Cell& cell = cells.at(rowIndex);
        if (!cell.disabled && cell.date.isValid()) {
          activateCell(cell);
          event->accept();
          return;
        }
      }
    }

    QWidget::mousePressEvent(event);
  }

  void keyPressEvent(QKeyEvent* event) override {
    if (!panel_ || panel_->disabled_ || !event) {
      QWidget::keyPressEvent(event);
      return;
    }
    if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Time) {
      QWidget::keyPressEvent(event);
      return;
    }

    QDate base;
    if (panel_->selectionMode_ == AdDatePickerPanel::SelectionMode::Range) {
      base = panel_->rangeEndDate_.isValid() ? panel_->rangeEndDate_ : panel_->rangeStartDate_;
    } else if (panel_->selectionMode_ == AdDatePickerPanel::SelectionMode::Multiple) {
      base = panel_->selectedDates_.isEmpty() ? QDate() : panel_->selectedDates_.constLast();
    } else {
      base = panel_->selectedDate_;
    }
    if (!base.isValid()) {
      base = panel_->viewDate_.isValid() ? panel_->viewDate_ : todayDate();
    }

    QDate next = base;
    int keyStep = 0;
    switch (event->key()) {
      case Qt::Key_Left:
        keyStep = -1;
        break;
      case Qt::Key_Right:
        keyStep = 1;
        break;
      case Qt::Key_Up:
        keyStep = -7;
        break;
      case Qt::Key_Down:
        keyStep = 7;
        break;
      case Qt::Key_PageUp:
        if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Date) {
          panel_->navigate(-1, 0);
        } else if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Decade) {
          panel_->navigate(0, -100);
        } else {
          panel_->navigate(0,
                           panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Year ? -10 : -1);
        }
        event->accept();
        return;
      case Qt::Key_PageDown:
        if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Date) {
          panel_->navigate(1, 0);
        } else if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Decade) {
          panel_->navigate(0, 100);
        } else {
          panel_->navigate(0,
                           panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Year ? 10 : 1);
        }
        event->accept();
        return;
      case Qt::Key_Return:
      case Qt::Key_Enter:
        panel_->selectDateFromGrid(base);
        event->accept();
        return;
      default:
        QWidget::keyPressEvent(event);
        return;
    }

    if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Date) {
      next = base.addDays(keyStep);
    } else if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Month) {
      next = base.addMonths(keyStep);
    } else if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Quarter) {
      next = base.addMonths(keyStep * 3);
    } else if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Year) {
      next = base.addYears(keyStep);
    } else {
      next = base.addYears(keyStep * 10);
    }
    if (!panel_->isSelectableForMode(panel_->pickerMode_, next,
                                     panel_->rangeSelectionContextFrom())) {
      event->accept();
      return;
    }
    panel_->setViewDate(next);
    if (panel_->selectionMode_ == AdDatePickerPanel::SelectionMode::Single) {
      panel_->setSelectedDate(
          normalizeForPicker(panel_->pickerMode_, next, panel_->firstDayOfWeek_));
    }
    event->accept();
  }

 private:
  struct Cell {
    QRect rect;
    QRect innerRect;
    QDate date;
    QString text;
    AdDatePickerPanel::PickerMode type = AdDatePickerPanel::PickerMode::Date;
    bool inView = true;
    bool today = false;
    bool selected = false;
    bool rangeStart = false;
    bool rangeEnd = false;
    bool inRange = false;
    bool rangeBefore = false;
    bool rangeAfter = false;
    bool hoverRange = false;
    bool hoverRangeStart = false;
    bool hoverRangeEnd = false;
    bool hoverRangeBefore = false;
    bool hoverRangeAfter = false;
    bool disabled = false;
  };

  static int datePanelRowHeight(const DatePickerVisualStyle& style) {
    return std::max(1, style.metrics.cellHeight + style.metrics.cellPaddingVertical * 2);
  }

  void setHoverFromPosition(const QPoint& pos) {
    const QVector<Cell>& cells = buildCells();
    int nextIndex = -1;
    QDate nextPreviewDate;
    for (int i = 0; i < cells.size(); ++i) {
      const Cell& cell = cells.at(i);
      if (!cell.rect.contains(pos)) {
        continue;
      }
      nextIndex = i;
      if (!cell.disabled && !panel_->disabled_ && cell.date.isValid()) {
        nextPreviewDate = cell.date;
      }
      break;
    }
    if (nextIndex < 0 && useWeekRowPainting()) {
      const int row = dateGridRowFromPosition(pos);
      const int rowIndex = row * 7;
      if (row >= 0 && rowIndex >= 0 && rowIndex < cells.size()) {
        const Cell& cell = cells.at(rowIndex);
        nextIndex = rowIndex;
        if (!cell.disabled && !panel_->disabled_ && cell.date.isValid()) {
          nextPreviewDate = cell.date;
        }
      }
    }
    setHoverState(nextIndex, nextPreviewDate);
  }

  void setHoverState(int nextIndex, const QDate& nextPreviewDate) {
    if (hoveredIndex_ == nextIndex && hoveredPreviewDate_ == nextPreviewDate) {
      return;
    }
    const bool previewChanged = hoveredPreviewDate_ != nextPreviewDate;
    hoveredIndex_ = nextIndex;
    hoveredPreviewDate_ = nextPreviewDate;
    if (previewChanged && panel_) {
      invalidateCells();
      emit panel_->previewDateChanged(hoveredPreviewDate_);
    }
    update();
  }

  const QVector<Cell>& buildCells() const {
    if (!panel_) {
      static const QVector<Cell> empty;
      return empty;
    }
    if (!cellsDirty_ && cachedSize_ == size() && cachedDisplayMode_ == panel_->displayMode_) {
      return cachedCells_;
    }

    cachedCells_.clear();
    if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Date) {
      cachedCells_ = buildDateCells();
    } else if (panel_->displayMode_ != AdDatePickerPanel::DisplayMode::Time) {
      cachedCells_ = buildUnitCells();
    }
    cachedSize_ = size();
    cachedDisplayMode_ = panel_->displayMode_;
    cellsDirty_ = false;
    return cachedCells_;
  }

  QVector<Cell> buildDateCells() const {
    const DatePickerVisualStyle style = panel_->resolvedStyle();
    QVector<Cell> cells;
    cells.reserve(42);

    const int paddingX = datePanelPaddingHorizontal(style);
    const int paddingY = style.metrics.panelPaddingVertical;
    const int rowH = datePanelRowHeight(style);
    const int weekdayHeight = rowH;
    const QRect content = rect().adjusted(paddingX, paddingY, -paddingX, -paddingY);
    const bool showWeek = showWeekColumn();
    const int weekColumnWidth = showWeek ? std::max(1, content.width() / 8) : 0;
    const int dayGridLeft = content.left() + weekColumnWidth;
    const int cellW = std::max(1, (content.width() - weekColumnWidth) / 7);
    const int gridTop = content.top() + weekdayHeight;

    QDate view = panel_->viewDate_.isValid() ? panel_->viewDate_ : todayDate();
    view = QDate(view.year(), view.month(), 1);
    const QDate first =
        view.addDays(-((view.dayOfWeek() - static_cast<int>(panel_->firstDayOfWeek_) + 7) % 7));

    const QDate today = todayDate();
    const QDate normalizedSelected =
        normalizeForPicker(panel_->pickerMode_, panel_->selectedDate_, panel_->firstDayOfWeek_);
    const QDate normalizedStart =
        normalizeForPicker(panel_->pickerMode_, panel_->rangeStartDate_, panel_->firstDayOfWeek_);
    const QDate normalizedEnd =
        normalizeForPicker(panel_->pickerMode_, panel_->rangeEndDate_, panel_->firstDayOfWeek_);
    const QDate normalizedHover =
        normalizeForPicker(panel_->pickerMode_, hoveredPreviewDate_, panel_->firstDayOfWeek_);
    QDate hoverRangeStart =
        normalizeForPicker(panel_->pickerMode_, panel_->hoverRangeStartDate_,
                           panel_->firstDayOfWeek_);
    QDate hoverRangeEnd =
        normalizeForPicker(panel_->pickerMode_, panel_->hoverRangeEndDate_,
                           panel_->firstDayOfWeek_);
    if (!panel_->hoverRangeActive_ && normalizedHover.isValid()) {
      if (normalizedStart.isValid() && !normalizedEnd.isValid()) {
        hoverRangeStart = normalizedStart;
        hoverRangeEnd = normalizedHover;
      } else if (!normalizedStart.isValid() && normalizedEnd.isValid()) {
        hoverRangeStart = normalizedHover;
        hoverRangeEnd = normalizedEnd;
      } else if (normalizedStart.isValid() && normalizedEnd.isValid()) {
        if (panel_->visibleRangeTimePart_ == AdDatePickerPanel::TimeSelectionPart::End) {
          hoverRangeStart = normalizedStart;
          hoverRangeEnd = normalizedHover;
        } else {
          hoverRangeStart = normalizedHover;
          hoverRangeEnd = normalizedEnd;
        }
      }
    }
    const bool hasHoverRange = panel_->selectionMode_ == AdDatePickerPanel::SelectionMode::Range &&
                               (hoverRangeStart.isValid() || hoverRangeEnd.isValid());
    const bool hasCommittedRange = normalizedStart.isValid() && normalizedEnd.isValid();
    const QDate committedLow = hasCommittedRange ? std::min(normalizedStart, normalizedEnd) : QDate();
    const QDate committedHigh = hasCommittedRange ? std::max(normalizedStart, normalizedEnd) : QDate();
    QDate hoverLow = hoverRangeStart;
    QDate hoverHigh = hoverRangeEnd;
    if (hasHoverRange) {
      if (!hoverLow.isValid()) {
        hoverLow = hoverHigh;
      }
      if (!hoverHigh.isValid()) {
        hoverHigh = hoverLow;
      }
      if (hoverHigh < hoverLow) {
        std::swap(hoverLow, hoverHigh);
      }
    }

    for (int row = 0; row < 6; ++row) {
      for (int col = 0; col < 7; ++col) {
        const int index = row * 7 + col;
        const QDate date = first.addDays(index);
        QRect cellRect(dayGridLeft + col * cellW, gridTop + row * rowH, cellW, rowH);
        const int innerSide =
            std::max(1, std::min({style.metrics.cellHeight, cellRect.width(), cellRect.height()}));
        QRect inner(cellRect.left() + (cellRect.width() - innerSide) / 2,
                    cellRect.top() + (cellRect.height() - innerSide) / 2, innerSide, innerSide);

        const QDate normalized =
            normalizeForPicker(panel_->pickerMode_, date, panel_->firstDayOfWeek_);
        Cell cell;
        cell.rect = cellRect;
        cell.innerRect = inner;
        cell.date = normalized;
        cell.text = QString::number(date.day());
        cell.type = AdDatePickerPanel::PickerMode::Date;
        cell.inView = date.month() == view.month() && date.year() == view.year();
        cell.today = date == today;
        const QDate disabledProbe =
            panel_->pickerMode_ == AdDatePickerPanel::PickerMode::Date ? date : normalized;
        cell.disabled = !panel_->isSelectableForMode(panel_->pickerMode_, disabledProbe,
                                                     panel_->rangeSelectionContextFrom());
        if (panel_->selectionMode_ == AdDatePickerPanel::SelectionMode::Single) {
          cell.selected = normalizedSelected.isValid() && normalizedSelected == normalized;
        } else if (panel_->selectionMode_ == AdDatePickerPanel::SelectionMode::Multiple) {
          cell.selected = panel_->selectedDateKeys_.contains(normalized.toJulianDay());
        } else {
          cell.rangeStart = normalizedStart.isValid() && normalizedStart == normalized;
          cell.rangeEnd = normalizedEnd.isValid() && normalizedEnd == normalized;
          if (hasCommittedRange) {
            cell.inRange = normalized >= committedLow && normalized <= committedHigh;
            cell.rangeBefore = cell.inRange && normalized > committedLow;
            cell.rangeAfter = cell.inRange && normalized < committedHigh;
          }
          if (hasHoverRange) {
            cell.hoverRange = normalized >= hoverLow && normalized <= hoverHigh;
            cell.hoverRangeStart = cell.hoverRange && normalized == hoverLow;
            cell.hoverRangeEnd = cell.hoverRange && normalized == hoverHigh;
            cell.hoverRangeBefore = cell.hoverRange && normalized > hoverLow;
            cell.hoverRangeAfter = cell.hoverRange && normalized < hoverHigh;
            // rc-picker renders the hover pair instead of the committed range.
            cell.rangeStart = cell.hoverRangeStart;
            cell.rangeEnd = cell.hoverRangeEnd;
            cell.inRange = cell.hoverRange;
            cell.rangeBefore = cell.hoverRangeBefore;
            cell.rangeAfter = cell.hoverRangeAfter;
          }
          cell.selected = (cell.rangeStart || cell.rangeEnd) && !hasHoverRange;
        }
        cells.append(cell);
      }
    }

    return cells;
  }

  QVector<Cell> buildUnitCells() const {
    const DatePickerVisualStyle style = panel_->resolvedStyle();
    QVector<Cell> cells;
    const int paddingX = unitPanelPaddingHorizontal(style);
    const QRect content = rect().adjusted(paddingX, 0, -paddingX, 0);
    const int cols = panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Quarter ? 4 : 3;
    const int count = panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Quarter ? 4 : 12;
    const int rows = std::max(1, (count + cols - 1) / cols);
    const int cellW = std::max(1, content.width() / cols);
    const int rowH = std::max(1, content.height() / rows);
    const QDate view = panel_->viewDate_.isValid() ? panel_->viewDate_ : todayDate();
    AdDatePickerPanel::PickerMode compareMode = panel_->pickerMode_;
    if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Month) {
      compareMode = AdDatePickerPanel::PickerMode::Month;
    } else if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Quarter) {
      compareMode = AdDatePickerPanel::PickerMode::Quarter;
    } else if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Year) {
      compareMode = AdDatePickerPanel::PickerMode::Year;
    } else if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Decade) {
      compareMode = AdDatePickerPanel::PickerMode::Decade;
    }

    const QDate today = todayDate();
    const QDate normalizedToday =
        normalizeForPicker(compareMode, today, panel_->firstDayOfWeek_);
    const QDate normalizedSelected =
        normalizeForPicker(compareMode, panel_->selectedDate_, panel_->firstDayOfWeek_);
    const QDate normalizedStart =
        normalizeForPicker(compareMode, panel_->rangeStartDate_, panel_->firstDayOfWeek_);
    const QDate normalizedEnd =
        normalizeForPicker(compareMode, panel_->rangeEndDate_, panel_->firstDayOfWeek_);
    const QDate normalizedHover =
        normalizeForPicker(compareMode, hoveredPreviewDate_, panel_->firstDayOfWeek_);
    QDate hoverRangeStart = normalizeForPicker(compareMode, panel_->hoverRangeStartDate_,
                                               panel_->firstDayOfWeek_);
    QDate hoverRangeEnd = normalizeForPicker(compareMode, panel_->hoverRangeEndDate_,
                                             panel_->firstDayOfWeek_);
    if (!panel_->hoverRangeActive_ && normalizedHover.isValid()) {
      if (normalizedStart.isValid() && !normalizedEnd.isValid()) {
        hoverRangeStart = normalizedStart;
        hoverRangeEnd = normalizedHover;
      } else if (!normalizedStart.isValid() && normalizedEnd.isValid()) {
        hoverRangeStart = normalizedHover;
        hoverRangeEnd = normalizedEnd;
      } else if (normalizedStart.isValid() && normalizedEnd.isValid()) {
        if (panel_->visibleRangeTimePart_ == AdDatePickerPanel::TimeSelectionPart::End) {
          hoverRangeStart = normalizedStart;
          hoverRangeEnd = normalizedHover;
        } else {
          hoverRangeStart = normalizedHover;
          hoverRangeEnd = normalizedEnd;
        }
      }
    }
    const bool hasHoverRange = panel_->selectionMode_ == AdDatePickerPanel::SelectionMode::Range &&
                               (hoverRangeStart.isValid() || hoverRangeEnd.isValid());
    QDate committedLow = normalizedStart;
    QDate committedHigh = normalizedEnd;
    if (committedLow.isValid() && committedHigh.isValid() && committedHigh < committedLow) {
      std::swap(committedLow, committedHigh);
    }
    QDate hoverLow = hoverRangeStart;
    QDate hoverHigh = hoverRangeEnd;
    if (hasHoverRange) {
      if (!hoverLow.isValid()) {
        hoverLow = hoverHigh;
      }
      if (!hoverHigh.isValid()) {
        hoverHigh = hoverLow;
      }
      if (hoverHigh < hoverLow) {
        std::swap(hoverLow, hoverHigh);
      }
    }

    cells.reserve(count);
    for (int i = 0; i < count; ++i) {
      const int row = i / cols;
      const int col = i % cols;
      QRect cellRect(content.left() + col * cellW, content.top() + row * rowH, cellW, rowH);
      const int innerHeight = std::max(1, std::min(style.metrics.cellHeight, cellRect.height()));
      const int verticalInset = std::max(0, (rowH - innerHeight) / 2);
      QRect inner;
      if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Decade) {
        inner = cellRect.adjusted(4, verticalInset, -4, -verticalInset);
      } else {
        const int innerWidth = std::max(
            innerHeight, std::min(style.metrics.yearMonthCellWidth, std::max(1, cellRect.width())));
        inner = QRect(cellRect.left() + (cellRect.width() - innerWidth) / 2,
                      cellRect.top() + verticalInset, innerWidth, innerHeight);
      }

      Cell cell;
      cell.rect = cellRect;
      cell.innerRect = inner;
      if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Month) {
        const int month = i + 1;
        cell.date = QDate(view.year(), month, 1);
        cell.text = monthName(month, panel_->locale_, true);
      } else if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Quarter) {
        const int quarter = i + 1;
        cell.date = QDate(view.year(), (quarter - 1) * 3 + 1, 1);
        cell.text = QStringLiteral("Q%1").arg(quarter);
      } else if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Year) {
        const int year = decadeStartForYear(view.year()) - 1 + i;
        cell.date = QDate(year, 1, 1);
        cell.text = QString::number(year);
        cell.inView =
            year >= decadeStartForYear(view.year()) && year < decadeStartForYear(view.year()) + 10;
      } else {
        const int decade = centuryStartForYear(view.year()) - 10 + i * 10;
        cell.date = QDate(decade, 1, 1);
        cell.text = QStringLiteral("%1-%2").arg(decade).arg(decade + 9);
        cell.inView = decade >= centuryStartForYear(view.year()) &&
                      decade < centuryStartForYear(view.year()) + 100;
      }

      const QDate normalized = normalizeForPicker(compareMode, cell.date, panel_->firstDayOfWeek_);
      cell.type = compareMode;
      if (panel_->selectionMode_ == AdDatePickerPanel::SelectionMode::Single) {
        cell.selected = normalizedSelected.isValid() && normalizedSelected == normalized;
      } else if (panel_->selectionMode_ == AdDatePickerPanel::SelectionMode::Multiple) {
        cell.selected = compareMode == panel_->pickerMode_
                            ? panel_->selectedDateKeys_.contains(normalized.toJulianDay())
                            : dateVectorContainsPickerValue(panel_->selectedDates_, compareMode,
                                                            normalized, panel_->firstDayOfWeek_);
      } else {
        cell.rangeStart = normalizedStart.isValid() && normalizedStart == normalized;
        cell.rangeEnd = normalizedEnd.isValid() && normalizedEnd == normalized;
        if (normalizedStart.isValid() && normalizedEnd.isValid()) {
          cell.inRange = normalized >= committedLow && normalized <= committedHigh;
          cell.rangeBefore = cell.inRange && normalized > committedLow;
          cell.rangeAfter = cell.inRange && normalized < committedHigh;
        }
        if (hasHoverRange) {
          cell.hoverRange = normalized >= hoverLow && normalized <= hoverHigh;
          cell.hoverRangeStart = cell.hoverRange && normalized == hoverLow;
          cell.hoverRangeEnd = cell.hoverRange && normalized == hoverHigh;
          cell.hoverRangeBefore = cell.hoverRange && normalized > hoverLow;
          cell.hoverRangeAfter = cell.hoverRange && normalized < hoverHigh;
        }
        if (hasHoverRange) {
          // rc-picker renders the hover pair instead of the committed range.
          cell.rangeStart = cell.hoverRangeStart;
          cell.rangeEnd = cell.hoverRangeEnd;
          cell.inRange = cell.hoverRange;
          cell.rangeBefore = cell.hoverRangeBefore;
          cell.rangeAfter = cell.hoverRangeAfter;
        }
        cell.selected = (cell.rangeStart || cell.rangeEnd) && !hasHoverRange;
      }
      cell.today = normalizedToday.isValid() && normalizedToday == normalized;
      cell.disabled =
          !panel_->isSelectableForMode(compareMode, cell.date, panel_->rangeSelectionContextFrom());
      cells.append(cell);
    }
    return cells;
  }

  void paintDateGrid(QPainter& painter, const DatePickerVisualStyle& style) {
    const int paddingX = datePanelPaddingHorizontal(style);
    const int paddingY = style.metrics.panelPaddingVertical;
    const int weekdayHeight = datePanelRowHeight(style);
    const QRect content = rect().adjusted(paddingX, paddingY, -paddingX, -paddingY);
    const bool showWeek = showWeekColumn();
    const int weekColumnWidth = showWeek ? std::max(1, content.width() / 8) : 0;
    const int dayGridLeft = content.left() + weekColumnWidth;
    const int cellW = std::max(1, (content.width() - weekColumnWidth) / 7);
    const int rowH = datePanelRowHeight(style);
    const int gridTop = content.top() + weekdayHeight;
    const QVector<Cell>& dateCells = buildCells();

    painter.setFont(style.metrics.font);
    painter.setPen(style.textColor);
    if (showWeek) {
      const QRect weekHeaderRect(content.left(), content.top(), weekColumnWidth, weekdayHeight);
      painter.drawText(weekHeaderRect, Qt::AlignCenter, AdDatePickerPanel::tr("Week"));
    }
    for (int col = 0; col < 7; ++col) {
      const int dayNumber = ((static_cast<int>(panel_->firstDayOfWeek_) - 1 + col) % 7) + 1;
      const QRect weekdayRect(dayGridLeft + col * cellW, content.top(), cellW, weekdayHeight);
      painter.drawText(weekdayRect, Qt::AlignCenter, weekdayLabel(dayNumber));
    }

    if (showWeek) {
      QDate view = panel_->viewDate_.isValid() ? panel_->viewDate_ : todayDate();
      view = QDate(view.year(), view.month(), 1);
      const QDate first =
          view.addDays(-((view.dayOfWeek() - static_cast<int>(panel_->firstDayOfWeek_) + 7) % 7));
      if (useWeekRowPainting()) {
        paintWeekRowBackgrounds(painter, style, dateCells, content, gridTop, rowH);
      }
      for (int row = 0; row < 6; ++row) {
        const QDate weekDate = first.addDays(row * 7);
        const int week = weekNumberForDate(weekDate, panel_->firstDayOfWeek_, nullptr);
        const QRect weekRect(content.left(), gridTop + row * rowH, weekColumnWidth, rowH);
        painter.setPen(weekNumberTextColorForRow(style, dateCells, row));
        painter.drawText(weekRect, Qt::AlignCenter, QString::number(week));
      }
    }

    painter.setFont(style.metrics.font);
    paintCells(painter, style, dateCells);
  }

  void paintUnitGrid(QPainter& painter, const DatePickerVisualStyle& style) {
    painter.setFont(style.metrics.font);
    paintCells(painter, style, buildCells());
  }

  void paintCells(QPainter& painter, const DatePickerVisualStyle& style,
                  const QVector<Cell>& cells) {
    const int radius = std::max(0, style.metrics.cellRadius);
    const bool weekRows = useWeekRowPainting();
    const bool cellBeforeVisible =
        !weekRows && (!panel_ || panel_->displayMode_ != AdDatePickerPanel::DisplayMode::Decade);
    for (int i = 0; i < cells.size(); ++i) {
      const Cell& cell = cells.at(i);
      if (!cell.date.isValid()) {
        continue;
      }

      const bool hovered = i == hoveredIndex_ && !cell.disabled && !panel_->disabled_;
      if (style.itemBackground.alpha() > 0) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(style.itemBackground);
        painter.drawRoundedRect(cell.innerRect, radius, radius);
      }
      if (cellBeforeVisible && cell.hoverRange && cell.inView &&
          (cell.hoverRangeBefore || cell.hoverRangeAfter)) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(style.rangeBackground);
        const QRect fillRect = rangeFillRect(cell, style.metrics.cellHeight, cell.hoverRangeBefore,
                                             cell.hoverRangeAfter);
        painter.drawRect(fillRect);
      } else if (cellBeforeVisible && cell.inRange && cell.inView &&
                 (cell.rangeBefore || cell.rangeAfter)) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(style.rangeBackground);
        painter.drawRect(
            rangeFillRect(cell, style.metrics.cellHeight, cell.rangeBefore, cell.rangeAfter));
      }
      if (style.itemBorderColor.alpha() > 0) {
        QRectF itemRect = QRectF(cell.innerRect).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(QPen(style.itemBorderColor, style.metrics.borderWidth));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(itemRect, radius, radius);
      }

      const bool disabled = cell.disabled || panel_->disabled_;
      QColor textColor = cell.inView ? style.textColor : style.disabledTextColor;
      if (disabled) {
        textColor = style.disabledTextColor;
      }

      if (disabled && cellBeforeVisible) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(style.disabledCellBackground);
        painter.drawRect(rangeFillRect(cell, style.metrics.cellHeight, true, true));
      }

      const bool hoverEndpointVisual =
          cell.hoverRange && (cell.hoverRangeStart || cell.hoverRangeEnd);
      const bool selectedVisual =
          (cell.selected || hoverEndpointVisual) && (cell.inView || weekRows);
      if (!weekRows && hovered && !selectedVisual) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(style.hoverBackground);
        painter.drawRoundedRect(cell.innerRect, radius, radius);
      }

      if (cell.today && cell.inView && !selectedVisual) {
        QRectF todayRect = QRectF(cell.innerRect).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(QPen(disabled ? style.disabledTextColor : style.todayBorderColor,
                            style.metrics.borderWidth));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(todayRect, radius, radius);
      }

      if (selectedVisual) {
        if (!weekRows && !disabled) {
          painter.setPen(Qt::NoPen);
          painter.setBrush(style.selectedBackground);
          painter.drawPath(selectedCellPath(cell, radius, hoverEndpointVisual));
        }
        if (!disabled) {
          textColor = style.selectedTextColor;
        }
      }

      painter.setPen(textColor);
      painter.drawText(cell.innerRect, Qt::AlignCenter, cell.text);

      if (panel_->cellRenderCallback_) {
        painter.save();
        panel_->cellRenderCallback_(painter, renderInfoForCell(cell));
        painter.restore();
      }
    }
  }

  QRect rangeFillRect(const Cell& cell, int height, bool rangeBefore, bool rangeAfter) const {
    const int clampedHeight = std::max(1, std::min(height, cell.rect.height()));
    const int y = cell.rect.top() + (cell.rect.height() - clampedHeight) / 2;
    const int centerX = cell.rect.left() + cell.rect.width() / 2;
    if (rangeBefore && !rangeAfter) {
      return QRect(cell.rect.left(), y, centerX - cell.rect.left(), clampedHeight);
    }
    if (!rangeBefore && rangeAfter) {
      return QRect(centerX, y, cell.rect.right() - centerX + 1, clampedHeight);
    }
    return QRect(cell.rect.left(), y, cell.rect.width(), clampedHeight);
  }

  QPainterPath selectedCellPath(const Cell& cell, int radius, bool useHoverRange) const {
    if (useHoverRange) {
      const bool splitHoverEndpoint = cell.hoverRangeStart != cell.hoverRangeEnd &&
                                      (cell.hoverRangeBefore || cell.hoverRangeAfter);
      if (splitHoverEndpoint) {
        const bool roundLeft = !cell.hoverRangeBefore;
        const bool roundRight = !cell.hoverRangeAfter;
        return roundedCellPath(QRectF(cell.innerRect), radius, roundLeft, roundRight);
      }
    }

    const bool splitRangeEndpoint =
        cell.inRange && cell.rangeStart != cell.rangeEnd && (cell.rangeBefore || cell.rangeAfter);
    if (!splitRangeEndpoint) {
      QPainterPath path;
      path.addRoundedRect(QRectF(cell.innerRect), radius, radius);
      return path;
    }

    const bool roundLeft = !cell.rangeBefore;
    const bool roundRight = !cell.rangeAfter;
    return roundedCellPath(QRectF(cell.innerRect), radius, roundLeft, roundRight);
  }

  void paintWeekRowBackgrounds(QPainter& painter, const DatePickerVisualStyle& style,
                               const QVector<Cell>& cells, const QRect& content, int gridTop,
                               int rowH) const {
    const int radius = std::max(0, style.metrics.cellRadius);
    const int rowFillHeight = std::max(1, std::min(style.metrics.cellHeight, rowH));
    const int hoveredRow = hoveredIndex_ >= 0 ? hoveredIndex_ / 7 : -1;
    for (int row = 0; row < 6; ++row) {
      const int rowIndex = row * 7;
      if (rowIndex >= cells.size()) {
        break;
      }
      bool primary = false;
      bool hoverRange = false;
      bool inRange = false;
      bool rowDisabled = true;
      for (int col = 0; col < 7 && rowIndex + col < cells.size(); ++col) {
        const Cell& cell = cells.at(rowIndex + col);
        primary = primary || cell.selected;
        hoverRange = hoverRange || cell.hoverRange;
        inRange = inRange || cell.inRange;
        rowDisabled = rowDisabled && (cell.disabled || !cell.date.isValid());
      }

      QColor background;
      if (primary) {
        background = style.selectedBackground;
      } else if (hoverRange || inRange) {
        background = style.rangeBackground;
      } else if (row == hoveredRow && !rowDisabled && !panel_->disabled_) {
        background = style.hoverBackground;
      }
      if (!background.isValid() || background.alpha() <= 0) {
        continue;
      }

      const int rowTop = gridTop + row * rowH + (rowH - rowFillHeight) / 2;
      const QRect rowRect(content.left(), rowTop, content.width(), rowFillHeight);
      painter.setPen(Qt::NoPen);
      painter.setBrush(background);
      painter.drawRoundedRect(rowRect, radius, radius);
    }
  }

  QColor weekNumberTextColorForRow(const DatePickerVisualStyle& style, const QVector<Cell>& cells,
                                   int row) const {
    if (!useWeekRowPainting()) {
      return style.textColor;
    }
    const int rowIndex = row * 7;
    bool primary = false;
    bool rowDisabled = true;
    for (int col = 0; col < 7 && rowIndex + col < cells.size(); ++col) {
      const Cell& cell = cells.at(rowIndex + col);
      primary = primary || cell.selected;
      rowDisabled = rowDisabled && (cell.disabled || !cell.date.isValid());
    }
    if (primary) {
      QColor color = style.selectedTextColor;
      color.setAlphaF(0.5);
      return color;
    }
    return rowDisabled || (panel_ && panel_->disabled_) ? style.disabledTextColor
                                                        : style.secondaryTextColor;
  }

  QPainterPath roundedCellPath(const QRectF& rect, int radius, bool roundLeft,
                               bool roundRight) const {
    const QRectF bounds = rect.normalized();
    const qreal r =
        std::min<qreal>(std::max(0, radius), std::min(bounds.width(), bounds.height()) / 2.0);
    QPainterPath path;
    if (r <= 0.0) {
      path.addRect(bounds);
      return path;
    }

    const bool topLeft = roundLeft;
    const bool bottomLeft = roundLeft;
    const bool topRight = roundRight;
    const bool bottomRight = roundRight;

    path.moveTo(bounds.left() + (topLeft ? r : 0.0), bounds.top());
    path.lineTo(bounds.right() - (topRight ? r : 0.0), bounds.top());
    if (topRight) {
      path.quadTo(bounds.right(), bounds.top(), bounds.right(), bounds.top() + r);
    } else {
      path.lineTo(bounds.right(), bounds.top());
    }
    path.lineTo(bounds.right(), bounds.bottom() - (bottomRight ? r : 0.0));
    if (bottomRight) {
      path.quadTo(bounds.right(), bounds.bottom(), bounds.right() - r, bounds.bottom());
    } else {
      path.lineTo(bounds.right(), bounds.bottom());
    }
    path.lineTo(bounds.left() + (bottomLeft ? r : 0.0), bounds.bottom());
    if (bottomLeft) {
      path.quadTo(bounds.left(), bounds.bottom(), bounds.left(), bounds.bottom() - r);
    } else {
      path.lineTo(bounds.left(), bounds.bottom());
    }
    path.lineTo(bounds.left(), bounds.top() + (topLeft ? r : 0.0));
    if (topLeft) {
      path.quadTo(bounds.left(), bounds.top(), bounds.left() + r, bounds.top());
    } else {
      path.lineTo(bounds.left(), bounds.top());
    }
    path.closeSubpath();
    return path;
  }

  AdDatePickerPanel::CellRenderInfo renderInfoForCell(const Cell& cell) const {
    AdDatePickerPanel::CellRenderInfo info;
    info.date = cell.date;
    info.text = cell.text;
    info.type = cell.type;
    info.cellRect = cell.rect;
    info.contentRect = cell.innerRect;
    info.inView = cell.inView;
    info.today = cell.today;
    info.selected = cell.selected;
    info.rangeStart = cell.rangeStart;
    info.rangeEnd = cell.rangeEnd;
    info.inRange = cell.inRange;
    info.hoverRange = cell.hoverRange;
    info.hoverRangeStart = cell.hoverRangeStart;
    info.hoverRangeEnd = cell.hoverRangeEnd;
    info.disabled = cell.disabled || (panel_ && panel_->disabled_);
    return info;
  }

  const QString& weekdayLabel(int dayNumber) const {
    if (!panel_) {
      static const QString empty;
      return empty;
    }
    if (!weekdayLabelsValid_ || weekdayLabelsLocale_ != panel_->locale_) {
      weekdayLabels_.clear();
      weekdayLabels_.reserve(7);
      for (int day = 1; day <= 7; ++day) {
        weekdayLabels_.append(
            weekdayName(static_cast<Qt::DayOfWeek>(day), panel_->locale_));
      }
      weekdayLabelsLocale_ = panel_->locale_;
      weekdayLabelsValid_ = true;
    }
    const int index = std::clamp(dayNumber - 1, 0, static_cast<int>(weekdayLabels_.size()) - 1);
    return weekdayLabels_.at(index);
  }

  mutable QVector<Cell> cachedCells_;
  mutable QSize cachedSize_;
  mutable AdDatePickerPanel::DisplayMode cachedDisplayMode_ =
      AdDatePickerPanel::DisplayMode::Time;
  mutable bool cellsDirty_ = true;
  mutable QStringList weekdayLabels_;
  mutable QLocale weekdayLabelsLocale_;
  mutable bool weekdayLabelsValid_ = false;

  bool showWeekColumn() const {
    return panel_ && panel_->showWeek_ &&
           panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Date;
  }

  bool useWeekRowPainting() const {
    return panel_ && panel_->pickerMode_ == AdDatePickerPanel::PickerMode::Week && showWeekColumn();
  }

  int datePanelPaddingHorizontal(const DatePickerVisualStyle& style) const {
    return showWeekColumn() ? style.metrics.weekPanelPaddingHorizontal
                            : style.metrics.panelPaddingHorizontal;
  }

  int unitPanelPaddingHorizontal(const DatePickerVisualStyle& style) const {
    return panel_ && panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Decade
               ? 0
               : style.metrics.unitPanelPaddingHorizontal;
  }

  int dateGridRowFromPosition(const QPoint& pos) const {
    if (!panel_ || panel_->displayMode_ != AdDatePickerPanel::DisplayMode::Date) {
      return -1;
    }
    const DatePickerVisualStyle style = panel_->resolvedStyle();
    const int paddingX = datePanelPaddingHorizontal(style);
    const int paddingY = style.metrics.panelPaddingVertical;
    const int rowH = datePanelRowHeight(style);
    const QRect content = rect().adjusted(paddingX, paddingY, -paddingX, -paddingY);
    const int gridTop = content.top() + rowH;
    for (int row = 0; row < 6; ++row) {
      const QRect rowRect(content.left(), gridTop + row * rowH, content.width(), rowH);
      if (rowRect.contains(pos)) {
        return row;
      }
    }
    return -1;
  }

  void activateCell(const Cell& cell) {
    if (!panel_) {
      return;
    }

    if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Date) {
      if (!cell.inView) {
        panel_->setViewDate(cell.date);
      }
      panel_->selectDateFromGrid(cell.date);
      return;
    }

    if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Month) {
      if (panel_->pickerMode_ == AdDatePickerPanel::PickerMode::Month) {
        panel_->selectDateFromGrid(cell.date);
      } else {
        panel_->setDisplayMode(AdDatePickerPanel::DisplayMode::Date);
        panel_->setViewDate(cell.date);
      }
      return;
    }

    if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Quarter) {
      if (panel_->pickerMode_ == AdDatePickerPanel::PickerMode::Quarter) {
        panel_->selectDateFromGrid(cell.date);
      } else {
        panel_->setDisplayMode(AdDatePickerPanel::DisplayMode::Date);
        panel_->setViewDate(cell.date);
      }
      return;
    }

    if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Year) {
      if (panel_->pickerMode_ == AdDatePickerPanel::PickerMode::Year) {
        panel_->selectDateFromGrid(cell.date);
      } else {
        panel_->setDisplayMode(panel_->pickerMode_ == AdDatePickerPanel::PickerMode::Quarter
                                   ? AdDatePickerPanel::DisplayMode::Quarter
                                   : AdDatePickerPanel::DisplayMode::Month);
        panel_->setViewDate(cell.date);
      }
      return;
    }

    if (panel_->displayMode_ == AdDatePickerPanel::DisplayMode::Decade) {
      if (panel_->pickerMode_ == AdDatePickerPanel::PickerMode::Decade) {
        panel_->selectDateFromGrid(cell.date);
      } else {
        panel_->setDisplayMode(AdDatePickerPanel::DisplayMode::Year);
        panel_->setViewDate(cell.date);
      }
    }
  }

  AdDatePickerPanel* panel_ = nullptr;
  int hoveredIndex_ = -1;
  QDate hoveredPreviewDate_;
};

class DatePickerTimeColumnDelegate final : public QStyledItemDelegate {
 public:
  DatePickerTimeColumnDelegate(AdDatePickerPanel* panel, AdDatePickerPanel::TimeSelectionPart part,
                               AdDatePickerPanel::CellSubType subType, QObject* parent = nullptr)
      : QStyledItemDelegate(parent), panel_(panel), part_(part), subType_(subType) {}

  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override {
    QStyledItemDelegate::paint(painter, option, index);
    if (!painter || !panel_ || !panel_->cellRenderCallback() || !index.isValid()) {
      return;
    }

    const QVariant valueData = index.data(Qt::UserRole);
    bool valueOk = false;
    const int value = valueData.toInt(&valueOk);
    if (!valueOk) {
      return;
    }

    painter->save();
    panel_->cellRenderCallback()(*painter, renderInfo(option, index, value));
    painter->restore();
  }

 private:
  AdDatePickerPanel::CellRenderInfo renderInfo(const QStyleOptionViewItem& option,
                                               const QModelIndex& index, int value) const {
    AdDatePickerPanel::CellRenderInfo info;
    const QTime base = baseTime();
    const QDate date = dateForPart();
    info.date = date;
    info.time = timeForValue(base, value);
    info.text = index.data(Qt::DisplayRole).toString();
    info.type = AdDatePickerPanel::PickerMode::Time;
    info.subType = subType_;
    info.timePart = part_;
    info.value = value;
    info.cellRect = option.rect;
    info.contentRect = option.rect.adjusted(4, 0, -4, 0);
    info.inView = true;
    info.today = date.isValid() && date == todayDate();
    info.selected = option.state.testFlag(QStyle::State_Selected);
    info.rangeStart = part_ == AdDatePickerPanel::TimeSelectionPart::Start;
    info.rangeEnd = part_ == AdDatePickerPanel::TimeSelectionPart::End;
    info.disabled = panel_->disabled() || !option.state.testFlag(QStyle::State_Enabled);
    return info;
  }

  QDate dateForPart() const {
    if (!panel_) {
      return {};
    }
    if (part_ == AdDatePickerPanel::TimeSelectionPart::Start) {
      if (panel_->rangeStartDate().isValid()) {
        return panel_->rangeStartDate();
      }
      return panel_->viewDate().isValid() ? panel_->viewDate() : todayDate();
    }
    if (part_ == AdDatePickerPanel::TimeSelectionPart::End) {
      if (panel_->rangeEndDate().isValid()) {
        return panel_->rangeEndDate();
      }
      if (panel_->rangeStartDate().isValid()) {
        return panel_->rangeStartDate();
      }
      return panel_->viewDate().isValid() ? panel_->viewDate() : todayDate();
    }
    if (panel_->selectedDate().isValid()) {
      return panel_->selectedDate();
    }
    return panel_->viewDate().isValid() ? panel_->viewDate() : todayDate();
  }

  QTime baseTime() const {
    if (!panel_) {
      return defaultTimeValue();
    }
    if (part_ == AdDatePickerPanel::TimeSelectionPart::Start) {
      return normalizedTimeValue(panel_->rangeStartTime());
    }
    if (part_ == AdDatePickerPanel::TimeSelectionPart::End) {
      return normalizedTimeValue(panel_->rangeEndTime());
    }
    return normalizedTimeValue(panel_->selectedTime());
  }

  QTime timeForValue(const QTime& base, int value) const {
    const QTime normalized = normalizedTimeValue(base);
    switch (subType_) {
      case AdDatePickerPanel::CellSubType::Hour: {
        const int hour = panel_ && panel_->use12Hours()
                             ? hour24FromDisplayHour(value, normalized.hour() >= 12)
                             : std::clamp(value, 0, 23);
        return QTime(hour, normalized.minute(), normalized.second());
      }
      case AdDatePickerPanel::CellSubType::Minute:
        return QTime(normalized.hour(), std::clamp(value, 0, 59), normalized.second());
      case AdDatePickerPanel::CellSubType::Second:
        return QTime(normalized.hour(), normalized.minute(), std::clamp(value, 0, 59));
      case AdDatePickerPanel::CellSubType::Meridiem:
        return QTime(hour24FromDisplayHour(displayHourFromHour24(normalized.hour()), value == 1),
                     normalized.minute(), normalized.second());
      case AdDatePickerPanel::CellSubType::None:
      default:
        return normalized;
    }
  }

  AdDatePickerPanel* panel_ = nullptr;
  AdDatePickerPanel::TimeSelectionPart part_ = AdDatePickerPanel::TimeSelectionPart::Single;
  AdDatePickerPanel::CellSubType subType_ = AdDatePickerPanel::CellSubType::None;
};

}  // namespace detail

AdDatePickerPanel::AdDatePickerPanel(QWidget* parent) : QWidget(parent) {
  firstDayOfWeek_ = firstDayOfWeekForLocale(locale_);
  viewDate_ = todayDate();
  buildUi();
  refreshStyle();
  syncGridState();
}

AdDatePickerPanel::~AdDatePickerPanel() = default;

void AdDatePickerPanel::invalidateResolvedStyle() const { resolvedStyle_.reset(); }

const detail::DatePickerVisualStyle& AdDatePickerPanel::resolvedStyle() const {
  if (!resolvedStyle_) {
    detail::DatePickerStyleInput input;
    input.componentTokens = componentTokens_;
    input.semanticStyles = semanticStyles_;
    input.baseFont = font();
    input.disabled = disabled_;
    resolvedStyle_ =
        std::make_unique<detail::DatePickerVisualStyle>(detail::resolveDatePickerVisualStyle(
            input, adqt::theme::ThemeManager::instance().resolve(this)));
  }
  return *resolvedStyle_;
}

AdDatePickerPanel::PickerMode AdDatePickerPanel::pickerMode() const { return pickerMode_; }

void AdDatePickerPanel::setPickerMode(PickerMode value) {
  if (pickerMode_ == value) {
    return;
  }
  const QDate previousSelected = selectedDate_;
  const QVector<QDate> previousSelectedDates = selectedDates_;
  const QDate previousRangeStart = rangeStartDate_;
  const QDate previousRangeEnd = rangeEndDate_;
  const PickerMode previousPanelMode = panelMode();
  pickerMode_ = value;
  displayMode_ = defaultDisplayModeForPicker(pickerMode_);
  selectedDate_ = normalizeForPicker(pickerMode_, selectedDate_, firstDayOfWeek_);
  selectedDates_ = normalizedDates(selectedDates_);
  syncSelectedDateKeys();
  rangeStartDate_ = normalizeForPicker(pickerMode_, rangeStartDate_, firstDayOfWeek_);
  rangeEndDate_ = normalizeForPicker(pickerMode_, rangeEndDate_, firstDayOfWeek_);
  refreshStyle();
  refreshHeader();
  refreshFooter();
  syncGridState();
  emit pickerModeChanged(pickerMode_);
  if (panelMode() != previousPanelMode) {
    emit panelModeChanged(panelMode());
  }
  if (selectedDate_ != previousSelected) {
    emit selectedDateChanged(selectedDate_);
  }
  if (selectedDates_ != previousSelectedDates) {
    emit selectedDatesChanged(selectedDates_);
  }
  if (rangeStartDate_ != previousRangeStart || rangeEndDate_ != previousRangeEnd) {
    emit rangeChanged(rangeStartDate_, rangeEndDate_);
  }
}

AdDatePickerPanel::SelectionMode AdDatePickerPanel::selectionMode() const { return selectionMode_; }

void AdDatePickerPanel::setSelectionMode(SelectionMode value) {
  if (selectionMode_ == value) {
    return;
  }
  selectionMode_ = value;
  refreshStyle();
  syncGridState();
  emit selectionModeChanged(selectionMode_);
}

AdDatePickerPanel::PickerMode AdDatePickerPanel::panelMode() const {
  return panelModeForDisplayMode(displayMode_);
}

void AdDatePickerPanel::setPanelMode(PickerMode value) {
  setDisplayMode(displayModeForPanelMode(value));
}

QDate AdDatePickerPanel::selectedDate() const { return selectedDate_; }

void AdDatePickerPanel::setSelectedDate(const QDate& value) {
  const QDate normalized = normalizeForPicker(pickerMode_, value, firstDayOfWeek_);
  if (selectedDate_ == normalized) {
    return;
  }
  selectedDate_ = normalized;
  if (selectedDate_.isValid()) {
    viewDate_ = normalizedViewDate(selectedDate_);
  }
  syncGridState();
  emit selectedDateChanged(selectedDate_);
}

QVector<QDate> AdDatePickerPanel::selectedDates() const { return selectedDates_; }

void AdDatePickerPanel::setSelectedDates(const QVector<QDate>& values) {
  const QVector<QDate> normalized = normalizedDates(values);
  if (selectedDates_ == normalized) {
    return;
  }
  selectedDates_ = normalized;
  syncSelectedDateKeys();
  if (!selectedDates_.isEmpty()) {
    viewDate_ = normalizedViewDate(selectedDates_.constFirst());
  }
  syncGridState();
  emit selectedDatesChanged(selectedDates_);
}

QDateTime AdDatePickerPanel::selectedDateTime() const {
  return dateTimeFromParts(selectedDate_, selectedTime_);
}

void AdDatePickerPanel::setSelectedDateTime(const QDateTime& value) {
  if (!value.isValid()) {
    setSelectedDate(QDate());
    setSelectedTime(defaultTimeValue());
    return;
  }
  setSelectedDate(value.date());
  setSelectedTime(value.time());
}

QTime AdDatePickerPanel::selectedTime() const { return selectedTime_; }

void AdDatePickerPanel::setSelectedTime(const QTime& value) {
  const QTime normalized = normalizedTimeValue(value);
  if (selectedTime_ == normalized) {
    return;
  }
  selectedTime_ = normalized;
  syncTimeEditors();
  refreshFooter();
  emit selectedTimeChanged(selectedTime_);
}

QDate AdDatePickerPanel::rangeStartDate() const { return rangeStartDate_; }

void AdDatePickerPanel::setRangeStartDate(const QDate& value) {
  const QDate normalized = normalizeForPicker(pickerMode_, value, firstDayOfWeek_);
  if (rangeStartDate_ == normalized) {
    return;
  }
  rangeStartDate_ = normalized;
  syncGridState();
  emit rangeChanged(rangeStartDate_, rangeEndDate_);
}

QDate AdDatePickerPanel::rangeEndDate() const { return rangeEndDate_; }

void AdDatePickerPanel::setRangeEndDate(const QDate& value) {
  const QDate normalized = normalizeForPicker(pickerMode_, value, firstDayOfWeek_);
  if (rangeEndDate_ == normalized) {
    return;
  }
  rangeEndDate_ = normalized;
  syncGridState();
  emit rangeChanged(rangeStartDate_, rangeEndDate_);
}

void AdDatePickerPanel::setRange(const QDate& start, const QDate& end) {
  QDate nextStart = normalizeForPicker(pickerMode_, start, firstDayOfWeek_);
  QDate nextEnd = normalizeForPicker(pickerMode_, end, firstDayOfWeek_);
  if (order_ && nextStart.isValid() && nextEnd.isValid() && nextEnd < nextStart) {
    std::swap(nextStart, nextEnd);
  }
  if (rangeStartDate_ == nextStart && rangeEndDate_ == nextEnd) {
    // A range click can update the panel fields before the range picker sends
    // the normalized pair back through this setter. Keep the grid invalidated
    // even when the values are already equal so the current range metadata is
    // painted after every synchronization.
    if (grid_) {
      grid_->updateGeometry();
      grid_->invalidateCells();
      grid_->update();
    }
    return;
  }
  rangeStartDate_ = nextStart;
  rangeEndDate_ = nextEnd;
  if (rangeStartDate_.isValid()) {
    viewDate_ = normalizedViewDate(rangeStartDate_);
  }
  syncGridState();
  emit rangeChanged(rangeStartDate_, rangeEndDate_);
}

void AdDatePickerPanel::setHoverRange(const QDate& start, const QDate& end) {
  QDate nextStart = normalizeForPicker(pickerMode_, start, firstDayOfWeek_);
  QDate nextEnd = normalizeForPicker(pickerMode_, end, firstDayOfWeek_);
  if (nextStart.isValid() && nextEnd.isValid() && nextEnd < nextStart) {
    std::swap(nextStart, nextEnd);
  }
  const bool nextActive = nextStart.isValid() || nextEnd.isValid();
  if (hoverRangeActive_ == nextActive && hoverRangeStartDate_ == nextStart &&
      hoverRangeEndDate_ == nextEnd) {
    return;
  }
  hoverRangeActive_ = nextActive;
  hoverRangeStartDate_ = nextStart;
  hoverRangeEndDate_ = nextEnd;
  if (grid_) {
    grid_->updateGeometry();
    grid_->invalidateCells();
    grid_->update();
  }
}

void AdDatePickerPanel::clearHoverRange() {
  if (!hoverRangeActive_ && !hoverRangeStartDate_.isValid() && !hoverRangeEndDate_.isValid()) {
    if (grid_) {
      grid_->clearHoverState();
    }
    return;
  }
  hoverRangeActive_ = false;
  hoverRangeStartDate_ = QDate();
  hoverRangeEndDate_ = QDate();
  if (grid_) {
    grid_->clearHoverState();
  }
}

QDateTime AdDatePickerPanel::rangeStartDateTime() const {
  return dateTimeFromParts(rangeStartDate_, rangeStartTime_);
}

QDateTime AdDatePickerPanel::rangeEndDateTime() const {
  return dateTimeFromParts(rangeEndDate_, rangeEndTime_);
}

void AdDatePickerPanel::setDateTimeRange(const QDateTime& start, const QDateTime& end) {
  QDateTime nextStart = start;
  QDateTime nextEnd = end;
  if (order_ && nextStart.isValid() && nextEnd.isValid() && nextEnd < nextStart) {
    std::swap(nextStart, nextEnd);
  }

  QDate nextStartDate = nextStart.isValid()
                            ? normalizeForPicker(pickerMode_, nextStart.date(), firstDayOfWeek_)
                            : QDate();
  QDate nextEndDate = nextEnd.isValid()
                          ? normalizeForPicker(pickerMode_, nextEnd.date(), firstDayOfWeek_)
                          : QDate();
  QTime nextStartTime =
      nextStart.isValid() ? normalizedTimeValue(nextStart.time()) : defaultTimeValue();
  QTime nextEndTime = nextEnd.isValid() ? normalizedTimeValue(nextEnd.time()) : defaultTimeValue();

  const QDateTime normalizedStart = dateTimeFromParts(nextStartDate, nextStartTime);
  const QDateTime normalizedEnd = dateTimeFromParts(nextEndDate, nextEndTime);
  if (order_ && normalizedStart.isValid() && normalizedEnd.isValid() &&
      normalizedEnd < normalizedStart) {
    std::swap(nextStartDate, nextEndDate);
    std::swap(nextStartTime, nextEndTime);
  }

  setRange(nextStartDate, nextEndDate);
  setTimeRange(nextStartTime, nextEndTime);
}

QTime AdDatePickerPanel::rangeStartTime() const { return rangeStartTime_; }

void AdDatePickerPanel::setRangeStartTime(const QTime& value) {
  const QTime normalized = normalizedTimeValue(value);
  if (rangeStartTime_ == normalized) {
    return;
  }
  rangeStartTime_ = normalized;
  syncTimeEditors();
  refreshFooter();
  emit rangeTimeChanged(rangeStartTime_, rangeEndTime_);
}

QTime AdDatePickerPanel::rangeEndTime() const { return rangeEndTime_; }

void AdDatePickerPanel::setRangeEndTime(const QTime& value) {
  const QTime normalized = normalizedTimeValue(value);
  if (rangeEndTime_ == normalized) {
    return;
  }
  rangeEndTime_ = normalized;
  syncTimeEditors();
  refreshFooter();
  emit rangeTimeChanged(rangeStartTime_, rangeEndTime_);
}

void AdDatePickerPanel::setTimeRange(const QTime& start, const QTime& end) {
  const QTime nextStart = normalizedTimeValue(start);
  const QTime nextEnd = normalizedTimeValue(end);
  if (rangeStartTime_ == nextStart && rangeEndTime_ == nextEnd) {
    return;
  }
  rangeStartTime_ = nextStart;
  rangeEndTime_ = nextEnd;
  syncTimeEditors();
  refreshFooter();
  emit rangeTimeChanged(rangeStartTime_, rangeEndTime_);
}

QDate AdDatePickerPanel::viewDate() const { return viewDate_; }

void AdDatePickerPanel::setViewDate(const QDate& value) {
  const QDate normalized = normalizedViewDate(value.isValid() ? value : todayDate());
  if (viewDate_ == normalized) {
    return;
  }
  viewDate_ = normalized;
  refreshHeader();
  syncGridState();
  emit viewDateChanged(viewDate_);
}

QDate AdDatePickerPanel::minDate() const { return minDate_; }

void AdDatePickerPanel::setMinDate(const QDate& value) {
  if (minDate_ == value) {
    return;
  }
  minDate_ = value;
  const QDate previousView = viewDate_;
  viewDate_ = normalizedViewDate(viewDate_);
  syncGridState();
  emit minDateChanged(minDate_);
  if (viewDate_ != previousView) {
    emit viewDateChanged(viewDate_);
  }
}

QDate AdDatePickerPanel::maxDate() const { return maxDate_; }

void AdDatePickerPanel::setMaxDate(const QDate& value) {
  if (maxDate_ == value) {
    return;
  }
  maxDate_ = value;
  const QDate previousView = viewDate_;
  viewDate_ = normalizedViewDate(viewDate_);
  syncGridState();
  emit maxDateChanged(maxDate_);
  if (viewDate_ != previousView) {
    emit viewDateChanged(viewDate_);
  }
}

bool AdDatePickerPanel::showToday() const { return showToday_; }

void AdDatePickerPanel::setShowToday(bool value) {
  if (showToday_ == value) {
    return;
  }
  showToday_ = value;
  refreshFooter();
  emit showTodayChanged(showToday_);
}

bool AdDatePickerPanel::showWeek() const { return showWeek_; }

void AdDatePickerPanel::setShowWeek(bool value) {
  if (showWeek_ == value) {
    return;
  }
  showWeek_ = value;
  syncGridState();
  emit showWeekChanged(showWeek_);
}

bool AdDatePickerPanel::needConfirm() const { return needConfirm_; }

void AdDatePickerPanel::setNeedConfirm(bool value) {
  if (needConfirm_ == value) {
    return;
  }
  needConfirm_ = value;
  refreshFooter();
  emit needConfirmChanged(needConfirm_);
}

bool AdDatePickerPanel::showTime() const { return showTime_; }

void AdDatePickerPanel::setShowTime(bool value) {
  if (showTime_ == value) {
    return;
  }
  showTime_ = value;
  refreshStyle();
  syncGridState();
  emit showTimeChanged(showTime_);
}

bool AdDatePickerPanel::showNow() const { return showNow_; }

void AdDatePickerPanel::setShowNow(bool value) {
  if (showNow_ == value) {
    return;
  }
  showNow_ = value;
  refreshFooter();
  emit showNowChanged(showNow_);
}

bool AdDatePickerPanel::order() const { return order_; }

void AdDatePickerPanel::setOrder(bool value) {
  if (order_ == value) {
    return;
  }
  const QVector<QDate> previousSelectedDates = selectedDates_;
  const QDate previousRangeStart = rangeStartDate_;
  const QDate previousRangeEnd = rangeEndDate_;
  const QTime previousRangeStartTime = rangeStartTime_;
  const QTime previousRangeEndTime = rangeEndTime_;
  order_ = value;
  selectedDates_ = normalizedDates(selectedDates_);
  if (order_) {
    const QDateTime start = rangeStartDateTime();
    const QDateTime end = rangeEndDateTime();
    if (start.isValid() && end.isValid() && end < start) {
      std::swap(rangeStartDate_, rangeEndDate_);
      std::swap(rangeStartTime_, rangeEndTime_);
    }
  }
  if (rangeStartTime_ != previousRangeStartTime || rangeEndTime_ != previousRangeEndTime) {
    syncTimeEditors();
    refreshFooter();
  }
  syncGridState();
  emit orderChanged(order_);
  if (selectedDates_ != previousSelectedDates) {
    emit selectedDatesChanged(selectedDates_);
  }
  if (rangeStartDate_ != previousRangeStart || rangeEndDate_ != previousRangeEnd) {
    emit rangeChanged(rangeStartDate_, rangeEndDate_);
  }
  if (rangeStartTime_ != previousRangeStartTime || rangeEndTime_ != previousRangeEndTime) {
    emit rangeTimeChanged(rangeStartTime_, rangeEndTime_);
  }
}

QTime AdDatePickerPanel::defaultOpenTime() const { return defaultOpenTime_; }

void AdDatePickerPanel::setDefaultOpenTime(const QTime& value) {
  const QTime normalized = normalizedTimeValue(value);
  if (defaultOpenTime_ == normalized) {
    return;
  }
  const QTime previous = defaultOpenTime_;
  defaultOpenTime_ = normalized;
  if (selectionMode_ == SelectionMode::Single && !selectedDate_.isValid() &&
      selectedTime_ == previous) {
    selectedTime_ = defaultOpenTime_;
    syncTimeEditors();
    emit selectedTimeChanged(selectedTime_);
  }
  refreshFooter();
  emit defaultOpenTimeChanged(defaultOpenTime_);
}

QTime AdDatePickerPanel::defaultOpenStartTime() const { return defaultOpenStartTime_; }

void AdDatePickerPanel::setDefaultOpenStartTime(const QTime& value) {
  const QTime normalized = normalizedTimeValue(value);
  if (defaultOpenStartTime_ == normalized) {
    return;
  }
  const QTime previous = defaultOpenStartTime_;
  defaultOpenStartTime_ = normalized;
  if (selectionMode_ == SelectionMode::Range && !rangeStartDate_.isValid() &&
      rangeStartTime_ == previous) {
    rangeStartTime_ = defaultOpenStartTime_;
    syncTimeEditors();
    emit rangeTimeChanged(rangeStartTime_, rangeEndTime_);
  }
  refreshFooter();
  emit defaultOpenTimeRangeChanged(defaultOpenStartTime_, defaultOpenEndTime_);
}

QTime AdDatePickerPanel::defaultOpenEndTime() const { return defaultOpenEndTime_; }

void AdDatePickerPanel::setDefaultOpenEndTime(const QTime& value) {
  const QTime normalized = normalizedTimeValue(value);
  if (defaultOpenEndTime_ == normalized) {
    return;
  }
  const QTime previous = defaultOpenEndTime_;
  defaultOpenEndTime_ = normalized;
  if (selectionMode_ == SelectionMode::Range && !rangeEndDate_.isValid() &&
      rangeEndTime_ == previous) {
    rangeEndTime_ = defaultOpenEndTime_;
    syncTimeEditors();
    emit rangeTimeChanged(rangeStartTime_, rangeEndTime_);
  }
  refreshFooter();
  emit defaultOpenTimeRangeChanged(defaultOpenStartTime_, defaultOpenEndTime_);
}

void AdDatePickerPanel::setDefaultOpenTimeRange(const QTime& start, const QTime& end) {
  const QTime normalizedStart = normalizedTimeValue(start);
  const QTime normalizedEnd = normalizedTimeValue(end);
  if (defaultOpenStartTime_ == normalizedStart && defaultOpenEndTime_ == normalizedEnd) {
    return;
  }
  const QTime previousStart = defaultOpenStartTime_;
  const QTime previousEnd = defaultOpenEndTime_;
  defaultOpenStartTime_ = normalizedStart;
  defaultOpenEndTime_ = normalizedEnd;
  bool didRangeTimeChange = false;
  if (selectionMode_ == SelectionMode::Range && !rangeStartDate_.isValid() &&
      rangeStartTime_ == previousStart) {
    rangeStartTime_ = defaultOpenStartTime_;
    didRangeTimeChange = true;
  }
  if (selectionMode_ == SelectionMode::Range && !rangeEndDate_.isValid() &&
      rangeEndTime_ == previousEnd) {
    rangeEndTime_ = defaultOpenEndTime_;
    didRangeTimeChange = true;
  }
  if (didRangeTimeChange) {
    syncTimeEditors();
    emit rangeTimeChanged(rangeStartTime_, rangeEndTime_);
  }
  refreshFooter();
  emit defaultOpenTimeRangeChanged(defaultOpenStartTime_, defaultOpenEndTime_);
}

QString AdDatePickerPanel::timeFormat() const { return effectiveTimeFormat(); }

void AdDatePickerPanel::setTimeFormat(const QString& value) {
  const QString normalized = normalizeDateFormatSyntax(value.trimmed());
  if (timeFormat_ == normalized) {
    return;
  }
  const bool previousShowSecond = effectiveShowSecondColumn();
  timeFormat_ = normalized;
  refreshTimeEditors();
  emit timeFormatChanged(effectiveTimeFormat());
  if (previousShowSecond != effectiveShowSecondColumn()) {
    emit showSecondChanged(effectiveShowSecondColumn());
  }
}

int AdDatePickerPanel::hourStep() const { return hourStep_; }

void AdDatePickerPanel::setHourStep(int value) {
  const int normalized = normalizedTimeStep(value, 24);
  if (hourStep_ == normalized) {
    return;
  }
  hourStep_ = normalized;
  rebuildTimeColumns();
  emit timeStepChanged(hourStep_, minuteStep_, secondStep_);
}

int AdDatePickerPanel::minuteStep() const { return minuteStep_; }

void AdDatePickerPanel::setMinuteStep(int value) {
  const int normalized = normalizedTimeStep(value, 60);
  if (minuteStep_ == normalized) {
    return;
  }
  minuteStep_ = normalized;
  rebuildTimeColumns();
  emit timeStepChanged(hourStep_, minuteStep_, secondStep_);
}

int AdDatePickerPanel::secondStep() const { return secondStep_; }

void AdDatePickerPanel::setSecondStep(int value) {
  const int normalized = normalizedTimeStep(value, 60);
  if (secondStep_ == normalized) {
    return;
  }
  secondStep_ = normalized;
  rebuildTimeColumns();
  emit timeStepChanged(hourStep_, minuteStep_, secondStep_);
}

void AdDatePickerPanel::setTimeSteps(int hourStep, int minuteStep, int secondStep) {
  const int nextHourStep = normalizedTimeStep(hourStep, 24);
  const int nextMinuteStep = normalizedTimeStep(minuteStep, 60);
  const int nextSecondStep = normalizedTimeStep(secondStep, 60);
  if (hourStep_ == nextHourStep && minuteStep_ == nextMinuteStep && secondStep_ == nextSecondStep) {
    return;
  }
  hourStep_ = nextHourStep;
  minuteStep_ = nextMinuteStep;
  secondStep_ = nextSecondStep;
  rebuildTimeColumns();
  emit timeStepChanged(hourStep_, minuteStep_, secondStep_);
}

bool AdDatePickerPanel::hideDisabledOptions() const { return hideDisabledOptions_; }

void AdDatePickerPanel::setHideDisabledOptions(bool value) {
  if (hideDisabledOptions_ == value) {
    return;
  }
  hideDisabledOptions_ = value;
  updateTimeColumnStates();
  syncTimeColumnSelections();
  emit hideDisabledOptionsChanged(hideDisabledOptions_);
}

bool AdDatePickerPanel::use12Hours() const { return use12Hours_; }

void AdDatePickerPanel::setUse12Hours(bool value) {
  if (use12Hours_ == value) {
    return;
  }
  use12Hours_ = value;
  rebuildTimeColumns();
  emit use12HoursChanged(use12Hours_);
}

bool AdDatePickerPanel::changeOnScroll() const { return changeOnScroll_; }

void AdDatePickerPanel::setChangeOnScroll(bool value) {
  if (changeOnScroll_ == value) {
    return;
  }
  changeOnScroll_ = value;
  emit changeOnScrollChanged(changeOnScroll_);
}

AdDatePickerPanel::TimeSelectionPart AdDatePickerPanel::visibleRangeTimePart() const {
  return visibleRangeTimePart_;
}

void AdDatePickerPanel::setVisibleRangeTimePart(TimeSelectionPart value) {
  const TimeSelectionPart normalized =
      value == TimeSelectionPart::End ? TimeSelectionPart::End : TimeSelectionPart::Start;
  if (visibleRangeTimePart_ == normalized) {
    return;
  }
  visibleRangeTimePart_ = normalized;
  refreshStyle();
}

bool AdDatePickerPanel::showHour() const { return effectiveShowHourColumn(); }

void AdDatePickerPanel::setShowHour(bool value) {
  if (showHour_ == value) {
    return;
  }
  showHour_ = value;
  refreshStyle();
  emit showHourChanged(effectiveShowHourColumn());
}

bool AdDatePickerPanel::showMinute() const { return effectiveShowMinuteColumn(); }

void AdDatePickerPanel::setShowMinute(bool value) {
  if (showMinute_ == value) {
    return;
  }
  showMinute_ = value;
  refreshStyle();
  emit showMinuteChanged(effectiveShowMinuteColumn());
}

bool AdDatePickerPanel::showSecond() const { return effectiveShowSecondColumn(); }

void AdDatePickerPanel::setShowSecond(bool value) {
  const bool previous = effectiveShowSecondColumn();
  if (showSecondExplicit_ && showSecond_ == value) {
    return;
  }
  showSecond_ = value;
  showSecondExplicit_ = true;
  refreshStyle();
  if (previous != effectiveShowSecondColumn()) {
    emit showSecondChanged(effectiveShowSecondColumn());
  }
}

void AdDatePickerPanel::resetShowSecond() {
  if (!showSecondExplicit_) {
    return;
  }
  const bool previous = effectiveShowSecondColumn();
  showSecond_ = true;
  showSecondExplicit_ = false;
  refreshStyle();
  if (previous != effectiveShowSecondColumn()) {
    emit showSecondChanged(effectiveShowSecondColumn());
  }
}

void AdDatePickerPanel::setVisibleTimeColumns(bool hour, bool minute, bool second) {
  const bool previousHour = effectiveShowHourColumn();
  const bool previousMinute = effectiveShowMinuteColumn();
  const bool previousSecond = effectiveShowSecondColumn();
  if (showHour_ == hour && showMinute_ == minute && showSecondExplicit_ && showSecond_ == second) {
    return;
  }
  showHour_ = hour;
  showMinute_ = minute;
  showSecond_ = second;
  showSecondExplicit_ = true;
  refreshStyle();
  if (previousHour != effectiveShowHourColumn()) {
    emit showHourChanged(effectiveShowHourColumn());
  }
  if (previousMinute != effectiveShowMinuteColumn()) {
    emit showMinuteChanged(effectiveShowMinuteColumn());
  }
  if (previousSecond != effectiveShowSecondColumn()) {
    emit showSecondChanged(effectiveShowSecondColumn());
  }
}

bool AdDatePickerPanel::allowEmptyStart() const { return allowEmptyStart_; }

void AdDatePickerPanel::setAllowEmptyStart(bool value) {
  if (allowEmptyStart_ == value) {
    return;
  }
  allowEmptyStart_ = value;
  emit allowEmptyStartChanged(allowEmptyStart_);
}

bool AdDatePickerPanel::allowEmptyEnd() const { return allowEmptyEnd_; }

void AdDatePickerPanel::setAllowEmptyEnd(bool value) {
  if (allowEmptyEnd_ == value) {
    return;
  }
  allowEmptyEnd_ = value;
  emit allowEmptyEndChanged(allowEmptyEnd_);
}

void AdDatePickerPanel::setAllowEmpty(bool start, bool end) {
  setAllowEmptyStart(start);
  setAllowEmptyEnd(end);
}

bool AdDatePickerPanel::footerVisible() const { return footerVisible_; }

void AdDatePickerPanel::setFooterVisible(bool value) {
  if (footerVisible_ == value) {
    return;
  }
  footerVisible_ = value;
  refreshFooter();
  emit footerVisibleChanged(footerVisible_);
}

bool AdDatePickerPanel::disabled() const { return disabled_; }

void AdDatePickerPanel::setDisabled(bool value) {
  if (disabled_ == value) {
    return;
  }
  disabled_ = value;
  setEnabled(!disabled_);
  refreshStyle();
  syncGridState();
  updateTimeColumnStates();
  emit disabledChanged(disabled_);
}

QLocale AdDatePickerPanel::locale() const { return locale_; }

void AdDatePickerPanel::setLocale(const QLocale& value) {
  if (locale_ == value) {
    return;
  }
  locale_ = value;
  if (!applyingGlobalLocale_) {
    localeExplicit_ = true;
  }
  setFirstDayOfWeek(firstDayOfWeekForLocale(locale_));
  refreshHeader();
  refreshTimeEditors();
  syncGridState();
  emit localeChanged(locale_);
}

Qt::DayOfWeek AdDatePickerPanel::firstDayOfWeek() const { return firstDayOfWeek_; }

void AdDatePickerPanel::setFirstDayOfWeek(Qt::DayOfWeek value) {
  const Qt::DayOfWeek normalized = normalizedFirstDayOfWeek(value);
  if (firstDayOfWeek_ == normalized) {
    return;
  }
  const QDate previousSelected = selectedDate_;
  const QVector<QDate> previousSelectedDates = selectedDates_;
  const QDate previousRangeStart = rangeStartDate_;
  const QDate previousRangeEnd = rangeEndDate_;
  firstDayOfWeek_ = normalized;
  selectedDate_ = normalizeForPicker(pickerMode_, selectedDate_, firstDayOfWeek_);
  selectedDates_ = normalizedDates(selectedDates_);
  rangeStartDate_ = normalizeForPicker(pickerMode_, rangeStartDate_, firstDayOfWeek_);
  rangeEndDate_ = normalizeForPicker(pickerMode_, rangeEndDate_, firstDayOfWeek_);
  syncGridState();
  if (selectedDate_ != previousSelected) {
    emit selectedDateChanged(selectedDate_);
  }
  if (selectedDates_ != previousSelectedDates) {
    emit selectedDatesChanged(selectedDates_);
  }
  if (rangeStartDate_ != previousRangeStart || rangeEndDate_ != previousRangeEnd) {
    emit rangeChanged(rangeStartDate_, rangeEndDate_);
  }
}

adqt::icons::IconRef AdDatePickerPanel::prevIconRef() const { return prevIconRef_; }

void AdDatePickerPanel::setPrevIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(prevIconRef_, value)) {
    return;
  }
  prevIconRef_ = value;
  refreshNavigationButtons();
  emit prevIconRefChanged(prevIconRef_);
}

adqt::icons::IconRef AdDatePickerPanel::nextIconRef() const { return nextIconRef_; }

void AdDatePickerPanel::setNextIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(nextIconRef_, value)) {
    return;
  }
  nextIconRef_ = value;
  refreshNavigationButtons();
  emit nextIconRefChanged(nextIconRef_);
}

adqt::icons::IconRef AdDatePickerPanel::superPrevIconRef() const { return superPrevIconRef_; }

void AdDatePickerPanel::setSuperPrevIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(superPrevIconRef_, value)) {
    return;
  }
  superPrevIconRef_ = value;
  refreshNavigationButtons();
  emit superPrevIconRefChanged(superPrevIconRef_);
}

adqt::icons::IconRef AdDatePickerPanel::superNextIconRef() const { return superNextIconRef_; }

void AdDatePickerPanel::setSuperNextIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(superNextIconRef_, value)) {
    return;
  }
  superNextIconRef_ = value;
  refreshNavigationButtons();
  emit superNextIconRefChanged(superNextIconRef_);
}

void AdDatePickerPanel::setNavigationIconRefs(const adqt::icons::IconRef& superPrev,
                                              const adqt::icons::IconRef& prev,
                                              const adqt::icons::IconRef& next,
                                              const adqt::icons::IconRef& superNext) {
  const bool superPrevChanged = !iconRefsEqual(superPrevIconRef_, superPrev);
  const bool prevChanged = !iconRefsEqual(prevIconRef_, prev);
  const bool nextChanged = !iconRefsEqual(nextIconRef_, next);
  const bool superNextChanged = !iconRefsEqual(superNextIconRef_, superNext);
  if (!superPrevChanged && !prevChanged && !nextChanged && !superNextChanged) {
    return;
  }

  superPrevIconRef_ = superPrev;
  prevIconRef_ = prev;
  nextIconRef_ = next;
  superNextIconRef_ = superNext;
  refreshNavigationButtons();

  if (superPrevChanged) {
    emit superPrevIconRefChanged(superPrevIconRef_);
  }
  if (prevChanged) {
    emit prevIconRefChanged(prevIconRef_);
  }
  if (nextChanged) {
    emit nextIconRefChanged(nextIconRef_);
  }
  if (superNextChanged) {
    emit superNextIconRefChanged(superNextIconRef_);
  }
}

void AdDatePickerPanel::resetNavigationIconRefs() {
  setNavigationIconRefs(adqt::icons::IconRef(), adqt::icons::IconRef(), adqt::icons::IconRef(),
                        adqt::icons::IconRef());
}

bool AdDatePickerPanel::hidePreviousNavigation() const { return hidePreviousNavigation_; }

void AdDatePickerPanel::setHidePreviousNavigation(bool value) {
  if (hidePreviousNavigation_ == value) {
    return;
  }
  hidePreviousNavigation_ = value;
  refreshNavigationButtons();
  refreshHeader();
}

bool AdDatePickerPanel::hideNextNavigation() const { return hideNextNavigation_; }

void AdDatePickerPanel::setHideNextNavigation(bool value) {
  if (hideNextNavigation_ == value) {
    return;
  }
  hideNextNavigation_ = value;
  refreshNavigationButtons();
  refreshHeader();
}

AdDatePickerPanel::ComponentTokens AdDatePickerPanel::componentTokens() const {
  return componentTokens_;
}

void AdDatePickerPanel::setComponentTokens(const ComponentTokens& tokens) {
  if (panelComponentTokensEqual(componentTokens_, tokens)) {
    return;
  }
  componentTokens_ = tokens;
  refreshStyle();
  syncGridState();
  emit componentTokensChanged();
}

void AdDatePickerPanel::resetComponentTokens() { setComponentTokens(ComponentTokens()); }

AdDatePickerPanel::SemanticStyles AdDatePickerPanel::semanticStyles() const {
  return semanticStyles_;
}

void AdDatePickerPanel::setSemanticStyles(const SemanticStyles& styles) {
  if (panelSemanticStylesEqual(semanticStyles_, styles)) {
    return;
  }
  semanticStyles_ = styles;
  refreshStyle();
  syncGridState();
  emit semanticStylesChanged();
}

void AdDatePickerPanel::resetSemanticStyles() { setSemanticStyles(SemanticStyles()); }

QVector<AdDatePickerPanel::PresetItem> AdDatePickerPanel::presets() const { return presets_; }

void AdDatePickerPanel::setPresets(const QVector<PresetItem>& presets) {
  if (presetsEqual(presets_, presets)) {
    return;
  }
  presets_ = presets;
  rebuildPresets();
  emit presetsChanged();
}

void AdDatePickerPanel::clearPresets() { setPresets({}); }

QWidget* AdDatePickerPanel::extraFooterWidget() const { return extraFooterWidget_; }

void AdDatePickerPanel::setExtraFooterWidget(QWidget* widget) {
  if (extraFooterWidget_ == widget) {
    return;
  }

  if (extraFooterWidget_) {
    if (auto* layout = extraFooterHost_ ? extraFooterHost_->layout() : nullptr) {
      layout->removeWidget(extraFooterWidget_);
    }
    extraFooterWidget_->hide();
    extraFooterWidget_->setParent(nullptr);
  }

  extraFooterWidget_ = widget;
  if (extraFooterWidget_ && extraFooterHost_) {
    extraFooterWidget_->setParent(extraFooterHost_);
    if (auto* layout = extraFooterHost_->layout()) {
      layout->addWidget(extraFooterWidget_);
    }
    extraFooterWidget_->show();
  }

  refreshFooter();
  emit extraFooterWidgetChanged(extraFooterWidget_);
}

QWidget* AdDatePickerPanel::takeExtraFooterWidget() {
  QWidget* widget = extraFooterWidget_;
  if (!widget) {
    return nullptr;
  }
  if (auto* layout = extraFooterHost_ ? extraFooterHost_->layout() : nullptr) {
    layout->removeWidget(widget);
  }
  extraFooterWidget_ = nullptr;
  widget->hide();
  widget->setParent(nullptr);
  refreshFooter();
  emit extraFooterWidgetChanged(nullptr);
  return widget;
}

AdDatePickerPanel::DatePredicate AdDatePickerPanel::disabledDatePredicate() const {
  return disabledDatePredicate_;
}

void AdDatePickerPanel::setDisabledDatePredicate(DatePredicate predicate) {
  const bool hadPredicate = static_cast<bool>(disabledDatePredicate_);
  const bool hasPredicate = static_cast<bool>(predicate);
  if (!hadPredicate && !hasPredicate) {
    return;
  }
  disabledDatePredicate_ = std::move(predicate);
  syncGridState();
}

AdDatePickerPanel::DisabledDatePredicate AdDatePickerPanel::disabledDateContextPredicate() const {
  return disabledDateContextPredicate_;
}

void AdDatePickerPanel::setDisabledDateContextPredicate(DisabledDatePredicate predicate) {
  const bool hadPredicate = static_cast<bool>(disabledDateContextPredicate_);
  const bool hasPredicate = static_cast<bool>(predicate);
  if (!hadPredicate && !hasPredicate) {
    return;
  }
  disabledDateContextPredicate_ = std::move(predicate);
  syncGridState();
}

AdDatePickerPanel::DisabledTimePredicate AdDatePickerPanel::disabledTimePredicate() const {
  return disabledTimePredicate_;
}

void AdDatePickerPanel::setDisabledTimePredicate(DisabledTimePredicate predicate) {
  const bool hadPredicate = static_cast<bool>(disabledTimePredicate_);
  const bool hasPredicate = static_cast<bool>(predicate);
  if (!hadPredicate && !hasPredicate) {
    return;
  }
  disabledTimePredicate_ = std::move(predicate);
  updateTimeColumnStates();
  syncTimeColumnSelections();
  refreshFooter();
}

AdDatePickerPanel::CellRenderCallback AdDatePickerPanel::cellRenderCallback() const {
  return cellRenderCallback_;
}

void AdDatePickerPanel::setCellRenderCallback(CellRenderCallback callback) {
  const bool hadCallback = static_cast<bool>(cellRenderCallback_);
  const bool hasCallback = static_cast<bool>(callback);
  if (!hadCallback && !hasCallback) {
    return;
  }
  cellRenderCallback_ = std::move(callback);
  syncGridState();
  updateTimeColumnRendering();
  emit cellRenderCallbackChanged();
}

void AdDatePickerPanel::clearCellRenderCallback() { setCellRenderCallback(CellRenderCallback()); }

QSize AdDatePickerPanel::sizeHint() const {
  const detail::DatePickerVisualStyle style = resolvedStyle();
  const bool timeVisible = timeControlsVisible();
  const bool timePanel = displayMode_ == DisplayMode::Time;
  const QSize gridHint = grid_ ? grid_->sizeHint() : QSize();
  const int timePanelWidth = std::max(1, timePanelColumnCount()) * style.metrics.timeColumnWidth;
  const int timePanelHeight =
      style.metrics.timeColumnHeight + (timePanel ? style.metrics.timePanelPaddingTop : 0);
  int width = timePanel ? timePanelWidth : style.metrics.panelWidth;
  int height = (!timePanel || timeVisible) ? style.metrics.headerHeight : 0;
  if (timeVisible && !timePanel) {
    width = std::max(width, style.metrics.panelWidth + timePanelWidth);
    height += std::max(gridHint.height(), timePanelHeight);
  } else if (timePanel) {
    height += timePanelHeight;
  } else {
    height += gridHint.height();
  }
  const bool showTodayAction = !timeVisible && showToday_;
  const bool showNowAction = timeVisible && showNow_;
  const bool hasActions = showTodayAction || showNowAction || effectiveNeedConfirm();
  const bool hasExtraFooter = extraFooterWidget_ != nullptr;
  if (footer_ && footerVisible_ && (hasActions || hasExtraFooter)) {
    int footerHeight = hasActions ? style.metrics.footerHeight : 0;
    if (hasExtraFooter) {
      const int extraFooterContentHeight =
          extraFooterWidget_ ? extraFooterWidget_->sizeHint().height() : 0;
      footerHeight += std::max(style.metrics.footerLineHeight, extraFooterContentHeight);
      if (hasActions) {
        footerHeight += std::max(1, style.metrics.borderWidth);
      }
    }
    height += std::max(style.metrics.footerHeight, footerHeight);
  }
  if (!presets_.isEmpty()) {
    const int minWidth = style.metrics.presetsWidth;
    const int maxWidth = std::max(minWidth, style.metrics.presetsMaxWidth);
    const int hintWidth = presetsWidget_ ? presetsWidget_->sizeHint().width() : minWidth;
    width += std::min(std::max(hintWidth, minWidth), maxWidth);
  }
  return QSize(width, height);
}

QSize AdDatePickerPanel::minimumSizeHint() const { return QSize(180, 160); }

bool AdDatePickerPanel::eventFilter(QObject* watched, QEvent* event) {
  if (handleTimeColumnWheel(watched, event)) {
    return true;
  }
  if (handleTimeColumnPreview(watched, event)) {
    return false;
  }
  return QWidget::eventFilter(watched, event);
}

void AdDatePickerPanel::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (!event) {
    return;
  }
  if (event->type() == QEvent::LanguageChange) {
    if (!localeExplicit_) {
      applyingGlobalLocale_ = true;
      setLocale(adqt::locale::LocaleManager::instance().locale());
      applyingGlobalLocale_ = false;
    }
    if (todayButton_) {
      todayButton_->setText(tr("Today"));
    }
    if (okButton_) {
      okButton_->setText(tr("OK"));
    }
    refreshHeader();
    refreshFooter();
    refreshTimeEditors();
    syncGridState();
    return;
  }
  if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange ||
      event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange ||
      event->type() == QEvent::StyleChange || event->type() == QEvent::EnabledChange) {
    refreshStyle();
  }
}

void AdDatePickerPanel::buildUi() {
  setAttribute(Qt::WA_StyledBackground, false);
  setAutoFillBackground(false);
  setSemanticSlot(this, "popup.root", QStringLiteral("addatepicker-panel"));

  rootLayout_ = new QVBoxLayout(this);
  rootLayout_->setContentsMargins(0, 0, 0, 0);
  rootLayout_->setSpacing(0);

  panelLayoutWidget_ = new QWidget(this);
  setSemanticSlot(panelLayoutWidget_, "popup.panel.layout",
                  QStringLiteral("addatepicker-panel-layout"));
  panelLayout_ = new QHBoxLayout(panelLayoutWidget_);
  panelLayout_->setContentsMargins(0, 0, 0, 0);
  panelLayout_->setSpacing(0);
  rootLayout_->addWidget(panelLayoutWidget_);

  panelFrame_ = new QWidget(panelLayoutWidget_);
  setSemanticSlot(panelFrame_, "popup.panel", QStringLiteral("addatepicker-panel-frame"));
  panelFrame_->setAutoFillBackground(false);
  panelFrameLayout_ = new QVBoxLayout(panelFrame_);
  panelFrameLayout_->setContentsMargins(0, 0, 0, 0);
  panelFrameLayout_->setSpacing(0);
  panelLayout_->addWidget(panelFrame_);

  header_ = new QWidget(panelFrame_);
  setSemanticSlot(header_, "popup.header", QStringLiteral("addatepicker-panel-header"));
  headerLayout_ = new QHBoxLayout(header_);
  headerLayout_->setContentsMargins(8, 0, 8, 0);
  headerLayout_->setSpacing(2);

  superPrevButton_ = createPanelToolButton(header_, QStringLiteral("<<"));
  prevButton_ = createPanelToolButton(header_, QStringLiteral("<"));
  viewHost_ = new QWidget(header_);
  setSemanticSlot(viewHost_, "popup.header.view", QStringLiteral("addatepicker-panel-header-view"));
  viewHost_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  viewHostLayout_ = new QHBoxLayout(viewHost_);
  viewHostLayout_->setContentsMargins(0, 0, 0, 0);
  viewHostLayout_->setSpacing(8);
  viewButton_ = createPanelToolButton(viewHost_, QString());
  monthViewButton_ = createPanelToolButton(viewHost_, QString());
  setSemanticSlot(monthViewButton_, "popup.header.month",
                  QStringLiteral("addatepicker-panel-header-month"));
  yearViewButton_ = createPanelToolButton(viewHost_, QString());
  setSemanticSlot(yearViewButton_, "popup.header.year",
                  QStringLiteral("addatepicker-panel-header-year"));
  viewHostLayout_->addStretch(1);
  viewHostLayout_->addWidget(viewButton_);
  viewHostLayout_->addWidget(monthViewButton_);
  viewHostLayout_->addWidget(yearViewButton_);
  viewHostLayout_->addStretch(1);
  nextButton_ = createPanelToolButton(header_, QStringLiteral(">"));
  superNextButton_ = createPanelToolButton(header_, QStringLiteral(">>"));
  timeHeader_ = new QWidget(header_);
  setSemanticSlot(timeHeader_, "popup.header.time",
                  QStringLiteral("addatepicker-panel-time-header"));
  timeHeader_->setAttribute(Qt::WA_StyledBackground, true);
  auto* timeHeaderLayout = new QHBoxLayout(timeHeader_);
  timeHeaderLayout->setContentsMargins(0, 0, 0, 0);
  timeHeaderLayout->setSpacing(0);
  timeHeaderLabel_ = new QLabel(timeHeader_);
  timeHeaderLabel_->setAlignment(Qt::AlignCenter);
  timeHeaderLabel_->setTextInteractionFlags(Qt::NoTextInteraction);
  timeHeaderLayout->addWidget(timeHeaderLabel_);
  timeHeader_->hide();

  headerLayout_->addWidget(superPrevButton_);
  headerLayout_->addWidget(prevButton_);
  headerLayout_->addWidget(viewHost_, 1);
  headerLayout_->addWidget(nextButton_);
  headerLayout_->addWidget(superNextButton_);
  headerLayout_->addWidget(timeHeader_);
  panelFrameLayout_->addWidget(header_);

  contentWidget_ = new QWidget(panelFrame_);
  setSemanticSlot(contentWidget_, "popup.content.wrapper",
                  QStringLiteral("addatepicker-panel-content-wrapper"));
  contentLayout_ = new QHBoxLayout(contentWidget_);
  contentLayout_->setContentsMargins(0, 0, 0, 0);
  contentLayout_->setSpacing(0);
  panelFrameLayout_->addWidget(contentWidget_);

  grid_ = new detail::DatePickerCalendarGrid(this, contentWidget_);
  setSemanticSlot(grid_, "popup.content", QStringLiteral("addatepicker-panel-content"));
  contentLayout_->addWidget(grid_);

  timeWidget_ = new QWidget(contentWidget_);
  setSemanticSlot(timeWidget_, "popup.footer.time", QStringLiteral("addatepicker-panel-time"));
  timeWidget_->setAttribute(Qt::WA_StyledBackground, true);
  timeLayout_ = new QHBoxLayout(timeWidget_);
  timeLayout_->setContentsMargins(0, 0, 0, 0);
  timeLayout_->setSpacing(0);

  footer_ = new QWidget(panelFrame_);
  setSemanticSlot(footer_, "popup.footer", QStringLiteral("addatepicker-panel-footer"));
  footerOuterLayout_ = new QVBoxLayout(footer_);
  footerOuterLayout_->setContentsMargins(0, 0, 0, 0);
  footerOuterLayout_->setSpacing(0);

  extraFooterHost_ = new QWidget(footer_);
  setSemanticSlot(extraFooterHost_, "popup.footer.extra",
                  QStringLiteral("addatepicker-panel-extra-footer"));
  auto* extraFooterLayout = new QVBoxLayout(extraFooterHost_);
  extraFooterLayout->setContentsMargins(12, 0, 12, 0);
  extraFooterLayout->setSpacing(0);
  extraFooterHost_->hide();
  footerOuterLayout_->addWidget(extraFooterHost_);

  timeWidget_->hide();
  contentLayout_->addWidget(timeWidget_);

  footerActionsWidget_ = new QWidget(footer_);
  setSemanticSlot(footerActionsWidget_, "popup.footer.actions",
                  QStringLiteral("addatepicker-panel-footer-actions"));
  footerLayout_ = new QHBoxLayout(footerActionsWidget_);
  footerLayout_->setContentsMargins(12, 0, 12, 0);
  footerLayout_->setSpacing(8);
  todayButton_ = createPanelToolButton(footerActionsWidget_, tr("Today"));
  okButton_ = createPanelToolButton(footerActionsWidget_, tr("OK"));
  footerLayout_->addWidget(todayButton_);
  footerLayout_->addStretch(1);
  footerLayout_->addWidget(okButton_);
  footerOuterLayout_->addWidget(footerActionsWidget_);
  panelFrameLayout_->addWidget(footer_);

  connect(superPrevButton_, &QToolButton::clicked, this, [this]() {
    if (displayMode_ == DisplayMode::Time) {
      return;
    }
    navigate(0, displayMode_ == DisplayMode::Date
                    ? -1
                    : (displayMode_ == DisplayMode::Decade ? -100 : -10));
  });
  connect(prevButton_, &QToolButton::clicked, this, [this]() {
    if (displayMode_ == DisplayMode::Time) {
      return;
    }
    if (displayMode_ == DisplayMode::Date) {
      navigate(-1, 0);
    } else if (displayMode_ == DisplayMode::Decade) {
      navigate(0, -100);
    } else {
      navigate(0, displayMode_ == DisplayMode::Year ? -10 : -1);
    }
  });
  connect(nextButton_, &QToolButton::clicked, this, [this]() {
    if (displayMode_ == DisplayMode::Time) {
      return;
    }
    if (displayMode_ == DisplayMode::Date) {
      navigate(1, 0);
    } else if (displayMode_ == DisplayMode::Decade) {
      navigate(0, 100);
    } else {
      navigate(0, displayMode_ == DisplayMode::Year ? 10 : 1);
    }
  });
  connect(superNextButton_, &QToolButton::clicked, this, [this]() {
    if (displayMode_ == DisplayMode::Time) {
      return;
    }
    navigate(0, displayMode_ == DisplayMode::Date
                    ? 1
                    : (displayMode_ == DisplayMode::Decade ? 100 : 10));
  });
  connect(viewButton_, &QToolButton::clicked, this, [this]() { switchHeaderView(); });
  connect(monthViewButton_, &QToolButton::clicked, this, [this]() {
    if (displayMode_ == DisplayMode::Date) {
      setDisplayMode(DisplayMode::Month);
    }
  });
  connect(yearViewButton_, &QToolButton::clicked, this, [this]() {
    if (displayMode_ == DisplayMode::Date) {
      setDisplayMode(DisplayMode::Year);
    }
  });
  connect(todayButton_, &QToolButton::clicked, this, [this]() {
    const QDateTime now = QDateTime::currentDateTime();
    const QDate currentDate = normalizeForPicker(pickerMode_, now.date(), firstDayOfWeek_);
    if (!isSelectableForMode(pickerMode_, currentDate) ||
        !isTimeSelectable(currentDate, now.time(), TimeSelectionPart::Single)) {
      return;
    }
    if (timeControlsVisible()) {
      setSelectedTime(now.time());
    }
    setViewDate(currentDate);
    selectDateFromGrid(currentDate);
  });
  connect(okButton_, &QToolButton::clicked, this, [this]() { acceptCurrentSelection(); });
  connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
          [this]() { refreshStyle(); });
}

void AdDatePickerPanel::refreshStyle() {
  invalidateResolvedStyle();
  const detail::DatePickerVisualStyle style = resolvedStyle();
  if (timeControlsVisible()) {
    ensureTimeColumnsPopulated();
  }
  setFont(style.metrics.font);
  const bool timeVisible = timeControlsVisible();
  const bool timePanel = displayMode_ == DisplayMode::Time;
  const int timePanelWidth =
      timeVisible ? std::max(1, timePanelColumnCount()) * style.metrics.timeColumnWidth : 0;
  if (header_) {
    const int headerWidth = timePanel ? (timeVisible ? timePanelWidth : style.metrics.panelWidth)
                                      : style.metrics.panelWidth + timePanelWidth;
    header_->setFixedWidth(headerWidth);
    header_->setFixedHeight(style.metrics.headerHeight);
    if (headerLayout_) {
      headerLayout_->setContentsMargins(timePanel ? 0 : 8, 0, timeVisible ? 0 : 8, 0);
      headerLayout_->setSpacing(timePanel ? 0 : 2);
    }
    QPalette headerPalette = header_->palette();
    headerPalette.setColor(QPalette::Window, style.headerBackground);
    header_->setPalette(headerPalette);
    header_->setAutoFillBackground(style.headerBackground.alpha() > 0);
    header_->setStyleSheet(QStringLiteral("QWidget#addatepicker-panel-header { background: %1; "
                                          "border-bottom: %2px solid %3; }")
                               .arg(cssColor(style.headerBackground))
                               .arg(std::max(1, style.metrics.borderWidth))
                               .arg(cssColor(style.panelBorderColor)));
  }
  if (timeHeader_) {
    const bool timeHeaderHasLeftBorder = timeVisible && !timePanel;
    timeHeader_->setFixedWidth(timeVisible ? timePanelWidth : 0);
    timeHeader_->setFixedHeight(style.metrics.headerHeight);
    timeHeader_->setVisible(timeVisible);
    timeHeader_->setStyleSheet(
        QStringLiteral("QWidget#addatepicker-panel-time-header { background: %1; "
                       "border-bottom: %2px solid %3; %4 }")
            .arg(cssColor(style.headerBackground))
            .arg(std::max(1, style.metrics.borderWidth))
            .arg(cssColor(style.panelBorderColor))
            .arg(timeHeaderHasLeftBorder ? QStringLiteral("border-left: %1px solid %2;")
                                               .arg(std::max(1, style.metrics.borderWidth))
                                               .arg(cssColor(style.panelBorderColor))
                                         : QStringLiteral("border-left: none;")));
  }
  if (timeHeaderLabel_) {
    timeHeaderLabel_->setFont(style.metrics.headerFont);
    QPalette timeHeaderPalette = timeHeaderLabel_->palette();
    timeHeaderPalette.setColor(QPalette::WindowText, style.headerTextColor);
    timeHeaderLabel_->setPalette(timeHeaderPalette);
  }
  if (footerActionsWidget_) {
    footerActionsWidget_->setFixedHeight(style.metrics.footerHeight);
  }
  if (extraFooterHost_) {
    extraFooterHost_->setFont(style.metrics.font);
    extraFooterHost_->setMinimumHeight(style.metrics.footerLineHeight);
    extraFooterHost_->setMaximumHeight(QWIDGETSIZE_MAX);
    extraFooterHost_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
  }
  refreshPresetsStyle(style);
  if (timeWidget_) {
    refreshTimePanelGeometry();
    const bool timePanelDividerVisible = displayMode_ != DisplayMode::Time && timeControlsVisible();
    timeWidget_->setStyleSheet(
        QStringLiteral("QWidget#addatepicker-panel-time { background: %1; %2 }")
            .arg(cssColor(style.contentBackground),
                 timePanelDividerVisible ? QStringLiteral("border-left: %1px solid %2;")
                                               .arg(std::max(1, style.metrics.borderWidth))
                                               .arg(cssColor(style.panelBorderColor))
                                         : QStringLiteral("border: none;")));
  }
  if (footer_) {
    footer_->setMinimumHeight(style.metrics.footerHeight);
    footer_->setMaximumHeight(QWIDGETSIZE_MAX);
    QPalette footerPalette = footer_->palette();
    footerPalette.setColor(QPalette::Window, style.footerBackground);
    footer_->setPalette(footerPalette);
    footer_->setAutoFillBackground(style.footerBackground.alpha() > 0);
    footer_->setStyleSheet(QStringLiteral("QWidget#addatepicker-panel-footer { background: %1; "
                                          "border-top: %2px solid %3; }")
                               .arg(cssColor(style.footerBackground))
                               .arg(std::max(1, style.metrics.borderWidth))
                               .arg(cssColor(style.footerBorderColor)));
  }
  QColor disabledHeaderNavColor = style.secondaryTextColor;
  disabledHeaderNavColor.setAlphaF(disabledHeaderNavColor.alphaF() * 0.25F);
  const QString headerNavButtonStyle =
      QStringLiteral(
          "QToolButton { background: transparent; border: none; padding: 0; }"
          "QToolButton:hover { color: %1; background: transparent; }"
          "QToolButton:disabled { color: %2; background: transparent; }")
          .arg(cssColor(style.headerTextColor), cssColor(disabledHeaderNavColor));
  const QList<QToolButton*> headerNavButtons = {superPrevButton_, prevButton_, nextButton_,
                                                superNextButton_};
  for (QToolButton* button : headerNavButtons) {
    if (!button) {
      continue;
    }
    button->setFont(style.metrics.font);
    setButtonPalette(button, style.secondaryTextColor);
    button->setStyleSheet(headerNavButtonStyle);
  }
  if (viewButton_) {
    const QString headerViewButtonStyle =
        QStringLiteral(
            "QToolButton { background: transparent; border: none; padding: 0; }"
            "QToolButton:hover { color: %1; background: transparent; }"
            "QToolButton:disabled { color: %2; background: transparent; }")
            .arg(cssColor(style.linkColor), cssColor(style.disabledTextColor));
    const QList<QToolButton*> headerViewButtons = {viewButton_, monthViewButton_, yearViewButton_};
    for (QToolButton* button : headerViewButtons) {
      if (!button) {
        continue;
      }
      button->setFont(style.metrics.headerFont);
      button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);
      setButtonPalette(button, style.headerTextColor);
      button->setStyleSheet(headerViewButtonStyle);
    }
  }
  if (todayButton_) {
    todayButton_->setFont(style.metrics.font);
    todayButton_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    setButtonPalette(todayButton_, style.linkColor);
    todayButton_->setStyleSheet(
        QStringLiteral(
            "QToolButton { background: transparent; border: none; padding: 0; color: %1; }"
            "QToolButton:hover { background: transparent; color: %1; }"
            "QToolButton:disabled { background: transparent; color: %2; }")
            .arg(cssColor(style.linkColor), cssColor(style.disabledTextColor)));
  }
  if (okButton_) {
    okButton_->setFont(style.metrics.font);
    okButton_->setFixedHeight(std::max(20, style.metrics.cellHeight));
    okButton_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    setButtonPalette(okButton_, style.selectedTextColor);
    okButton_->setStyleSheet(
        QStringLiteral("QToolButton { background: %1; border: %2px solid %1; border-radius: %3px; "
                       "padding: 0 7px; color: %4; }"
                       "QToolButton:hover { background: %1; border-color: %1; color: %4; }"
                       "QToolButton:disabled { background: %5; border-color: %5; color: %6; }")
            .arg(cssColor(style.selectedBackground))
            .arg(std::max(1, style.metrics.borderWidth))
            .arg(std::max(0, style.metrics.cellRadius))
            .arg(cssColor(style.selectedTextColor))
            .arg(cssColor(style.hoverBackground))
            .arg(cssColor(style.disabledTextColor)));
  }
  refreshNavigationButtons();
  const QList<QLabel*> timeLabels = {singleTimeLabel_, rangeStartTimeLabel_, rangeEndTimeLabel_};
  for (QLabel* label : timeLabels) {
    if (!label) {
      continue;
    }
    label->setFont(style.metrics.smallFont);
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, style.secondaryTextColor);
    label->setPalette(palette);
  }
  const QString timeEditStyle =
      QStringLiteral(
          "QTimeEdit { background: %1; border: 1px solid %2; "
          "border-radius: 4px; padding: 2px 6px; color: %3; }"
          "QTimeEdit:disabled { color: %4; }")
          .arg(cssColor(style.footerBackground), cssColor(style.panelBorderColor),
               cssColor(style.textColor), cssColor(style.disabledTextColor));
  const QList<QListWidget*> timeColumns = {
      selectedHourList_,   selectedMinuteList_,   selectedSecondList_,   selectedMeridiemList_,
      rangeStartHourList_, rangeStartMinuteList_, rangeStartSecondList_, rangeStartMeridiemList_,
      rangeEndHourList_,   rangeEndMinuteList_,   rangeEndSecondList_,   rangeEndMeridiemList_};
  for (QListWidget* list : timeColumns) {
    if (!list) {
      continue;
    }
    const int columnHeight = effectiveTimeColumnListHeight(list, style);
    list->setFont(style.metrics.font);
    list->setFixedWidth(style.metrics.timeColumnWidth);
    list->setFixedHeight(columnHeight);
    for (int i = 0; i < list->count(); ++i) {
      if (QListWidgetItem* item = list->item(i)) {
        if (isTimeColumnSpacerItem(item)) {
          continue;
        }
        item->setSizeHint(QSize(style.metrics.timeColumnWidth, style.metrics.timeCellHeight));
      }
    }
    syncTimeColumnSpacer(list, style, columnHeight);
    if (auto* timeList = dynamic_cast<DatePickerTimeColumnList*>(list)) {
      timeList->applyOverlayScrollBarStyle(style);
    }
  }
  const QList<QTimeEdit*> timeEdits = {selectedTimeEdit_, rangeStartTimeEdit_, rangeEndTimeEdit_};
  for (QTimeEdit* timeEdit : timeEdits) {
    if (!timeEdit) {
      continue;
    }
    timeEdit->setFont(style.metrics.font);
    timeEdit->setFixedHeight(style.metrics.timeCellHeight);
    timeEdit->setFixedWidth(style.metrics.timeColumnWidth);
    timeEdit->setStyleSheet(timeEditStyle);
  }
  if (grid_) {
    grid_->setFixedWidth(style.metrics.panelWidth);
    grid_->updateGeometry();
    grid_->invalidateCells();
    grid_->update();
  }
  refreshTimeEditors();
  updateGeometry();
  update();
}

void AdDatePickerPanel::refreshNavigationButtons() {
  const detail::DatePickerVisualStyle style = resolvedStyle();
  const int iconSide = std::max(12, std::min(16, style.metrics.headerHeight - 12));
  const int navMinWidth = std::max(1, (style.metrics.font.pixelSize() * 16 + 9) / 10);

  setPanelNavigationButtonIcon(superPrevButton_, superPrevIconRef_, NavigationIconSlot::SuperPrev,
                               style.secondaryTextColor, iconSide);
  setPanelNavigationButtonIcon(prevButton_, prevIconRef_, NavigationIconSlot::Prev,
                               style.secondaryTextColor, iconSide);
  setPanelNavigationButtonIcon(nextButton_, nextIconRef_, NavigationIconSlot::Next,
                               style.secondaryTextColor, iconSide);
  setPanelNavigationButtonIcon(superNextButton_, superNextIconRef_, NavigationIconSlot::SuperNext,
                               style.secondaryTextColor, iconSide);
  const QList<QToolButton*> headerNavButtons = {superPrevButton_, prevButton_, nextButton_,
                                                superNextButton_};
  for (QToolButton* button : headerNavButtons) {
    if (!button) {
      continue;
    }
    button->setMinimumWidth(navMinWidth);
    button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);
  }
  const auto reserveHiddenButton = [navMinWidth](QToolButton* button, bool hidden) {
    if (!button) {
      return;
    }
    if (hidden) {
      button->setIcon(QIcon());
      button->setText(QString());
      button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    }
    button->setAttribute(Qt::WA_TransparentForMouseEvents, hidden);
    button->setMinimumWidth(std::max(button->minimumWidth(), navMinWidth));
  };
  reserveHiddenButton(superPrevButton_, hidePreviousNavigation_);
  reserveHiddenButton(prevButton_, hidePreviousNavigation_);
  reserveHiddenButton(nextButton_, hideNextNavigation_);
  reserveHiddenButton(superNextButton_, hideNextNavigation_);
}

void AdDatePickerPanel::refreshHeader() {
  if (!viewButton_) {
    return;
  }

  const QDate view = viewDate_.isValid() ? viewDate_ : todayDate();
  const bool timePanel = displayMode_ == DisplayMode::Time;
  const bool showDateHeader = !timePanel;
  const bool splitDateHeader = displayMode_ == DisplayMode::Date;
  const QList<QWidget*> dateHeaderControls = {superPrevButton_, prevButton_, viewHost_, nextButton_,
                                              superNextButton_};
  for (QWidget* control : dateHeaderControls) {
    if (control) {
      control->setVisible(showDateHeader);
    }
  }
  if (timeHeader_) {
    timeHeader_->setVisible(timeControlsVisible());
  }
  viewButton_->setVisible(showDateHeader && !splitDateHeader);
  if (monthViewButton_) {
    monthViewButton_->setVisible(showDateHeader && splitDateHeader);
  }
  if (yearViewButton_) {
    yearViewButton_->setVisible(showDateHeader && splitDateHeader);
  }
  if (timePanel) {
    viewButton_->setText(tr("Time"));
  } else if (displayMode_ == DisplayMode::Date) {
    if (monthViewButton_) {
      monthViewButton_->setText(monthName(view.month(), locale_, true));
    }
    if (yearViewButton_) {
      yearViewButton_->setText(QString::number(view.year()));
    }
  } else if (displayMode_ == DisplayMode::Month || displayMode_ == DisplayMode::Quarter) {
    viewButton_->setText(QString::number(view.year()));
  } else if (displayMode_ == DisplayMode::Year) {
    const int start = decadeStartForYear(view.year());
    viewButton_->setText(QStringLiteral("%1-%2").arg(start).arg(start + 9));
  } else {
    const int start = centuryStartForYear(view.year());
    viewButton_->setText(QStringLiteral("%1-%2").arg(start).arg(start + 99));
  }

  const auto navigationTarget = [this](int months, int years) {
    QDate target = viewDate_.isValid() ? viewDate_ : todayDate();
    if (months != 0) {
      target = target.addMonths(months);
    }
    if (years != 0) {
      target = target.addYears(years);
    }
    return target;
  };
  const auto superYearStep = [this](int direction) {
    if (displayMode_ == DisplayMode::Time) {
      return 0;
    }
    if (displayMode_ == DisplayMode::Date) {
      return direction;
    }
    return direction * (displayMode_ == DisplayMode::Decade ? 100 : 10);
  };
  const auto adjacentYearStep = [this](int direction) {
    if (displayMode_ == DisplayMode::Time) {
      return 0;
    }
    if (displayMode_ == DisplayMode::Year) {
      return direction * 10;
    }
    if (displayMode_ == DisplayMode::Decade) {
      return direction * 100;
    }
    return direction;
  };
  if (superPrevButton_) {
    superPrevButton_->setEnabled(!hidePreviousNavigation_ && !disabled_ &&
                                 viewDateCanDisplay(navigationTarget(0, superYearStep(-1))));
  }
  if (prevButton_) {
    prevButton_->setEnabled(!hidePreviousNavigation_ && !disabled_ &&
                            viewDateCanDisplay(navigationTarget(
                                displayMode_ == DisplayMode::Date ? -1 : 0,
                                displayMode_ == DisplayMode::Date ? 0 : adjacentYearStep(-1))));
  }
  if (nextButton_) {
    nextButton_->setEnabled(!hideNextNavigation_ && !disabled_ &&
                            viewDateCanDisplay(navigationTarget(
                                displayMode_ == DisplayMode::Date ? 1 : 0,
                                displayMode_ == DisplayMode::Date ? 0 : adjacentYearStep(1))));
  }
  if (superNextButton_) {
    superNextButton_->setEnabled(!hideNextNavigation_ && !disabled_ &&
                                 viewDateCanDisplay(navigationTarget(0, superYearStep(1))));
  }
  if (todayButton_) {
    const QDate currentDate = todayDate();
    const QTime currentTime = QTime::currentTime();
    todayButton_->setEnabled(!disabled_ && isSelectableForMode(pickerMode_, currentDate) &&
                             isTimeSelectable(currentDate, currentTime, TimeSelectionPart::Single));
  }

  if (grid_) {
    grid_->updateGeometry();
    grid_->invalidateCells();
    grid_->update();
  }
}

void AdDatePickerPanel::refreshFooter() {
  refreshTimeEditors();
  const bool hasTime = timeControlsVisible();
  const bool showTodayAction = !hasTime && showToday_;
  const bool showNowAction = hasTime && showNow_;
  const bool hasActions = showTodayAction || showNowAction || effectiveNeedConfirm();
  const bool hasPresets = !presets_.isEmpty();
  const bool hasExtraFooter = extraFooterWidget_ != nullptr;
  const bool visible = footerVisible_ && (hasActions || hasExtraFooter);
  if (footer_) {
    footer_->setVisible(visible);
  }
  if (extraFooterHost_) {
    extraFooterHost_->setVisible(hasExtraFooter);
    const detail::DatePickerVisualStyle style = resolvedStyle();
    const int borderWidth = std::max(1, style.metrics.borderWidth);
    const int extraFooterMinHeight =
        style.metrics.footerLineHeight + (hasActions ? borderWidth : 0);
    extraFooterHost_->setMinimumHeight(extraFooterMinHeight);
    extraFooterHost_->setStyleSheet(
        QStringLiteral("QWidget#addatepicker-panel-extra-footer { background: %1; color: %2; %3 }")
            .arg(cssColor(style.footerBackground))
            .arg(cssColor(style.textColor))
            .arg(hasActions ? QStringLiteral("border-bottom: %1px solid %2;")
                                  .arg(borderWidth)
                                  .arg(cssColor(style.footerBorderColor))
                            : QStringLiteral("border: none;")));
  }
  if (presetsWidget_) {
    presetsWidget_->setVisible(hasPresets);
  }
  if (timeWidget_) {
    timeWidget_->setVisible(hasTime);
  }
  if (footerActionsWidget_) {
    footerActionsWidget_->setVisible(hasActions);
  }
  if (todayButton_) {
    todayButton_->setText(showNowAction ? tr("Now") : tr("Today"));
    todayButton_->setVisible(showTodayAction || showNowAction);
  }
  if (okButton_) {
    okButton_->setVisible(effectiveNeedConfirm());
    bool okEnabled = !disabled_;
    if (okEnabled && selectionMode_ == SelectionMode::Single) {
      const QDate valueDate = displayMode_ == DisplayMode::Time
                                  ? effectiveDateForTimePart(TimeSelectionPart::Single)
                                  : selectedDate_;
      okEnabled = valueDate.isValid() && isSelectableForMode(pickerMode_, valueDate) &&
                  isTimeSelectable(valueDate, selectedTime_, TimeSelectionPart::Single);
    } else if (okEnabled && selectionMode_ == SelectionMode::Multiple) {
      okEnabled = std::any_of(
          selectedDates_.cbegin(), selectedDates_.cend(),
          [this](const QDate& value) { return isSelectableForMode(pickerMode_, value); });
    } else if (okEnabled && selectionMode_ == SelectionMode::Range) {
      const bool startTimeOk =
          !rangeStartDate_.isValid() || isTimeSelectable(rangeStartDate_, rangeStartTime_,
                                                         TimeSelectionPart::Start, rangeEndDate_);
      const bool endTimeOk =
          !rangeEndDate_.isValid() ||
          isTimeSelectable(rangeEndDate_, rangeEndTime_, TimeSelectionPart::End, rangeStartDate_);
      okEnabled = canAcceptRange(rangeStartDate_, rangeEndDate_) && startTimeOk && endTimeOk;
    }
    okButton_->setEnabled(okEnabled);
  }
  updateGeometry();
}

void AdDatePickerPanel::refreshTimeEditors() {
  const bool visible = timeControlsVisible();
  const bool range = selectionMode_ == SelectionMode::Range;
  const QString format = effectiveTimeFormat();
  const bool showHour = effectiveShowHourColumn();
  const bool showMinute = effectiveShowMinuteColumn();
  const bool showSeconds = effectiveShowSecondColumn();
  const bool showStartTime = visible && range && visibleRangeTimePart_ != TimeSelectionPart::End;
  const bool showEndTime = visible && range && visibleRangeTimePart_ == TimeSelectionPart::End;
  if (timeHeader_) {
    timeHeader_->setVisible(visible);
  }
  if (timeHeaderLabel_) {
    const QTime headerTime =
        range ? (visibleRangeTimePart_ == TimeSelectionPart::End ? rangeEndTime_ : rangeStartTime_)
              : selectedTime_;
    const QString headerText =
        visible ? normalizedTimeValue(headerTime).toString(format) : QString();
    timeHeaderLabel_->setText(headerText.isEmpty() ? QStringLiteral(" ") : headerText);
  }
  const QList<QTimeEdit*> edits = {selectedTimeEdit_, rangeStartTimeEdit_, rangeEndTimeEdit_};
  for (QTimeEdit* edit : edits) {
    if (!edit) {
      continue;
    }
    edit->setLocale(locale_);
    edit->setDisplayFormat(format);
    edit->setEnabled(!disabled_);
  }
  if (singleTimeLabel_) {
    singleTimeLabel_->setVisible(false);
  }
  if (selectedTimeEdit_) {
    selectedTimeEdit_->setVisible(false);
  }
  if (selectedHourList_) {
    selectedHourList_->setVisible(visible && !range && showHour);
  }
  if (selectedMinuteList_) {
    selectedMinuteList_->setVisible(visible && !range && showMinute);
  }
  if (selectedSecondList_) {
    selectedSecondList_->setVisible(visible && !range && showSeconds);
  }
  if (selectedMeridiemList_) {
    selectedMeridiemList_->setVisible(visible && !range && use12Hours_ && showHour);
  }
  if (rangeStartTimeLabel_) {
    rangeStartTimeLabel_->setVisible(false);
  }
  if (rangeStartTimeEdit_) {
    rangeStartTimeEdit_->setVisible(false);
  }
  if (rangeStartHourList_) {
    rangeStartHourList_->setVisible(showStartTime && showHour);
  }
  if (rangeStartMinuteList_) {
    rangeStartMinuteList_->setVisible(showStartTime && showMinute);
  }
  if (rangeStartSecondList_) {
    rangeStartSecondList_->setVisible(showStartTime && showSeconds);
  }
  if (rangeStartMeridiemList_) {
    rangeStartMeridiemList_->setVisible(showStartTime && use12Hours_ && showHour);
  }
  if (rangeEndTimeLabel_) {
    rangeEndTimeLabel_->setVisible(false);
  }
  if (rangeEndTimeEdit_) {
    rangeEndTimeEdit_->setVisible(false);
  }
  if (rangeEndHourList_) {
    rangeEndHourList_->setVisible(showEndTime && showHour);
  }
  if (rangeEndMinuteList_) {
    rangeEndMinuteList_->setVisible(showEndTime && showMinute);
  }
  if (rangeEndSecondList_) {
    rangeEndSecondList_->setVisible(showEndTime && showSeconds);
  }
  if (rangeEndMeridiemList_) {
    rangeEndMeridiemList_->setVisible(showEndTime && use12Hours_ && showHour);
  }
  refreshTimePanelGeometry();
  updateTimeColumnStates();
  syncTimeEditors();
}

void AdDatePickerPanel::refreshPanelBodyVisibility() {
  const bool timePanel = displayMode_ == DisplayMode::Time;
  if (header_) {
    header_->setVisible(!timePanel || timeControlsVisible());
  }
  if (grid_) {
    grid_->setVisible(!timePanel);
  }
}

void AdDatePickerPanel::ensureTimeControlsCreated() {
  if (!timeWidget_ || !timeLayout_ || selectionMode_ == SelectionMode::Multiple) {
    return;
  }

  if (timeLayout_->count() == 0) {
    timeLayout_->addStretch(1);
  }

  const auto appendBeforeStretch = [this](QWidget* widget) {
    timeLayout_->insertWidget(std::max(0, timeLayout_->count() - 1), widget);
  };
  const auto createGroup = [this, &appendBeforeStretch](
                               TimeSelectionPart part, const QString& labelText, QLabel*& label,
                               QTimeEdit*& edit, QListWidget*& hourList, QListWidget*& minuteList,
                               QListWidget*& secondList, QListWidget*& meridiemList) {
    if (hourList) {
      return;
    }

    label = new QLabel(labelText, timeWidget_);
    edit = new QTimeEdit(timeWidget_);
    edit->setKeyboardTracking(false);
    edit->hide();
    hourList = createTimeColumn(timeWidget_, 23, hourStep_, false);
    minuteList = createTimeColumn(timeWidget_, 59, minuteStep_, false);
    secondList = createTimeColumn(timeWidget_, 59, secondStep_, false);
    meridiemList = createMeridiemColumn(timeWidget_, locale_, false);

    const auto setDelegate = [this, part](QListWidget* list, CellSubType subType) {
      list->setItemDelegate(new detail::DatePickerTimeColumnDelegate(this, part, subType, list));
    };
    setDelegate(hourList, CellSubType::Hour);
    setDelegate(minuteList, CellSubType::Minute);
    setDelegate(secondList, CellSubType::Second);
    setDelegate(meridiemList, CellSubType::Meridiem);

    const QList<QListWidget*> columns = {hourList, minuteList, secondList, meridiemList};
    for (QListWidget* list : columns) {
      list->installEventFilter(this);
      if (list->viewport()) {
        list->viewport()->installEventFilter(this);
      }
    }

    appendBeforeStretch(label);
    appendBeforeStretch(hourList);
    appendBeforeStretch(minuteList);
    appendBeforeStretch(secondList);
    appendBeforeStretch(meridiemList);

    if (part == TimeSelectionPart::Start) {
      connect(edit, &QTimeEdit::timeChanged, this,
              [this](const QTime& value) { setRangeStartTime(value); });
    } else if (part == TimeSelectionPart::End) {
      connect(edit, &QTimeEdit::timeChanged, this,
              [this](const QTime& value) { setRangeEndTime(value); });
    } else {
      connect(edit, &QTimeEdit::timeChanged, this,
              [this](const QTime& value) { setSelectedTime(value); });
    }

    const auto update = [this, hourList, minuteList, secondList, meridiemList, part]() {
      const QTime current = part == TimeSelectionPart::Start
                                ? rangeStartTime_
                                : (part == TimeSelectionPart::End ? rangeEndTime_ : selectedTime_);
      const QTime normalized = normalizedTimeValue(current);
      const auto currentValue = [](QListWidget* list, int fallback) {
        return list && list->isVisible() && list->currentItem()
                   ? list->currentItem()->data(Qt::UserRole).toInt()
                   : fallback;
      };
      setTimeFromColumnRows(
          part,
          currentValue(hourList,
                       use12Hours_ ? displayHourFromHour24(normalized.hour()) : normalized.hour()),
          currentValue(minuteList, normalized.minute()),
          currentValue(secondList, normalized.second()),
          currentValue(meridiemList, normalized.hour() >= 12 ? 1 : 0) == 1);
    };
    connect(hourList, &QListWidget::currentRowChanged, this, update);
    connect(minuteList, &QListWidget::currentRowChanged, this, update);
    connect(secondList, &QListWidget::currentRowChanged, this, update);
    connect(meridiemList, &QListWidget::currentRowChanged, this, update);
    timeColumnsPopulated_ = false;
  };

  if (selectionMode_ == SelectionMode::Range) {
    if (visibleRangeTimePart_ == TimeSelectionPart::End) {
      createGroup(TimeSelectionPart::End, tr("End"), rangeEndTimeLabel_, rangeEndTimeEdit_,
                  rangeEndHourList_, rangeEndMinuteList_, rangeEndSecondList_,
                  rangeEndMeridiemList_);
    } else {
      createGroup(TimeSelectionPart::Start, tr("Start"), rangeStartTimeLabel_, rangeStartTimeEdit_,
                  rangeStartHourList_, rangeStartMinuteList_, rangeStartSecondList_,
                  rangeStartMeridiemList_);
    }
  } else {
    createGroup(TimeSelectionPart::Single, tr("Time"), singleTimeLabel_, selectedTimeEdit_,
                selectedHourList_, selectedMinuteList_, selectedSecondList_, selectedMeridiemList_);
  }
}

void AdDatePickerPanel::ensureTimeColumnsPopulated() {
  ensureTimeControlsCreated();
  if (timeColumnsPopulated_) {
    return;
  }
  const QList<QListWidget*> hourLists = {selectedHourList_, rangeStartHourList_, rangeEndHourList_};
  const QList<QListWidget*> minuteLists = {selectedMinuteList_, rangeStartMinuteList_,
                                           rangeEndMinuteList_};
  const QList<QListWidget*> secondLists = {selectedSecondList_, rangeStartSecondList_,
                                           rangeEndSecondList_};
  const QList<QListWidget*> meridiemLists = {selectedMeridiemList_, rangeStartMeridiemList_,
                                             rangeEndMeridiemList_};
  for (QListWidget* list : hourLists) {
    populateHourColumn(list, hourStep_, use12Hours_);
  }
  for (QListWidget* list : minuteLists) {
    populateTimeColumn(list, 59, minuteStep_);
  }
  for (QListWidget* list : secondLists) {
    populateTimeColumn(list, 59, secondStep_);
  }
  for (QListWidget* list : meridiemLists) {
    populateMeridiemColumn(list, locale_);
  }
  timeColumnsPopulated_ = true;
}

void AdDatePickerPanel::rebuildTimeColumns() {
  if (!timeColumnsPopulated_) {
    refreshStyle();
    refreshTimeEditors();
    return;
  }
  const QList<QListWidget*> hourLists = {selectedHourList_, rangeStartHourList_, rangeEndHourList_};
  const QList<QListWidget*> minuteLists = {selectedMinuteList_, rangeStartMinuteList_,
                                           rangeEndMinuteList_};
  const QList<QListWidget*> secondLists = {selectedSecondList_, rangeStartSecondList_,
                                           rangeEndSecondList_};
  const QList<QListWidget*> meridiemLists = {selectedMeridiemList_, rangeStartMeridiemList_,
                                             rangeEndMeridiemList_};
  for (QListWidget* list : hourLists) {
    populateHourColumn(list, hourStep_, use12Hours_);
  }
  for (QListWidget* list : minuteLists) {
    populateTimeColumn(list, 59, minuteStep_);
  }
  for (QListWidget* list : secondLists) {
    populateTimeColumn(list, 59, secondStep_);
  }
  for (QListWidget* list : meridiemLists) {
    populateMeridiemColumn(list, locale_);
  }
  refreshStyle();
  refreshTimeEditors();
}

void AdDatePickerPanel::syncTimeColumnSelections() {
  const auto syncGroup = [this](QListWidget* hourList, QListWidget* minuteList,
                                QListWidget* secondList, QListWidget* meridiemList,
                                const QTime& time) {
    const QTime normalized = normalizedTimeValue(time);
    const QList<QPair<QListWidget*, int>> rows = {
        {hourList, use12Hours_ ? displayHourFromHour24(normalized.hour()) : normalized.hour()},
        {minuteList, normalized.minute()},
        {secondList, normalized.second()}};
    for (const auto& row : rows) {
      QListWidget* list = row.first;
      if (!list) {
        continue;
      }
      const QSignalBlocker blocker(list);
      int bestRow = 0;
      int bestDistance = std::numeric_limits<int>::max();
      for (int i = 0; i < list->count(); ++i) {
        const QListWidgetItem* item = list->item(i);
        if (!item || item->isHidden() || isTimeColumnSpacerItem(item)) {
          continue;
        }
        const int distance = std::abs(item->data(Qt::UserRole).toInt() - row.second);
        if (distance < bestDistance) {
          bestDistance = distance;
          bestRow = i;
        }
      }
      if (bestDistance == std::numeric_limits<int>::max()) {
        continue;
      }
      const bool alreadyCurrent = list->currentRow() == bestRow;
      list->setCurrentRow(bestRow, QItemSelectionModel::ClearAndSelect);
      if (QListWidgetItem* item = list->item(bestRow)) {
        if (!alreadyCurrent) {
          list->scrollToItem(item, QAbstractItemView::PositionAtTop);
        }
      }
    }
    if (meridiemList) {
      const QSignalBlocker blocker(meridiemList);
      const int target = normalized.hour() >= 12 ? 1 : 0;
      for (int i = 0; i < meridiemList->count(); ++i) {
        const QListWidgetItem* item = meridiemList->item(i);
        if (item && !isTimeColumnSpacerItem(item) && item->data(Qt::UserRole).toInt() == target) {
          const bool alreadyCurrent = meridiemList->currentRow() == i;
          meridiemList->setCurrentRow(i, QItemSelectionModel::ClearAndSelect);
          if (!alreadyCurrent) {
            meridiemList->scrollToItem(meridiemList->item(i), QAbstractItemView::PositionAtTop);
          }
          break;
        }
      }
    }
  };

  syncGroup(selectedHourList_, selectedMinuteList_, selectedSecondList_, selectedMeridiemList_,
            selectedTime_);
  syncGroup(rangeStartHourList_, rangeStartMinuteList_, rangeStartSecondList_,
            rangeStartMeridiemList_, rangeStartTime_);
  syncGroup(rangeEndHourList_, rangeEndMinuteList_, rangeEndSecondList_, rangeEndMeridiemList_,
            rangeEndTime_);
}

void AdDatePickerPanel::updateTimeColumnStates() {
  const auto dateForPart = [this](TimeSelectionPart part) {
    switch (part) {
      case TimeSelectionPart::Start:
        return rangeStartDate_.isValid() ? rangeStartDate_
                                         : (viewDate_.isValid() ? viewDate_ : todayDate());
      case TimeSelectionPart::End:
        return rangeEndDate_.isValid()
                   ? rangeEndDate_
                   : (rangeStartDate_.isValid() ? rangeStartDate_
                                                : (viewDate_.isValid() ? viewDate_ : todayDate()));
      case TimeSelectionPart::Single:
      default:
        return selectedDate_.isValid() ? selectedDate_
                                       : (viewDate_.isValid() ? viewDate_ : todayDate());
    }
  };
  const auto fromForPart = [this](TimeSelectionPart part) {
    switch (part) {
      case TimeSelectionPart::Start:
        return rangeEndDate_;
      case TimeSelectionPart::End:
        return rangeStartDate_;
      case TimeSelectionPart::Single:
      default:
        return QDate();
    }
  };
  const auto setGroupState = [this, dateForPart, fromForPart](
                                 QListWidget* hourList, QListWidget* minuteList,
                                 QListWidget* secondList, QListWidget* meridiemList,
                                 const QTime& current, TimeSelectionPart part) {
    const QDate date = dateForPart(part);
    const QDate from = fromForPart(part);
    const QTime normalized = normalizedTimeValue(current);
    const int displayHour =
        use12Hours_ ? displayHourFromHour24(normalized.hour()) : normalized.hour();
    const bool currentPm = normalized.hour() >= 12;
    const auto updateList = [this, date, from, normalized, part, displayHour, currentPm](
                                QListWidget* list, int component) {
      if (!list) {
        return;
      }
      for (int i = 0; i < list->count(); ++i) {
        QListWidgetItem* item = list->item(i);
        if (!item || isTimeColumnSpacerItem(item)) {
          continue;
        }
        const int value = item->data(Qt::UserRole).toInt();
        QTime probe = normalized;
        if (component == 0) {
          const int hour = use12Hours_ ? hour24FromDisplayHour(value, currentPm) : value;
          probe = QTime(hour, normalized.minute(), normalized.second());
        } else if (component == 1) {
          probe = QTime(normalized.hour(), value, normalized.second());
        } else if (component == 2) {
          probe = QTime(normalized.hour(), normalized.minute(), value);
        } else {
          probe = QTime(hour24FromDisplayHour(displayHour, value == 1), normalized.minute(),
                        normalized.second());
        }
        Qt::ItemFlags flags = item->flags();
        const bool selectable = isTimeSelectable(date, probe, part, from);
        if (!disabled_ && selectable) {
          flags |= Qt::ItemIsEnabled;
        } else {
          flags &= ~Qt::ItemIsEnabled;
        }
        item->setFlags(flags);
        item->setHidden(hideDisabledOptions_ && !selectable);
      }
    };
    updateList(hourList, 0);
    updateList(minuteList, 1);
    updateList(secondList, 2);
    updateList(meridiemList, 3);
  };

  setGroupState(selectedHourList_, selectedMinuteList_, selectedSecondList_, selectedMeridiemList_,
                selectedTime_, TimeSelectionPart::Single);
  setGroupState(rangeStartHourList_, rangeStartMinuteList_, rangeStartSecondList_,
                rangeStartMeridiemList_, rangeStartTime_, TimeSelectionPart::Start);
  setGroupState(rangeEndHourList_, rangeEndMinuteList_, rangeEndSecondList_, rangeEndMeridiemList_,
                rangeEndTime_, TimeSelectionPart::End);
  updateTimeColumnRendering();
}

void AdDatePickerPanel::updateTimeColumnRendering() {
  const detail::DatePickerVisualStyle style = resolvedStyle();
  const QList<QListWidget*> timeColumns = {
      selectedHourList_,   selectedMinuteList_,   selectedSecondList_,   selectedMeridiemList_,
      rangeStartHourList_, rangeStartMinuteList_, rangeStartSecondList_, rangeStartMeridiemList_,
      rangeEndHourList_,   rangeEndMinuteList_,   rangeEndSecondList_,   rangeEndMeridiemList_};
  bool hasVisibleColumnBefore = false;
  for (QListWidget* list : timeColumns) {
    if (!list) {
      continue;
    }
    const bool visibleColumn = !list->isHidden();
    applyTimeColumnVisualStyle(list, style, visibleColumn && hasVisibleColumnBefore);
    if (visibleColumn) {
      hasVisibleColumnBefore = true;
    }
    list->update();
    if (QWidget* viewport = list->viewport()) {
      viewport->update();
    }
  }
}

bool AdDatePickerPanel::handleTimeColumnWheel(QObject* watched, QEvent* event) {
  if (!changeOnScroll_ || disabled_ || !watched || !event || event->type() != QEvent::Wheel) {
    return false;
  }

  auto* list = qobject_cast<QListWidget*>(watched);
  if (!list) {
    if (auto* widget = qobject_cast<QWidget*>(watched)) {
      list = qobject_cast<QListWidget*>(widget->parentWidget());
    }
  }
  const QList<QListWidget*> timeColumns = {
      selectedHourList_,   selectedMinuteList_,   selectedSecondList_,   selectedMeridiemList_,
      rangeStartHourList_, rangeStartMinuteList_, rangeStartSecondList_, rangeStartMeridiemList_,
      rangeEndHourList_,   rangeEndMinuteList_,   rangeEndSecondList_,   rangeEndMeridiemList_};
  if (!list || !list->isVisible() || !timeColumns.contains(list)) {
    return false;
  }

  auto* wheelEvent = static_cast<QWheelEvent*>(event);
  int delta = wheelEvent->angleDelta().y();
  if (delta == 0) {
    delta = wheelEvent->pixelDelta().y();
  }
  if (delta == 0) {
    return false;
  }

  const int direction = delta < 0 ? 1 : -1;
  int row = list->currentRow();
  if (row < 0) {
    row = direction > 0 ? -1 : list->count();
  }
  for (int next = row + direction; next >= 0 && next < list->count(); next += direction) {
    const QListWidgetItem* item = list->item(next);
    if (!item || item->isHidden() || !(item->flags() & Qt::ItemIsEnabled)) {
      continue;
    }
    list->setCurrentRow(next, QItemSelectionModel::ClearAndSelect);
    list->scrollToItem(list->item(next), QAbstractItemView::PositionAtTop);
    wheelEvent->accept();
    return true;
  }

  wheelEvent->accept();
  return true;
}

bool AdDatePickerPanel::handleTimeColumnPreview(QObject* watched, QEvent* event) {
  if (!watched || !event ||
      (event->type() != QEvent::MouseMove && event->type() != QEvent::Enter &&
       event->type() != QEvent::Leave)) {
    return false;
  }

  auto* list = qobject_cast<QListWidget*>(watched);
  if (!list) {
    if (auto* widget = qobject_cast<QWidget*>(watched)) {
      list = qobject_cast<QListWidget*>(widget->parentWidget());
    }
  }
  const auto columnInfo = [this](QListWidget* column, TimeSelectionPart* part,
                                 CellSubType* subType) {
    if (!column || !part || !subType) {
      return false;
    }
    if (column == selectedHourList_) {
      *part = TimeSelectionPart::Single;
      *subType = CellSubType::Hour;
    } else if (column == selectedMinuteList_) {
      *part = TimeSelectionPart::Single;
      *subType = CellSubType::Minute;
    } else if (column == selectedSecondList_) {
      *part = TimeSelectionPart::Single;
      *subType = CellSubType::Second;
    } else if (column == selectedMeridiemList_) {
      *part = TimeSelectionPart::Single;
      *subType = CellSubType::Meridiem;
    } else if (column == rangeStartHourList_) {
      *part = TimeSelectionPart::Start;
      *subType = CellSubType::Hour;
    } else if (column == rangeStartMinuteList_) {
      *part = TimeSelectionPart::Start;
      *subType = CellSubType::Minute;
    } else if (column == rangeStartSecondList_) {
      *part = TimeSelectionPart::Start;
      *subType = CellSubType::Second;
    } else if (column == rangeStartMeridiemList_) {
      *part = TimeSelectionPart::Start;
      *subType = CellSubType::Meridiem;
    } else if (column == rangeEndHourList_) {
      *part = TimeSelectionPart::End;
      *subType = CellSubType::Hour;
    } else if (column == rangeEndMinuteList_) {
      *part = TimeSelectionPart::End;
      *subType = CellSubType::Minute;
    } else if (column == rangeEndSecondList_) {
      *part = TimeSelectionPart::End;
      *subType = CellSubType::Second;
    } else if (column == rangeEndMeridiemList_) {
      *part = TimeSelectionPart::End;
      *subType = CellSubType::Meridiem;
    } else {
      return false;
    }
    return true;
  };

  TimeSelectionPart part = TimeSelectionPart::Single;
  CellSubType subType = CellSubType::None;
  if (!list || !columnInfo(list, &part, &subType)) {
    return false;
  }
  if (event->type() == QEvent::Enter) {
    return false;
  }
  if (event->type() == QEvent::Leave || !list->isVisible() || disabled_) {
    emit previewTimeChanged(QTime(), part);
    return false;
  }

  auto* mouseEvent = static_cast<QMouseEvent*>(event);
  const QPoint pos = watched == list ? mouseEventPos(mouseEvent)
                                     : list->viewport()->mapFrom(qobject_cast<QWidget*>(watched),
                                                                 mouseEventPos(mouseEvent));
  QListWidgetItem* item = list->itemAt(pos);
  if (!item || item->isHidden() || !(item->flags() & Qt::ItemIsEnabled)) {
    emit previewTimeChanged(QTime(), part);
    return false;
  }

  const int value = item->data(Qt::UserRole).toInt();
  const QTime current = part == TimeSelectionPart::Start
                            ? rangeStartTime_
                            : (part == TimeSelectionPart::End ? rangeEndTime_ : selectedTime_);
  const QTime normalized = normalizedTimeValue(current);
  QTime preview = normalized;
  switch (subType) {
    case CellSubType::Hour: {
      const int hour = use12Hours_ ? hour24FromDisplayHour(value, normalized.hour() >= 12)
                                   : std::clamp(value, 0, 23);
      preview = QTime(hour, normalized.minute(), normalized.second());
      break;
    }
    case CellSubType::Minute:
      preview = QTime(normalized.hour(), std::clamp(value, 0, 59), normalized.second());
      break;
    case CellSubType::Second:
      preview = QTime(normalized.hour(), normalized.minute(), std::clamp(value, 0, 59));
      break;
    case CellSubType::Meridiem:
      preview = QTime(hour24FromDisplayHour(displayHourFromHour24(normalized.hour()), value == 1),
                      normalized.minute(), normalized.second());
      break;
    case CellSubType::None:
    default:
      break;
  }
  emit previewTimeChanged(preview, part);
  return false;
}

void AdDatePickerPanel::setTimeFromColumnRows(TimeSelectionPart part, int hour, int minute,
                                              int second, bool pm) {
  hour = use12Hours_ ? hour24FromDisplayHour(hour, pm) : std::clamp(hour, 0, 23);
  minute = std::clamp(minute, 0, 59);
  second = std::clamp(second, 0, 59);
  const QTime next(hour, minute, second);
  const QDate date = effectiveDateForTimePart(part);
  const QDate from = part == TimeSelectionPart::Start
                         ? rangeEndDate_
                         : (part == TimeSelectionPart::End ? rangeStartDate_ : QDate());
  if (!isTimeSelectable(date, next, part, from)) {
    syncTimeColumnSelections();
    return;
  }

  switch (part) {
    case TimeSelectionPart::Start:
      setRangeStartTime(next);
      break;
    case TimeSelectionPart::End:
      setRangeEndTime(next);
      break;
    case TimeSelectionPart::Single:
    default:
      setSelectedTime(next);
      if (!effectiveNeedConfirm() && displayMode_ == DisplayMode::Time &&
          selectionMode_ == SelectionMode::Single && date.isValid()) {
        selectedDate_ = normalizeForPicker(pickerMode_, date, firstDayOfWeek_);
        emit accepted(selectedDate_);
        emit dateTimeAccepted(selectedDateTime());
      }
      break;
  }
}

void AdDatePickerPanel::rebuildPresets() {
  if (!presets_.isEmpty()) {
    ensurePresetsUi();
  }
  if (!presetsLayout_) {
    refreshFooter();
    return;
  }

  while (QLayoutItem* item = presetsLayout_->takeAt(0)) {
    if (QWidget* widget = item->widget()) {
      widget->deleteLater();
    }
    delete item;
  }

  const detail::DatePickerVisualStyle style = resolvedStyle();
  for (const PresetItem& preset : presets_) {
    if (preset.label.trimmed().isEmpty()) {
      continue;
    }
    auto* button = createPanelToolButton(presetsListWidget_ ? presetsListWidget_ : presetsWidget_,
                                         preset.label);
    button->setToolTip(preset.label);
    applyPresetButtonStyle(button, style);
    connect(button, &QToolButton::clicked, this, [this, preset]() { applyPreset(preset); });
    presetsLayout_->addWidget(button);
  }
  presetsLayout_->addStretch(1);
  refreshFooter();
}

void AdDatePickerPanel::ensurePresetsUi() {
  if (presetsWidget_ || !panelLayoutWidget_ || !panelLayout_) {
    return;
  }

  presetsWidget_ = new QWidget(panelLayoutWidget_);
  setSemanticSlot(presetsWidget_, "popup.presets", QStringLiteral("addatepicker-panel-presets"));
  presetsWidget_->setAttribute(Qt::WA_StyledBackground, true);
  auto* presetsOuterLayout = new QVBoxLayout(presetsWidget_);
  presetsOuterLayout->setContentsMargins(0, 0, 0, 0);
  presetsOuterLayout->setSpacing(0);
  presetsScrollArea_ = new QScrollArea(presetsWidget_);
  setSemanticSlot(presetsScrollArea_, "popup.presets.scroll",
                  QStringLiteral("addatepicker-panel-presets-scroll"));
  presetsScrollArea_->setFrameShape(QFrame::NoFrame);
  presetsScrollArea_->setWidgetResizable(true);
  presetsScrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  presetsScrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  presetsScrollArea_->setAutoFillBackground(false);
  if (presetsScrollArea_->viewport()) {
    presetsScrollArea_->viewport()->setAutoFillBackground(false);
  }
  presetsListWidget_ = new QWidget(presetsScrollArea_);
  setSemanticSlot(presetsListWidget_, "popup.presets.list",
                  QStringLiteral("addatepicker-panel-presets-list"));
  presetsListWidget_->setAutoFillBackground(false);
  presetsLayout_ = new QVBoxLayout(presetsListWidget_);
  presetsLayout_->setContentsMargins(8, 8, 8, 8);
  presetsLayout_->setSpacing(8);
  presetsScrollArea_->setWidget(presetsListWidget_);
  presetsOuterLayout->addWidget(presetsScrollArea_);
  presetsWidget_->hide();
  panelLayout_->insertWidget(0, presetsWidget_);
  refreshPresetsStyle(resolvedStyle());
}

void AdDatePickerPanel::refreshPresetsStyle(const detail::DatePickerVisualStyle& style) {
  if (presetsWidget_) {
    const int minWidth = style.metrics.presetsWidth;
    const int maxWidth = std::max(minWidth, style.metrics.presetsMaxWidth);
    presetsWidget_->setMinimumWidth(minWidth);
    presetsWidget_->setMaximumWidth(maxWidth);
    presetsWidget_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    presetsWidget_->setStyleSheet(
        QStringLiteral("QWidget#addatepicker-panel-presets { background: %1; "
                       "border-right: %2px solid %3; }")
            .arg(cssColor(style.panelBackground))
            .arg(std::max(1, style.metrics.borderWidth))
            .arg(cssColor(style.panelBorderColor)));
  }
  if (presetsScrollArea_) {
    presetsScrollArea_->setStyleSheet(
        QStringLiteral("QScrollArea#addatepicker-panel-presets-scroll { background: transparent; "
                       "border: none; }"));
    presetsScrollArea_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    if (presetsScrollArea_->viewport()) {
      presetsScrollArea_->viewport()->setStyleSheet(QStringLiteral("background: transparent;"));
    }
  }
  if (presetsListWidget_) {
    presetsListWidget_->setStyleSheet(
        QStringLiteral("QWidget#addatepicker-panel-presets-list { background: transparent; }"));
    const QList<QToolButton*> presetButtons =
        presetsListWidget_->findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly);
    for (QToolButton* button : presetButtons) {
      applyPresetButtonStyle(button, style);
    }
  }
}

void AdDatePickerPanel::syncGridState() {
  refreshPanelBodyVisibility();
  refreshHeader();
  refreshFooter();
  if (grid_) {
    grid_->updateGeometry();
    grid_->invalidateCells();
    grid_->update();
  }
  updateGeometry();
}

void AdDatePickerPanel::syncTimeEditors() {
  if (selectedTimeEdit_) {
    const QSignalBlocker blocker(selectedTimeEdit_);
    selectedTimeEdit_->setTime(selectedTime_);
  }
  if (rangeStartTimeEdit_) {
    const QSignalBlocker blocker(rangeStartTimeEdit_);
    rangeStartTimeEdit_->setTime(rangeStartTime_);
  }
  if (rangeEndTimeEdit_) {
    const QSignalBlocker blocker(rangeEndTimeEdit_);
    rangeEndTimeEdit_->setTime(rangeEndTime_);
  }
  syncTimeColumnSelections();
}

void AdDatePickerPanel::setDisplayMode(DisplayMode value) {
  if (displayMode_ == value) {
    return;
  }
  const PickerMode previousPanelMode = panelMode();
  displayMode_ = value;
  refreshStyle();
  syncGridState();
  if (panelMode() != previousPanelMode) {
    emit panelModeChanged(panelMode());
  }
}

void AdDatePickerPanel::navigate(int months, int years) {
  if (displayMode_ == DisplayMode::Time) {
    return;
  }
  QDate base = viewDate_.isValid() ? viewDate_ : todayDate();
  if (months != 0) {
    base = base.addMonths(months);
  }
  if (years != 0) {
    base = base.addYears(years);
  }
  if (!viewDateCanDisplay(base)) {
    return;
  }
  setViewDate(base);
}

void AdDatePickerPanel::switchHeaderView() {
  DisplayMode nextMode;
  if (displayMode_ == DisplayMode::Time) {
    return;
  } else if (displayMode_ == DisplayMode::Date) {
    nextMode = pickerMode_ == PickerMode::Quarter ? DisplayMode::Quarter : DisplayMode::Month;
  } else if (displayMode_ == DisplayMode::Month || displayMode_ == DisplayMode::Quarter) {
    nextMode = DisplayMode::Year;
  } else if (displayMode_ == DisplayMode::Year) {
    nextMode = DisplayMode::Decade;
  } else {
    nextMode = defaultDisplayModeForPicker(pickerMode_);
  }
  setDisplayMode(nextMode);
}

void AdDatePickerPanel::selectDateFromGrid(const QDate& value) {
  if (!value.isValid() || disabled_) {
    return;
  }
  const QDate normalized = normalizeForPicker(pickerMode_, value, firstDayOfWeek_);
  if (!normalized.isValid()) {
    return;
  }
  const QDate contextFrom = rangeSelectionContextFrom();
  if (!isSelectableForMode(pickerMode_, normalized, contextFrom)) {
    return;
  }

  viewDate_ = normalizedViewDate(normalized);
  if (selectionMode_ == SelectionMode::Single) {
    if (selectedDate_ != normalized) {
      selectedDate_ = normalized;
      emit selectedDateChanged(selectedDate_);
    }
    emit dateSelected(selectedDate_);
    emit dateActivated(selectedDate_);
    if (!effectiveNeedConfirm()) {
      emit accepted(selectedDate_);
    }
  } else if (selectionMode_ == SelectionMode::Multiple) {
    QVector<QDate> next = selectedDates_;
    const qsizetype index = next.indexOf(normalized);
    if (index >= 0) {
      next.removeAt(index);
    } else {
      next.append(normalized);
    }
    next = normalizedDates(next);
    if (selectedDates_ != next) {
      selectedDates_ = next;
      emit selectedDatesChanged(selectedDates_);
    }
    emit dateSelected(normalized);
    emit dateActivated(normalized);
    if (!effectiveNeedConfirm()) {
      emit datesAccepted(selectedDates_);
    }
  } else {
    if (!rangeStartDate_.isValid() || rangeEndDate_.isValid()) {
      rangeStartDate_ = normalized;
      rangeEndDate_ = QDate();
    } else {
      if (order_ && normalized < rangeStartDate_) {
        rangeEndDate_ = rangeStartDate_;
        rangeStartDate_ = normalized;
      } else {
        rangeEndDate_ = normalized;
      }
    }
    const bool selectionCompletedRange = rangeStartDate_.isValid() && rangeEndDate_.isValid();
    emit rangeChanged(rangeStartDate_, rangeEndDate_);
    emit dateActivated(normalized);
    if (selectionCompletedRange && !effectiveNeedConfirm()) {
      emit rangeAccepted(rangeStartDate_, rangeEndDate_);
    }
  }
  syncGridState();
}

void AdDatePickerPanel::applyPreset(const PresetItem& preset) {
  if (disabled_) {
    return;
  }

  const QDate presetValue = resolvedPresetValue(preset);
  const std::pair<QDate, QDate> presetRange = resolvedPresetRange(preset);

  if (selectionMode_ == SelectionMode::Multiple) {
    QDate value = normalizeForPicker(pickerMode_, presetValue, firstDayOfWeek_);
    if (!value.isValid()) {
      value = normalizeForPicker(pickerMode_, presetRange.first, firstDayOfWeek_);
    }
    if (!value.isValid() || !isSelectableForMode(pickerMode_, value)) {
      return;
    }
    QVector<QDate> next = selectedDates_;
    if (!next.contains(value)) {
      next.append(value);
    }
    next = normalizedDates(next);
    if (selectedDates_ != next) {
      selectedDates_ = next;
      emit selectedDatesChanged(selectedDates_);
    }
    emit dateSelected(value);
    if (!effectiveNeedConfirm()) {
      emit datesAccepted(selectedDates_);
    }
    syncGridState();
    return;
  }

  if (selectionMode_ == SelectionMode::Range) {
    QDate start = normalizeForPicker(pickerMode_, presetRange.first, firstDayOfWeek_);
    QDate end = normalizeForPicker(pickerMode_, presetRange.second, firstDayOfWeek_);
    if (!start.isValid() && presetValue.isValid()) {
      start = normalizeForPicker(pickerMode_, presetValue, firstDayOfWeek_);
    }
    if (!end.isValid() && !allowEmptyEnd_) {
      end = start;
    }
    if (!canAcceptRange(start, end)) {
      return;
    }
    if (order_ && start.isValid() && end.isValid() && end < start) {
      std::swap(start, end);
    }
    rangeStartDate_ = start;
    rangeEndDate_ = end;
    viewDate_ = normalizedViewDate(start);
    emit rangeChanged(rangeStartDate_, rangeEndDate_);
    if (!effectiveNeedConfirm()) {
      emit rangeAccepted(rangeStartDate_, rangeEndDate_);
    }
    syncGridState();
    return;
  }

  QDate value = normalizeForPicker(pickerMode_, presetValue, firstDayOfWeek_);
  if (!value.isValid()) {
    value = normalizeForPicker(pickerMode_, presetRange.first, firstDayOfWeek_);
  }
  if (!value.isValid()) {
    return;
  }
  if (!isSelectableForMode(pickerMode_, value)) {
    return;
  }
  selectedDate_ = value;
  viewDate_ = normalizedViewDate(value);
  emit selectedDateChanged(selectedDate_);
  emit dateSelected(selectedDate_);
  if (!effectiveNeedConfirm()) {
    emit accepted(selectedDate_);
  }
  syncGridState();
}

void AdDatePickerPanel::acceptCurrentSelection() {
  if (selectionMode_ == SelectionMode::Single) {
    QDate valueDate = selectedDate_;
    if (displayMode_ == DisplayMode::Time) {
      valueDate = effectiveDateForTimePart(TimeSelectionPart::Single);
    }
    if (valueDate.isValid() && isSelectableForMode(pickerMode_, valueDate) &&
        isTimeSelectable(valueDate, selectedTime_, TimeSelectionPart::Single)) {
      selectedDate_ = normalizeForPicker(pickerMode_, valueDate, firstDayOfWeek_);
      emit accepted(selectedDate_);
      if (effectiveShowTime() || displayMode_ == DisplayMode::Time) {
        emit dateTimeAccepted(selectedDateTime());
      }
    }
    return;
  }
  if (selectionMode_ == SelectionMode::Multiple) {
    QVector<QDate> selectableDates;
    selectableDates.reserve(selectedDates_.size());
    for (const QDate& value : selectedDates_) {
      if (isSelectableForMode(pickerMode_, value)) {
        selectableDates.append(value);
      }
    }
    if (!selectableDates.isEmpty()) {
      emit datesAccepted(selectableDates);
    }
    return;
  }
  if (canAcceptRange(rangeStartDate_, rangeEndDate_)) {
    const bool startTimeOk =
        !rangeStartDate_.isValid() ||
        isTimeSelectable(rangeStartDate_, rangeStartTime_, TimeSelectionPart::Start, rangeEndDate_);
    const bool endTimeOk =
        !rangeEndDate_.isValid() ||
        isTimeSelectable(rangeEndDate_, rangeEndTime_, TimeSelectionPart::End, rangeStartDate_);
    if (startTimeOk && endTimeOk) {
      emit rangeAccepted(rangeStartDate_, rangeEndDate_);
      if (effectiveShowTime()) {
        emit rangeDateTimeAccepted(rangeStartDateTime(), rangeEndDateTime());
      }
    }
  }
}

QDate AdDatePickerPanel::effectiveDateForTimePart(TimeSelectionPart part) const {
  switch (part) {
    case TimeSelectionPart::Start:
      if (rangeStartDate_.isValid()) {
        return rangeStartDate_;
      }
      return viewDate_.isValid() ? viewDate_ : todayDate();
    case TimeSelectionPart::End:
      if (rangeEndDate_.isValid()) {
        return rangeEndDate_;
      }
      if (rangeStartDate_.isValid()) {
        return rangeStartDate_;
      }
      return viewDate_.isValid() ? viewDate_ : todayDate();
    case TimeSelectionPart::Single:
    default:
      if (selectedDate_.isValid()) {
        return selectedDate_;
      }
      return viewDate_.isValid() ? viewDate_ : todayDate();
  }
}

bool AdDatePickerPanel::canAcceptRange(const QDate& start, const QDate& end) const {
  if (!rangeEndpointsAcceptable(start, end, allowEmptyStart_, allowEmptyEnd_)) {
    return false;
  }
  if (start.isValid() && !isSelectableForMode(pickerMode_, start, end)) {
    return false;
  }
  if (end.isValid() && !isSelectableForMode(pickerMode_, end, start)) {
    return false;
  }
  return true;
}

QVector<QDate> AdDatePickerPanel::normalizedDates(const QVector<QDate>& values) const {
  return normalizedDateVector(pickerMode_, values, firstDayOfWeek_, order_);
}

void AdDatePickerPanel::syncSelectedDateKeys() {
  selectedDateKeys_.clear();
  selectedDateKeys_.reserve(selectedDates_.size());
  for (const QDate& date : selectedDates_) {
    if (date.isValid()) {
      selectedDateKeys_.insert(date.toJulianDay());
    }
  }
}

QDate AdDatePickerPanel::normalizedViewDate(const QDate& value) const {
  const QDate base = value.isValid() ? value : todayDate();
  QDate candidate(base.year(), base.month(), 1);
  if (viewDateCanDisplay(candidate)) {
    return candidate;
  }

  QDate lower;
  QDate upper;
  normalizedDateBounds(minDate_, maxDate_, &lower, &upper);
  if (lower.isValid() && viewRangeEnd(candidate) < lower) {
    candidate = QDate(lower.year(), lower.month(), 1);
  }
  if (upper.isValid() && viewRangeStart(candidate) > upper) {
    candidate = QDate(upper.year(), upper.month(), 1);
  }
  return candidate;
}

QDate AdDatePickerPanel::viewRangeStart(const QDate& value) const {
  const QDate base = value.isValid() ? value : todayDate();
  switch (displayMode_) {
    case DisplayMode::Time:
      return base;
    case DisplayMode::Month:
    case DisplayMode::Quarter:
      return QDate(base.year(), 1, 1);
    case DisplayMode::Year:
      return QDate(decadeStartForYear(base.year()), 1, 1);
    case DisplayMode::Decade:
      return QDate(centuryStartForYear(base.year()), 1, 1);
    case DisplayMode::Date:
    default:
      return QDate(base.year(), base.month(), 1);
  }
}

QDate AdDatePickerPanel::viewRangeEnd(const QDate& value) const {
  const QDate start = viewRangeStart(value);
  if (!start.isValid()) {
    return {};
  }
  switch (displayMode_) {
    case DisplayMode::Time:
      return start;
    case DisplayMode::Month:
    case DisplayMode::Quarter:
      return QDate(start.year(), 12, 31);
    case DisplayMode::Year:
      return QDate(start.year() + 9, 12, 31);
    case DisplayMode::Decade:
      return QDate(start.year() + 99, 12, 31);
    case DisplayMode::Date:
    default:
      return endOfMonth(start);
  }
}

bool AdDatePickerPanel::viewDateCanDisplay(const QDate& value) const {
  const QDate start = viewRangeStart(value);
  const QDate end = viewRangeEnd(value);
  if (!start.isValid() || !end.isValid()) {
    return false;
  }

  QDate lower;
  QDate upper;
  normalizedDateBounds(minDate_, maxDate_, &lower, &upper);
  if (lower.isValid() && end < lower) {
    return false;
  }
  if (upper.isValid() && start > upper) {
    return false;
  }
  return true;
}

bool AdDatePickerPanel::isSelectableForMode(PickerMode mode, const QDate& value,
                                            const QDate& from) const {
  const QDate normalized = normalizeForPicker(mode, value, firstDayOfWeek_);
  if (!normalized.isValid()) {
    return false;
  }
  if (!pickerValueWithinBounds(mode, normalized, firstDayOfWeek_, minDate_, maxDate_)) {
    return false;
  }
  if (disabledDatePredicate_ && disabledDatePredicate_(normalized)) {
    return false;
  }
  if (disabledDateContextPredicate_) {
    DisabledDateContext context;
    context.from = normalizeForPicker(mode, from, firstDayOfWeek_);
    context.type = mode;
    if (disabledDateContextPredicate_(normalized, context)) {
      return false;
    }
  }
  return true;
}

QDate AdDatePickerPanel::rangeSelectionContextFrom() const {
  return selectionMode_ == SelectionMode::Range && rangeStartDate_.isValid() &&
                 !rangeEndDate_.isValid()
             ? rangeStartDate_
             : QDate();
}

AdDatePickerPanel::DisplayMode AdDatePickerPanel::defaultDisplayModeForPicker(
    PickerMode value) const {
  switch (value) {
    case PickerMode::Month:
      return DisplayMode::Month;
    case PickerMode::Quarter:
      return DisplayMode::Quarter;
    case PickerMode::Year:
      return DisplayMode::Year;
    case PickerMode::Decade:
      return DisplayMode::Decade;
    case PickerMode::Time:
      return DisplayMode::Time;
    case PickerMode::Date:
    case PickerMode::Week:
    default:
      return DisplayMode::Date;
  }
}

AdDatePickerPanel::DisplayMode AdDatePickerPanel::displayModeForPanelMode(PickerMode value) const {
  switch (value) {
    case PickerMode::Month:
      return DisplayMode::Month;
    case PickerMode::Quarter:
      return DisplayMode::Quarter;
    case PickerMode::Year:
      return DisplayMode::Year;
    case PickerMode::Decade:
      return DisplayMode::Decade;
    case PickerMode::Time:
      return DisplayMode::Time;
    case PickerMode::Date:
    case PickerMode::Week:
    default:
      return DisplayMode::Date;
  }
}

AdDatePickerPanel::PickerMode AdDatePickerPanel::panelModeForDisplayMode(DisplayMode value) const {
  switch (value) {
    case DisplayMode::Month:
      return PickerMode::Month;
    case DisplayMode::Quarter:
      return PickerMode::Quarter;
    case DisplayMode::Year:
      return PickerMode::Year;
    case DisplayMode::Decade:
      return PickerMode::Decade;
    case DisplayMode::Time:
      return PickerMode::Time;
    case DisplayMode::Date:
    default:
      return PickerMode::Date;
  }
}

bool AdDatePickerPanel::effectiveShowTime() const {
  return showTime_ && pickerMode_ == PickerMode::Date && selectionMode_ != SelectionMode::Multiple;
}

bool AdDatePickerPanel::timeControlsVisible() const {
  return (effectiveShowTime() || displayMode_ == DisplayMode::Time) &&
         selectionMode_ != SelectionMode::Multiple;
}

int AdDatePickerPanel::timePanelColumnCount() const {
  if (!timeControlsVisible()) {
    return 0;
  }
  int count = 0;
  if (effectiveShowHourColumn()) {
    ++count;
  }
  if (effectiveShowMinuteColumn()) {
    ++count;
  }
  if (effectiveShowSecondColumn()) {
    ++count;
  }
  if (use12Hours_ && effectiveShowHourColumn()) {
    ++count;
  }
  return count;
}

void AdDatePickerPanel::refreshTimePanelGeometry() {
  if (!timeWidget_) {
    return;
  }
  const detail::DatePickerVisualStyle style = resolvedStyle();
  const bool timePanel = displayMode_ == DisplayMode::Time;
  const bool timeVisible = timeControlsVisible();
  const int timePanelBodyWidth =
      std::max(1, timePanelColumnCount()) * style.metrics.timeColumnWidth;
  const int visibleTimePanelWidth = timeVisible ? timePanelBodyWidth : 0;
  if (header_) {
    const int headerWidth = timePanel
                                ? (timeVisible ? visibleTimePanelWidth : style.metrics.panelWidth)
                                : style.metrics.panelWidth + visibleTimePanelWidth;
    header_->setFixedWidth(headerWidth);
  }
  if (timeHeader_) {
    timeHeader_->setFixedWidth(visibleTimePanelWidth);
  }
  const int topPadding = timePanel ? style.metrics.timePanelPaddingTop : 0;
  if (timeLayout_) {
    timeLayout_->setContentsMargins(0, style.metrics.timeColumnMarginVertical + topPadding, 0,
                                    style.metrics.timeColumnMarginVertical);
  }
  int timeHostHeight = style.metrics.timeColumnHeight + topPadding;
  if (!timePanel && grid_) {
    timeHostHeight = std::max(timeHostHeight, grid_->sizeHint().height());
  }
  timeWidget_->setFixedHeight(timeHostHeight);
  timeWidget_->setFixedWidth(timePanelBodyWidth);
}

bool AdDatePickerPanel::effectiveNeedConfirm() const { return needConfirm_ || effectiveShowTime(); }

QString AdDatePickerPanel::effectiveTimeFormat() const {
  return normalizedTimeFormat(timeFormat_, use12Hours_);
}

bool AdDatePickerPanel::effectiveShowHourColumn() const { return showHour_; }

bool AdDatePickerPanel::effectiveShowMinuteColumn() const { return showMinute_; }

bool AdDatePickerPanel::effectiveShowSecondColumn() const {
  return showSecondExplicit_ ? showSecond_ : formatHasSecondToken(effectiveTimeFormat());
}

bool AdDatePickerPanel::isTimeSelectable(const QDate& date, const QTime& time,
                                         TimeSelectionPart part, const QDate& from) const {
  if (!timeControlsVisible() || !disabledTimePredicate_) {
    return true;
  }
  const QDate normalizedDate = normalizeForPicker(pickerMode_, date, firstDayOfWeek_);
  if (!normalizedDate.isValid()) {
    return true;
  }
  DisabledTimeContext context;
  context.from = normalizeForPicker(pickerMode_, from, firstDayOfWeek_);
  context.part = part;
  return !disabledTimePredicate_(normalizedDate, normalizedTimeValue(time), context);
}

AdDatePicker::AdDatePicker(QWidget* parent) : QWidget(parent) {
  popupController_ = new detail::OverlayPopupController(this, this);
  popupController_->setVisibilityMode(detail::OverlayPopupController::VisibilityMode::External);
  popupController_->setTriggerModes(detail::OverlayPopupController::Triggers{});
  connect(popupController_, &detail::OverlayPopupController::popupVisibleChanged, this,
          &AdDatePicker::handleControllerPopupVisibleChanged);
  connect(popupController_, &detail::OverlayPopupController::popupVisibilityRequested, this,
          [this](bool value) {
            if (!value) {
              setPopupVisibleInternal(false, true);
            }
          });
  buildUi();
  popupController_->anchorWidgetChanged();
}

AdDatePicker::~AdDatePicker() {
  if (popupController_) {
    popupController_->setPopupVisible(false);
  }
  destroyPopup();
}

QDate AdDatePicker::date() const { return date_; }

void AdDatePicker::setDate(const QDate& value) {
  if (multiple_) {
    QVector<QDate> values;
    if (value.isValid()) {
      values.append(value);
    }
    setSelectedDates(values);
    return;
  }

  const QDate normalized = normalizeForPicker(pickerMode_, value, effectiveFirstDayOfWeek());
  if (date_ == normalized) {
    return;
  }
  date_ = normalized;
  syncLineEdit();
  if (popupVisible_) {
    syncPanelState();
  }
  emit dateChanged(date_);
  emit dateTimeChanged(dateTime());
}

QDateTime AdDatePicker::dateTime() const { return dateTimeFromParts(date_, time_); }

void AdDatePicker::setDateTime(const QDateTime& value) {
  if (multiple_) {
    if (value.isValid()) {
      setSelectedDates({value.date()});
      setTime(value.time());
    } else {
      setSelectedDates({});
      setTime(defaultTimeValue());
    }
    return;
  }

  if (!value.isValid()) {
    const bool hadDate = date_.isValid();
    const bool hadTime = time_ != defaultTimeValue();
    date_ = QDate();
    time_ = defaultTimeValue();
    syncLineEdit();
    if (popupVisible_) {
      syncPanelState();
    }
    if (hadDate) {
      emit dateChanged(date_);
    }
    if (hadTime) {
      emit timeChanged(time_);
    }
    if (hadDate || hadTime) {
      emit dateTimeChanged(dateTime());
    }
    return;
  }

  const QDate nextDate = normalizeForPicker(pickerMode_, value.date(), effectiveFirstDayOfWeek());
  const QTime nextTime = normalizedTimeValue(value.time());
  const bool dateChangedValue = date_ != nextDate;
  const bool timeChangedValue = time_ != nextTime;
  if (!dateChangedValue && !timeChangedValue) {
    return;
  }

  date_ = nextDate;
  time_ = nextTime;
  syncLineEdit();
  if (popupVisible_) {
    syncPanelState();
  }
  if (dateChangedValue) {
    emit dateChanged(date_);
  }
  if (timeChangedValue) {
    emit timeChanged(time_);
  }
  emit dateTimeChanged(dateTime());
}

QTime AdDatePicker::time() const { return time_; }

void AdDatePicker::setTime(const QTime& value) {
  const QTime normalized = normalizedTimeValue(value);
  const bool materializeTimeValue = pickerMode_ == PickerMode::Time && !date_.isValid();
  if (time_ == normalized && !materializeTimeValue) {
    return;
  }
  const bool dateChangedValue = materializeTimeValue;
  time_ = normalized;
  if (materializeTimeValue) {
    date_ = todayDate();
  }
  syncLineEdit();
  if (popupVisible_) {
    syncPanelState();
  }
  if (dateChangedValue) {
    emit dateChanged(date_);
  }
  emit timeChanged(time_);
  if (date_.isValid()) {
    emit dateTimeChanged(dateTime());
  }
}

QVector<QDate> AdDatePicker::selectedDates() const { return selectedDates_; }

void AdDatePicker::setSelectedDates(const QVector<QDate>& values) {
  const QVector<QDate> normalized = normalizedDates(values);
  const QDate nextDate = normalized.isEmpty() ? QDate() : normalized.constFirst();
  const bool selectedChanged = selectedDates_ != normalized;
  const bool dateChangedValue = date_ != nextDate;
  if (!selectedChanged && !dateChangedValue) {
    return;
  }

  selectedDates_ = normalized;
  date_ = nextDate;
  syncLineEdit();
  if (popupVisible_) {
    syncPanelState();
  }
  if (selectedChanged) {
    emit selectedDatesChanged(selectedDates_);
  }
  if (dateChangedValue) {
    emit dateChanged(date_);
    emit dateTimeChanged(dateTime());
  }
}

bool AdDatePicker::multiple() const { return multiple_; }

void AdDatePicker::setMultiple(bool value) {
  if (multiple_ == value) {
    return;
  }

  if (value && showTime_) {
    setShowTime(false);
  }
  if (value && pickerMode_ == PickerMode::Time) {
    setPickerMode(PickerMode::Date);
  }

  const QVector<QDate> previousSelectedDates = selectedDates_;
  const QDate previousDate = date_;
  multiple_ = value;
  if (multiple_) {
    if (selectedDates_.isEmpty() && date_.isValid()) {
      selectedDates_ = normalizedDates({date_});
    } else {
      selectedDates_ = normalizedDates(selectedDates_);
    }
    date_ = selectedDates_.isEmpty() ? QDate() : selectedDates_.constFirst();
  } else if (!selectedDates_.isEmpty()) {
    date_ = selectedDates_.constFirst();
  }

  syncLineEditMask();
  syncLineEdit();
  syncPanelState();
  emit multipleChanged(multiple_);
  if (selectedDates_ != previousSelectedDates) {
    emit selectedDatesChanged(selectedDates_);
  }
  if (date_ != previousDate) {
    emit dateChanged(date_);
    emit dateTimeChanged(dateTime());
  }
}

bool AdDatePicker::order() const { return order_; }

void AdDatePicker::setOrder(bool value) {
  if (order_ == value) {
    return;
  }
  const QVector<QDate> previousSelectedDates = selectedDates_;
  const QDate previousDate = date_;
  order_ = value;
  selectedDates_ = normalizedDates(selectedDates_);
  date_ = selectedDates_.isEmpty() ? date_ : selectedDates_.constFirst();
  syncLineEdit();
  syncPanelState();
  emit orderChanged(order_);
  if (selectedDates_ != previousSelectedDates) {
    emit selectedDatesChanged(selectedDates_);
  }
  if (date_ != previousDate) {
    emit dateChanged(date_);
    emit dateTimeChanged(dateTime());
  }
}

int AdDatePicker::maxTagCount() const { return maxTagCount_; }

void AdDatePicker::setMaxTagCount(int value) {
  const int normalized = value < 0 ? -1 : value;
  if (maxTagCount_ == normalized) {
    return;
  }
  maxTagCount_ = normalized;
  syncLineEdit();
  emit maxTagCountChanged(maxTagCount_);
}

bool AdDatePicker::responsiveMaxTagCount() const { return responsiveMaxTagCount_; }

void AdDatePicker::setResponsiveMaxTagCount(bool value) {
  if (responsiveMaxTagCount_ == value) {
    return;
  }
  responsiveMaxTagCount_ = value;
  syncLineEdit();
  emit responsiveMaxTagCountChanged(responsiveMaxTagCount_);
}

AdDatePicker::PickerMode AdDatePicker::pickerMode() const { return pickerMode_; }

void AdDatePicker::setPickerMode(PickerMode value) {
  if (value == PickerMode::Time && multiple_) {
    setMultiple(false);
  }
  if (pickerMode_ == value) {
    return;
  }
  const QDate previousDate = date_;
  const QVector<QDate> previousSelectedDates = selectedDates_;
  const bool previousShowWeek = effectiveShowWeek();
  const PickerMode previousPanelMode = effectivePanelMode();
  pickerMode_ = value;
  date_ = normalizeForPicker(pickerMode_, date_, effectiveFirstDayOfWeek());
  selectedDates_ = normalizedDates(selectedDates_);
  if (multiple_) {
    date_ = selectedDates_.isEmpty() ? QDate() : selectedDates_.constFirst();
  }
  syncLineEditMask();
  syncLineEditStyle();
  syncLineEdit();
  syncPanelState();
  emit pickerModeChanged(pickerMode_);
  if (effectivePanelMode() != previousPanelMode) {
    emit panelModeChanged(effectivePanelMode());
  }
  if (effectiveShowWeek() != previousShowWeek) {
    emit showWeekChanged(effectiveShowWeek());
  }
  if (selectedDates_ != previousSelectedDates) {
    emit selectedDatesChanged(selectedDates_);
  }
  if (date_ != previousDate) {
    emit dateChanged(date_);
    emit dateTimeChanged(dateTime());
  }
}

AdDatePicker::Size AdDatePicker::size() const { return size_; }

void AdDatePicker::setSize(Size value) {
  if (size_ == value) {
    return;
  }
  size_ = value;
  syncLineEditStyle();
  emit sizeChanged(size_);
}

AdDatePicker::Variant AdDatePicker::variant() const { return variant_; }

void AdDatePicker::setVariant(Variant value) {
  if (variant_ == value) {
    return;
  }
  variant_ = value;
  syncLineEditStyle();
  emit variantChanged(variant_);
}

AdDatePicker::Status AdDatePicker::status() const { return status_; }

void AdDatePicker::setStatus(Status value) {
  if (status_ == value) {
    return;
  }
  status_ = value;
  syncLineEditStyle();
  emit statusChanged(status_);
}

bool AdDatePicker::allowClear() const { return allowClear_; }

void AdDatePicker::setAllowClear(bool value) {
  if (allowClear_ == value) {
    return;
  }
  allowClear_ = value;
  if (lineEdit_) {
    lineEdit_->setAllowClear(allowClear_);
  }
  emit allowClearChanged(allowClear_);
}

bool AdDatePicker::inputReadOnly() const { return inputReadOnly_; }

void AdDatePicker::setInputReadOnly(bool value) {
  if (inputReadOnly_ == value) {
    return;
  }
  inputReadOnly_ = value;
  if (lineEdit_) {
    lineEdit_->setReadOnly(inputReadOnly_);
  }
  emit inputReadOnlyChanged(inputReadOnly_);
}

QString AdDatePicker::id() const { return id_; }

void AdDatePicker::setId(const QString& value) {
  const QString nextId = normalizedInputId(value);
  if (id_ == nextId) {
    return;
  }
  id_ = nextId;
  syncInputIds();
  emit idChanged(id_);
}

AdDatePicker::PreviewValue AdDatePicker::previewValue() const { return previewValue_; }

void AdDatePicker::setPreviewValue(PreviewValue value) {
  if (previewValue_ == value) {
    return;
  }
  previewValue_ = value;
  if (previewValue_ == PreviewValue::Disabled) {
    clearPreviewText();
  }
  emit previewValueChanged(previewValue_);
}

bool AdDatePicker::popupVisible() const { return popupVisible_; }

void AdDatePicker::setPopupVisible(bool value) { setPopupVisibleInternal(value, true); }

void AdDatePicker::showPopup() { setPopupVisible(true); }

void AdDatePicker::hidePopup() { setPopupVisible(false); }

bool AdDatePicker::defaultOpen() const { return defaultOpen_; }

void AdDatePicker::setDefaultOpen(bool value) {
  if (defaultOpen_ == value) {
    return;
  }
  defaultOpen_ = value;
  defaultOpenApplied_ = false;
  if (defaultOpen_ && isVisible()) {
    QTimer::singleShot(0, this, [this]() {
      if (defaultOpen_ && !defaultOpenApplied_ && isVisible()) {
        defaultOpenApplied_ = true;
        showPopup();
      }
    });
  }
  emit defaultOpenChanged(defaultOpen_);
}

QDate AdDatePicker::defaultPickerValue() const { return defaultPickerValue_; }

void AdDatePicker::setDefaultPickerValue(const QDate& value) {
  if (defaultPickerValue_ == value) {
    return;
  }
  defaultPickerValue_ = value;
  if (!popupVisible_ && !pickerValue_.isValid()) {
    syncPanelState();
  }
  emit defaultPickerValueChanged(defaultPickerValue_);
}

QDate AdDatePicker::pickerValue() const { return pickerValue_; }

void AdDatePicker::setPickerValue(const QDate& value) {
  if (pickerValue_ == value) {
    return;
  }
  pickerValue_ = value;
  syncPanelState();
  emit pickerValueChanged(pickerValue_);
}

AdDatePicker::PickerMode AdDatePicker::panelMode() const { return effectivePanelMode(); }

void AdDatePicker::setPanelMode(PickerMode value) {
  const PickerMode normalized = normalizedPanelMode(value);
  if (panelModeExplicit_ && panelMode_ == normalized) {
    return;
  }
  panelMode_ = normalized;
  panelModeExplicit_ = true;
  syncPanelState();
  emit panelModeChanged(panelMode_);
}

bool AdDatePicker::showToday() const { return showToday_; }

void AdDatePicker::setShowToday(bool value) {
  if (showToday_ == value) {
    return;
  }
  showToday_ = value;
  syncPanelState();
  emit showTodayChanged(showToday_);
}

bool AdDatePicker::showWeek() const { return effectiveShowWeek(); }

void AdDatePicker::setShowWeek(bool value) {
  const bool previous = effectiveShowWeek();
  showWeek_ = value;
  showWeekExplicit_ = true;
  if (effectiveShowWeek() == previous) {
    return;
  }
  syncPanelState();
  emit showWeekChanged(effectiveShowWeek());
}

bool AdDatePicker::needConfirm() const { return needConfirm_; }

void AdDatePicker::setNeedConfirm(bool value) {
  if (needConfirm_ == value) {
    return;
  }
  needConfirm_ = value;
  syncPanelState();
  emit needConfirmChanged(needConfirm_);
}

bool AdDatePicker::showTime() const { return showTime_; }

void AdDatePicker::setShowTime(bool value) {
  if (showTime_ == value) {
    return;
  }
  if (value && multiple_) {
    setMultiple(false);
  }
  showTime_ = value;
  syncLineEditMask();
  syncLineEdit();
  syncPanelState();
  syncPopupGeometry();
  emit showTimeChanged(showTime_);
}

bool AdDatePicker::showNow() const { return showNow_; }

void AdDatePicker::setShowNow(bool value) {
  if (showNow_ == value) {
    return;
  }
  showNow_ = value;
  syncPanelState();
  syncPopupGeometry();
  emit showNowChanged(showNow_);
}

QTime AdDatePicker::defaultOpenTime() const { return defaultOpenTime_; }

void AdDatePicker::setDefaultOpenTime(const QTime& value) {
  const QTime normalized = normalizedTimeValue(value);
  if (defaultOpenTime_ == normalized) {
    return;
  }
  defaultOpenTime_ = normalized;
  syncPanelState();
  emit defaultOpenTimeChanged(defaultOpenTime_);
}

QString AdDatePicker::timeFormat() const { return effectiveTimeFormat(); }

void AdDatePicker::setTimeFormat(const QString& value) {
  const QString normalized = normalizeDateFormatSyntax(value.trimmed());
  if (timeFormat_ == normalized) {
    return;
  }
  const bool previousShowSecond = effectiveShowSecondColumn();
  timeFormat_ = normalized;
  syncLineEditMask();
  syncLineEdit();
  syncPanelState();
  syncPopupGeometry();
  emit timeFormatChanged(effectiveTimeFormat());
  if (previousShowSecond != effectiveShowSecondColumn()) {
    emit showSecondChanged(effectiveShowSecondColumn());
  }
}

int AdDatePicker::hourStep() const { return hourStep_; }

void AdDatePicker::setHourStep(int value) {
  const int normalized = normalizedTimeStep(value, 24);
  if (hourStep_ == normalized) {
    return;
  }
  hourStep_ = normalized;
  syncPanelState();
  syncPopupGeometry();
  emit timeStepChanged(hourStep_, minuteStep_, secondStep_);
}

int AdDatePicker::minuteStep() const { return minuteStep_; }

void AdDatePicker::setMinuteStep(int value) {
  const int normalized = normalizedTimeStep(value, 60);
  if (minuteStep_ == normalized) {
    return;
  }
  minuteStep_ = normalized;
  syncPanelState();
  syncPopupGeometry();
  emit timeStepChanged(hourStep_, minuteStep_, secondStep_);
}

int AdDatePicker::secondStep() const { return secondStep_; }

void AdDatePicker::setSecondStep(int value) {
  const int normalized = normalizedTimeStep(value, 60);
  if (secondStep_ == normalized) {
    return;
  }
  secondStep_ = normalized;
  syncPanelState();
  syncPopupGeometry();
  emit timeStepChanged(hourStep_, minuteStep_, secondStep_);
}

void AdDatePicker::setTimeSteps(int hourStep, int minuteStep, int secondStep) {
  const int nextHourStep = normalizedTimeStep(hourStep, 24);
  const int nextMinuteStep = normalizedTimeStep(minuteStep, 60);
  const int nextSecondStep = normalizedTimeStep(secondStep, 60);
  if (hourStep_ == nextHourStep && minuteStep_ == nextMinuteStep && secondStep_ == nextSecondStep) {
    return;
  }
  hourStep_ = nextHourStep;
  minuteStep_ = nextMinuteStep;
  secondStep_ = nextSecondStep;
  syncPanelState();
  syncPopupGeometry();
  emit timeStepChanged(hourStep_, minuteStep_, secondStep_);
}

bool AdDatePicker::hideDisabledOptions() const { return hideDisabledOptions_; }

void AdDatePicker::setHideDisabledOptions(bool value) {
  if (hideDisabledOptions_ == value) {
    return;
  }
  hideDisabledOptions_ = value;
  syncPanelState();
  syncPopupGeometry();
  emit hideDisabledOptionsChanged(hideDisabledOptions_);
}

bool AdDatePicker::use12Hours() const { return use12Hours_; }

void AdDatePicker::setUse12Hours(bool value) {
  if (use12Hours_ == value) {
    return;
  }
  use12Hours_ = value;
  syncLineEditMask();
  syncLineEdit();
  syncPanelState();
  syncPopupGeometry();
  emit use12HoursChanged(use12Hours_);
}

bool AdDatePicker::changeOnScroll() const { return changeOnScroll_; }

void AdDatePicker::setChangeOnScroll(bool value) {
  if (changeOnScroll_ == value) {
    return;
  }
  changeOnScroll_ = value;
  syncPanelState();
  emit changeOnScrollChanged(changeOnScroll_);
}

bool AdDatePicker::showHour() const { return showHour_; }

void AdDatePicker::setShowHour(bool value) {
  if (showHour_ == value) {
    return;
  }
  showHour_ = value;
  syncPanelState();
  syncPopupGeometry();
  emit showHourChanged(showHour_);
}

bool AdDatePicker::showMinute() const { return showMinute_; }

void AdDatePicker::setShowMinute(bool value) {
  if (showMinute_ == value) {
    return;
  }
  showMinute_ = value;
  syncPanelState();
  syncPopupGeometry();
  emit showMinuteChanged(showMinute_);
}

bool AdDatePicker::showSecond() const { return effectiveShowSecondColumn(); }

void AdDatePicker::setShowSecond(bool value) {
  const bool previous = effectiveShowSecondColumn();
  if (showSecondExplicit_ && showSecond_ == value) {
    return;
  }
  showSecond_ = value;
  showSecondExplicit_ = true;
  syncPanelState();
  syncPopupGeometry();
  if (previous != effectiveShowSecondColumn()) {
    emit showSecondChanged(effectiveShowSecondColumn());
  }
}

void AdDatePicker::resetShowSecond() {
  if (!showSecondExplicit_) {
    return;
  }
  const bool previous = effectiveShowSecondColumn();
  showSecond_ = true;
  showSecondExplicit_ = false;
  syncPanelState();
  syncPopupGeometry();
  if (previous != effectiveShowSecondColumn()) {
    emit showSecondChanged(effectiveShowSecondColumn());
  }
}

void AdDatePicker::setVisibleTimeColumns(bool hour, bool minute, bool second) {
  const bool previousHour = showHour_;
  const bool previousMinute = showMinute_;
  const bool previousSecond = effectiveShowSecondColumn();
  if (showHour_ == hour && showMinute_ == minute && showSecondExplicit_ && showSecond_ == second) {
    return;
  }
  showHour_ = hour;
  showMinute_ = minute;
  showSecond_ = second;
  showSecondExplicit_ = true;
  syncPanelState();
  syncPopupGeometry();
  if (previousHour != showHour_) {
    emit showHourChanged(showHour_);
  }
  if (previousMinute != showMinute_) {
    emit showMinuteChanged(showMinute_);
  }
  if (previousSecond != effectiveShowSecondColumn()) {
    emit showSecondChanged(effectiveShowSecondColumn());
  }
}

bool AdDatePicker::disabled() const { return !isEnabled(); }

void AdDatePicker::setDisabled(bool value) {
  if (disabled() == value) {
    return;
  }
  if (value && popupVisible_) {
    setPopupVisibleInternal(false, true);
  }
  QWidget::setDisabled(value);
  if (lineEdit_) {
    lineEdit_->setDisabled(value);
  }
  syncPanelState();
  emit disabledChanged(value);
}

QDate AdDatePicker::minDate() const { return minDate_; }

void AdDatePicker::setMinDate(const QDate& value) {
  if (minDate_ == value) {
    return;
  }
  minDate_ = value;
  syncPanelState();
  emit minDateChanged(minDate_);
}

QDate AdDatePicker::maxDate() const { return maxDate_; }

void AdDatePicker::setMaxDate(const QDate& value) {
  if (maxDate_ == value) {
    return;
  }
  maxDate_ = value;
  syncPanelState();
  emit maxDateChanged(maxDate_);
}

QString AdDatePicker::displayFormat() const {
  return displayFormats_.isEmpty() ? displayFormat_ : displayFormats_.first();
}

void AdDatePicker::setDisplayFormat(const QString& value) {
  const QString normalized = normalizeDateFormatSyntax(value.trimmed());
  if (displayFormat_ == normalized && displayFormats_.isEmpty()) {
    return;
  }
  const QString previousDisplay = displayFormat();
  const QStringList previousFormats = displayFormats_;
  const bool previousMaskFormat = maskFormat_;
  const QString previousTimeFormat = effectiveTimeFormat();
  const bool previousShowSecond = effectiveShowSecondColumn();
  displayFormat_ = normalized;
  displayFormats_.clear();
  if (maskFormat_ && displayFormat_.isEmpty()) {
    maskFormat_ = false;
  }
  syncLineEditMask();
  syncLineEdit();
  syncPanelState();
  syncPopupGeometry();
  if (displayFormat() != previousDisplay) {
    emit displayFormatChanged(displayFormat());
  }
  if (displayFormats_ != previousFormats) {
    emit displayFormatsChanged(displayFormats_);
  }
  if (maskFormat_ != previousMaskFormat) {
    emit maskFormatChanged(maskFormat_);
  }
  if (previousTimeFormat != effectiveTimeFormat()) {
    emit timeFormatChanged(effectiveTimeFormat());
  }
  if (previousShowSecond != effectiveShowSecondColumn()) {
    emit showSecondChanged(effectiveShowSecondColumn());
  }
}

QStringList AdDatePicker::displayFormats() const { return displayFormats_; }

void AdDatePicker::setDisplayFormats(const QStringList& values) {
  const QStringList normalized = normalizeDateFormatSyntax(normalizedFormats(values));
  if (displayFormats_ == normalized) {
    return;
  }

  const QString previousDisplay = displayFormat();
  const bool previousMaskFormat = maskFormat_;
  const QString previousTimeFormat = effectiveTimeFormat();
  const bool previousShowSecond = effectiveShowSecondColumn();
  displayFormats_ = normalized;
  if (maskFormat_ && displayFormats_.isEmpty()) {
    maskFormat_ = false;
  }
  syncLineEditMask();
  syncLineEdit();
  syncPanelState();
  syncPopupGeometry();
  emit displayFormatsChanged(displayFormats_);
  if (displayFormat() != previousDisplay) {
    emit displayFormatChanged(displayFormat());
  }
  if (maskFormat_ != previousMaskFormat) {
    emit maskFormatChanged(maskFormat_);
  }
  if (previousTimeFormat != effectiveTimeFormat()) {
    emit timeFormatChanged(effectiveTimeFormat());
  }
  if (previousShowSecond != effectiveShowSecondColumn()) {
    emit showSecondChanged(effectiveShowSecondColumn());
  }
}

bool AdDatePicker::maskFormat() const { return maskFormat_; }

void AdDatePicker::setMaskFormat(bool value) {
  if (maskFormat_ == value) {
    return;
  }
  maskFormat_ = value;
  if (maskFormat_ && displayFormat().trimmed().isEmpty()) {
    displayFormat_ = defaultDisplayFormat();
  }
  syncLineEditMask();
  syncLineEdit();
  emit maskFormatChanged(maskFormat_);
}

bool AdDatePicker::preserveInvalidOnBlur() const { return preserveInvalidOnBlur_; }

void AdDatePicker::setPreserveInvalidOnBlur(bool value) {
  if (preserveInvalidOnBlur_ == value) {
    return;
  }
  preserveInvalidOnBlur_ = value;
  emit preserveInvalidOnBlurChanged(preserveInvalidOnBlur_);
}

QLocale AdDatePicker::locale() const { return locale_; }

void AdDatePicker::setLocale(const QLocale& value) {
  if (locale_ == value) {
    return;
  }

  const QDate previousDate = date_;
  const QVector<QDate> previousSelectedDates = selectedDates_;
  locale_ = value;
  if (!applyingGlobalLocale_) {
    localeExplicit_ = true;
  }
  date_ = normalizeForPicker(pickerMode_, date_, effectiveFirstDayOfWeek());
  selectedDates_ = normalizedDates(selectedDates_);
  if (multiple_) {
    date_ = selectedDates_.isEmpty() ? QDate() : selectedDates_.constFirst();
  }

  syncLineEdit();
  syncPanelState();
  emit localeChanged(locale_);
  if (selectedDates_ != previousSelectedDates) {
    emit selectedDatesChanged(selectedDates_);
  }
  if (date_ != previousDate) {
    emit dateChanged(date_);
    emit dateTimeChanged(dateTime());
  }
}

QString AdDatePicker::placeholder() const { return placeholder_; }

void AdDatePicker::setPlaceholder(const QString& value) {
  if (placeholder_ == value) {
    return;
  }
  placeholder_ = value;
  syncLineEdit();
  emit placeholderChanged(placeholder_);
}

QString AdDatePicker::prefixText() const { return prefixText_; }

void AdDatePicker::setPrefixText(const QString& value) {
  if (prefixText_ == value) {
    return;
  }
  prefixText_ = value;
  syncLineEditStyle();
  emit prefixTextChanged(prefixText_);
}

QString AdDatePicker::suffixText() const { return suffixText_; }

void AdDatePicker::setSuffixText(const QString& value) {
  if (suffixText_ == value) {
    return;
  }
  suffixText_ = value;
  syncLineEditStyle();
  emit suffixTextChanged(suffixText_);
}

adqt::icons::IconRef AdDatePicker::prefixIconRef() const { return prefixIconRef_; }

void AdDatePicker::setPrefixIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(prefixIconRef_, value)) {
    return;
  }
  prefixIconRef_ = value;
  syncLineEditStyle();
  emit prefixIconRefChanged(prefixIconRef_);
}

adqt::icons::IconRef AdDatePicker::suffixIconRef() const { return suffixIconRef_; }

void AdDatePicker::setSuffixIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(suffixIconRef_, value)) {
    return;
  }
  suffixIconRef_ = value;
  syncLineEditStyle();
  emit suffixIconRefChanged(suffixIconRef_);
}

adqt::icons::IconRef AdDatePicker::feedbackIconRef() const { return feedbackIconRef_; }

void AdDatePicker::setFeedbackIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(feedbackIconRef_, value)) {
    return;
  }
  feedbackIconRef_ = value;
  syncLineEditStyle();
  emit feedbackIconRefChanged(feedbackIconRef_);
}

bool AdDatePicker::suffixIconVisible() const { return suffixIconVisible_; }

void AdDatePicker::setSuffixIconVisible(bool value) {
  if (suffixIconVisible_ == value) {
    return;
  }
  suffixIconVisible_ = value;
  syncLineEditStyle();
  emit suffixIconVisibleChanged(suffixIconVisible_);
}

adqt::icons::IconRef AdDatePicker::clearIconRef() const { return clearIconRef_; }

void AdDatePicker::setClearIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(clearIconRef_, value)) {
    return;
  }
  clearIconRef_ = value;
  syncLineEditStyle();
  emit clearIconRefChanged(clearIconRef_);
}

adqt::icons::IconRef AdDatePicker::prevIconRef() const { return prevIconRef_; }

void AdDatePicker::setPrevIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(prevIconRef_, value)) {
    return;
  }
  prevIconRef_ = value;
  syncPanelState();
  emit prevIconRefChanged(prevIconRef_);
}

adqt::icons::IconRef AdDatePicker::nextIconRef() const { return nextIconRef_; }

void AdDatePicker::setNextIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(nextIconRef_, value)) {
    return;
  }
  nextIconRef_ = value;
  syncPanelState();
  emit nextIconRefChanged(nextIconRef_);
}

adqt::icons::IconRef AdDatePicker::superPrevIconRef() const { return superPrevIconRef_; }

void AdDatePicker::setSuperPrevIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(superPrevIconRef_, value)) {
    return;
  }
  superPrevIconRef_ = value;
  syncPanelState();
  emit superPrevIconRefChanged(superPrevIconRef_);
}

adqt::icons::IconRef AdDatePicker::superNextIconRef() const { return superNextIconRef_; }

void AdDatePicker::setSuperNextIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(superNextIconRef_, value)) {
    return;
  }
  superNextIconRef_ = value;
  syncPanelState();
  emit superNextIconRefChanged(superNextIconRef_);
}

AdDatePicker::Placement AdDatePicker::placement() const { return placement_; }

void AdDatePicker::setPlacement(Placement value) {
  if (placement_ == value) {
    return;
  }
  placement_ = value;
  syncPopupGeometry();
  emit placementChanged(placement_);
}

AdDatePicker::PopupLayerMode AdDatePicker::popupLayerMode() const { return popupLayerMode_; }

void AdDatePicker::setPopupLayerMode(PopupLayerMode value) {
  if (popupLayerMode_ == value) {
    return;
  }
  const bool wasVisible = popupVisible_;
  if (wasVisible) {
    setPopupVisibleInternal(false, true);
  }
  popupLayerMode_ = value;
  applyPopupLayerMode();
  if (wasVisible) {
    setPopupVisibleInternal(true, true);
  }
  emit popupLayerModeChanged(popupLayerMode_);
}

AdDatePicker::ComponentTokens AdDatePicker::componentTokens() const { return componentTokens_; }

void AdDatePicker::setComponentTokens(const ComponentTokens& tokens) {
  if (panelComponentTokensEqual(componentTokens_, tokens)) {
    return;
  }
  componentTokens_ = tokens;
  syncLineEditStyle();
  syncPanelState();
  emit componentTokensChanged();
}

void AdDatePicker::resetComponentTokens() { setComponentTokens(ComponentTokens()); }

AdDatePicker::SemanticStyles AdDatePicker::semanticStyles() const { return semanticStyles_; }

void AdDatePicker::setSemanticStyles(const SemanticStyles& styles) {
  if (datePickerSemanticStylesEqual(semanticStyles_, styles)) {
    return;
  }
  semanticStyles_ = styles;
  syncLineEditStyle();
  syncPanelState();
  syncPopupGeometry();
  emit semanticStylesChanged();
}

void AdDatePicker::setSemanticStyleResolver(SemanticStyleResolver resolver) {
  semanticStyleResolver_ = std::move(resolver);
  syncLineEditStyle();
  syncPanelState();
  syncPopupGeometry();
  emit semanticStylesChanged();
}

void AdDatePicker::clearSemanticStyleResolver() {
  setSemanticStyleResolver(SemanticStyleResolver());
}

QVector<AdDatePicker::PresetItem> AdDatePicker::presets() const { return presets_; }

void AdDatePicker::setPresets(const QVector<PresetItem>& presets) {
  if (presetsEqual(presets_, presets)) {
    return;
  }
  presets_ = presets;
  syncPanelState();
  emit presetsChanged();
}

void AdDatePicker::clearPresets() { setPresets({}); }

QWidget* AdDatePicker::extraFooterWidget() const { return extraFooterWidget_; }

void AdDatePicker::setExtraFooterWidget(QWidget* widget) {
  if (extraFooterWidget_ == widget) {
    return;
  }

  QWidget* previous = extraFooterWidget_.data();
  if (popupPanel_) {
    previous = popupPanel_->takeExtraFooterWidget();
    if (previous && previous != widget) {
      previous->setParent(nullptr);
    }
  } else if (previous && previous != widget) {
    previous->hide();
    previous->setParent(nullptr);
  }

  extraFooterWidget_ = widget;
  if (popupPanel_) {
    popupPanel_->setExtraFooterWidget(extraFooterWidget_.data());
  } else if (extraFooterWidget_) {
    extraFooterWidget_->setParent(this);
    extraFooterWidget_->hide();
  }
  emit extraFooterWidgetChanged(extraFooterWidget_.data());
}

QWidget* AdDatePicker::takeExtraFooterWidget() {
  QWidget* widget = popupPanel_ ? popupPanel_->takeExtraFooterWidget() : extraFooterWidget_.data();
  if (!widget) {
    return nullptr;
  }
  if (!popupPanel_) {
    widget->hide();
    widget->setParent(nullptr);
  }
  extraFooterWidget_ = nullptr;
  emit extraFooterWidgetChanged(nullptr);
  return widget;
}

AdDatePicker::DatePredicate AdDatePicker::disabledDatePredicate() const {
  return disabledDatePredicate_;
}

void AdDatePicker::setDisabledDatePredicate(DatePredicate predicate) {
  const bool hadPredicate = static_cast<bool>(disabledDatePredicate_);
  const bool hasPredicate = static_cast<bool>(predicate);
  if (!hadPredicate && !hasPredicate) {
    return;
  }
  disabledDatePredicate_ = std::move(predicate);
  panelDisabledDatePredicateDirty_ = true;
  if (popupVisible_) {
    syncPanelState();
  }
}

AdDatePicker::DisabledDatePredicate AdDatePicker::disabledDateContextPredicate() const {
  return disabledDateContextPredicate_;
}

void AdDatePicker::setDisabledDateContextPredicate(DisabledDatePredicate predicate) {
  const bool hadPredicate = static_cast<bool>(disabledDateContextPredicate_);
  const bool hasPredicate = static_cast<bool>(predicate);
  if (!hadPredicate && !hasPredicate) {
    return;
  }
  disabledDateContextPredicate_ = std::move(predicate);
  panelDisabledDateContextPredicateDirty_ = true;
  if (popupVisible_) {
    syncPanelState();
  }
}

AdDatePicker::DisabledTimePredicate AdDatePicker::disabledTimePredicate() const {
  return disabledTimePredicate_;
}

void AdDatePicker::setDisabledTimePredicate(DisabledTimePredicate predicate) {
  const bool hadPredicate = static_cast<bool>(disabledTimePredicate_);
  const bool hasPredicate = static_cast<bool>(predicate);
  if (!hadPredicate && !hasPredicate) {
    return;
  }
  disabledTimePredicate_ = std::move(predicate);
  panelDisabledTimePredicateDirty_ = true;
  if (popupVisible_) {
    syncPanelState();
    syncPopupGeometry();
  }
}

AdDatePicker::DisplayTextCallback AdDatePicker::displayTextCallback() const {
  return displayTextCallback_;
}

void AdDatePicker::setDisplayTextCallback(DisplayTextCallback callback) {
  displayTextCallback_ = std::move(callback);
  syncLineEdit();
  emit displayTextCallbackChanged();
}

void AdDatePicker::clearDisplayTextCallback() { setDisplayTextCallback(DisplayTextCallback()); }

AdDatePicker::CellRenderCallback AdDatePicker::cellRenderCallback() const {
  return cellRenderCallback_;
}

void AdDatePicker::setCellRenderCallback(CellRenderCallback callback) {
  const bool hadCallback = static_cast<bool>(cellRenderCallback_);
  const bool hasCallback = static_cast<bool>(callback);
  if (!hadCallback && !hasCallback) {
    return;
  }
  cellRenderCallback_ = std::move(callback);
  panelCellRenderCallbackDirty_ = true;
  if (popupVisible_) {
    syncPanelState();
  }
  emit cellRenderCallbackChanged();
}

void AdDatePicker::clearCellRenderCallback() { setCellRenderCallback(CellRenderCallback()); }

AdDatePicker::PopupContentWrapperFactory AdDatePicker::popupContentWrapperFactory() const {
  return popupContentWrapperFactory_;
}

void AdDatePicker::setPopupContentWrapperFactory(PopupContentWrapperFactory factory) {
  popupContentWrapperFactory_ = std::move(factory);
  const bool wasVisible = popupVisible_;
  if (popup_) {
    if (wasVisible) {
      setPopupVisibleInternal(false, true);
    }
    destroyPopup();
    if (wasVisible) {
      setPopupVisibleInternal(true, true);
    }
  }
  emit popupContentWrapperFactoryChanged();
}

void AdDatePicker::clearPopupContentWrapperFactory() {
  setPopupContentWrapperFactory(PopupContentWrapperFactory());
}

AdDatePicker::PanelComponentFactory AdDatePicker::panelComponentFactory() const {
  return panelComponentFactory_;
}

void AdDatePicker::setPanelComponentFactory(PanelComponentFactory factory) {
  panelComponentFactory_ = std::move(factory);
  const bool wasVisible = popupVisible_;
  if (popup_) {
    if (wasVisible) {
      setPopupVisibleInternal(false, true);
    }
    destroyPopup();
    if (wasVisible) {
      setPopupVisibleInternal(true, true);
    }
  }
  emit panelComponentFactoryChanged();
}

void AdDatePicker::clearPanelComponentFactory() {
  setPanelComponentFactory(PanelComponentFactory());
}

void AdDatePicker::focus() {
  if (lineEdit_) {
    lineEdit_->focusEditor(AdLineEdit::FocusSelection::Preserve);
  } else {
    setFocus(Qt::OtherFocusReason);
  }
}

void AdDatePicker::blur() {
  if (lineEdit_) {
    lineEdit_->blurInput();
  } else {
    clearFocus();
  }
}

AdLineEdit* AdDatePicker::lineEdit() const { return lineEdit_; }

AdDatePickerPanel* AdDatePicker::panel() const { return popupPanel_; }

QSize AdDatePicker::sizeHint() const { return lineEdit_ ? lineEdit_->sizeHint() : QSize(160, 32); }

QSize AdDatePicker::minimumSizeHint() const {
  return lineEdit_ ? lineEdit_->minimumSizeHint() : QSize(96, 32);
}

void AdDatePicker::schedulePopupFocusOutClose() {
  if (!popupVisible_) {
    return;
  }

  suppressInputCommitOnFocusOut_ = true;
  QPointer<AdDatePicker> guard(this);
  QTimer::singleShot(0, this, [guard]() {
    if (!guard) {
      return;
    }

    QWidget* focused = QApplication::focusWidget();
    const bool focusInsideInput = widgetInTree(focused, guard->lineEdit_);
    const bool focusInsidePopup = widgetInTree(focused, guard->popup_);
    if (focusInsideInput || focusInsidePopup) {
      guard->suppressInputCommitOnFocusOut_ = false;
      return;
    }

    if (guard->popupVisible_) {
      guard->hidePopup();
    }
    guard->syncLineEdit();
    guard->suppressInputCommitOnFocusOut_ = false;
  });
}

bool AdDatePicker::eventFilter(QObject* watched, QEvent* event) {
  if (!event || !lineEdit_) {
    return QWidget::eventFilter(watched, event);
  }

  if (watched == lineEdit_ || watched == lineEdit_->trailingActionButton()) {
    if (watched == lineEdit_ && event->type() == QEvent::FocusIn) {
      emit focused();
    } else if (watched == lineEdit_ && event->type() == QEvent::FocusOut) {
      schedulePopupFocusOutClose();
      emit blurred();
    } else if (event->type() == QEvent::MouseButtonPress) {
      auto* mouseEvent = static_cast<QMouseEvent*>(event);
      if (mouseEvent->button() == Qt::LeftButton && isEnabled()) {
        showPopup();
      }
    } else if (event->type() == QEvent::KeyPress) {
      auto* keyEvent = static_cast<QKeyEvent*>(event);
      if (keyEvent->key() == Qt::Key_Down) {
        showPopup();
        keyEvent->accept();
        return true;
      }
      if (keyEvent->key() == Qt::Key_Escape && popupVisible_) {
        hidePopup();
        keyEvent->accept();
        return true;
      }
    }
  } else if (watched == popup_ && event->type() == QEvent::Hide && popupVisible_ &&
             (!popupController_ || popupController_->popupVisible()) && !suppressPopupHideClose_) {
    setPopupVisibleInternal(false, true);
  }

  return QWidget::eventFilter(watched, event);
}

void AdDatePicker::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (!event) {
    return;
  }
  if (event->type() == QEvent::LanguageChange) {
    if (!localeExplicit_) {
      applyingGlobalLocale_ = true;
      setLocale(adqt::locale::LocaleManager::instance().locale());
      applyingGlobalLocale_ = false;
    }
    syncLineEditStyle();
    syncPanelState();
  } else if (event->type() == QEvent::EnabledChange) {
    syncLineEditStyle();
    syncPanelState();
  }
}

void AdDatePicker::moveEvent(QMoveEvent* event) {
  QWidget::moveEvent(event);
  syncPopupGeometry();
}

void AdDatePicker::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  syncPopupGeometry();
}

void AdDatePicker::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  if (!defaultOpen_ || defaultOpenApplied_) {
    return;
  }
  defaultOpenApplied_ = true;
  QTimer::singleShot(0, this, [this]() {
    if (defaultOpen_ && isVisible() && !popupVisible_) {
      showPopup();
    }
  });
}

void AdDatePicker::hideEvent(QHideEvent* event) {
  QWidget::hideEvent(event);
  hidePopup();
}

void AdDatePicker::buildUi() {
  setFocusPolicy(Qt::StrongFocus);
  setSemanticSlot(this, "root", QStringLiteral("addatepicker"));
  rootLayout_ = new QHBoxLayout(this);
  rootLayout_->setContentsMargins(0, 0, 0, 0);
  rootLayout_->setSpacing(0);

  lineEdit_ = new detail::DatePickerLineEdit(this);
  setSemanticSlot(lineEdit_, "input", QStringLiteral("addatepicker-input"));
  lineEdit_->setClearOverlaysTrailingAction(true);
  lineEdit_->setTrailingActionVisible(true);
  lineEdit_->setTrailingActionAccessibleName(tr("Open calendar"));
  lineEdit_->setAllowClear(allowClear_);
  lineEdit_->installEventFilter(this);
  if (lineEdit_->trailingActionButton()) {
    lineEdit_->trailingActionButton()->installEventFilter(this);
    connect(lineEdit_->trailingActionButton(), &QToolButton::clicked, this, [this]() {
      if (popupVisible_) {
        hidePopup();
      } else {
        showPopup();
      }
    });
  }
  connect(lineEdit_, &QLineEdit::editingFinished, this, [this]() { commitInputText(); });
  connect(lineEdit_, &QLineEdit::returnPressed, this, [this]() { commitInputText(); });
  connect(lineEdit_, &AdLineEdit::cleared, this, [this]() { clearDateInternal(true); });
  rootLayout_->addWidget(lineEdit_);

  syncInputIds();
  syncLineEditStyle();
  syncLineEdit();
  syncLineEditMask();
}

void AdDatePicker::ensurePopup() {
  if (popup_) {
    return;
  }
  QWidget* scopeWindow = detail::resolvePopupScopeWindow(this);
  auto* surface = new detail::OverlayPopupSurface(
      popupLayerMode_ == PopupLayerMode::QtTool ? nullptr : scopeWindow);
  if (popupLayerMode_ == PopupLayerMode::QtTool) {
    surface->setWindowFlags(adQtToolWindowFlags());
    surface->setAttribute(Qt::WA_ShowWithoutActivating, true);
    surface->setAttribute(Qt::WA_TranslucentBackground, true);
    surface->setAttribute(Qt::WA_QuitOnClose, false);
  }
  popup_ = surface;
  popup_->setObjectName(QStringLiteral("addatepicker-popup"));
  setSemanticSlot(popup_, "popup.root", QStringLiteral("addatepicker-popup"));
  popup_->setProperty("adqt.interaction.surface", true);
  popup_->setAttribute(Qt::WA_DeleteOnClose, false);
  popup_->installEventFilter(this);
  applyPopupLayerMode();

  popupBodyHost_ = surface->bodyWidget();
  if (popupBodyHost_) {
    popupBodyHost_->setObjectName(QStringLiteral("addatepicker-popup-body"));
    setSemanticSlot(popupBodyHost_, "popup.container", QStringLiteral("addatepicker-popup-body"));
    popupBodyHost_->setProperty("adqt.interaction.surface", true);
    popupBodyHost_->setAutoFillBackground(false);
  }

  popupLayout_ = new QVBoxLayout(popupBodyHost_ ? popupBodyHost_ : popup_);
  popupLayout_->setContentsMargins(0, 0, 0, 0);
  popupLayout_->setSpacing(0);

  popupPanel_ = new AdDatePickerPanel(popupBodyHost_ ? popupBodyHost_ : popup_);
  connect(popupPanel_, &AdDatePickerPanel::accepted, this, [this](const QDate& value) {
    const QTime selectedTime = popupPanel_ ? popupPanel_->selectedTime() : time_;
    setDateTime(dateTimeFromParts(value, selectedTime));
    hidePopup();
    emit accepted(date_);
    emit acceptedDateTime(dateTime());
  });
  connect(popupPanel_, &AdDatePickerPanel::datesAccepted, this,
          [this](const QVector<QDate>& values) {
            setSelectedDates(values);
            if (needConfirm_) {
              hidePopup();
            }
            emit datesAccepted(selectedDates_);
          });
  connect(popupPanel_, &AdDatePickerPanel::viewDateChanged, this, [this](const QDate& value) {
    if (!syncingPopupPanel_) {
      emit panelChanged(value, popupPanel_ ? popupPanel_->panelMode() : effectivePanelMode());
    }
  });
  connect(popupPanel_, &AdDatePickerPanel::previewDateChanged, this,
          [this](const QDate& value) { handlePreviewDateChanged(value); });
  connect(popupPanel_, &AdDatePickerPanel::previewTimeChanged, this,
          [this](const QTime& value, TimeSelectionPart part) {
            handlePreviewTimeChanged(value, part);
          });
  connect(popupPanel_, &AdDatePickerPanel::selectedTimeChanged, this,
          [this](const QTime& value) { handlePopupSelectedTimeChanged(value); });
  connect(popupPanel_, &AdDatePickerPanel::panelModeChanged, this, [this](PickerMode value) {
    if (syncingPopupPanel_) {
      return;
    }
    const PickerMode normalized = normalizedPanelMode(value);
    const bool changed = !panelModeExplicit_ || panelMode_ != normalized;
    panelMode_ = normalized;
    panelModeExplicit_ = true;
    if (changed) {
      emit panelModeChanged(panelMode_);
    }
    emit panelChanged(popupPanel_ ? popupPanel_->viewDate() : QDate(), panelMode_);
  });
  syncPanelState();
  QWidget* popupContentParent = popupBodyHost_ ? popupBodyHost_ : popup_;
  QWidget* componentContent =
      createPanelComponentWidget(popupContentParent, popupPanel_, PanelComponentRole::Single);
  popupContentWidget_ =
      wrappedPopupContent(popupContentParent, componentContent, popupContentWrapperFactory_);
  popupLayout_->addWidget(popupContentWidget_ ? popupContentWidget_ : componentContent);
  if (popupController_) {
    popupController_->popupSurfaceChanged();
  }
}

void AdDatePicker::destroyPopup() {
  if (!popup_) {
    return;
  }

  QScopedValueRollback<bool> guard(suppressPopupHideClose_, true);
  popup_->hide();
  popup_->deleteLater();
  popup_ = nullptr;
  popupBodyHost_ = nullptr;
  popupLayout_ = nullptr;
  popupContentWidget_ = nullptr;
  popupPanel_ = nullptr;
  panelDisabledDatePredicateDirty_ = true;
  panelDisabledDateContextPredicateDirty_ = true;
  panelDisabledTimePredicateDirty_ = true;
  panelCellRenderCallbackDirty_ = true;
  if (popupController_) {
    popupController_->popupSurfaceChanged();
    popupController_->invalidatePopupGeometry();
  }
}

void AdDatePicker::applyPopupLayerMode() {
  if (!popup_) {
    return;
  }
  QWidget* scopeWindow = detail::resolvePopupScopeWindow(this);
  QWidget* targetParent = popupLayerMode_ == PopupLayerMode::QtTool ? nullptr : scopeWindow;
  const Qt::WindowFlags flags =
      popupLayerMode_ == PopupLayerMode::QtTool ? adQtToolWindowFlags() : Qt::Widget;
  const bool useToolWindow = popupLayerMode_ == PopupLayerMode::QtTool;
  popup_->setAttribute(Qt::WA_ShowWithoutActivating, useToolWindow);
  popup_->setAttribute(Qt::WA_TranslucentBackground, useToolWindow);
  popup_->setAttribute(Qt::WA_QuitOnClose, !useToolWindow);
  if (popup_->parentWidget() != targetParent || popup_->windowFlags() != flags) {
    popup_->setParent(targetParent, flags);
    if (popupController_) {
      popupController_->popupSurfaceChanged();
      popupController_->invalidatePopupGeometry();
    }
  }
}

void AdDatePicker::syncPopupGeometry() {
  if (!popupController_) {
    return;
  }
  popupController_->invalidatePopupGeometry();
  if (popupController_->popupVisible()) {
    popupController_->refreshVisiblePopup();
  }
}

void AdDatePicker::popupPrepareToShow() {
  const bool popupAlreadyCreated = popup_ != nullptr;
  ensurePopup();
  if (popupAlreadyCreated) {
    syncPanelState();
  }

  auto* surface = static_cast<detail::OverlayPopupSurface*>(popup_);
  detail::DatePickerStyleInput input;
  input.size = size_;
  input.variant = variant_;
  input.status = status_;
  input.disabled = !isEnabled();
  input.baseFont = font();
  input.componentTokens = componentTokens_;
  input.semanticStyles = effectiveSemanticStyles().popup;
  const detail::DatePickerVisualStyle style = detail::resolveDatePickerVisualStyle(
      input, adqt::theme::ThemeManager::instance().resolve(this));
  detail::OverlayPopupSurfaceStyle surfaceStyle;
  surfaceStyle.background = style.panelBackground;
  surfaceStyle.borderColor = QColor(0, 0, 0, 0);
  surfaceStyle.metrics.borderRadius = std::max(0, style.metrics.borderRadius);
  surfaceStyle.metrics.borderWidth = 0;
  surfaceStyle.metrics.arrowSize = 0;
  surface->setSurfaceStyle(surfaceStyle);
  surface->setArrowVisible(false);
  surface->setPlacement(toOverlayPopupPlacement(placement_));
  popup_->setProperty("adqt.zIndex", style.metrics.zIndexPopup);

  if (popupLayout_) {
    popupLayout_->activate();
  }
  if (popupBodyHost_) {
    popupBodyHost_->updateGeometry();
  }
  popup_->adjustSize();
}

void AdDatePicker::setPopupVisibleInternal(bool value, bool emitSignal) {
  Q_UNUSED(emitSignal)
  if (popupVisible_ == value) {
    return;
  }
  if (value && !isEnabled()) {
    return;
  }
  if (!popupController_) {
    return;
  }

  popupController_->setDisabled(!isEnabled());
  popupController_->setPopupVisible(value);
}

void AdDatePicker::syncLineEdit() {
  if (!lineEdit_) {
    return;
  }
  QScopedValueRollback<bool> guard(syncingText_, true);
  lineEdit_->setPlaceholderText(effectivePlaceholder());
  if (multiple_) {
    const QStringList tags = effectiveMultipleDisplayTexts(selectedDates_);
    lineEdit_->setDateTagTexts(tags, maxTagCount_, responsiveMaxTagCount_);
    lineEdit_->setText(tags.join(QStringLiteral(", ")));
    return;
  }
  lineEdit_->clearDateTagTexts();
  lineEdit_->setText(effectiveDisplayText(date_));
}

void AdDatePicker::syncLineEditMask() {
  if (!lineEdit_) {
    return;
  }

  const bool enabled = maskFormat_ && !multiple_ && pickerMode_ == PickerMode::Date;
  const QString format = defaultDisplayFormat();
  const QString maskBody = enabled ? inputMaskBodyForDateFormat(format) : QString();
  lineEdit_->setInputMask(maskBody.isEmpty() ? QString() : maskBody + QStringLiteral(";_"));
}

void AdDatePicker::syncInputIds() {
  const QString inputId = normalizedInputId(id_);
  setObjectName(inputId);
  setProperty("adqt.inputId", inputId);
  applyAccessibleIdentifier(this, inputId);

  if (!lineEdit_) {
    return;
  }
  lineEdit_->setObjectName(inputId);
  lineEdit_->setProperty("adqt.inputId", inputId);
  applyAccessibleIdentifier(lineEdit_, inputId);
}

AdDatePicker::SemanticStyles AdDatePicker::effectiveSemanticStyles() const {
  if (!semanticStyleResolver_) {
    return semanticStyles_;
  }

  StyleContext context;
  context.pickerMode = pickerMode_;
  context.size = size_;
  context.variant = variant_;
  context.status = status_;
  context.disabled = !isEnabled();
  context.popupVisible = popupVisible_;
  context.multiple = multiple_;
  context.showTime = showTime_;
  context.needConfirm = needConfirm_;
  context.date = date_;
  context.selectedDates = selectedDates_;
  return semanticStyleResolver_(context);
}

AdDatePicker::PanelComponentContext AdDatePicker::makePanelComponentContext(
    AdDatePickerPanel* panel, PanelComponentRole role) {
  PanelComponentContext context;
  context.originPanel = panel;
  context.role = role;
  context.pickerMode = pickerMode_;
  context.panelMode = panel ? panel->panelMode() : effectivePanelMode();
  context.selectedDate = date_;
  context.selectedDates = selectedDates_;
  context.viewDate = panel ? panel->viewDate() : QDate();
  context.range = false;
  context.multiple = multiple_;
  context.disabled = !isEnabled();
  context.selectDate = [this](const QDate& value) { selectPanelComponentDate(value); };
  context.previewDate = [this](const QDate& value) { handlePreviewDateChanged(value); };
  context.setViewDate = [panel](const QDate& value) {
    if (panel) {
      panel->setViewDate(value);
    }
  };
  context.setPanelMode = [panel](PickerMode value) {
    if (panel) {
      panel->setPanelMode(value);
    }
  };
  context.acceptSelection = [this]() { acceptPanelComponentSelection(); };
  return context;
}

QWidget* AdDatePicker::createPanelComponentWidget(QWidget* parent, AdDatePickerPanel* panel,
                                                  PanelComponentRole role) {
  QWidget* content = panel;
  if (panelComponentFactory_) {
    if (QWidget* replacement =
            panelComponentFactory_(makePanelComponentContext(panel, role), parent)) {
      replacement->setProperty("adqt.panelComponentContent", true);
      if (!replacement->parentWidget() && parent) {
        replacement->setParent(parent);
      }
      content = replacement;
    }
  }
  if (panel && content != panel) {
    panel->hide();
  }
  return content;
}

void AdDatePicker::selectPanelComponentDate(const QDate& value) {
  if (!isEnabled()) {
    return;
  }
  const QDate normalized = normalizeForPicker(pickerMode_, value, effectiveFirstDayOfWeek());
  if (!normalized.isValid() || !isDateSelectable(normalized)) {
    return;
  }

  if (multiple_) {
    QVector<QDate> next = selectedDates_;
    const qsizetype index = next.indexOf(normalized);
    if (index >= 0) {
      next.removeAt(index);
    } else {
      next.append(normalized);
    }
    setSelectedDates(next);
    if (!needConfirm_) {
      emit datesAccepted(selectedDates_);
    }
    return;
  }

  const QTime selectedTime = popupPanel_ ? popupPanel_->selectedTime() : time_;
  if (needConfirm_) {
    if (popupPanel_) {
      popupPanel_->setSelectedDate(normalized);
      popupPanel_->setSelectedTime(selectedTime);
    }
    return;
  }

  setDateTime(dateTimeFromParts(normalized, selectedTime));
  hidePopup();
  emit accepted(date_);
  emit acceptedDateTime(dateTime());
}

void AdDatePicker::acceptPanelComponentSelection() {
  if (!isEnabled()) {
    return;
  }
  if (multiple_) {
    emit datesAccepted(selectedDates_);
    hidePopup();
    return;
  }
  const QDate value =
      popupPanel_ && popupPanel_->selectedDate().isValid() ? popupPanel_->selectedDate() : date_;
  if (!value.isValid() || !isDateSelectable(value)) {
    return;
  }
  const QTime selectedTime = popupPanel_ ? popupPanel_->selectedTime() : time_;
  setDateTime(dateTimeFromParts(value, selectedTime));
  hidePopup();
  emit accepted(date_);
  emit acceptedDateTime(dateTime());
}

bool AdDatePicker::syncPopupPanelSelectionText() {
  if (!lineEdit_ || !popupVisible_ || !popupPanel_ || multiple_ || !effectiveTextIncludesTime()) {
    return false;
  }

  QDate displayDate = popupPanel_->selectedDate().isValid() ? popupPanel_->selectedDate() : date_;
  if (!displayDate.isValid() && pickerMode_ != PickerMode::Time) {
    return false;
  }

  const QTime displayTime = popupPanel_->selectedTime();
  const QString text =
      displayTextCallback_ && (displayDate.isValid() || pickerMode_ == PickerMode::Time)
          ? displayTextCallback_(displayDate, displayTime)
          : formatDefaultDateTime(displayDate, displayTime, pickerMode_, defaultDisplayFormat(),
                                  locale_, effectiveFirstDayOfWeek(), effectiveTextIncludesTime());

  QScopedValueRollback<bool> guard(syncingText_, true);
  lineEdit_->clearDateTagTexts();
  lineEdit_->setText(text);
  return true;
}

void AdDatePicker::handlePopupSelectedTimeChanged(const QTime& value) {
  Q_UNUSED(value)
  if (syncingPopupPanel_) {
    return;
  }

  previewDate_ = QDate();
  previewTime_ = QTime();
  previewTimeActive_ = false;
  if (!syncPopupPanelSelectionText()) {
    syncLineEdit();
  }
}

void AdDatePicker::handlePreviewDateChanged(const QDate& value) {
  if (previewValue_ != PreviewValue::Hover || multiple_ || !lineEdit_ || !popupVisible_) {
    clearPreviewText();
    return;
  }

  const QDate normalized = normalizeForPicker(pickerMode_, value, effectiveFirstDayOfWeek());
  if (!normalized.isValid()) {
    clearPreviewText();
    return;
  }
  if (previewDate_ == normalized) {
    return;
  }

  previewDate_ = normalized;
  QScopedValueRollback<bool> guard(syncingText_, true);
  lineEdit_->clearDateTagTexts();
  const QTime displayTime = previewTimeActive_ ? previewTime_ : time_;
  lineEdit_->setText(formatDefaultDateTime(previewDate_, displayTime, pickerMode_,
                                           defaultDisplayFormat(), locale_,
                                           effectiveFirstDayOfWeek(), effectiveTextIncludesTime()));
}

void AdDatePicker::handlePreviewTimeChanged(const QTime& value, TimeSelectionPart part) {
  Q_UNUSED(part)
  if (previewValue_ != PreviewValue::Hover || multiple_ || !lineEdit_ || !popupVisible_ ||
      !effectiveTextIncludesTime()) {
    clearPreviewText();
    return;
  }
  if (!value.isValid()) {
    clearPreviewText();
    return;
  }

  QDate displayDate = previewDate_.isValid() ? previewDate_ : date_;
  if (!displayDate.isValid() && popupPanel_) {
    displayDate = popupPanel_->selectedDate().isValid() ? popupPanel_->selectedDate()
                                                        : popupPanel_->viewDate();
  }
  if (!displayDate.isValid() && pickerMode_ != PickerMode::Time) {
    clearPreviewText();
    return;
  }
  if (previewTimeActive_ && previewTime_ == value && previewDate_ == displayDate) {
    return;
  }

  previewTime_ = value;
  previewTimeActive_ = true;
  QScopedValueRollback<bool> guard(syncingText_, true);
  lineEdit_->clearDateTagTexts();
  lineEdit_->setText(formatDefaultDateTime(displayDate, previewTime_, pickerMode_,
                                           defaultDisplayFormat(), locale_,
                                           effectiveFirstDayOfWeek(), effectiveTextIncludesTime()));
}

void AdDatePicker::clearPreviewText() {
  if (!previewDate_.isValid() && !previewTimeActive_) {
    return;
  }
  previewDate_ = QDate();
  previewTime_ = QTime();
  previewTimeActive_ = false;
  if (!syncPopupPanelSelectionText()) {
    syncLineEdit();
  }
}

void AdDatePicker::syncLineEditStyle() {
  if (!lineEdit_) {
    return;
  }
  const SemanticStyles semantic = effectiveSemanticStyles();
  applyInputSemanticColors(lineEdit_, semantic);
  applyPickerSuffixTokenColors(lineEdit_, semantic);
  lineEdit_->setControlSize(toInputSize(size_));
  lineEdit_->setVariant(toInputVariant(variant_));
  lineEdit_->setStatus(toInputStatus(status_));
  lineEdit_->setDisabled(!isEnabled());
  lineEdit_->setAllowClear(allowClear_);
  lineEdit_->setReadOnly(inputReadOnly_);
  lineEdit_->setTrailingActionLeading(true);
  lineEdit_->setTrailingActionVisible(suffixIconVisible_);
  lineEdit_->setTrailingActionAccessibleName(pickerMode_ == AdDatePickerPanel::PickerMode::Time
                                                 ? tr("Open time picker")
                                                 : tr("Open calendar"));
  lineEdit_->setPrefixText(prefixText_);
  lineEdit_->setSuffixText(suffixText_);
  lineEdit_->setPrefixIconRef(prefixIconRef_);
  const bool customSuffixIcon = adqt::icons::isValid(suffixIconRef_);
  lineEdit_->setSuffixIconRef(adqt::icons::IconRef());
  lineEdit_->setFeedbackIconRef(suffixIconVisible_ && !customSuffixIcon ? feedbackIconRef_
                                                                        : adqt::icons::IconRef());
  lineEdit_->setClearIconRef(clearIconRef_);
  lineEdit_->setTrailingActionIconRef(customSuffixIcon ? suffixIconRef_
                                                       : defaultPickerSuffixIcon(pickerMode_));
  lineEdit_->setProperty("adqt.datePicker.defaultSuffixIcon",
                         suffixIconVisible_ && !customSuffixIcon
                             ? defaultPickerSuffixIconName(pickerMode_)
                             : QString());
  lineEdit_->setDateTagTokens(componentTokens_, size_);
}

void AdDatePicker::syncPanelState() {
  if (!popupPanel_) {
    return;
  }
  QScopedValueRollback<bool> guard(syncingPopupPanel_, true);
  const QDate previousPanelViewDate = popupPanel_->viewDate();
  popupPanel_->setPickerMode(pickerMode_);
  popupPanel_->setMinDate(minDate_);
  popupPanel_->setMaxDate(maxDate_);
  popupPanel_->setSelectionMode(multiple_ ? AdDatePickerPanel::SelectionMode::Multiple
                                          : AdDatePickerPanel::SelectionMode::Single);
  popupPanel_->setSelectedDate(date_);
  popupPanel_->setSelectedDates(selectedDates_);
  popupPanel_->setDefaultOpenTime(defaultOpenTime_);
  popupPanel_->setSelectedTime(date_.isValid() ? time_ : defaultOpenTime_);
  popupPanel_->setPanelMode(effectivePanelMode());
  popupPanel_->setLocale(locale_);
  QDate panelViewDate = pickerValue_;
  if (!panelViewDate.isValid() && popupVisible_ && previousPanelViewDate.isValid()) {
    panelViewDate = previousPanelViewDate;
  }
  if (!panelViewDate.isValid()) {
    panelViewDate = defaultPickerValue_.isValid() ? defaultPickerValue_
                                                  : (date_.isValid() ? date_ : todayDate());
  }
  popupPanel_->setViewDate(panelViewDate);
  popupPanel_->setShowToday(showToday_);
  popupPanel_->setShowWeek(effectiveShowWeek());
  popupPanel_->setNeedConfirm(needConfirm_);
  popupPanel_->setShowTime(showTime_ && !multiple_);
  popupPanel_->setShowNow(showNow_);
  popupPanel_->setOrder(order_);
  popupPanel_->setTimeFormat(effectiveTimeFormat());
  popupPanel_->setTimeSteps(hourStep_, minuteStep_, secondStep_);
  popupPanel_->setHideDisabledOptions(hideDisabledOptions_);
  popupPanel_->setUse12Hours(effectiveUse12Hours());
  popupPanel_->setChangeOnScroll(changeOnScroll_);
  popupPanel_->setShowHour(showHour_);
  popupPanel_->setShowMinute(showMinute_);
  if (showSecondExplicit_) {
    popupPanel_->setShowSecond(showSecond_);
  } else {
    popupPanel_->resetShowSecond();
  }
  popupPanel_->setDisabled(!isEnabled());
  popupPanel_->setComponentTokens(componentTokens_);
  popupPanel_->setSemanticStyles(effectiveSemanticStyles().popup);
  popupPanel_->setPresets(presets_);
  popupPanel_->setExtraFooterWidget(extraFooterWidget_.data());
  if (panelDisabledDatePredicateDirty_) {
    popupPanel_->setDisabledDatePredicate(disabledDatePredicate_);
    panelDisabledDatePredicateDirty_ = false;
  }
  if (panelDisabledDateContextPredicateDirty_) {
    popupPanel_->setDisabledDateContextPredicate(disabledDateContextPredicate_);
    panelDisabledDateContextPredicateDirty_ = false;
  }
  if (panelDisabledTimePredicateDirty_) {
    popupPanel_->setDisabledTimePredicate(disabledTimePredicate_);
    panelDisabledTimePredicateDirty_ = false;
  }
  if (panelCellRenderCallbackDirty_) {
    popupPanel_->setCellRenderCallback(cellRenderCallback_);
    panelCellRenderCallbackDirty_ = false;
  }
  popupPanel_->setNavigationIconRefs(superPrevIconRef_, prevIconRef_, nextIconRef_,
                                     superNextIconRef_);
}

void AdDatePicker::commitInputText() {
  if (!lineEdit_ || syncingText_) {
    return;
  }
  if (suppressInputCommitOnFocusOut_) {
    syncLineEdit();
    return;
  }
  if (!isEnabled()) {
    syncLineEdit();
    return;
  }
  const QString text = lineEdit_->text().trimmed();

  if (multiple_) {
    if (text.isEmpty()) {
      syncLineEdit();
      return;
    }

    static const QRegularExpression separator(QStringLiteral("\\s*[,;，；]\\s*"));
    const QStringList parts = text.split(separator, Qt::SkipEmptyParts);
    QVector<QDate> parsedValues;
    parsedValues.reserve(parts.size());
    bool allOk = !parts.isEmpty();
    for (const QString& part : parts) {
      bool ok = false;
      const QDate parsed = parsePickerText(pickerMode_, part, effectiveParseFormats(), locale_,
                                           effectiveFirstDayOfWeek(), &ok);
      if (!ok || !parsed.isValid()) {
        allOk = false;
        break;
      }
      if (!isDateSelectable(parsed)) {
        allOk = false;
        break;
      }
      parsedValues.append(parsed);
    }

    if (allOk) {
      setSelectedDates(parsedValues);
      emit datesAccepted(selectedDates_);
    } else if (!preserveInvalidOnBlur_) {
      syncLineEdit();
    }
    return;
  }

  if (text.isEmpty()) {
    clearDateInternal(true);
    return;
  }

  bool ok = false;
  const QDateTime parsed = maskFormat_ ? parseMaskedText(text, &ok) : parseText(text, &ok);
  if (ok && parsed.isValid() && isDateTimeSelectable(parsed)) {
    setDateTime(parsed);
    emit accepted(date_);
    emit acceptedDateTime(dateTime());
  } else if (!preserveInvalidOnBlur_) {
    syncLineEdit();
  }
}

void AdDatePicker::clearDateInternal(bool emitSignals) {
  const bool hadDate = date_.isValid();
  const bool hadTime = time_ != defaultTimeValue();
  const bool hadSelectedDates = !selectedDates_.isEmpty();
  if (!hadDate && !hadTime && !hadSelectedDates) {
    syncLineEdit();
    return;
  }
  date_ = QDate();
  time_ = defaultTimeValue();
  selectedDates_.clear();
  syncLineEdit();
  syncPanelState();
  if (emitSignals) {
    if (hadSelectedDates) {
      emit selectedDatesChanged(selectedDates_);
    }
    if (hadDate) {
      emit dateChanged(date_);
    }
    if (hadTime) {
      emit timeChanged(time_);
    }
    emit dateTimeChanged(dateTime());
    emit cleared();
  }
}

QVector<QDate> AdDatePicker::normalizedDates(const QVector<QDate>& values) const {
  return normalizedDateVector(pickerMode_, values, effectiveFirstDayOfWeek(), order_);
}

QStringList AdDatePicker::effectiveMultipleDisplayTexts(const QVector<QDate>& values) const {
  QStringList out;
  out.reserve(values.size());
  for (const QDate& value : values) {
    const QString text = displayTextCallback_
                             ? displayTextCallback_(value, time_)
                             : formatDefaultDate(value, pickerMode_, defaultDisplayFormat(),
                                                 locale_, effectiveFirstDayOfWeek());
    if (!text.isEmpty()) {
      out.append(text);
    }
  }
  return out;
}

QString AdDatePicker::effectiveDisplayText(const QDate& value) const {
  if (displayTextCallback_ && (value.isValid() || pickerMode_ == PickerMode::Time)) {
    return displayTextCallback_(value, time_);
  }
  return formatDefaultDateTime(value, time_, pickerMode_, defaultDisplayFormat(), locale_,
                               effectiveFirstDayOfWeek(), effectiveTextIncludesTime());
}

QString AdDatePicker::effectivePlaceholder() const {
  if (!placeholder_.isEmpty()) {
    return placeholder_;
  }
  switch (pickerMode_) {
    case PickerMode::Week:
      return tr("Select week");
    case PickerMode::Month:
      return tr("Select month");
    case PickerMode::Quarter:
      return tr("Select quarter");
    case PickerMode::Year:
      return tr("Select year");
    case PickerMode::Decade:
      return tr("Select decade");
    case PickerMode::Time:
      return tr("Select a time");
    case PickerMode::Date:
    default:
      return tr("Select date");
  }
}

QString AdDatePicker::defaultDisplayFormat() const {
  const QStringList formats = effectiveParseFormats();
  return formats.isEmpty() ? QString() : formats.first();
}

QStringList AdDatePicker::effectiveParseFormats() const {
  return effectiveFormatsForPicker(pickerMode_, displayFormat_, displayFormats_,
                                   effectiveTextIncludesTime(), effectiveTimeFormat(),
                                   effectiveUse12Hours());
}

bool AdDatePicker::effectiveTextIncludesTime() const {
  if (pickerMode_ == PickerMode::Time) {
    return true;
  }
  if (showTime_ && !multiple_) {
    return true;
  }
  if (!maskFormat_ || multiple_ || pickerMode_ != PickerMode::Date) {
    return false;
  }
  const QStringList formats = effectiveFormatsForPicker(
      pickerMode_, displayFormat_, displayFormats_, false, timeFormat_, use12Hours_);
  return !formats.isEmpty() && formatHasTimeToken(formats.first());
}

QString AdDatePicker::effectiveTimeFormat() const {
  const QString source = !timeFormat_.trimmed().isEmpty()
                             ? normalizeDateFormatSyntax(timeFormat_.trimmed())
                             : inferredTimeFormatFromDisplayFormat(displayFormat());
  return normalizedTimeFormat(source, use12Hours_ || formatUses12HourClock(source));
}

bool AdDatePicker::effectiveUse12Hours() const {
  if (use12Hours_) {
    return true;
  }
  const QString source = !timeFormat_.trimmed().isEmpty()
                             ? normalizeDateFormatSyntax(timeFormat_.trimmed())
                             : inferredTimeFormatFromDisplayFormat(displayFormat());
  return formatUses12HourClock(source);
}

bool AdDatePicker::effectiveShowSecondColumn() const {
  return showSecondExplicit_ ? showSecond_ : formatHasSecondToken(effectiveTimeFormat());
}

Qt::DayOfWeek AdDatePicker::effectiveFirstDayOfWeek() const {
  return firstDayOfWeekForLocale(locale_);
}

bool AdDatePicker::effectiveShowWeek() const {
  return showWeekExplicit_ ? showWeek_ : pickerMode_ == PickerMode::Week;
}

AdDatePicker::PickerMode AdDatePicker::normalizedPanelMode(PickerMode value) const {
  return normalizedPanelPickerMode(value);
}

AdDatePicker::PickerMode AdDatePicker::effectivePanelMode() const {
  return panelModeExplicit_ ? panelMode_ : normalizedPanelMode(pickerMode_);
}

QDateTime AdDatePicker::parseText(const QString& text, bool* ok) const {
  return parsePickerDateTimeText(pickerMode_, text, effectiveParseFormats(), locale_,
                                 effectiveFirstDayOfWeek(), time_, effectiveTextIncludesTime(), ok);
}

QDateTime AdDatePicker::parseMaskedText(const QString& text, bool* ok) const {
  if (ok) {
    *ok = false;
  }
  if (!maskFormat_ || pickerMode_ != PickerMode::Date || multiple_ || !textContainsDigit(text)) {
    return {};
  }
  bool parsedOk = false;
  QDateTime parsed = parseText(text, &parsedOk);
  if (!parsedOk || !parsed.isValid()) {
    parsed = alignMaskedDateTimeText(pickerMode_, text, defaultDisplayFormat(),
                                     effectiveFirstDayOfWeek(), time_, &parsedOk);
  }
  if (parsedOk && parsed.isValid() && ok) {
    *ok = true;
  }
  return parsed;
}

bool AdDatePicker::isDateSelectable(const QDate& value, const QDate& from) const {
  const QDate normalized = normalizeForPicker(pickerMode_, value, effectiveFirstDayOfWeek());
  if (!normalized.isValid()) {
    return false;
  }
  if (!pickerValueWithinBounds(pickerMode_, normalized, effectiveFirstDayOfWeek(), minDate_,
                               maxDate_)) {
    return false;
  }
  if (disabledDatePredicate_ && disabledDatePredicate_(normalized)) {
    return false;
  }
  if (disabledDateContextPredicate_) {
    DisabledDateContext context;
    context.from = normalizeForPicker(pickerMode_, from, effectiveFirstDayOfWeek());
    context.type = pickerMode_;
    if (disabledDateContextPredicate_(normalized, context)) {
      return false;
    }
  }
  return true;
}

bool AdDatePicker::isDateTimeSelectable(const QDateTime& value) const {
  if (!value.isValid() || !isDateSelectable(value.date())) {
    return false;
  }
  if (!showTime_ || !disabledTimePredicate_) {
    return true;
  }
  DisabledTimeContext context;
  context.part = TimeSelectionPart::Single;
  return !disabledTimePredicate_(
      normalizeForPicker(pickerMode_, value.date(), effectiveFirstDayOfWeek()),
      normalizedTimeValue(value.time()), context);
}

void AdDatePicker::handleControllerPopupVisibleChanged(bool value) {
  if (popupVisible_ == value) {
    return;
  }

  popupVisible_ = value;
  syncLineEditStyle();
  if (!popupVisible_) {
    clearPreviewText();
    syncLineEdit();
  } else {
    if (popupPanel_) {
      popupPanel_->setFocus(Qt::PopupFocusReason);
    }
  }
  emit popupVisibleChanged(popupVisible_);
}

QObject* AdDatePicker::popupOwnerObject() const { return const_cast<AdDatePicker*>(this); }

QWidget* AdDatePicker::popupAnchorWidget() const { return const_cast<AdDatePicker*>(this); }

QWidget* AdDatePicker::popupScopeWindow() const { return detail::resolvePopupScopeWindow(this); }

QWidget* AdDatePicker::popupSurfaceWidget() const { return popup_; }

QWidget* AdDatePicker::popupEnsureSurface() {
  ensurePopup();
  return popup_;
}

bool AdDatePicker::popupHasContent() const { return true; }

detail::OverlayPopupPlacement AdDatePicker::popupPlacement() const {
  return toOverlayPopupPlacement(placement_);
}

bool AdDatePicker::popupAutoAdjustOverflow() const { return true; }

bool AdDatePicker::popupArrowVisible() const { return false; }

bool AdDatePicker::popupArrowPointAtCenter() const { return false; }

int AdDatePicker::popupOffset() const {
  detail::DatePickerStyleInput input;
  input.size = size_;
  input.variant = variant_;
  input.status = status_;
  input.disabled = !isEnabled();
  input.baseFont = font();
  input.componentTokens = componentTokens_;
  input.semanticStyles = effectiveSemanticStyles().popup;
  const detail::DatePickerVisualStyle style = detail::resolveDatePickerVisualStyle(
      input, adqt::theme::ThemeManager::instance().resolve(this));
  return std::max(0, style.metrics.popupOffset);
}

int AdDatePicker::popupArrowOffsetHorizontal() const { return 0; }

int AdDatePicker::popupArrowOffsetVertical() const { return 0; }

void AdDatePicker::popupApplyResolvedPlacement(detail::OverlayPopupPlacement placement,
                                               qreal arrowCenterCoord) {
  Q_UNUSED(arrowCenterCoord)
  if (auto* surface = dynamic_cast<detail::OverlayPopupSurface*>(popup_)) {
    surface->setPlacement(placement);
  }
}

AdDateRangePicker::AdDateRangePicker(QWidget* parent) : QWidget(parent) {
  popupController_ = new detail::OverlayPopupController(this, this);
  popupController_->setVisibilityMode(detail::OverlayPopupController::VisibilityMode::External);
  popupController_->setTriggerModes(detail::OverlayPopupController::Triggers{});
  connect(popupController_, &detail::OverlayPopupController::popupVisibleChanged, this,
          &AdDateRangePicker::handleControllerPopupVisibleChanged);
  connect(popupController_, &detail::OverlayPopupController::popupVisibilityRequested, this,
          [this](bool value) {
            if (!value) {
              QScopedValueRollback<bool> closeGuard(suppressPopupCloseSubmit_, true);
              setPopupVisibleInternal(false, true);
            }
          });
  buildUi();
  popupController_->anchorWidgetChanged();
}

AdDateRangePicker::~AdDateRangePicker() {
  if (popupController_) {
    popupController_->setPopupVisible(false);
  }
  destroyPopup();
}

QDate AdDateRangePicker::startDate() const { return startDate_; }

void AdDateRangePicker::setStartDate(const QDate& value) { setRange(value, endDate_); }

QDate AdDateRangePicker::endDate() const { return endDate_; }

void AdDateRangePicker::setEndDate(const QDate& value) { setRange(startDate_, value); }

void AdDateRangePicker::setRange(const QDate& start, const QDate& end) {
  popupCalendarActive_ = false;
  activeRangeHistory_.clear();
  QDate nextStart = normalizeForPicker(pickerMode_, start, effectiveFirstDayOfWeek());
  QDate nextEnd = normalizeForPicker(pickerMode_, end, effectiveFirstDayOfWeek());
  if (order_ && !startDisabled_ && !endDisabled_ && nextStart.isValid() && nextEnd.isValid() &&
      nextEnd < nextStart) {
    std::swap(nextStart, nextEnd);
  }
  if (startDate_ == nextStart && endDate_ == nextEnd) {
    return;
  }
  startDate_ = nextStart;
  endDate_ = nextEnd;
  syncLineEdit();
  if (popupVisible_) {
    syncPanelState();
  }
  emit rangeChanged(startDate_, endDate_);
  emit dateTimeRangeChanged(startDateTime(), endDateTime());
}

void AdDateRangePicker::clear() { clearRangeInternal(true); }

QDateTime AdDateRangePicker::startDateTime() const {
  return dateTimeFromParts(startDate_, startTime_);
}

void AdDateRangePicker::setStartDateTime(const QDateTime& value) {
  setDateTimeRange(value, endDateTime());
}

QDateTime AdDateRangePicker::endDateTime() const { return dateTimeFromParts(endDate_, endTime_); }

void AdDateRangePicker::setEndDateTime(const QDateTime& value) {
  setDateTimeRange(startDateTime(), value);
}

void AdDateRangePicker::setDateTimeRange(const QDateTime& start, const QDateTime& end) {
  popupCalendarActive_ = false;
  activeRangeHistory_.clear();
  QDateTime nextStartValue = start;
  QDateTime nextEndValue = end;
  if (order_ && !startDisabled_ && !endDisabled_ && nextStartValue.isValid() &&
      nextEndValue.isValid() && nextEndValue < nextStartValue) {
    std::swap(nextStartValue, nextEndValue);
  }

  QDate nextStartDate =
      nextStartValue.isValid()
          ? normalizeForPicker(pickerMode_, nextStartValue.date(), effectiveFirstDayOfWeek())
          : QDate();
  QDate nextEndDate = nextEndValue.isValid() ? normalizeForPicker(pickerMode_, nextEndValue.date(),
                                                                  effectiveFirstDayOfWeek())
                                             : QDate();
  QTime nextStartTime =
      nextStartValue.isValid() ? normalizedTimeValue(nextStartValue.time()) : defaultTimeValue();
  QTime nextEndTime =
      nextEndValue.isValid() ? normalizedTimeValue(nextEndValue.time()) : defaultTimeValue();

  const QDateTime normalizedStart = dateTimeFromParts(nextStartDate, nextStartTime);
  const QDateTime normalizedEnd = dateTimeFromParts(nextEndDate, nextEndTime);
  if (order_ && !startDisabled_ && !endDisabled_ && normalizedStart.isValid() &&
      normalizedEnd.isValid() && normalizedEnd < normalizedStart) {
    std::swap(nextStartDate, nextEndDate);
    std::swap(nextStartTime, nextEndTime);
  }

  const bool datesChanged = startDate_ != nextStartDate || endDate_ != nextEndDate;
  const bool timesChanged = startTime_ != nextStartTime || endTime_ != nextEndTime;
  if (!datesChanged && !timesChanged) {
    return;
  }

  startDate_ = nextStartDate;
  endDate_ = nextEndDate;
  startTime_ = nextStartTime;
  endTime_ = nextEndTime;
  syncLineEdit();
  if (popupVisible_) {
    syncPanelState();
  }
  if (datesChanged) {
    emit rangeChanged(startDate_, endDate_);
  }
  if (timesChanged) {
    emit timeRangeChanged(startTime_, endTime_);
  }
  emit dateTimeRangeChanged(startDateTime(), endDateTime());
}

QTime AdDateRangePicker::startTime() const { return startTime_; }

void AdDateRangePicker::setStartTime(const QTime& value) { setTimeRange(value, endTime_); }

QTime AdDateRangePicker::endTime() const { return endTime_; }

void AdDateRangePicker::setEndTime(const QTime& value) { setTimeRange(startTime_, value); }

void AdDateRangePicker::setTimeRange(const QTime& start, const QTime& end) {
  QTime nextStart = normalizedTimeValue(start);
  QTime nextEnd = normalizedTimeValue(end);
  if (order_ && nextStart.isValid() && nextEnd.isValid() && nextEnd < nextStart) {
    std::swap(nextStart, nextEnd);
  }
  QDate nextStartDate = startDate_;
  QDate nextEndDate = endDate_;
  if (pickerMode_ == PickerMode::Time) {
    const QDate materializedDate = todayDate();
    if (!nextStartDate.isValid()) {
      nextStartDate = materializedDate;
    }
    if (!nextEndDate.isValid()) {
      nextEndDate = materializedDate;
    }
  }
  const bool datesChanged = startDate_ != nextStartDate || endDate_ != nextEndDate;
  const bool timesChanged = startTime_ != nextStart || endTime_ != nextEnd;
  if (!datesChanged && !timesChanged) {
    return;
  }
  startDate_ = nextStartDate;
  endDate_ = nextEndDate;
  startTime_ = nextStart;
  endTime_ = nextEnd;
  syncLineEdit();
  if (popupVisible_) {
    syncPanelState();
  }
  if (datesChanged) {
    emit rangeChanged(startDate_, endDate_);
  }
  if (timesChanged) {
    emit timeRangeChanged(startTime_, endTime_);
  }
  emit dateTimeRangeChanged(startDateTime(), endDateTime());
}

AdDateRangePicker::PickerMode AdDateRangePicker::pickerMode() const { return pickerMode_; }

void AdDateRangePicker::setPickerMode(PickerMode value) {
  if (pickerMode_ == value) {
    return;
  }
  const QDate previousStart = startDate_;
  const QDate previousEnd = endDate_;
  const QTime previousStartTime = startTime_;
  const QTime previousEndTime = endTime_;
  const PickerMode previousPanelMode = effectivePanelMode();
  pickerMode_ = value;
  startDate_ = normalizeForPicker(pickerMode_, startDate_, effectiveFirstDayOfWeek());
  endDate_ = normalizeForPicker(pickerMode_, endDate_, effectiveFirstDayOfWeek());
  syncLineEditMask();
  syncLineEditStyle();
  syncLineEdit();
  syncPanelState();
  emit pickerModeChanged(pickerMode_);
  if (effectivePanelMode() != previousPanelMode) {
    emit panelModeChanged(effectivePanelMode());
  }
  if (startDate_ != previousStart || endDate_ != previousEnd) {
    emit rangeChanged(startDate_, endDate_);
  }
  if (startTime_ != previousStartTime || endTime_ != previousEndTime) {
    emit timeRangeChanged(startTime_, endTime_);
  }
  if (startDate_ != previousStart || endDate_ != previousEnd || startTime_ != previousStartTime ||
      endTime_ != previousEndTime) {
    emit dateTimeRangeChanged(startDateTime(), endDateTime());
  }
}

AdDateRangePicker::Size AdDateRangePicker::size() const { return size_; }

void AdDateRangePicker::setSize(Size value) {
  if (size_ == value) {
    return;
  }
  size_ = value;
  syncLineEditStyle();
  emit sizeChanged(size_);
}

AdDateRangePicker::Variant AdDateRangePicker::variant() const { return variant_; }

void AdDateRangePicker::setVariant(Variant value) {
  if (variant_ == value) {
    return;
  }
  variant_ = value;
  syncLineEditStyle();
  emit variantChanged(variant_);
}

AdDateRangePicker::Status AdDateRangePicker::status() const { return status_; }

void AdDateRangePicker::setStatus(Status value) {
  if (status_ == value) {
    return;
  }
  status_ = value;
  syncLineEditStyle();
  emit statusChanged(status_);
}

bool AdDateRangePicker::allowClear() const { return allowClear_; }

void AdDateRangePicker::setAllowClear(bool value) {
  if (allowClear_ == value) {
    return;
  }
  allowClear_ = value;
  if (lineEdit_) {
    lineEdit_->setAllowClear(allowClear_);
  }
  emit allowClearChanged(allowClear_);
}

bool AdDateRangePicker::inputReadOnly() const { return inputReadOnly_; }

void AdDateRangePicker::setInputReadOnly(bool value) {
  if (inputReadOnly_ == value) {
    return;
  }
  inputReadOnly_ = value;
  if (lineEdit_) {
    lineEdit_->setReadOnly(inputReadOnly_);
  }
  emit inputReadOnlyChanged(inputReadOnly_);
}

QString AdDateRangePicker::id() const { return id_; }

void AdDateRangePicker::setId(const QString& value) {
  const QString nextId = normalizedInputId(value);
  if (id_ == nextId) {
    return;
  }
  id_ = nextId;
  syncInputIds();
  emit idChanged(id_);
}

QString AdDateRangePicker::startId() const { return startId_; }

void AdDateRangePicker::setStartId(const QString& value) {
  const QString nextId = normalizedInputId(value);
  if (startId_ == nextId) {
    return;
  }
  startId_ = nextId;
  syncInputIds();
  emit startIdChanged(startId_);
  emit rangeIdsChanged(startId_, endId_);
}

QString AdDateRangePicker::endId() const { return endId_; }

void AdDateRangePicker::setEndId(const QString& value) {
  const QString nextId = normalizedInputId(value);
  if (endId_ == nextId) {
    return;
  }
  endId_ = nextId;
  syncInputIds();
  emit endIdChanged(endId_);
  emit rangeIdsChanged(startId_, endId_);
}

void AdDateRangePicker::setRangeIds(const QString& start, const QString& end) {
  const QString nextStartId = normalizedInputId(start);
  const QString nextEndId = normalizedInputId(end);
  const bool changedStart = startId_ != nextStartId;
  const bool changedEnd = endId_ != nextEndId;
  if (!changedStart && !changedEnd) {
    return;
  }

  startId_ = nextStartId;
  endId_ = nextEndId;
  syncInputIds();
  if (changedStart) {
    emit startIdChanged(startId_);
  }
  if (changedEnd) {
    emit endIdChanged(endId_);
  }
  emit rangeIdsChanged(startId_, endId_);
}

AdDateRangePicker::PreviewValue AdDateRangePicker::previewValue() const { return previewValue_; }

void AdDateRangePicker::setPreviewValue(PreviewValue value) {
  if (previewValue_ == value) {
    return;
  }
  previewValue_ = value;
  if (previewValue_ == PreviewValue::Disabled) {
    clearPreviewText();
  }
  emit previewValueChanged(previewValue_);
}

bool AdDateRangePicker::popupVisible() const { return popupVisible_; }

void AdDateRangePicker::setPopupVisible(bool value) { setPopupVisibleInternal(value, true); }

void AdDateRangePicker::showPopup() { setPopupVisible(true); }

void AdDateRangePicker::hidePopup() { setPopupVisible(false); }

bool AdDateRangePicker::defaultOpen() const { return defaultOpen_; }

void AdDateRangePicker::setDefaultOpen(bool value) {
  if (defaultOpen_ == value) {
    return;
  }
  defaultOpen_ = value;
  defaultOpenApplied_ = false;
  if (defaultOpen_ && isVisible()) {
    QTimer::singleShot(0, this, [this]() {
      if (defaultOpen_ && !defaultOpenApplied_ && isVisible()) {
        defaultOpenApplied_ = true;
        showPopup();
      }
    });
  }
  emit defaultOpenChanged(defaultOpen_);
}

QDate AdDateRangePicker::defaultPickerValue() const { return defaultPickerValue_; }

void AdDateRangePicker::setDefaultPickerValue(const QDate& value) {
  if (defaultPickerValue_ == value) {
    return;
  }
  defaultPickerValue_ = value;
  if (!popupVisible_ && !pickerValue_.isValid()) {
    syncPanelState();
  }
  emit defaultPickerValueChanged(defaultPickerValue_);
}

QDate AdDateRangePicker::pickerValue() const { return pickerValue_; }

void AdDateRangePicker::setPickerValue(const QDate& value) {
  if (pickerValue_ == value) {
    return;
  }
  pickerValue_ = value;
  syncPanelState();
  emit pickerValueChanged(pickerValue_);
}

AdDateRangePicker::PickerMode AdDateRangePicker::panelMode() const { return effectivePanelMode(); }

void AdDateRangePicker::setPanelMode(PickerMode value) {
  const PickerMode normalized = normalizedPanelMode(value);
  if (panelModeExplicit_ && panelMode_ == normalized) {
    return;
  }
  panelMode_ = normalized;
  panelModeExplicit_ = true;
  syncPanelState();
  emit panelModeChanged(panelMode_);
}

bool AdDateRangePicker::order() const { return order_; }

void AdDateRangePicker::setOrder(bool value) {
  if (order_ == value) {
    return;
  }

  const QDate previousStartDate = startDate_;
  const QDate previousEndDate = endDate_;
  const QTime previousStartTime = startTime_;
  const QTime previousEndTime = endTime_;

  order_ = value;
  if (order_) {
    const QDateTime start = startDateTime();
    const QDateTime end = endDateTime();
    if (start.isValid() && end.isValid() && end < start) {
      std::swap(startDate_, endDate_);
      std::swap(startTime_, endTime_);
    }
  }

  const bool datesChanged = startDate_ != previousStartDate || endDate_ != previousEndDate;
  const bool timesChanged = startTime_ != previousStartTime || endTime_ != previousEndTime;
  syncLineEdit();
  syncPanelState();
  emit orderChanged(order_);
  if (datesChanged) {
    emit rangeChanged(startDate_, endDate_);
  }
  if (timesChanged) {
    emit timeRangeChanged(startTime_, endTime_);
  }
  if (datesChanged || timesChanged) {
    emit dateTimeRangeChanged(startDateTime(), endDateTime());
  }
}

bool AdDateRangePicker::needConfirm() const { return needConfirm_; }

void AdDateRangePicker::setNeedConfirm(bool value) {
  if (needConfirm_ == value) {
    return;
  }
  needConfirm_ = value;
  syncPanelState();
  emit needConfirmChanged(needConfirm_);
}

bool AdDateRangePicker::showTime() const { return showTime_; }

void AdDateRangePicker::setShowTime(bool value) {
  if (showTime_ == value) {
    return;
  }
  showTime_ = value;
  syncLineEditMask();
  syncLineEdit();
  syncPanelState();
  syncPopupGeometry();
  emit showTimeChanged(showTime_);
}

bool AdDateRangePicker::showToday() const { return showToday_; }

void AdDateRangePicker::setShowToday(bool value) {
  const bool explicitChanged = !showTodayExplicit_;
  showTodayExplicit_ = true;
  if (showToday_ == value && !explicitChanged) {
    return;
  }
  const bool valueChanged = showToday_ != value;
  showToday_ = value;
  refreshRangeFooter();
  syncPopupGeometry();
  if (valueChanged) {
    emit showTodayChanged(showToday_);
  }
}

bool AdDateRangePicker::showNow() const { return showNow_; }

void AdDateRangePicker::setShowNow(bool value) {
  const bool explicitChanged = !showNowExplicit_;
  showNowExplicit_ = true;
  if (showNow_ == value && !explicitChanged) {
    return;
  }
  const bool valueChanged = showNow_ != value;
  showNow_ = value;
  refreshRangeFooter();
  syncPopupGeometry();
  if (valueChanged) {
    emit showNowChanged(showNow_);
  }
}

QTime AdDateRangePicker::defaultOpenStartTime() const { return defaultOpenStartTime_; }

void AdDateRangePicker::setDefaultOpenStartTime(const QTime& value) {
  const QTime normalized = normalizedTimeValue(value);
  if (defaultOpenStartTime_ == normalized) {
    return;
  }
  defaultOpenStartTime_ = normalized;
  syncPanelState();
  emit defaultOpenTimeRangeChanged(defaultOpenStartTime_, defaultOpenEndTime_);
}

QTime AdDateRangePicker::defaultOpenEndTime() const { return defaultOpenEndTime_; }

void AdDateRangePicker::setDefaultOpenEndTime(const QTime& value) {
  const QTime normalized = normalizedTimeValue(value);
  if (defaultOpenEndTime_ == normalized) {
    return;
  }
  defaultOpenEndTime_ = normalized;
  syncPanelState();
  emit defaultOpenTimeRangeChanged(defaultOpenStartTime_, defaultOpenEndTime_);
}

void AdDateRangePicker::setDefaultOpenTimeRange(const QTime& start, const QTime& end) {
  const QTime normalizedStart = normalizedTimeValue(start);
  const QTime normalizedEnd = normalizedTimeValue(end);
  if (defaultOpenStartTime_ == normalizedStart && defaultOpenEndTime_ == normalizedEnd) {
    return;
  }
  defaultOpenStartTime_ = normalizedStart;
  defaultOpenEndTime_ = normalizedEnd;
  syncPanelState();
  emit defaultOpenTimeRangeChanged(defaultOpenStartTime_, defaultOpenEndTime_);
}

QString AdDateRangePicker::timeFormat() const { return effectiveTimeFormat(); }

void AdDateRangePicker::setTimeFormat(const QString& value) {
  const QString normalized = normalizeDateFormatSyntax(value.trimmed());
  if (timeFormat_ == normalized) {
    return;
  }
  const bool previousShowSecond = effectiveShowSecondColumn();
  timeFormat_ = normalized;
  syncLineEditMask();
  syncLineEdit();
  syncPanelState();
  syncPopupGeometry();
  emit timeFormatChanged(effectiveTimeFormat());
  if (previousShowSecond != effectiveShowSecondColumn()) {
    emit showSecondChanged(effectiveShowSecondColumn());
  }
}

int AdDateRangePicker::hourStep() const { return hourStep_; }

void AdDateRangePicker::setHourStep(int value) {
  const int normalized = normalizedTimeStep(value, 24);
  if (hourStep_ == normalized) {
    return;
  }
  hourStep_ = normalized;
  syncPanelState();
  syncPopupGeometry();
  emit timeStepChanged(hourStep_, minuteStep_, secondStep_);
}

int AdDateRangePicker::minuteStep() const { return minuteStep_; }

void AdDateRangePicker::setMinuteStep(int value) {
  const int normalized = normalizedTimeStep(value, 60);
  if (minuteStep_ == normalized) {
    return;
  }
  minuteStep_ = normalized;
  syncPanelState();
  syncPopupGeometry();
  emit timeStepChanged(hourStep_, minuteStep_, secondStep_);
}

int AdDateRangePicker::secondStep() const { return secondStep_; }

void AdDateRangePicker::setSecondStep(int value) {
  const int normalized = normalizedTimeStep(value, 60);
  if (secondStep_ == normalized) {
    return;
  }
  secondStep_ = normalized;
  syncPanelState();
  syncPopupGeometry();
  emit timeStepChanged(hourStep_, minuteStep_, secondStep_);
}

void AdDateRangePicker::setTimeSteps(int hourStep, int minuteStep, int secondStep) {
  const int nextHourStep = normalizedTimeStep(hourStep, 24);
  const int nextMinuteStep = normalizedTimeStep(minuteStep, 60);
  const int nextSecondStep = normalizedTimeStep(secondStep, 60);
  if (hourStep_ == nextHourStep && minuteStep_ == nextMinuteStep && secondStep_ == nextSecondStep) {
    return;
  }
  hourStep_ = nextHourStep;
  minuteStep_ = nextMinuteStep;
  secondStep_ = nextSecondStep;
  syncPanelState();
  syncPopupGeometry();
  emit timeStepChanged(hourStep_, minuteStep_, secondStep_);
}

bool AdDateRangePicker::hideDisabledOptions() const { return hideDisabledOptions_; }

void AdDateRangePicker::setHideDisabledOptions(bool value) {
  if (hideDisabledOptions_ == value) {
    return;
  }
  hideDisabledOptions_ = value;
  syncPanelState();
  syncPopupGeometry();
  emit hideDisabledOptionsChanged(hideDisabledOptions_);
}

bool AdDateRangePicker::use12Hours() const { return use12Hours_; }

void AdDateRangePicker::setUse12Hours(bool value) {
  if (use12Hours_ == value) {
    return;
  }
  use12Hours_ = value;
  syncLineEditMask();
  syncLineEdit();
  syncPanelState();
  syncPopupGeometry();
  emit use12HoursChanged(use12Hours_);
}

bool AdDateRangePicker::changeOnScroll() const { return changeOnScroll_; }

void AdDateRangePicker::setChangeOnScroll(bool value) {
  if (changeOnScroll_ == value) {
    return;
  }
  changeOnScroll_ = value;
  syncPanelState();
  emit changeOnScrollChanged(changeOnScroll_);
}

bool AdDateRangePicker::showHour() const { return showHour_; }

void AdDateRangePicker::setShowHour(bool value) {
  if (showHour_ == value) {
    return;
  }
  showHour_ = value;
  syncPanelState();
  syncPopupGeometry();
  emit showHourChanged(showHour_);
}

bool AdDateRangePicker::showMinute() const { return showMinute_; }

void AdDateRangePicker::setShowMinute(bool value) {
  if (showMinute_ == value) {
    return;
  }
  showMinute_ = value;
  syncPanelState();
  syncPopupGeometry();
  emit showMinuteChanged(showMinute_);
}

bool AdDateRangePicker::showSecond() const { return effectiveShowSecondColumn(); }

void AdDateRangePicker::setShowSecond(bool value) {
  const bool previous = effectiveShowSecondColumn();
  if (showSecondExplicit_ && showSecond_ == value) {
    return;
  }
  showSecond_ = value;
  showSecondExplicit_ = true;
  syncPanelState();
  syncPopupGeometry();
  if (previous != effectiveShowSecondColumn()) {
    emit showSecondChanged(effectiveShowSecondColumn());
  }
}

void AdDateRangePicker::resetShowSecond() {
  if (!showSecondExplicit_) {
    return;
  }
  const bool previous = effectiveShowSecondColumn();
  showSecond_ = true;
  showSecondExplicit_ = false;
  syncPanelState();
  syncPopupGeometry();
  if (previous != effectiveShowSecondColumn()) {
    emit showSecondChanged(effectiveShowSecondColumn());
  }
}

void AdDateRangePicker::setVisibleTimeColumns(bool hour, bool minute, bool second) {
  const bool previousHour = showHour_;
  const bool previousMinute = showMinute_;
  const bool previousSecond = effectiveShowSecondColumn();
  if (showHour_ == hour && showMinute_ == minute && showSecondExplicit_ && showSecond_ == second) {
    return;
  }
  showHour_ = hour;
  showMinute_ = minute;
  showSecond_ = second;
  showSecondExplicit_ = true;
  syncPanelState();
  syncPopupGeometry();
  if (previousHour != showHour_) {
    emit showHourChanged(showHour_);
  }
  if (previousMinute != showMinute_) {
    emit showMinuteChanged(showMinute_);
  }
  if (previousSecond != effectiveShowSecondColumn()) {
    emit showSecondChanged(effectiveShowSecondColumn());
  }
}

bool AdDateRangePicker::allowEmptyStart() const { return allowEmptyStart_; }

void AdDateRangePicker::setAllowEmptyStart(bool value) {
  if (allowEmptyStart_ == value) {
    return;
  }
  allowEmptyStart_ = value;
  syncPanelState();
  emit allowEmptyStartChanged(allowEmptyStart_);
}

bool AdDateRangePicker::allowEmptyEnd() const { return allowEmptyEnd_; }

void AdDateRangePicker::setAllowEmptyEnd(bool value) {
  if (allowEmptyEnd_ == value) {
    return;
  }
  allowEmptyEnd_ = value;
  syncPanelState();
  emit allowEmptyEndChanged(allowEmptyEnd_);
}

void AdDateRangePicker::setAllowEmpty(bool start, bool end) {
  setAllowEmptyStart(start);
  setAllowEmptyEnd(end);
}

bool AdDateRangePicker::startDisabled() const { return startDisabled_; }

void AdDateRangePicker::setStartDisabled(bool value) {
  if (startDisabled_ == value) {
    return;
  }
  startDisabled_ = value;
  if (effectiveInputDisabled()) {
    hidePopup();
  }
  syncLineEditStyle();
  panelDisabledDateContextPredicateDirty_ = true;
  if (popupVisible_) {
    syncPanelState();
  }
  emit startDisabledChanged(startDisabled_);
}

bool AdDateRangePicker::endDisabled() const { return endDisabled_; }

void AdDateRangePicker::setEndDisabled(bool value) {
  if (endDisabled_ == value) {
    return;
  }
  endDisabled_ = value;
  if (effectiveInputDisabled()) {
    hidePopup();
  }
  syncLineEditStyle();
  panelDisabledDateContextPredicateDirty_ = true;
  if (popupVisible_) {
    syncPanelState();
  }
  emit endDisabledChanged(endDisabled_);
}

void AdDateRangePicker::setDisabledRange(bool start, bool end) {
  setStartDisabled(start);
  setEndDisabled(end);
}

bool AdDateRangePicker::disabled() const { return !isEnabled(); }

void AdDateRangePicker::setDisabled(bool value) {
  if (disabled() == value) {
    return;
  }
  if (value && popupVisible_) {
    setPopupVisibleInternal(false, true);
  }
  QWidget::setDisabled(value);
  syncLineEditStyle();
  syncPanelState();
  emit disabledChanged(value);
}

QDate AdDateRangePicker::minDate() const { return minDate_; }

void AdDateRangePicker::setMinDate(const QDate& value) {
  if (minDate_ == value) {
    return;
  }
  minDate_ = value;
  syncPanelState();
  emit minDateChanged(minDate_);
}

QDate AdDateRangePicker::maxDate() const { return maxDate_; }

void AdDateRangePicker::setMaxDate(const QDate& value) {
  if (maxDate_ == value) {
    return;
  }
  maxDate_ = value;
  syncPanelState();
  emit maxDateChanged(maxDate_);
}

QString AdDateRangePicker::displayFormat() const {
  return displayFormats_.isEmpty() ? displayFormat_ : displayFormats_.first();
}

void AdDateRangePicker::setDisplayFormat(const QString& value) {
  const QString normalized = normalizeDateFormatSyntax(value.trimmed());
  if (displayFormat_ == normalized && displayFormats_.isEmpty()) {
    return;
  }
  const QString previousDisplay = displayFormat();
  const QStringList previousFormats = displayFormats_;
  const bool previousMaskFormat = maskFormat_;
  const QString previousTimeFormat = effectiveTimeFormat();
  const bool previousShowSecond = effectiveShowSecondColumn();
  displayFormat_ = normalized;
  displayFormats_.clear();
  if (maskFormat_ && displayFormat_.isEmpty()) {
    maskFormat_ = false;
  }
  syncLineEditMask();
  syncLineEdit();
  syncPanelState();
  syncPopupGeometry();
  if (displayFormat() != previousDisplay) {
    emit displayFormatChanged(displayFormat());
  }
  if (displayFormats_ != previousFormats) {
    emit displayFormatsChanged(displayFormats_);
  }
  if (maskFormat_ != previousMaskFormat) {
    emit maskFormatChanged(maskFormat_);
  }
  if (previousTimeFormat != effectiveTimeFormat()) {
    emit timeFormatChanged(effectiveTimeFormat());
  }
  if (previousShowSecond != effectiveShowSecondColumn()) {
    emit showSecondChanged(effectiveShowSecondColumn());
  }
}

QStringList AdDateRangePicker::displayFormats() const { return displayFormats_; }

void AdDateRangePicker::setDisplayFormats(const QStringList& values) {
  const QStringList normalized = normalizeDateFormatSyntax(normalizedFormats(values));
  if (displayFormats_ == normalized) {
    return;
  }

  const QString previousDisplay = displayFormat();
  const bool previousMaskFormat = maskFormat_;
  const QString previousTimeFormat = effectiveTimeFormat();
  const bool previousShowSecond = effectiveShowSecondColumn();
  displayFormats_ = normalized;
  if (maskFormat_ && displayFormats_.isEmpty()) {
    maskFormat_ = false;
  }
  syncLineEditMask();
  syncLineEdit();
  syncPanelState();
  syncPopupGeometry();
  emit displayFormatsChanged(displayFormats_);
  if (displayFormat() != previousDisplay) {
    emit displayFormatChanged(displayFormat());
  }
  if (maskFormat_ != previousMaskFormat) {
    emit maskFormatChanged(maskFormat_);
  }
  if (previousTimeFormat != effectiveTimeFormat()) {
    emit timeFormatChanged(effectiveTimeFormat());
  }
  if (previousShowSecond != effectiveShowSecondColumn()) {
    emit showSecondChanged(effectiveShowSecondColumn());
  }
}

bool AdDateRangePicker::maskFormat() const { return maskFormat_; }

void AdDateRangePicker::setMaskFormat(bool value) {
  if (maskFormat_ == value) {
    return;
  }
  maskFormat_ = value;
  if (maskFormat_ && displayFormat().trimmed().isEmpty()) {
    displayFormat_ = defaultDisplayFormat();
  }
  syncLineEditMask();
  syncLineEdit();
  emit maskFormatChanged(maskFormat_);
}

bool AdDateRangePicker::preserveInvalidOnBlur() const { return preserveInvalidOnBlur_; }

void AdDateRangePicker::setPreserveInvalidOnBlur(bool value) {
  if (preserveInvalidOnBlur_ == value) {
    return;
  }
  preserveInvalidOnBlur_ = value;
  emit preserveInvalidOnBlurChanged(preserveInvalidOnBlur_);
}

QLocale AdDateRangePicker::locale() const { return locale_; }

void AdDateRangePicker::setLocale(const QLocale& value) {
  if (locale_ == value) {
    return;
  }

  const QDate previousStart = startDate_;
  const QDate previousEnd = endDate_;
  locale_ = value;
  if (!applyingGlobalLocale_) {
    localeExplicit_ = true;
  }
  startDate_ = normalizeForPicker(pickerMode_, startDate_, effectiveFirstDayOfWeek());
  endDate_ = normalizeForPicker(pickerMode_, endDate_, effectiveFirstDayOfWeek());
  if (order_ && startDate_.isValid() && endDate_.isValid() && endDate_ < startDate_) {
    std::swap(startDate_, endDate_);
    std::swap(startTime_, endTime_);
  }

  syncLineEdit();
  syncPanelState();
  emit localeChanged(locale_);
  if (startDate_ != previousStart || endDate_ != previousEnd) {
    emit rangeChanged(startDate_, endDate_);
    emit dateTimeRangeChanged(startDateTime(), endDateTime());
  }
}

QString AdDateRangePicker::placeholder() const { return placeholder_; }

void AdDateRangePicker::setPlaceholder(const QString& value) {
  if (placeholder_ == value && startPlaceholder_.isEmpty() && endPlaceholder_.isEmpty()) {
    return;
  }
  const bool hadStartPlaceholder = !startPlaceholder_.isEmpty();
  const bool hadEndPlaceholder = !endPlaceholder_.isEmpty();
  placeholder_ = value;
  startPlaceholder_.clear();
  endPlaceholder_.clear();
  syncLineEdit();
  emit placeholderChanged(placeholder_);
  if (hadStartPlaceholder) {
    emit startPlaceholderChanged(startPlaceholder_);
  }
  if (hadEndPlaceholder) {
    emit endPlaceholderChanged(endPlaceholder_);
  }
  if (hadStartPlaceholder || hadEndPlaceholder) {
    emit rangePlaceholdersChanged(startPlaceholder_, endPlaceholder_);
  }
}

QString AdDateRangePicker::startPlaceholder() const { return startPlaceholder_; }

void AdDateRangePicker::setStartPlaceholder(const QString& value) {
  if (placeholder_.isEmpty() && startPlaceholder_ == value) {
    return;
  }
  const bool hadFullPlaceholder = !placeholder_.isEmpty();
  placeholder_.clear();
  startPlaceholder_ = value;
  syncLineEdit();
  if (hadFullPlaceholder) {
    emit placeholderChanged(placeholder_);
  }
  emit startPlaceholderChanged(startPlaceholder_);
  emit rangePlaceholdersChanged(startPlaceholder_, endPlaceholder_);
}

QString AdDateRangePicker::endPlaceholder() const { return endPlaceholder_; }

void AdDateRangePicker::setEndPlaceholder(const QString& value) {
  if (placeholder_.isEmpty() && endPlaceholder_ == value) {
    return;
  }
  const bool hadFullPlaceholder = !placeholder_.isEmpty();
  placeholder_.clear();
  endPlaceholder_ = value;
  syncLineEdit();
  if (hadFullPlaceholder) {
    emit placeholderChanged(placeholder_);
  }
  emit endPlaceholderChanged(endPlaceholder_);
  emit rangePlaceholdersChanged(startPlaceholder_, endPlaceholder_);
}

void AdDateRangePicker::setRangePlaceholders(const QString& start, const QString& end) {
  if (placeholder_.isEmpty() && startPlaceholder_ == start && endPlaceholder_ == end) {
    return;
  }
  const bool hadFullPlaceholder = !placeholder_.isEmpty();
  const bool startChanged = startPlaceholder_ != start;
  const bool endChanged = endPlaceholder_ != end;
  placeholder_.clear();
  startPlaceholder_ = start;
  endPlaceholder_ = end;
  syncLineEdit();
  if (hadFullPlaceholder) {
    emit placeholderChanged(placeholder_);
  }
  if (startChanged || hadFullPlaceholder) {
    emit startPlaceholderChanged(startPlaceholder_);
  }
  if (endChanged || hadFullPlaceholder) {
    emit endPlaceholderChanged(endPlaceholder_);
  }
  emit rangePlaceholdersChanged(startPlaceholder_, endPlaceholder_);
}

QString AdDateRangePicker::separator() const { return effectiveSeparator(); }

void AdDateRangePicker::setSeparator(const QString& value) {
  if (separator_ == value) {
    return;
  }
  separator_ = value;
  syncLineEdit();
  emit separatorChanged(effectiveSeparator());
}

QString AdDateRangePicker::prefixText() const { return prefixText_; }

void AdDateRangePicker::setPrefixText(const QString& value) {
  if (prefixText_ == value) {
    return;
  }
  prefixText_ = value;
  syncLineEditStyle();
  emit prefixTextChanged(prefixText_);
}

QString AdDateRangePicker::suffixText() const { return suffixText_; }

void AdDateRangePicker::setSuffixText(const QString& value) {
  if (suffixText_ == value) {
    return;
  }
  suffixText_ = value;
  syncLineEditStyle();
  emit suffixTextChanged(suffixText_);
}

adqt::icons::IconRef AdDateRangePicker::prefixIconRef() const { return prefixIconRef_; }

void AdDateRangePicker::setPrefixIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(prefixIconRef_, value)) {
    return;
  }
  prefixIconRef_ = value;
  syncLineEditStyle();
  emit prefixIconRefChanged(prefixIconRef_);
}

adqt::icons::IconRef AdDateRangePicker::suffixIconRef() const { return suffixIconRef_; }

void AdDateRangePicker::setSuffixIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(suffixIconRef_, value)) {
    return;
  }
  suffixIconRef_ = value;
  syncLineEditStyle();
  emit suffixIconRefChanged(suffixIconRef_);
}

adqt::icons::IconRef AdDateRangePicker::feedbackIconRef() const { return feedbackIconRef_; }

void AdDateRangePicker::setFeedbackIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(feedbackIconRef_, value)) {
    return;
  }
  feedbackIconRef_ = value;
  syncLineEditStyle();
  emit feedbackIconRefChanged(feedbackIconRef_);
}

bool AdDateRangePicker::suffixIconVisible() const { return suffixIconVisible_; }

void AdDateRangePicker::setSuffixIconVisible(bool value) {
  if (suffixIconVisible_ == value) {
    return;
  }
  suffixIconVisible_ = value;
  syncLineEditStyle();
  emit suffixIconVisibleChanged(suffixIconVisible_);
}

adqt::icons::IconRef AdDateRangePicker::clearIconRef() const { return clearIconRef_; }

void AdDateRangePicker::setClearIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(clearIconRef_, value)) {
    return;
  }
  clearIconRef_ = value;
  syncLineEditStyle();
  emit clearIconRefChanged(clearIconRef_);
}

adqt::icons::IconRef AdDateRangePicker::prevIconRef() const { return prevIconRef_; }

void AdDateRangePicker::setPrevIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(prevIconRef_, value)) {
    return;
  }
  prevIconRef_ = value;
  syncPanelState();
  emit prevIconRefChanged(prevIconRef_);
}

adqt::icons::IconRef AdDateRangePicker::nextIconRef() const { return nextIconRef_; }

void AdDateRangePicker::setNextIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(nextIconRef_, value)) {
    return;
  }
  nextIconRef_ = value;
  syncPanelState();
  emit nextIconRefChanged(nextIconRef_);
}

adqt::icons::IconRef AdDateRangePicker::superPrevIconRef() const { return superPrevIconRef_; }

void AdDateRangePicker::setSuperPrevIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(superPrevIconRef_, value)) {
    return;
  }
  superPrevIconRef_ = value;
  syncPanelState();
  emit superPrevIconRefChanged(superPrevIconRef_);
}

adqt::icons::IconRef AdDateRangePicker::superNextIconRef() const { return superNextIconRef_; }

void AdDateRangePicker::setSuperNextIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(superNextIconRef_, value)) {
    return;
  }
  superNextIconRef_ = value;
  syncPanelState();
  emit superNextIconRefChanged(superNextIconRef_);
}

AdDateRangePicker::Placement AdDateRangePicker::placement() const { return placement_; }

void AdDateRangePicker::setPlacement(Placement value) {
  if (placement_ == value) {
    return;
  }
  placement_ = value;
  syncPopupGeometry();
  emit placementChanged(placement_);
}

AdDateRangePicker::PopupLayerMode AdDateRangePicker::popupLayerMode() const {
  return popupLayerMode_;
}

void AdDateRangePicker::setPopupLayerMode(PopupLayerMode value) {
  if (popupLayerMode_ == value) {
    return;
  }
  const bool wasVisible = popupVisible_;
  if (wasVisible) {
    setPopupVisibleInternal(false, true);
  }
  popupLayerMode_ = value;
  applyPopupLayerMode();
  if (wasVisible) {
    setPopupVisibleInternal(true, true);
  }
  emit popupLayerModeChanged(popupLayerMode_);
}

AdDateRangePicker::ComponentTokens AdDateRangePicker::componentTokens() const {
  return componentTokens_;
}

void AdDateRangePicker::setComponentTokens(const ComponentTokens& tokens) {
  if (panelComponentTokensEqual(componentTokens_, tokens)) {
    return;
  }
  componentTokens_ = tokens;
  syncLineEditStyle();
  syncPanelState();
  emit componentTokensChanged();
}

void AdDateRangePicker::resetComponentTokens() { setComponentTokens(ComponentTokens()); }

AdDateRangePicker::SemanticStyles AdDateRangePicker::semanticStyles() const {
  return semanticStyles_;
}

void AdDateRangePicker::setSemanticStyles(const SemanticStyles& styles) {
  if (datePickerSemanticStylesEqual(semanticStyles_, styles)) {
    return;
  }
  semanticStyles_ = styles;
  syncLineEditStyle();
  syncPanelState();
  syncPopupGeometry();
  emit semanticStylesChanged();
}

void AdDateRangePicker::setSemanticStyleResolver(SemanticStyleResolver resolver) {
  semanticStyleResolver_ = std::move(resolver);
  syncLineEditStyle();
  syncPanelState();
  syncPopupGeometry();
  emit semanticStylesChanged();
}

void AdDateRangePicker::clearSemanticStyleResolver() {
  setSemanticStyleResolver(SemanticStyleResolver());
}

QVector<AdDateRangePicker::PresetItem> AdDateRangePicker::presets() const { return presets_; }

void AdDateRangePicker::setPresets(const QVector<PresetItem>& presets) {
  if (presetsEqual(presets_, presets)) {
    return;
  }
  presets_ = presets;
  rebuildRangePresets();
  syncPanelState();
  emit presetsChanged();
}

void AdDateRangePicker::clearPresets() { setPresets({}); }

QWidget* AdDateRangePicker::extraFooterWidget() const { return extraFooterWidget_; }

void AdDateRangePicker::setExtraFooterWidget(QWidget* widget) {
  if (extraFooterWidget_ == widget) {
    return;
  }

  QWidget* previous = extraFooterWidget_.data();
  if (popupExtraFooterHost_) {
    if (previous) {
      if (auto* layout = popupExtraFooterHost_->layout()) {
        layout->removeWidget(previous);
      }
    }
    if (previous && previous != widget) {
      previous->hide();
      previous->setParent(nullptr);
    }
  } else if (popupPanel_) {
    previous = popupPanel_->takeExtraFooterWidget();
    if (previous && previous != widget) {
      previous->setParent(nullptr);
    }
  } else if (previous && previous != widget) {
    previous->hide();
    previous->setParent(nullptr);
  }

  extraFooterWidget_ = widget;
  if (extraFooterWidget_ && popupExtraFooterHost_) {
    extraFooterWidget_->setParent(popupExtraFooterHost_);
    if (auto* layout = popupExtraFooterHost_->layout()) {
      layout->addWidget(extraFooterWidget_);
    }
    extraFooterWidget_->show();
  } else if (extraFooterWidget_) {
    extraFooterWidget_->setParent(this);
    extraFooterWidget_->hide();
  }
  refreshRangeFooter();
  emit extraFooterWidgetChanged(extraFooterWidget_.data());
}

QWidget* AdDateRangePicker::takeExtraFooterWidget() {
  QWidget* widget = extraFooterWidget_.data();
  if (!widget && popupPanel_) {
    widget = popupPanel_->takeExtraFooterWidget();
  }
  if (!widget) {
    return nullptr;
  }
  if (popupExtraFooterHost_) {
    if (auto* layout = popupExtraFooterHost_->layout()) {
      layout->removeWidget(widget);
    }
  }
  widget->hide();
  widget->setParent(nullptr);
  extraFooterWidget_ = nullptr;
  refreshRangeFooter();
  emit extraFooterWidgetChanged(nullptr);
  return widget;
}

AdDateRangePicker::DatePredicate AdDateRangePicker::disabledDatePredicate() const {
  return disabledDatePredicate_;
}

void AdDateRangePicker::setDisabledDatePredicate(DatePredicate predicate) {
  const bool hadPredicate = static_cast<bool>(disabledDatePredicate_);
  const bool hasPredicate = static_cast<bool>(predicate);
  if (!hadPredicate && !hasPredicate) {
    return;
  }
  disabledDatePredicate_ = std::move(predicate);
  panelDisabledDatePredicateDirty_ = true;
  if (popupVisible_) {
    syncPanelState();
  }
}

AdDateRangePicker::DisabledDatePredicate AdDateRangePicker::disabledDateContextPredicate() const {
  return disabledDateContextPredicate_;
}

void AdDateRangePicker::setDisabledDateContextPredicate(DisabledDatePredicate predicate) {
  const bool hadPredicate = static_cast<bool>(disabledDateContextPredicate_);
  const bool hasPredicate = static_cast<bool>(predicate);
  if (!hadPredicate && !hasPredicate) {
    return;
  }
  disabledDateContextPredicate_ = std::move(predicate);
  panelDisabledDateContextPredicateDirty_ = true;
  if (popupVisible_) {
    syncPanelState();
  }
}

AdDateRangePicker::DisabledTimePredicate AdDateRangePicker::disabledTimePredicate() const {
  return disabledTimePredicate_;
}

void AdDateRangePicker::setDisabledTimePredicate(DisabledTimePredicate predicate) {
  const bool hadPredicate = static_cast<bool>(disabledTimePredicate_);
  const bool hasPredicate = static_cast<bool>(predicate);
  if (!hadPredicate && !hasPredicate) {
    return;
  }
  disabledTimePredicate_ = std::move(predicate);
  panelDisabledTimePredicateDirty_ = true;
  if (popupVisible_) {
    syncPanelState();
    syncPopupGeometry();
  }
}

AdDateRangePicker::DisplayTextCallback AdDateRangePicker::displayTextCallback() const {
  return displayTextCallback_;
}

void AdDateRangePicker::setDisplayTextCallback(DisplayTextCallback callback) {
  displayTextCallback_ = std::move(callback);
  syncLineEdit();
  emit displayTextCallbackChanged();
}

void AdDateRangePicker::clearDisplayTextCallback() {
  setDisplayTextCallback(DisplayTextCallback());
}

AdDateRangePicker::CellRenderCallback AdDateRangePicker::cellRenderCallback() const {
  return cellRenderCallback_;
}

void AdDateRangePicker::setCellRenderCallback(CellRenderCallback callback) {
  const bool hadCallback = static_cast<bool>(cellRenderCallback_);
  const bool hasCallback = static_cast<bool>(callback);
  if (!hadCallback && !hasCallback) {
    return;
  }
  cellRenderCallback_ = std::move(callback);
  panelCellRenderCallbackDirty_ = true;
  if (popupVisible_) {
    syncPanelState();
  }
  emit cellRenderCallbackChanged();
}

void AdDateRangePicker::clearCellRenderCallback() { setCellRenderCallback(CellRenderCallback()); }

AdDateRangePicker::PopupContentWrapperFactory AdDateRangePicker::popupContentWrapperFactory()
    const {
  return popupContentWrapperFactory_;
}

void AdDateRangePicker::setPopupContentWrapperFactory(PopupContentWrapperFactory factory) {
  popupContentWrapperFactory_ = std::move(factory);
  const bool wasVisible = popupVisible_;
  if (popup_) {
    if (wasVisible) {
      setPopupVisibleInternal(false, true);
    }
    destroyPopup();
    if (wasVisible) {
      setPopupVisibleInternal(true, true);
    }
  }
  emit popupContentWrapperFactoryChanged();
}

void AdDateRangePicker::clearPopupContentWrapperFactory() {
  setPopupContentWrapperFactory(PopupContentWrapperFactory());
}

AdDateRangePicker::PanelComponentFactory AdDateRangePicker::panelComponentFactory() const {
  return panelComponentFactory_;
}

void AdDateRangePicker::setPanelComponentFactory(PanelComponentFactory factory) {
  panelComponentFactory_ = std::move(factory);
  const bool wasVisible = popupVisible_;
  if (popup_) {
    if (wasVisible) {
      setPopupVisibleInternal(false, true);
    }
    destroyPopup();
    if (wasVisible) {
      setPopupVisibleInternal(true, true);
    }
  }
  emit panelComponentFactoryChanged();
}

void AdDateRangePicker::clearPanelComponentFactory() {
  setPanelComponentFactory(PanelComponentFactory());
}

void AdDateRangePicker::focus(RangePart range) {
  if (lineEdit_) {
    lineEdit_->focusEditor(AdLineEdit::FocusSelection::Preserve);
    moveCursorToRangePart(range);
    // Focusing an endpoint explicitly starts a new endpoint-editing sequence.
    // Do not let a previous in-popup start/end sequence affect the next confirm.
    activeRangeHistory_.clear();
    setActiveRangePart(range, true);
    syncLineEditRangeDisplay(popupCalendarStartDate(), popupCalendarEndDate(),
                             popupCalendarStartTime(), popupCalendarEndTime());
    syncPopupActiveAlignment();
  } else {
    activeRangeHistory_.clear();
    setActiveRangePart(range, true);
    setFocus(Qt::OtherFocusReason);
  }
}

void AdDateRangePicker::blur() {
  if (lineEdit_) {
    lineEdit_->blurInput();
  } else {
    clearFocus();
  }
}

AdLineEdit* AdDateRangePicker::lineEdit() const { return lineEdit_; }

AdDatePickerPanel* AdDateRangePicker::panel() const { return popupPanel_; }

AdDatePickerPanel* AdDateRangePicker::endPanel() const { return popupEndPanel_; }

QSize AdDateRangePicker::sizeHint() const {
  QSize hint = lineEdit_ ? lineEdit_->sizeHint() : QSize(220, 32);
  hint.setWidth(std::max(hint.width(), 240));
  return hint;
}

QSize AdDateRangePicker::minimumSizeHint() const {
  return lineEdit_ ? QSize(std::max(160, lineEdit_->minimumSizeHint().width()),
                           lineEdit_->minimumSizeHint().height())
                   : QSize(160, 32);
}

bool AdDateRangePicker::eventFilter(QObject* watched, QEvent* event) {
  if (!event || !lineEdit_) {
    return QWidget::eventFilter(watched, event);
  }
  if (watched == lineEdit_ || watched == lineEdit_->trailingActionButton()) {
    if (watched == lineEdit_ && event->type() == QEvent::FocusIn) {
      setActiveRangePart(activeRangePart(), true);
      syncLineEditRangeDisplay(popupCalendarStartDate(), popupCalendarEndDate(),
                               popupCalendarStartTime(), popupCalendarEndTime());
      emit focused(lastFocusedRangePart_);
    } else if (watched == lineEdit_ && event->type() == QEvent::FocusOut) {
      if (!popupVisible_ && !popupNeedsExplicitSubmit()) {
        if (!commitPopupCalendarRange(false, false)) {
          popupCalendarActive_ = false;
          activeRangeHistory_.clear();
        }
      }
      syncLineEditRangeDisplay(startDate_, endDate_, startTime_, endTime_);
      emit blurred(lastFocusedRangePart_);
    } else if (watched == lineEdit_ && event->type() == QEvent::MouseButtonPress) {
      auto* mouseEvent = static_cast<QMouseEvent*>(event);
      activeRangeHistory_.clear();
      setActiveRangePart(lineEdit_->rangePartAt(mouseEventPos(mouseEvent)), true);
      moveCursorToRangePart(lastFocusedRangePart_);
      syncLineEditRangeDisplay(popupCalendarStartDate(), popupCalendarEndDate(),
                               popupCalendarStartTime(), popupCalendarEndTime());
      syncPopupActiveAlignment();
      if (mouseEvent->button() == Qt::LeftButton && isEnabled()) {
        showPopup();
      }
    } else if (event->type() == QEvent::MouseButtonPress) {
      auto* mouseEvent = static_cast<QMouseEvent*>(event);
      if (mouseEvent->button() == Qt::LeftButton && isEnabled()) {
        showPopup();
      }
    } else if (event->type() == QEvent::KeyPress) {
      auto* keyEvent = static_cast<QKeyEvent*>(event);
      if (keyEvent->key() == Qt::Key_Down) {
        showPopup();
        keyEvent->accept();
        return true;
      }
      if (keyEvent->key() == Qt::Key_Escape && popupVisible_) {
        hidePopup();
        keyEvent->accept();
        return true;
      }
      if ((keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) &&
          popupVisible_) {
        confirmActiveRangePart(true, true);
        keyEvent->accept();
        return true;
      }
      if (keyEvent->key() == Qt::Key_Tab && popupVisible_) {
        const std::optional<RangePart> nextActive =
            nextActiveRangePart(popupCalendarStartDate(), popupCalendarEndDate());
        if (nextActive.has_value()) {
          setActiveRangePart(*nextActive, true);
          moveCursorToRangePart(lastFocusedRangePart_);
          syncLineEditRangeDisplay(popupCalendarStartDate(), popupCalendarEndDate(),
                                   popupCalendarStartTime(), popupCalendarEndTime());
          syncPopupActiveAlignment();
          keyEvent->accept();
          return true;
        }
        commitPopupCalendarRange(true, true);
      } else if (keyEvent->key() == Qt::Key_Backtab && popupVisible_) {
        const RangePart previous =
            lastFocusedRangePart_ == RangePart::End ? RangePart::Start : RangePart::End;
        if ((previous == RangePart::Start && !startDisabled_) ||
            (previous == RangePart::End && !endDisabled_)) {
          setActiveRangePart(previous, true);
          moveCursorToRangePart(lastFocusedRangePart_);
          syncLineEditRangeDisplay(popupCalendarStartDate(), popupCalendarEndDate(),
                                   popupCalendarStartTime(), popupCalendarEndTime());
          syncPopupActiveAlignment();
          keyEvent->accept();
          return true;
        }
      }
    }
  } else if (watched == popup_ && event->type() == QEvent::Hide && popupVisible_ &&
             (!popupController_ || popupController_->popupVisible()) && !suppressPopupHideClose_) {
    setPopupVisibleInternal(false, true);
  } else if (watched && watched->property("adqt.rangePresetIndex").isValid()) {
    const int index = watched->property("adqt.rangePresetIndex").toInt();
    if (index >= 0 && index < presets_.size()) {
      if (event->type() == QEvent::Enter) {
        const PresetItem preset = presets_.at(index);
        const QDate presetValue = resolvedPresetValue(preset);
        const std::pair<QDate, QDate> presetRange = resolvedPresetRange(preset);
        QDate start = normalizeForPicker(pickerMode_, presetRange.first, effectiveFirstDayOfWeek());
        QDate end = normalizeForPicker(pickerMode_, presetRange.second, effectiveFirstDayOfWeek());
        if (!start.isValid() && presetValue.isValid()) {
          start = normalizeForPicker(pickerMode_, presetValue, effectiveFirstDayOfWeek());
        }
        if (!end.isValid() && !allowEmptyEnd_) {
          end = start;
        }
        if (order_ && !startDisabled_ && !endDisabled_ && start.isValid() && end.isValid() &&
            end < start) {
          std::swap(start, end);
        }
        mergeEndpointDisabledPopupRange(&start, &end);
        handlePreviewRangeChanged(start, end);
      } else if (event->type() == QEvent::Leave) {
        clearPreviewText();
      }
    }
  }
  return QWidget::eventFilter(watched, event);
}

void AdDateRangePicker::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (event && event->type() == QEvent::LanguageChange) {
    if (!localeExplicit_) {
      applyingGlobalLocale_ = true;
      setLocale(adqt::locale::LocaleManager::instance().locale());
      applyingGlobalLocale_ = false;
    }
    syncLineEditStyle();
    syncPanelState();
  } else if (event && event->type() == QEvent::EnabledChange) {
    syncLineEditStyle();
    syncPanelState();
  }
}

void AdDateRangePicker::moveEvent(QMoveEvent* event) {
  QWidget::moveEvent(event);
  syncPopupGeometry();
}

void AdDateRangePicker::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  syncPopupGeometry();
}

void AdDateRangePicker::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  if (!defaultOpen_ || defaultOpenApplied_) {
    return;
  }
  defaultOpenApplied_ = true;
  QTimer::singleShot(0, this, [this]() {
    if (defaultOpen_ && isVisible() && !popupVisible_) {
      showPopup();
    }
  });
}

void AdDateRangePicker::hideEvent(QHideEvent* event) {
  QWidget::hideEvent(event);
  hidePopup();
}

void AdDateRangePicker::buildUi() {
  setFocusPolicy(Qt::StrongFocus);
  setSemanticSlot(this, "root", QStringLiteral("addaterangepicker"));
  rootLayout_ = new QHBoxLayout(this);
  rootLayout_->setContentsMargins(0, 0, 0, 0);
  rootLayout_->setSpacing(0);

  lineEdit_ = new detail::DatePickerLineEdit(this);
  setSemanticSlot(lineEdit_, "input", QStringLiteral("addaterangepicker-input"));
  lineEdit_->setClearOverlaysTrailingAction(true);
  lineEdit_->setTrailingActionVisible(true);
  lineEdit_->setTrailingActionAccessibleName(tr("Open calendar"));
  lineEdit_->installEventFilter(this);
  if (lineEdit_->trailingActionButton()) {
    lineEdit_->trailingActionButton()->installEventFilter(this);
    connect(lineEdit_->trailingActionButton(), &QToolButton::clicked, this, [this]() {
      if (popupVisible_) {
        hidePopup();
      } else {
        showPopup();
      }
    });
  }
  connect(lineEdit_, &AdLineEdit::cleared, this, [this]() { clearRangeInternal(true, true); });
  connect(lineEdit_, &QLineEdit::editingFinished, this, [this]() { commitInputText(); });
  connect(lineEdit_, &QLineEdit::returnPressed, this, [this]() { commitInputText(); });
  rootLayout_->addWidget(lineEdit_);
  syncInputIds();
  syncLineEditStyle();
  syncLineEdit();
  syncLineEditMask();
}

void AdDateRangePicker::ensurePopup() {
  if (popup_) {
    return;
  }
  QWidget* scopeWindow = detail::resolvePopupScopeWindow(this);
  auto* surface = new detail::OverlayPopupSurface(
      popupLayerMode_ == PopupLayerMode::QtTool ? nullptr : scopeWindow);
  if (popupLayerMode_ == PopupLayerMode::QtTool) {
    surface->setWindowFlags(adQtToolWindowFlags());
    surface->setAttribute(Qt::WA_ShowWithoutActivating, true);
    surface->setAttribute(Qt::WA_TranslucentBackground, true);
    surface->setAttribute(Qt::WA_QuitOnClose, false);
  }
  popup_ = surface;
  popup_->setObjectName(QStringLiteral("addaterangepicker-popup"));
  setSemanticSlot(popup_, "popup.root", QStringLiteral("addaterangepicker-popup"));
  popup_->setProperty("adqt.interaction.surface", true);
  popup_->setAttribute(Qt::WA_DeleteOnClose, false);
  popup_->installEventFilter(this);
  applyPopupLayerMode();

  popupBodyHost_ = surface->bodyWidget();
  if (popupBodyHost_) {
    popupBodyHost_->setObjectName(QStringLiteral("addaterangepicker-popup-body"));
    setSemanticSlot(popupBodyHost_, "popup.container",
                    QStringLiteral("addaterangepicker-popup-body"));
    popupBodyHost_->setProperty("adqt.interaction.surface", true);
    popupBodyHost_->setAutoFillBackground(false);
  }

  popupLayout_ = new QVBoxLayout(popupBodyHost_ ? popupBodyHost_ : popup_);
  popupLayout_->setContentsMargins(0, 0, 0, 0);
  popupLayout_->setSpacing(0);

  QWidget* popupContentParent = popupBodyHost_ ? popupBodyHost_ : popup_;
  popupPanelsWidget_ = new QWidget(popupContentParent);
  setSemanticSlot(popupPanelsWidget_, "popup.container",
                  QStringLiteral("addaterangepicker-popup-container"));
  popupPanelsLayout_ = new QHBoxLayout(popupPanelsWidget_);
  popupPanelsLayout_->setContentsMargins(0, 0, 0, 0);
  popupPanelsLayout_->setSpacing(0);

  popupMainWidget_ = new QWidget(popupPanelsWidget_);
  setSemanticSlot(popupMainWidget_, "popup.main", QStringLiteral("addaterangepicker-popup-main"));
  popupMainLayout_ = new QVBoxLayout(popupMainWidget_);
  popupMainLayout_->setContentsMargins(0, 0, 0, 0);
  popupMainLayout_->setSpacing(0);

  popupPanelRowWidget_ = new QWidget(popupMainWidget_);
  setSemanticSlot(popupPanelRowWidget_, "popup.panels",
                  QStringLiteral("addaterangepicker-popup-panels"));
  popupPanelRowLayout_ = new QHBoxLayout(popupPanelRowWidget_);
  popupPanelRowLayout_->setContentsMargins(0, 0, 0, 0);
  popupPanelRowLayout_->setSpacing(0);

  popupPanel_ = new AdDatePickerPanel(popupPanelRowWidget_);
  popupPanel_->setSelectionMode(AdDatePickerPanel::SelectionMode::Range);
  popupPanel_->setFooterVisible(false);
  popupEndPanel_ = new AdDatePickerPanel(popupPanelRowWidget_);
  popupEndPanel_->setSelectionMode(AdDatePickerPanel::SelectionMode::Range);
  popupEndPanel_->setFooterVisible(false);

  popupFooter_ = new QWidget(popupMainWidget_);
  setSemanticSlot(popupFooter_, "popup.footer", QStringLiteral("addaterangepicker-popup-footer"));
  popupFooter_->setAttribute(Qt::WA_StyledBackground, true);
  popupFooterOuterLayout_ = new QVBoxLayout(popupFooter_);
  popupFooterOuterLayout_->setContentsMargins(0, 0, 0, 0);
  popupFooterOuterLayout_->setSpacing(0);

  popupExtraFooterHost_ = new QWidget(popupFooter_);
  setSemanticSlot(popupExtraFooterHost_, "popup.footer.extra",
                  QStringLiteral("addaterangepicker-popup-extra-footer"));
  popupExtraFooterHost_->setAttribute(Qt::WA_StyledBackground, true);
  auto* popupExtraFooterLayout = new QVBoxLayout(popupExtraFooterHost_);
  popupExtraFooterLayout->setContentsMargins(12, 0, 12, 0);
  popupExtraFooterLayout->setSpacing(0);
  popupExtraFooterHost_->hide();
  popupFooterOuterLayout_->addWidget(popupExtraFooterHost_);

  popupFooterActionsWidget_ = new QWidget(popupFooter_);
  setSemanticSlot(popupFooterActionsWidget_, "popup.footer.actions",
                  QStringLiteral("addaterangepicker-popup-footer-actions"));
  popupFooterActionsWidget_->setAttribute(Qt::WA_StyledBackground, true);
  popupFooterLayout_ = new QHBoxLayout(popupFooterActionsWidget_);
  popupFooterLayout_->setContentsMargins(12, 0, 12, 0);
  popupFooterLayout_->setSpacing(8);
  popupNowButton_ = createPanelToolButton(popupFooterActionsWidget_, tr("Now"));
  popupOkButton_ = createPanelToolButton(popupFooterActionsWidget_, tr("OK"));
  popupFooterLayout_->addWidget(popupNowButton_);
  popupFooterLayout_->addStretch(1);
  popupFooterLayout_->addWidget(popupOkButton_);
  popupFooterActionsWidget_->hide();
  popupFooterOuterLayout_->addWidget(popupFooterActionsWidget_);
  popupFooter_->hide();

  popupMainLayout_->addWidget(popupPanelRowWidget_);
  popupMainLayout_->addWidget(popupFooter_);

  const auto connectPanel = [this](AdDatePickerPanel* panel) {
    if (!panel) {
      return;
    }
    connect(panel, &AdDatePickerPanel::rangeChanged, this,
            [this, panel](const QDate& start, const QDate& end) {
              handlePopupRangeChanged(panel, start, end);
              refreshRangeFooter();
            });
    connect(panel, &AdDatePickerPanel::rangeTimeChanged, this,
            [this, panel](const QTime& start, const QTime& end) {
              handlePopupRangeTimeChanged(panel, start, end);
              refreshRangeFooter();
            });
    connect(panel, &AdDatePickerPanel::rangeAccepted, this,
            [this](const QDate& start, const QDate& end) { handlePopupRangeAccepted(start, end); });
    connect(panel, &AdDatePickerPanel::previewDateChanged, this,
            [this](const QDate& value) { handlePreviewDateChanged(value); });
    connect(panel, &AdDatePickerPanel::previewTimeChanged, this,
            [this](const QTime& value, TimeSelectionPart part) {
              handlePreviewTimeChanged(value, part);
            });
  };
  connectPanel(popupPanel_);
  connectPanel(popupEndPanel_);

  connect(popupPanel_, &AdDatePickerPanel::viewDateChanged, this, [this](const QDate& value) {
    if (syncingPopupPanels_) {
      return;
    }
    syncPopupPanelViewsFromPrimary(value);
    emit panelChanged(popupPanel_ ? popupPanel_->viewDate() : QDate(),
                      popupEndPanel_ ? popupEndPanel_->viewDate() : QDate(),
                      popupPanel_ ? popupPanel_->panelMode() : effectivePanelMode());
  });
  connect(popupEndPanel_, &AdDatePickerPanel::viewDateChanged, this, [this](const QDate& value) {
    if (syncingPopupPanels_) {
      return;
    }
    syncPopupPanelViewsFromSecondary(value);
    emit panelChanged(popupPanel_ ? popupPanel_->viewDate() : QDate(),
                      popupEndPanel_ ? popupEndPanel_->viewDate() : QDate(),
                      popupEndPanel_ ? popupEndPanel_->panelMode() : effectivePanelMode());
  });
  const auto connectPanelMode = [this](AdDatePickerPanel* panel) {
    if (!panel) {
      return;
    }
    connect(panel, &AdDatePickerPanel::panelModeChanged, this, [this](PickerMode value) {
      if (syncingPopupPanels_) {
        return;
      }
      const PickerMode normalized = normalizedPanelMode(value);
      const bool changed = !panelModeExplicit_ || panelMode_ != normalized;
      panelMode_ = normalized;
      panelModeExplicit_ = true;
      {
        QScopedValueRollback<bool> guard(syncingPopupPanels_, true);
        const std::optional<QSignalBlocker> primaryBlocker =
            popupPanel_ ? std::optional<QSignalBlocker>(std::in_place, popupPanel_) : std::nullopt;
        const std::optional<QSignalBlocker> secondaryBlocker =
            popupEndPanel_ ? std::optional<QSignalBlocker>(std::in_place, popupEndPanel_)
                           : std::nullopt;
        if (popupPanel_) {
          popupPanel_->setPanelMode(panelMode_);
        }
        if (popupEndPanel_) {
          popupEndPanel_->setPanelMode(panelMode_);
        }
      }
      if (changed) {
        emit panelModeChanged(panelMode_);
      }
      emit panelChanged(popupPanel_ ? popupPanel_->viewDate() : QDate(),
                        popupEndPanel_ ? popupEndPanel_->viewDate() : QDate(), panelMode_);
    });
  };
  connectPanelMode(popupPanel_);
  connectPanelMode(popupEndPanel_);

  QWidget* primaryContent =
      createPanelComponentWidget(popupPanelRowWidget_, popupPanel_, PanelComponentRole::RangeStart);
  QWidget* secondaryContent = createPanelComponentWidget(popupPanelRowWidget_, popupEndPanel_,
                                                         PanelComponentRole::RangeEnd);
  popupPrimaryContentWidget_ = primaryContent ? primaryContent : popupPanel_;
  popupSecondaryContentWidget_ = secondaryContent ? secondaryContent : popupEndPanel_;
  popupPanelRowLayout_->addWidget(popupPrimaryContentWidget_);
  popupPanelRowLayout_->addWidget(popupSecondaryContentWidget_);
  popupPanelsLayout_->addWidget(popupMainWidget_);
  connect(popupNowButton_, &QToolButton::clicked, this, [this]() { handlePopupNow(); });
  connect(popupOkButton_, &QToolButton::clicked, this,
          [this]() { acceptPanelComponentSelection(); });
  syncPanelState();
  rebuildRangePresets();
  popupContentWidget_ =
      wrappedPopupContent(popupContentParent, popupPanelsWidget_, popupContentWrapperFactory_);
  popupLayout_->addWidget(popupContentWidget_ ? popupContentWidget_ : popupPanelsWidget_);
  if (popupController_) {
    popupController_->popupSurfaceChanged();
  }
}

void AdDateRangePicker::destroyPopup() {
  if (!popup_) {
    return;
  }

  QScopedValueRollback<bool> guard(suppressPopupHideClose_, true);
  popup_->hide();
  popup_->deleteLater();
  popup_ = nullptr;
  popupBodyHost_ = nullptr;
  popupLayout_ = nullptr;
  popupContentWidget_ = nullptr;
  popupPanelsWidget_ = nullptr;
  popupPresetsWidget_ = nullptr;
  popupPresetsScrollArea_ = nullptr;
  popupPresetsListWidget_ = nullptr;
  popupPresetsLayout_ = nullptr;
  popupMainWidget_ = nullptr;
  popupMainLayout_ = nullptr;
  popupPanelRowWidget_ = nullptr;
  popupPanelRowLayout_ = nullptr;
  popupFooter_ = nullptr;
  popupFooterOuterLayout_ = nullptr;
  popupExtraFooterHost_ = nullptr;
  popupFooterActionsWidget_ = nullptr;
  popupFooterLayout_ = nullptr;
  popupNowButton_ = nullptr;
  popupOkButton_ = nullptr;
  popupPrimaryContentWidget_ = nullptr;
  popupSecondaryContentWidget_ = nullptr;
  popupPanelsLayout_ = nullptr;
  popupPanel_ = nullptr;
  popupEndPanel_ = nullptr;
  panelDisabledDatePredicateDirty_ = true;
  panelDisabledDateContextPredicateDirty_ = true;
  panelDisabledTimePredicateDirty_ = true;
  panelCellRenderCallbackDirty_ = true;
  if (popupController_) {
    popupController_->popupSurfaceChanged();
    popupController_->invalidatePopupGeometry();
  }
}

void AdDateRangePicker::applyPopupLayerMode() {
  if (!popup_) {
    return;
  }
  QWidget* scopeWindow = detail::resolvePopupScopeWindow(this);
  QWidget* targetParent = popupLayerMode_ == PopupLayerMode::QtTool ? nullptr : scopeWindow;
  const Qt::WindowFlags flags =
      popupLayerMode_ == PopupLayerMode::QtTool ? adQtToolWindowFlags() : Qt::Widget;
  const bool useToolWindow = popupLayerMode_ == PopupLayerMode::QtTool;
  popup_->setAttribute(Qt::WA_ShowWithoutActivating, useToolWindow);
  popup_->setAttribute(Qt::WA_TranslucentBackground, useToolWindow);
  popup_->setAttribute(Qt::WA_QuitOnClose, !useToolWindow);
  if (popup_->parentWidget() != targetParent || popup_->windowFlags() != flags) {
    popup_->setParent(targetParent, flags);
    if (popupController_) {
      popupController_->popupSurfaceChanged();
      popupController_->invalidatePopupGeometry();
    }
  }
}

void AdDateRangePicker::syncPopupGeometry() {
  if (!popupController_) {
    return;
  }
  popupController_->invalidatePopupGeometry();
  if (popupController_->popupVisible()) {
    popupController_->refreshVisiblePopup();
  }
}

void AdDateRangePicker::syncPopupActiveAlignment() {
  if (popupController_ && popupController_->popupVisible()) {
    popupController_->invalidatePopupGeometry();
    popupController_->refreshVisiblePopup();
  }
  syncPopupArrowPosition();
}

int AdDateRangePicker::popupPanelContainerWidth() const {
  int containerWidth = 0;
  if (const auto* surface = dynamic_cast<const detail::OverlayPopupSurface*>(popup_)) {
    containerWidth = std::max(containerWidth, surface->visualSizeHint().width());
  }

  const QWidget* content = popupContentWidget_ ? popupContentWidget_ : popupPanelsWidget_;
  if (content) {
    const QSize hint = content->sizeHint();
    if (hint.width() > 0) {
      containerWidth = std::max(containerWidth, hint.width());
    }
    if (content->width() > 0) {
      containerWidth = std::max(containerWidth, content->width());
    }
  }
  return std::max(0, containerWidth);
}

int AdDateRangePicker::popupPanelAlignmentOffset(detail::OverlayPopupPlacement placement) const {
  if (!lineEdit_) {
    return 0;
  }

  const int containerWidth = popupPanelContainerWidth();
  const int selectorWidth = std::max(0, width());
  if (containerWidth <= 0 || selectorWidth <= 0 || containerWidth >= selectorWidth) {
    return 0;
  }

  int wrapperLeft = 0;
  switch (placement) {
    case detail::OverlayPopupPlacement::TopRight:
    case detail::OverlayPopupPlacement::BottomRight:
      wrapperLeft = selectorWidth - containerWidth;
      break;
    case detail::OverlayPopupPlacement::TopLeft:
    case detail::OverlayPopupPlacement::BottomLeft:
    default:
      wrapperLeft = 0;
      break;
  }

  const QRect activeRect = lineEdit_->rangeInputPartRect(lastFocusedRangePart_);
  if (!activeRect.isValid()) {
    return 0;
  }

  const int activeLeft = lineEdit_->x() + activeRect.left();
  const int activeRight = lineEdit_->x() + activeRect.right() + 1;
  const int arrowWidth = std::max(1, kRangePopupArrowSize * 2);
  if (layoutDirection() == Qt::RightToLeft) {
    const int wrapperRight = wrapperLeft + containerWidth;
    const int offset = wrapperRight - (activeRight - arrowWidth + containerWidth);
    return -std::max(0, offset);
  }

  const int offset = activeLeft + arrowWidth - wrapperLeft - containerWidth;
  return std::max(0, offset);
}

void AdDateRangePicker::syncPopupArrowPosition() {
  const bool controllerVisible = popupController_ && popupController_->popupVisible();
  if ((!popupVisible_ && !controllerVisible) || !popup_ || !lineEdit_) {
    return;
  }

  auto* surface = dynamic_cast<detail::OverlayPopupSurface*>(popup_);
  if (!surface || !surface->arrowVisible()) {
    return;
  }

  const QMargins shadowMargins = surface->shadowMargins();
  const QPoint visualTopLeft =
      surface->mapToGlobal(QPoint(shadowMargins.left(), shadowMargins.top()));
  const int partCenter = lineEdit_->x() + lineEdit_->rangeInputPartCenterX(lastFocusedRangePart_);
  const QPoint activePoint = mapToGlobal(QPoint(partCenter, height() / 2));

  switch (surface->placement()) {
    case detail::OverlayPopupPlacement::Left:
    case detail::OverlayPopupPlacement::LeftTop:
    case detail::OverlayPopupPlacement::LeftBottom:
    case detail::OverlayPopupPlacement::Right:
    case detail::OverlayPopupPlacement::RightTop:
    case detail::OverlayPopupPlacement::RightBottom:
      surface->setArrowCenter(activePoint.y() - visualTopLeft.y());
      break;
    case detail::OverlayPopupPlacement::Top:
    case detail::OverlayPopupPlacement::TopLeft:
    case detail::OverlayPopupPlacement::TopRight:
    case detail::OverlayPopupPlacement::Bottom:
    case detail::OverlayPopupPlacement::BottomLeft:
    case detail::OverlayPopupPlacement::BottomRight:
    default:
      surface->setArrowCenter(activePoint.x() - visualTopLeft.x());
      break;
  }
}

void AdDateRangePicker::popupPrepareToShow() {
  const bool popupAlreadyCreated = popup_ != nullptr;
  ensurePopup();
  if (popupAlreadyCreated) {
    syncPanelState();
  }

  auto* surface = static_cast<detail::OverlayPopupSurface*>(popup_);
  detail::DatePickerStyleInput input;
  input.size = size_;
  input.variant = variant_;
  input.status = status_;
  input.disabled = effectiveInputDisabled();
  input.baseFont = font();
  input.componentTokens = componentTokens_;
  input.semanticStyles = effectiveSemanticStyles().popup;
  const detail::DatePickerVisualStyle style = detail::resolveDatePickerVisualStyle(
      input, adqt::theme::ThemeManager::instance().resolve(this));
  detail::OverlayPopupSurfaceStyle surfaceStyle;
  surfaceStyle.background = style.panelBackground;
  surfaceStyle.borderColor = QColor(0, 0, 0, 0);
  surfaceStyle.arrowBackground = style.panelBackground;
  surfaceStyle.arrowBorderColor = QColor(0, 0, 0, 0);
  surfaceStyle.metrics.borderRadius = std::max(0, style.metrics.borderRadius);
  surfaceStyle.metrics.borderWidth = 0;
  surfaceStyle.metrics.arrowSize = kRangePopupArrowSize;
  surface->setSurfaceStyle(surfaceStyle);
  surface->setArrowVisible(true);
  surface->setPlacement(toOverlayPopupPlacement(placement_));
  popup_->setProperty("adqt.zIndex", style.metrics.zIndexPopup);
  if (popupLayout_) {
    popupLayout_->activate();
  }
  if (popupBodyHost_) {
    popupBodyHost_->updateGeometry();
  }
  popup_->adjustSize();
}

void AdDateRangePicker::setPopupVisibleInternal(bool value, bool emitSignal) {
  Q_UNUSED(emitSignal)
  if (popupVisible_ == value) {
    return;
  }
  if (value && effectiveInputDisabled()) {
    return;
  }
  if (!popupController_) {
    return;
  }

  popupController_->setDisabled(effectiveInputDisabled());
  if (value) {
    syncPopupCalendarFromCommitted();
  }
  popupController_->setPopupVisible(value);
}

void AdDateRangePicker::syncLineEdit() {
  if (!lineEdit_) {
    return;
  }
  QScopedValueRollback<bool> guard(syncingText_, true);
  lineEdit_->setPlaceholderText(effectivePlaceholder());
  lineEdit_->setText(effectiveRangeText(popupCalendarStartDate(), popupCalendarEndDate(),
                                        popupCalendarStartTime(), popupCalendarEndTime()));
  syncLineEditRangeDisplay(popupCalendarStartDate(), popupCalendarEndDate(),
                           popupCalendarStartTime(), popupCalendarEndTime());
}

void AdDateRangePicker::syncLineEditRangeDisplay(const QDate& start, const QDate& end,
                                                 const QTime& startTime, const QTime& endTime) {
  if (!lineEdit_) {
    return;
  }
  const QString startText = start.isValid() ? effectiveDisplayText(start, startTime) : QString();
  const QString endText = end.isValid() ? effectiveDisplayText(end, endTime) : QString();
  const bool useDefaultSeparatorIcon = separator_.isEmpty();
  lineEdit_->setRangeInputDisplay(
      startText, endText, effectiveStartPlaceholder(), effectiveEndPlaceholder(),
      useDefaultSeparatorIcon ? QString() : separator_, useDefaultSeparatorIcon,
      lastFocusedRangePart_, popupVisible_ || lineEdit_->hasFocus());
}

void AdDateRangePicker::syncLineEditMask() {
  if (!lineEdit_) {
    return;
  }
  lineEdit_->setInputMask(QString());
}

void AdDateRangePicker::syncInputIds() {
  const QString inputId = normalizedInputId(id_);
  const QString startInputId = normalizedInputId(startId_);
  const QString endInputId = normalizedInputId(endId_);
  const QString objectId =
      !inputId.isEmpty() ? inputId : (!startInputId.isEmpty() ? startInputId : endInputId);

  setObjectName(objectId);
  setProperty("adqt.inputId", inputId);
  setProperty("adqt.startInputId", startInputId);
  setProperty("adqt.endInputId", endInputId);
  applyAccessibleIdentifier(this, objectId);

  if (!lineEdit_) {
    return;
  }
  lineEdit_->setObjectName(objectId);
  lineEdit_->setProperty("adqt.inputId", inputId);
  lineEdit_->setProperty("adqt.startInputId", startInputId);
  lineEdit_->setProperty("adqt.endInputId", endInputId);
  applyAccessibleIdentifier(lineEdit_, objectId);
}

AdDateRangePicker::SemanticStyles AdDateRangePicker::effectiveSemanticStyles() const {
  if (!semanticStyleResolver_) {
    return semanticStyles_;
  }

  StyleContext context;
  context.pickerMode = pickerMode_;
  context.size = size_;
  context.variant = variant_;
  context.status = status_;
  context.disabled = effectiveInputDisabled();
  context.popupVisible = popupVisible_;
  context.showTime = showTime_;
  context.needConfirm = needConfirm_;
  context.activeRange = lastFocusedRangePart_;
  context.startDate = startDate_;
  context.endDate = endDate_;
  return semanticStyleResolver_(context);
}

AdDateRangePicker::PanelComponentContext AdDateRangePicker::makePanelComponentContext(
    AdDatePickerPanel* panel, PanelComponentRole role) {
  PanelComponentContext context;
  context.originPanel = panel;
  context.role = role;
  context.pickerMode = pickerMode_;
  context.panelMode = panel ? panel->panelMode() : effectivePanelMode();
  context.selectedDate = QDate();
  context.rangeStartDate = popupCalendarStartDate();
  context.rangeEndDate = popupCalendarEndDate();
  context.viewDate = panel ? panel->viewDate() : QDate();
  context.range = true;
  context.multiple = false;
  context.disabled = effectiveInputDisabled();
  context.selectDate = [this, role](const QDate& value) { selectPanelComponentDate(role, value); };
  context.previewDate = [this](const QDate& value) { handlePreviewDateChanged(value); };
  context.setViewDate = [panel](const QDate& value) {
    if (panel) {
      panel->setViewDate(value);
    }
  };
  context.setPanelMode = [panel](PickerMode value) {
    if (panel) {
      panel->setPanelMode(value);
    }
  };
  context.acceptSelection = [this]() { acceptPanelComponentSelection(); };
  return context;
}

QWidget* AdDateRangePicker::createPanelComponentWidget(QWidget* parent, AdDatePickerPanel* panel,
                                                       PanelComponentRole role) {
  QWidget* content = panel;
  if (panelComponentFactory_) {
    if (QWidget* replacement =
            panelComponentFactory_(makePanelComponentContext(panel, role), parent)) {
      replacement->setProperty("adqt.panelComponentContent", true);
      if (!replacement->parentWidget() && parent) {
        replacement->setParent(parent);
      }
      content = replacement;
    }
  }
  if (panel && content != panel) {
    panel->hide();
  }
  return content;
}

void AdDateRangePicker::selectPanelComponentDate(PanelComponentRole role, const QDate& value) {
  if (effectiveInputDisabled()) {
    return;
  }
  const QDate normalized = normalizeForPicker(pickerMode_, value, effectiveFirstDayOfWeek());
  if (!normalized.isValid() || !isDateSelectable(normalized)) {
    return;
  }

  const RangePart activePart =
      role == PanelComponentRole::RangeEnd ? RangePart::End : RangePart::Start;
  setActiveRangePart(activePart, true);

  QDate nextStart = popupCalendarStartDate();
  QDate nextEnd = popupCalendarEndDate();
  if (activePart == RangePart::End) {
    nextEnd = normalized;
  } else {
    nextStart = normalized;
  }

  AdDatePickerPanel* sourcePanel =
      role == PanelComponentRole::RangeEnd ? popupEndPanel_ : popupPanel_;
  handlePopupRangeChanged(sourcePanel, nextStart, nextEnd);
}

void AdDateRangePicker::acceptPanelComponentSelection() {
  if (effectiveInputDisabled()) {
    return;
  }
  confirmActiveRangePart(true, true);
}

void AdDateRangePicker::handlePreviewDateChanged(const QDate& value) {
  if (previewValue_ != PreviewValue::Hover || !lineEdit_ || !popupVisible_) {
    clearPreviewText();
    return;
  }

  QDate normalized = normalizeForPicker(pickerMode_, value, effectiveFirstDayOfWeek());
  if (!normalized.isValid()) {
    clearPreviewText();
    return;
  }

  QDate previewStart = popupCalendarStartDate();
  QDate previewEnd = popupCalendarEndDate();
  QTime previewStartTime = popupCalendarStartTime();
  QTime previewEndTime = popupCalendarEndTime();
  if (lastFocusedRangePart_ == RangePart::Start) {
    previewStart = normalized;
    previewStartTime = defaultOpenStartTime_;
  } else if (lastFocusedRangePart_ == RangePart::End) {
    previewEnd = normalized;
    previewEndTime = defaultOpenEndTime_;
  } else {
    previewStart = normalized;
    previewEnd = QDate();
    previewStartTime = defaultOpenStartTime_;
    previewEndTime = defaultOpenEndTime_;
  }

  if (popupPanel_) {
    popupPanel_->setHoverRange(previewStart, previewEnd);
  }
  if (popupEndPanel_) {
    popupEndPanel_->setHoverRange(previewStart, previewEnd);
  }

  previewRangeActive_ = true;
  QScopedValueRollback<bool> guard(syncingText_, true);
  lineEdit_->setText(
      effectiveRangeText(previewStart, previewEnd, previewStartTime, previewEndTime));
  syncLineEditRangeDisplay(previewStart, previewEnd, previewStartTime, previewEndTime);
}

void AdDateRangePicker::handlePreviewRangeChanged(const QDate& start, const QDate& end) {
  if (previewValue_ != PreviewValue::Hover || !lineEdit_ || !popupVisible_) {
    clearPreviewText();
    return;
  }
  if (!start.isValid() && !end.isValid()) {
    clearPreviewText();
    return;
  }

  const QTime previewStartTime = start.isValid() ? popupCalendarStartTime() : defaultOpenStartTime_;
  const QTime previewEndTime = end.isValid() ? popupCalendarEndTime() : defaultOpenEndTime_;
  previewRangeActive_ = true;
  QScopedValueRollback<bool> guard(syncingText_, true);
  lineEdit_->setText(effectiveRangeText(start, end, previewStartTime, previewEndTime));
  syncLineEditRangeDisplay(start, end, previewStartTime, previewEndTime);
}

void AdDateRangePicker::handlePreviewTimeChanged(const QTime& value, TimeSelectionPart part) {
  if (previewValue_ != PreviewValue::Hover || !lineEdit_ || !popupVisible_ ||
      !effectiveTextIncludesTime()) {
    clearPreviewText();
    return;
  }
  if (!value.isValid()) {
    clearPreviewText();
    return;
  }

  QDate previewStart = popupCalendarStartDate();
  QDate previewEnd = popupCalendarEndDate();
  if (!previewStart.isValid() && popupPanel_) {
    previewStart = popupPanel_->rangeStartDate();
  }
  if (!previewEnd.isValid() && popupPanel_) {
    previewEnd = popupPanel_->rangeEndDate();
  }
  if (!previewEnd.isValid() && popupEndPanel_) {
    previewEnd = popupEndPanel_->rangeEndDate();
  }

  QTime previewStartTime = popupCalendarStartTime();
  QTime previewEndTime = popupCalendarEndTime();
  if (part == TimeSelectionPart::Start) {
    if (!previewStart.isValid()) {
      clearPreviewText();
      return;
    }
    previewStartTime = value;
  } else if (part == TimeSelectionPart::End) {
    if (!previewEnd.isValid()) {
      clearPreviewText();
      return;
    }
    previewEndTime = value;
  } else {
    clearPreviewText();
    return;
  }

  previewRangeActive_ = true;
  previewTimeActive_ = true;
  previewTimePart_ = part;
  QScopedValueRollback<bool> guard(syncingText_, true);
  lineEdit_->setText(
      effectiveRangeText(previewStart, previewEnd, previewStartTime, previewEndTime));
  syncLineEditRangeDisplay(previewStart, previewEnd, previewStartTime, previewEndTime);
}

void AdDateRangePicker::clearPreviewText() {
  if (popupPanel_) {
    popupPanel_->clearHoverRange();
  }
  if (popupEndPanel_) {
    popupEndPanel_->clearHoverRange();
  }
  if (!previewRangeActive_ && !previewTimeActive_) {
    return;
  }
  previewRangeActive_ = false;
  previewTimeActive_ = false;
  previewTimePart_ = TimeSelectionPart::Single;
  syncLineEdit();
}

void AdDateRangePicker::rebuildRangePresets() {
  if (!presets_.isEmpty()) {
    ensureRangePresetsUi();
  }
  if (!popupPresetsLayout_) {
    return;
  }

  while (QLayoutItem* item = popupPresetsLayout_->takeAt(0)) {
    if (QWidget* widget = item->widget()) {
      widget->deleteLater();
    }
    delete item;
  }

  const detail::DatePickerVisualStyle style =
      popupPanel_ ? popupPanel_->resolvedStyle() : resolveStyleForPanel(nullptr);
  for (int i = 0; i < presets_.size(); ++i) {
    const PresetItem& preset = presets_.at(i);
    if (preset.label.trimmed().isEmpty()) {
      continue;
    }
    auto* button = createPanelToolButton(
        popupPresetsListWidget_ ? popupPresetsListWidget_ : popupPresetsWidget_, preset.label);
    button->setToolTip(preset.label);
    button->setProperty("adqt.rangePresetIndex", i);
    button->installEventFilter(this);
    applyPresetButtonStyle(button, style);
    connect(button, &QToolButton::clicked, this, [this, preset]() { applyRangePreset(preset); });
    popupPresetsLayout_->addWidget(button);
  }
  popupPresetsLayout_->addStretch(1);
  refreshRangePresets();
}

void AdDateRangePicker::ensureRangePresetsUi() {
  if (popupPresetsWidget_ || !popupPanelsWidget_ || !popupPanelsLayout_) {
    return;
  }

  popupPresetsWidget_ = new QWidget(popupPanelsWidget_);
  setSemanticSlot(popupPresetsWidget_, "popup.presets",
                  QStringLiteral("addaterangepicker-popup-presets"));
  popupPresetsWidget_->setAttribute(Qt::WA_StyledBackground, true);
  auto* popupPresetsOuterLayout = new QVBoxLayout(popupPresetsWidget_);
  popupPresetsOuterLayout->setContentsMargins(0, 0, 0, 0);
  popupPresetsOuterLayout->setSpacing(0);
  popupPresetsScrollArea_ = new QScrollArea(popupPresetsWidget_);
  setSemanticSlot(popupPresetsScrollArea_, "popup.presets.scroll",
                  QStringLiteral("addaterangepicker-popup-presets-scroll"));
  popupPresetsScrollArea_->setFrameShape(QFrame::NoFrame);
  popupPresetsScrollArea_->setWidgetResizable(true);
  popupPresetsScrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  popupPresetsScrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  popupPresetsListWidget_ = new QWidget(popupPresetsScrollArea_);
  setSemanticSlot(popupPresetsListWidget_, "popup.presets.list",
                  QStringLiteral("addaterangepicker-popup-presets-list"));
  popupPresetsListWidget_->setAttribute(Qt::WA_StyledBackground, true);
  popupPresetsLayout_ = new QVBoxLayout(popupPresetsListWidget_);
  popupPresetsLayout_->setContentsMargins(8, 8, 8, 8);
  popupPresetsLayout_->setSpacing(8);
  popupPresetsScrollArea_->setWidget(popupPresetsListWidget_);
  popupPresetsOuterLayout->addWidget(popupPresetsScrollArea_);
  popupPresetsWidget_->hide();
  popupPanelsLayout_->insertWidget(0, popupPresetsWidget_);
}

void AdDateRangePicker::refreshRangePresets() {
  if (!popupPresetsWidget_) {
    return;
  }

  const detail::DatePickerVisualStyle style =
      popupPanel_ ? popupPanel_->resolvedStyle() : resolveStyleForPanel(nullptr);
  const int minWidth = style.metrics.presetsWidth;
  const int maxWidth = std::max(minWidth, style.metrics.presetsMaxWidth);
  popupPresetsWidget_->setMinimumWidth(minWidth);
  popupPresetsWidget_->setMaximumWidth(maxWidth);
  popupPresetsWidget_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
  popupPresetsWidget_->setStyleSheet(
      QStringLiteral("QWidget#addaterangepicker-popup-presets { background: %1; "
                     "border-right: %2px solid %3; }")
          .arg(cssColor(style.panelBackground))
          .arg(std::max(1, style.metrics.borderWidth))
          .arg(cssColor(style.panelBorderColor)));

  if (popupPresetsScrollArea_) {
    popupPresetsScrollArea_->setStyleSheet(QStringLiteral(
        "QScrollArea#addaterangepicker-popup-presets-scroll { background: transparent; "
        "border: none; }"));
    popupPresetsScrollArea_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    if (popupPresetsScrollArea_->viewport()) {
      popupPresetsScrollArea_->viewport()->setStyleSheet(
          QStringLiteral("background: transparent;"));
    }
  }
  if (popupPresetsListWidget_) {
    popupPresetsListWidget_->setStyleSheet(QStringLiteral(
        "QWidget#addaterangepicker-popup-presets-list { background: transparent; }"));
    const QList<QToolButton*> presetButtons =
        popupPresetsListWidget_->findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly);
    for (QToolButton* button : presetButtons) {
      applyPresetButtonStyle(button, style);
    }
  }

  popupPresetsWidget_->setVisible(!presets_.isEmpty());
  if (popupController_) {
    popupController_->invalidatePopupGeometry();
  }
}

bool AdDateRangePicker::effectiveShowNowAction() const {
  if (pickerMode_ != PickerMode::Date && pickerMode_ != PickerMode::Time) {
    return false;
  }
  const PickerMode mode = effectivePanelMode();
  if (mode != PickerMode::Date && mode != PickerMode::Time) {
    return false;
  }
  if (showNowExplicit_) {
    return showNow_;
  }
  if (showTodayExplicit_) {
    return showToday_;
  }
  return false;
}

void AdDateRangePicker::handlePopupNow() {
  if (effectiveInputDisabled() || !effectiveShowNowAction()) {
    return;
  }

  RangePart activePart = activeRangePart();
  setActiveRangePart(activePart, true);
  activePart = lastFocusedRangePart_;
  if ((activePart == RangePart::Start && startDisabled_) ||
      (activePart == RangePart::End && endDisabled_)) {
    return;
  }

  const QDateTime now = QDateTime::currentDateTime();
  const QDate selected = normalizeForPicker(pickerMode_, now.date(), effectiveFirstDayOfWeek());
  if (!selected.isValid()) {
    return;
  }

  QDate nextStart = popupCalendarStartDate();
  QDate nextEnd = popupCalendarEndDate();
  QTime nextStartTime = popupCalendarStartTime();
  QTime nextEndTime = popupCalendarEndTime();
  const TimeSelectionPart timePart =
      activePart == RangePart::End ? TimeSelectionPart::End : TimeSelectionPart::Start;
  const QDate from = activePart == RangePart::End ? nextStart : nextEnd;
  const std::optional<QTime> validTime =
      validTimeForRangePart(selected, now.time(), activePart, from);
  if (!validTime.has_value()) {
    return;
  }
  const QTime selectedTime = *validTime;
  if (activePart == RangePart::End) {
    nextEnd = selected;
    nextEndTime = selectedTime;
  } else {
    nextStart = selected;
    nextStartTime = selectedTime;
  }
  mergeEndpointDisabledPopupRange(&nextStart, &nextEnd);

  if (!isDateTimeSelectable(dateTimeFromParts(selected, selectedTime), timePart, from) ||
      !respectsEndpointDisabledOrder(nextStart, nextEnd)) {
    return;
  }

  popupCalendarActive_ = true;
  popupCalendarStartDate_ = nextStart;
  popupCalendarEndDate_ = nextEnd;
  popupCalendarStartTime_ =
      nextStart.isValid() ? normalizedTimeValue(nextStartTime) : defaultOpenStartTime_;
  popupCalendarEndTime_ =
      nextEnd.isValid() ? normalizedTimeValue(nextEndTime) : defaultOpenEndTime_;
  emit calendarChanged(nextStart, nextEnd, activePart);

  const std::optional<RangePart> nextActive = nextActiveRangePart(nextStart, nextEnd);
  if (nextActive.has_value()) {
    setActiveRangePart(*nextActive, true);
  } else {
    setActiveRangePart(activePart, false);
  }
  moveCursorToRangePart(lastFocusedRangePart_);

  if (selected.isValid() && popupPanel_) {
    QScopedValueRollback<bool> guard(syncingPopupPanels_, true);
    const QSignalBlocker primaryBlocker(popupPanel_);
    const std::optional<QSignalBlocker> secondaryBlocker =
        popupEndPanel_ ? std::optional<QSignalBlocker>(std::in_place, popupEndPanel_)
                       : std::nullopt;
    popupPanel_->setViewDate(adjustedPrimaryPanelViewDate(selected));
    popupPanel_->setRange(nextStart, nextEnd);
    popupPanel_->setTimeRange(popupCalendarStartTime_, popupCalendarEndTime_);
    popupPanel_->setVisibleRangeTimePart(lastFocusedRangePart_ == RangePart::End
                                             ? TimeSelectionPart::End
                                             : TimeSelectionPart::Start);
    if (popupEndPanel_) {
      popupEndPanel_->setViewDate(secondaryPanelViewDate(popupPanel_->viewDate()));
      popupEndPanel_->setRange(nextStart, nextEnd);
      popupEndPanel_->setTimeRange(popupCalendarStartTime_, popupCalendarEndTime_);
      popupEndPanel_->setVisibleRangeTimePart(lastFocusedRangePart_ == RangePart::End
                                                  ? TimeSelectionPart::End
                                                  : TimeSelectionPart::Start);
    }
  }

  syncLineEdit();
  refreshRangeFooter();
  syncPopupActiveAlignment();

  if (!nextActive.has_value()) {
    commitPopupCalendarRange(true, true);
  }
}

void AdDateRangePicker::refreshRangeFooter() {
  if (!popupFooter_) {
    return;
  }

  const detail::DatePickerVisualStyle style =
      popupPanel_ ? popupPanel_->resolvedStyle() : resolveStyleForPanel(nullptr);
  const bool hasExtraFooter = extraFooterWidget_ != nullptr;
  const bool showNowAction = effectiveShowNowAction();
  const bool showOkAction = popupNeedsExplicitSubmit();
  const bool hasActions = showNowAction || showOkAction;
  const bool footerVisible = hasExtraFooter || hasActions;
  const int borderWidth = std::max(1, style.metrics.borderWidth);

  popupFooter_->setVisible(footerVisible);
  popupFooter_->setMinimumHeight(style.metrics.footerHeight);
  popupFooter_->setMaximumHeight(QWIDGETSIZE_MAX);
  popupFooter_->setStyleSheet(
      QStringLiteral("QWidget#addaterangepicker-popup-footer { background: %1; "
                     "border-top: %2px solid %3; }")
          .arg(cssColor(style.footerBackground))
          .arg(borderWidth)
          .arg(cssColor(style.footerBorderColor)));

  if (popupExtraFooterHost_) {
    if (extraFooterWidget_ && extraFooterWidget_->parentWidget() != popupExtraFooterHost_) {
      extraFooterWidget_->setParent(popupExtraFooterHost_);
      if (auto* layout = popupExtraFooterHost_->layout()) {
        layout->addWidget(extraFooterWidget_);
      }
      extraFooterWidget_->show();
    }
    const int extraFooterMinHeight =
        style.metrics.footerLineHeight + (hasActions ? borderWidth : 0);
    popupExtraFooterHost_->setFont(style.metrics.font);
    popupExtraFooterHost_->setMinimumHeight(extraFooterMinHeight);
    popupExtraFooterHost_->setMaximumHeight(QWIDGETSIZE_MAX);
    popupExtraFooterHost_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    popupExtraFooterHost_->setVisible(hasExtraFooter);
    popupExtraFooterHost_->setStyleSheet(
        QStringLiteral(
            "QWidget#addaterangepicker-popup-extra-footer { background: %1; color: %2; %3 }")
            .arg(cssColor(style.footerBackground))
            .arg(cssColor(style.textColor))
            .arg(hasActions ? QStringLiteral("border-bottom: %1px solid %2;")
                                  .arg(borderWidth)
                                  .arg(cssColor(style.footerBorderColor))
                            : QStringLiteral("border: none;")));
  }

  if (popupFooterActionsWidget_) {
    popupFooterActionsWidget_->setFixedHeight(style.metrics.footerHeight);
    popupFooterActionsWidget_->setVisible(hasActions);
  }

  if (popupNowButton_) {
    popupNowButton_->setFont(style.metrics.font);
    popupNowButton_->setText((pickerMode_ == PickerMode::Date && !showTime_) ? tr("Today")
                                                                             : tr("Now"));
    popupNowButton_->setVisible(showNowAction);
    popupNowButton_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    setButtonPalette(popupNowButton_, style.linkColor);
    popupNowButton_->setStyleSheet(
        QStringLiteral(
            "QToolButton { background: transparent; border: none; padding: 0; color: %1; }"
            "QToolButton:hover { background: transparent; color: %1; }"
            "QToolButton:disabled { background: transparent; color: %2; }")
            .arg(cssColor(style.linkColor), cssColor(style.disabledTextColor)));

    bool nowEnabled = !effectiveInputDisabled();
    if (nowEnabled && showNowAction) {
      RangePart activePart = activeRangePart();
      if ((activePart == RangePart::Start && startDisabled_) ||
          (activePart == RangePart::End && endDisabled_)) {
        activePart = activePart == RangePart::Start ? RangePart::End : RangePart::Start;
      }
      if ((activePart == RangePart::Start && startDisabled_) ||
          (activePart == RangePart::End && endDisabled_)) {
        nowEnabled = false;
      }

      const QDateTime now = QDateTime::currentDateTime();
      const QDate selected = normalizeForPicker(pickerMode_, now.date(), effectiveFirstDayOfWeek());
      QDate nextStart = popupCalendarStartDate();
      QDate nextEnd = popupCalendarEndDate();
      if (activePart == RangePart::End) {
        nextEnd = selected;
      } else {
        nextStart = selected;
      }
      mergeEndpointDisabledPopupRange(&nextStart, &nextEnd);
      const TimeSelectionPart timePart =
          activePart == RangePart::End ? TimeSelectionPart::End : TimeSelectionPart::Start;
      const QDate from = activePart == RangePart::End ? nextStart : nextEnd;
      const std::optional<QTime> validTime =
          validTimeForRangePart(selected, now.time(), activePart, from);
      nowEnabled = nowEnabled && selected.isValid() && validTime.has_value() &&
                   isDateTimeSelectable(dateTimeFromParts(selected, *validTime), timePart, from) &&
                   respectsEndpointDisabledOrder(nextStart, nextEnd);
    }
    popupNowButton_->setEnabled(nowEnabled);
  }

  if (popupOkButton_) {
    popupOkButton_->setFont(style.metrics.font);
    popupOkButton_->setVisible(showOkAction);
    popupOkButton_->setFixedHeight(std::max(20, style.metrics.cellHeight));
    popupOkButton_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    setButtonPalette(popupOkButton_, style.selectedTextColor);
    popupOkButton_->setStyleSheet(
        QStringLiteral("QToolButton { background: %1; border: %2px solid %1; border-radius: %3px; "
                       "padding: 0 7px; color: %4; }"
                       "QToolButton:hover { background: %1; border-color: %1; color: %4; }"
                       "QToolButton:disabled { background: %5; border-color: %5; color: %6; }")
            .arg(cssColor(style.selectedBackground))
            .arg(borderWidth)
            .arg(std::max(0, style.metrics.cellRadius))
            .arg(cssColor(style.selectedTextColor))
            .arg(cssColor(style.hoverBackground))
            .arg(cssColor(style.disabledTextColor)));

    QDate pendingStart = popupCalendarStartDate();
    QDate pendingEnd = popupCalendarEndDate();
    mergeEndpointDisabledPopupRange(&pendingStart, &pendingEnd);
    const bool okEnabled =
        !effectiveInputDisabled() && canConfirmActiveRangePart(pendingStart, pendingEnd);
    popupOkButton_->setEnabled(okEnabled);
  }

  popupFooter_->updateGeometry();
  if (popupMainWidget_) {
    popupMainWidget_->updateGeometry();
  }
  if (popupController_) {
    popupController_->invalidatePopupGeometry();
  }
}

void AdDateRangePicker::applyRangePreset(const PresetItem& preset) {
  if (effectiveInputDisabled()) {
    return;
  }

  const QDate presetValue = resolvedPresetValue(preset);
  const std::pair<QDate, QDate> presetRange = resolvedPresetRange(preset);
  QDate start = normalizeForPicker(pickerMode_, presetRange.first, effectiveFirstDayOfWeek());
  QDate end = normalizeForPicker(pickerMode_, presetRange.second, effectiveFirstDayOfWeek());
  if (!start.isValid() && presetValue.isValid()) {
    start = normalizeForPicker(pickerMode_, presetValue, effectiveFirstDayOfWeek());
  }
  if (!end.isValid() && !allowEmptyEnd_) {
    end = start;
  }
  if (order_ && !startDisabled_ && !endDisabled_ && start.isValid() && end.isValid() &&
      end < start) {
    std::swap(start, end);
  }
  mergeEndpointDisabledPopupRange(&start, &end);
  if (!canAcceptRangeForInteraction(start, end)) {
    return;
  }

  const QDate viewBase = start.isValid() ? start : end;
  if (viewBase.isValid() && popupPanel_) {
    QScopedValueRollback<bool> guard(syncingPopupPanels_, true);
    const QSignalBlocker primaryBlocker(popupPanel_);
    const std::optional<QSignalBlocker> secondaryBlocker =
        popupEndPanel_ ? std::optional<QSignalBlocker>(std::in_place, popupEndPanel_)
                       : std::nullopt;
    popupPanel_->setViewDate(adjustedPrimaryPanelViewDate(viewBase));
    if (popupEndPanel_) {
      popupEndPanel_->setViewDate(secondaryPanelViewDate(popupPanel_->viewDate()));
    }
  }

  popupCalendarActive_ = true;
  popupCalendarStartDate_ = start;
  popupCalendarEndDate_ = end;
  popupCalendarStartTime_ = start.isValid() ? popupCalendarStartTime() : defaultOpenStartTime_;
  popupCalendarEndTime_ = end.isValid() ? popupCalendarEndTime() : defaultOpenEndTime_;
  emit calendarChanged(start, end, lastFocusedRangePart_);
  syncPanelState();
  syncLineEdit();
  commitPopupCalendarRange(true, true);
}

void AdDateRangePicker::syncLineEditStyle() {
  if (!lineEdit_) {
    return;
  }
  const SemanticStyles semantic = effectiveSemanticStyles();
  applyInputSemanticColors(lineEdit_, semantic);
  applyPickerSuffixTokenColors(lineEdit_, semantic);
  lineEdit_->setControlSize(toInputSize(size_));
  lineEdit_->setVariant(toInputVariant(variant_));
  lineEdit_->setStatus(toInputStatus(status_));
  lineEdit_->setDisabled(effectiveInputDisabled());
  lineEdit_->setAllowClear(allowClear_);
  lineEdit_->setReadOnly(inputReadOnly_);
  lineEdit_->setTrailingActionLeading(true);
  lineEdit_->setTrailingActionVisible(suffixIconVisible_);
  lineEdit_->setTrailingActionAccessibleName(pickerMode_ == AdDatePickerPanel::PickerMode::Time
                                                 ? tr("Open time picker")
                                                 : tr("Open calendar"));
  lineEdit_->setPrefixText(prefixText_);
  lineEdit_->setSuffixText(suffixText_);
  lineEdit_->setPrefixIconRef(prefixIconRef_);
  const bool customSuffixIcon = adqt::icons::isValid(suffixIconRef_);
  lineEdit_->setSuffixIconRef(adqt::icons::IconRef());
  lineEdit_->setFeedbackIconRef(suffixIconVisible_ && !customSuffixIcon ? feedbackIconRef_
                                                                        : adqt::icons::IconRef());
  lineEdit_->setClearIconRef(clearIconRef_);
  lineEdit_->setTrailingActionIconRef(customSuffixIcon ? suffixIconRef_
                                                       : defaultPickerSuffixIcon(pickerMode_));
  lineEdit_->setProperty("adqt.datePicker.defaultSuffixIcon",
                         suffixIconVisible_ && !customSuffixIcon
                             ? defaultPickerSuffixIconName(pickerMode_)
                             : QString());
  lineEdit_->setDateTagTokens(componentTokens_, size_);
  syncLineEditRangeDisplay(startDate_, endDate_, startTime_, endTime_);
}

void AdDateRangePicker::syncPanelState() {
  if (!popupPanel_) {
    return;
  }

  QScopedValueRollback<bool> guard(syncingPopupPanels_, true);
  const QSignalBlocker primaryBlocker(popupPanel_);
  const std::optional<QSignalBlocker> secondaryBlocker =
      popupEndPanel_ ? std::optional<QSignalBlocker>(std::in_place, popupEndPanel_) : std::nullopt;

  const QDate previousPrimaryViewDate = popupPanel_->viewDate();
  QDate primaryViewDate = pickerValue_;
  if (!primaryViewDate.isValid() && popupVisible_ && previousPrimaryViewDate.isValid()) {
    primaryViewDate = previousPrimaryViewDate;
  }
  const QDate calendarStart = popupCalendarStartDate();
  const QDate calendarEnd = popupCalendarEndDate();
  const QTime calendarStartTime = popupCalendarStartTime();
  const QTime calendarEndTime = popupCalendarEndTime();

  if (!primaryViewDate.isValid()) {
    primaryViewDate =
        defaultPickerValue_.isValid()
            ? defaultPickerValue_
            : (calendarStart.isValid() ? calendarStart
                                       : (calendarEnd.isValid() ? calendarEnd : todayDate()));
  }

  const SemanticStyles semantic = effectiveSemanticStyles();
  const bool showSecondaryPanel = pickerMode_ != PickerMode::Time && !showTime_;
  const bool needsContextPredicate =
      startDisabled_ || endDisabled_ || static_cast<bool>(disabledDateContextPredicate_);
  primaryViewDate = adjustedPrimaryPanelViewDate(primaryViewDate);
  const auto configurePanel = [this, &semantic, showSecondaryPanel, &calendarStart, &calendarEnd,
                               &calendarStartTime,
                               &calendarEndTime](AdDatePickerPanel* panel, bool primary) {
    if (!panel) {
      return;
    }
    panel->setPickerMode(pickerMode_);
    panel->setLocale(locale_);
    panel->setMinDate(minDate_);
    panel->setMaxDate(maxDate_);
    panel->setSelectionMode(AdDatePickerPanel::SelectionMode::Range);
    panel->setOrder(order_ && !startDisabled_ && !endDisabled_);
    panel->setRange(calendarStart, calendarEnd);
    panel->setPanelMode(effectivePanelMode());
    panel->setDefaultOpenTimeRange(defaultOpenStartTime_, defaultOpenEndTime_);
    panel->setTimeRange(calendarStart.isValid() ? calendarStartTime : defaultOpenStartTime_,
                        calendarEnd.isValid() ? calendarEndTime : defaultOpenEndTime_);
    if (pickerMode_ == PickerMode::Time) {
      panel->setVisibleRangeTimePart(
          showSecondaryPanel
              ? (primary ? TimeSelectionPart::Start : TimeSelectionPart::End)
              : (lastFocusedRangePart_ == RangePart::End ? TimeSelectionPart::End
                                                         : TimeSelectionPart::Start));
    } else {
      panel->setVisibleRangeTimePart(lastFocusedRangePart_ == RangePart::End
                                         ? TimeSelectionPart::End
                                         : TimeSelectionPart::Start);
    }
    panel->setShowToday(false);
    panel->setShowWeek(pickerMode_ == PickerMode::Week);
    panel->setNeedConfirm(needConfirm_ || allowEmptyStart_ || allowEmptyEnd_ || showTime_);
    panel->setShowTime(primary ? showTime_ : false);
    panel->setTimeFormat(effectiveTimeFormat());
    panel->setTimeSteps(hourStep_, minuteStep_, secondStep_);
    panel->setHideDisabledOptions(hideDisabledOptions_);
    panel->setUse12Hours(effectiveUse12Hours());
    panel->setChangeOnScroll(changeOnScroll_);
    panel->setShowHour(showHour_);
    panel->setShowMinute(showMinute_);
    if (showSecondExplicit_) {
      panel->setShowSecond(showSecond_);
    } else {
      panel->resetShowSecond();
    }
    panel->setAllowEmpty(allowEmptyStart_, allowEmptyEnd_);
    panel->setDisabled(effectiveInputDisabled());
    panel->setComponentTokens(componentTokens_);
    panel->setSemanticStyles(semantic.popup);
    panel->setPresets({});
    panel->setExtraFooterWidget(nullptr);
    panel->setNavigationIconRefs(superPrevIconRef_, prevIconRef_, nextIconRef_, superNextIconRef_);
    panel->setHidePreviousNavigation(!primary && showSecondaryPanel);
    panel->setHideNextNavigation(primary && showSecondaryPanel);
    panel->setFooterVisible(false);
  };

  configurePanel(popupPanel_, true);
  configurePanel(popupEndPanel_, false);

  const auto applyToPanels = [this](auto&& apply) {
    apply(popupPanel_);
    if (popupEndPanel_) {
      apply(popupEndPanel_);
    }
  };
  if (panelDisabledDatePredicateDirty_) {
    applyToPanels([this](AdDatePickerPanel* panel) {
      if (panel) {
        panel->setDisabledDatePredicate(disabledDatePredicate_);
      }
    });
    panelDisabledDatePredicateDirty_ = false;
  }
  if (panelDisabledDateContextPredicateDirty_) {
    applyToPanels([this, needsContextPredicate](AdDatePickerPanel* panel) {
      if (!panel) {
        return;
      }
      if (needsContextPredicate) {
        panel->setDisabledDateContextPredicate(
            [this](const QDate& value, const DisabledDateContext& context) {
              if (isDisabledEndpointCrossingCandidate(value)) {
                return true;
              }
              return disabledDateContextPredicate_ && disabledDateContextPredicate_(value, context);
            });
      } else {
        panel->setDisabledDateContextPredicate(DisabledDatePredicate());
      }
    });
    panelDisabledDateContextPredicateDirty_ = false;
  }
  if (panelDisabledTimePredicateDirty_) {
    applyToPanels([this](AdDatePickerPanel* panel) {
      if (panel) {
        panel->setDisabledTimePredicate(disabledTimePredicate_);
      }
    });
    panelDisabledTimePredicateDirty_ = false;
  }
  if (panelCellRenderCallbackDirty_) {
    applyToPanels([this](AdDatePickerPanel* panel) {
      if (panel) {
        panel->setCellRenderCallback(cellRenderCallback_);
      }
    });
    panelCellRenderCallbackDirty_ = false;
  }

  if (popupSecondaryContentWidget_) {
    popupSecondaryContentWidget_->setVisible(showSecondaryPanel);
  } else if (popupEndPanel_) {
    popupEndPanel_->setVisible(showSecondaryPanel);
  }
  popupPanel_->setViewDate(primaryViewDate);
  const QDate boundedPrimaryViewDate = popupPanel_->viewDate();
  if (popupEndPanel_) {
    popupEndPanel_->setViewDate(secondaryPanelViewDate(boundedPrimaryViewDate));
  }
  refreshRangePresets();
  refreshRangeFooter();
}

void AdDateRangePicker::syncPopupPanelViewsFromPrimary(const QDate& primaryViewDate) {
  if (syncingPopupPanels_ || !popupEndPanel_) {
    return;
  }
  QScopedValueRollback<bool> guard(syncingPopupPanels_, true);
  const QDate adjustedPrimary = adjustedPrimaryPanelViewDate(primaryViewDate);
  if (popupPanel_ && adjustedPrimary != primaryViewDate) {
    const QSignalBlocker primaryBlocker(popupPanel_);
    popupPanel_->setViewDate(adjustedPrimary);
  }
  const QSignalBlocker blocker(popupEndPanel_);
  popupEndPanel_->setViewDate(secondaryPanelViewDate(adjustedPrimary));
}

void AdDateRangePicker::syncPopupPanelViewsFromSecondary(const QDate& secondaryViewDate) {
  if (syncingPopupPanels_ || !popupPanel_) {
    return;
  }
  QScopedValueRollback<bool> guard(syncingPopupPanels_, true);
  const QSignalBlocker blocker(popupPanel_);
  popupPanel_->setViewDate(adjustedPrimaryPanelViewDate(primaryPanelViewDate(secondaryViewDate)));
}

void AdDateRangePicker::handlePopupRangeChanged(AdDatePickerPanel* sourcePanel, const QDate& start,
                                                const QDate& end) {
  if (syncingPopupPanels_ || effectiveInputDisabled()) {
    return;
  }

  QDate previousStart = popupCalendarStartDate();
  QDate previousEnd = popupCalendarEndDate();
  QTime previousStartTime = popupCalendarStartTime();
  QTime previousEndTime = popupCalendarEndTime();

  RangePart activePart = lastFocusedRangePart_;
  if (endDisabled_ && !startDisabled_) {
    activePart = RangePart::Start;
  } else if ((startDisabled_ && !endDisabled_) || sourcePanel == popupEndPanel_) {
    activePart = RangePart::End;
  }

  QDate selected = activePart == RangePart::End ? (end.isValid() ? end : start)
                                                : (start.isValid() ? start : end);
  selected = normalizeForPicker(pickerMode_, selected, effectiveFirstDayOfWeek());
  if (!selected.isValid()) {
    return;
  }

  QDate nextStart = previousStart;
  QDate nextEnd = previousEnd;
  QTime nextStartTime = previousStartTime;
  QTime nextEndTime = previousEndTime;
  if (activePart == RangePart::End) {
    nextEnd = selected;
    if (!nextEndTime.isValid()) {
      nextEndTime = defaultOpenEndTime_;
    }
  } else {
    nextStart = selected;
    if (!nextStartTime.isValid()) {
      nextStartTime = defaultOpenStartTime_;
    }
  }

  mergeEndpointDisabledPopupRange(&nextStart, &nextEnd);

  popupCalendarActive_ = true;
  popupCalendarStartDate_ = nextStart;
  popupCalendarEndDate_ = nextEnd;
  popupCalendarStartTime_ =
      nextStart.isValid() ? normalizedTimeValue(nextStartTime) : defaultOpenStartTime_;
  popupCalendarEndTime_ =
      nextEnd.isValid() ? normalizedTimeValue(nextEndTime) : defaultOpenEndTime_;

  emit calendarChanged(nextStart, nextEnd, activePart);

  // The endpoint changed by this click is part of the current edit sequence.
  // Record it before deciding whether the next step is the other endpoint.
  setActiveRangePart(activePart, true);
  const std::optional<RangePart> nextActive = nextActiveRangePart(nextStart, nextEnd);
  if (nextActive.has_value()) {
    setActiveRangePart(*nextActive, true);
  } else {
    setActiveRangePart(activePart, false);
  }
  syncPopupActiveAlignment();

  {
    QScopedValueRollback<bool> guard(syncingPopupPanels_, true);
    const QSignalBlocker primaryBlocker(popupPanel_);
    const std::optional<QSignalBlocker> secondaryBlocker =
        popupEndPanel_ ? std::optional<QSignalBlocker>(std::in_place, popupEndPanel_)
                       : std::nullopt;
    if (popupPanel_) {
      popupPanel_->setRange(nextStart, nextEnd);
      popupPanel_->setVisibleRangeTimePart(lastFocusedRangePart_ == RangePart::End
                                               ? TimeSelectionPart::End
                                               : TimeSelectionPart::Start);
    }
    if (popupEndPanel_) {
      popupEndPanel_->setRange(nextStart, nextEnd);
      popupEndPanel_->setVisibleRangeTimePart(lastFocusedRangePart_ == RangePart::End
                                                  ? TimeSelectionPart::End
                                                  : TimeSelectionPart::Start);
    }
    if (popupPanel_) {
      popupPanel_->setHoverRange(nextStart, nextEnd);
    }
    if (popupEndPanel_) {
      popupEndPanel_->setHoverRange(nextStart, nextEnd);
    }
  }

  syncLineEdit();
  moveCursorToRangePart(lastFocusedRangePart_);

  if (popupNeedsExplicitSubmit() || nextActive.has_value()) {
    return;
  }
  commitPopupCalendarRange(true, true);
}

void AdDateRangePicker::handlePopupRangeTimeChanged(AdDatePickerPanel* sourcePanel,
                                                    const QTime& start, const QTime& end) {
  if (syncingPopupPanels_ || effectiveInputDisabled() ||
      (pickerMode_ != PickerMode::Time && !showTime_)) {
    return;
  }

  const TimeSelectionPart timePart =
      sourcePanel ? sourcePanel->visibleRangeTimePart() : TimeSelectionPart::Start;
  const RangePart changedPart =
      timePart == TimeSelectionPart::End ? RangePart::End : RangePart::Start;

  QDate baseDate =
      popupCalendarStartDate().isValid() ? popupCalendarStartDate() : popupCalendarEndDate();
  if (!baseDate.isValid()) {
    baseDate =
        pickerValue_.isValid()
            ? pickerValue_
            : (defaultPickerValue_.isValid()
                   ? defaultPickerValue_
                   : (sourcePanel && sourcePanel->viewDate().isValid() ? sourcePanel->viewDate()
                                                                       : todayDate()));
  }

  QDate nextStart = popupCalendarStartDate();
  QDate nextEnd = popupCalendarEndDate();
  QTime nextStartTime = popupCalendarStartTime();
  QTime nextEndTime = popupCalendarEndTime();
  if (changedPart == RangePart::End) {
    if (!endDisabled_) {
      nextEnd = nextEnd.isValid() ? nextEnd : baseDate;
      nextEndTime = normalizedTimeValue(end);
    }
  } else if (!startDisabled_) {
    nextStart = nextStart.isValid() ? nextStart : baseDate;
    nextStartTime = normalizedTimeValue(start);
  }

  mergeEndpointDisabledPopupRange(&nextStart, &nextEnd);
  if (!respectsEndpointDisabledOrder(nextStart, nextEnd)) {
    return;
  }
  if (!startDisabled_ && nextStart.isValid() &&
      !isDateTimeSelectable(dateTimeFromParts(nextStart, nextStartTime), TimeSelectionPart::Start,
                            nextEnd)) {
    return;
  }
  if (!endDisabled_ && nextEnd.isValid() &&
      !isDateTimeSelectable(dateTimeFromParts(nextEnd, nextEndTime), TimeSelectionPart::End,
                            nextStart)) {
    return;
  }

  emit calendarChanged(nextStart, nextEnd, changedPart);
  popupCalendarActive_ = true;
  popupCalendarStartDate_ = nextStart;
  popupCalendarEndDate_ = nextEnd;
  popupCalendarStartTime_ =
      nextStart.isValid() ? normalizedTimeValue(nextStartTime) : defaultOpenStartTime_;
  popupCalendarEndTime_ =
      nextEnd.isValid() ? normalizedTimeValue(nextEndTime) : defaultOpenEndTime_;
  setActiveRangePart(changedPart, true);
  syncLineEdit();
  if (!popupNeedsExplicitSubmit()) {
    commitPopupCalendarRange(true, true);
  }
}

void AdDateRangePicker::handlePopupRangeAccepted(const QDate& start, const QDate& end) {
  if (effectiveInputDisabled() || (!popupVisible_ && !popupCalendarActive_)) {
    return;
  }
  QDate nextStart = start;
  QDate nextEnd = end;
  mergeEndpointDisabledPopupRange(&nextStart, &nextEnd);
  if (!canAcceptRangeForInteraction(nextStart, nextEnd)) {
    return;
  }
  const QTime startTime = popupPanel_ ? popupPanel_->rangeStartTime() : startTime_;
  const QTime endTime = popupPanel_ ? popupPanel_->rangeEndTime() : endTime_;
  popupCalendarActive_ = true;
  popupCalendarStartDate_ = nextStart;
  popupCalendarEndDate_ = nextEnd;
  popupCalendarStartTime_ = startDisabled_ ? startTime_ : startTime;
  popupCalendarEndTime_ = endDisabled_ ? endTime_ : endTime;
  confirmActiveRangePart(true, true);
}

void AdDateRangePicker::syncPopupCalendarFromCommitted() {
  popupCalendarActive_ = true;
  popupCalendarStartDate_ = startDate_;
  popupCalendarEndDate_ = endDate_;
  popupCalendarStartTime_ = startDate_.isValid() ? startTime_ : defaultOpenStartTime_;
  popupCalendarEndTime_ = endDate_.isValid() ? endTime_ : defaultOpenEndTime_;
}

QDate AdDateRangePicker::popupCalendarStartDate() const {
  return popupCalendarActive_ ? popupCalendarStartDate_ : startDate_;
}

QDate AdDateRangePicker::popupCalendarEndDate() const {
  return popupCalendarActive_ ? popupCalendarEndDate_ : endDate_;
}

QTime AdDateRangePicker::popupCalendarStartTime() const {
  return popupCalendarActive_ ? normalizedTimeValue(popupCalendarStartTime_)
                              : (startDate_.isValid() ? startTime_ : defaultOpenStartTime_);
}

QTime AdDateRangePicker::popupCalendarEndTime() const {
  return popupCalendarActive_ ? normalizedTimeValue(popupCalendarEndTime_)
                              : (endDate_.isValid() ? endTime_ : defaultOpenEndTime_);
}

void AdDateRangePicker::setActiveRangePart(RangePart range, bool record) {
  if ((range == RangePart::Start && startDisabled_ && !endDisabled_) ||
      (range == RangePart::End && endDisabled_ && !startDisabled_)) {
    range = range == RangePart::Start ? RangePart::End : RangePart::Start;
  }

  lastFocusedRangePart_ = range;
  const TimeSelectionPart visiblePart =
      range == RangePart::End ? TimeSelectionPart::End : TimeSelectionPart::Start;
  if (popupPanel_) {
    popupPanel_->setVisibleRangeTimePart(visiblePart);
  }
  if (popupEndPanel_) {
    popupEndPanel_->setVisibleRangeTimePart(visiblePart);
  }
  if (record) {
    if (activeRangeHistory_.isEmpty() || activeRangeHistory_.last() != range) {
      activeRangeHistory_.append(range);
    }
    while (activeRangeHistory_.size() > 2) {
      activeRangeHistory_.removeFirst();
    }
  }
}

std::optional<AdDateRangePicker::RangePart> AdDateRangePicker::nextActiveRangePart(
    const QDate& start, const QDate& end) const {
  const bool startFilled = start.isValid() || allowEmptyStart_;
  const bool endFilled = end.isValid() || allowEmptyEnd_;
  bool activeStartFilled = false;
  bool activeEndFilled = false;
  for (RangePart part : activeRangeHistory_) {
    if (part == RangePart::Start && startFilled) {
      activeStartFilled = true;
    } else if (part == RangePart::End && endFilled) {
      activeEndFilled = true;
    }
  }
  if (activeStartFilled && activeEndFilled) {
    return std::nullopt;
  }

  const RangePart next =
      activeRangeHistory_.isEmpty() || activeRangeHistory_.last() == RangePart::Start
          ? RangePart::End
          : RangePart::Start;
  if (next == RangePart::Start && !startDisabled_) {
    return RangePart::Start;
  }
  if (next == RangePart::End && !endDisabled_) {
    return RangePart::End;
  }
  return std::nullopt;
}

bool AdDateRangePicker::canConfirmActiveRangePart(const QDate& start, const QDate& end) const {
  QDate pendingStart = start;
  QDate pendingEnd = end;
  mergeEndpointDisabledPopupRange(&pendingStart, &pendingEnd);

  if (!pendingStart.isValid() && !pendingEnd.isValid()) {
    return false;
  }

  RangePart activePart = lastFocusedRangePart_;
  if (activePart == RangePart::Start && startDisabled_ && !endDisabled_) {
    activePart = RangePart::End;
  } else if (activePart == RangePart::End && endDisabled_ && !startDisabled_) {
    activePart = RangePart::Start;
  }

  const bool activeFilled = activePart == RangePart::Start
                                ? (pendingStart.isValid() || allowEmptyStart_)
                                : (pendingEnd.isValid() || allowEmptyEnd_);
  if (!activeFilled || !respectsEndpointDisabledOrder(pendingStart, pendingEnd)) {
    return false;
  }

  const QTime pendingStartTime = popupCalendarStartTime();
  const QTime pendingEndTime = popupCalendarEndTime();
  if (!startDisabled_ && pendingStart.isValid() &&
      !isDateTimeSelectable(dateTimeFromParts(pendingStart, pendingStartTime),
                            TimeSelectionPart::Start, pendingEnd)) {
    return false;
  }
  if (!endDisabled_ && pendingEnd.isValid() &&
      !isDateTimeSelectable(dateTimeFromParts(pendingEnd, pendingEndTime), TimeSelectionPart::End,
                            pendingStart)) {
    return false;
  }

  return true;
}

bool AdDateRangePicker::confirmActiveRangePart(bool closePopup, bool emitAccepted) {
  RangePart activePart = lastFocusedRangePart_;
  setActiveRangePart(activePart, true);

  QDate nextStart = popupCalendarStartDate();
  QDate nextEnd = popupCalendarEndDate();
  const QTime nextStartTime = popupCalendarStartTime();
  const QTime nextEndTime = popupCalendarEndTime();
  mergeEndpointDisabledPopupRange(&nextStart, &nextEnd);
  if (!canConfirmActiveRangePart(nextStart, nextEnd)) {
    syncLineEditRangeDisplay(popupCalendarStartDate(), popupCalendarEndDate(),
                             popupCalendarStartTime(), popupCalendarEndTime());
    refreshRangeFooter();
    return false;
  }

  popupCalendarActive_ = true;
  popupCalendarStartDate_ = nextStart;
  popupCalendarEndDate_ = nextEnd;
  popupCalendarStartTime_ =
      nextStart.isValid() ? normalizedTimeValue(nextStartTime) : defaultOpenStartTime_;
  popupCalendarEndTime_ =
      nextEnd.isValid() ? normalizedTimeValue(nextEndTime) : defaultOpenEndTime_;

  const std::optional<RangePart> nextActive = nextActiveRangePart(nextStart, nextEnd);
  if (!nextActive.has_value()) {
    return commitPopupCalendarRange(closePopup, emitAccepted);
  }

  setActiveRangePart(*nextActive, true);
  syncPanelState();
  syncLineEdit();
  moveCursorToRangePart(lastFocusedRangePart_);
  syncPopupActiveAlignment();
  return true;
}

bool AdDateRangePicker::popupNeedsExplicitSubmit() const {
  return needConfirm_ || allowEmptyStart_ || allowEmptyEnd_ || showTime_ ||
         pickerMode_ == PickerMode::Time;
}

bool AdDateRangePicker::commitPopupCalendarRange(bool closePopup, bool emitAccepted) {
  QDate nextStart = popupCalendarStartDate();
  QDate nextEnd = popupCalendarEndDate();
  mergeEndpointDisabledPopupRange(&nextStart, &nextEnd);
  if (!canAcceptRangeForInteraction(nextStart, nextEnd)) {
    syncLineEditRangeDisplay(popupCalendarStartDate(), popupCalendarEndDate(),
                             popupCalendarStartTime(), popupCalendarEndTime());
    return false;
  }

  QTime nextStartTime = startDisabled_
                            ? startTime_
                            : (nextStart.isValid() ? popupCalendarStartTime() : defaultTimeValue());
  QTime nextEndTime =
      endDisabled_ ? endTime_ : (nextEnd.isValid() ? popupCalendarEndTime() : defaultTimeValue());
  if (nextStart.isValid() && !isDateTimeSelectable(dateTimeFromParts(nextStart, nextStartTime),
                                                   TimeSelectionPart::Start, nextEnd)) {
    return false;
  }
  if (nextEnd.isValid() && !isDateTimeSelectable(dateTimeFromParts(nextEnd, nextEndTime),
                                                 TimeSelectionPart::End, nextStart)) {
    return false;
  }

  popupCalendarActive_ = false;
  setDateTimeRange(dateTimeFromParts(nextStart, nextStartTime),
                   dateTimeFromParts(nextEnd, nextEndTime));
  activeRangeHistory_.clear();
  if (closePopup) {
    QScopedValueRollback<bool> closeGuard(suppressPopupCloseSubmit_, true);
    hidePopup();
  } else {
    syncLineEdit();
  }
  if (emitAccepted) {
    emit accepted(startDate_, endDate_);
    emit acceptedDateTimeRange(startDateTime(), endDateTime());
  }
  return true;
}

void AdDateRangePicker::commitInputText() {
  if (!lineEdit_ || syncingText_ || popupVisible_) {
    return;
  }
  if (effectiveInputDisabled()) {
    syncLineEdit();
    return;
  }

  const QString rawText = lineEdit_->text();
  if (rawText.trimmed().isEmpty()) {
    clearRangeInternal(true, true);
    return;
  }

  QStringList separatorPatterns = {
      QStringLiteral("->"),
      QStringLiteral("~"),
      QStringLiteral("\\bto\\b"),
  };
  const QString customSeparator = effectiveSeparator().trimmed();
  if (!customSeparator.isEmpty()) {
    separatorPatterns.prepend(QRegularExpression::escape(customSeparator));
  }
  const QRegularExpression separator(
      QStringLiteral("\\s*(?:%1)\\s*").arg(separatorPatterns.join(QLatin1Char('|'))),
      QRegularExpression::CaseInsensitiveOption);
  const QStringList parts = rawText.trimmed().split(separator, Qt::KeepEmptyParts);
  if (parts.size() != 2) {
    if (preserveInvalidOnBlur_) {
      lineEdit_->clearRangeInputDisplay();
    } else {
      syncLineEdit();
    }
    return;
  }

  bool startOk = false;
  bool endOk = false;
  QDateTime start;
  QDateTime end;
  if (startDisabled_) {
    start = startDateTime();
    startOk = start.isValid() || allowEmptyStart_;
  } else if (parts.at(0).trimmed().isEmpty()) {
    startOk = allowEmptyStart_;
  } else {
    start = maskFormat_ ? parseMaskedText(parts.at(0), startTime_, &startOk)
                        : parsePickerDateTimeText(pickerMode_, parts.at(0), effectiveParseFormats(),
                                                  locale_, effectiveFirstDayOfWeek(), startTime_,
                                                  effectiveTextIncludesTime(), &startOk);
  }
  if (endDisabled_) {
    end = endDateTime();
    endOk = end.isValid() || allowEmptyEnd_;
  } else if (parts.at(1).trimmed().isEmpty()) {
    endOk = allowEmptyEnd_;
  } else {
    end = maskFormat_ ? parseMaskedText(parts.at(1), endTime_, &endOk)
                      : parsePickerDateTimeText(pickerMode_, parts.at(1), effectiveParseFormats(),
                                                locale_, effectiveFirstDayOfWeek(), endTime_,
                                                effectiveTextIncludesTime(), &endOk);
  }

  const QDate startDate = start.isValid() ? start.date() : QDate();
  const QDate endDate = end.isValid() ? end.date() : QDate();
  const bool startTimeOk = startDisabled_ || !start.isValid() ||
                           isDateTimeSelectable(start, TimeSelectionPart::Start, endDate);
  const bool endTimeOk = endDisabled_ || !end.isValid() ||
                         isDateTimeSelectable(end, TimeSelectionPart::End, startDate);
  if (startOk && endOk && canAcceptRangeForInteraction(startDate, endDate) && startTimeOk &&
      endTimeOk) {
    setDateTimeRange(start, end);
    emit accepted(startDate_, endDate_);
    emit acceptedDateTimeRange(startDateTime(), endDateTime());
    return;
  }

  if (preserveInvalidOnBlur_) {
    lineEdit_->clearRangeInputDisplay();
  } else {
    syncLineEdit();
  }
}

bool AdDateRangePicker::canAcceptRange(const QDate& start, const QDate& end) const {
  if (!rangeEndpointsAcceptable(start, end, allowEmptyStart_, allowEmptyEnd_)) {
    return false;
  }
  if (!respectsEndpointDisabledOrder(start, end)) {
    return false;
  }
  if (start.isValid() && !isDateSelectable(start, end)) {
    return false;
  }
  if (end.isValid() && !isDateSelectable(end, start)) {
    return false;
  }
  return true;
}

std::optional<QTime> AdDateRangePicker::validTimeForRangePart(const QDate& date,
                                                              const QTime& preferred,
                                                              RangePart range,
                                                              const QDate& from) const {
  if (!showTime_ && pickerMode_ != PickerMode::Time) {
    return defaultTimeValue();
  }

  const QTime base = normalizedTimeValue(preferred);
  const TimeSelectionPart part =
      range == RangePart::End ? TimeSelectionPart::End : TimeSelectionPart::Start;
  const auto selectable = [this, &date, part, &from](const QTime& time) {
    return isDateTimeSelectable(dateTimeFromParts(date, time), part, from);
  };
  const auto valuesForStep = [](int end, int step) {
    QVector<int> values;
    const int normalizedStep = std::max(1, step);
    for (int value = 0; value <= end; value += normalizedStep) {
      values.append(value);
    }
    return values;
  };
  const QVector<int> hours = valuesForStep(23, hourStep_);
  const QVector<int> minutes = valuesForStep(59, minuteStep_);
  const QVector<int> seconds = valuesForStep(59, secondStep_);
  const auto alignedUnit = [](int preferredValue, const QVector<int>& values,
                              const std::function<bool(int)>& enabled) -> std::optional<int> {
    if (values.contains(preferredValue) && enabled(preferredValue)) {
      return preferredValue;
    }

    std::optional<int> firstEnabled;
    std::optional<int> closestLower;
    for (int value : values) {
      if (!enabled(value)) {
        continue;
      }
      if (!firstEnabled.has_value()) {
        firstEnabled = value;
      }
      if (value <= preferredValue) {
        closestLower = value;
      }
    }
    return closestLower.has_value() ? closestLower : firstEnabled;
  };

  const std::optional<int> hour = alignedUnit(base.hour(), hours, [&](int value) {
    return selectable(QTime(value, base.minute(), base.second()));
  });
  if (!hour.has_value()) {
    return std::nullopt;
  }
  const std::optional<int> minute = alignedUnit(base.minute(), minutes, [&](int value) {
    return selectable(QTime(*hour, value, base.second()));
  });
  if (!minute.has_value()) {
    return std::nullopt;
  }
  const std::optional<int> second = alignedUnit(
      base.second(), seconds, [&](int value) { return selectable(QTime(*hour, *minute, value)); });
  if (!second.has_value()) {
    return std::nullopt;
  }

  const QTime aligned(*hour, *minute, *second);
  if (selectable(aligned)) {
    return aligned;
  }

  const int targetSeconds = base.hour() * 3600 + base.minute() * 60 + base.second();
  std::optional<QTime> firstSelectable;
  std::optional<QTime> closestLower;
  int closestLowerSeconds = -1;
  for (int h : hours) {
    for (int m : minutes) {
      for (int s : seconds) {
        const QTime candidate(h, m, s);
        if (!selectable(candidate)) {
          continue;
        }
        if (!firstSelectable.has_value()) {
          firstSelectable = candidate;
        }
        const int candidateSeconds = h * 3600 + m * 60 + s;
        if (candidateSeconds <= targetSeconds && candidateSeconds >= closestLowerSeconds) {
          closestLower = candidate;
          closestLowerSeconds = candidateSeconds;
        }
      }
    }
  }
  return closestLower.has_value() ? closestLower : firstSelectable;
}

QDate AdDateRangePicker::adjustedPrimaryPanelViewDate(const QDate& primaryViewDate) const {
  QDate adjusted = primaryViewDate.isValid() ? primaryViewDate : todayDate();
  const bool showSecondaryPanel = pickerMode_ != PickerMode::Time && !showTime_;
  QDate lower;
  QDate upper;
  normalizedDateBounds(minDate_, maxDate_, &lower, &upper);
  if (lower.isValid() && adjusted < lower) {
    adjusted = lower;
  }
  if (upper.isValid()) {
    if (showSecondaryPanel) {
      const QDate secondaryView = secondaryPanelViewDate(adjusted);
      if (secondaryView.isValid() && secondaryView > upper) {
        adjusted = primaryPanelViewDate(upper);
      }
    } else if (adjusted > upper) {
      adjusted = upper;
    }
  }
  return adjusted;
}

QDate AdDateRangePicker::secondaryPanelViewDate(const QDate& primaryViewDate) const {
  const QDate base = primaryViewDate.isValid() ? primaryViewDate : todayDate();
  switch (pickerMode_) {
    case PickerMode::Year:
      return base.addYears(10);
    case PickerMode::Decade:
      return base.addYears(100);
    case PickerMode::Time:
      return base;
    case PickerMode::Month:
    case PickerMode::Quarter:
      return base.addYears(1);
    case PickerMode::Week:
    case PickerMode::Date:
    default:
      return base.addMonths(1);
  }
}

QDate AdDateRangePicker::primaryPanelViewDate(const QDate& secondaryViewDate) const {
  const QDate base = secondaryViewDate.isValid() ? secondaryViewDate : todayDate();
  switch (pickerMode_) {
    case PickerMode::Year:
      return base.addYears(-10);
    case PickerMode::Decade:
      return base.addYears(-100);
    case PickerMode::Time:
      return base;
    case PickerMode::Month:
    case PickerMode::Quarter:
      return base.addYears(-1);
    case PickerMode::Week:
    case PickerMode::Date:
    default:
      return base.addMonths(-1);
  }
}

QString AdDateRangePicker::effectiveDisplayText(const QDate& value) const {
  if (displayTextCallback_ && value.isValid()) {
    return displayTextCallback_(value, defaultTimeValue());
  }
  return formatDefaultDate(value, pickerMode_, defaultDisplayFormat(), locale_,
                           effectiveFirstDayOfWeek());
}

QString AdDateRangePicker::effectiveDisplayText(const QDate& value, const QTime& time) const {
  if (displayTextCallback_ && (value.isValid() || pickerMode_ == PickerMode::Time)) {
    return displayTextCallback_(value, time);
  }
  return formatDefaultDateTime(value, time, pickerMode_, defaultDisplayFormat(), locale_,
                               effectiveFirstDayOfWeek(), effectiveTextIncludesTime());
}

QString AdDateRangePicker::effectiveRangeText() const {
  return effectiveRangeText(startDate_, endDate_, startTime_, endTime_);
}

QString AdDateRangePicker::effectiveRangeText(const QDate& start, const QDate& end,
                                              const QTime& startTime, const QTime& endTime) const {
  if (!start.isValid() && !end.isValid()) {
    return QString();
  }
  const QString startText = effectiveDisplayText(start, startTime);
  const QString endText = effectiveDisplayText(end, endTime);
  return startText + effectiveSeparator() + endText;
}

QString AdDateRangePicker::effectivePlaceholder() const {
  if (!placeholder_.isEmpty()) {
    return placeholder_;
  }
  return effectiveStartPlaceholder() + effectiveSeparator() + effectiveEndPlaceholder();
}

QString AdDateRangePicker::effectiveStartPlaceholder() const {
  if (!startPlaceholder_.isEmpty()) {
    return startPlaceholder_;
  }
  if (!placeholder_.isEmpty()) {
    const qsizetype separatorIndex = placeholder_.indexOf(effectiveSeparator());
    if (separatorIndex >= 0) {
      return placeholder_.left(separatorIndex).trimmed();
    }
    return placeholder_;
  }

  QString startText;
  switch (pickerMode_) {
    case PickerMode::Week:
      startText = tr("Start week");
      break;
    case PickerMode::Month:
      startText = tr("Start month");
      break;
    case PickerMode::Quarter:
      startText = tr("Start quarter");
      break;
    case PickerMode::Year:
      startText = tr("Start year");
      break;
    case PickerMode::Decade:
      startText = tr("Start decade");
      break;
    case PickerMode::Time:
      startText = tr("Start time");
      break;
    case PickerMode::Date:
    default:
      startText = tr("Start date");
      break;
  }
  return startText;
}

QString AdDateRangePicker::effectiveEndPlaceholder() const {
  if (!endPlaceholder_.isEmpty()) {
    return endPlaceholder_;
  }
  if (!placeholder_.isEmpty()) {
    const qsizetype separatorIndex = placeholder_.indexOf(effectiveSeparator());
    if (separatorIndex >= 0) {
      return placeholder_.mid(separatorIndex + effectiveSeparator().size()).trimmed();
    }
    return QString();
  }

  QString endText;
  switch (pickerMode_) {
    case PickerMode::Week:
      endText = tr("End week");
      break;
    case PickerMode::Month:
      endText = tr("End month");
      break;
    case PickerMode::Quarter:
      endText = tr("End quarter");
      break;
    case PickerMode::Year:
      endText = tr("End year");
      break;
    case PickerMode::Decade:
      endText = tr("End decade");
      break;
    case PickerMode::Time:
      endText = tr("End time");
      break;
    case PickerMode::Date:
    default:
      endText = tr("End date");
      break;
  }
  return endText;
}

QString AdDateRangePicker::effectiveSeparator() const {
  return separator_.isEmpty() ? defaultRangeSeparator() : separator_;
}

QString AdDateRangePicker::defaultDisplayFormat() const {
  const QStringList formats = effectiveParseFormats();
  return formats.isEmpty() ? QString() : formats.first();
}

QStringList AdDateRangePicker::effectiveParseFormats() const {
  return effectiveFormatsForPicker(pickerMode_, displayFormat_, displayFormats_,
                                   effectiveTextIncludesTime(), effectiveTimeFormat(),
                                   effectiveUse12Hours());
}

bool AdDateRangePicker::effectiveTextIncludesTime() const {
  if (pickerMode_ == PickerMode::Time) {
    return true;
  }
  if (showTime_) {
    return true;
  }
  if (!maskFormat_ || pickerMode_ != PickerMode::Date) {
    return false;
  }
  const QStringList formats = effectiveFormatsForPicker(
      pickerMode_, displayFormat_, displayFormats_, false, timeFormat_, use12Hours_);
  return !formats.isEmpty() && formatHasTimeToken(formats.first());
}

QString AdDateRangePicker::effectiveTimeFormat() const {
  const QString source = !timeFormat_.trimmed().isEmpty()
                             ? normalizeDateFormatSyntax(timeFormat_.trimmed())
                             : inferredTimeFormatFromDisplayFormat(displayFormat());
  return normalizedTimeFormat(source, use12Hours_ || formatUses12HourClock(source));
}

bool AdDateRangePicker::effectiveUse12Hours() const {
  if (use12Hours_) {
    return true;
  }
  const QString source = !timeFormat_.trimmed().isEmpty()
                             ? normalizeDateFormatSyntax(timeFormat_.trimmed())
                             : inferredTimeFormatFromDisplayFormat(displayFormat());
  return formatUses12HourClock(source);
}

bool AdDateRangePicker::effectiveShowSecondColumn() const {
  return showSecondExplicit_ ? showSecond_ : formatHasSecondToken(effectiveTimeFormat());
}

Qt::DayOfWeek AdDateRangePicker::effectiveFirstDayOfWeek() const {
  return firstDayOfWeekForLocale(locale_);
}

AdDateRangePicker::RangePart AdDateRangePicker::activeRangePart() const {
  if (startDisabled_ && !endDisabled_) {
    return RangePart::End;
  }
  if (endDisabled_ && !startDisabled_) {
    return RangePart::Start;
  }
  if (!lineEdit_) {
    return lastFocusedRangePart_;
  }

  const QString text = lineEdit_->text();
  const QString separatorText = effectiveSeparator();
  const qsizetype separatorIndex = text.indexOf(separatorText);
  if (separatorIndex < 0) {
    return lastFocusedRangePart_;
  }

  const qsizetype cursor = lineEdit_->cursorPosition();
  return cursor > separatorIndex + separatorText.size() / 2 ? RangePart::End : RangePart::Start;
}

void AdDateRangePicker::moveCursorToRangePart(RangePart range) {
  if (!lineEdit_) {
    return;
  }

  const QString text = lineEdit_->text();
  const QString separatorText = effectiveSeparator();
  const qsizetype separatorIndex = text.indexOf(separatorText);
  if (range == RangePart::Start) {
    lineEdit_->setCursorPosition(0);
  } else if (separatorIndex >= 0) {
    lineEdit_->setCursorPosition(static_cast<int>(separatorIndex + separatorText.size()));
  } else {
    lineEdit_->setCursorPosition(static_cast<int>(text.size()));
  }
}

AdDateRangePicker::PickerMode AdDateRangePicker::normalizedPanelMode(PickerMode value) const {
  return normalizedPanelPickerMode(value);
}

AdDateRangePicker::PickerMode AdDateRangePicker::effectivePanelMode() const {
  return panelModeExplicit_ ? panelMode_ : normalizedPanelMode(pickerMode_);
}

QDateTime AdDateRangePicker::parseMaskedText(const QString& text, const QTime& fallbackTime,
                                             bool* ok) const {
  if (ok) {
    *ok = false;
  }
  if (!maskFormat_ || pickerMode_ != PickerMode::Date || !textContainsDigit(text)) {
    return {};
  }
  bool parsedOk = false;
  QDateTime parsed = parsePickerDateTimeText(pickerMode_, text, effectiveParseFormats(), locale_,
                                             effectiveFirstDayOfWeek(), fallbackTime,
                                             effectiveTextIncludesTime(), &parsedOk);
  if (!parsedOk || !parsed.isValid()) {
    parsed = alignMaskedDateTimeText(pickerMode_, text, defaultDisplayFormat(),
                                     effectiveFirstDayOfWeek(), fallbackTime, &parsedOk);
  }
  if (parsedOk && parsed.isValid() && ok) {
    *ok = true;
  }
  return parsed;
}

bool AdDateRangePicker::effectiveInputDisabled() const {
  return !isEnabled() || (startDisabled_ && endDisabled_);
}

bool AdDateRangePicker::canAcceptRangeForInteraction(const QDate& start, const QDate& end) const {
  if (!rangeEndpointsAcceptable(start, end, allowEmptyStart_, allowEmptyEnd_)) {
    return false;
  }
  if (!respectsEndpointDisabledOrder(start, end)) {
    return false;
  }
  if (!startDisabled_ && start.isValid() && !isDateSelectable(start, end)) {
    return false;
  }
  if (!endDisabled_ && end.isValid() && !isDateSelectable(end, start)) {
    return false;
  }
  return true;
}

bool AdDateRangePicker::respectsEndpointDisabledOrder(const QDate& start, const QDate& end) const {
  const QDate normalizedStart = normalizeForPicker(pickerMode_, start, effectiveFirstDayOfWeek());
  const QDate normalizedEnd = normalizeForPicker(pickerMode_, end, effectiveFirstDayOfWeek());
  if (startDisabled_ && !endDisabled_ && normalizedStart.isValid() && normalizedEnd.isValid() &&
      !samePickerValue(pickerMode_, normalizedStart, normalizedEnd, effectiveFirstDayOfWeek()) &&
      normalizedStart > normalizedEnd) {
    return false;
  }
  if (endDisabled_ && !startDisabled_ && normalizedStart.isValid() && normalizedEnd.isValid() &&
      !samePickerValue(pickerMode_, normalizedStart, normalizedEnd, effectiveFirstDayOfWeek()) &&
      normalizedStart > normalizedEnd) {
    return false;
  }
  return true;
}

bool AdDateRangePicker::isDisabledEndpointCrossingCandidate(const QDate& value) const {
  const QDate normalized = normalizeForPicker(pickerMode_, value, effectiveFirstDayOfWeek());
  if (!normalized.isValid()) {
    return false;
  }
  if (startDisabled_ && !endDisabled_) {
    const QDate fixedStart = normalizeForPicker(pickerMode_, startDate_, effectiveFirstDayOfWeek());
    return fixedStart.isValid() &&
           !samePickerValue(pickerMode_, fixedStart, normalized, effectiveFirstDayOfWeek()) &&
           fixedStart > normalized;
  }
  if (endDisabled_ && !startDisabled_) {
    const QDate fixedEnd = normalizeForPicker(pickerMode_, endDate_, effectiveFirstDayOfWeek());
    return fixedEnd.isValid() &&
           !samePickerValue(pickerMode_, fixedEnd, normalized, effectiveFirstDayOfWeek()) &&
           normalized > fixedEnd;
  }
  return false;
}

void AdDateRangePicker::applyEndpointDisabledToRange(QDate* start, QDate* end) const {
  if (startDisabled_ && start) {
    *start = startDate_;
  }
  if (endDisabled_ && end) {
    *end = endDate_;
  }
}

void AdDateRangePicker::mergeEndpointDisabledPopupRange(QDate* start, QDate* end) const {
  if (!start || !end) {
    return;
  }
  if (startDisabled_ && !endDisabled_) {
    const bool preferEnd =
        end->isValid() && (*end != endDate_ || !start->isValid() || *start == startDate_);
    const QDate candidate = preferEnd ? *end : *start;
    *start = startDate_;
    *end = candidate.isValid() ? candidate : endDate_;
    return;
  }
  if (endDisabled_ && !startDisabled_) {
    const bool preferStart =
        start->isValid() && (*start != startDate_ || !end->isValid() || *end == endDate_);
    const QDate candidate = preferStart ? *start : *end;
    *start = candidate.isValid() ? candidate : startDate_;
    *end = endDate_;
    return;
  }
  applyEndpointDisabledToRange(start, end);
}

bool AdDateRangePicker::isDateSelectable(const QDate& value, const QDate& from) const {
  const QDate normalized = normalizeForPicker(pickerMode_, value, effectiveFirstDayOfWeek());
  if (!normalized.isValid()) {
    return false;
  }
  if (!pickerValueWithinBounds(pickerMode_, normalized, effectiveFirstDayOfWeek(), minDate_,
                               maxDate_)) {
    return false;
  }
  if (disabledDatePredicate_ && disabledDatePredicate_(normalized)) {
    return false;
  }
  if (disabledDateContextPredicate_) {
    DisabledDateContext context;
    context.from = normalizeForPicker(pickerMode_, from, effectiveFirstDayOfWeek());
    context.type = pickerMode_;
    if (disabledDateContextPredicate_(normalized, context)) {
      return false;
    }
  }
  return true;
}

bool AdDateRangePicker::isDateTimeSelectable(const QDateTime& value, TimeSelectionPart part,
                                             const QDate& from) const {
  if (!value.isValid() || !isDateSelectable(value.date(), from)) {
    return false;
  }
  if ((!showTime_ && pickerMode_ != PickerMode::Time) || !disabledTimePredicate_) {
    return true;
  }
  DisabledTimeContext context;
  context.from = normalizeForPicker(pickerMode_, from, effectiveFirstDayOfWeek());
  context.part = part;
  return !disabledTimePredicate_(
      normalizeForPicker(pickerMode_, value.date(), effectiveFirstDayOfWeek()),
      normalizedTimeValue(value.time()), context);
}

void AdDateRangePicker::clearRangeInternal(bool emitSignals, bool respectEndpointDisabled) {
  popupCalendarActive_ = false;
  activeRangeHistory_.clear();
  const QDate nextStartDate = respectEndpointDisabled && startDisabled_ ? startDate_ : QDate();
  const QDate nextEndDate = respectEndpointDisabled && endDisabled_ ? endDate_ : QDate();
  const QTime nextStartTime =
      respectEndpointDisabled && startDisabled_ ? startTime_ : defaultTimeValue();
  const QTime nextEndTime = respectEndpointDisabled && endDisabled_ ? endTime_ : defaultTimeValue();

  const bool datesChanged = startDate_ != nextStartDate || endDate_ != nextEndDate;
  const bool timesChanged = startTime_ != nextStartTime || endTime_ != nextEndTime;
  if (!datesChanged && !timesChanged) {
    syncLineEdit();
    return;
  }

  startDate_ = nextStartDate;
  endDate_ = nextEndDate;
  startTime_ = nextStartTime;
  endTime_ = nextEndTime;
  syncLineEdit();
  syncPanelState();
  if (emitSignals) {
    if (datesChanged) {
      emit rangeChanged(startDate_, endDate_);
    }
    if (timesChanged) {
      emit timeRangeChanged(startTime_, endTime_);
    }
    emit dateTimeRangeChanged(startDateTime(), endDateTime());
    emit cleared();
  }
}

void AdDateRangePicker::handleControllerPopupVisibleChanged(bool value) {
  if (popupVisible_ == value) {
    return;
  }

  popupVisible_ = value;
  syncLineEditStyle();
  if (!popupVisible_) {
    if (suppressPopupCloseSubmit_ || popupNeedsExplicitSubmit() ||
        !commitPopupCalendarRange(false, false)) {
      popupCalendarActive_ = false;
      activeRangeHistory_.clear();
      syncLineEdit();
    }
    clearPreviewText();
  } else {
    if (popupPanel_) {
      popupPanel_->setFocus(Qt::PopupFocusReason);
    }
  }
  emit popupVisibleChanged(popupVisible_);
}

QObject* AdDateRangePicker::popupOwnerObject() const {
  return const_cast<AdDateRangePicker*>(this);
}

QWidget* AdDateRangePicker::popupAnchorWidget() const {
  return const_cast<AdDateRangePicker*>(this);
}

QWidget* AdDateRangePicker::popupScopeWindow() const {
  return detail::resolvePopupScopeWindow(this);
}

QWidget* AdDateRangePicker::popupSurfaceWidget() const { return popup_; }

QWidget* AdDateRangePicker::popupEnsureSurface() {
  ensurePopup();
  return popup_;
}

bool AdDateRangePicker::popupHasContent() const { return true; }

detail::OverlayPopupPlacement AdDateRangePicker::popupPlacement() const {
  return toOverlayPopupPlacement(placement_);
}

bool AdDateRangePicker::popupAutoAdjustOverflow() const { return true; }

bool AdDateRangePicker::popupArrowVisible() const { return true; }

bool AdDateRangePicker::popupArrowPointAtCenter() const { return false; }

int AdDateRangePicker::popupOffset() const {
  detail::DatePickerStyleInput input;
  input.size = size_;
  input.variant = variant_;
  input.status = status_;
  input.disabled = effectiveInputDisabled();
  input.baseFont = font();
  input.componentTokens = componentTokens_;
  input.semanticStyles = effectiveSemanticStyles().popup;
  const detail::DatePickerVisualStyle style = detail::resolveDatePickerVisualStyle(
      input, adqt::theme::ThemeManager::instance().resolve(this));
  return std::max(0, style.metrics.popupOffset);
}

int AdDateRangePicker::popupArrowOffsetHorizontal() const {
  if (!lineEdit_) {
    return 0;
  }

  const int partCenter = lineEdit_->x() + lineEdit_->rangeInputPartCenterX(lastFocusedRangePart_);
  switch (placement_) {
    case Placement::BottomRight:
    case Placement::TopRight:
      return std::max(0, width() - partCenter);
    case Placement::BottomLeft:
    case Placement::TopLeft:
    default:
      return std::max(0, partCenter);
  }
}

int AdDateRangePicker::popupArrowOffsetVertical() const { return 0; }

void AdDateRangePicker::popupApplyResolvedPlacement(detail::OverlayPopupPlacement placement,
                                                    qreal arrowCenterCoord) {
  if (auto* surface = dynamic_cast<detail::OverlayPopupSurface*>(popup_)) {
    surface->setPlacement(placement);
    surface->setArrowCenter(arrowCenterCoord);
    const int alignmentOffset = popupPanelAlignmentOffset(placement);
    if (alignmentOffset != 0) {
      popup_->move(popup_->pos() + QPoint(alignmentOffset, 0));
    }
    syncPopupArrowPosition();
  }
}

}  // namespace adqt::widgets
