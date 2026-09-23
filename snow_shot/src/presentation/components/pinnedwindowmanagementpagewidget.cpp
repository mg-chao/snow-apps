#include "snow_shot/presentation/components/pinnedwindowmanagementpagewidget.h"

#include "snow_shot/presentation/components/historyselectionbar.h"
#include "snow_shot/presentation/components/pagecontainerwidget.h"
#include "snow_shot/presentation/components/themedheadericonbutton.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"

#include "antd_icons.h"
#include "icon_renderer.h"
#include "icons/widget_icons.h"
#include "widgets/button.h"
#include "widgets/checkbox.h"
#include "widgets/date_picker.h"
#include "widgets/pagination.h"
#include "widgets/popconfirm.h"
#include "widgets/select.h"

#include <QBoxLayout>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QThreadPool>
#include <QVBoxLayout>

#include <algorithm>

using namespace snow_shot;

namespace {
namespace outlined_icons = adqt::icons::antd::outlined;
namespace styles = snow_shot::presentation::styles;
namespace storage = snow_shot::storage;

constexpr int kPreviewWidth = 260;
constexpr int kPreviewHeight = 156;
constexpr int kWideEntryBreakpoint = 560;

class ApplicationPinnedDataSource final : public PinnedWindowManagementDataSource {
  public:
    explicit ApplicationPinnedDataSource(QObject* parent)
        : PinnedWindowManagementDataSource(parent) {
        m_pool.setMaxThreadCount(2);
        connect(&storage::ApplicationStorage::instance(),
                &storage::ApplicationStorage::pinnedWindowsChanged, this,
                &PinnedWindowManagementDataSource::changed);
    }

    ~ApplicationPinnedDataSource() override {
        m_pool.clear();
        m_pool.waitForDone();
    }

    QVector<storage::PinnedWindowSummary> records() const override {
        return storage::ApplicationStorage::instance().pinnedWindows().summaries();
    }

    QVector<storage::PinnedWindowGroup> groups() const override {
        return storage::ApplicationStorage::instance().pinnedWindows().groups();
    }

    void cancelPreviews() override {
        m_pool.clear();
    }

    void requestPreview(const QString& id, quint64 generation) override {
        auto* repository = &storage::ApplicationStorage::instance().pinnedWindows();
        m_pool.start([this, repository, id, generation]() {
            const auto record = repository->loadRecord(id);
            QImage image;
            if (record) {
                image = record->image;
                if (record->sourceKind == storage::PinnedWindowSourceKind::ClipboardText) {
                    ScreenshotClipboardOriginalContent content;
                    content.html = record->originalHtml;
                    content.text = record->originalText;
                    const auto rendered = ScreenshotClipboardContentReader::renderOriginalText(
                        content, record->firstCreationTextDpi);
                    if (rendered) {
                        image = rendered->image;
                    }
                }
                if (!image.isNull()) {
                    image = image.scaled(640, 320, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                }
            }
            QMetaObject::invokeMethod(
                this, [this, id, generation, image]() { emit previewReady(id, generation, image); },
                Qt::QueuedConnection);
        });
    }

    void showRecord(const QString& id) override {
        storage::ApplicationStorage::instance().requestPinnedWindowShow(id);
    }

    void removeRecords(const QVector<QString>& ids) override {
        storage::ApplicationStorage::instance().requestPinnedWindowDelete(ids);
    }

  private:
    QThreadPool m_pool;
};

class PreviewLabel final : public QLabel {
  public:
    using QLabel::QLabel;

    void setImage(QImage image) {
        m_image = std::move(image);
        updateImage();
    }

  protected:
    void resizeEvent(QResizeEvent* event) override {
        QLabel::resizeEvent(event);
        updateImage();
    }

  private:
    void updateImage() {
        if (!m_image.isNull()) {
            setPixmap(QPixmap::fromImage(
                m_image.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        }
    }

    QImage m_image;
};

} // namespace

PinnedWindowManagementPageWidget::PinnedWindowManagementPageWidget(QWidget* parent)
    : PinnedWindowManagementPageWidget(nullptr, parent) {}

PinnedWindowManagementPageWidget::PinnedWindowManagementPageWidget(
    PinnedWindowManagementDataSource* source, QWidget* parent)
    : QWidget(parent), m_source(source != nullptr ? source : new ApplicationPinnedDataSource(this)),
      m_scheme(styles::ThemeManager::instance().themeColorScheme()) {
    setObjectName(QStringLiteral("pinnedWindowManagementPage"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    const auto metric = m_scheme.metricAlias;
    auto* container = new PageContainerWidget(metric, this);
    container->setObjectName(QStringLiteral("pinnedManagementPageContainer"));
    root->addWidget(container, 1);
    auto* content = container->contentWidget();
    auto* layout = container->contentLayout();
    layout->setSpacing(0);

    auto* titleGroup = new QWidget(content);
    auto* titleLayout = new QVBoxLayout(titleGroup);
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(metric.marginXXS);
    m_title = new QLabel(titleGroup);
    m_title->setObjectName(QStringLiteral("pinnedManagementTitle"));
    m_count = new QLabel(titleGroup);
    m_count->setObjectName(QStringLiteral("pinnedManagementCountLabel"));
    titleLayout->addWidget(m_title);
    titleLayout->addWidget(m_count);

    auto* headerActions = new QWidget(content);
    auto* headerActionsLayout = new QHBoxLayout(headerActions);
    headerActionsLayout->setContentsMargins(0, 0, 0, 0);
    headerActionsLayout->setSpacing(metric.marginXS);
    m_deleteAll = new ThemedHeaderIconButton(metric, outlined_icons::IconDelete(), headerActions);
    m_deleteAll->setObjectName(QStringLiteral("pinnedManagementDeleteAll"));
    m_deleteAll->setAccentRole(adqt::widgets::AdButton::AccentRole::Danger);
    headerActionsLayout->addWidget(m_deleteAll);
    m_refresh = new ThemedHeaderIconButton(metric, outlined_icons::Reload(), headerActions);
    m_refresh->setObjectName(QStringLiteral("pinnedManagementRefresh"));
    headerActionsLayout->addWidget(m_refresh);
    auto* headerLayout = new QHBoxLayout;
    headerLayout->setContentsMargins(0, metric.marginMD, 0, 0);
    headerLayout->setSpacing(12);
    headerLayout->addWidget(titleGroup, 1);
    headerLayout->addWidget(headerActions, 0, Qt::AlignTop);
    layout->addLayout(headerLayout);
    layout->addSpacing(metric.marginSM);

    auto* filters = new QWidget(content);
    filters->setObjectName(QStringLiteral("pinnedManagementFilters"));
    filters->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* filtersLayout = new QHBoxLayout(filters);
    filtersLayout->setContentsMargins(0, 0, 0, 0);
    filtersLayout->setSpacing(8);
    m_sourceFilter = new adqt::widgets::AdSelect(filters);
    m_sourceFilter->setObjectName(QStringLiteral("pinnedManagementSourceFilter"));
    m_sourceFilter->setMode(adqt::widgets::AdSelect::Mode::Multiple);
    m_sourceFilter->setAllowClear(true);
    m_sourceFilter->setMaxTagCount(1);
    m_sourceFilter->setMinimumWidth(140);
    m_sourceFilter->setMaximumWidth(170);
    filtersLayout->addWidget(m_sourceFilter);
    m_dates = new adqt::widgets::AdDateRangePicker(filters);
    m_dates->setObjectName(QStringLiteral("pinnedManagementDateFilter"));
    m_dates->setAllowClear(true);
    m_dates->setMinimumWidth(220);
    m_dates->setMaximumWidth(260);
    filtersLayout->addWidget(m_dates);
    filtersLayout->addStretch(1);
    layout->addWidget(filters);
    layout->addSpacing(metric.marginSM);

    m_selectionBar = new QWidget(content);
    m_selectionBar->setObjectName(QStringLiteral("pinnedManagementSelectionBar"));
    m_selectionBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* selectionOuter = new QVBoxLayout(m_selectionBar);
    selectionOuter->setContentsMargins(0, 0, 0, metric.marginSM);
    m_selectionPanel = new HistorySelectionBar(m_selectionBar);
    m_selectionPanel->setObjectName(QStringLiteral("pinnedManagementSelectionPanel"));
    m_selectionPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_selectionLayout = new QBoxLayout(QBoxLayout::LeftToRight, m_selectionPanel);
    m_selectionLayout->setContentsMargins(metric.paddingMD, metric.paddingSM, metric.paddingMD,
                                          metric.paddingSM);
    m_selectionLayout->setSpacing(metric.marginXS);
    m_selectionSummary = new QLabel(m_selectionPanel);
    m_selectionSummary->setObjectName(QStringLiteral("pinnedManagementSelectionSummary"));
    m_selectionLayout->addWidget(m_selectionSummary);
    m_selectionLayout->addStretch(1);
    m_selectionActions = new QWidget(m_selectionPanel);
    m_selectionActions->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto* selectionActionsLayout = new QHBoxLayout(m_selectionActions);
    selectionActionsLayout->setContentsMargins(0, 0, 0, 0);
    selectionActionsLayout->setSpacing(0);
    m_deleteSelected = new adqt::widgets::AdButton(m_selectionActions);
    m_deleteSelected->setObjectName(QStringLiteral("pinnedManagementDeleteSelected"));
    m_deleteSelected->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Link);
    m_deleteSelected->setAccentRole(adqt::widgets::AdButton::AccentRole::Danger);
    selectionActionsLayout->addWidget(m_deleteSelected);
    m_selectPage = new adqt::widgets::AdButton(m_selectionActions);
    m_selectPage->setObjectName(QStringLiteral("pinnedManagementSelectAll"));
    m_selectPage->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Link);
    m_selectPage->setAccentRole(adqt::widgets::AdButton::AccentRole::Primary);
    selectionActionsLayout->addWidget(m_selectPage);
    m_deselect = new adqt::widgets::AdButton(m_selectionActions);
    m_deselect->setObjectName(QStringLiteral("pinnedManagementDeselectAll"));
    m_deselect->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Link);
    m_deselect->setAccentRole(adqt::widgets::AdButton::AccentRole::Primary);
    selectionActionsLayout->addWidget(m_deselect);
    m_selectionLayout->addWidget(m_selectionActions);
    selectionOuter->addWidget(m_selectionPanel);
    layout->addWidget(m_selectionBar);
    m_selectionBar->hide();

    m_deleteAllConfirmation = new adqt::widgets::AdPopconfirm(content);
    m_deleteAllConfirmation->setObjectName(QStringLiteral("pinnedManagementDeleteAllConfirm"));
    m_deleteAllConfirmation->setSourceWidget(m_deleteAll);
    m_deleteAllConfirmation->setButtonAccentRole(adqt::widgets::AdPopconfirm::StandardButton::Ok,
                                                 adqt::widgets::AdButton::AccentRole::Danger);
    m_deleteSelectedConfirmation = new adqt::widgets::AdPopconfirm(content);
    m_deleteSelectedConfirmation->setObjectName(
        QStringLiteral("pinnedManagementDeleteSelectedConfirm"));
    m_deleteSelectedConfirmation->setSourceWidget(m_deleteSelected);
    m_deleteSelectedConfirmation->setButtonAccentRole(
        adqt::widgets::AdPopconfirm::StandardButton::Ok,
        adqt::widgets::AdButton::AccentRole::Danger);

    m_entries = new QWidget(content);
    m_entries->setObjectName(QStringLiteral("pinnedManagementEntries"));
    m_entries->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_entries->setMinimumHeight(260);
    m_entryLayout = new QVBoxLayout(m_entries);
    m_entryLayout->setContentsMargins(0, 0, 0, 0);
    m_entryLayout->setSpacing(metric.marginSM);
    layout->addWidget(m_entries, 1);
    m_emptyIcon = new QLabel(m_entries);
    m_emptyIcon->setObjectName(QStringLiteral("pinnedManagementEmptyIcon"));
    m_emptyIcon->setAlignment(Qt::AlignCenter);
    m_emptyIcon->setFixedSize(96, 62);
    m_emptyTitle = new QLabel(m_entries);
    m_emptyTitle->setObjectName(QStringLiteral("pinnedManagementEmptyTitle"));
    m_emptyTitle->setAlignment(Qt::AlignCenter);
    m_emptyDescription = new QLabel(m_entries);
    m_emptyDescription->setObjectName(QStringLiteral("pinnedManagementEmptyDescription"));
    m_emptyDescription->setAlignment(Qt::AlignCenter);
    m_emptyDescription->setWordWrap(true);

    m_pagination = new adqt::widgets::AdPagination(content);
    m_pagination->setObjectName(QStringLiteral("pinnedManagementPagination"));
    m_pagination->setPageSize(10);
    m_pagination->setPageSizeOptions({10, 20, 50});
    m_pagination->setSizeChangerMode(adqt::widgets::AdPagination::SizeChangerMode::Always);
    m_pagination->setAlignment(adqt::widgets::AdPagination::Alignment::End);
    m_pagination->setResponsive(true);
    m_pagination->setTotalTextFormatter(
        [](int total, const adqt::widgets::AdPagination::Range& range) {
            return PinnedWindowManagementPageWidget::tr("%1-%2 of %3")
                .arg(range.first)
                .arg(range.last)
                .arg(total);
        });
    layout->addSpacing(14);
    layout->addWidget(m_pagination);

    connect(m_refresh, &QAbstractButton::clicked, this, &PinnedWindowManagementPageWidget::refresh);
    connect(m_deleteAllConfirmation, &adqt::widgets::AdPopconfirm::accepted, this,
            &PinnedWindowManagementPageWidget::requestDeleteAll);
    connect(m_deleteSelectedConfirmation, &adqt::widgets::AdPopconfirm::accepted, this,
            &PinnedWindowManagementPageWidget::deleteSelected);
    connect(m_selectPage, &QAbstractButton::clicked, this,
            &PinnedWindowManagementPageWidget::selectCurrentPage);
    connect(m_deselect, &QAbstractButton::clicked, this,
            &PinnedWindowManagementPageWidget::clearSelection);
    connect(m_sourceFilter, &adqt::widgets::AdSelect::currentValuesChanged, this,
            [this](const QVariantList&) {
                clearSelection();
                rebuildFilteredRecords(true);
            });
    connect(m_dates, &adqt::widgets::AdDateRangePicker::rangeChanged, this,
            [this](const QDate&, const QDate&) {
                clearSelection();
                rebuildFilteredRecords(true);
            });
    connect(m_pagination, &adqt::widgets::AdPagination::currentPageChanged, this, [this](int) {
        if (!m_updatingPagination)
            rebuildEntries();
    });
    connect(m_pagination, &adqt::widgets::AdPagination::pageSizeChanged, this, [this](int) {
        if (!m_updatingPagination)
            rebuildEntries();
    });
    connect(m_source, &PinnedWindowManagementDataSource::changed, this,
            &PinnedWindowManagementPageWidget::refresh);
    connect(m_source, &PinnedWindowManagementDataSource::previewReady, this,
            [this](const QString& id, quint64 generation, const QImage& image) {
                if (generation != m_generation || !m_previews.value(id)) {
                    return;
                }
                auto* preview = static_cast<PreviewLabel*>(m_previews.value(id).data());
                if (image.isNull()) {
                    preview->setText(tr("Preview unavailable"));
                } else {
                    preview->setImage(image);
                }
            });
    connect(&styles::ThemeManager::instance(), &styles::ThemeManager::themeChanged, this,
            [this]() { applyTheme(styles::ThemeManager::instance().themeColorScheme()); });

    retranslateUi();
    refresh();
    applyTheme(m_scheme);
}

PinnedWindowManagementPageWidget::~PinnedWindowManagementPageWidget() {
    if (m_source) {
        m_source->cancelPreviews();
    }
}

QString
PinnedWindowManagementPageWidget::sourceLabel(storage::PinnedWindowCreationSource source) const {
    switch (source) {
    case storage::PinnedWindowCreationSource::Screenshot:
        return tr("Screenshot");
    case storage::PinnedWindowCreationSource::ScreenshotHistory:
        return tr("Screenshot history");
    case storage::PinnedWindowCreationSource::Clipboard:
        return tr("Clipboard");
    case storage::PinnedWindowCreationSource::SelectedFiles:
        return tr("Selected files");
    case storage::PinnedWindowCreationSource::Other:
        return tr("Other / legacy");
    }
    return {};
}

void PinnedWindowManagementPageWidget::refresh() {
    if (!m_source) {
        return;
    }
    m_records = m_source->records();
    QSet<QString> ids;
    for (const auto& record : std::as_const(m_records)) {
        ids.insert(record.id);
    }
    m_selected.intersect(ids);
    std::sort(m_records.begin(), m_records.end(), [](const auto& left, const auto& right) {
        if (left.activitySequence != right.activitySequence) {
            return left.activitySequence > right.activitySequence;
        }
        if (left.activityUtc() != right.activityUtc()) {
            return left.activityUtc() > right.activityUtc();
        }
        return left.id < right.id;
    });
    rebuildFilteredRecords(false);
}

void PinnedWindowManagementPageWidget::rebuildFilteredRecords(bool resetPage) {
    m_filteredRecords.clear();
    const auto sources = m_sourceFilter->currentValues();
    for (const auto& record : std::as_const(m_records)) {
        if (!sources.isEmpty() && !sources.contains(static_cast<int>(record.creationSource))) {
            continue;
        }
        const QDate date = record.activityUtc().toLocalTime().date();
        if ((m_dates->startDate().isValid() && date < m_dates->startDate()) ||
            (m_dates->endDate().isValid() && date > m_dates->endDate())) {
            continue;
        }
        m_filteredRecords.push_back(record);
    }
    m_updatingPagination = true;
    if (resetPage) {
        m_pagination->setCurrentPage(1);
    }
    m_pagination->setTotal(static_cast<int>(m_filteredRecords.size()));
    m_updatingPagination = false;
    updateHeader();
    rebuildEntries();
}

void PinnedWindowManagementPageWidget::rebuildEntries() {
    ++m_generation;
    if (m_source) {
        m_source->cancelPreviews();
    }
    m_previews.clear();
    while (auto* item = m_entryLayout->takeAt(0)) {
        QWidget* widget = item->widget();
        if (widget == m_emptyIcon || widget == m_emptyTitle || widget == m_emptyDescription) {
            widget->hide();
        } else {
            delete widget;
        }
        delete item;
    }
    m_pageIds.clear();

    const int first = std::max(0, (m_pagination->currentPage() - 1) * m_pagination->pageSize());
    const int last =
        std::min(first + m_pagination->pageSize(), static_cast<int>(m_filteredRecords.size()));
    QHash<QString, QString> groups;
    if (m_source) {
        for (const auto& group : m_source->groups()) {
            groups.insert(group.id,
                          group.id == QStringLiteral("default") ? tr("Default") : group.name);
        }
    }

    for (int index = first; index < last; ++index) {
        const auto record = m_filteredRecords[index];
        m_pageIds.push_back(record.id);
        auto* row = new QFrame(m_entries);
        row->setObjectName(QStringLiteral("pinnedManagementRecord"));
        row->setProperty("recordId", record.id);
        row->setFrameShape(QFrame::NoFrame);
        row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        auto* rowLayout =
            new QBoxLayout(m_entries->width() >= kWideEntryBreakpoint ? QBoxLayout::LeftToRight
                                                                      : QBoxLayout::TopToBottom,
                           row);
        rowLayout->setContentsMargins(18, 16, 18, 16);
        rowLayout->setSpacing(18);
        auto* details = new QWidget(row);
        auto* detailsLayout = new QVBoxLayout(details);
        detailsLayout->setContentsMargins(0, 0, 0, 0);
        detailsLayout->setSpacing(9);
        auto* checkbox = new adqt::widgets::AdCheckbox(
            record.activityUtc().toLocalTime().toString(QStringLiteral("yyyy-MM-dd  HH:mm:ss")),
            details);
        checkbox->setObjectName(QStringLiteral("pinnedManagementEntrySelection"));
        checkbox->setChecked(m_selected.contains(record.id));
        checkbox->setAccessibleName(tr("Select record"));
        detailsLayout->addWidget(checkbox);
        auto* source = new QLabel(sourceLabel(record.creationSource), details);
        source->setObjectName(QStringLiteral("pinnedManagementSourceBadge"));
        detailsLayout->addWidget(source, 0, Qt::AlignLeft);
        auto* group =
            new QLabel(tr("Group: %1").arg(groups.value(record.groupId, tr("Default"))), details);
        group->setObjectName(QStringLiteral("pinnedManagementGroup"));
        detailsLayout->addWidget(group);
        auto* status = new QLabel(record.ignored ? tr("Closed") : tr("Retained"), details);
        status->setObjectName(QStringLiteral("pinnedManagementStatus"));
        detailsLayout->addWidget(status);
        detailsLayout->addStretch(1);
        auto* actions = new QHBoxLayout;
        actions->setContentsMargins(0, 0, 0, 0);
        actions->setSpacing(6);
        auto* show = new adqt::widgets::AdButton(details);
        show->setObjectName(QStringLiteral("pinnedManagementEntryShow"));
        show->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
        show->setAccentRole(adqt::widgets::AdButton::AccentRole::Primary);
        show->setText(record.ignored ? tr("Restore") : tr("Show"));
        actions->addWidget(show);
        auto* remove = new adqt::widgets::AdButton(details);
        remove->setObjectName(QStringLiteral("pinnedManagementEntryDelete"));
        remove->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
        remove->setAccentRole(adqt::widgets::AdButton::AccentRole::Danger);
        remove->setText(tr("Delete"));
        actions->addWidget(remove);
        actions->addStretch(1);
        detailsLayout->addLayout(actions);
        rowLayout->addWidget(details, 1);

        auto* preview = new PreviewLabel(row);
        preview->setObjectName(QStringLiteral("pinnedManagementPreview"));
        preview->setFixedSize(kPreviewWidth, kPreviewHeight);
        preview->setAlignment(Qt::AlignCenter);
        preview->setText(tr("Loading preview…"));
        rowLayout->addWidget(preview, 0, Qt::AlignCenter);
        m_previews.insert(record.id, preview);
        auto* confirmation = new adqt::widgets::AdPopconfirm(row);
        confirmation->setObjectName(QStringLiteral("pinnedManagementEntryDeleteConfirm"));
        confirmation->setSourceWidget(remove);
        confirmation->setPlacement(adqt::widgets::AdPopconfirm::Placement::Top);
        confirmation->setPopupLayerMode(adqt::widgets::AdPopconfirm::PopupLayerMode::QtTool);
        confirmation->setButtonAccentRole(adqt::widgets::AdPopconfirm::StandardButton::Ok,
                                          adqt::widgets::AdButton::AccentRole::Danger);
        confirmation->setText(tr("Delete this pinned window?"));
        confirmation->setInformativeText(
            tr("The saved record and its open window will be removed"));
        confirmation->setButtonText(adqt::widgets::AdPopconfirm::StandardButton::Ok, tr("Delete"));
        confirmation->setButtonText(adqt::widgets::AdPopconfirm::StandardButton::Cancel,
                                    tr("Cancel"));
        connect(confirmation, &adqt::widgets::AdPopconfirm::accepted, this,
                [this, id = record.id]() {
                    if (m_source)
                        m_source->removeRecords({id});
                });
        connect(show, &QAbstractButton::clicked, this, [this, id = record.id]() {
            if (m_source)
                m_source->showRecord(id);
        });
        connect(checkbox, &QAbstractButton::toggled, this, [this, id = record.id](bool selected) {
            if (selected)
                m_selected.insert(id);
            else
                m_selected.remove(id);
            updateSelectionBar();
        });
        m_entryLayout->addWidget(row);
        row->show();
        if (m_source) {
            m_source->requestPreview(record.id, m_generation);
        }
    }

    if (m_pageIds.isEmpty()) {
        m_emptyTitle->setText(m_records.isEmpty() ? tr("No pinned windows")
                                                  : tr("No matching pinned windows"));
        m_emptyDescription->setText(
            m_records.isEmpty() ? tr("Pinned images and text will appear here")
                                : tr("Change the source or date range to see more pinned windows"));
        m_entryLayout->addSpacing(48);
        m_entryLayout->addStretch(1);
        m_entryLayout->addWidget(m_emptyIcon, 0, Qt::AlignHCenter);
        m_entryLayout->addSpacing(18);
        m_entryLayout->addWidget(m_emptyTitle);
        m_entryLayout->addSpacing(8);
        m_entryLayout->addWidget(m_emptyDescription);
        m_entryLayout->addStretch(1);
        m_entryLayout->addSpacing(48);
    } else {
        m_emptyIcon->hide();
        m_emptyTitle->hide();
        m_emptyDescription->hide();
        m_entryLayout->addStretch(1);
    }
    updateSelectionBar();
    applyTheme(m_scheme);
}

void PinnedWindowManagementPageWidget::updateHeader() {
    m_count->setText(tr("%n pinned window(s)", nullptr, static_cast<int>(m_records.size())));
    m_deleteAll->setEnabled(!m_records.isEmpty());
    m_deleteAllConfirmation->setEnabled(!m_records.isEmpty());
    updateSelectionBar();
}

void PinnedWindowManagementPageWidget::updateSelectionBar() {
    const int count = static_cast<int>(m_selected.size());
    const QString summary = tr("Selected %n item(s)", nullptr, count);
    m_selectionSummary->setText(summary);
    m_selectionBar->setAccessibleName(summary);
    m_deleteSelectedConfirmation->setText(tr("Delete %n selected item(s)?", nullptr, count));
    bool allSelected = !m_pageIds.isEmpty();
    for (const auto& id : std::as_const(m_pageIds)) {
        allSelected &= m_selected.contains(id);
    }
    m_selectPage->setEnabled(!m_pageIds.isEmpty() && !allSelected);
    m_deleteSelected->setEnabled(count > 0);
    m_deleteSelectedConfirmation->setEnabled(count > 0);
    m_deselect->setEnabled(count > 0);
    const bool wide = m_entries->width() >= kWideEntryBreakpoint;
    m_selectionLayout->setDirection(wide ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
    m_selectionLayout->setAlignment(m_selectionSummary, wide ? Qt::AlignVCenter : Qt::AlignLeft);
    m_selectionLayout->setAlignment(m_selectionActions, wide ? Qt::AlignRight | Qt::AlignVCenter
                                                             : Qt::AlignRight | Qt::AlignTop);
    m_selectionBar->setVisible(count > 0);
}

void PinnedWindowManagementPageWidget::clearSelection() {
    m_selected.clear();
    for (auto* checkbox : m_entries->findChildren<adqt::widgets::AdCheckbox*>()) {
        const QSignalBlocker blocker(checkbox);
        checkbox->setChecked(false);
    }
    updateSelectionBar();
}

void PinnedWindowManagementPageWidget::selectCurrentPage() {
    for (const auto& id : std::as_const(m_pageIds)) {
        m_selected.insert(id);
    }
    for (auto* checkbox : m_entries->findChildren<adqt::widgets::AdCheckbox*>()) {
        const QSignalBlocker blocker(checkbox);
        checkbox->setChecked(true);
    }
    updateSelectionBar();
}

void PinnedWindowManagementPageWidget::deleteSelected() {
    if (!m_source || m_selected.isEmpty()) {
        return;
    }
    QVector<QString> ids;
    for (const auto& record : std::as_const(m_records)) {
        if (m_selected.contains(record.id)) {
            ids.push_back(record.id);
        }
    }
    if (!ids.isEmpty()) {
        m_source->removeRecords(ids);
        clearSelection();
    }
}

void PinnedWindowManagementPageWidget::requestDeleteAll() {
    if (!m_source) {
        return;
    }
    QVector<QString> ids;
    ids.reserve(m_records.size());
    for (const auto& record : std::as_const(m_records)) {
        ids.push_back(record.id);
    }
    if (!ids.isEmpty()) {
        m_source->removeRecords(ids);
    }
}

void PinnedWindowManagementPageWidget::retranslateUi() {
    m_title->setText(tr("Pin to Screen Management"));
    m_sourceFilter->setPlaceholder(tr("All sources"));
    m_sourceFilter->setAccessibleName(tr("Source"));
    const auto selectedSources = m_sourceFilter->currentValues();
    QVector<adqt::widgets::AdSelect::Option> options;
    for (int index = 0;
         index <= static_cast<int>(storage::PinnedWindowCreationSource::SelectedFiles); ++index) {
        options.push_back(
            {index, sourceLabel(static_cast<storage::PinnedWindowCreationSource>(index))});
    }
    {
        const QSignalBlocker blocker(m_sourceFilter);
        m_sourceFilter->setOptions(options);
        m_sourceFilter->setCurrentValues(selectedSources);
    }
    m_dates->setAccessibleName(tr("Date"));
    m_dates->setRangePlaceholders(tr("Start date"), tr("End date"));
    m_selectionBar->setAccessibleDescription(tr("Bulk actions for selected pinned windows"));
    m_deleteSelected->setText(tr("Delete"));
    m_deleteSelected->setToolTip(tr("Delete selected pinned windows"));
    m_deleteSelected->setAccessibleName(tr("Delete selected pinned windows"));
    m_selectPage->setText(tr("Select all"));
    m_selectPage->setToolTip(tr("Select all entries on this page"));
    m_selectPage->setAccessibleName(tr("Select all entries on this page"));
    m_deselect->setText(tr("Deselect all"));
    m_deselect->setToolTip(tr("Deselect all pinned windows"));
    m_deselect->setAccessibleName(tr("Deselect all pinned windows"));
    m_deleteAll->setToolTip(tr("Delete all pinned windows"));
    m_deleteAll->setAccessibleName(tr("Delete all pinned windows"));
    m_refresh->setToolTip(tr("Refresh pinned windows"));
    m_refresh->setAccessibleName(tr("Refresh pinned windows"));
    for (auto* confirmation : {m_deleteAllConfirmation, m_deleteSelectedConfirmation}) {
        confirmation->setInformativeText(
            tr("Saved records and their open windows will be removed"));
        confirmation->setButtonText(adqt::widgets::AdPopconfirm::StandardButton::Ok, tr("Delete"));
        confirmation->setButtonText(adqt::widgets::AdPopconfirm::StandardButton::Cancel,
                                    tr("Cancel"));
    }
    m_deleteAllConfirmation->setText(tr("Delete all pinned windows?"));
    updateHeader();
}

void PinnedWindowManagementPageWidget::applyTheme(const styles::ThemeColorScheme& scheme) {
    m_scheme = scheme;
    QPalette titlePalette = m_title->palette();
    titlePalette.setColor(QPalette::WindowText, scheme.map.colorText);
    m_title->setPalette(titlePalette);
    QFont titleFont = m_title->font();
    titleFont.setPixelSize(scheme.metricAlias.fontSizeHeading4);
    titleFont.setWeight(QFont::DemiBold);
    m_title->setFont(titleFont);
    QPalette muted = m_count->palette();
    muted.setColor(QPalette::WindowText, scheme.map.colorTextSecondary);
    m_count->setPalette(muted);
    m_selectionSummary->setPalette(muted);
    m_emptyDescription->setPalette(muted);
    m_emptyTitle->setPalette(titlePalette);
    QFont emptyFont = m_emptyTitle->font();
    emptyFont.setPixelSize(scheme.metricAlias.fontSizeLG);
    emptyFont.setWeight(QFont::DemiBold);
    m_emptyTitle->setFont(emptyFont);
    static_cast<HistorySelectionBar*>(m_selectionPanel)->applyTheme(scheme);
    m_emptyIcon->setPixmap(adqt::icons::renderIconPixmap(
        adqt::widgets::icons::twotone::EmptySimple(adqt::icons::IconColors::threeTone(
            scheme.map.colorFill, scheme.map.colorFillQuaternary, scheme.map.colorFillTertiary)),
        {m_emptyIcon->size(), m_emptyIcon->devicePixelRatioF()}));
    const QString cardStyle =
        QStringLiteral("QFrame#pinnedManagementRecord { background: %1; border: %2px solid %3; "
                       "border-radius: %4px; }")
            .arg(scheme.map.colorBgContainer.name())
            .arg(std::max<qreal>(1.0, scheme.metricAlias.lineWidth))
            .arg(scheme.map.colorBorderSecondary.name())
            .arg(scheme.metricAlias.borderRadius);
    for (auto* row : m_entries->findChildren<QFrame*>(QStringLiteral("pinnedManagementRecord"))) {
        row->setStyleSheet(cardStyle);
        auto* badge = row->findChild<QLabel*>(QStringLiteral("pinnedManagementSourceBadge"));
        badge->setStyleSheet(
            QStringLiteral("QLabel { color: %1; background: %2; border: 1px solid %3; "
                           "border-radius: 4px; padding: 2px 7px; }")
                .arg(scheme.map.colorPrimaryText.name(), scheme.map.colorPrimaryBg.name(),
                     scheme.map.colorPrimaryBorder.name()));
        auto* checkbox = row->findChild<adqt::widgets::AdCheckbox*>();
        QFont dateFont = checkbox->font();
        dateFont.setPixelSize(scheme.metricAlias.fontSizeLG);
        dateFont.setWeight(QFont::DemiBold);
        checkbox->setFont(dateFont);
        checkbox->setPalette(titlePalette);
        row->findChild<QLabel*>(QStringLiteral("pinnedManagementGroup"))->setPalette(muted);
        row->findChild<QLabel*>(QStringLiteral("pinnedManagementStatus"))->setPalette(muted);
    }
    updateSelectionBar();
}

void PinnedWindowManagementPageWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
        rebuildEntries();
    }
}

void PinnedWindowManagementPageWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const bool wide = m_entries != nullptr && m_entries->width() >= kWideEntryBreakpoint;
    if (m_entries != nullptr) {
        for (auto* row :
             m_entries->findChildren<QFrame*>(QStringLiteral("pinnedManagementRecord"))) {
            if (auto* rowLayout = dynamic_cast<QBoxLayout*>(row->layout())) {
                rowLayout->setDirection(wide ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
            }
        }
        updateSelectionBar();
    }
}
