#include "checkerboard.h"

#include "theme/theme_manager.h"

#include <QHash>
#include <QPainter>
#include <QPair>
#include <QPointer>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace adqt::widgets {
namespace {

struct CheckerboardKey {
  int cellSize = 0;
  QRgb base = 0;
  QRgb alternate = 0;

  bool operator==(const CheckerboardKey& other) const = default;
};

size_t qHash(const CheckerboardKey& key, size_t seed) {
  return qHashMulti(seed, key.cellSize, key.base, key.alternate);
}

QPair<QColor, QColor> colorsForValues(const theme::ThemeMapToken& values) {
  const bool dark = values.scheme == theme::ThemeScheme::Dark;
  const QColor base = values.colorBgElevated.isValid()
                          ? values.colorBgElevated
                          : (dark ? QColor(31, 31, 31) : QColor(Qt::white));
  const QColor alternate = values.colorFillSecondary.isValid()
                               ? values.colorFillSecondary
                               : (dark ? QColor(255, 255, 255, 20) : QColor(0, 0, 0, 15));
  return {base, alternate};
}

QPair<QColor, QColor> themeColors(const QWidget* widget) {
  auto& manager = theme::ThemeManager::instance();
  if (widget == nullptr) return colorsForValues(manager.globalResolvedTheme().values);
  struct CachedColors {
    QPointer<QWidget> widget;
    quint64 revision = 0;
    QPair<QColor, QColor> colors;
  };
  thread_local std::vector<CachedColors> cache;
  const quint64 revision = manager.themeRevision();
  for (const auto& entry : cache) {
    if (entry.widget == widget && entry.revision == revision) return entry.colors;
  }

  const QPair<QColor, QColor> colors = colorsForValues(manager.resolveTheme(widget));
  if (cache.size() >= 32) cache.clear();
  cache.push_back({const_cast<QWidget*>(widget), revision, colors});
  return colors;
}

}  // namespace

QImage checkerboardTile(int cellSize, const QColor& base, const QColor& alternate) {
  const int cell = std::max(2, cellSize);
  QImage tile(cell * 2, cell * 2, QImage::Format_ARGB32_Premultiplied);
  tile.fill(base);
  QPainter painter(&tile);
  painter.fillRect(QRect(0, 0, cell, cell), alternate);
  painter.fillRect(QRect(cell, cell, cell, cell), alternate);
  return tile;
}

QBrush checkerboardBrush(int cellSize, const QColor& base, const QColor& alternate) {
  thread_local QHash<CheckerboardKey, QBrush> cache;
  const CheckerboardKey key{std::max(2, cellSize), base.rgba(), alternate.rgba()};
  if (const auto found = cache.constFind(key); found != cache.constEnd()) return found.value();
  if (cache.size() >= 32) cache.clear();
  const QBrush brush(checkerboardTile(key.cellSize, base, alternate));
  cache.insert(key, brush);
  return brush;
}

QImage themedCheckerboardTile(const QWidget* widget, int cellSize) {
  const auto [base, alternate] = themeColors(widget);
  return checkerboardTile(cellSize, base, alternate);
}

QBrush themedCheckerboardBrush(const QWidget* widget, int cellSize) {
  const auto [base, alternate] = themeColors(widget);
  return checkerboardBrush(cellSize, base, alternate);
}

}  // namespace adqt::widgets
