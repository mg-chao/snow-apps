#include "theme/theme_color_utils.h"
#include "theme/theme_manager.h"
#include "widgets/date_picker.h"
#include "widgets/input_line_edit.h"
#include "widgets/input_style.h"

#include <QApplication>
#include <QImage>
#include <QPainter>

#include <cmath>
#include <stdexcept>

void datePickerBackgroundOpacityTests();

namespace {

class DatePickerBackdrop final : public QWidget {
 public:
  QColor background = QColor(30, 90, 150);

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.fillRect(rect(), background);
  }
};

void requireBlend(const QColor& rendered, const QColor& fill, const QColor& backdrop) {
  const qreal alpha = static_cast<qreal>(fill.alphaF());
  if (std::abs(rendered.red() - qRound(fill.red() * alpha + backdrop.red() * (1 - alpha))) > 1 ||
      std::abs(rendered.green() - qRound(fill.green() * alpha + backdrop.green() * (1 - alpha))) >
          1 ||
      std::abs(rendered.blue() - qRound(fill.blue() * alpha + backdrop.blue() * (1 - alpha))) > 1) {
    throw std::runtime_error("date range selector must paint its semantic fill with one mask");
  }
}

}  // namespace

void datePickerBackgroundOpacityTests() {
  auto& manager = adqt::theme::ThemeManager::instance();
  const auto originalConfig = manager.config();
  for (const auto scheme : {adqt::theme::ThemeScheme::Light, adqt::theme::ThemeScheme::Dark}) {
    manager.setConfig(adqt::theme::defaultThemeConfig(scheme));
    DatePickerBackdrop surface;
    surface.resize(340, 100);
    adqt::widgets::AdDateRangePicker picker(&surface);
    picker.setGeometry(20, 20, 280, 36);
    picker.setRangePlaceholders(QString(), QString());
    surface.show();
    QCoreApplication::processEvents();

    for (const QColor fill : {QColor(), QColor(180, 60, 100), QColor(80, 170, 60, 100)}) {
      adqt::widgets::AdDateRangePicker::SemanticStyles semantic;
      if (fill.isValid()) semantic.input.backgroundColor = fill;
      picker.setSemanticStyles(semantic);
      for (const double opacity : {0.0, 0.4, 1.0}) {
        adqt::theme::ThemeOverride overrideValue;
        overrideValue.backgroundOpacity = opacity;
        manager.setScopeOverride(&surface, overrideValue);
        QCoreApplication::processEvents();
        QImage rendered(surface.size(), QImage::Format_ARGB32_Premultiplied);
        rendered.fill(Qt::transparent);
        surface.render(&rendered);

        const auto* lineEdit = picker.lineEdit();
        const QPoint sample = lineEdit->mapTo(&surface, QPoint(40, 5));
        QColor expectedFill;
        if (fill.isValid()) {
          expectedFill = adqt::theme::applyBackgroundOpacity(fill, opacity);
        } else {
          adqt::widgets::detail::InputStyleInput input;
          input.controlSize = lineEdit->controlSize();
          input.variant = lineEdit->variant();
          input.baseFont = lineEdit->font();
          expectedFill =
              adqt::widgets::detail::resolveInputVisualStyle(input, manager.resolve(lineEdit))
                  .selectorBg;
        }
        requireBlend(rendered.pixelColor(sample), expectedFill, surface.background);
      }
      manager.clearScopeOverride(&surface);
    }
  }
  manager.setConfig(originalConfig);
}
