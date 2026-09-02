#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_SCREENSHOTHISTORYPAGEWIDGET_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_SCREENSHOTHISTORYPAGEWIDGET_H

#include "snow_shot/presentation/styles/themecolorscheme.h"
#include "snow_shot/storage/capturehistorytypes.h"

#include <QDate>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QMetaType>
#include <QSet>
#include <QVector>
#include <QWidget>

#include <optional>

struct ScreenshotHistoryAssetResolution {
    QString recordId;
    std::optional<snow_shot::storage::CaptureHistoryAssetSet> assets;
};
Q_DECLARE_METATYPE(ScreenshotHistoryAssetResolution)
Q_DECLARE_METATYPE(QVector<ScreenshotHistoryAssetResolution>)

struct ScreenshotHistoryResultResolution {
    QString recordId;
    std::optional<QImage> image;
};
Q_DECLARE_METATYPE(ScreenshotHistoryResultResolution)

class QEvent;
class QLabel;
class QResizeEvent;
class QShowEvent;
class QHideEvent;
class QVBoxLayout;
namespace adqt::widgets {
class AdButton;
class AdDateRangePicker;
class AdPagination;
class AdPopconfirm;
class AdScrollArea;
class AdSelect;
} // namespace adqt::widgets

class ScreenshotHistoryPageDataSource : public QObject {
    Q_OBJECT

  public:
    using QObject::QObject;
    ~ScreenshotHistoryPageDataSource() override = default;

    // Cancels page-facing work submitted by the current request generation.
    // Cache-maintenance work is intentionally independent and is not canceled.
    virtual void cancelPending() {}

    [[nodiscard]] virtual QVector<snow_shot::storage::CaptureHistoryRecord> records() const = 0;
    [[nodiscard]] virtual std::optional<snow_shot::storage::CaptureHistoryAssetSet>
    displayAssets(const snow_shot::storage::CaptureHistoryRecord& record) const = 0;
    virtual bool supportsAsyncDisplayAssets() const { return false; }
    virtual void requestDisplayAssets(const QVector<snow_shot::storage::CaptureHistoryRecord>&,
                                      quint64) {}
    virtual void requestResultImage(const snow_shot::storage::CaptureHistoryRecord&, quint64) {}
    virtual void remove(const QString& id) = 0;
    [[nodiscard]] virtual bool requestClear() = 0;

  signals:
    void historyChanged();
    void clearFinished(bool success, const QString& error);
    void displayAssetsReady(quint64 generation,
                            const QVector<ScreenshotHistoryAssetResolution>& resolutions);
    void resultImageReady(quint64 generation, const ScreenshotHistoryResultResolution& resolution);
};

class ScreenshotHistoryPageWidget final : public QWidget {
    Q_OBJECT

  public:
    explicit ScreenshotHistoryPageWidget(QWidget* parent = nullptr);
    ScreenshotHistoryPageWidget(ScreenshotHistoryPageDataSource* dataSource, QWidget* parent);
    ~ScreenshotHistoryPageWidget() override;

    void refresh();
    void setActive(bool active);
    [[nodiscard]] bool isActive() const;
    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme);
    void retranslateUi();

  signals:
    void editRequested(const QString& recordId);

  protected:
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    void rebuildFilteredRecords(bool resetPage);
    void rebuildEntries();
    void updateHeader();
    void requestDeleteAll();
    void removeEntry(const QString& entryId);
    void handleHistoryChanged();
    void handleDisplayAssetsReady(
        quint64 generation, const QVector<ScreenshotHistoryAssetResolution>& resolutions);
    void handleResultImageReady(quint64 generation,
                                const ScreenshotHistoryResultResolution& resolution);
    void copyEntry(const snow_shot::storage::CaptureHistoryRecord& record);
    void queueRefresh();
    void updateEmptyStateMinimumHeight();
    [[nodiscard]] bool matchesFilters(const snow_shot::storage::CaptureHistoryRecord& record) const;

    QLabel* m_titleLabel = nullptr;
    QLabel* m_countLabel = nullptr;
    adqt::widgets::AdSelect* m_sourceFilter = nullptr;
    adqt::widgets::AdDateRangePicker* m_dateRangeFilter = nullptr;
    adqt::widgets::AdButton* m_deleteAllButton = nullptr;
    adqt::widgets::AdButton* m_refreshButton = nullptr;
    adqt::widgets::AdPopconfirm* m_deleteAllConfirmation = nullptr;
    adqt::widgets::AdScrollArea* m_scrollArea = nullptr;
    QWidget* m_entriesHost = nullptr;
    QVBoxLayout* m_entriesLayout = nullptr;
    QLabel* m_emptyIcon = nullptr;
    QLabel* m_emptyTitle = nullptr;
    QLabel* m_emptyDescription = nullptr;
    adqt::widgets::AdPagination* m_pagination = nullptr;
    QVector<snow_shot::storage::CaptureHistoryRecord> m_records;
    QVector<snow_shot::storage::CaptureHistoryRecord> m_filteredRecords;
    QPointer<ScreenshotHistoryPageDataSource> m_dataSource;
    QHash<QString, QWidget*> m_entryWidgetsById;
    QVector<QString> m_entryLayoutIds;
    QHash<QString, std::optional<snow_shot::storage::CaptureHistoryAssetSet>> m_resolvedAssets;
    snow_shot::presentation::styles::ThemeColorScheme m_colorScheme;
    bool m_active = false;
    bool m_dirty = true;
    bool m_refreshQueued = false;
    bool m_updatingPagination = false;
    quint64 m_assetGeneration = 0;
    quint64 m_resultGeneration = 0;
    QSet<QString> m_pendingResultRecordIds;
    int m_emptyStateMinimumHeight = 0;
};

// Diagnostics for the dedicated history executor. These counters include both
// running and queued work and are intentionally read-only to callers.
[[nodiscard]] int screenshotHistoryPendingJobCount();
[[nodiscard]] int screenshotHistoryPendingPersistenceJobCount();
[[nodiscard]] int screenshotHistoryQueuedPersistenceJobCount();
[[nodiscard]] quint64 screenshotHistorySubmittedPersistenceJobCount();
[[nodiscard]] quint64 screenshotHistoryCompletedPersistenceJobCount();
[[nodiscard]] int screenshotHistoryWorkerCount();
[[nodiscard]] int screenshotHistoryWorkerExpiryTimeout();

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_SCREENSHOTHISTORYPAGEWIDGET_H
