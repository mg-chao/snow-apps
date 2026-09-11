#pragma once

#include <QObject>
#include <QString>

#include <limits>

class QWidget;

namespace adqt::widgets {

class AdInputTextPolicy : public QObject {
  Q_OBJECT

 public:
  explicit AdInputTextPolicy(QObject* parent = nullptr) : QObject(parent) {}
  ~AdInputTextPolicy() override = default;

  virtual int characterCount(const QString& text) const {
    return static_cast<int>(qMin(text.size(), qsizetype(std::numeric_limits<int>::max())));
  }

  virtual QString normalizeText(const QString& text, int maximumCharacterCount) const {
    Q_UNUSED(maximumCharacterCount)
    return text;
  }

  virtual QString formatCountLabel(const QString& text, int currentCount,
                                   int maximumCharacterCount) const {
    Q_UNUSED(text)
    if (maximumCharacterCount > 0) {
      return QStringLiteral("%1 / %2").arg(currentCount).arg(maximumCharacterCount);
    }
    return QString::number(currentCount);
  }
};

class AdOtpCodeFormatter : public QObject {
  Q_OBJECT

 public:
  explicit AdOtpCodeFormatter(QObject* parent = nullptr) : QObject(parent) {}
  ~AdOtpCodeFormatter() override = default;

  virtual QString formatCode(const QString& value) const { return value; }
};

class AdOtpSeparatorFactory : public QObject {
  Q_OBJECT

 public:
  explicit AdOtpSeparatorFactory(QObject* parent = nullptr) : QObject(parent) {}
  ~AdOtpSeparatorFactory() override = default;

  virtual QWidget* createSeparator(int index, QWidget* parent) const {
    Q_UNUSED(index)
    Q_UNUSED(parent)
    return nullptr;
  }
};

}  // namespace adqt::widgets
