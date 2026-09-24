#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_HISTORYPAGECOMMON_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_HISTORYPAGECOMMON_H

#include "snow_shot/presentation/styles/themecolorscheme.h"

#include <QDate>
#include <QSet>
#include <QString>
#include <QVariant>
#include <QVector>

class QBoxLayout;
class QLabel;
class QWidget;

namespace adqt::widgets {
class AdButton;
class AdDateRangePicker;
class AdPagination;
class AdPopconfirm;
class AdSelect;
} // namespace adqt::widgets

namespace snow_shot::presentation::components::history_page {

inline constexpr int kWideEntryBreakpoint = 560;

struct SelectionControls {
    QWidget* bar = nullptr;
    QBoxLayout* layout = nullptr;
    QLabel* summary = nullptr;
    QWidget* actions = nullptr;
    adqt::widgets::AdButton* selectPage = nullptr;
    adqt::widgets::AdButton* deleteSelected = nullptr;
    adqt::widgets::AdButton* deselect = nullptr;
    adqt::widgets::AdPopconfirm* deleteConfirmation = nullptr;
};

struct PageRange {
    int first = 0;
    int last = 0;
};

struct TextControls {
    QLabel* title = nullptr;
    QLabel* count = nullptr;
    QLabel* selectionSummary = nullptr;
    QLabel* emptyTitle = nullptr;
    QLabel* emptyDescription = nullptr;
};

void configureSourceFilter(adqt::widgets::AdSelect* filter);
void configureDateFilter(adqt::widgets::AdDateRangePicker* filter);
void configurePagination(adqt::widgets::AdPagination* pagination);
void configureSelectionAction(adqt::widgets::AdButton* button, bool destructive);
void configureDeleteConfirmation(adqt::widgets::AdPopconfirm* confirmation);
void applyTextTheme(const TextControls& controls, const styles::ThemeColorScheme& scheme);

[[nodiscard]] bool matchesFilters(const QVariant& source, const QDate& date,
                                  const adqt::widgets::AdSelect* sourceFilter,
                                  const adqt::widgets::AdDateRangePicker* dateFilter);
void updatePagination(adqt::widgets::AdPagination* pagination, int total, bool resetPage,
                      bool& updating);
[[nodiscard]] PageRange pageRange(const adqt::widgets::AdPagination* pagination, int total);
[[nodiscard]] bool isCurrentPageSelected(const QSet<QString>& selected,
                                         const QVector<QString>& pageIds);
void updateSelectionBar(const SelectionControls& controls, const QSet<QString>& selected,
                        const QVector<QString>& pageIds, bool canDelete, int availableWidth,
                        const QString& summary, const QString& deletePrompt);

} // namespace snow_shot::presentation::components::history_page

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_HISTORYPAGECOMMON_H
