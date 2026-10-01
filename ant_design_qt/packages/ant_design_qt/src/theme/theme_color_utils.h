#pragma once

#include <QColor>
#include <QVector>

namespace adqt::theme {

QColor applyBackgroundOpacity(QColor color, qreal opacity);

qreal colorContrastRatio(const QColor& first, const QColor& second);
QColor ensureContrastWithText(const QColor& background, const QColor& text,
                              qreal minimumContrast = 4.5);
QColor deriveSolidHoverColor(const QColor& background, const QColor& text,
                             qreal minimumContrast = 4.5);
QColor deriveSolidActiveColor(const QColor& background, const QColor& text,
                              qreal minimumContrast = 4.5);
QVector<QColor> generateMappedPalette(const QColor& base, bool darkTheme,
                                      const QColor& background = QColor());

}  // namespace adqt::theme
