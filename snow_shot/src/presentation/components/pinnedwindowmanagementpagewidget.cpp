#include "snow_shot/presentation/components/pinnedwindowmanagementpagewidget.h"

#include "snow_shot/presentation/components/historyselectionbar.h"
#include "snow_shot/presentation/components/historypagecommon.h"
#include "snow_shot/presentation/components/thumbnailcache.h"
#include "snow_shot/presentation/components/emptystateicon.h"
#include "snow_shot/presentation/components/pagecontainerwidget.h"
#include "snow_shot/presentation/components/themedheadericonbutton.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"

#include "antd_icons.h"
#include "widgets/button.h"
#include "widgets/checkbox.h"
#include "widgets/date_picker.h"
#include "widgets/image.h"
#include "widgets/pagination.h"
#include "widgets/popconfirm.h"
#include "widgets/select.h"

#include <QBoxLayout>
#include <QCache>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QSet>
#include <QSignalBlocker>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

using namespace snow_shot;

namespace {
namespace outlined_icons = adqt::icons::antd::outlined;
namespace styles = snow_shot::presentation::styles;
namespace storage = snow_shot::storage;
namespace history_page = snow_shot::presentation::components::history_page;
namespace thumbnail_cache = snow_shot::presentation::components::thumbnail_cache;

constexpr int kPreviewWidth = 260;
constexpr int kPreviewHeight = 156;

QImage loadPinnedImage(storage::PinnedWindowRepository* repository, const QString& id) {
    const auto record = repository->loadRecord(id);
    if (!record) {
        return {};
    }
    if (record->sourceKind == storage::PinnedWindowSourceKind::ClipboardText) {
        ScreenshotClipboardOriginalContent content;
        content.html = record->originalHtml;
        content.text = record->originalText;
        const auto rendered = ScreenshotClipboardContentReader::renderOriginalText(
            content, record->firstCreationTextDpi);
        if (rendered) {
            return rendered->image;
        }
    }
    return record->image;
}

QImage loadPinnedPreview(storage::PinnedWindowRepository* repository, const QString& id) {
    const auto source = repository->loadPreviewSource(id);
    if (!source) {
        return {};
    }
    if (source->sourceKind == storage::PinnedWindowSourceKind::ClipboardText) {
        ScreenshotClipboardOriginalContent content;
        content.html = source->originalHtml;
        content.text = source->originalText;
        const auto rendered = ScreenshotClipboardContentReader::renderOriginalText(
            content, source->firstCreationTextDpi);
        return rendered ? rendered->image : QImage{};
    }
    return source->image;
}

struct PinnedPreviewCacheEntry {
    QImage image;
    QSize naturalSize;
};

class ApplicationPinnedDataSource final : public PinnedWindowManagementDataSource {
  public:
    explicit ApplicationPinnedDataSource(QObject* parent)
        : PinnedWindowManagementDataSource(parent) {
        m_previewCache.setMaxCost(32 * 1024);
        connect(&storage::ApplicationStorage::instance(),
                &storage::ApplicationStorage::pinnedWindowsChanged, this, [this]() {
                    auto& applicationStorage = storage::ApplicationStorage::instance();
                    if (applicationStorage.isInitialized()) {
                        QSet<QString> retainedIds;
                        for (const auto& summary : applicationStorage.pinnedWindows().summaries()) {
                            retainedIds.insert(summary.id);
                        }
                        for (const QString& key : m_previewCache.keys()) {
                            if (!retainedIds.contains(key.left(key.indexOf(u':')))) {
                                m_previewCache.remove(key);
                            }
                        }
                    } else {
                        m_previewCache.clear();
                    }
                    emit changed();
                });
    }

    ~ApplicationPinnedDataSource() override {
        m_alive->store(false);
        cancelPreviews();
    }

    QVector<storage::PinnedWindowSummary> records() const override {
        return storage::ApplicationStorage::instance().pinnedWindows().summaries();
    }

    QVector<storage::PinnedWindowGroup> groups() const override {
        return storage::ApplicationStorage::instance().pinnedWindows().groups();
    }

    std::optional<quint64> previewRevision(const QString& id) const override {
        return storage::ApplicationStorage::instance().pinnedWindows().previewSourceRevision(id);
    }

    void cancelPreviews() override {
        m_previewEpoch->fetch_add(1);
    }

    void requestPreview(const QString& id, quint64 requestId, const QSize& targetSize) override {
        auto& applicationStorage = storage::ApplicationStorage::instance();
        if (!applicationStorage.isInitialized()) {
            emit previewReady(id, requestId, {}, {});
            return;
        }
        auto* repository = &applicationStorage.pinnedWindows();
        const auto revision = repository->previewSourceRevision(id);
        if (!revision) {
            emit previewReady(id, requestId, {}, {});
            return;
        }
        const QSize boundedSize = targetSize.isValid() && !targetSize.isEmpty()
                                      ? targetSize
                                      : QSize(kPreviewWidth, kPreviewHeight);
        const QString cacheKey = id + u':' + QString::number(*revision) + u':' +
                                 QString::number(boundedSize.width()) + u'x' +
                                 QString::number(boundedSize.height());
        if (const PinnedPreviewCacheEntry* cached = m_previewCache.object(cacheKey)) {
            emit previewReady(id, requestId, cached->image, cached->naturalSize);
            return;
        }
        const auto alive = m_alive;
        const auto previewEpoch = m_previewEpoch;
        const quint64 requestedEpoch = previewEpoch->load();
        const QString cachePath = thumbnail_cache::pathForKey(QStringLiteral("pinned|") + cacheKey);
        auto* receiver = this;
        applicationStorage.pinnedPreviewPool().start([alive, previewEpoch, requestedEpoch, receiver,
                                                      repository, id, requestId, boundedSize,
                                                      cacheKey, cachePath]() {
            if (!alive->load() || previewEpoch->load() != requestedEpoch) {
                return;
            }
            auto thumbnail = thumbnail_cache::load(cachePath);
            QImage image = std::move(thumbnail.image);
            QSize naturalSize = thumbnail.naturalSize;
            if (image.isNull()) {
                image = loadPinnedPreview(repository, id);
                naturalSize = image.size();
                if (!image.isNull() && (image.width() > boundedSize.width() ||
                                        image.height() > boundedSize.height())) {
                    image =
                        image.scaled(boundedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                }
                if (!image.isNull())
                    thumbnail_cache::persist(cachePath, image, naturalSize);
            }
            if (!alive->load() || previewEpoch->load() != requestedEpoch) {
                return;
            }
            QMetaObject::invokeMethod(
                &storage::ApplicationStorage::instance(),
                [alive, previewEpoch, requestedEpoch, receiver, id, requestId, cacheKey, image,
                 naturalSize]() {
                    if (!alive->load() || previewEpoch->load() != requestedEpoch) {
                        return;
                    }
                    if (!image.isNull()) {
                        const auto cost =
                            std::max<qsizetype>(1, (image.sizeInBytes() + 1023) / 1024);
                        receiver->m_previewCache.insert(
                            cacheKey, new PinnedPreviewCacheEntry{image, naturalSize},
                            static_cast<int>(cost));
                    }
                    emit receiver->previewReady(id, requestId, image, naturalSize);
                },
                Qt::QueuedConnection);
        });
    }

    void requestFullImage(const QString& id, quint64 requestId) override {
        auto& applicationStorage = storage::ApplicationStorage::instance();
        if (!applicationStorage.isInitialized()) {
            emit fullImageReady(id, requestId, {});
            return;
        }
        auto* repository = &applicationStorage.pinnedWindows();
        const auto alive = m_alive;
        auto* receiver = this;
        applicationStorage.pinnedFullImagePool().start(
            [alive, receiver, repository, id, requestId]() {
                if (!alive->load()) {
                    return;
                }
                const QImage image = loadPinnedImage(repository, id);
                if (!alive->load()) {
                    return;
                }
                QMetaObject::invokeMethod(
                    &storage::ApplicationStorage::instance(),
                    [alive, receiver, id, requestId, image]() {
                        if (alive->load()) {
                            emit receiver->fullImageReady(id, requestId, image);
                        }
                    },
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
    std::shared_ptr<std::atomic_bool> m_alive = std::make_shared<std::atomic_bool>(true);
    std::shared_ptr<std::atomic<quint64>> m_previewEpoch =
        std::make_shared<std::atomic<quint64>>(0);
    QCache<QString, PinnedPreviewCacheEntry> m_previewCache;
};

class PinnedImageReply final : public adqt::widgets::AdImageReply {
  public:
    PinnedImageReply(PinnedWindowManagementDataSource* source, QString id,
                     const adqt::widgets::AdImageLoadOptions& options, QObject* parent)
        : AdImageReply(parent), m_source(source), m_id(std::move(id)),
          m_requestId(++s_nextRequestId), m_targetSize(options.targetPixelSize) {
        const bool thumbnail = m_targetSize.isValid() && !m_targetSize.isEmpty();
        if (thumbnail) {
            connect(source, &PinnedWindowManagementDataSource::previewReady, this,
                    [this](const QString& id, quint64 requestId, const QImage& image,
                           const QSize& naturalSize) {
                        completeImage(id, requestId, image, naturalSize);
                    });
        } else {
            connect(source, &PinnedWindowManagementDataSource::fullImageReady, this,
                    [this](const QString& id, quint64 requestId, const QImage& image) {
                        completeImage(id, requestId, image, image.size());
                    });
        }
        QMetaObject::invokeMethod(
            this,
            [this]() {
                if (!isFinished()) {
                    if (m_source) {
                        if (m_targetSize.isValid() && !m_targetSize.isEmpty()) {
                            m_source->requestPreview(m_id, m_requestId, m_targetSize);
                        } else {
                            m_source->requestFullImage(m_id, m_requestId);
                        }
                    } else {
                        fail(QStringLiteral("Pinned image source is unavailable"));
                    }
                }
            },
            Qt::QueuedConnection);
    }

    void abort() override {
        if (!isFinished()) {
            fail(QStringLiteral("Pinned image load aborted"));
        }
    }

  private:
    void completeImage(const QString& id, quint64 requestId, const QImage& image,
                       const QSize& naturalSize) {
        if (isFinished() || id != m_id || requestId != m_requestId) {
            return;
        }
        if (image.isNull()) {
            fail(QStringLiteral("Pinned image is unavailable"));
        } else {
            succeed(image, naturalSize);
        }
    }

    static std::atomic<quint64> s_nextRequestId;
    QPointer<PinnedWindowManagementDataSource> m_source;
    QString m_id;
    quint64 m_requestId;
    QSize m_targetSize;
};

std::atomic<quint64> PinnedImageReply::s_nextRequestId{0};

class PinnedImageLoader final : public adqt::widgets::AdImageLoader {
  public:
    PinnedImageLoader(PinnedWindowManagementDataSource* source, QString id, QObject* parent)
        : AdImageLoader(parent), m_source(source), m_id(std::move(id)) {}

    adqt::widgets::AdImageReply* load(const QUrl&, const adqt::widgets::AdImageLoadOptions& options,
                                      QObject* parent) override {
        return new PinnedImageReply(m_source, m_id, options, parent);
    }

  private:
    QPointer<PinnedWindowManagementDataSource> m_source;
    QString m_id;
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
    history_page::configureSourceFilter(m_sourceFilter);
    filtersLayout->addWidget(m_sourceFilter);
    m_dates = new adqt::widgets::AdDateRangePicker(filters);
    m_dates->setObjectName(QStringLiteral("pinnedManagementDateFilter"));
    history_page::configureDateFilter(m_dates);
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
    history_page::configureSelectionAction(m_deleteSelected, true);
    selectionActionsLayout->addWidget(m_deleteSelected);
    m_selectPage = new adqt::widgets::AdButton(m_selectionActions);
    m_selectPage->setObjectName(QStringLiteral("pinnedManagementSelectAll"));
    history_page::configureSelectionAction(m_selectPage, false);
    selectionActionsLayout->addWidget(m_selectPage);
    m_deselect = new adqt::widgets::AdButton(m_selectionActions);
    m_deselect->setObjectName(QStringLiteral("pinnedManagementDeselectAll"));
    history_page::configureSelectionAction(m_deselect, false);
    selectionActionsLayout->addWidget(m_deselect);
    m_selectionLayout->addWidget(m_selectionActions);
    selectionOuter->addWidget(m_selectionPanel);
    layout->addWidget(m_selectionBar);
    m_selectionBar->hide();

    m_deleteAllConfirmation = new adqt::widgets::AdPopconfirm(content);
    m_deleteAllConfirmation->setObjectName(QStringLiteral("pinnedManagementDeleteAllConfirm"));
    m_deleteAllConfirmation->setSourceWidget(m_deleteAll);
    history_page::configureDeleteConfirmation(m_deleteAllConfirmation);
    m_deleteSelectedConfirmation = new adqt::widgets::AdPopconfirm(content);
    m_deleteSelectedConfirmation->setObjectName(
        QStringLiteral("pinnedManagementDeleteSelectedConfirm"));
    m_deleteSelectedConfirmation->setSourceWidget(m_deleteSelected);
    history_page::configureDeleteConfirmation(m_deleteSelectedConfirmation);
    m_entryDeleteConfirmation = new adqt::widgets::AdPopconfirm(content);
    m_entryDeleteConfirmation->setObjectName(QStringLiteral("pinnedManagementEntryDeleteConfirm"));
    m_entryDeleteConfirmation->setPlacement(adqt::widgets::AdPopconfirm::Placement::Top);
    m_entryDeleteConfirmation->setPopupLayerMode(
        adqt::widgets::AdPopconfirm::PopupLayerMode::QtTool);
    history_page::configureDeleteConfirmation(m_entryDeleteConfirmation);

    m_entries = new QWidget(content);
    m_entries->setObjectName(QStringLiteral("pinnedManagementEntries"));
    m_entries->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_entries->setMinimumHeight(260);
    m_entryLayout = new QVBoxLayout(m_entries);
    m_entryLayout->setContentsMargins(0, 0, 0, 0);
    m_entryLayout->setSpacing(metric.marginSM);
    m_entries->installEventFilter(this);
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
    history_page::configurePagination(m_pagination);
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
    connect(m_entryDeleteConfirmation, &adqt::widgets::AdPopconfirm::accepted, this, [this]() {
        const QString id = std::exchange(m_pendingEntryDeleteId, {});
        m_entryDeleteConfirmation->hide();
        m_entryDeleteConfirmation->setSourceWidget(nullptr);
        if (m_source && !id.isEmpty())
            m_source->removeRecords({id});
    });
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
    for (const auto& record : std::as_const(m_records)) {
        if (!history_page::matchesFilters(static_cast<int>(record.creationSource),
                                          record.activityUtc().toLocalTime().date(), m_sourceFilter,
                                          m_dates)) {
            continue;
        }
        m_filteredRecords.push_back(record);
    }
    history_page::updatePagination(m_pagination, static_cast<int>(m_filteredRecords.size()),
                                   resetPage, m_updatingPagination);
    updateHeader();
    rebuildEntries();
}

void PinnedWindowManagementPageWidget::rebuildEntries() {
    const auto [first, last] =
        history_page::pageRange(m_pagination, static_cast<int>(m_filteredRecords.size()));
    QSet<QString> retainedIds;
    QVector<QString> nextPageIds;
    for (int index = first; index < last; ++index) {
        const QString& id = m_filteredRecords[index].id;
        retainedIds.insert(id);
        nextPageIds.push_back(id);
    }
    bool layoutChanged = nextPageIds != m_pageIds || m_entryLayout->count() == 0;
    for (const QString& id : std::as_const(nextPageIds)) {
        const auto revision = m_source ? m_source->previewRevision(id) : std::nullopt;
        if (m_entryRows.value(id).isNull() || !revision ||
            m_entryPreviewRevisions.value(id) != *revision) {
            layoutChanged = true;
        }
    }
    if (layoutChanged && !m_pendingEntryDeleteId.isEmpty() &&
        !retainedIds.contains(m_pendingEntryDeleteId)) {
        m_entryDeleteConfirmation->hide();
        m_entryDeleteConfirmation->setSourceWidget(nullptr);
        m_pendingEntryDeleteId.clear();
    }
    if (layoutChanged) {
        while (auto* item = m_entryLayout->takeAt(0)) {
            QWidget* widget = item->widget();
            if (widget == m_emptyIcon || widget == m_emptyTitle || widget == m_emptyDescription)
                widget->hide();
            delete item;
        }
        for (auto it = m_entryRows.begin(); it != m_entryRows.end();) {
            if (!retainedIds.contains(it.key()) || it.value().isNull()) {
                delete it.value();
                m_entryPreviewRevisions.remove(it.key());
                it = m_entryRows.erase(it);
            } else {
                ++it;
            }
        }
    }
    m_pageIds = std::move(nextPageIds);
    bool createdRows = false;
    QHash<QString, QString> groups;
    if (m_source) {
        for (const auto& group : m_source->groups()) {
            groups.insert(group.id,
                          group.id == QStringLiteral("default") ? tr("Default") : group.name);
        }
    }

    for (int index = first; index < last; ++index) {
        const auto record = m_filteredRecords[index];
        const auto previewRevision = m_source ? m_source->previewRevision(record.id) : std::nullopt;
        QFrame* row = m_entryRows.value(record.id);
        if (row != nullptr &&
            (!previewRevision || m_entryPreviewRevisions.value(record.id) != *previewRevision)) {
            if (m_pendingEntryDeleteId == record.id) {
                m_entryDeleteConfirmation->hide();
                m_entryDeleteConfirmation->setSourceWidget(nullptr);
                m_pendingEntryDeleteId.clear();
            }
            delete row;
            m_entryRows.remove(record.id);
            m_entryPreviewRevisions.remove(record.id);
            row = nullptr;
        }
        if (row == nullptr) {
            createdRows = true;
            row = new QFrame(m_entries);
            row->setObjectName(QStringLiteral("pinnedManagementRecord"));
            row->setProperty("recordId", record.id);
            row->setFrameShape(QFrame::NoFrame);
            row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
            auto* rowLayout = new QBoxLayout(
                m_entries->width() >= history_page::kWideEntryBreakpoint ? QBoxLayout::LeftToRight
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
            auto* group = new QLabel(
                tr("Group: %1").arg(groups.value(record.groupId, tr("Default"))), details);
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

            auto* preview = new adqt::widgets::AdImage(row);
            preview->setObjectName(QStringLiteral("pinnedManagementPreview"));
            preview->setFixedSize(kPreviewWidth, kPreviewHeight);
            adqt::widgets::AdImage::SemanticStyles imageStyles;
            imageStyles.root.borderColor = QColor(Qt::transparent);
            preview->setSemanticStyles(imageStyles);
            preview->setLoadingPolicy(adqt::widgets::AdImage::LoadingPolicy::WhenVisible);
            preview->setDecodePolicy(adqt::widgets::AdImage::DecodePolicy::FitWidget);
            preview->setPreferredImageSize(QSize(kPreviewWidth, kPreviewHeight));
            rowLayout->addWidget(preview, 0, Qt::AlignCenter);
            auto* viewer = new adqt::widgets::AdImageViewer(row);
            viewer->setOwnerWindow(preview);
            auto* imageLoader = new PinnedImageLoader(m_source, record.id, viewer);
            viewer->setImageLoader(imageLoader);
            auto* previewModel = new adqt::widgets::AdImageListModel(viewer);
            QUrl imageSource;
            imageSource.setScheme(QStringLiteral("pinned"));
            imageSource.setPath(record.id);
            const QString altText = tr("Pinned window image");
            previewModel->setItems({{imageSource, altText}});
            viewer->setModel(previewModel);
            preview->setViewer(viewer);
            preview->setImageLoader(imageLoader);
            preview->setPreviewRow(0);
            preview->setAltText(altText);
            preview->setSource(imageSource);
            connect(remove, &QAbstractButton::clicked, this, [this, remove, id = record.id]() {
                m_entryDeleteConfirmation->hide();
                m_pendingEntryDeleteId = id;
                m_entryDeleteConfirmation->setSourceWidget(remove);
                m_entryDeleteConfirmation->show();
            });
            connect(show, &QAbstractButton::clicked, this, [this, id = record.id]() {
                if (m_source)
                    m_source->showRecord(id);
            });
            connect(checkbox, &QAbstractButton::toggled, this,
                    [this, id = record.id](bool selected) {
                        if (selected)
                            m_selected.insert(id);
                        else
                            m_selected.remove(id);
                        updateSelectionBar();
                    });
            m_entryRows.insert(record.id, row);
            if (previewRevision)
                m_entryPreviewRevisions.insert(record.id, *previewRevision);
        }
        auto* checkbox = row->findChild<adqt::widgets::AdCheckbox*>(
            QStringLiteral("pinnedManagementEntrySelection"));
        checkbox->setText(
            record.activityUtc().toLocalTime().toString(QStringLiteral("yyyy-MM-dd  HH:mm:ss")));
        checkbox->setAccessibleName(tr("Select record"));
        {
            const QSignalBlocker blocker(checkbox);
            checkbox->setChecked(m_selected.contains(record.id));
        }
        row->findChild<QLabel*>(QStringLiteral("pinnedManagementSourceBadge"))
            ->setText(sourceLabel(record.creationSource));
        row->findChild<QLabel*>(QStringLiteral("pinnedManagementGroup"))
            ->setText(tr("Group: %1").arg(groups.value(record.groupId, tr("Default"))));
        row->findChild<QLabel*>(QStringLiteral("pinnedManagementStatus"))
            ->setText(record.ignored ? tr("Closed") : tr("Retained"));
        row->findChild<adqt::widgets::AdButton*>(QStringLiteral("pinnedManagementEntryShow"))
            ->setText(record.ignored ? tr("Restore") : tr("Show"));
        row->findChild<adqt::widgets::AdButton*>(QStringLiteral("pinnedManagementEntryDelete"))
            ->setText(tr("Delete"));
        const QString altText = tr("Pinned window image");
        row->findChild<adqt::widgets::AdImage*>(QStringLiteral("pinnedManagementPreview"))
            ->setAltText(altText);
        if (auto* viewer = row->findChild<adqt::widgets::AdImageViewer*>()) {
            if (auto* model = qobject_cast<adqt::widgets::AdImageListModel*>(viewer->model())) {
                QUrl imageSource;
                imageSource.setScheme(QStringLiteral("pinned"));
                imageSource.setPath(record.id);
                model->setItems({{imageSource, altText}});
            }
        }
        if (layoutChanged)
            m_entryLayout->addWidget(row);
        row->show();
    }

    if (m_pageIds.isEmpty()) {
        m_emptyTitle->setText(m_records.isEmpty() ? tr("No pinned windows")
                                                  : tr("No matching pinned windows"));
        m_emptyDescription->setText(
            m_records.isEmpty() ? tr("Pinned images and text will appear here")
                                : tr("Change the source or date range to see more pinned windows"));
        if (layoutChanged) {
            m_entryLayout->addSpacing(48);
            m_entryLayout->addStretch(1);
            m_entryLayout->addWidget(m_emptyIcon, 0, Qt::AlignHCenter);
            m_entryLayout->addSpacing(18);
            m_entryLayout->addWidget(m_emptyTitle);
            m_entryLayout->addSpacing(8);
            m_entryLayout->addWidget(m_emptyDescription);
            m_entryLayout->addStretch(1);
            m_entryLayout->addSpacing(48);
        }
    } else {
        m_emptyIcon->hide();
        m_emptyTitle->hide();
        m_emptyDescription->hide();
        if (layoutChanged)
            m_entryLayout->addStretch(1);
    }
    updateSelectionBar();
    if (createdRows)
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
    history_page::updateSelectionBar(
        {m_selectionBar, m_selectionLayout, m_selectionSummary, m_selectionActions, m_selectPage,
         m_deleteSelected, m_deselect, m_deleteSelectedConfirmation},
        m_selected, m_pageIds, true, m_entries->width(), tr("Selected %n item(s)", nullptr, count),
        tr("Delete %n selected item(s)?", nullptr, count));
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
    m_entryDeleteConfirmation->setText(tr("Delete this pinned window?"));
    m_entryDeleteConfirmation->setInformativeText(
        tr("The saved record and its open window will be removed"));
    m_entryDeleteConfirmation->setButtonText(adqt::widgets::AdPopconfirm::StandardButton::Ok,
                                             tr("Delete"));
    m_entryDeleteConfirmation->setButtonText(adqt::widgets::AdPopconfirm::StandardButton::Cancel,
                                             tr("Cancel"));
    updateHeader();
}

void PinnedWindowManagementPageWidget::applyTheme(const styles::ThemeColorScheme& scheme) {
    m_scheme = scheme;
    history_page::applyTextTheme(
        {m_title, m_count, m_selectionSummary, m_emptyTitle, m_emptyDescription}, scheme);
    const QPalette titlePalette = m_title->palette();
    const QPalette muted = m_count->palette();
    static_cast<HistorySelectionBar*>(m_selectionPanel)->applyTheme(scheme);
    m_emptyIcon->setPixmap(snow_shot::presentation::components::renderEmptyStateIcon(
        scheme, m_emptyIcon->size(), m_emptyIcon->devicePixelRatioF()));
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

bool PinnedWindowManagementPageWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_entries && event->type() == QEvent::Resize) {
        updateResponsiveLayout();
    }
    return QWidget::eventFilter(watched, event);
}

void PinnedWindowManagementPageWidget::updateResponsiveLayout() {
    const auto direction = m_entries->width() >= history_page::kWideEntryBreakpoint
                               ? QBoxLayout::LeftToRight
                               : QBoxLayout::TopToBottom;
    for (auto* row : m_entries->findChildren<QFrame*>(QStringLiteral("pinnedManagementRecord"))) {
        if (auto* rowLayout = dynamic_cast<QBoxLayout*>(row->layout());
            rowLayout != nullptr && rowLayout->direction() != direction) {
            rowLayout->setDirection(direction);
        }
    }
    updateSelectionBar();
}
