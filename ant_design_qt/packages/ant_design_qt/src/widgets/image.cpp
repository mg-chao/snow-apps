#include "image.h"

#include "image_style.h"
#include "antd_icons.h"
#include "theme/theme.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QBuffer>
#include <QCache>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDir>
#include <QEnterEvent>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QImage>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStackedLayout>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QTransform>
#include <QVariant>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <memory>
#include <limits>
#include <utility>

namespace adqt::widgets {

namespace {

namespace outlined_icons = adqt::icons::antd::outlined;

QString sourceToString(const QUrl& source) { return source.toString(QUrl::FullyEncoded); }

QString normalizeSourceKey(const QUrl& source) {
  if (source.isEmpty()) {
    return QString();
  }

  if (source.isLocalFile()) {
    return QUrl::fromLocalFile(source.toLocalFile()).toString(QUrl::FullyEncoded);
  }

  return source.toString(QUrl::FullyEncoded);
}

bool isRemoteSource(const QUrl& source) {
  if (!source.isValid()) {
    return false;
  }

  const QString scheme = source.scheme().toLower();
  return scheme == QStringLiteral("http") || scheme == QStringLiteral("https");
}

bool decodeDataUrl(const QUrl& source, QByteArray* bytes) {
  if (!bytes) {
    return false;
  }

  const QString encoded = source.toString(QUrl::FullyEncoded);
  if (!encoded.startsWith(QStringLiteral("data:"), Qt::CaseInsensitive)) {
    return false;
  }

  const qsizetype commaIndex = encoded.indexOf(',');
  if (commaIndex <= 0) {
    return false;
  }

  const QString meta = encoded.mid(5, commaIndex - 5);
  const QString payload = encoded.mid(commaIndex + 1);
  if (meta.contains(QStringLiteral(";base64"), Qt::CaseInsensitive)) {
    *bytes = QByteArray::fromBase64(payload.toUtf8());
  } else {
    *bytes = QUrl::fromPercentEncoding(payload.toUtf8()).toUtf8();
  }
  return !bytes->isEmpty();
}

int imageCostBytes(const QImage& image) {
  if (image.isNull()) {
    return 1;
  }

  qint64 bytes = image.sizeInBytes();
  if (bytes <= 0) {
    bytes = static_cast<qint64>(image.width()) * static_cast<qint64>(image.height()) * 4;
  }
  return static_cast<int>(std::clamp<qint64>(bytes, 1, std::numeric_limits<int>::max()));
}

QSize boundedDecodeSize(const QSize& naturalSize, const AdImageLoadOptions& options) {
  if (!naturalSize.isValid() || naturalSize.isEmpty() || !options.targetPixelSize.isValid() ||
      options.targetPixelSize.isEmpty()) {
    return naturalSize;
  }

  QSize target = options.aspectRatioMode == Qt::IgnoreAspectRatio
                     ? options.targetPixelSize
                     : naturalSize.scaled(options.targetPixelSize, options.aspectRatioMode);
  if (!options.allowUpscale) {
    target.setWidth(std::min(target.width(), naturalSize.width()));
    target.setHeight(std::min(target.height(), naturalSize.height()));
  }
  return target.expandedTo(QSize(1, 1));
}

struct DecodedImage {
  QImage image;
  QSize naturalSize;
};

DecodedImage decodeWithReader(QImageReader* reader, const AdImageLoadOptions& options) {
  if (!reader) {
    return {};
  }
  reader->setAutoTransform(true);
  const QSize naturalSize = reader->size();
  const QSize decodeSize = boundedDecodeSize(naturalSize, options);
  if (decodeSize.isValid() && decodeSize != naturalSize) {
    reader->setScaledSize(decodeSize);
  }
  return {reader->read(), naturalSize};
}

DecodedImage decodeImage(const QUrl& source, const QByteArray& suppliedBytes,
                         const AdImageLoadOptions& options) {
  QByteArray bytes = suppliedBytes;
  if (bytes.isEmpty() && decodeDataUrl(source, &bytes)) {
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    return decodeWithReader(&reader, options);
  }
  if (!bytes.isEmpty()) {
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    return decodeWithReader(&reader, options);
  }

  QString path;
  if (source.isLocalFile()) {
    path = source.toLocalFile();
  } else if (source.scheme().compare(QStringLiteral("qrc"), Qt::CaseInsensitive) == 0) {
    path = source.path();
  } else if (source.isRelative()) {
    path = QFileInfo(sourceToString(source)).filePath();
  } else {
    path = sourceToString(source);
  }
  QImageReader reader(path);
  return decodeWithReader(&reader, options);
}

QString loadOptionsKey(const AdImageLoadOptions& options) {
  return QStringLiteral("%1x%2:%3:%4")
      .arg(options.targetPixelSize.width())
      .arg(options.targetPixelSize.height())
      .arg(static_cast<int>(options.aspectRatioMode))
      .arg(options.allowUpscale ? 1 : 0);
}

QString imageCacheKey(const QUrl& source, const AdImageLoadOptions& options) {
  QString sourceKey;
  if (source.isLocalFile()) {
    const QFileInfo info(source.toLocalFile());
    sourceKey = QStringLiteral("file:%1:%2:%3")
                    .arg(QDir::fromNativeSeparators(info.absoluteFilePath()))
                    .arg(info.size())
                    .arg(info.lastModified().toMSecsSinceEpoch());
  } else if (source.scheme().compare(QStringLiteral("data"), Qt::CaseInsensitive) == 0) {
    const QByteArray digest =
        QCryptographicHash::hash(source.toEncoded(), QCryptographicHash::Sha256).toHex();
    sourceKey = QStringLiteral("data:%1").arg(QString::fromLatin1(digest));
  } else {
    sourceKey = normalizeSourceKey(source);
  }
  return sourceKey + u'|' + loadOptionsKey(options);
}

QSize pixmapActualSize(const QPixmap& pixmap) {
  if (pixmap.isNull()) {
    return QSize();
  }

  const qreal dpr = std::max<qreal>(1.0, pixmap.devicePixelRatioF());
  return QSize(qRound(pixmap.width() / dpr), qRound(pixmap.height() / dpr));
}

QPixmap imageToPixmap(const QImage& image) {
  return image.isNull() ? QPixmap() : QPixmap::fromImage(image);
}

template <typename SlotStyle>
void mergeSlotStyle(const SlotStyle& source, SlotStyle* target) {
  if (!target) {
    return;
  }
  if (source.textColor.has_value()) {
    target->textColor = source.textColor;
  }
  if (source.backgroundColor.has_value()) {
    target->backgroundColor = source.backgroundColor;
  }
  if (source.borderColor.has_value()) {
    target->borderColor = source.borderColor;
  }
}

AdImage::SemanticStyles mergedImageSemanticStyles(const AdImage::SemanticStyles& base,
                                                  const AdImage::SemanticStyles& extra) {
  AdImage::SemanticStyles output = base;
  mergeSlotStyle(extra.root, &output.root);
  mergeSlotStyle(extra.image, &output.image);
  mergeSlotStyle(extra.cover, &output.cover);
  return output;
}

AdImageViewer::SemanticStyles mergedViewerSemanticStyles(
    const AdImageViewer::SemanticStyles& base, const AdImageViewer::SemanticStyles& extra) {
  AdImageViewer::SemanticStyles output = base;
  mergeSlotStyle(extra.popupRoot, &output.popupRoot);
  mergeSlotStyle(extra.popupMask, &output.popupMask);
  mergeSlotStyle(extra.popupBody, &output.popupBody);
  mergeSlotStyle(extra.popupFooter, &output.popupFooter);
  mergeSlotStyle(extra.popupActions, &output.popupActions);
  return output;
}

AdImageItem readItemFromModel(const QAbstractItemModel* model, int row) {
  if (!model || row < 0 || row >= model->rowCount()) {
    return AdImageItem{};
  }

  const QModelIndex index = model->index(row, 0);
  if (!index.isValid()) {
    return AdImageItem{};
  }

  AdImageItem item;
  const QVariant sourceValue = model->data(index, AdImageItemRoles::SourceRole);
  if (sourceValue.canConvert<QUrl>()) {
    item.source = sourceValue.toUrl();
  } else if (sourceValue.metaType().id() == QMetaType::QString) {
    item.source = QUrl(sourceValue.toString());
  }

  const QVariant altTextValue = model->data(index, AdImageItemRoles::AltTextRole);
  if (altTextValue.isValid()) {
    item.altText = altTextValue.toString();
  }

  return item;
}

struct ImageMetaTypeRegistration {
  ImageMetaTypeRegistration() {
    qRegisterMetaType<adqt::widgets::AdImageItem>("adqt::widgets::AdImageItem");
    qRegisterMetaType<adqt::widgets::AdImageItems>("adqt::widgets::AdImageItems");
    qRegisterMetaType<adqt::widgets::AdImageViewer*>("adqt::widgets::AdImageViewer*");
    qRegisterMetaType<adqt::widgets::AdImageLoader*>("adqt::widgets::AdImageLoader*");
  }
};

const ImageMetaTypeRegistration kImageMetaTypeRegistration;

class DefaultImageReply final : public AdImageReply {
 public:
  explicit DefaultImageReply(QObject* parent = nullptr) : AdImageReply(parent) {}
  ~DefaultImageReply() override { detach(); }

  void setDetachCallback(std::function<void()> callback) { detachCallback_ = std::move(callback); }

  void completeSuccess(const QImage& image, const QSize& naturalSize) {
    detachCallback_ = {};
    succeed(image, naturalSize);
  }

  void completeFailure(const QString& errorString = QString()) {
    detachCallback_ = {};
    fail(errorString);
  }

  void abort() override {
    if (isFinished()) {
      return;
    }
    detach();
    fail(QStringLiteral("Image load aborted"));
  }

 private:
  void detach() {
    if (!detachCallback_) {
      return;
    }
    auto callback = std::move(detachCallback_);
    detachCallback_ = {};
    callback();
  }

  std::function<void()> detachCallback_;
};

class DefaultImageLoader final : public AdImageLoader {
 public:
  explicit DefaultImageLoader(QObject* parent = nullptr)
      : AdImageLoader(parent), cache_(64 * 1024 * 1024) {
    decodePool_.setMaxThreadCount(2);
    decodePool_.setExpiryTimeout(30'000);
  }

  AdImageReply* load(const QUrl& source, QObject* parent = nullptr) override {
    return load(source, AdImageLoadOptions{}, parent);
  }

  AdImageReply* load(const QUrl& source, const AdImageLoadOptions& options,
                     QObject* parent = nullptr) override {
    Q_UNUSED(kImageMetaTypeRegistration);

    auto* reply = new DefaultImageReply(parent);
    const QString key = imageCacheKey(source, options);
    if (key.isEmpty()) {
      QTimer::singleShot(
          0, reply, [reply]() { reply->completeFailure(QStringLiteral("Empty image source")); });
      return reply;
    }

    if (const DecodedImage* cached = cache_.object(key)) {
      const DecodedImage decoded = *cached;
      QTimer::singleShot(0, reply, [reply, decoded]() {
        if (!reply->isFinished()) {
          reply->completeSuccess(decoded.image, decoded.naturalSize);
        }
      });
      return reply;
    }

    if (const std::shared_ptr<Operation> existing = operations_.value(key)) {
      existing->subscribers.push_back(reply);
      reply->setDetachCallback([this, key, reply]() { detachSubscriber(key, reply); });
      return reply;
    }

    auto operation = std::make_shared<Operation>();
    operation->key = key;
    operation->source = source;
    operation->options = options;
    operation->subscribers.push_back(reply);
    operations_.insert(key, operation);
    reply->setDetachCallback([this, key, reply]() { detachSubscriber(key, reply); });

    if (isRemoteSource(source)) {
      QNetworkReply* networkReply = sharedNetworkManager()->get(QNetworkRequest(source));
      operation->networkReply = networkReply;
      QObject::connect(
          networkReply, &QNetworkReply::finished, this, [this, operation, networkReply]() {
            networkReply->deleteLater();
            if (operations_.value(operation->key) != operation ||
                operation->subscribers.isEmpty()) {
              return;
            }
            if (networkReply->error() != QNetworkReply::NoError) {
              finishOperation(operation, DecodedImage{}, networkReply->errorString());
              return;
            }
            const QByteArray bytes = networkReply->readAll();
            if (bytes.isEmpty()) {
              finishOperation(operation, DecodedImage{},
                              QStringLiteral("Image response was empty"));
              return;
            }
            startDecode(operation, bytes);
          });
    } else {
      startDecode(operation, QByteArray());
    }

    return reply;
  }

 private:
  struct Operation {
    QString key;
    QUrl source;
    AdImageLoadOptions options;
    QVector<QPointer<DefaultImageReply>> subscribers;
    QPointer<QNetworkReply> networkReply;
  };

  void startDecode(const std::shared_ptr<Operation>& operation, QByteArray bytes) {
    const QPointer<DefaultImageLoader> guardedLoader(this);
    decodePool_.start([guardedLoader, operation, bytes = std::move(bytes)]() mutable {
      const DecodedImage decoded = decodeImage(operation->source, bytes, operation->options);
      if (!guardedLoader) {
        return;
      }
      QMetaObject::invokeMethod(
          guardedLoader,
          [guardedLoader, operation, decoded]() {
            if (!guardedLoader) {
              return;
            }
            guardedLoader->finishOperation(
                operation, decoded,
                decoded.image.isNull() ? QStringLiteral("Failed to decode image data")
                                       : QString());
          },
          Qt::QueuedConnection);
    });
  }

  void finishOperation(const std::shared_ptr<Operation>& operation, const DecodedImage& decoded,
                       const QString& errorString) {
    if (operations_.value(operation->key) != operation) {
      return;
    }
    operations_.remove(operation->key);
    if (operation->subscribers.isEmpty()) {
      return;
    }
    if (!decoded.image.isNull()) {
      cache_.insert(operation->key, new DecodedImage(decoded), imageCostBytes(decoded.image));
    }
    const QVector<QPointer<DefaultImageReply>> subscribers = operation->subscribers;
    operation->subscribers.clear();
    for (const QPointer<DefaultImageReply>& subscriber : subscribers) {
      if (!subscriber || subscriber->isFinished()) {
        continue;
      }
      if (!decoded.image.isNull()) {
        subscriber->completeSuccess(decoded.image, decoded.naturalSize);
      } else {
        subscriber->completeFailure(errorString);
      }
    }
  }

  void detachSubscriber(const QString& key, DefaultImageReply* reply) {
    const std::shared_ptr<Operation> operation = operations_.value(key);
    if (!operation) {
      return;
    }
    operation->subscribers.erase(
        std::remove_if(operation->subscribers.begin(), operation->subscribers.end(),
                       [reply](const QPointer<DefaultImageReply>& subscriber) {
                         return !subscriber || subscriber.data() == reply;
                       }),
        operation->subscribers.end());
    if (!operation->subscribers.isEmpty()) {
      return;
    }
    operations_.remove(key);
    if (operation->networkReply) {
      operation->networkReply->disconnect(this);
      if (operation->networkReply->isRunning()) {
        operation->networkReply->abort();
      }
      operation->networkReply->deleteLater();
      operation->networkReply.clear();
    }
  }

  static QNetworkAccessManager* sharedNetworkManager() {
    static QNetworkAccessManager* manager = new QNetworkAccessManager(qApp);
    return manager;
  }

  QCache<QString, DecodedImage> cache_;
  QHash<QString, std::shared_ptr<Operation>> operations_;
  QThreadPool decodePool_;
};

AdImageLoader* defaultImageLoaderInstance() {
  static DefaultImageLoader* loader = new DefaultImageLoader(qApp);
  return loader;
}

AdImageLoader* resolvedImageLoader(AdImageLoader* loader) {
  return loader ? loader : defaultImageLoaderInstance();
}

void cancelImageReply(QPointer<AdImageReply>& reply) {
  if (!reply) {
    return;
  }

  reply->disconnect();
  reply->abort();
  reply->deleteLater();
  reply.clear();
}

struct PreviewDialogEntry {
  AdImageItem item;
  QPointer<AdImageLoader> loader;
};

class ImagePreviewDialog final : public QWidget {
 public:
  class RoundedBackgroundWidget final : public QWidget {
   public:
    explicit RoundedBackgroundWidget(QWidget* parent = nullptr) : QWidget(parent) {
      setAttribute(Qt::WA_TranslucentBackground, true);
      setAttribute(Qt::WA_NoSystemBackground, true);
      setAutoFillBackground(false);
    }

    void setBackgroundColor(const QColor& color) {
      if (backgroundColor_ == color) {
        return;
      }
      backgroundColor_ = color;
      update();
    }

    void setCornerRadius(qreal radius) {
      const qreal clamped = std::max(0.0, radius);
      if (qFuzzyCompare(cornerRadius_ + 1.0, clamped + 1.0)) {
        return;
      }
      cornerRadius_ = clamped;
      update();
    }

   protected:
    void paintEvent(QPaintEvent* event) override {
      Q_UNUSED(event);
      if (!backgroundColor_.isValid() || backgroundColor_.alpha() <= 0) {
        return;
      }

      QPainter painter(this);
      painter.setRenderHint(QPainter::Antialiasing, true);
      painter.setPen(Qt::NoPen);
      painter.setBrush(backgroundColor_);

      QRectF bounds = rect();
      bounds.adjust(0.5, 0.5, -0.5, -0.5);
      const qreal radius = std::min(cornerRadius_, std::min(bounds.width(), bounds.height()) * 0.5);
      painter.drawRoundedRect(bounds, radius, radius);
    }

   private:
    QColor backgroundColor_ = QColor(Qt::transparent);
    qreal cornerRadius_ = 0.0;
  };

  class OverlayIconButton final : public QToolButton {
   public:
    explicit OverlayIconButton(QWidget* parent = nullptr) : QToolButton(parent) {
      setAttribute(Qt::WA_Hover, true);
      setMouseTracking(true);
      setAutoRaise(false);
      setToolButtonStyle(Qt::ToolButtonIconOnly);
    }

    void setVisualStyle(const QColor& normalBackground, const QColor& hoverBackground,
                        const QColor& pressedBackground, const QColor& disabledBackground,
                        qreal radius) {
      const qreal normalizedRadius = std::max<qreal>(0.0, radius);
      const bool changed =
          normalBackground_ != normalBackground || hoverBackground_ != hoverBackground ||
          pressedBackground_ != pressedBackground || disabledBackground_ != disabledBackground ||
          !qFuzzyCompare(radius_ + 1.0, normalizedRadius + 1.0);
      normalBackground_ = normalBackground;
      hoverBackground_ = hoverBackground;
      pressedBackground_ = pressedBackground;
      disabledBackground_ = disabledBackground;
      radius_ = normalizedRadius;
      if (changed) {
        update();
      }
    }

   protected:
    void enterEvent(QEnterEvent* event) override {
      hovered_ = true;
      update();
      QToolButton::enterEvent(event);
    }

    void leaveEvent(QEvent* event) override {
      hovered_ = false;
      update();
      QToolButton::leaveEvent(event);
    }

    void changeEvent(QEvent* event) override {
      QToolButton::changeEvent(event);
      if (event && event->type() == QEvent::EnabledChange) {
        update();
      }
    }

    void paintEvent(QPaintEvent* event) override {
      Q_UNUSED(event);

      QPainter painter(this);
      painter.setRenderHint(QPainter::Antialiasing, true);

      QColor background = isEnabled() ? normalBackground_ : disabledBackground_;
      if (isEnabled() && isDown()) {
        background = pressedBackground_.isValid() ? pressedBackground_ : hoverBackground_;
      } else if (isEnabled() && hovered_) {
        background = hoverBackground_.isValid() ? hoverBackground_ : normalBackground_;
      }

      if (background.isValid() && background.alpha() > 0) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(background);
        QRectF bounds = rect();
        bounds.adjust(0.5, 0.5, -0.5, -0.5);
        const qreal radius = std::min(radius_, std::min(bounds.width(), bounds.height()) * 0.5);
        painter.drawRoundedRect(bounds, radius, radius);
      }

      const QIcon currentIcon = icon();
      if (!currentIcon.isNull()) {
        const QIcon::Mode mode = !isEnabled()
                                     ? QIcon::Disabled
                                     : ((hovered_ || isDown()) ? QIcon::Active : QIcon::Normal);
        const QSize logicalSize = iconSize().isValid() ? iconSize() : QSize(16, 16);
        const QPixmap pixmap = currentIcon.pixmap(logicalSize, mode, QIcon::Off);
        if (!pixmap.isNull()) {
          const QSize pixmapSize = pixmapActualSize(pixmap);
          const QPoint topLeft((width() - pixmapSize.width()) / 2,
                               (height() - pixmapSize.height()) / 2);
          painter.drawPixmap(topLeft, pixmap);
        }
      }
    }

   private:
    bool hovered_ = false;
    QColor normalBackground_ = QColor(Qt::transparent);
    QColor hoverBackground_ = QColor(Qt::transparent);
    QColor pressedBackground_ = QColor(Qt::transparent);
    QColor disabledBackground_ = QColor(Qt::transparent);
    qreal radius_ = 0.0;
  };

  class ImagePreviewCanvas final : public QWidget {
   public:
    explicit ImagePreviewCanvas(QWidget* parent = nullptr) : QWidget(parent) {
      setAttribute(Qt::WA_TranslucentBackground, true);
      setAttribute(Qt::WA_NoSystemBackground, true);
      setAutoFillBackground(false);
      setMouseTracking(true);
      setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
      updateCursor();
    }

    bool hasImage() const { return !image_.isNull(); }

    void setImage(const QImage& image) {
      const bool changed = image.cacheKey() != image_.cacheKey() || image.size() != image_.size();
      image_ = image;
      dragging_ = false;
      if (image_.isNull()) {
        resetTransform();
        return;
      }
      if (changed) {
        clampPanOffset();
        updateCursor();
        update();
      }
    }

    void clearImage() {
      if (image_.isNull() && panOffset_.isNull() && !dragging_ && rotation_ == 0 && !flipX_ &&
          !flipY_ && qFuzzyCompare(userScale_ + 1.0, 2.0)) {
        return;
      }

      image_ = QImage();
      dragging_ = false;
      rotation_ = 0;
      flipX_ = false;
      flipY_ = false;
      userScale_ = 1.0;
      panOffset_ = QPointF();
      updateCursor();
      update();
    }

    void resetTransform() {
      const bool hadState = dragging_ || rotation_ != 0 || flipX_ || flipY_ ||
                            !panOffset_.isNull() || !qFuzzyCompare(userScale_ + 1.0, 2.0);
      dragging_ = false;
      rotation_ = 0;
      flipX_ = false;
      flipY_ = false;
      userScale_ = 1.0;
      panOffset_ = QPointF();
      clampPanOffset();
      updateCursor();
      if (hadState) {
        update();
      }
    }

    qreal scale() const { return userScale_; }

    void setScale(qreal value, std::optional<QPointF> anchor = std::nullopt) {
      const qreal nextScale = std::clamp(value, 1.0, 50.0);
      const qreal previousScale = userScale_;
      const QRectF previousRect = targetRect();
      const bool anchoredZoom = anchor.has_value();

      userScale_ = nextScale;

      if (!image_.isNull() && previousRect.isValid() &&
          !qFuzzyCompare(previousScale + 1.0, nextScale + 1.0)) {
        const QRectF zoomBounds = contentRect();
        QPointF zoomAnchor = zoomBounds.center();
        if (anchor.has_value() && zoomBounds.isValid()) {
          const QPointF requestedAnchor = anchor.value();
          zoomAnchor.setX(std::clamp(requestedAnchor.x(), zoomBounds.left(), zoomBounds.right()));
          zoomAnchor.setY(std::clamp(requestedAnchor.y(), zoomBounds.top(), zoomBounds.bottom()));
        }

        const QSizeF nextSize = displaySize();
        if (nextSize.isValid()) {
          QPointF normalized(0.5, 0.5);
          if (previousRect.width() > 0.5) {
            normalized.setX((zoomAnchor.x() - previousRect.left()) / previousRect.width());
          }
          if (previousRect.height() > 0.5) {
            normalized.setY((zoomAnchor.y() - previousRect.top()) / previousRect.height());
          }

          const QPointF nextTopLeft(zoomAnchor.x() - normalized.x() * nextSize.width(),
                                    zoomAnchor.y() - normalized.y() * nextSize.height());
          panOffset_ = nextTopLeft + QPointF(nextSize.width() * 0.5, nextSize.height() * 0.5) -
                       contentRect().center();
        }
      }

      if (!anchoredZoom || nextScale <= 1.001) {
        clampPanOffset();
      }
      updateCursor();
      update();
    }

    void rotateBy(int delta) {
      if (image_.isNull()) {
        return;
      }
      rotation_ = normalizeRotation(rotation_ + delta);
      clampPanOffset();
      updateCursor();
      update();
    }

    void toggleFlipX() {
      if (image_.isNull()) {
        return;
      }
      flipX_ = !flipX_;
      update();
    }

    void toggleFlipY() {
      if (image_.isNull()) {
        return;
      }
      flipY_ = !flipY_;
      update();
    }

    bool containsImagePoint(const QPointF& point) const {
      const QRectF rect = targetRect();
      return rect.isValid() && rect.contains(point);
    }

    std::function<void(int delta, const QPointF& position)> wheelZoomRequested;
    std::function<void()> backgroundClickRequested;

   protected:
    void paintEvent(QPaintEvent* event) override {
      Q_UNUSED(event);
      if (image_.isNull()) {
        return;
      }

      const QRectF bounds = targetRect();
      const QSizeF transformedSize = transformedImageSize();
      if (!bounds.isValid() || !transformedSize.isValid()) {
        return;
      }

      const qreal scale = std::min(bounds.width() / transformedSize.width(),
                                   bounds.height() / transformedSize.height());
      if (!(scale > 0.0)) {
        return;
      }

      QPainter painter(this);
      painter.setRenderHint(QPainter::Antialiasing, true);
      painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
      painter.translate(bounds.center());
      painter.scale(scale, scale);
      painter.scale(flipX_ ? -1.0 : 1.0, flipY_ ? -1.0 : 1.0);
      painter.rotate(rotation_);
      const QRectF imageRect(-image_.width() * 0.5, -image_.height() * 0.5,
                             static_cast<qreal>(image_.width()),
                             static_cast<qreal>(image_.height()));
      painter.drawImage(imageRect, image_);
    }

    void resizeEvent(QResizeEvent* event) override {
      QWidget::resizeEvent(event);
      clampPanOffset();
      updateCursor();
      update();
    }

    void mousePressEvent(QMouseEvent* event) override {
      if (event && event->button() == Qt::LeftButton) {
        if (!containsImagePoint(event->position())) {
          if (backgroundClickRequested) {
            backgroundClickRequested();
          }
          event->accept();
          return;
        }

        if (canPan()) {
          dragging_ = true;
          dragAnchor_ = event->position();
          dragOriginOffset_ = panOffset_;
          updateCursor();
          event->accept();
          return;
        }

        event->accept();
        return;
      }

      QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
      if (!event) {
        QWidget::mouseMoveEvent(event);
        return;
      }

      if (dragging_) {
        panOffset_ = dragOriginOffset_ + (event->position() - dragAnchor_);
        clampPanOffset();
        update();
        event->accept();
        return;
      }

      updateCursor();
      QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
      if (event && event->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        updateCursor();
        event->accept();
        return;
      }

      QWidget::mouseReleaseEvent(event);
    }

    void leaveEvent(QEvent* event) override {
      if (!dragging_) {
        updateCursor();
      }
      QWidget::leaveEvent(event);
    }

    void wheelEvent(QWheelEvent* event) override {
      if (event && wheelZoomRequested && event->angleDelta().y() != 0) {
        wheelZoomRequested(event->angleDelta().y(), event->position());
        event->accept();
        return;
      }

      QWidget::wheelEvent(event);
    }

   private:
    static int normalizeRotation(int value) {
      int normalized = value % 360;
      if (normalized < 0) {
        normalized += 360;
      }
      return normalized;
    }

    QRectF contentRect() const { return rect(); }

    QSizeF transformedImageSize() const {
      if (image_.isNull()) {
        return QSizeF();
      }

      const bool rotated = (normalizeRotation(rotation_) % 180) != 0;
      return rotated ? QSizeF(image_.height(), image_.width())
                     : QSizeF(image_.width(), image_.height());
    }

    QSizeF displaySize() const {
      const QSizeF logicalSize = transformedImageSize();
      const QRectF fitBounds = contentRect();
      if (!logicalSize.isValid() || fitBounds.isEmpty()) {
        return QSizeF();
      }

      constexpr qreal kPreviewImageMaxHeightRatio = 0.7;
      const qreal fitScale =
          std::min(fitBounds.width() / logicalSize.width(),
                   fitBounds.height() * kPreviewImageMaxHeightRatio / logicalSize.height());
      const qreal baseScale = std::min<qreal>(1.0, fitScale);
      const qreal finalScale = std::max(0.01, baseScale * userScale_);
      return QSizeF(logicalSize.width() * finalScale, logicalSize.height() * finalScale);
    }

    qreal panOffsetLimit(qreal viewportExtent, qreal contentExtent) const {
      if (viewportExtent <= 0.5 || contentExtent <= 0.5) {
        return 0.0;
      }
      if (contentExtent >= viewportExtent) {
        return (contentExtent - viewportExtent) * 0.5;
      }
      if (userScale_ <= 1.001) {
        return 0.0;
      }
      return viewportExtent * 0.5;
    }

    bool canPan() const {
      const QRectF inner = contentRect();
      const QSizeF size = displaySize();
      if (inner.isEmpty() || !size.isValid()) {
        return false;
      }

      return panOffsetLimit(inner.width(), size.width()) > 0.5 ||
             panOffsetLimit(inner.height(), size.height()) > 0.5;
    }

    void clampPanOffset() {
      const QRectF inner = contentRect();
      const QSizeF size = displaySize();
      if (inner.isEmpty() || !size.isValid()) {
        panOffset_ = QPointF();
        return;
      }

      const qreal maxOffsetX = panOffsetLimit(inner.width(), size.width());
      const qreal maxOffsetY = panOffsetLimit(inner.height(), size.height());
      panOffset_.setX(maxOffsetX > 0.5 ? std::clamp(panOffset_.x(), -maxOffsetX, maxOffsetX) : 0.0);
      panOffset_.setY(maxOffsetY > 0.5 ? std::clamp(panOffset_.y(), -maxOffsetY, maxOffsetY) : 0.0);
    }

    QRectF targetRect() const {
      const QRectF inner = contentRect();
      const QSizeF size = displaySize();
      if (inner.isEmpty() || !size.isValid()) {
        return QRectF();
      }

      const QPointF center = inner.center() + panOffset_;
      const QPointF topLeft(center.x() - size.width() * 0.5, center.y() - size.height() * 0.5);
      return QRectF(topLeft, size);
    }

    void updateCursor() {
      if (dragging_) {
        setCursor(Qt::ClosedHandCursor);
        return;
      }
      if (canPan()) {
        setCursor(Qt::OpenHandCursor);
        return;
      }
      setCursor(Qt::ArrowCursor);
    }

    QImage image_;
    qreal userScale_ = 1.0;
    QPointF panOffset_;
    QPointF dragAnchor_;
    QPointF dragOriginOffset_;
    bool dragging_ = false;
    int rotation_ = 0;
    bool flipX_ = false;
    bool flipY_ = false;
  };

  struct ButtonIconSpec {
    QToolButton* button = nullptr;
    adqt::icons::IconRef token;
    qreal rotationDegrees = 0.0;
  };

  explicit ImagePreviewDialog(QWidget* parent = nullptr) : QWidget(parent) {
    setAttribute(Qt::WA_DeleteOnClose, false);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::StrongFocus);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    container_ = new QWidget(this);
    container_->setAttribute(Qt::WA_StyledBackground, true);
    container_->setObjectName(QStringLiteral("ad-image-preview-container"));
    rootLayout->addWidget(container_);

    maskLayer_ = new RoundedBackgroundWidget(container_);
    maskLayer_->setObjectName(QStringLiteral("ad-image-preview-mask"));
    maskLayer_->installEventFilter(this);

    closeButton_ = new OverlayIconButton(container_);
    closeButton_->setObjectName(QStringLiteral("ad-image-preview-close"));
    closeButton_->setCursor(Qt::PointingHandCursor);
    closeButton_->setToolTip(tr("Close"));
    closeButtonSpec_.button = closeButton_;
    closeButtonSpec_.token = outlined_icons::Close();
    connect(closeButton_, &QToolButton::clicked, this, [this]() { close(); });

    prevButton_ = new OverlayIconButton(container_);
    prevButton_->setObjectName(QStringLiteral("ad-image-preview-prev"));
    prevButton_->setCursor(Qt::PointingHandCursor);
    prevButton_->setToolTip(tr("Previous"));
    prevButtonSpec_.button = prevButton_;
    prevButtonSpec_.token = outlined_icons::Left();
    connect(prevButton_, &QToolButton::clicked, this, [this]() { activate(-1); });

    nextButton_ = new OverlayIconButton(container_);
    nextButton_->setObjectName(QStringLiteral("ad-image-preview-next"));
    nextButton_->setCursor(Qt::PointingHandCursor);
    nextButton_->setToolTip(tr("Next"));
    nextButtonSpec_.button = nextButton_;
    nextButtonSpec_.token = outlined_icons::Right();
    connect(nextButton_, &QToolButton::clicked, this, [this]() { activate(1); });

    bodyHost_ = new QWidget(container_);
    bodyHost_->setAttribute(Qt::WA_StyledBackground, true);
    bodyHost_->setObjectName(QStringLiteral("ad-image-preview-body"));
    bodyHost_->installEventFilter(this);

    auto* bodyLayout = new QVBoxLayout(bodyHost_);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    imageStack_ = new QStackedLayout();
    imageStack_->setContentsMargins(0, 0, 0, 0);
    imageStack_->setStackingMode(QStackedLayout::StackOne);

    loadingLabel_ = new QLabel(tr("Loading image..."), bodyHost_);
    loadingLabel_->setObjectName(QStringLiteral("ad-image-preview-loading"));
    loadingLabel_->setAlignment(Qt::AlignCenter);
    loadingLabel_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    imageStack_->addWidget(loadingLabel_);

    imageCanvas_ = new ImagePreviewCanvas(bodyHost_);
    imageCanvas_->setObjectName(QStringLiteral("ad-image-preview-image"));
    imageCanvas_->wheelZoomRequested = [this](int delta, const QPointF& position) {
      if (delta > 0) {
        zoomIn(position);
      } else if (delta < 0) {
        zoomOut(position);
      }
    };
    imageCanvas_->backgroundClickRequested = [this]() { close(); };
    imageStack_->addWidget(imageCanvas_);
    bodyLayout->addLayout(imageStack_);

    footer_ = new QWidget(container_);
    footer_->setAttribute(Qt::WA_StyledBackground, true);
    footer_->setObjectName(QStringLiteral("ad-image-preview-footer"));

    footerLayout_ = new QVBoxLayout(footer_);
    footerLayout_->setContentsMargins(0, 0, 0, 0);
    footerLayout_->setSpacing(16);

    countLabel_ = new QLabel(footer_);
    countLabel_->setObjectName(QStringLiteral("ad-image-preview-count"));
    countLabel_->setAlignment(Qt::AlignCenter);
    footerLayout_->addWidget(countLabel_, 0, Qt::AlignHCenter);

    actions_ = new RoundedBackgroundWidget(footer_);
    actionsLayout_ = new QHBoxLayout(actions_);
    actionsLayout_->setContentsMargins(24, 0, 24, 0);
    actionsLayout_->setSpacing(12);

    auto createAction = [this](const adqt::icons::IconRef& token, qreal rotationDegrees,
                               const QString& tooltip, const std::function<void()>& fn) {
      auto* button = new OverlayIconButton(actions_);
      button->setCursor(Qt::PointingHandCursor);
      button->setToolTip(tooltip);
      actionIconSpecs_.append(ButtonIconSpec{button, token, rotationDegrees});
      connect(button, &QToolButton::clicked, this, [fn]() {
        if (fn) {
          fn();
        }
      });
      actionsLayout_->addWidget(button);
      actionButtons_.append(button);
      return button;
    };

    flipYButton_ =
        createAction(outlined_icons::Swap(), 90.0, tr("Flip vertical"), [this]() { flipY(); });
    flipYButton_->setObjectName(QStringLiteral("ad-image-preview-flip-y"));
    flipXButton_ =
        createAction(outlined_icons::Swap(), 0.0, tr("Flip horizontal"), [this]() { flipX(); });
    flipXButton_->setObjectName(QStringLiteral("ad-image-preview-flip-x"));
    rotateLeftButton_ = createAction(outlined_icons::RotateLeft(), 0.0, tr("Rotate left"),
                                     [this]() { rotateLeft(); });
    rotateLeftButton_->setObjectName(QStringLiteral("ad-image-preview-rotate-left"));
    rotateRightButton_ = createAction(outlined_icons::RotateRight(), 0.0, tr("Rotate right"),
                                      [this]() { rotateRight(); });
    rotateRightButton_->setObjectName(QStringLiteral("ad-image-preview-rotate-right"));
    zoomOutButton_ =
        createAction(outlined_icons::ZoomOut(), 0.0, tr("Zoom out"), [this]() { zoomOut(); });
    zoomOutButton_->setObjectName(QStringLiteral("ad-image-preview-zoom-out"));
    zoomInButton_ =
        createAction(outlined_icons::ZoomIn(), 0.0, tr("Zoom in"), [this]() { zoomIn(); });
    zoomInButton_->setObjectName(QStringLiteral("ad-image-preview-zoom-in"));

    footerLayout_->addWidget(actions_, 0, Qt::AlignHCenter);

    applyVisualStyle();
    updateOverlayLayout();
  }

  ~ImagePreviewDialog() override {
    cancelImageReply(loadReply_);
    if (hostWindow_) {
      hostWindow_->removeEventFilter(this);
    }
  }

  std::function<void(bool visible)> visibilityChanged;
  std::function<void(int currentRow)> currentRowChanged;
  std::function<void(int currentRow, int totalCount, const AdImageItem& item,
                     const QSize& actualSize)>
      currentItemChanged;

  void setEntries(const QVector<PreviewDialogEntry>& entries, int preferredRow = -1) {
    entries_ = entries;
    loadedImage_ = QImage();
    cancelImageReply(loadReply_);

    const int nextRow = resolveOpenableEntryRow(preferredRow >= 0 ? preferredRow : currentRow_);
    if (nextRow < 0) {
      currentRow_ = -1;
      imageCanvas_->clearImage();
      imageStack_->setCurrentWidget(loadingLabel_);
      loadingLabel_->setText(tr("No preview available"));
      updateControls();
      return;
    }

    setCurrentRow(nextRow, false);
  }

  void setCurrentRow(int row, bool emitChange) {
    const int next = resolveOpenableEntryRow(row);
    if (next < 0) {
      const int previous = currentRow_;
      currentRow_ = -1;
      loadedImage_ = QImage();
      imageCanvas_->clearImage();
      imageStack_->setCurrentWidget(loadingLabel_);
      loadingLabel_->setText(tr("No preview available"));
      updateControls();
      if (emitChange && previous != currentRow_ && currentRowChanged) {
        currentRowChanged(currentRow_);
      }
      return;
    }

    const int previous = currentRow_;
    if (next == currentRow_ && !loadedImage_.isNull()) {
      updateControls();
      return;
    }

    currentRow_ = next;
    imageCanvas_->resetTransform();
    loadCurrentItem();
    updateControls();

    if (emitChange && previous != currentRow_ && currentRowChanged) {
      currentRowChanged(currentRow_);
    }
  }

  int currentRow() const { return currentRow_; }

  int totalCount() const { return static_cast<int>(entries_.size()); }

  void setCountTextFormat(const QString& value) {
    countTextFormat_ = value;
    updateControls();
  }

  void setScaleStep(double value) { scaleStep_ = std::clamp(value, 0.05, 10.0); }

  void setMaskVisible(bool value) {
    maskVisible_ = value;
    applyVisualStyle();
  }

  void setVisualStyle(const detail::ImageVisualStyle& style) {
    visualStyle_ = style;
    applyVisualStyle();
  }

  void openFor(QWidget* owner) {
    QWidget* ownerWindow = owner ? owner->window() : nullptr;
    if (!ownerWindow) {
      ownerWindow = qobject_cast<QWidget*>(qApp->activeWindow());
    }
    if (!ownerWindow) {
      return;
    }

    attachToHostWindow(ownerWindow);
    syncToHostGeometry();
    show();
    raise();
    updateOverlayLayout();
    setFocus(Qt::OtherFocusReason);
  }

  void activate(int delta) {
    if (entries_.size() <= 1 || delta == 0) {
      return;
    }

    const int next = nextOpenableEntryRow(delta);
    if (next >= 0) {
      setCurrentRow(next, true);
    }
  }

  void zoomIn(std::optional<QPointF> anchor = std::nullopt) {
    if (!imageCanvas_ || !imageCanvas_->hasImage()) {
      return;
    }
    imageCanvas_->setScale(imageCanvas_->scale() + scaleStep_, anchor);
    updateControls();
  }

  void zoomOut(std::optional<QPointF> anchor = std::nullopt) {
    if (!imageCanvas_ || !imageCanvas_->hasImage()) {
      return;
    }
    imageCanvas_->setScale(imageCanvas_->scale() - scaleStep_, anchor);
    updateControls();
  }

  void rotateLeft() {
    if (!imageCanvas_ || !imageCanvas_->hasImage()) {
      return;
    }
    imageCanvas_->rotateBy(-90);
    updateControls();
  }

  void rotateRight() {
    if (!imageCanvas_ || !imageCanvas_->hasImage()) {
      return;
    }
    imageCanvas_->rotateBy(90);
    updateControls();
  }

  void flipX() {
    if (!imageCanvas_ || !imageCanvas_->hasImage()) {
      return;
    }
    imageCanvas_->toggleFlipX();
    updateControls();
  }

  void flipY() {
    if (!imageCanvas_ || !imageCanvas_->hasImage()) {
      return;
    }
    imageCanvas_->toggleFlipY();
    updateControls();
  }

  void resetTransform() {
    if (!imageCanvas_) {
      return;
    }
    imageCanvas_->resetTransform();
    updateControls();
  }

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (watched == maskLayer_ && event && event->type() == QEvent::MouseButtonPress) {
      close();
      return true;
    }

    if (watched == bodyHost_ && event && event->type() == QEvent::MouseButtonPress) {
      close();
      return true;
    }

    if (watched == hostWindow_ && event) {
      switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Move:
        case QEvent::Show:
          syncToHostGeometry();
          updateOverlayLayout();
          break;
        case QEvent::Hide:
          close();
          break;
        default:
          break;
      }
    }

    return QWidget::eventFilter(watched, event);
  }

  void resizeEvent(QResizeEvent* event) override {
    QWidget::resizeEvent(event);
    updateOverlayLayout();
  }

  void wheelEvent(QWheelEvent* event) override {
    if (!event) {
      return;
    }

    if (event->angleDelta().y() > 0) {
      zoomIn();
    } else if (event->angleDelta().y() < 0) {
      zoomOut();
    }
    event->accept();
  }

  void keyPressEvent(QKeyEvent* event) override {
    if (!event) {
      return;
    }

    switch (event->key()) {
      case Qt::Key_Escape:
        close();
        return;
      case Qt::Key_Left:
        activate(-1);
        return;
      case Qt::Key_Right:
        activate(1);
        return;
      case Qt::Key_Plus:
      case Qt::Key_Equal:
        zoomIn();
        return;
      case Qt::Key_Minus:
      case Qt::Key_Underscore:
        zoomOut();
        return;
      default:
        break;
    }

    QWidget::keyPressEvent(event);
  }

  void closeEvent(QCloseEvent* event) override {
    ++loadToken_;
    cancelImageReply(loadReply_);
    loadedImage_ = QImage();
    imageCanvas_->clearImage();
    imageStack_->setCurrentWidget(loadingLabel_);
    QWidget::closeEvent(event);
    if (visibilityChanged) {
      visibilityChanged(false);
    }
  }

  void showEvent(QShowEvent* event) override {
    QWidget::showEvent(event);
    syncToHostGeometry();
    updateOverlayLayout();
    if (visibilityChanged) {
      visibilityChanged(true);
    }
  }

 private:
  static adqt::icons::IconRef withPrimaryColor(adqt::icons::IconRef token, const QColor& color) {
    if (!token.isValid() || !color.isValid()) {
      return token;
    }

    return token.withColors(token.colors().withPrimary(color));
  }

  static QPixmap rotatedIconPixmap(const QPixmap& source, qreal degrees) {
    if (source.isNull() || qFuzzyIsNull(std::fmod(std::abs(degrees), 360.0))) {
      return source;
    }

    QTransform transform;
    transform.rotate(degrees);
    QPixmap rotated = source.transformed(transform, Qt::SmoothTransformation);
    if (!rotated.isNull()) {
      rotated.setDevicePixelRatio(source.devicePixelRatio());
    }
    return rotated;
  }

  QPixmap renderButtonIconPixmap(const ButtonIconSpec& spec, const QColor& color,
                                 const QSize& logicalSize, QIcon::Mode mode) const {
    if (!spec.button || !spec.token.isValid()) {
      return QPixmap();
    }

    const adqt::icons::IconRef coloredToken = withPrimaryColor(spec.token, color);
    const qreal dpr = std::max(1.0, devicePixelRatioF());
    QPixmap pixmap =
        adqt::icons::renderIconPixmap(coloredToken, {logicalSize, dpr, mode, QIcon::Off});
    if (pixmap.isNull()) {
      return pixmap;
    }

    return rotatedIconPixmap(pixmap, spec.rotationDegrees);
  }

  void applyButtonIconColorOverrides(const ButtonIconSpec& spec, const QSize& logicalSize,
                                     const QColor& normalColor, const QColor& activeColor,
                                     const QColor& disabledColor) {
    if (!spec.button) {
      return;
    }

    if (!spec.token.isValid()) {
      spec.button->setIcon(QIcon());
      return;
    }

    QIcon icon;

    const QPixmap normal = renderButtonIconPixmap(spec, normalColor, logicalSize, QIcon::Normal);
    if (!normal.isNull()) {
      icon.addPixmap(normal, QIcon::Normal, QIcon::Off);
    }

    const QColor resolvedActiveColor = activeColor.isValid() ? activeColor : normalColor;
    const QPixmap active =
        renderButtonIconPixmap(spec, resolvedActiveColor, logicalSize, QIcon::Active);
    if (!active.isNull()) {
      icon.addPixmap(active, QIcon::Active, QIcon::Off);
    }

    const QColor resolvedDisabledColor = disabledColor.isValid() ? disabledColor : normalColor;
    const QPixmap disabled =
        renderButtonIconPixmap(spec, resolvedDisabledColor, logicalSize, QIcon::Disabled);
    if (!disabled.isNull()) {
      icon.addPixmap(disabled, QIcon::Disabled, QIcon::Off);
    }

    if (!icon.isNull()) {
      spec.button->setIcon(icon);
      return;
    }

    spec.button->setIcon(adqt::icons::makeIcon(spec.token));
  }

  int resolveOpenableEntryRow(int preferredRow) const {
    if (entries_.isEmpty()) {
      return -1;
    }

    if (preferredRow >= 0 && preferredRow < entries_.size() &&
        entries_.at(preferredRow).item.isValid()) {
      return preferredRow;
    }

    for (int row = 0; row < entries_.size(); ++row) {
      if (entries_.at(row).item.isValid()) {
        return row;
      }
    }

    return -1;
  }

  int nextOpenableEntryRow(int delta) const {
    if (entries_.isEmpty() || delta == 0) {
      return -1;
    }

    int row = currentRow_;
    while (true) {
      row += delta > 0 ? 1 : -1;
      if (row < 0 || row >= entries_.size()) {
        return -1;
      }
      if (entries_.at(row).item.isValid()) {
        return row;
      }
    }
  }

  void attachToHostWindow(QWidget* hostWindow) {
    if (hostWindow_ == hostWindow) {
      return;
    }

    if (hostWindow_) {
      hostWindow_->removeEventFilter(this);
    }

    hostWindow_ = hostWindow;
    if (!hostWindow_) {
      return;
    }

    hostWindow_->installEventFilter(this);
    if (parentWidget() != hostWindow_) {
      setParent(hostWindow_);
    }
  }

  void syncToHostGeometry() {
    if (!hostWindow_) {
      return;
    }
    setGeometry(hostWindow_->rect());
  }

  void loadCurrentItem() {
    cancelImageReply(loadReply_);

    if (currentRow_ < 0 || currentRow_ >= entries_.size()) {
      loadedImage_ = QImage();
      imageCanvas_->clearImage();
      imageStack_->setCurrentWidget(loadingLabel_);
      loadingLabel_->setText(tr("No preview available"));
      updateControls();
      return;
    }

    const PreviewDialogEntry entry = entries_.at(currentRow_);
    if (!entry.item.isValid()) {
      loadedImage_ = QImage();
      imageCanvas_->clearImage();
      imageStack_->setCurrentWidget(loadingLabel_);
      loadingLabel_->setText(tr("No preview available"));
      if (currentItemChanged) {
        currentItemChanged(currentRow_, totalCount(), entry.item, QSize());
      }
      updateControls();
      return;
    }

    const int token = ++loadToken_;
    const AdImageItem item = entry.item;
    imageStack_->setCurrentWidget(loadingLabel_);
    loadingLabel_->setText(tr("Loading image..."));

    AdImageReply* reply =
        resolvedImageLoader(entry.loader.data())->load(item.source, AdImageLoadOptions{}, this);
    loadReply_ = reply;
    const QPointer<AdImageReply> guardedReply(reply);
    QObject::connect(reply, &AdImageReply::finished, this, [this, token, item, guardedReply]() {
      if (token != loadToken_) {
        if (guardedReply) {
          guardedReply->deleteLater();
        }
        return;
      }

      const QImage image = guardedReply ? guardedReply->image() : QImage();
      const bool ok = guardedReply && guardedReply->isSuccessful() && !image.isNull();
      loadReply_.clear();
      if (guardedReply) {
        guardedReply->deleteLater();
      }
      if (!ok) {
        loadedImage_ = QImage();
        imageCanvas_->clearImage();
        loadingLabel_->setText(tr("Failed to load image"));
        imageStack_->setCurrentWidget(loadingLabel_);
        if (currentItemChanged) {
          currentItemChanged(currentRow_, totalCount(), item, QSize());
        }
        updateControls();
        return;
      }

      loadedImage_ = image;
      imageCanvas_->setImage(image);
      imageStack_->setCurrentWidget(imageCanvas_);
      if (currentItemChanged) {
        currentItemChanged(currentRow_, totalCount(), item, image.size());
      }
      updateControls();
    });
  }

  void updateControls() {
    const int total = totalCount();
    const int current = currentRow_ + 1;

    QString format = countTextFormat_.trimmed();
    if (format.isEmpty()) {
      format = QStringLiteral("%1 / %2");
    }

    if (total > 1 && currentRow_ >= 0) {
      countLabel_->setText(format.arg(current).arg(total));
      countLabel_->show();
    } else {
      countLabel_->hide();
    }

    const bool hasImage = imageCanvas_ && imageCanvas_->hasImage();
    const bool canNavigate = total > 1;
    prevButton_->setEnabled(canNavigate && nextOpenableEntryRow(-1) >= 0);
    nextButton_->setEnabled(canNavigate && nextOpenableEntryRow(1) >= 0);
    prevButton_->setVisible(canNavigate);
    nextButton_->setVisible(canNavigate);

    const qreal scale = imageCanvas_ ? imageCanvas_->scale() : 1.0;
    zoomOutButton_->setEnabled(hasImage && scale > 1.001);
    zoomInButton_->setEnabled(hasImage && scale < 49.999);
    rotateLeftButton_->setEnabled(hasImage);
    rotateRightButton_->setEnabled(hasImage);
    flipXButton_->setEnabled(hasImage);
    flipYButton_->setEnabled(hasImage);

    updateOverlayLayout();
  }

  void applyVisualStyle() {
    QColor maskColor = visualStyle_.popupMask;
    if (!maskVisible_) {
      maskColor = QColor(Qt::transparent);
      maskLayer_->setVisible(false);
    } else {
      maskLayer_->setVisible(true);
    }

    maskLayer_->setBackgroundColor(maskColor);
    maskLayer_->setCornerRadius(0.0);

    auto applyLabelStyle = [](QLabel* label, const QColor& textColor, const QColor& disabledColor) {
      if (!label) {
        return;
      }

      QPalette palette = label->palette();
      palette.setColor(QPalette::WindowText, textColor);
      palette.setColor(QPalette::Disabled, QPalette::WindowText,
                       disabledColor.isValid() ? disabledColor : textColor);
      label->setPalette(palette);

      QFont labelFont = label->font();
      labelFont.setPixelSize(14);
      label->setFont(labelFont);
    };

    applyLabelStyle(loadingLabel_, visualStyle_.popupFooterText,
                    visualStyle_.operationDisabledColor);
    applyLabelStyle(countLabel_, visualStyle_.operationColor, visualStyle_.operationDisabledColor);

    auto applyPanelBackground = [](QWidget* widget, const QColor& color) {
      if (!widget) {
        return;
      }

      QPalette palette = widget->palette();
      palette.setColor(QPalette::Window, color);
      palette.setColor(QPalette::Base, color);
      widget->setPalette(palette);
      widget->setAutoFillBackground(color.isValid() && color.alpha() > 0);
    };

    applyPanelBackground(bodyHost_, visualStyle_.popupBodyBackground);
    applyPanelBackground(loadingLabel_, QColor(Qt::transparent));

    const int iconSize = std::max(10, visualStyle_.metrics.operationIconSize);
    const int basePadding = std::max(4, visualStyle_.metrics.footerPadding);
    const int closeSize =
        std::max(iconSize + basePadding * 2, visualStyle_.metrics.closeButtonSize);
    const int switchSize =
        std::max(iconSize + basePadding * 2, visualStyle_.metrics.switchButtonSize);
    const int actionSize = iconSize + basePadding * 2;
    const int actionsHorizontalPadding = std::max(0, visualStyle_.metrics.actionsHorizontalPadding);
    const int actionsGap = std::max(0, visualStyle_.metrics.actionsGap);
    const int footerGap = std::max(0, visualStyle_.metrics.footerGap);
    const int controlOffset = std::max(4, visualStyle_.metrics.controlOffset);
    const int footerBottomOffset = std::max(controlOffset, visualStyle_.metrics.footerBottomOffset);

    closeButtonSizePx_ = closeSize;
    switchButtonSizePx_ = switchSize;
    controlOffsetPx_ = controlOffset;
    footerBottomOffsetPx_ = footerBottomOffset;

    QColor operationBg = visualStyle_.popupActionsBackground;
    QColor operationBgHover = operationBg;
    if (operationBgHover.alpha() > 0) {
      operationBgHover.setAlpha(
          std::min(255, std::max(operationBg.alpha() + 24, operationBg.alpha() * 2)));
    }
    const QColor floatingColor = visualStyle_.popupFooterText;

    auto applyFloatingButtonStyle = [&](OverlayIconButton* button, int sizePx) {
      if (!button) {
        return;
      }
      button->setIconSize(QSize(iconSize, iconSize));
      button->setFixedSize(sizePx, sizePx);
      button->setVisualStyle(operationBg, operationBgHover, operationBg, QColor(Qt::transparent),
                             sizePx / 2.0);
    };

    auto applyActionStyle = [&](OverlayIconButton* button) {
      if (!button) {
        return;
      }
      button->setIconSize(QSize(iconSize, iconSize));
      button->setFixedSize(actionSize, actionSize);
      button->setVisualStyle(QColor(Qt::transparent), QColor(Qt::transparent),
                             QColor(Qt::transparent), QColor(Qt::transparent), actionSize / 2.0);
    };

    applyFloatingButtonStyle(closeButton_, closeSize);
    applyFloatingButtonStyle(prevButton_, switchSize);
    applyFloatingButtonStyle(nextButton_, switchSize);

    const QSize iconLogicalSize(iconSize, iconSize);
    applyButtonIconColorOverrides(closeButtonSpec_, iconLogicalSize, floatingColor, floatingColor,
                                  visualStyle_.operationDisabledColor);
    applyButtonIconColorOverrides(prevButtonSpec_, iconLogicalSize, floatingColor, floatingColor,
                                  visualStyle_.operationDisabledColor);
    applyButtonIconColorOverrides(nextButtonSpec_, iconLogicalSize, floatingColor, floatingColor,
                                  visualStyle_.operationDisabledColor);

    for (OverlayIconButton* button : actionButtons_) {
      applyActionStyle(button);
    }
    for (const ButtonIconSpec& spec : actionIconSpecs_) {
      applyButtonIconColorOverrides(spec, iconLogicalSize, visualStyle_.operationColor,
                                    visualStyle_.operationHoverColor,
                                    visualStyle_.operationDisabledColor);
    }

    actionsLayout_->setContentsMargins(actionsHorizontalPadding, 0, actionsHorizontalPadding, 0);
    actionsLayout_->setSpacing(actionsGap);
    footerLayout_->setSpacing(footerGap);

    actions_->setBackgroundColor(operationBg);
    actions_->setCornerRadius(100.0);

    updateControls();
    updateOverlayLayout();
  }

  void updateOverlayLayout() {
    if (!container_) {
      return;
    }

    const QRect bounds = container_->rect();
    if (bounds.isEmpty()) {
      return;
    }

    if (maskLayer_) {
      maskLayer_->setGeometry(bounds);
      maskLayer_->lower();
    }

    if (footer_) {
      footer_->adjustSize();
      const QSize footerSize = footer_->sizeHint();
      footer_->setFixedSize(footerSize);
      const int footerX = (bounds.width() - footerSize.width()) / 2;
      const int footerY = bounds.height() - footerBottomOffsetPx_ - footerSize.height();
      footer_->move(std::max(0, footerX), std::max(0, footerY));
    }

    if (closeButton_) {
      const int closeX = bounds.width() - controlOffsetPx_ - closeButtonSizePx_;
      closeButton_->move(std::max(0, closeX), controlOffsetPx_);
    }

    const int switchY = (bounds.height() - switchButtonSizePx_) / 2;
    if (prevButton_) {
      prevButton_->move(controlOffsetPx_, std::max(0, switchY));
    }
    if (nextButton_) {
      const int nextX = bounds.width() - controlOffsetPx_ - switchButtonSizePx_;
      nextButton_->move(std::max(0, nextX), std::max(0, switchY));
    }

    if (bodyHost_) {
      bodyHost_->setGeometry(bounds);
      bodyHost_->raise();
    }
    if (footer_) {
      footer_->raise();
    }
    if (closeButton_) {
      closeButton_->raise();
    }
    if (prevButton_) {
      prevButton_->raise();
    }
    if (nextButton_) {
      nextButton_->raise();
    }
  }

  QVector<PreviewDialogEntry> entries_;
  int currentRow_ = -1;
  int loadToken_ = 0;
  QString countTextFormat_ = QStringLiteral("%1 / %2");
  qreal scaleStep_ = 0.5;
  bool maskVisible_ = true;
  int closeButtonSizePx_ = 42;
  int switchButtonSizePx_ = 42;
  int controlOffsetPx_ = 12;
  int footerBottomOffsetPx_ = 32;
  QImage loadedImage_;
  detail::ImageVisualStyle visualStyle_;
  QPointer<QWidget> hostWindow_;
  QPointer<AdImageReply> loadReply_;

  QWidget* container_ = nullptr;
  RoundedBackgroundWidget* maskLayer_ = nullptr;
  QWidget* bodyHost_ = nullptr;
  QLabel* countLabel_ = nullptr;
  QLabel* loadingLabel_ = nullptr;
  ImagePreviewCanvas* imageCanvas_ = nullptr;
  QStackedLayout* imageStack_ = nullptr;
  QWidget* footer_ = nullptr;
  QVBoxLayout* footerLayout_ = nullptr;
  RoundedBackgroundWidget* actions_ = nullptr;
  QHBoxLayout* actionsLayout_ = nullptr;
  OverlayIconButton* closeButton_ = nullptr;
  OverlayIconButton* prevButton_ = nullptr;
  OverlayIconButton* nextButton_ = nullptr;
  OverlayIconButton* zoomOutButton_ = nullptr;
  OverlayIconButton* zoomInButton_ = nullptr;
  OverlayIconButton* rotateLeftButton_ = nullptr;
  OverlayIconButton* rotateRightButton_ = nullptr;
  OverlayIconButton* flipXButton_ = nullptr;
  OverlayIconButton* flipYButton_ = nullptr;
  ButtonIconSpec closeButtonSpec_;
  ButtonIconSpec prevButtonSpec_;
  ButtonIconSpec nextButtonSpec_;
  QVector<ButtonIconSpec> actionIconSpecs_;
  QList<OverlayIconButton*> actionButtons_;
};

ImagePreviewDialog* asPreviewDialog(const QPointer<QWidget>& dialog) {
  return dynamic_cast<ImagePreviewDialog*>(dialog.data());
}

QVector<PreviewDialogEntry> buildPreviewDialogEntries(const QAbstractItemModel* model,
                                                      AdImageLoader* loader) {
  QVector<PreviewDialogEntry> entries;
  if (!model) {
    return entries;
  }

  const int total = model->rowCount();
  entries.reserve(total);
  for (int row = 0; row < total; ++row) {
    PreviewDialogEntry entry;
    entry.item = readItemFromModel(model, row);
    entry.loader = loader;
    entries.append(entry);
  }

  return entries;
}

int firstOpenableRow(const AdImageViewer* viewer, int preferredRow) {
  if (!viewer) {
    return -1;
  }

  const int count = viewer->rowCount();
  if (count <= 0) {
    return -1;
  }

  if (preferredRow >= 0 && preferredRow < count && viewer->canOpenRow(preferredRow)) {
    return preferredRow;
  }

  for (int row = 0; row < count; ++row) {
    if (viewer->canOpenRow(row)) {
      return row;
    }
  }

  return -1;
}

}  // namespace

AdImageReply::AdImageReply(QObject* parent) : QObject(parent) {}

AdImageReply::~AdImageReply() = default;

bool AdImageReply::isFinished() const { return finished_; }

bool AdImageReply::isSuccessful() const { return successful_; }

QImage AdImageReply::image() const { return image_; }

QSize AdImageReply::naturalSize() const { return naturalSize_; }

QString AdImageReply::errorString() const { return errorString_; }

void AdImageReply::succeed(const QImage& image, const QSize& naturalSize) {
  if (finished_) {
    return;
  }

  finished_ = true;
  successful_ = true;
  image_ = image;
  naturalSize_ = naturalSize.isValid() ? naturalSize : image.size();
  errorString_.clear();
  emit finished();
}

void AdImageReply::fail(const QString& errorString) {
  if (finished_) {
    return;
  }

  finished_ = true;
  successful_ = false;
  image_ = QImage();
  naturalSize_ = QSize();
  errorString_ = errorString;
  emit finished();
}

AdImageLoader::AdImageLoader(QObject* parent) : QObject(parent) {}

AdImageLoader::~AdImageLoader() = default;

AdImageLoader* defaultAdImageLoader() { return defaultImageLoaderInstance(); }

AdImageReply* AdImageLoader::load(const QUrl& source, const AdImageLoadOptions& options,
                                  QObject* parent) {
  Q_UNUSED(options);
  return load(source, parent);
}

bool operator==(const AdImageLoadOptions& lhs, const AdImageLoadOptions& rhs) {
  return lhs.targetPixelSize == rhs.targetPixelSize && lhs.aspectRatioMode == rhs.aspectRatioMode &&
         lhs.allowUpscale == rhs.allowUpscale;
}

bool operator!=(const AdImageLoadOptions& lhs, const AdImageLoadOptions& rhs) {
  return !(lhs == rhs);
}

bool AdImageItem::isValid() const {
  return !source.isEmpty() && !sourceToString(source).trimmed().isEmpty();
}

bool operator==(const AdImageItem& lhs, const AdImageItem& rhs) {
  return lhs.source == rhs.source && lhs.altText == rhs.altText;
}

bool operator!=(const AdImageItem& lhs, const AdImageItem& rhs) { return !(lhs == rhs); }

AdImageListModel::AdImageListModel(QObject* parent) : QAbstractListModel(parent) {}

AdImageListModel::~AdImageListModel() = default;

int AdImageListModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid()) {
    return 0;
  }
  return static_cast<int>(items_.size());
}

QVariant AdImageListModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= items_.size()) {
    return QVariant();
  }

  const AdImageItem& item = items_.at(index.row());
  switch (role) {
    case Qt::DisplayRole:
    case AdImageItemRoles::AltTextRole:
      return item.altText;
    case AdImageItemRoles::SourceRole:
      return QVariant::fromValue(item.source);
    default:
      break;
  }

  return QVariant();
}

QHash<int, QByteArray> AdImageListModel::roleNames() const {
  QHash<int, QByteArray> roles = QAbstractListModel::roleNames();
  roles.insert(AdImageItemRoles::SourceRole, QByteArrayLiteral("source"));
  roles.insert(AdImageItemRoles::AltTextRole, QByteArrayLiteral("altText"));
  return roles;
}

AdImageItems AdImageListModel::items() const { return items_; }

void AdImageListModel::setItems(const AdImageItems& value) {
  if (items_ == value) {
    return;
  }

  beginResetModel();
  items_ = value;
  endResetModel();
  emit itemsChanged(items_);
}

AdImageItem AdImageListModel::itemAt(int row) const {
  if (row < 0 || row >= items_.size()) {
    return AdImageItem{};
  }
  return items_.at(row);
}

AdImageViewer::AdImageViewer(QObject* parent) : QObject(parent) {
  QObject::connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged,
                   this, [this]() { applyPreviewSurfaceVisuals(); });
}

AdImageViewer::~AdImageViewer() {
  disconnectModelSignals();
  if (previewSurface_) {
    previewSurface_->close();
    previewSurface_->deleteLater();
  }
}

bool AdImageViewer::isVisible() const { return visible_; }

void AdImageViewer::setVisible(bool value) {
  if (!value) {
    close();
    return;
  }

  openAt(currentRow_);
}

int AdImageViewer::currentRow() const { return currentRow_; }

void AdImageViewer::setCurrentRow(int value) {
  const int count = rowCount();
  const int next = count <= 0 ? -1 : std::clamp(value, 0, count - 1);
  if (currentRow_ == next) {
    return;
  }

  currentRow_ = next;
  emit currentRowChanged(currentRow_);

  if (currentRow_ < 0) {
    close();
    return;
  }

  if (ImagePreviewDialog* dialog = asPreviewDialog(previewSurface_)) {
    dialog->setCurrentRow(currentRow_, false);
  }

  applyPreviewSurfaceVisuals();
}

QString AdImageViewer::countTextFormat() const { return countTextFormat_; }

void AdImageViewer::setCountTextFormat(const QString& value) {
  if (countTextFormat_ == value) {
    return;
  }

  countTextFormat_ = value;
  emit countTextFormatChanged(countTextFormat_);
  applyPreviewSurfaceVisuals();
}

bool AdImageViewer::maskVisible() const { return maskVisible_; }

void AdImageViewer::setMaskVisible(bool value) {
  if (maskVisible_ == value) {
    return;
  }

  maskVisible_ = value;
  emit maskVisibleChanged(maskVisible_);
  applyPreviewSurfaceVisuals();
}

double AdImageViewer::scaleStep() const { return scaleStep_; }

void AdImageViewer::setScaleStep(double value) {
  const double clamped = std::clamp(value, 0.05, 10.0);
  if (qFuzzyCompare(scaleStep_ + 1.0, clamped + 1.0)) {
    return;
  }

  scaleStep_ = clamped;
  emit scaleStepChanged(scaleStep_);
  applyPreviewSurfaceVisuals();
}

QWidget* AdImageViewer::ownerWindow() const { return ownerWindow_; }

void AdImageViewer::setOwnerWindow(QWidget* value) {
  if (ownerWindow_ == value) {
    return;
  }

  ownerWindow_ = value;
  emit ownerWindowChanged(ownerWindow_);

  if (ImagePreviewDialog* dialog = asPreviewDialog(previewSurface_)) {
    dialog->openFor(resolvedOwnerWindow());
  }
}

QAbstractItemModel* AdImageViewer::model() const { return model_; }

void AdImageViewer::setModel(QAbstractItemModel* value) {
  if (model_ == value) {
    return;
  }

  disconnectModelSignals();
  model_ = value;
  emit modelChanged(model_);

  if (model_) {
    modelConnections_.append(QObject::connect(model_, &QAbstractItemModel::modelReset, this,
                                              [this]() { handleModelReset(); }));
    modelConnections_.append(
        QObject::connect(model_, &QAbstractItemModel::rowsInserted, this,
                         [this](const QModelIndex&, int, int) { handleModelReset(); }));
    modelConnections_.append(
        QObject::connect(model_, &QAbstractItemModel::rowsRemoved, this,
                         [this](const QModelIndex&, int, int) { handleModelReset(); }));
    modelConnections_.append(QObject::connect(
        model_, &QAbstractItemModel::dataChanged, this,
        [this](const QModelIndex&, const QModelIndex&, const QList<int>&) { handleModelReset(); }));
    modelConnections_.append(QObject::connect(model_, &QAbstractItemModel::layoutChanged, this,
                                              [this]() { handleModelReset(); }));
    modelConnections_.append(QObject::connect(model_, &QObject::destroyed, this, [this]() {
      disconnectModelSignals();
      model_ = nullptr;
      emit modelChanged(nullptr);
      handleModelReset();
    }));
  }

  handleModelReset();
}

AdImageLoader* AdImageViewer::imageLoader() const { return imageLoader_; }

void AdImageViewer::setImageLoader(AdImageLoader* value) {
  if (imageLoader_ == value) {
    return;
  }

  imageLoader_ = value;
  emit imageLoaderChanged(imageLoader_);

  syncPreviewSurfaceEntries();
  applyPreviewSurfaceVisuals();
}

int AdImageViewer::rowCount() const { return model_ ? model_->rowCount() : 0; }

AdImageItem AdImageViewer::itemAt(int row) const { return readItemFromModel(model_, row); }

bool AdImageViewer::canOpenRow(int row) const {
  if (row < 0 || row >= rowCount()) {
    return false;
  }
  return itemAt(row).isValid();
}

AdImageViewer::ComponentTokens AdImageViewer::componentTokens() const { return componentTokens_; }

void AdImageViewer::setComponentTokens(const ComponentTokens& tokens) {
  componentTokens_ = tokens;
  emit componentTokensChanged();
  applyPreviewSurfaceVisuals();
}

void AdImageViewer::resetComponentTokens() {
  componentTokens_ = ComponentTokens{};
  emit componentTokensChanged();
  applyPreviewSurfaceVisuals();
}

AdImageViewer::SemanticStyles AdImageViewer::semanticStyles() const { return semanticStyles_; }

void AdImageViewer::setSemanticStyles(const SemanticStyles& styles) {
  semanticStyles_ = styles;
  emit semanticStylesChanged();
  applyPreviewSurfaceVisuals();
}

void AdImageViewer::setSemanticStyleResolver(SemanticStyleResolver resolver) {
  semanticStyleResolver_ = std::move(resolver);
  emit semanticStylesChanged();
  applyPreviewSurfaceVisuals();
}

void AdImageViewer::openAt(int row) { openAtFromWidget(row, contextWidget_); }

void AdImageViewer::close() {
  if (ImagePreviewDialog* dialog = asPreviewDialog(previewSurface_)) {
    dialog->close();
    return;
  }

  handlePreviewVisibilityChanged(false);
}

void AdImageViewer::activate(int delta) {
  if (ImagePreviewDialog* dialog = asPreviewDialog(previewSurface_)) {
    dialog->activate(delta);
  }
}

void AdImageViewer::zoomIn() {
  if (ImagePreviewDialog* dialog = asPreviewDialog(previewSurface_)) {
    dialog->zoomIn();
  }
}

void AdImageViewer::zoomOut() {
  if (ImagePreviewDialog* dialog = asPreviewDialog(previewSurface_)) {
    dialog->zoomOut();
  }
}

void AdImageViewer::rotateLeft() {
  if (ImagePreviewDialog* dialog = asPreviewDialog(previewSurface_)) {
    dialog->rotateLeft();
  }
}

void AdImageViewer::rotateRight() {
  if (ImagePreviewDialog* dialog = asPreviewDialog(previewSurface_)) {
    dialog->rotateRight();
  }
}

void AdImageViewer::flipX() {
  if (ImagePreviewDialog* dialog = asPreviewDialog(previewSurface_)) {
    dialog->flipX();
  }
}

void AdImageViewer::flipY() {
  if (ImagePreviewDialog* dialog = asPreviewDialog(previewSurface_)) {
    dialog->flipY();
  }
}

void AdImageViewer::resetTransform() {
  if (ImagePreviewDialog* dialog = asPreviewDialog(previewSurface_)) {
    dialog->resetTransform();
  }
}

void AdImageViewer::openAtFromWidget(int row, QWidget* widget) {
  const int targetRow = firstOpenableRow(this, row);
  if (targetRow < 0) {
    return;
  }

  if (widget) {
    contextWidget_ = widget;
  }

  const int previousRow = currentRow_;
  currentRow_ = targetRow;
  if (previousRow != currentRow_) {
    emit currentRowChanged(currentRow_);
  }

  ensurePreviewSurface();
  if (ImagePreviewDialog* dialog = asPreviewDialog(previewSurface_)) {
    dialog->setEntries(buildPreviewDialogEntries(model_, imageLoader_), currentRow_);
    applyPreviewSurfaceVisuals();
    QWidget* owner = widget ? widget : resolvedOwnerWindow();
    dialog->openFor(owner);
  }
}

void AdImageViewer::handlePreviewVisibilityChanged(bool visible) {
  if (visible_ == visible) {
    return;
  }

  visible_ = visible;
  emit visibleChanged(visible_);
  applyPreviewSurfaceVisuals();
}

void AdImageViewer::handlePreviewCurrentRowChanged(int row) {
  if (currentRow_ == row) {
    return;
  }

  currentRow_ = row;
  emit currentRowChanged(currentRow_);
  applyPreviewSurfaceVisuals();
}

void AdImageViewer::handlePreviewCurrentItemChanged(int currentRow, int totalCount,
                                                    const AdImageItem& item,
                                                    const QSize& actualSize) {
  emit currentItemChanged(currentRow, totalCount, item, actualSize);
}

void AdImageViewer::handleModelReset() {
  const int nextRow = firstOpenableRow(this, currentRow_);
  if (currentRow_ != nextRow) {
    currentRow_ = nextRow;
    emit currentRowChanged(currentRow_);
  }

  syncPreviewSurfaceEntries();
  if (currentRow_ < 0 && visible_) {
    close();
    return;
  }

  if (currentRow_ >= 0) {
    if (ImagePreviewDialog* dialog = asPreviewDialog(previewSurface_)) {
      dialog->setCurrentRow(currentRow_, false);
    }
  }

  applyPreviewSurfaceVisuals();
}

void AdImageViewer::ensurePreviewSurface() {
  if (previewSurface_) {
    return;
  }

  auto* dialog = new ImagePreviewDialog();
  previewSurface_ = dialog;
  dialog->visibilityChanged = [this](bool visible) { handlePreviewVisibilityChanged(visible); };
  dialog->currentRowChanged = [this](int row) { handlePreviewCurrentRowChanged(row); };
  dialog->currentItemChanged = [this](int currentRow, int totalCount, const AdImageItem& item,
                                      const QSize& actualSize) {
    handlePreviewCurrentItemChanged(currentRow, totalCount, item, actualSize);
  };
}

void AdImageViewer::applyPreviewSurfaceVisuals() {
  ImagePreviewDialog* dialog = asPreviewDialog(previewSurface_);
  if (!dialog) {
    return;
  }

  SemanticStyles mergedSemantic = semanticStyles_;
  if (semanticStyleResolver_) {
    StyleContext context;
    context.visible = visible_;
    context.maskVisible = maskVisible_;
    context.currentRow = currentRow_;
    context.totalCount = rowCount();
    mergedSemantic = mergedViewerSemanticStyles(mergedSemantic, semanticStyleResolver_(context));
  }

  detail::ImageViewerStyleInput styleInput;
  styleInput.componentTokens = componentTokens_;
  styleInput.semanticStyles = mergedSemantic;
  styleInput.maskVisible = maskVisible_;

  const QWidget* logicalOwner = themeSourceWidget();
  const adqt::theme::ResolvedTheme resolvedTheme =
      logicalOwner ? adqt::theme::ThemeManager::instance().resolve(logicalOwner)
                   : adqt::theme::makeResolvedTheme(adqt::theme::makeTheme());

  dialog->setVisualStyle(detail::resolveImageViewerVisualStyle(styleInput, resolvedTheme));
  dialog->setCountTextFormat(countTextFormat_);
  dialog->setMaskVisible(maskVisible_);
  dialog->setScaleStep(scaleStep_);
}

void AdImageViewer::syncPreviewSurfaceEntries() {
  ImagePreviewDialog* dialog = asPreviewDialog(previewSurface_);
  if (!dialog) {
    return;
  }

  dialog->setEntries(buildPreviewDialogEntries(model_, imageLoader_), currentRow_);
}

QWidget* AdImageViewer::resolvedOwnerWindow() const {
  if (ownerWindow_) {
    return ownerWindow_->window();
  }
  if (contextWidget_) {
    return contextWidget_->window();
  }
  return qobject_cast<QWidget*>(qApp->activeWindow());
}

QWidget* AdImageViewer::themeSourceWidget() const {
  if (contextWidget_) {
    return contextWidget_;
  }
  if (ownerWindow_) {
    return ownerWindow_;
  }
  return resolvedOwnerWindow();
}

void AdImageViewer::disconnectModelSignals() {
  for (const QMetaObject::Connection& connection : modelConnections_) {
    QObject::disconnect(connection);
  }
  modelConnections_.clear();
}

AdImage::AdImage(QWidget* parent) : QWidget(parent) {
  setAttribute(Qt::WA_Hover, true);
  setMouseTracking(true);
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  QObject::connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged,
                   this, [this]() { update(); });
  syncInteractiveState();
  updateAccessibleText();
}

AdImage::~AdImage() {
  cancelMainLoads();
  cancelImageReply(placeholderReply_);
  disconnectViewerSignals();
  if (internalViewer_) {
    internalViewer_->close();
  }
}

QUrl AdImage::source() const { return source_; }

void AdImage::setSource(const QUrl& value) {
  if (source_ == value) {
    return;
  }

  source_ = value;
  emit sourceChanged(source_);
  reloadMainImage();
  syncInternalViewerData();
  syncInteractiveState();
  updateAccessibleText();
  update();
}

QString AdImage::altText() const { return altText_; }

void AdImage::setAltText(const QString& value) {
  if (altText_ == value) {
    return;
  }

  altText_ = value;
  emit altTextChanged(altText_);
  updateAccessibleText();
  syncInternalViewerData();
  update();
}

QUrl AdImage::fallbackSource() const { return fallbackSource_; }

void AdImage::setFallbackSource(const QUrl& value) {
  if (fallbackSource_ == value) {
    return;
  }

  fallbackSource_ = value;
  emit fallbackSourceChanged(fallbackSource_);
  if (loadFailed_ || imagePixmap_.isNull()) {
    reloadMainImage();
  }
}

QUrl AdImage::placeholderSource() const { return placeholderSource_; }

void AdImage::setPlaceholderSource(const QUrl& value) {
  if (placeholderSource_ == value) {
    return;
  }

  placeholderSource_ = value;
  emit placeholderSourceChanged(placeholderSource_);
  reloadPlaceholderImage();
}

QUrl AdImage::previewSource() const { return previewSource_; }

void AdImage::setPreviewSource(const QUrl& value) {
  if (previewSource_ == value) {
    return;
  }

  previewSource_ = value;
  emit previewSourceChanged(previewSource_);
  syncInternalViewerData();
  syncInteractiveState();
  updateAccessibleText();
  update();
}

AdImageItems AdImage::previewItems() const { return previewItems_; }

void AdImage::setPreviewItems(const AdImageItems& value) {
  if (previewItems_ == value) {
    return;
  }

  previewItems_ = value;
  emit previewItemsChanged(previewItems_);
  syncInternalViewerData();
  syncInteractiveState();
  updateAccessibleText();
  update();
}

bool AdImage::previewEnabled() const { return previewEnabled_; }

void AdImage::setPreviewEnabled(bool value) {
  if (previewEnabled_ == value) {
    return;
  }

  const bool wasVisible = previewVisibleForThisImage();
  previewEnabled_ = value;
  emit previewEnabledChanged(previewEnabled_);

  if (!previewEnabled_ && wasVisible) {
    if (viewer_) {
      viewer_->close();
    } else if (internalViewer_) {
      internalViewer_->close();
    }
  }

  syncInteractiveState();
  updateAccessibleText();
  update();
}

QString AdImage::previewText() const {
  return previewText_.trimmed().isEmpty() ? tr("Preview") : previewText_;
}

void AdImage::setPreviewText(const QString& value) {
  if (previewText_ == value) {
    return;
  }

  previewText_ = value;
  emit previewTextChanged(previewText());
  update();
}

QSize AdImage::preferredImageSize() const { return preferredImageSize_; }

void AdImage::setPreferredImageSize(const QSize& value) {
  if (preferredImageSize_ == value) {
    return;
  }

  preferredImageSize_ = value;
  emit preferredImageSizeChanged(preferredImageSize_);
  updateGeometry();
  update();
}

bool AdImage::loading() const { return loading_; }

bool AdImage::loadFailed() const { return loadFailed_; }

int AdImage::previewRow() const { return previewRow_; }

void AdImage::setPreviewRow(int value) {
  if (previewRow_ == value) {
    return;
  }

  previewRow_ = value;
  emit previewRowChanged(previewRow_);
  syncInteractiveState();
  updateAccessibleText();
  update();
}

AdImageViewer* AdImage::viewer() const { return viewer_; }

void AdImage::setViewer(AdImageViewer* value) {
  if (viewer_ == value) {
    return;
  }

  disconnectViewerSignals();
  viewer_ = value;
  emit viewerChanged(viewer_);

  if (viewer_ && internalViewer_ && internalViewer_->isVisible()) {
    internalViewer_->close();
  }

  bindEffectiveViewerSignals();
  syncInteractiveState();
  updateAccessibleText();
  update();
}

AdImageViewer* AdImage::ensureViewer() {
  if (!internalViewer_) {
    internalViewer_ = new AdImageViewer(this);
    ensureInternalViewerModel();
    internalViewer_->setModel(internalViewerModel_);
    internalViewer_->setImageLoader(imageLoader_);
    syncInternalViewerData();
  }

  if (!viewer_) {
    bindEffectiveViewerSignals();
  }

  return internalViewer_;
}

AdImageLoader* AdImage::imageLoader() const { return imageLoader_; }

void AdImage::setImageLoader(AdImageLoader* value) {
  if (imageLoader_ == value) {
    return;
  }

  imageLoader_ = value;
  emit imageLoaderChanged(imageLoader_);
  reloadMainImage();
  reloadPlaceholderImage();

  if (internalViewer_) {
    internalViewer_->setImageLoader(imageLoader_);
  }

  syncInternalViewerData();
}

AdImage::LoadingPolicy AdImage::loadingPolicy() const { return loadingPolicy_; }

void AdImage::setLoadingPolicy(LoadingPolicy value) {
  if (loadingPolicy_ == value) {
    return;
  }
  loadingPolicy_ = value;
  emit loadingPolicyChanged(loadingPolicy_);
  reloadMainImage();
}

AdImage::DecodePolicy AdImage::decodePolicy() const { return decodePolicy_; }

void AdImage::setDecodePolicy(DecodePolicy value) {
  if (decodePolicy_ == value) {
    return;
  }
  decodePolicy_ = value;
  emit decodePolicyChanged(decodePolicy_);
  reloadMainImage();
}

AdImage::ComponentTokens AdImage::componentTokens() const { return componentTokens_; }

void AdImage::setComponentTokens(const ComponentTokens& tokens) {
  componentTokens_ = tokens;
  emit componentTokensChanged();
  update();
}

void AdImage::resetComponentTokens() {
  componentTokens_ = ComponentTokens{};
  emit componentTokensChanged();
  update();
}

AdImage::SemanticStyles AdImage::semanticStyles() const { return semanticStyles_; }

void AdImage::setSemanticStyles(const SemanticStyles& styles) {
  semanticStyles_ = styles;
  emit semanticStylesChanged();
  update();
}

void AdImage::setSemanticStyleResolver(SemanticStyleResolver resolver) {
  semanticStyleResolver_ = std::move(resolver);
  emit semanticStylesChanged();
  update();
}

QSize AdImage::sizeHint() const {
  const int targetWidth = preferredImageSize_.width() > 0 ? preferredImageSize_.width() : -1;
  const int targetHeight = preferredImageSize_.height() > 0 ? preferredImageSize_.height() : -1;

  if (targetWidth > 0 && targetHeight > 0) {
    return QSize(targetWidth, targetHeight);
  }

  QPixmap sourcePixmap = imagePixmap_;
  if (sourcePixmap.isNull() && !placeholderPixmap_.isNull()) {
    sourcePixmap = placeholderPixmap_;
  }

  if (!sourcePixmap.isNull()) {
    const QSize natural = naturalMainPixelSize_.isValid() ? naturalMainPixelSize_
                                                          : pixmapActualSize(sourcePixmap);
    if (targetWidth > 0 && natural.width() > 0) {
      const int h = std::max(1, static_cast<int>(std::round(targetWidth * natural.height() /
                                                            std::max(1, natural.width()))));
      return QSize(targetWidth, h);
    }
    if (targetHeight > 0 && natural.height() > 0) {
      const int w = std::max(1, static_cast<int>(std::round(targetHeight * natural.width() /
                                                            std::max(1, natural.height()))));
      return QSize(w, targetHeight);
    }
    return natural;
  }

  if (targetWidth > 0) {
    return QSize(targetWidth, std::max(80, targetWidth * 3 / 4));
  }
  if (targetHeight > 0) {
    return QSize(std::max(100, targetHeight * 4 / 3), targetHeight);
  }

  return QSize(160, 120);
}

QSize AdImage::minimumSizeHint() const { return QSize(40, 30); }

void AdImage::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event);

  requestMainImageIfNeeded();

  StyleContext context;
  context.hovered = hovered_;
  context.loading = loading_;
  context.failed = loadFailed_;
  context.previewEnabled = previewEnabled_;
  context.previewable = canOpenPreview();
  context.previewVisible = previewVisibleForThisImage();
  context.focused = hasFocus();

  SemanticStyles mergedSemantic = semanticStyles_;
  if (semanticStyleResolver_) {
    mergedSemantic = mergedImageSemanticStyles(mergedSemantic, semanticStyleResolver_(context));
  }

  detail::ImageStyleInput styleInput;
  styleInput.componentTokens = componentTokens_;
  styleInput.semanticStyles = mergedSemantic;
  const adqt::theme::ResolvedTheme resolvedTheme =
      adqt::theme::ThemeManager::instance().resolve(this);
  const detail::ImageVisualStyle visual =
      detail::resolveImageVisualStyle(styleInput, resolvedTheme);

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

  const QRect drawRect = rect().adjusted(0, 0, -1, -1);
  const qreal radius = std::max(0, visual.metrics.borderRadius);
  QPainterPath clipPath;
  clipPath.addRoundedRect(drawRect, radius, radius);
  painter.setClipPath(clipPath);

  if (visual.rootBackground.alpha() > 0) {
    painter.fillRect(drawRect, visual.rootBackground);
  }

  QPixmap displayPixmap = imagePixmap_;
  if (displayPixmap.isNull() && loading_ && !placeholderPixmap_.isNull()) {
    displayPixmap = placeholderPixmap_;
  }

  if (!displayPixmap.isNull()) {
    const QSize scaled =
        pixmapActualSize(displayPixmap).scaled(drawRect.size(), Qt::KeepAspectRatio);
    const QPoint topLeft(drawRect.center().x() - scaled.width() / 2,
                         drawRect.center().y() - scaled.height() / 2);
    painter.drawPixmap(QRect(topLeft, scaled), displayPixmap,
                       QRect(QPoint(0, 0), pixmapActualSize(displayPixmap)));
  } else {
    painter.fillRect(drawRect, visual.placeholderBackground);

    const int iconSize = std::max(14, std::min(drawRect.width(), drawRect.height()) / 4);
    const auto iconColors = adqt::icons::IconColors::primary(visual.placeholderIcon);
    const QPixmap iconPixmap = adqt::icons::renderIconPixmap(
        outlined_icons::Picture(iconColors), {QSize(iconSize, iconSize), devicePixelRatioF()});
    const QPoint iconPos(drawRect.center().x() - iconSize / 2,
                         drawRect.center().y() - iconSize / 2 - 8);
    painter.drawPixmap(iconPos, iconPixmap);

    painter.setPen(visual.placeholderIcon);
    const QString text =
        loadFailed_ ? tr("Failed to load image") : (loading_ ? tr("Loading...") : tr("No image"));
    const QRect textRect(drawRect.left() + 8, drawRect.center().y() + iconSize / 2,
                         drawRect.width() - 16, 30);
    painter.drawText(textRect, Qt::AlignHCenter | Qt::AlignTop, text);
  }

  if (context.previewable && (hovered_ || context.previewVisible)) {
    painter.fillRect(drawRect, visual.coverBackground);
    painter.setPen(visual.coverText);

    const int iconSize = std::max(12, std::min(18, drawRect.height() / 6));
    const auto iconColors = adqt::icons::IconColors::primary(visual.coverText);
    const QPixmap iconPixmap = adqt::icons::renderIconPixmap(
        outlined_icons::ZoomIn(iconColors), {QSize(iconSize, iconSize), devicePixelRatioF()});

    const QString coverText = previewText();

    const QFontMetrics metrics(font());
    const int textWidth = metrics.horizontalAdvance(coverText);
    const int contentWidth = iconSize + 6 + textWidth;
    const int startX = drawRect.center().x() - contentWidth / 2;
    const int iconY = drawRect.center().y() - iconSize / 2;
    painter.drawPixmap(QPoint(startX, iconY), iconPixmap);
    const QRect textRect(startX + iconSize + 6, drawRect.top(), textWidth + 4, drawRect.height());
    painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, coverText);
  }

  painter.setClipping(false);
  if (visual.rootBorder.alpha() > 0) {
    QPen pen(visual.rootBorder);
    pen.setWidthF(1.0);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(drawRect, radius, radius);
  }

  if (context.previewable && context.focused && visual.focusRing.alpha() > 0) {
    const qreal ringWidth = std::max<qreal>(1.0, visual.metrics.focusRingWidth);
    QRectF ringRect = drawRect;
    ringRect.adjust(ringWidth * 0.5, ringWidth * 0.5, -ringWidth * 0.5, -ringWidth * 0.5);
    QPen ringPen(visual.focusRing);
    ringPen.setWidthF(ringWidth);
    painter.setPen(ringPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(ringRect, radius, radius);
  }
}

void AdImage::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (!event) {
    return;
  }

  switch (event->type()) {
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange:
    case QEvent::StyleChange:
    case QEvent::LanguageChange:
      if (previewText_.trimmed().isEmpty()) {
        emit previewTextChanged(previewText());
      }
      update();
      break;
    case QEvent::DevicePixelRatioChange:
      if (decodePolicy_ == DecodePolicy::FitWidget) {
        mainLoadPending_ = mainImageNeedsLargerDecode();
        update();
      }
      break;
    default:
      break;
  }
}

void AdImage::enterEvent(QEnterEvent* event) {
  QWidget::enterEvent(event);
  hovered_ = true;
  update();
}

void AdImage::leaveEvent(QEvent* event) {
  QWidget::leaveEvent(event);
  hovered_ = false;
  update();
}

void AdImage::mouseReleaseEvent(QMouseEvent* event) {
  QWidget::mouseReleaseEvent(event);
  if (!event || event->button() != Qt::LeftButton) {
    return;
  }
  if (!rect().contains(event->position().toPoint())) {
    return;
  }
  activatePreviewFromUser();
}

void AdImage::keyPressEvent(QKeyEvent* event) {
  if (!event) {
    QWidget::keyPressEvent(event);
    return;
  }

  switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
      if (canOpenPreview()) {
        activatePreviewFromUser();
        event->accept();
        return;
      }
      break;
    default:
      break;
  }

  QWidget::keyPressEvent(event);
}

void AdImage::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  if (decodePolicy_ == DecodePolicy::FitWidget && mainImageNeedsLargerDecode()) {
    mainLoadPending_ = true;
  }
  update();
}

void AdImage::hideEvent(QHideEvent* event) {
  QWidget::hideEvent(event);
  if (loadingPolicy_ == LoadingPolicy::WhenVisible && (mainReply_ || fallbackReply_)) {
    ++mainLoadToken_;
    cancelMainLoads();
    mainLoadPending_ = imagePixmap_.isNull();
    setLoadingState(false);
  }
}

void AdImage::focusInEvent(QFocusEvent* event) {
  QWidget::focusInEvent(event);
  update();
}

void AdImage::focusOutEvent(QFocusEvent* event) {
  QWidget::focusOutEvent(event);
  update();
}

AdImageItem AdImage::effectivePreviewItem() const {
  AdImageItem item;
  item.source = previewSource_.isEmpty() ? source_ : previewSource_;
  item.altText = altText_;
  return item;
}

AdImageItems AdImage::effectiveInternalViewerItems() const {
  const AdImageItem baseItem = effectivePreviewItem();
  if (!previewItems_.isEmpty()) {
    AdImageItems items;
    items.reserve(previewItems_.size());
    for (AdImageItem item : previewItems_) {
      if (!item.isValid()) {
        continue;
      }
      if (item.altText.trimmed().isEmpty()) {
        item.altText = baseItem.altText;
      }
      items.append(item);
    }
    return items;
  }

  if (baseItem.isValid()) {
    return AdImageItems{baseItem};
  }

  return AdImageItems{};
}

bool AdImage::canOpenPreview() const {
  if (!previewEnabled_) {
    return false;
  }

  if (viewer_) {
    return viewer_->canOpenRow(effectivePreviewRow());
  }

  return !effectiveInternalViewerItems().isEmpty();
}

bool AdImage::previewVisibleForThisImage() const {
  if (!previewEnabled_) {
    return false;
  }

  if (viewer_) {
    return viewer_->isVisible() && viewer_->currentRow() == effectivePreviewRow();
  }

  return internalViewer_ && internalViewer_->isVisible();
}

int AdImage::effectivePreviewRow() const { return previewRow_ >= 0 ? previewRow_ : 0; }

AdImageViewer* AdImage::effectiveViewer() const {
  return viewer_ ? viewer_.data() : internalViewer_.data();
}

void AdImage::ensureInternalViewerModel() {
  if (internalViewerModel_) {
    return;
  }

  AdImageViewer* targetViewer =
      internalViewer_ ? internalViewer_.data() : new AdImageViewer(const_cast<AdImage*>(this));
  if (!internalViewer_) {
    internalViewer_ = targetViewer;
  }
  internalViewerModel_ = new AdImageListModel(targetViewer);
}

void AdImage::syncInternalViewerData() {
  if (!internalViewer_) {
    return;
  }

  ensureInternalViewerModel();
  internalViewer_->setImageLoader(imageLoader_);
  internalViewerModel_->setItems(effectiveInternalViewerItems());
}

void AdImage::syncInteractiveState() {
  const bool previewable = canOpenPreview();
  setFocusPolicy(previewable ? Qt::StrongFocus : Qt::NoFocus);
  if (!previewable && hasFocus()) {
    clearFocus();
  }
  setCursor(previewable ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void AdImage::reloadMainImage() {
  cancelMainLoads();

  ++mainLoadToken_;
  if (source_.isEmpty()) {
    imagePixmap_ = QPixmap();
    requestedMainPixelSize_ = QSize();
    decodedMainPixelSize_ = QSize();
    naturalMainPixelSize_ = QSize();
    mainLoadPending_ = false;
    setLoadingState(false);
    setLoadFailedState(false);
    update();
    updateGeometry();
    return;
  }

  mainLoadPending_ = true;
  requestedMainPixelSize_ = QSize();
  decodedMainPixelSize_ = QSize();
  naturalMainPixelSize_ = QSize();
  setLoadFailedState(false);
  imagePixmap_ = QPixmap();
  setLoadingState(false);

  requestMainImageIfNeeded();
  update();
  updateGeometry();
}

AdImageLoadOptions AdImage::mainLoadOptions() const {
  AdImageLoadOptions options;
  if (decodePolicy_ != DecodePolicy::FitWidget) {
    return options;
  }

  const qreal dpr = std::max<qreal>(1.0, devicePixelRatioF());
  const QSize physicalSize(std::max(1, qCeil(width() * dpr)), std::max(1, qCeil(height() * dpr)));
  constexpr int kBucketSize = 64;
  options.targetPixelSize =
      QSize(((physicalSize.width() + kBucketSize - 1) / kBucketSize) * kBucketSize,
            ((physicalSize.height() + kBucketSize - 1) / kBucketSize) * kBucketSize);
  options.aspectRatioMode = Qt::KeepAspectRatio;
  options.allowUpscale = false;
  return options;
}

bool AdImage::mainImageNeedsLargerDecode() const {
  if (decodePolicy_ != DecodePolicy::FitWidget || imagePixmap_.isNull()) {
    return imagePixmap_.isNull();
  }
  if (naturalMainPixelSize_.isValid() && decodedMainPixelSize_ == naturalMainPixelSize_) {
    return false;
  }
  const QSize needed = mainLoadOptions().targetPixelSize;
  if (!needed.isValid() || !requestedMainPixelSize_.isValid()) {
    return false;
  }
  const bool widthTooSmall = needed.width() > requestedMainPixelSize_.width() &&
                             needed.width() - requestedMainPixelSize_.width() >= 64;
  const bool heightTooSmall = needed.height() > requestedMainPixelSize_.height() &&
                              needed.height() - requestedMainPixelSize_.height() >= 64;
  return widthTooSmall || heightTooSmall;
}

void AdImage::requestMainImageIfNeeded() {
  if (source_.isEmpty()) {
    return;
  }
  if (loadingPolicy_ == LoadingPolicy::WhenVisible && !isVisible()) {
    mainLoadPending_ = true;
    return;
  }
  if (mainReply_ || fallbackReply_) {
    const QSize needed = mainLoadOptions().targetPixelSize;
    const bool largerRequest = decodePolicy_ == DecodePolicy::FitWidget && needed.isValid() &&
                               requestedMainPixelSize_.isValid() &&
                               (needed.width() - requestedMainPixelSize_.width() >= 64 ||
                                needed.height() - requestedMainPixelSize_.height() >= 64);
    if (!largerRequest) {
      return;
    }
    mainLoadPending_ = true;
  }
  if (!mainLoadPending_ && !mainImageNeedsLargerDecode()) {
    return;
  }

  cancelMainLoads();
  const int token = ++mainLoadToken_;
  const AdImageLoadOptions options = mainLoadOptions();
  requestedMainPixelSize_ = options.targetPixelSize;
  mainLoadPending_ = false;
  setLoadingState(true);
  setLoadFailedState(false);

  AdImageReply* reply = resolvedImageLoader(imageLoader_)->load(source_, options, this);
  mainReply_ = reply;
  const QPointer<AdImageReply> guardedReply(reply);
  QObject::connect(reply, &AdImageReply::finished, this, [this, token, guardedReply]() {
    if (token != mainLoadToken_) {
      if (guardedReply) {
        guardedReply->deleteLater();
      }
      return;
    }

    const QImage image = guardedReply ? guardedReply->image() : QImage();
    const QSize naturalSize = guardedReply ? guardedReply->naturalSize() : QSize();
    const bool ok = guardedReply && guardedReply->isSuccessful() && !image.isNull();
    mainReply_.clear();
    if (guardedReply) {
      guardedReply->deleteLater();
    }
    if (ok) {
      imagePixmap_ = imageToPixmap(image);
      decodedMainPixelSize_ = image.size();
      naturalMainPixelSize_ = naturalSize;
      setLoadingState(false);
      setLoadFailedState(false);
      update();
      updateGeometry();
      return;
    }

    if (!fallbackSource_.isEmpty() &&
        normalizeSourceKey(fallbackSource_) != normalizeSourceKey(source_)) {
      const AdImageLoadOptions options = mainLoadOptions();
      AdImageReply* fallbackReply =
          resolvedImageLoader(imageLoader_)->load(fallbackSource_, options, this);
      fallbackReply_ = fallbackReply;
      const QPointer<AdImageReply> guardedFallbackReply(fallbackReply);
      QObject::connect(
          fallbackReply, &AdImageReply::finished, this, [this, token, guardedFallbackReply]() {
            if (token != mainLoadToken_) {
              if (guardedFallbackReply) {
                guardedFallbackReply->deleteLater();
              }
              return;
            }

            const QImage fallbackImage =
                guardedFallbackReply ? guardedFallbackReply->image() : QImage();
            const QSize fallbackNaturalSize =
                guardedFallbackReply ? guardedFallbackReply->naturalSize() : QSize();
            const bool fallbackOk = guardedFallbackReply && guardedFallbackReply->isSuccessful() &&
                                    !fallbackImage.isNull();
            fallbackReply_.clear();
            if (guardedFallbackReply) {
              guardedFallbackReply->deleteLater();
            }
            if (fallbackOk) {
              imagePixmap_ = imageToPixmap(fallbackImage);
              decodedMainPixelSize_ = fallbackImage.size();
              naturalMainPixelSize_ = fallbackNaturalSize;
              setLoadingState(false);
              setLoadFailedState(false);
            } else {
              imagePixmap_ = QPixmap();
              decodedMainPixelSize_ = QSize();
              naturalMainPixelSize_ = QSize();
              setLoadingState(false);
              setLoadFailedState(true);
            }
            update();
            updateGeometry();
          });
      return;
    }

    imagePixmap_ = QPixmap();
    decodedMainPixelSize_ = QSize();
    naturalMainPixelSize_ = QSize();
    setLoadingState(false);
    setLoadFailedState(true);
    update();
    updateGeometry();
  });
}

void AdImage::reloadPlaceholderImage() {
  cancelImageReply(placeholderReply_);

  const int token = ++placeholderLoadToken_;
  if (placeholderSource_.isEmpty()) {
    placeholderPixmap_ = QPixmap();
    updateGeometry();
    update();
    return;
  }

  AdImageReply* reply = resolvedImageLoader(imageLoader_)->load(placeholderSource_, this);
  placeholderReply_ = reply;
  const QPointer<AdImageReply> guardedReply(reply);
  QObject::connect(reply, &AdImageReply::finished, this, [this, token, guardedReply]() {
    if (token != placeholderLoadToken_) {
      if (guardedReply) {
        guardedReply->deleteLater();
      }
      return;
    }

    placeholderReply_.clear();
    const QImage image = guardedReply ? guardedReply->image() : QImage();
    if (guardedReply) {
      guardedReply->deleteLater();
    }
    if (!image.isNull() && guardedReply && guardedReply->isSuccessful()) {
      placeholderPixmap_ = imageToPixmap(image);
    } else {
      placeholderPixmap_ = QPixmap();
    }
    updateGeometry();
    update();
  });
}

void AdImage::cancelMainLoads() {
  cancelImageReply(mainReply_);
  cancelImageReply(fallbackReply_);
}

void AdImage::setLoadingState(bool value) {
  if (loading_ == value) {
    return;
  }

  loading_ = value;
  emit loadingChanged(loading_);
  updateAccessibleText();
}

void AdImage::setLoadFailedState(bool value) {
  if (loadFailed_ == value) {
    return;
  }

  loadFailed_ = value;
  emit loadFailedChanged(loadFailed_);
  updateAccessibleText();
}

void AdImage::activatePreviewFromUser() {
  if (!canOpenPreview()) {
    return;
  }

  if (viewer_) {
    viewer_->openAtFromWidget(effectivePreviewRow(), this);
    return;
  }

  AdImageViewer* targetViewer = ensureViewer();
  syncInternalViewerData();
  targetViewer->openAtFromWidget(effectivePreviewRow(), this);
}

void AdImage::bindEffectiveViewerSignals() {
  disconnectViewerSignals();

  AdImageViewer* target = effectiveViewer();
  if (!target) {
    return;
  }

  viewerConnections_.append(
      QObject::connect(target, &AdImageViewer::visibleChanged, this, [this](bool) { update(); }));
  viewerConnections_.append(
      QObject::connect(target, &AdImageViewer::currentRowChanged, this, [this](int) { update(); }));
}

void AdImage::disconnectViewerSignals() {
  for (const QMetaObject::Connection& connection : viewerConnections_) {
    QObject::disconnect(connection);
  }
  viewerConnections_.clear();
}

void AdImage::updateAccessibleText() {
  setAccessibleName(altText_.trimmed());
  if (loadFailed_) {
    setAccessibleDescription(tr("Image failed to load"));
  } else if (canOpenPreview()) {
    setAccessibleDescription(tr("Previewable image"));
  } else {
    setAccessibleDescription(tr("Image"));
  }
}

}  // namespace adqt::widgets
