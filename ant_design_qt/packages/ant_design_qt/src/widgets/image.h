#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QImage>
#include <QList>
#include <QMetaObject>
#include <QMetaType>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <QVector>
#include <QWidget>

#include <functional>
#include <optional>

class QAbstractItemModel;
class QEnterEvent;
class QEvent;
class QFocusEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QHideEvent;

namespace adqt::widgets {

struct AdImageLoadOptions {
  QSize targetPixelSize;
  Qt::AspectRatioMode aspectRatioMode = Qt::KeepAspectRatio;
  bool allowUpscale = false;
};

bool operator==(const AdImageLoadOptions& lhs, const AdImageLoadOptions& rhs);
bool operator!=(const AdImageLoadOptions& lhs, const AdImageLoadOptions& rhs);

class AdImageReply : public QObject {
  Q_OBJECT

 public:
  explicit AdImageReply(QObject* parent = nullptr);
  ~AdImageReply() override;

  bool isFinished() const;
  bool isSuccessful() const;
  QImage image() const;
  QSize naturalSize() const;
  QString errorString() const;

 public slots:
  virtual void abort() = 0;

 signals:
  void finished();

 protected:
  void succeed(const QImage& image, const QSize& naturalSize = QSize());
  void fail(const QString& errorString = QString());

 private:
  bool finished_ = false;
  bool successful_ = false;
  QImage image_;
  QSize naturalSize_;
  QString errorString_;
};

class AdImageLoader : public QObject {
  Q_OBJECT

 public:
  explicit AdImageLoader(QObject* parent = nullptr);
  ~AdImageLoader() override;

  virtual AdImageReply* load(const QUrl& source, QObject* parent = nullptr) = 0;
  virtual AdImageReply* load(const QUrl& source, const AdImageLoadOptions& options,
                             QObject* parent = nullptr);
};

AdImageLoader* defaultAdImageLoader();

struct AdImageItemRoles {
  Q_GADGET

 public:
  enum Role {
    SourceRole = Qt::UserRole + 1,
    AltTextRole,
  };
  Q_ENUM(Role)
};

struct AdImageItem {
  Q_GADGET

  Q_PROPERTY(QUrl source MEMBER source)
  Q_PROPERTY(QString altText MEMBER altText)

 public:
  QUrl source;
  QString altText;

  bool isValid() const;
};

bool operator==(const AdImageItem& lhs, const AdImageItem& rhs);
bool operator!=(const AdImageItem& lhs, const AdImageItem& rhs);

using AdImageItems = QList<AdImageItem>;

class AdImageListModel final : public QAbstractListModel {
  Q_OBJECT

  Q_PROPERTY(adqt::widgets::AdImageItems items READ items WRITE setItems NOTIFY itemsChanged)

 public:
  explicit AdImageListModel(QObject* parent = nullptr);
  ~AdImageListModel() override;

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QHash<int, QByteArray> roleNames() const override;

  AdImageItems items() const;
  void setItems(const AdImageItems& value);
  AdImageItem itemAt(int row) const;

 signals:
  void itemsChanged(const adqt::widgets::AdImageItems& value);

 private:
  AdImageItems items_;
};

class AdImageViewer final : public QObject {
  Q_OBJECT

  Q_PROPERTY(bool visible READ isVisible WRITE setVisible NOTIFY visibleChanged)
  Q_PROPERTY(int currentRow READ currentRow WRITE setCurrentRow NOTIFY currentRowChanged)
  Q_PROPERTY(QString countTextFormat READ countTextFormat WRITE setCountTextFormat NOTIFY
                 countTextFormatChanged)
  Q_PROPERTY(bool maskVisible READ maskVisible WRITE setMaskVisible NOTIFY maskVisibleChanged)
  Q_PROPERTY(double scaleStep READ scaleStep WRITE setScaleStep NOTIFY scaleStepChanged)
  Q_PROPERTY(QWidget* ownerWindow READ ownerWindow WRITE setOwnerWindow NOTIFY ownerWindowChanged)
  Q_PROPERTY(QAbstractItemModel* model READ model WRITE setModel NOTIFY modelChanged)
  Q_PROPERTY(adqt::widgets::AdImageLoader* imageLoader READ imageLoader WRITE setImageLoader NOTIFY
                 imageLoaderChanged)

 public:
  struct ComponentTokens {
    std::optional<int> zIndexPopup;
    std::optional<int> previewOperationSize;
    std::optional<QColor> previewOperationColor;
    std::optional<QColor> previewOperationHoverColor;
    std::optional<QColor> previewOperationColorDisabled;
  };

  struct SemanticSlotStyle {
    std::optional<QColor> textColor;
    std::optional<QColor> backgroundColor;
    std::optional<QColor> borderColor;
  };

  struct SemanticStyles {
    SemanticSlotStyle popupRoot;
    SemanticSlotStyle popupMask;
    SemanticSlotStyle popupBody;
    SemanticSlotStyle popupFooter;
    SemanticSlotStyle popupActions;
  };

  struct StyleContext {
    bool visible = false;
    bool maskVisible = true;
    int currentRow = -1;
    int totalCount = 0;
  };

  using SemanticStyleResolver = std::function<SemanticStyles(const StyleContext&)>;

  explicit AdImageViewer(QObject* parent = nullptr);
  ~AdImageViewer() override;

  bool isVisible() const;
  void setVisible(bool value);

  int currentRow() const;
  void setCurrentRow(int value);

  QString countTextFormat() const;
  void setCountTextFormat(const QString& value);

  bool maskVisible() const;
  void setMaskVisible(bool value);

  double scaleStep() const;
  void setScaleStep(double value);

  QWidget* ownerWindow() const;
  void setOwnerWindow(QWidget* value);

  QAbstractItemModel* model() const;
  void setModel(QAbstractItemModel* value);

  AdImageLoader* imageLoader() const;
  void setImageLoader(AdImageLoader* value);

  int rowCount() const;
  AdImageItem itemAt(int row) const;
  bool canOpenRow(int row) const;

  ComponentTokens componentTokens() const;
  void setComponentTokens(const ComponentTokens& tokens);
  void resetComponentTokens();

  SemanticStyles semanticStyles() const;
  void setSemanticStyles(const SemanticStyles& styles);
  void setSemanticStyleResolver(SemanticStyleResolver resolver);

 public slots:
  void openAt(int row);
  void close();
  void activate(int delta);
  void zoomIn();
  void zoomOut();
  void rotateLeft();
  void rotateRight();
  void flipX();
  void flipY();
  void resetTransform();

 signals:
  void visibleChanged(bool value);
  void currentRowChanged(int value);
  void countTextFormatChanged(const QString& value);
  void maskVisibleChanged(bool value);
  void scaleStepChanged(double value);
  void ownerWindowChanged(QWidget* value);
  void modelChanged(QAbstractItemModel* value);
  void imageLoaderChanged(adqt::widgets::AdImageLoader* value);
  void componentTokensChanged();
  void semanticStylesChanged();
  void currentItemChanged(int currentRow, int totalCount, const adqt::widgets::AdImageItem& item,
                          const QSize& actualSize);

 private:
  friend class AdImage;

  void openAtFromWidget(int row, QWidget* widget);
  void handlePreviewVisibilityChanged(bool visible);
  void handlePreviewCurrentRowChanged(int row);
  void handlePreviewCurrentItemChanged(int currentRow, int totalCount,
                                       const adqt::widgets::AdImageItem& item,
                                       const QSize& actualSize);
  void handleModelReset();
  void ensurePreviewSurface();
  void applyPreviewSurfaceVisuals();
  void syncPreviewSurfaceEntries();
  QWidget* resolvedOwnerWindow() const;
  QWidget* themeSourceWidget() const;
  void disconnectModelSignals();

  QPointer<QWidget> ownerWindow_;
  QPointer<QWidget> contextWidget_;
  QPointer<QAbstractItemModel> model_;
  QPointer<AdImageLoader> imageLoader_;
  bool visible_ = false;
  int currentRow_ = -1;
  QString countTextFormat_ = QStringLiteral("%1 / %2");
  bool maskVisible_ = true;
  double scaleStep_ = 0.5;
  ComponentTokens componentTokens_;
  SemanticStyles semanticStyles_;
  SemanticStyleResolver semanticStyleResolver_;
  QPointer<QWidget> previewSurface_;
  QVector<QMetaObject::Connection> modelConnections_;
};

class AdImage final : public QWidget {
  Q_OBJECT

 public:
  enum class LoadingPolicy {
    Immediate,
    WhenVisible,
  };
  Q_ENUM(LoadingPolicy)

  enum class DecodePolicy {
    OriginalSize,
    FitWidget,
  };
  Q_ENUM(DecodePolicy)

 private:
  Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
  Q_PROPERTY(QString altText READ altText WRITE setAltText NOTIFY altTextChanged)
  Q_PROPERTY(
      QUrl fallbackSource READ fallbackSource WRITE setFallbackSource NOTIFY fallbackSourceChanged)
  Q_PROPERTY(QUrl placeholderSource READ placeholderSource WRITE setPlaceholderSource NOTIFY
                 placeholderSourceChanged)
  Q_PROPERTY(
      QUrl previewSource READ previewSource WRITE setPreviewSource NOTIFY previewSourceChanged)
  Q_PROPERTY(adqt::widgets::AdImageItems previewItems READ previewItems WRITE setPreviewItems NOTIFY
                 previewItemsChanged)
  Q_PROPERTY(
      bool previewEnabled READ previewEnabled WRITE setPreviewEnabled NOTIFY previewEnabledChanged)
  Q_PROPERTY(QString previewText READ previewText WRITE setPreviewText NOTIFY previewTextChanged)
  Q_PROPERTY(QSize preferredImageSize READ preferredImageSize WRITE setPreferredImageSize NOTIFY
                 preferredImageSizeChanged)
  Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
  Q_PROPERTY(bool loadFailed READ loadFailed NOTIFY loadFailedChanged)
  Q_PROPERTY(int previewRow READ previewRow WRITE setPreviewRow NOTIFY previewRowChanged)
  Q_PROPERTY(adqt::widgets::AdImageViewer* viewer READ viewer WRITE setViewer NOTIFY viewerChanged)
  Q_PROPERTY(adqt::widgets::AdImageLoader* imageLoader READ imageLoader WRITE setImageLoader NOTIFY
                 imageLoaderChanged)
  Q_PROPERTY(adqt::widgets::AdImage::LoadingPolicy loadingPolicy READ loadingPolicy WRITE
                 setLoadingPolicy NOTIFY loadingPolicyChanged)
  Q_PROPERTY(adqt::widgets::AdImage::DecodePolicy decodePolicy READ decodePolicy WRITE
                 setDecodePolicy NOTIFY decodePolicyChanged)

 public:
  struct ComponentTokens {
    std::optional<int> borderRadius;
    std::optional<QColor> placeholderBg;
    std::optional<QColor> placeholderIconColor;
    std::optional<QColor> coverBg;
    std::optional<QColor> coverColor;
  };

  struct SemanticSlotStyle {
    std::optional<QColor> textColor;
    std::optional<QColor> backgroundColor;
    std::optional<QColor> borderColor;
  };

  struct SemanticStyles {
    SemanticSlotStyle root;
    SemanticSlotStyle image;
    SemanticSlotStyle cover;
  };

  struct StyleContext {
    bool hovered = false;
    bool loading = false;
    bool failed = false;
    bool previewEnabled = true;
    bool previewable = false;
    bool previewVisible = false;
    bool focused = false;
  };

  using SemanticStyleResolver = std::function<SemanticStyles(const StyleContext&)>;

  explicit AdImage(QWidget* parent = nullptr);
  ~AdImage() override;

  QUrl source() const;
  void setSource(const QUrl& value);

  QString altText() const;
  void setAltText(const QString& value);

  QUrl fallbackSource() const;
  void setFallbackSource(const QUrl& value);

  QUrl placeholderSource() const;
  void setPlaceholderSource(const QUrl& value);

  QUrl previewSource() const;
  void setPreviewSource(const QUrl& value);

  AdImageItems previewItems() const;
  void setPreviewItems(const AdImageItems& value);

  bool previewEnabled() const;
  void setPreviewEnabled(bool value);

  QString previewText() const;
  void setPreviewText(const QString& value);

  QSize preferredImageSize() const;
  void setPreferredImageSize(const QSize& value);

  bool loading() const;
  bool loadFailed() const;

  int previewRow() const;
  void setPreviewRow(int value);

  AdImageViewer* viewer() const;
  void setViewer(AdImageViewer* value);

  AdImageViewer* ensureViewer();

  AdImageLoader* imageLoader() const;
  void setImageLoader(AdImageLoader* value);

  LoadingPolicy loadingPolicy() const;
  void setLoadingPolicy(LoadingPolicy value);

  DecodePolicy decodePolicy() const;
  void setDecodePolicy(DecodePolicy value);

  ComponentTokens componentTokens() const;
  void setComponentTokens(const ComponentTokens& tokens);
  void resetComponentTokens();

  SemanticStyles semanticStyles() const;
  void setSemanticStyles(const SemanticStyles& styles);
  void setSemanticStyleResolver(SemanticStyleResolver resolver);

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

 signals:
  void sourceChanged(const QUrl& value);
  void altTextChanged(const QString& value);
  void fallbackSourceChanged(const QUrl& value);
  void placeholderSourceChanged(const QUrl& value);
  void previewSourceChanged(const QUrl& value);
  void previewItemsChanged(const adqt::widgets::AdImageItems& value);
  void previewEnabledChanged(bool value);
  void previewTextChanged(const QString& value);
  void preferredImageSizeChanged(const QSize& value);
  void loadingChanged(bool value);
  void loadFailedChanged(bool value);
  void previewRowChanged(int value);
  void viewerChanged(adqt::widgets::AdImageViewer* value);
  void imageLoaderChanged(adqt::widgets::AdImageLoader* value);
  void loadingPolicyChanged(adqt::widgets::AdImage::LoadingPolicy value);
  void decodePolicyChanged(adqt::widgets::AdImage::DecodePolicy value);
  void componentTokensChanged();
  void semanticStylesChanged();

 protected:
  void paintEvent(QPaintEvent* event) override;
  void changeEvent(QEvent* event) override;
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void hideEvent(QHideEvent* event) override;
  void focusInEvent(QFocusEvent* event) override;
  void focusOutEvent(QFocusEvent* event) override;

 private:
  AdImageItem effectivePreviewItem() const;
  AdImageItems effectiveInternalViewerItems() const;
  bool canOpenPreview() const;
  bool previewVisibleForThisImage() const;
  int effectivePreviewRow() const;
  AdImageViewer* effectiveViewer() const;
  void ensureInternalViewerModel();
  void syncInternalViewerData();
  void syncInteractiveState();
  void reloadMainImage();
  void requestMainImageIfNeeded();
  AdImageLoadOptions mainLoadOptions() const;
  bool mainImageNeedsLargerDecode() const;
  void reloadPlaceholderImage();
  void cancelMainLoads();
  void setLoadingState(bool value);
  void setLoadFailedState(bool value);
  void activatePreviewFromUser();
  void bindEffectiveViewerSignals();
  void disconnectViewerSignals();
  void updateAccessibleText();

  QUrl source_;
  QString altText_;
  QUrl fallbackSource_;
  QUrl placeholderSource_;
  QUrl previewSource_;
  AdImageItems previewItems_;
  bool previewEnabled_ = true;
  // Empty means the locale-aware built-in label; non-empty values are application-owned text.
  QString previewText_;
  QSize preferredImageSize_;
  bool loading_ = false;
  bool loadFailed_ = false;
  bool hovered_ = false;
  int previewRow_ = -1;
  int mainLoadToken_ = 0;
  int placeholderLoadToken_ = 0;
  QPixmap imagePixmap_;
  QPixmap placeholderPixmap_;
  QSize requestedMainPixelSize_;
  QSize decodedMainPixelSize_;
  QSize naturalMainPixelSize_;
  bool mainLoadPending_ = false;
  QPointer<AdImageReply> mainReply_;
  QPointer<AdImageReply> fallbackReply_;
  QPointer<AdImageReply> placeholderReply_;
  QPointer<AdImageViewer> viewer_;
  QPointer<AdImageViewer> internalViewer_;
  QPointer<AdImageListModel> internalViewerModel_;
  QPointer<AdImageLoader> imageLoader_;
  LoadingPolicy loadingPolicy_ = LoadingPolicy::Immediate;
  DecodePolicy decodePolicy_ = DecodePolicy::OriginalSize;
  ComponentTokens componentTokens_;
  SemanticStyles semanticStyles_;
  SemanticStyleResolver semanticStyleResolver_;
  QVector<QMetaObject::Connection> viewerConnections_;
};

}  // namespace adqt::widgets

Q_DECLARE_METATYPE(adqt::widgets::AdImageItem)
Q_DECLARE_METATYPE(adqt::widgets::AdImageItems)
Q_DECLARE_METATYPE(adqt::widgets::AdImageViewer*)
Q_DECLARE_METATYPE(adqt::widgets::AdImageLoader*)
Q_DECLARE_TYPEINFO(adqt::widgets::AdImageItem, Q_RELOCATABLE_TYPE);
