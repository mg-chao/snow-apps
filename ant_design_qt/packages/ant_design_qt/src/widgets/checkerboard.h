#pragma once

#include <QBrush>
#include <QColor>
#include <QImage>

class QWidget;

namespace adqt::widgets {

// The second color is composited over the first, so translucent fill tokens
// still produce an opaque transparency backdrop.
QImage checkerboardTile(int cellSize, const QColor& base, const QColor& alternate);
QBrush checkerboardBrush(int cellSize, const QColor& base, const QColor& alternate);
QImage themedCheckerboardTile(const QWidget* widget = nullptr, int cellSize = 6);
QBrush themedCheckerboardBrush(const QWidget* widget = nullptr, int cellSize = 6);

}  // namespace adqt::widgets
