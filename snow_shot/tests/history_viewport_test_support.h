#pragma once

#include "history_sticky_selection_test_support.h"
#include "widgets/pagination.h"

#include <QFrame>
#include <QKeyEvent>
#include <QSet>

#include <algorithm>

namespace history_viewport_test {

template <typename Require>
void verifyPage(QWidget& page, const QString& prefix, const QSet<QString>& expectedIds,
                Require require) {
    const auto settle = history_sticky_test::settleLayout;
    auto* area = page.findChild<adqt::widgets::AdScrollArea*>();
    auto* selectAll = page.findChild<adqt::widgets::AdButton*>(prefix + "SelectAll");
    auto* deselect = page.findChild<adqt::widgets::AdButton*>(prefix + "DeselectAll");
    require(area && selectAll && deselect, "virtual page exposes navigation and selection");
    const auto rows = [&] {
        QList<QFrame*> result;
        for (auto* row : page.findChildren<QFrame*>()) {
            if (row->objectName() == QStringLiteral("pinnedManagementRecord") ||
                row->objectName().startsWith(QStringLiteral("screenshotHistoryEntry-")))
                result.push_back(row);
        }
        return result;
    };
    const auto idOf = [](QWidget* row) {
        return row->property("recordId").isValid()
                   ? row->property("recordId").toString()
                   : row->objectName().mid(QStringLiteral("screenshotHistoryEntry-").size());
    };
    page.resize(980, 640);
    page.show();
    settle();
    require(!rows().isEmpty() && rows().size() <= 8,
            "a 50-entry page materializes only viewport rows and a small buffer");
    selectAll->click();
    settle();
    require(!selectAll->isEnabled(), "select page includes unmaterialized entries");
    auto* scroll = area->verticalScrollBar();
    const int maximum = scroll->maximum();
    require(maximum > 5000, "spacers reserve the entire 50-entry page");
    QSet<QString> visited;
    for (int position = 0;; position = std::min(maximum, position + 250)) {
        scroll->setValue(position);
        settle();
        require(rows().size() <= 8 && scroll->maximum() == maximum,
                "scrolling recycles rows without changing the scroll range");
        for (auto* row : rows()) {
            visited.insert(idOf(row));
            require(row->findChild<adqt::widgets::AdCheckbox*>()->isChecked(),
                    "newly materialized rows restore page-wide selection");
        }
        if (position == maximum)
            break;
    }
    require(visited == expectedIds, "every page entry is reachable without rendering other pages");
    page.resize(500, 640);
    settle();
    require(rows().size() <= 8 && scroll->maximum() > maximum,
            "narrow cards update virtual geometry without expanding the widget population");
    page.resize(980, 640);
    deselect->click();
    scroll->setValue(0);
    settle();

    // Tab ordering must follow record order, including across recycled rows.
    auto ordered = rows();
    std::sort(ordered.begin(), ordered.end(),
              [](auto* left, auto* right) { return left->y() < right->y(); });
    ordered.front()->findChild<adqt::widgets::AdCheckbox*>()->setFocus();
    visited.clear();
    for (int step = 0; step < expectedIds.size() * 12 && visited != expectedIds; ++step) {
        auto* focus = QApplication::focusWidget();
        require(focus != nullptr, "virtual rows preserve a keyboard focus target");
        for (auto* row : rows()) {
            if (row == focus || row->isAncestorOf(focus))
                visited.insert(idOf(row));
        }
        QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
        QApplication::sendEvent(focus, &tab);
        settle();
        require(rows().size() <= 8, "keyboard traversal keeps a bounded number of rows");
    }
    require(visited == expectedIds, "Tab reaches every record across virtual row boundaries");
    visited.clear();
    for (int step = 0; step < expectedIds.size() * 12 && visited != expectedIds; ++step) {
        auto* focus = QApplication::focusWidget();
        require(focus != nullptr, "reverse navigation preserves keyboard focus");
        for (auto* row : rows()) {
            if (row == focus || row->isAncestorOf(focus))
                visited.insert(idOf(row));
        }
        QKeyEvent tab(QEvent::KeyPress, Qt::Key_Backtab, Qt::ShiftModifier);
        QApplication::sendEvent(focus, &tab);
        settle();
    }
    require(visited == expectedIds, "Shift+Tab reaches every virtual record in reverse");
}

} // namespace history_viewport_test
