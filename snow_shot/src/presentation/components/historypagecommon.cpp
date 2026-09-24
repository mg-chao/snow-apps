#include "snow_shot/presentation/components/historypagecommon.h"

#include "widgets/button.h"
#include "widgets/date_picker.h"
#include "widgets/pagination.h"
#include "widgets/popconfirm.h"
#include "widgets/select.h"

#include <QBoxLayout>
#include <QFont>
#include <QLabel>
#include <QPalette>
#include <QWidget>

#include <algorithm>

namespace snow_shot::presentation::components::history_page {

void configureSourceFilter(adqt::widgets::AdSelect* filter) {
    filter->setMode(adqt::widgets::AdSelect::Mode::Multiple);
    filter->setAllowClear(true);
    filter->setMaxTagCount(1);
    filter->setMinimumWidth(140);
    filter->setMaximumWidth(170);
}

void configureDateFilter(adqt::widgets::AdDateRangePicker* filter) {
    filter->setAllowClear(true);
    filter->setMinimumWidth(220);
    filter->setMaximumWidth(260);
}

void configurePagination(adqt::widgets::AdPagination* pagination) {
    pagination->setPageSize(10);
    pagination->setPageSizeOptions({10, 20, 50});
    pagination->setSizeChangerMode(adqt::widgets::AdPagination::SizeChangerMode::Always);
    pagination->setAlignment(adqt::widgets::AdPagination::Alignment::End);
    pagination->setResponsive(true);
}

void configureSelectionAction(adqt::widgets::AdButton* button, bool destructive) {
    button->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Link);
    button->setAccentRole(destructive ? adqt::widgets::AdButton::AccentRole::Danger
                                      : adqt::widgets::AdButton::AccentRole::Primary);
}

void configureDeleteConfirmation(adqt::widgets::AdPopconfirm* confirmation) {
    confirmation->setButtonAccentRole(adqt::widgets::AdPopconfirm::StandardButton::Ok,
                                      adqt::widgets::AdButton::AccentRole::Danger);
}

void applyTextTheme(const TextControls& controls, const styles::ThemeColorScheme& scheme) {
    auto setColor = [](QLabel* label, const QColor& color) {
        QPalette palette = label->palette();
        palette.setColor(QPalette::WindowText, color);
        label->setPalette(palette);
    };
    auto setFont = [](QLabel* label, int pixelSize, QFont::Weight weight) {
        QFont font = label->font();
        font.setPixelSize(pixelSize);
        font.setWeight(weight);
        label->setFont(font);
    };

    setColor(controls.title, scheme.map.colorText);
    setFont(controls.title, scheme.metricAlias.fontSizeHeading4, QFont::DemiBold);
    for (QLabel* label : {controls.count, controls.selectionSummary}) {
        setColor(label, scheme.map.colorTextSecondary);
        setFont(label, scheme.metricAlias.fontSize, QFont::Normal);
    }
    setColor(controls.emptyTitle, scheme.map.colorText);
    setFont(controls.emptyTitle, scheme.metricAlias.fontSizeLG, QFont::DemiBold);
    setColor(controls.emptyDescription, scheme.map.colorTextSecondary);
}

static bool matchesDateRange(const QDate& date, const adqt::widgets::AdDateRangePicker* filter) {
    const QDate start = filter->startDate();
    const QDate end = filter->endDate();
    return (!start.isValid() || date >= start) && (!end.isValid() || date <= end);
}

bool matchesFilters(const QVariant& source, const QDate& date,
                    const adqt::widgets::AdSelect* sourceFilter,
                    const adqt::widgets::AdDateRangePicker* dateFilter) {
    const QVariantList sources = sourceFilter->currentValues();
    return (sources.isEmpty() || sources.contains(source)) && matchesDateRange(date, dateFilter);
}

void updatePagination(adqt::widgets::AdPagination* pagination, int total, bool resetPage,
                      bool& updating) {
    updating = true;
    if (resetPage) {
        pagination->setCurrentPage(1);
    }
    pagination->setTotal(total);
    updating = false;
}

PageRange pageRange(const adqt::widgets::AdPagination* pagination, int total) {
    const int first = std::max(0, (pagination->currentPage() - 1) * pagination->pageSize());
    return {first, std::min(first + pagination->pageSize(), total)};
}

bool isCurrentPageSelected(const QSet<QString>& selected, const QVector<QString>& pageIds) {
    if (pageIds.isEmpty()) {
        return false;
    }
    for (const QString& id : pageIds) {
        if (!selected.contains(id)) {
            return false;
        }
    }
    return true;
}

void updateSelectionBar(const SelectionControls& controls, const QSet<QString>& selected,
                        const QVector<QString>& pageIds, bool canDelete, int availableWidth,
                        const QString& summary, const QString& deletePrompt) {
    const int count = static_cast<int>(selected.size());
    controls.summary->setText(summary);
    controls.bar->setAccessibleName(summary);
    controls.deleteConfirmation->setText(deletePrompt);
    controls.selectPage->setEnabled(!pageIds.isEmpty() &&
                                    !isCurrentPageSelected(selected, pageIds));
    controls.deleteSelected->setEnabled(canDelete && count > 0);
    controls.deleteConfirmation->setEnabled(canDelete && count > 0);
    controls.deselect->setEnabled(count > 0);
    const bool wide = availableWidth >= kWideEntryBreakpoint;
    controls.layout->setDirection(wide ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
    controls.layout->setAlignment(controls.summary, wide ? Qt::AlignVCenter : Qt::AlignLeft);
    controls.layout->setAlignment(controls.actions, wide ? Qt::AlignRight | Qt::AlignVCenter
                                                         : Qt::AlignRight | Qt::AlignTop);
    controls.bar->setVisible(count > 0);
}

} // namespace snow_shot::presentation::components::history_page
