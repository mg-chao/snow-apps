#include "widgets/radio.h"

#include <QApplication>
#include <QImage>
#include <QPainter>

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

QPointF colorCenter(const QImage& image, int channel) {
  double weight = 0;
  QPointF sum;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor color = image.pixelColor(x, y);
      const int value = channel == 0 ? color.red() : channel == 1 ? color.green() : color.blue();
      weight += value;
      sum += QPointF(x + 0.5, y + 0.5) * value;
    }
  }
  if (weight == 0) throw std::runtime_error("indicator color must be rendered");
  return sum / weight;
}

void selectedIndicatorIsConcentric() {
  using adqt::widgets::AdRadio;
  QWidget host;
  AdRadio radio(&host);
  radio.setChecked(true);
  // Separate color channels let us measure each painted shape, including
  // antialiased edge coverage, without depending on the platform's theme.
  AdRadio::ComponentTokens tokens;
  tokens.colors.indicatorFillColor = QColor(Qt::red);
  tokens.colors.indicatorBorderColor = QColor(Qt::blue);
  tokens.colors.indicatorDotColor = QColor(Qt::green);
  tokens.metrics.borderWidth = 1;
  for (qreal dpr : {1.0, 1.25, 1.5, 1.75, 2.0, 3.0}) {
    for (int radioSize : {16, 17}) {
      for (int dotSize : {6, 7}) {
        for (int height : {24, 25}) {
          for (int offset : {0, 1}) {
            tokens.metrics.radioSize = radioSize;
            tokens.metrics.dotSize = dotSize;
            radio.setComponentTokens(tokens);
            radio.setGeometry(offset, offset, 32, height);
            QImage image(QSize(qCeil(32 * dpr), qCeil(height * dpr)),
                         QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(dpr);
            image.fill(Qt::black);
            QPainter painter(&image);
            radio.render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
            painter.end();
            const QPointF background = colorCenter(image, 0);
            const QPointF dot = colorCenter(image, 1);
            const QPointF border = colorCenter(image, 2);
            // Rasterization can introduce tiny coverage differences; a half
            // device-pixel shift must fail at every scale.
            for (const QPointF center : {dot, border}) {
              if (std::abs(center.x() - background.x()) > 0.1 ||
                  std::abs(center.y() - background.y()) > 0.1) {
                std::cerr << "dpr=" << dpr << " radio=" << radioSize << " dot=" << dotSize
                          << " height=" << height << " offset=" << offset << " background=("
                          << background.x() << ',' << background.y() << ") center=(" << center.x()
                          << ',' << center.y() << ")\n";
                throw std::runtime_error("radio background, border and dot must share a center");
              }
            }
          }
        }
      }
    }
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  try {
    selectedIndicatorIsConcentric();
    std::cout << "Radio rendering tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
