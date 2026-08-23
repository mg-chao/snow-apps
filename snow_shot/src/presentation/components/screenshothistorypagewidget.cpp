#include "snow_shot/presentation/components/screenshothistorypagewidget.h"

#include "snowimageqtcodec.h"

#include "snow_shot/presentation/components/pagecontainerwidget.h"
#include "snow_shot/presentation/components/themedheadericonbutton.h"
#include "snow_shot/presentation/screenshotclipboardservice.h"

#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/capturehistoryrepository.h"

#include "antd_icons.h"
#include "icon_registry.h"
#include "icons/widget_icons.h"
#include "widgets/button.h"
#include "widgets/carousel.h"
#include "widgets/date_picker.h"
#include "widgets/image.h"
#include "widgets/pagination.h"
#include "widgets/popconfirm.h"
#include "widgets/select.h"
#include "widgets/scroll_area.h"

#include <QBoxLayout>
#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QSaveFile>
#include <QStandardPaths>
#include <QPainter>
#include <QPalette>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QSizePolicy>
#include <QUrl>
#include <QVariantList>
#include <QVBoxLayout>
#include <QTimer>
#include <QThreadPool>

#include <algorithm>

namespace {
namespace outlined_icons = adqt::icons::antd::outlined;
namespace storage = snow_shot::storage;
namespace styles = snow_shot::presentation::styles;

constexpr int kDefaultPageSize = 10;
constexpr int kWideEntryBreakpoint = 560;
constexpr int kPreviewWidth = 260;
constexpr int kPreviewHeight = 156;
// Reserve the empty-state stack before its first layout measurement is available.
constexpr int kEmptyStateBaselineHeight = 260;

QColor colorOnBackground(const QColor& foreground, const QColor& background) {
    if (!foreground.isValid()) {
        return background;
    }
    if (!background.isValid() || foreground.alpha() >= 255) {
        return foreground;
    }
    const qreal alpha = foreground.alphaF();
    return QColor::fromRgbF(foreground.redF() * alpha + background.redF() * (1.0 - alpha),
                            foreground.greenF() * alpha + background.greenF() * (1.0 - alpha),
                            foreground.blueF() * alpha + background.blueF() * (1.0 - alpha));
}

adqt::widgets::AdSelect::Option sourceOption(const QString& value, const QString& label) {
    adqt::widgets::AdSelect::Option option;
    option.value = value;
    option.label = label;
    return option;
}

QString sourceKey(storage::CaptureHistorySource source) {
    switch (source) {
    case storage::CaptureHistorySource::CopiedToClipboard:
        return QStringLiteral("clipboard");
    case storage::CaptureHistorySource::PinnedToScreen:
        return QStringLiteral("pinned");
    case storage::CaptureHistorySource::CurrentMonitor:
        return QStringLiteral("current-monitor");
    case storage::CaptureHistorySource::FocusedWindow:
        return QStringLiteral("focused-window");
    }
    return QStringLiteral("clipboard");
}

QString formattedBytes(qint64 bytes) {
    if (bytes < 1024) {
        return ScreenshotHistoryPageWidget::tr("%1 B").arg(bytes);
    }
    if (bytes < 1024 * 1024) {
        return ScreenshotHistoryPageWidget::tr("%1 KB").arg(static_cast<double>(bytes) / 1024.0, 0,
                                                            'f', 1);
    }
    return ScreenshotHistoryPageWidget::tr("%1 MB").arg(
        static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
}

void clearLayoutItems(QLayout* layout) {
    if (layout == nullptr) {
        return;
    }
    while (QLayoutItem* item = layout->takeAt(0)) {
        delete item;
    }
}

class ApplicationStorageHistoryDataSource final : public ScreenshotHistoryPageDataSource {
  public:
    explicit ApplicationStorageHistoryDataSource(QObject* parent = nullptr)
        : ScreenshotHistoryPageDataSource(parent) {
        auto& applicationStorage = storage::ApplicationStorage::instance();
        connect(&applicationStorage, &storage::ApplicationStorage::captureHistoryChanged, this,
                &ScreenshotHistoryPageDataSource::historyChanged);
        connect(&applicationStorage, &storage::ApplicationStorage::captureHistoryClearFinished,
                this, &ScreenshotHistoryPageDataSource::clearFinished);
    }

    QVector<storage::CaptureHistoryRecord> records() const override {
        auto& applicationStorage = storage::ApplicationStorage::instance();
        return applicationStorage.isInitialized() ? applicationStorage.captureHistory().records()
                                                  : QVector<storage::CaptureHistoryRecord>{};
    }

    std::optional<storage::CaptureHistoryAssetSet>
    displayAssets(const storage::CaptureHistoryRecord& record) const override {
        auto& applicationStorage = storage::ApplicationStorage::instance();
        return applicationStorage.isInitialized()
                   ? applicationStorage.captureHistory().displayAssets(record)
                   : std::nullopt;
    }

    bool supportsAsyncDisplayAssets() const override { return true; }

    void requestDisplayAssets(const QVector<storage::CaptureHistoryRecord>& records,
                              quint64 generation) override {
        const QPointer<ApplicationStorageHistoryDataSource> guarded(this);
        QThreadPool::globalInstance()->start(
            [guarded, records, generation]() {
                if (guarded == nullptr) {
                    return;
                }
                QVector<ScreenshotHistoryAssetResolution> resolutions;
                resolutions.reserve(records.size());
                for (const storage::CaptureHistoryRecord& record : records) {
                    if (guarded == nullptr) {
                        return;
                    }
                    resolutions.push_back({record.id, guarded->displayAssets(record)});
                }
                QMetaObject::invokeMethod(
                    guarded,
                    [guarded, generation, resolutions = std::move(resolutions)]() {
                        if (guarded != nullptr) {
                            emit guarded->displayAssetsReady(generation, resolutions);
                        }
                    },
                    Qt::QueuedConnection);
            });
    }

    void requestResultImage(const storage::CaptureHistoryRecord& record,
                            quint64 generation) override {
        const QPointer<ApplicationStorageHistoryDataSource> guarded(this);
        QThreadPool::globalInstance()->start([guarded, record, generation]() {
            if (guarded == nullptr) {
                return;
            }
            std::optional<QImage> image;
            auto& applicationStorage = storage::ApplicationStorage::instance();
            if (applicationStorage.isInitialized()) {
                image = applicationStorage.captureHistory().loadResultImage(record);
            }
            QMetaObject::invokeMethod(
                guarded,
                [guarded, generation, recordId = record.id, image = std::move(image)]() mutable {
                    if (guarded != nullptr) {
                        emit guarded->resultImageReady(
                            generation, ScreenshotHistoryResultResolution{recordId, std::move(image)});
                    }
                },
                Qt::QueuedConnection);
        });
    }

    void remove(const QString& id) override {
        auto& applicationStorage = storage::ApplicationStorage::instance();
        if (applicationStorage.isInitialized()) {
            static_cast<void>(applicationStorage.captureHistory().remove(id));
        }
    }

    bool requestClear() override {
        return storage::ApplicationStorage::instance().requestCaptureHistoryClear();
    }
};

class HistoryThumbnailReply final : public adqt::widgets::AdImageReply {
  public:
    explicit HistoryThumbnailReply(QObject* parent = nullptr) : AdImageReply(parent) {}

    void attach(adqt::widgets::AdImageReply* source, std::function<void(const QImage&)> onSuccess) {
        sourceReply_ = source;
        onSuccess_ = std::move(onSuccess);
        if (sourceReply_ == nullptr) {
            fail(QStringLiteral("Thumbnail source reply was unavailable"));
            return;
        }
        connect(sourceReply_, &adqt::widgets::AdImageReply::finished, this, [this]() {
            auto* source = sourceReply_.data();
            if (source == nullptr) {
                fail(QStringLiteral("Thumbnail source reply was destroyed"));
                return;
            }
            if (source->isSuccessful()) {
                const QImage image = source->image();
                if (onSuccess_) {
                    onSuccess_(image);
                }
                succeed(image, source->naturalSize());
            } else {
                fail(source->errorString());
            }
            source->deleteLater();
            sourceReply_.clear();
        });
    }

    void abort() override {
        if (isFinished()) {
            return;
        }
        if (sourceReply_ != nullptr) {
            sourceReply_->abort();
        }
        fail(QStringLiteral("Thumbnail load aborted"));
    }

  private:
    QPointer<adqt::widgets::AdImageReply> sourceReply_;
    std::function<void(const QImage&)> onSuccess_;
};

class HistoryThumbnailLoader final : public adqt::widgets::AdImageLoader {
  public:
    explicit HistoryThumbnailLoader(QObject* parent = nullptr)
        : AdImageLoader(parent),
          m_cacheDirectory([&]() {
              const QString cacheRoot =
                  QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
              return cacheRoot.isEmpty()
                         ? QString()
                         : QDir(cacheRoot).filePath(QStringLiteral("snow-shot/history-thumbnails"));
          }()) {}

    adqt::widgets::AdImageReply* load(const QUrl& source, QObject* parent = nullptr) override {
        return load(source, adqt::widgets::AdImageLoadOptions{}, parent);
    }

    adqt::widgets::AdImageReply* load(const QUrl& source,
                                      const adqt::widgets::AdImageLoadOptions& options,
                                      QObject* parent = nullptr) override {
        auto* reply = new HistoryThumbnailReply(parent);
        const QString cachePath = thumbnailPath(source, options);
        if (cachePath.isEmpty()) {
            reply->attach(adqt::widgets::defaultAdImageLoader()->load(source, options, reply), {});
            return reply;
        }

        const QFileInfo cacheInfo(cachePath);
        if (cacheInfo.isFile() && !cacheInfo.isSymLink()) {
            reply->attach(adqt::widgets::defaultAdImageLoader()->load(
                              QUrl::fromLocalFile(cachePath), {}, reply), {});
            return reply;
        }

        auto* sourceReply =
            adqt::widgets::defaultAdImageLoader()->load(source, options, reply);
        reply->attach(sourceReply, [this, cachePath](const QImage& image) {
            if (!image.isNull()) {
                persist(cachePath, image);
            }
        });
        return reply;
    }

  private:
    QString thumbnailPath(const QUrl& source,
                          const adqt::widgets::AdImageLoadOptions& options) const {
        if (m_cacheDirectory.isEmpty() || source.isEmpty()) {
            return {};
        }
        QString sourceKey;
        if (source.isLocalFile()) {
            const QFileInfo info(source.toLocalFile());
            sourceKey = QStringLiteral("%1|%2|%3")
                            .arg(QDir::fromNativeSeparators(info.absoluteFilePath()))
                            .arg(info.size())
                            .arg(info.lastModified().toMSecsSinceEpoch());
        } else {
            sourceKey = source.toString();
        }
        sourceKey += QStringLiteral("|%1x%2|%3|%4|v1")
                         .arg(options.targetPixelSize.width())
                         .arg(options.targetPixelSize.height())
                         .arg(static_cast<int>(options.aspectRatioMode))
                         .arg(options.allowUpscale ? 1 : 0);
        const QByteArray digest =
            QCryptographicHash::hash(sourceKey.toUtf8(), QCryptographicHash::Sha256).toHex();
        return QDir(m_cacheDirectory).filePath(QString::fromLatin1(digest) +
                                                QStringLiteral(".png"));
    }

    void persist(const QString& path, const QImage& image) const {
        const QString directory = QFileInfo(path).absolutePath();
        QThreadPool::globalInstance()->start([directory, path, image]() {
            if (!QDir().mkpath(directory)) {
                return;
            }
            QSaveFile file(path);
            const QByteArray png = snow_shot::image_codec::encodePng(image);
            if (!file.open(QIODevice::WriteOnly) || png.isEmpty() || file.write(png) != png.size() ||
                !file.commit()) {
                return;
            }

            QDir cache(directory);
            QFileInfoList entries =
                cache.entryInfoList({QStringLiteral("*.png")}, QDir::Files, QDir::Time | QDir::Reversed);
            qint64 totalBytes = 0;
            for (const QFileInfo& entry : entries) {
                totalBytes += entry.size();
            }
            constexpr qint64 kMaximumCacheBytes = 256LL * 1024LL * 1024LL;
            for (const QFileInfo& entry : entries) {
                if (totalBytes <= kMaximumCacheBytes) {
                    break;
                }
                if (QFile::remove(entry.absoluteFilePath())) {
                    totalBytes -= entry.size();
                }
            }
        });
    }

    QString m_cacheDirectory;
};

HistoryThumbnailLoader* historyThumbnailLoader() {
    static QPointer<HistoryThumbnailLoader> loader;
    if (loader == nullptr) {
        loader = new HistoryThumbnailLoader(QCoreApplication::instance());
    }
    return loader;
}

class HistoryEntryWidget final : public QFrame {
    Q_DECLARE_TR_FUNCTIONS(HistoryEntryWidget)

  public:
    HistoryEntryWidget(const storage::CaptureHistoryRecord& record,
                       const std::optional<storage::CaptureHistoryAssetSet>& assets,
                       std::function<void()> editRequested,
                       std::function<void()> copyRequested,
                       std::function<void()> deleteRequested, QWidget* parent = nullptr)
        : QFrame(parent), m_record(record), m_assets(assets),
          m_editRequested(std::move(editRequested)),
          m_copyRequested(std::move(copyRequested)),
          m_deleteRequested(std::move(deleteRequested)) {
        setObjectName(QStringLiteral("screenshotHistoryEntry-%1").arg(record.id));
        setFrameShape(QFrame::NoFrame);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

        m_layout = new QBoxLayout(QBoxLayout::LeftToRight, this);
        m_layout->setContentsMargins(18, 16, 18, 16);
        m_layout->setSpacing(18);

        auto* details = new QWidget(this);
        details->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto* detailsLayout = new QVBoxLayout(details);
        detailsLayout->setContentsMargins(0, 0, 0, 0);
        detailsLayout->setSpacing(9);

        auto* topRow = new QHBoxLayout;
        topRow->setContentsMargins(0, 0, 0, 0);
        topRow->setSpacing(8);
        m_dateLabel = new QLabel(
            record.createdUtc.toLocalTime().toString(QStringLiteral("yyyy-MM-dd  HH:mm:ss")),
            details);
        m_dateLabel->setObjectName(QStringLiteral("screenshotHistoryEntryDate"));
        m_dateLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        topRow->addWidget(m_dateLabel, 0, Qt::AlignVCenter);
        topRow->addStretch(1);
        m_sourceLabel = new QLabel(details);
        m_sourceLabel->setObjectName(QStringLiteral("screenshotHistorySourceBadge"));
        detailsLayout->addLayout(topRow);
        detailsLayout->addWidget(m_sourceLabel, 0, Qt::AlignLeft);

        const QRect selection = record.selection.rectangle;
        m_summaryLabel = new QLabel(HistoryEntryWidget::tr("%1 x %2 px  ·  %3 display(s)")
                                        .arg(selection.width())
                                        .arg(selection.height())
                                        .arg(record.displays.size()),
                                    details);
        m_summaryLabel->setObjectName(QStringLiteral("screenshotHistoryEntrySummary"));
        detailsLayout->addWidget(m_summaryLabel);

        m_metaLabel = new QLabel(HistoryEntryWidget::tr("Position %1, %2  ·  %3")
                                     .arg(selection.x())
                                     .arg(selection.y())
                                     .arg(formattedBytes(record.totalBytes)),
                                 details);
        m_metaLabel->setObjectName(QStringLiteral("screenshotHistoryEntryMetadata"));
        detailsLayout->addWidget(m_metaLabel);
        detailsLayout->addStretch(1);

        auto* actions = new QHBoxLayout;
        actions->setContentsMargins(0, 0, 0, 0);
        actions->setSpacing(6);
        m_editButton = new adqt::widgets::AdButton(details);
        m_editButton->setObjectName(QStringLiteral("screenshotHistoryEntryEdit"));
        m_editButton->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
        m_editButton->setAccentRole(adqt::widgets::AdButton::AccentRole::Primary);
        m_editButton->setIconRef(outlined_icons::Edit());
        actions->addWidget(m_editButton, 0);
        m_copyButton = new adqt::widgets::AdButton(details);
        m_copyButton->setObjectName(QStringLiteral("screenshotHistoryEntryCopy"));
        m_copyButton->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
        m_copyButton->setAccentRole(adqt::widgets::AdButton::AccentRole::Primary);
        m_copyButton->setIconRef(outlined_icons::Copy());
        m_copyButton->setVisible(record.result.has_value());
        actions->addWidget(m_copyButton, 0);
        m_deleteButton = new adqt::widgets::AdButton(details);
        m_deleteButton->setObjectName(QStringLiteral("screenshotHistoryEntryDelete"));
        m_deleteButton->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
        m_deleteButton->setAccentRole(adqt::widgets::AdButton::AccentRole::Danger);
        m_deleteButton->setIconRef(outlined_icons::IconDelete());
        actions->addWidget(m_deleteButton, 0);
        actions->addStretch(1);
        detailsLayout->addLayout(actions);
        m_layout->addWidget(details, 1);

        m_carousel = new adqt::widgets::AdCarousel(this);
        m_carousel->setObjectName(QStringLiteral("screenshotHistoryImageCarousel"));
        m_carousel->setFixedSize(kPreviewWidth, kPreviewHeight);
        m_carousel->setEffect(adqt::widgets::AdCarousel::Effect::Fade);
        m_carousel->setAutoplay(false);
        m_carousel->setDraggable(true);

        m_viewer = new adqt::widgets::AdImageViewer(this);
        m_previewModel = new adqt::widgets::AdImageListModel(m_viewer);
        adqt::widgets::AdImageItems previewItems;
        if (assets.has_value()) {
            previewItems.reserve(assets->displays.size() + (assets->result.has_value() ? 1 : 0));
            if (assets->result.has_value()) {
                adqt::widgets::AdImageItem item;
                item.source = assets->result->localFileUrl;
                item.altText = HistoryEntryWidget::tr("Screenshot result");
                previewItems.push_back(item);
            }
            for (const storage::CaptureHistoryDisplayAsset& asset : assets->displays) {
                adqt::widgets::AdImageItem item;
                item.source = asset.localFileUrl;
                item.altText = asset.name.isEmpty() ? HistoryEntryWidget::tr("Screenshot display")
                                                    : asset.name;
                previewItems.push_back(item);
            }
        }
        m_previewModel->setItems(previewItems);
        m_viewer->setModel(m_previewModel);

        for (qsizetype index = 0; index < previewItems.size(); ++index) {
            auto* slide = new QWidget;
            auto* slideLayout = new QVBoxLayout(slide);
            slideLayout->setContentsMargins(0, 0, 0, 0);
            slideLayout->setSpacing(4);
            auto* image = new adqt::widgets::AdImage(slide);
            image->setObjectName(QStringLiteral("screenshotHistoryImage"));
            image->setLoadingPolicy(adqt::widgets::AdImage::LoadingPolicy::WhenVisible);
            image->setDecodePolicy(adqt::widgets::AdImage::DecodePolicy::FitWidget);
            image->setViewer(m_viewer);
            image->setImageLoader(historyThumbnailLoader());
            image->setPreviewRow(static_cast<int>(index));
            image->setPreferredImageSize(QSize(kPreviewWidth, kPreviewHeight));
            image->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            image->setAltText(previewItems[index].altText);
            image->setSource(previewItems[index].source);
            slideLayout->addWidget(image, 1);
            m_carousel->addSlide(slide);
        }

        if (m_carousel->count() == 0) {
            auto* unavailable = new QLabel(HistoryEntryWidget::tr("Preview unavailable"));
            unavailable->setAlignment(Qt::AlignCenter);
            m_carousel->addSlide(unavailable);
        }
        const bool multiple = m_carousel->count() > 1;
        m_carousel->setArrowsVisible(multiple);
        m_carousel->setDotsVisible(multiple);
        m_carousel->setInfinite(multiple);
        m_layout->addWidget(m_carousel, 0, Qt::AlignCenter);

        m_deleteConfirmation = new adqt::widgets::AdPopconfirm(this);
        m_deleteConfirmation->setObjectName(QStringLiteral("screenshotHistoryEntryDeleteConfirm"));
        m_deleteConfirmation->setSourceWidget(m_deleteButton);
        m_deleteConfirmation->setButtonAccentRole(
            adqt::widgets::AdPopconfirm::StandardButton::Ok,
            adqt::widgets::AdButton::AccentRole::Danger);
        connect(m_editButton, &QAbstractButton::clicked, this, [this]() {
            if (m_editRequested) {
                m_editRequested();
            }
        });
        connect(m_copyButton, &QAbstractButton::clicked, this, [this]() {
            if (m_copyRequested) {
                m_copyButton->setEnabled(false);
                m_copyRequested();
            }
        });
        connect(m_deleteConfirmation, &adqt::widgets::AdPopconfirm::accepted, this, [this]() {
            if (m_deleteRequested) {
                m_deleteRequested();
            }
        });

        retranslateUi();
        updateResponsiveLayout();
    }

    bool matchesRecord(const storage::CaptureHistoryRecord& record) const {
        return m_record == record;
    }

    bool matchesAssets(const std::optional<storage::CaptureHistoryAssetSet>& assets) const {
        return m_assets == assets;
    }

    void setCopyEnabled(bool enabled) {
        if (m_copyButton != nullptr) {
            m_copyButton->setEnabled(enabled);
        }
    }

    void applyTheme(const styles::ThemeColorScheme& scheme) {
        m_scheme = scheme;
        QPalette primary = m_dateLabel->palette();
        primary.setColor(QPalette::WindowText, scheme.map.colorText);
        m_dateLabel->setPalette(primary);
        QFont dateFont = m_dateLabel->font();
        dateFont.setPixelSize(scheme.metricAlias.fontSizeLG);
        dateFont.setWeight(QFont::DemiBold);
        m_dateLabel->setFont(dateFont);

        QPalette secondary = m_summaryLabel->palette();
        secondary.setColor(QPalette::WindowText, scheme.map.colorTextSecondary);
        m_summaryLabel->setPalette(secondary);
        QPalette tertiary = m_metaLabel->palette();
        tertiary.setColor(QPalette::WindowText, scheme.map.colorTextTertiary);
        m_metaLabel->setPalette(tertiary);
        m_sourceLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; background: %2; border: 1px solid %3; "
                           "border-radius: 4px; padding: 2px 7px; }")
                .arg(scheme.map.colorPrimaryText.name(), scheme.map.colorPrimaryBg.name(),
                     scheme.map.colorPrimaryBorder.name()));
        update();
    }

    void retranslateUi() {
        switch (m_record.source) {
        case storage::CaptureHistorySource::CopiedToClipboard:
            m_sourceLabel->setText(HistoryEntryWidget::tr("Copy to Clipboard"));
            break;
        case storage::CaptureHistorySource::PinnedToScreen:
            m_sourceLabel->setText(HistoryEntryWidget::tr("Pin to Screen"));
            break;
        case storage::CaptureHistorySource::CurrentMonitor:
            m_sourceLabel->setText(HistoryEntryWidget::tr("Current Monitor"));
            break;
        case storage::CaptureHistorySource::FocusedWindow:
            m_sourceLabel->setText(HistoryEntryWidget::tr("Focused Window"));
            break;
        }
        m_editButton->setText(HistoryEntryWidget::tr("Edit"));
        m_editButton->setToolTip(HistoryEntryWidget::tr("Edit screenshot history entry"));
        m_editButton->setAccessibleName(HistoryEntryWidget::tr("Edit screenshot history entry"));
        m_copyButton->setText(HistoryEntryWidget::tr("Copy"));
        m_copyButton->setToolTip(HistoryEntryWidget::tr("Copy screenshot result"));
        m_copyButton->setAccessibleName(HistoryEntryWidget::tr("Copy screenshot result"));
        m_deleteButton->setText(HistoryEntryWidget::tr("Delete"));
        m_deleteButton->setToolTip(HistoryEntryWidget::tr("Delete history entry"));
        m_deleteButton->setAccessibleName(HistoryEntryWidget::tr("Delete history entry"));
        m_deleteConfirmation->setText(
            HistoryEntryWidget::tr("Delete this screenshot history entry?"));
        m_deleteConfirmation->setInformativeText(
            HistoryEntryWidget::tr("This action cannot be undone"));
        m_deleteConfirmation->setButtonText(adqt::widgets::AdPopconfirm::StandardButton::Ok,
                                            HistoryEntryWidget::tr("Delete"));
        m_deleteConfirmation->setButtonText(
            adqt::widgets::AdPopconfirm::StandardButton::Cancel,
            HistoryEntryWidget::tr("Cancel"));
    }

  protected:
    void resizeEvent(QResizeEvent* event) override {
        QFrame::resizeEvent(event);
        updateResponsiveLayout();
    }

    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event)
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF bounds = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(QPen(m_scheme.map.colorBorderSecondary, 1));
        painter.setBrush(m_scheme.map.colorBgContainer);
        painter.drawRoundedRect(bounds, m_scheme.metricAlias.borderRadius,
                                m_scheme.metricAlias.borderRadius);
    }

  private:
    void updateResponsiveLayout() {
        const int availableWidth = parentWidget() != nullptr ? parentWidget()->width() : width();
        const bool wide = availableWidth >= kWideEntryBreakpoint;
        m_layout->setDirection(wide ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
        setMinimumHeight(wide ? kPreviewHeight + 32 : kPreviewHeight + 190);
        m_carousel->setFixedWidth(wide ? kPreviewWidth : std::max(220, availableWidth - 36));
    }

    storage::CaptureHistoryRecord m_record;
    std::optional<storage::CaptureHistoryAssetSet> m_assets;
    std::function<void()> m_editRequested;
    std::function<void()> m_copyRequested;
    std::function<void()> m_deleteRequested;
    QBoxLayout* m_layout = nullptr;
    QLabel* m_dateLabel = nullptr;
    QLabel* m_sourceLabel = nullptr;
    QLabel* m_summaryLabel = nullptr;
    QLabel* m_metaLabel = nullptr;
    adqt::widgets::AdButton* m_editButton = nullptr;
    adqt::widgets::AdButton* m_copyButton = nullptr;
    adqt::widgets::AdButton* m_deleteButton = nullptr;
    adqt::widgets::AdPopconfirm* m_deleteConfirmation = nullptr;
    adqt::widgets::AdCarousel* m_carousel = nullptr;
    adqt::widgets::AdImageViewer* m_viewer = nullptr;
    adqt::widgets::AdImageListModel* m_previewModel = nullptr;
    styles::ThemeColorScheme m_scheme;
};
} // namespace

ScreenshotHistoryPageWidget::ScreenshotHistoryPageWidget(QWidget* parent)
    : ScreenshotHistoryPageWidget(nullptr, parent) {}

ScreenshotHistoryPageWidget::ScreenshotHistoryPageWidget(
    ScreenshotHistoryPageDataSource* dataSource, QWidget* parent)
    : QWidget(parent),
      m_dataSource(dataSource != nullptr ? dataSource
                                         : new ApplicationStorageHistoryDataSource(this)),
      m_colorScheme(styles::ThemeManager::instance().themeColorScheme()) {
    setObjectName(QStringLiteral("screenshotHistoryPage"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    const auto metric = m_colorScheme.metricAlias;
    auto* pageContainer = new PageContainerWidget(metric, this);
    pageContainer->setObjectName(QStringLiteral("screenshotHistoryPageContainer"));
    m_scrollArea = pageContainer->scrollArea();
    m_scrollArea->setObjectName(QStringLiteral("screenshotHistoryScrollArea"));
    QWidget* content = pageContainer->contentWidget();
    content->setObjectName(QStringLiteral("screenshotHistoryContent"));
    auto* contentLayout = pageContainer->contentLayout();
    contentLayout->setSpacing(0);
    root->addWidget(pageContainer, 1);

    auto* titleGroup = new QWidget(content);
    auto* titleLayout = new QVBoxLayout(titleGroup);
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(metric.marginXXS);
    m_titleLabel = new QLabel(titleGroup);
    m_titleLabel->setObjectName(QStringLiteral("screenshotHistoryTitle"));
    m_countLabel = new QLabel(titleGroup);
    m_countLabel->setObjectName(QStringLiteral("screenshotHistoryCountLabel"));
    titleLayout->addWidget(m_titleLabel);
    titleLayout->addWidget(m_countLabel);

    auto* actions = new QWidget(content);
    actions->setObjectName(QStringLiteral("screenshotHistoryActions"));
    auto* actionsLayout = new QHBoxLayout(actions);
    actionsLayout->setContentsMargins(0, 0, 0, 0);
    actionsLayout->setSpacing(metric.marginXS);
    m_deleteAllButton =
        new ThemedHeaderIconButton(metric, outlined_icons::IconDelete(), actions);
    m_deleteAllButton->setObjectName(QStringLiteral("screenshotHistoryDeleteAll"));
    m_deleteAllButton->setAccentRole(adqt::widgets::AdButton::AccentRole::Danger);
    actionsLayout->addWidget(m_deleteAllButton, 0);

    m_refreshButton = new ThemedHeaderIconButton(metric, outlined_icons::Reload(), actions);
    m_refreshButton->setObjectName(QStringLiteral("screenshotHistoryRefresh"));
    actionsLayout->addWidget(m_refreshButton, 0);

    auto* headerLayout = new QHBoxLayout;
    headerLayout->setContentsMargins(0, metric.marginMD, 0, 0);
    headerLayout->setSpacing(12);
    headerLayout->addWidget(titleGroup, 1);
    headerLayout->addWidget(actions, 0, Qt::AlignTop);
    contentLayout->addLayout(headerLayout);
    contentLayout->addSpacing(metric.marginSM);

    auto* filtersHost = new QWidget(content);
    filtersHost->setObjectName(QStringLiteral("screenshotHistoryFilters"));
    filtersHost->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* filtersLayout = new QHBoxLayout(filtersHost);
    filtersLayout->setContentsMargins(0, 0, 0, 0);
    filtersLayout->setSpacing(8);
    m_sourceFilter = new adqt::widgets::AdSelect(filtersHost);
    m_sourceFilter->setObjectName(QStringLiteral("screenshotHistorySourceFilter"));
    m_sourceFilter->setMode(adqt::widgets::AdSelect::Mode::Multiple);
    m_sourceFilter->setAllowClear(true);
    m_sourceFilter->setMaxTagCount(1);
    m_sourceFilter->setMinimumWidth(140);
    m_sourceFilter->setMaximumWidth(170);
    filtersLayout->addWidget(m_sourceFilter, 0);

    m_dateRangeFilter = new adqt::widgets::AdDateRangePicker(filtersHost);
    m_dateRangeFilter->setObjectName(QStringLiteral("screenshotHistoryDateRangeFilter"));
    m_dateRangeFilter->setAllowClear(true);
    m_dateRangeFilter->setMinimumWidth(220);
    m_dateRangeFilter->setMaximumWidth(260);
    filtersLayout->addWidget(m_dateRangeFilter, 0);
    filtersLayout->addStretch(1);
    contentLayout->addWidget(filtersHost, 0);
    contentLayout->addSpacing(14);

    m_deleteAllConfirmation = new adqt::widgets::AdPopconfirm(content);
    m_deleteAllConfirmation->setObjectName(QStringLiteral("screenshotHistoryDeleteAllConfirm"));
    m_deleteAllConfirmation->setSourceWidget(m_deleteAllButton);
    m_deleteAllConfirmation->setButtonAccentRole(adqt::widgets::AdPopconfirm::StandardButton::Ok,
                                                 adqt::widgets::AdButton::AccentRole::Danger);

    m_entriesHost = new QWidget(content);
    m_entriesHost->setObjectName(QStringLiteral("screenshotHistoryEntries"));
    m_entriesHost->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_emptyStateMinimumHeight = kEmptyStateBaselineHeight;
    m_entriesLayout = new QVBoxLayout(m_entriesHost);
    m_entriesLayout->setContentsMargins(0, 0, 0, 0);
    m_entriesLayout->setSpacing(10);
    contentLayout->addWidget(m_entriesHost, 1);

    m_emptyIcon = new QLabel(m_entriesHost);
    m_emptyIcon->setObjectName(QStringLiteral("screenshotHistoryEmptyIcon"));
    m_emptyIcon->setAlignment(Qt::AlignCenter);
    m_emptyIcon->setFixedSize(96, 62);
    m_emptyIcon->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_emptyTitle = new QLabel(m_entriesHost);
    m_emptyTitle->setObjectName(QStringLiteral("screenshotHistoryEmptyTitle"));
    m_emptyTitle->setAlignment(Qt::AlignCenter);
    m_emptyDescription = new QLabel(m_entriesHost);
    m_emptyDescription->setObjectName(QStringLiteral("screenshotHistoryEmptyDescription"));
    m_emptyDescription->setAlignment(Qt::AlignCenter);
    m_emptyDescription->setWordWrap(true);

    m_pagination = new adqt::widgets::AdPagination(content);
    m_pagination->setObjectName(QStringLiteral("screenshotHistoryPagination"));
    m_pagination->setPageSize(kDefaultPageSize);
    m_pagination->setPageSizeOptions({10, 20, 50});
    m_pagination->setSizeChangerMode(adqt::widgets::AdPagination::SizeChangerMode::Always);
    m_pagination->setAlignment(adqt::widgets::AdPagination::Alignment::End);
    m_pagination->setResponsive(true);
    m_pagination->setTotalTextFormatter(
        [](int total, const adqt::widgets::AdPagination::Range& range) {
            return ScreenshotHistoryPageWidget::tr("%1-%2 of %3")
                .arg(range.first)
                .arg(range.last)
                .arg(total);
        });
    contentLayout->addSpacing(14);
    contentLayout->addWidget(m_pagination, 0);

    connect(m_sourceFilter, &adqt::widgets::AdSelect::currentValuesChanged, this,
            [this](const QVariantList&) { rebuildFilteredRecords(true); });
    connect(m_dateRangeFilter, &adqt::widgets::AdDateRangePicker::rangeChanged, this,
            [this](const QDate&, const QDate&) { rebuildFilteredRecords(true); });
    connect(m_refreshButton, &QAbstractButton::clicked, this,
            &ScreenshotHistoryPageWidget::refresh);
    connect(m_deleteAllConfirmation, &adqt::widgets::AdPopconfirm::accepted, this,
            &ScreenshotHistoryPageWidget::requestDeleteAll);
    connect(m_pagination, &adqt::widgets::AdPagination::currentPageChanged, this, [this](int) {
        if (!m_updatingPagination) {
            rebuildEntries();
        }
    });
    connect(m_pagination, &adqt::widgets::AdPagination::pageSizeChanged, this, [this](int) {
        if (!m_updatingPagination) {
            rebuildEntries();
        }
    });

    connect(m_dataSource, &ScreenshotHistoryPageDataSource::historyChanged, this,
            &ScreenshotHistoryPageWidget::handleHistoryChanged);
    connect(m_dataSource, &ScreenshotHistoryPageDataSource::displayAssetsReady, this,
            &ScreenshotHistoryPageWidget::handleDisplayAssetsReady);
    connect(m_dataSource, &ScreenshotHistoryPageDataSource::resultImageReady, this,
            &ScreenshotHistoryPageWidget::handleResultImageReady);
    connect(m_dataSource, &ScreenshotHistoryPageDataSource::clearFinished, this,
            [this](bool, const QString&) {
                m_deleteAllButton->setEnabled(true);
                handleHistoryChanged();
            });

    const auto& themeManager = styles::ThemeManager::instance();
    connect(&themeManager, &styles::ThemeManager::themeChanged, this,
            &ScreenshotHistoryPageWidget::applyTheme);
    retranslateUi();
    applyTheme(m_colorScheme);
}

int ScreenshotHistoryPageWidget::totalCount() const {
    return m_records.size();
}

int ScreenshotHistoryPageWidget::filteredCount() const {
    return m_filteredRecords.size();
}

int ScreenshotHistoryPageWidget::currentPage() const {
    return m_pagination != nullptr ? m_pagination->currentPage() : 1;
}

void ScreenshotHistoryPageWidget::refresh() {
    m_refreshQueued = false;
    m_dirty = false;
    ++m_assetGeneration;
    ++m_resultGeneration;
    m_pendingResultRecordIds.clear();
    m_resolvedAssets.clear();
    m_records = m_dataSource != nullptr ? m_dataSource->records()
                                        : QVector<storage::CaptureHistoryRecord>{};
    rebuildFilteredRecords(false);
}

void ScreenshotHistoryPageWidget::setActive(bool active) {
    if (m_active == active) {
        if (m_active && m_dirty) {
            refresh();
        }
        return;
    }
    m_active = active;
    if (m_active && m_dirty) {
        refresh();
    }
}

bool ScreenshotHistoryPageWidget::isActive() const {
    return m_active;
}

void ScreenshotHistoryPageWidget::handleHistoryChanged() {
    m_dirty = true;
    if (m_active) {
        queueRefresh();
    }
}

void ScreenshotHistoryPageWidget::queueRefresh() {
    if (m_refreshQueued) {
        return;
    }
    m_refreshQueued = true;
    QTimer::singleShot(0, this, [this]() {
        m_refreshQueued = false;
        if (m_active && m_dirty) {
            refresh();
        }
    });
}

bool ScreenshotHistoryPageWidget::matchesFilters(
    const storage::CaptureHistoryRecord& record) const {
    const QVariantList selectedSources = m_sourceFilter->currentValues();
    if (!selectedSources.isEmpty()) {
        bool matched = false;
        const QString key = sourceKey(record.source);
        for (const QVariant& selected : selectedSources) {
            if (selected.toString() == key) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            return false;
        }
    }

    const QDate recordDate = record.createdUtc.toLocalTime().date();
    const QDate startDate = m_dateRangeFilter->startDate();
    const QDate endDate = m_dateRangeFilter->endDate();
    return (!startDate.isValid() || recordDate >= startDate) &&
           (!endDate.isValid() || recordDate <= endDate);
}

void ScreenshotHistoryPageWidget::rebuildFilteredRecords(bool resetPage) {
    m_filteredRecords.clear();
    for (const storage::CaptureHistoryRecord& record : m_records) {
        if (matchesFilters(record)) {
            m_filteredRecords.push_back(record);
        }
    }
    m_updatingPagination = true;
    if (resetPage) {
        m_pagination->setCurrentPage(1);
    }
    m_pagination->setTotal(m_filteredRecords.size());
    m_updatingPagination = false;
    updateHeader();
    rebuildEntries();
}

void ScreenshotHistoryPageWidget::rebuildEntries() {
    m_emptyIcon->hide();
    m_emptyTitle->hide();
    m_emptyDescription->hide();

    const int pageSize = m_pagination->pageSize();
    const int firstIndex = std::max(0, (m_pagination->currentPage() - 1) * pageSize);
    const int lastIndex =
        std::min(firstIndex + pageSize, static_cast<int>(m_filteredRecords.size()));

    auto requestUnresolvedAssets = [this, firstIndex, lastIndex]() {
        if (m_dataSource == nullptr || !m_dataSource->supportsAsyncDisplayAssets() ||
            firstIndex >= lastIndex) {
            return;
        }
        QVector<storage::CaptureHistoryRecord> unresolved;
        for (int index = firstIndex; index < lastIndex; ++index) {
            const storage::CaptureHistoryRecord& record = m_filteredRecords[index];
            if (!m_resolvedAssets.contains(record.id)) {
                unresolved.push_back(record);
            }
        }
        if (!unresolved.isEmpty()) {
            m_dataSource->requestDisplayAssets(unresolved, m_assetGeneration);
        }
    };

    QVector<QString> desiredIds;
    desiredIds.reserve(std::max(0, lastIndex - firstIndex));
    for (int index = firstIndex; index < lastIndex; ++index) {
        desiredIds.push_back(m_filteredRecords[index].id);
    }

    bool layoutNeedsRebuild = desiredIds != m_entryLayoutIds;
    for (int index = firstIndex; index < lastIndex; ++index) {
        const storage::CaptureHistoryRecord& record = m_filteredRecords[index];
        auto* entry = dynamic_cast<HistoryEntryWidget*>(
            m_entryWidgetsById.value(record.id, nullptr));
        if (entry == nullptr || !entry->matchesRecord(record)) {
            layoutNeedsRebuild = true;
        } else if (m_resolvedAssets.contains(record.id) &&
                   !entry->matchesAssets(m_resolvedAssets.value(record.id))) {
            layoutNeedsRebuild = true;
        }
    }

    for (auto iterator = m_entryWidgetsById.begin(); iterator != m_entryWidgetsById.end();) {
        if (desiredIds.contains(iterator.key())) {
            ++iterator;
            continue;
        }
        delete iterator.value();
        iterator = m_entryWidgetsById.erase(iterator);
    }

    if (firstIndex >= lastIndex) {
        if (!layoutNeedsRebuild && m_entriesLayout->count() > 0) {
            requestUnresolvedAssets();
            return;
        }
        clearLayoutItems(m_entriesLayout);
        m_entryLayoutIds.clear();
        m_entriesLayout->addSpacing(48);
        m_entriesLayout->addStretch(1);
        m_emptyIcon->show();
        m_entriesLayout->addWidget(m_emptyIcon, 0, Qt::AlignHCenter);
        m_entriesLayout->addSpacing(18);
        m_emptyTitle->show();
        m_emptyDescription->show();
        m_entriesLayout->addWidget(m_emptyTitle);
        m_entriesLayout->addSpacing(8);
        m_entriesLayout->addWidget(m_emptyDescription);
        m_entriesLayout->addStretch(1);
        m_entriesLayout->addSpacing(48);
        retranslateUi();
        applyTheme(m_colorScheme);
        return;
    }

    if (!layoutNeedsRebuild) {
        const int responsiveEntryMinimumHeight =
            m_entriesHost->width() >= kWideEntryBreakpoint ? kPreviewHeight + 32
                                                            : kPreviewHeight + 190;
        m_entriesHost->setMinimumHeight(
            std::max(m_emptyStateMinimumHeight, responsiveEntryMinimumHeight));
        requestUnresolvedAssets();
        return;
    }

    clearLayoutItems(m_entriesLayout);
    const int responsiveEntryMinimumHeight =
        m_entriesHost->width() >= kWideEntryBreakpoint ? kPreviewHeight + 32
                                                        : kPreviewHeight + 190;
    m_entriesHost->setMinimumHeight(
        std::max(m_emptyStateMinimumHeight, responsiveEntryMinimumHeight));
    for (int index = firstIndex; index < lastIndex; ++index) {
        const storage::CaptureHistoryRecord& record = m_filteredRecords[index];
        auto* entry =
            dynamic_cast<HistoryEntryWidget*>(m_entryWidgetsById.value(record.id, nullptr));
        std::optional<storage::CaptureHistoryAssetSet> assets;
        bool assetsResolved = false;
        if (m_dataSource != nullptr && m_dataSource->supportsAsyncDisplayAssets()) {
            assetsResolved = m_resolvedAssets.contains(record.id);
            if (assetsResolved) {
                assets = m_resolvedAssets.value(record.id);
            }
        } else if (m_dataSource != nullptr) {
            assets = m_dataSource->displayAssets(record);
            assetsResolved = true;
            m_resolvedAssets.insert(record.id, assets);
        }
        if (entry == nullptr || !entry->matchesRecord(record) ||
            (assetsResolved && !entry->matchesAssets(assets))) {
            delete entry;
            entry = new HistoryEntryWidget(
                record, assets, [this, id = record.id]() { emit editRequested(id); },
                [this, record]() { copyEntry(record); },
                [this, id = record.id]() { removeEntry(id); }, m_entriesHost);
            entry->applyTheme(m_colorScheme);
            m_entryWidgetsById.insert(record.id, entry);
        }
        m_entriesLayout->addWidget(entry);
    }
    m_entriesLayout->addStretch(1);
    m_entryLayoutIds = desiredIds;
    requestUnresolvedAssets();
    QTimer::singleShot(0, this, [this]() {
        if (m_scrollArea != nullptr && m_scrollArea->verticalScrollBar() != nullptr) {
            m_scrollArea->verticalScrollBar()->setValue(0);
        }
    });
}

void ScreenshotHistoryPageWidget::handleDisplayAssetsReady(
    quint64 generation, const QVector<ScreenshotHistoryAssetResolution>& resolutions) {
    if (generation != m_assetGeneration) {
        return;
    }
    for (const ScreenshotHistoryAssetResolution& resolution : resolutions) {
        m_resolvedAssets.insert(resolution.recordId, resolution.assets);
    }
    rebuildEntries();
}

void ScreenshotHistoryPageWidget::copyEntry(const storage::CaptureHistoryRecord& record) {
    if (m_dataSource == nullptr || !record.result.has_value() ||
        m_pendingResultRecordIds.contains(record.id)) {
        return;
    }
    auto* entry = dynamic_cast<HistoryEntryWidget*>(m_entryWidgetsById.value(record.id, nullptr));
    if (entry == nullptr) {
        return;
    }
    m_pendingResultRecordIds.insert(record.id);
    const quint64 generation = m_resultGeneration;
    m_dataSource->requestResultImage(record, generation);
}

void ScreenshotHistoryPageWidget::handleResultImageReady(
    quint64 generation, const ScreenshotHistoryResultResolution& resolution) {
    if (generation != m_resultGeneration ||
        !m_pendingResultRecordIds.contains(resolution.recordId)) {
        return;
    }
    const QString recordId = resolution.recordId;
    m_pendingResultRecordIds.remove(recordId);
    auto* entry = dynamic_cast<HistoryEntryWidget*>(m_entryWidgetsById.value(recordId, nullptr));
    if (entry != nullptr) {
        entry->setCopyEnabled(true);
    }
    if (resolution.image.has_value() && !resolution.image->isNull()) {
        static_cast<void>(ScreenshotClipboardService::publishImage(
            QApplication::clipboard(), *resolution.image));
    }
}

void ScreenshotHistoryPageWidget::updateEmptyStateMinimumHeight() {
    if (m_emptyIcon == nullptr || !m_emptyIcon->isVisible() || m_entriesLayout == nullptr ||
        m_entriesHost == nullptr) {
        return;
    }

    // Keep the empty-state footprint as the baseline so a first record does not
    // synchronously reflow the page and move the controls below it.
    if (m_entriesLayout->count() == 0) {
        return;
    }
    m_entriesLayout->activate();
    const int responsiveEntryMinimumHeight =
        m_entriesHost->width() >= kWideEntryBreakpoint ? kPreviewHeight + 32
                                                        : kPreviewHeight + 190;
    m_emptyStateMinimumHeight = std::max(
        {kEmptyStateBaselineHeight, m_entriesLayout->sizeHint().height(),
         responsiveEntryMinimumHeight});
    m_entriesHost->setMinimumHeight(m_emptyStateMinimumHeight);
}

void ScreenshotHistoryPageWidget::updateHeader() {
    if (m_countLabel != nullptr) {
        m_countLabel->setText(tr("%n screenshot(s)", nullptr, m_records.size()));
    }
    const bool hasEntries = !m_records.isEmpty();
    m_deleteAllButton->setEnabled(hasEntries);
    m_deleteAllConfirmation->setEnabled(hasEntries);
}

void ScreenshotHistoryPageWidget::requestDeleteAll() {
    m_deleteAllButton->setEnabled(false);
    if (m_dataSource == nullptr || !m_dataSource->requestClear()) {
        m_deleteAllButton->setEnabled(!m_records.isEmpty());
    }
}

void ScreenshotHistoryPageWidget::removeEntry(const QString& entryId) {
    if (m_dataSource != nullptr) {
        m_dataSource->remove(entryId);
    }
}

void ScreenshotHistoryPageWidget::retranslateUi() {
    m_titleLabel->setText(tr("Screenshot History"));
    m_sourceFilter->setPlaceholder(tr("All sources"));
    const QVariantList selectedSources = m_sourceFilter->currentValues();
    m_sourceFilter->setOptions(
        {sourceOption(QStringLiteral("clipboard"), tr("Copy to Clipboard")),
         sourceOption(QStringLiteral("pinned"), tr("Pin to Screen")),
         sourceOption(QStringLiteral("current-monitor"), tr("Current Monitor")),
         sourceOption(QStringLiteral("focused-window"), tr("Focused Window"))});
    m_sourceFilter->setCurrentValues(selectedSources);
    m_dateRangeFilter->setRangePlaceholders(tr("Start date"), tr("End date"));
    m_deleteAllButton->setToolTip(tr("Delete All History"));
    m_deleteAllButton->setAccessibleName(tr("Delete All History"));
    m_refreshButton->setToolTip(tr("Refresh history"));
    m_refreshButton->setAccessibleName(tr("Refresh history"));
    m_deleteAllConfirmation->setText(tr("Delete all screenshot history?"));
    m_deleteAllConfirmation->setInformativeText(
        tr("This permanently removes every saved screenshot history entry"));
    m_deleteAllConfirmation->setButtonText(adqt::widgets::AdPopconfirm::StandardButton::Ok,
                                           tr("Delete All"));
    m_deleteAllConfirmation->setButtonText(adqt::widgets::AdPopconfirm::StandardButton::Cancel,
                                           tr("Cancel"));
    if (m_emptyTitle != nullptr) {
        m_emptyTitle->setText(m_records.isEmpty() ? tr("No screenshot history")
                                                  : tr("No matching screenshots"));
    }
    if (m_emptyDescription != nullptr) {
        m_emptyDescription->setText(
            m_records.isEmpty() ? tr("Copied and pinned screenshots will appear here")
                                : tr("Change the source or date range to see more history"));
    }
    updateEmptyStateMinimumHeight();
    updateHeader();
}

void ScreenshotHistoryPageWidget::applyTheme(const styles::ThemeColorScheme& scheme) {
    m_colorScheme = scheme;
    QPalette pagePalette = palette();
    pagePalette.setColor(QPalette::Window, Qt::transparent);
    setPalette(pagePalette);
    setAutoFillBackground(false);

    QPalette titlePalette = m_titleLabel->palette();
    titlePalette.setColor(QPalette::WindowText, scheme.map.colorText);
    m_titleLabel->setPalette(titlePalette);
    QFont titleFont = m_titleLabel->font();
    titleFont.setPixelSize(scheme.metricAlias.fontSizeHeading4);
    titleFont.setWeight(QFont::DemiBold);
    m_titleLabel->setFont(titleFont);

    QPalette mutedPalette = m_countLabel->palette();
    mutedPalette.setColor(QPalette::WindowText, scheme.map.colorTextSecondary);
    m_countLabel->setPalette(mutedPalette);
    QFont countFont = m_countLabel->font();
    countFont.setPixelSize(scheme.metricAlias.fontSize);
    countFont.setWeight(QFont::Normal);
    m_countLabel->setFont(countFont);
    if (m_emptyTitle != nullptr) {
        m_emptyTitle->setPalette(titlePalette);
        QFont emptyFont = m_emptyTitle->font();
        emptyFont.setPixelSize(scheme.metricAlias.fontSizeLG);
        emptyFont.setWeight(QFont::DemiBold);
        m_emptyTitle->setFont(emptyFont);
    }
    if (m_emptyDescription != nullptr) {
        m_emptyDescription->setPalette(mutedPalette);
    }
    if (m_emptyIcon != nullptr) {
        const QColor background = scheme.map.colorBgContainer;
        const QPixmap icon = adqt::icons::renderIconPixmap(
            adqt::widgets::icons::twotone::EmptySimple(
                adqt::icons::IconColors::threeTone(
                    colorOnBackground(scheme.map.colorFill, background),
                    colorOnBackground(scheme.map.colorFillQuaternary, background),
                    colorOnBackground(scheme.map.colorFillTertiary, background))),
            {m_emptyIcon->size(), m_emptyIcon->devicePixelRatioF()});
        m_emptyIcon->setPixmap(icon);
    }
    for (QWidget* child :
         m_entriesHost->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (auto* entry = dynamic_cast<HistoryEntryWidget*>(child); entry != nullptr) {
            entry->applyTheme(scheme);
        }
    }
    updateEmptyStateMinimumHeight();
    update();
}

void ScreenshotHistoryPageWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void ScreenshotHistoryPageWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateEmptyStateMinimumHeight();
}

void ScreenshotHistoryPageWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    setActive(true);
}

void ScreenshotHistoryPageWidget::hideEvent(QHideEvent* event) {
    setActive(false);
    QWidget::hideEvent(event);
}
