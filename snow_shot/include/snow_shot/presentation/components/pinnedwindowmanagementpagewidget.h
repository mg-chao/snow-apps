#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_PINNEDWINDOWMANAGEMENTPAGEWIDGET_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_PINNEDWINDOWMANAGEMENTPAGEWIDGET_H

#include "snow_shot/presentation/styles/themecolorscheme.h"
#include "snow_shot/storage/pinnedwindowrepository.h"

#include <QHash>
#include <QPointer>
#include <QSet>
#include <QSize>
#include <QWidget>
#include <optional>

class QBoxLayout;
class QFrame;
class QLabel;
class QVBoxLayout;
namespace adqt::widgets {
class AdButton;
class AdDateRangePicker;
class AdPagination;
class AdPopconfirm;
class AdSelect;
} // namespace adqt::widgets

class PinnedWindowManagementDataSource : public QObject {
    Q_OBJECT

  public:
    using QObject::QObject;
    ~PinnedWindowManagementDataSource() override = default;
    virtual QVector<snow_shot::storage::PinnedWindowSummary> records() const = 0;
    virtual QVector<snow_shot::storage::PinnedWindowGroup> groups() const = 0;
    virtual void requestPreview(const QString& id, quint64 requestId, const QSize& targetSize) = 0;
    // Unknown revisions force a fresh row so custom sources cannot show a stale preview.
    virtual std::optional<quint64> previewRevision(const QString&) const {
        return std::nullopt;
    }
    virtual void requestFullImage(const QString& id, quint64 requestId) = 0;
    virtual void cancelPreviews() {}
    virtual void showRecord(const QString& id) = 0;
    virtual void removeRecords(const QVector<QString>& ids) = 0;

  signals:
    void changed();
    void previewReady(const QString& id, quint64 requestId, const QImage& image,
                      const QSize& naturalSize);
    void fullImageReady(const QString& id, quint64 requestId, const QImage& image);
};

class PinnedWindowManagementPageWidget final : public QWidget {
    Q_OBJECT

  public:
    explicit PinnedWindowManagementPageWidget(QWidget* parent = nullptr);
    PinnedWindowManagementPageWidget(PinnedWindowManagementDataSource* source, QWidget* parent);
    ~PinnedWindowManagementPageWidget() override;

    void refresh();
    void retranslateUi();
    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme);

  protected:
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void rebuildFilteredRecords(bool resetPage);
    void rebuildEntries();
    void updateResponsiveLayout();
    void updateHeader();
    void updateSelectionBar();
    void clearSelection();
    void selectCurrentPage();
    void deleteSelected();
    void requestDeleteAll();
    QString sourceLabel(snow_shot::storage::PinnedWindowCreationSource source) const;

    QPointer<PinnedWindowManagementDataSource> m_source;
    QLabel* m_title = nullptr;
    QLabel* m_count = nullptr;
    adqt::widgets::AdSelect* m_sourceFilter = nullptr;
    adqt::widgets::AdDateRangePicker* m_dates = nullptr;
    adqt::widgets::AdPagination* m_pagination = nullptr;
    adqt::widgets::AdButton* m_refresh = nullptr;
    adqt::widgets::AdButton* m_deleteAll = nullptr;
    adqt::widgets::AdPopconfirm* m_deleteAllConfirmation = nullptr;
    QWidget* m_selectionBar = nullptr;
    QWidget* m_selectionPanel = nullptr;
    QBoxLayout* m_selectionLayout = nullptr;
    QLabel* m_selectionSummary = nullptr;
    QWidget* m_selectionActions = nullptr;
    adqt::widgets::AdButton* m_deleteSelected = nullptr;
    adqt::widgets::AdButton* m_selectPage = nullptr;
    adqt::widgets::AdButton* m_deselect = nullptr;
    adqt::widgets::AdPopconfirm* m_deleteSelectedConfirmation = nullptr;
    adqt::widgets::AdPopconfirm* m_entryDeleteConfirmation = nullptr;
    QString m_pendingEntryDeleteId;
    QWidget* m_entries = nullptr;
    QVBoxLayout* m_entryLayout = nullptr;
    QLabel* m_emptyIcon = nullptr;
    QLabel* m_emptyTitle = nullptr;
    QLabel* m_emptyDescription = nullptr;
    QVector<snow_shot::storage::PinnedWindowSummary> m_records;
    QVector<snow_shot::storage::PinnedWindowSummary> m_filteredRecords;
    QVector<QString> m_pageIds;
    QHash<QString, QPointer<QFrame>> m_entryRows;
    QHash<QString, quint64> m_entryPreviewRevisions;
    QSet<QString> m_selected;
    snow_shot::presentation::styles::ThemeColorScheme m_scheme;
    bool m_updatingPagination = false;
};

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_PINNEDWINDOWMANAGEMENTPAGEWIDGET_H
