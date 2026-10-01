#include "theme_color_utils.h"

#include "palette_generate.h"

#include <algorithm>
#include <cmath>

namespace adqt::theme {

namespace {

QColor validOr(const QColor& value, const QColor& fallback) {
  return value.isValid() ? value : fallback;
}

qreal srgbChannelToLinear(int channel) {
  const qreal value = std::clamp(channel / 255.0, 0.0, 1.0);
  if (value <= 0.03928) {
    return value / 12.92;
  }
  return std::pow((value + 0.055) / 1.055, 2.4);
}

qreal relativeLuminance(const QColor& color) {
  const QColor opaque = color.toRgb();
  return 0.2126 * srgbChannelToLinear(opaque.red()) + 0.7152 * srgbChannelToLinear(opaque.green()) +
         0.0722 * srgbChannelToLinear(opaque.blue());
}

QVector<QColor> toMappedDefault(const QVector<QString>& colors) {
  QVector<QColor> mapped(11);
  if (colors.size() < 7) {
    return mapped;
  }

  mapped[1] = QColor(colors[0]);
  mapped[2] = QColor(colors[1]);
  mapped[3] = QColor(colors[2]);
  mapped[4] = QColor(colors[3]);
  mapped[5] = QColor(colors[4]);
  mapped[6] = QColor(colors[5]);
  mapped[7] = QColor(colors[6]);
  mapped[8] = QColor(colors[4]);
  mapped[9] = QColor(colors[5]);
  mapped[10] = QColor(colors[6]);
  return mapped;
}

QVector<QColor> toMappedDark(const QVector<QString>& colors) {
  QVector<QColor> mapped(11);
  if (colors.size() < 7) {
    return mapped;
  }

  mapped[1] = QColor(colors[0]);
  mapped[2] = QColor(colors[1]);
  mapped[3] = QColor(colors[2]);
  mapped[4] = QColor(colors[3]);
  mapped[5] = QColor(colors[6]);
  mapped[6] = QColor(colors[5]);
  mapped[7] = QColor(colors[4]);
  mapped[8] = QColor(colors[6]);
  mapped[9] = QColor(colors[5]);
  mapped[10] = QColor(colors[4]);
  return mapped;
}

}  // namespace

QColor applyBackgroundOpacity(QColor color, qreal opacity) {
  if (!color.isValid() || opacity == 1.0) {
    return color;
  }
  const qreal normalized = std::isfinite(opacity) ? std::clamp(opacity, qreal(0), qreal(1)) : 1.0;
  color.setAlphaF(static_cast<float>(static_cast<qreal>(color.alphaF()) * normalized));
  return color;
}

qreal colorContrastRatio(const QColor& first, const QColor& second) {
  const qreal firstLuminance = relativeLuminance(first);
  const qreal secondLuminance = relativeLuminance(second);
  const qreal lighter = std::max(firstLuminance, secondLuminance);
  const qreal darker = std::min(firstLuminance, secondLuminance);
  return (lighter + 0.05) / (darker + 0.05);
}

QColor ensureContrastWithText(const QColor& background, const QColor& text, qreal minimumContrast) {
  if (!background.isValid()) {
    return background;
  }

  QColor adjusted = background.toRgb();
  adjusted.setAlpha(255);

  QColor opaqueText = validOr(text, QColor(Qt::white)).toRgb();
  opaqueText.setAlpha(255);

  for (int i = 0; i < 12 && colorContrastRatio(adjusted, opaqueText) < minimumContrast; ++i) {
    adjusted = adjusted.darker(112);
    adjusted.setAlpha(255);
  }

  return adjusted;
}

QColor deriveSolidHoverColor(const QColor& background, const QColor& text, qreal minimumContrast) {
  const QColor hover = ensureContrastWithText(background.lighter(108), text, minimumContrast);
  return colorContrastRatio(hover, validOr(text, QColor(Qt::white))) >= minimumContrast
             ? hover
             : background;
}

QColor deriveSolidActiveColor(const QColor& background, const QColor& text, qreal minimumContrast) {
  return ensureContrastWithText(background.darker(112), text, minimumContrast);
}

QVector<QColor> generateMappedPalette(const QColor& base, bool darkTheme,
                                      const QColor& background) {
  const QColor resolvedBase = validOr(base, QColor(QStringLiteral("#1677ff")));
  const QString backgroundValue =
      background.isValid() ? background.name(QColor::HexRgb) : QString();
  const QVector<QString> raw =
      generatePalette(resolvedBase.name(QColor::HexRgb), darkTheme, backgroundValue);
  return darkTheme ? toMappedDark(raw) : toMappedDefault(raw);
}

}  // namespace adqt::theme
