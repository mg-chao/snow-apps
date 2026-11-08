#include "snow_shot/presentation/components/historyentryviewport.h"

#include "snow_shot/presentation/components/historyentrycard.h"

#include <QApplication>
#include <QKeyEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QScopedValueRollback>

#include <algorithm>

namespace snow_shot::presentation::components {
namespace {
bool containsFocus(QWidget* row, QWidget* focus) {
    return row && focus && (row == focus || row->isAncestorOf(focus));
}

QList<QWidget*> focusControls(QWidget* row) {
    QList<QWidget*> result;
    for (auto* child : row->findChildren<QWidget*>()) {
        if (child->isVisibleTo(row) && child->isEnabled() &&
            (child->focusPolicy() & Qt::TabFocus)) {
            result.push_back(child);
        }
    }
    return result;
}
} // namespace

HistoryEntryViewport::HistoryEntryViewport(QScrollArea* area, QWidget* host, QVBoxLayout* layout,
                                           std::function<void()> rebuild, QObject* parent)
    : QObject(parent), m_area(area), m_host(host), m_layout(layout), m_rebuild(std::move(rebuild)),
      m_spacing(std::max(0, layout->spacing())) {
    area->viewport()->installEventFilter(this);
    host->installEventFilter(this);
    connect(area->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { queueUpdate(); });
    connect(qApp, &QApplication::focusChanged, this, [this] { queueUpdate(); });
}

int HistoryEntryViewport::rowHeight() const {
    int height = HistoryEntryCard::minimumHeightForWidth(m_host->width());
    for (const auto& row : m_rows) {
        if (row && row->layout())
            height = std::max(height, row->layout()->sizeHint().height());
    }
    return height;
}

QVector<int> HistoryEntryViewport::wantedRows(int count, int focusedRow) const {
    if (count <= 0)
        return {};
    const int pitch = rowHeight() + m_spacing;
    const int top = -m_host->mapTo(m_area->viewport(), QPoint()).y();
    const int first = std::clamp(top / pitch - 1, 0, count - 1);
    const int last = std::clamp((top + m_area->viewport()->height()) / pitch + 2, first + 1, count);
    QVector<int> result;
    for (int index = first; index < last; ++index)
        result.push_back(index);
    // Do not destroy a focused control when the user scrolls it out of view.
    for (auto it = m_rows.cbegin(); it != m_rows.cend(); ++it) {
        const int index = focusedRow >= 0 ? focusedRow : it.key();
        if (index < count && containsFocus(it.value(), QApplication::focusWidget()) &&
            !result.contains(index)) {
            result.push_back(index);
        }
    }
    if (m_forcedRow >= 0 && m_forcedRow < count && !result.contains(m_forcedRow))
        result.push_back(m_forcedRow);
    std::sort(result.begin(), result.end());
    return result;
}

void HistoryEntryViewport::setRows(int count, const QMap<int, QWidget*>& rows) {
    int height = rowHeight();
    for (auto* row : rows) {
        if (row && row->layout())
            height = std::max(height, row->layout()->sizeHint().height());
    }
    bool changed = count != m_count || height != m_rowHeight || rows.size() != m_rows.size();
    for (auto it = rows.cbegin(); !changed && it != rows.cend(); ++it)
        changed = m_rows.value(it.key()) != it.value();
    if (!changed)
        return;
    const QScopedValueRollback updating(m_updating, true);
    m_count = count;
    m_rowHeight = height;
    m_layout->setEnabled(false);
    while (auto* item = m_layout->takeAt(0))
        delete item;
    m_layout->setSpacing(0);
    QMap<int, QPointer<QWidget>> nextRows;
    QWidget* previousFocus = nullptr;
    for (int index = 0; index < count; ++index) {
        if (index > 0)
            m_layout->addSpacing(m_spacing);
        if (auto* row = rows.value(index)) {
            row->setFixedHeight(height);
            m_layout->addWidget(row);
            row->show();
            nextRows.insert(index, row);
            if (m_rows.value(index) != row) {
                for (auto* child : row->findChildren<QWidget*>())
                    child->installEventFilter(this);
            }
            for (auto* control : focusControls(row)) {
                if (previousFocus)
                    QWidget::setTabOrder(previousFocus, control);
                previousFocus = control;
            }
        } else {
            m_layout->addSpacing(height);
        }
    }
    m_layout->addStretch(1);
    m_rows = std::move(nextRows);
    m_layout->setEnabled(true);
    queueUpdate();
}

void HistoryEntryViewport::clear() {
    m_rows.clear();
    m_count = 0;
    m_layout->setSpacing(m_spacing);
}

void HistoryEntryViewport::resetScroll() {
    m_area->verticalScrollBar()->setValue(0);
}

void HistoryEntryViewport::queueUpdate() {
    if (m_queued)
        return;
    m_queued = true;
    QTimer::singleShot(0, this, [this] {
        m_queued = false;
        if (m_count > 0 && !m_updating &&
            (rowHeight() != m_rowHeight || wantedRows(m_count) != m_rows.keys())) {
            m_rebuild();
        }
    });
}

bool HistoryEntryViewport::moveFocus(QWidget* current, bool forward) {
    int index = -1;
    QWidget* row = nullptr;
    for (auto it = m_rows.cbegin(); it != m_rows.cend(); ++it) {
        if (containsFocus(it.value(), current)) {
            index = it.key();
            row = it.value();
            break;
        }
    }
    if (!row)
        return false;
    const auto controls = focusControls(row);
    if (controls.isEmpty() || current != (forward ? controls.back() : controls.front()))
        return false;
    const int next = index + (forward ? 1 : -1);
    if (next < 0 || next >= m_count)
        return false;
    const QScopedValueRollback forced(m_forcedRow, next);
    m_area->verticalScrollBar()->setValue(m_host->mapTo(m_area->widget(), QPoint()).y() +
                                          next * (rowHeight() + m_spacing));
    m_rebuild();
    if (auto* target = m_rows.value(next).data()) {
        const auto nextControls = focusControls(target);
        if (!nextControls.isEmpty()) {
            (forward ? nextControls.front() : nextControls.back())
                ->setFocus(forward ? Qt::TabFocusReason : Qt::BacktabFocusReason);
        }
    }
    return true;
}

bool HistoryEntryViewport::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_host || watched == m_area->viewport()) {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Move ||
            event->type() == QEvent::LayoutRequest || event->type() == QEvent::Show)
            queueUpdate();
    } else if (event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) &&
            !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
            const bool forward =
                key->key() == Qt::Key_Tab && !(key->modifiers() & Qt::ShiftModifier);
            if (moveFocus(qobject_cast<QWidget*>(watched), forward))
                return true;
        }
    }
    return QObject::eventFilter(watched, event);
}

} // namespace snow_shot::presentation::components
