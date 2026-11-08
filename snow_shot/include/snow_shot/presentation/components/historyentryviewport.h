#pragma once

#include <QObject>
#include <QMap>
#include <QPointer>
#include <QVector>
#include <QWidget>

#include <functional>

class QScrollArea;
class QVBoxLayout;

namespace snow_shot::presentation::components {

// Keeps the full page's scroll geometry with lightweight spacers. Only the viewport,
// one adjacent row on either side, and the keyboard-focused row need real widgets.
class HistoryEntryViewport final : public QObject {
  public:
    HistoryEntryViewport(QScrollArea* area, QWidget* host, QVBoxLayout* layout,
                         std::function<void()> rebuild, QObject* parent);
    [[nodiscard]] QVector<int> wantedRows(int count, int focusedRow = -1) const;
    void setRows(int count, const QMap<int, QWidget*>& rows);
    void clear();
    void resetScroll();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    [[nodiscard]] int rowHeight() const;
    void queueUpdate();
    bool moveFocus(QWidget* current, bool forward);

    QScrollArea* m_area;
    QWidget* m_host;
    QVBoxLayout* m_layout;
    std::function<void()> m_rebuild;
    QMap<int, QPointer<QWidget>> m_rows;
    int m_count = 0;
    int m_spacing = 0;
    int m_rowHeight = 0;
    int m_forcedRow = -1;
    bool m_queued = false;
    bool m_updating = false;
};

} // namespace snow_shot::presentation::components
