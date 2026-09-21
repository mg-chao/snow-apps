#pragma once

#include <QPoint>
#include <QPointF>

class QWidget;

// Native integration fixtures run above screenshot windows without changing the
// running application's windows. Never use these levels in product code.
void macRaiseTestWindow(QWidget* widget, int level);
void macActivateApplication();
bool macWindowReceivesPoint(QWidget* widget, const QPoint& globalPoint);
bool macWindowHasShadow(QWidget* widget);
bool macCanPostMouseEvents();
void macPostClick(const QPoint& globalPoint);
void macPostMove(const QPoint& globalPoint);

class MacCursorRestore final {
  public:
    MacCursorRestore();
    ~MacCursorRestore();
    MacCursorRestore(const MacCursorRestore&) = delete;
    MacCursorRestore& operator=(const MacCursorRestore&) = delete;

  private:
    QPointF m_position;
};

// Own the button and restore the cursor even when a native assertion throws.
class MacMouseDrag final {
  public:
    explicit MacMouseDrag(const QPoint& start);
    ~MacMouseDrag();
    MacMouseDrag(const MacMouseDrag&) = delete;
    MacMouseDrag& operator=(const MacMouseDrag&) = delete;
    void moveTo(const QPoint& point);
    void finish();

  private:
    MacCursorRestore m_restore;
    QPoint m_position;
    bool m_pressed = true;
};
