#include "widgets/carousel.h"
#include "widgets/image.h"

#include <QBuffer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTranslator>
#include <QVBoxLayout>

#include <functional>

using namespace adqt::widgets;

namespace {
class ImageTranslator final : public QTranslator {
 public:
  QString translate(const char* context, const char* sourceText, const char*, int) const override {
    if (qstrcmp(context, "adqt::widgets::AdImage") == 0) {
      return QStringLiteral("translated:%1").arg(QString::fromUtf8(sourceText));
    }
    return {};
  }
};

class TestReply final : public AdImageReply {
 public:
  explicit TestReply(QObject* parent = nullptr) : AdImageReply(parent) {}

  void complete(const QImage& image) { succeed(image); }
  void abort() override {
    if (isFinished()) {
      return;
    }
    if (aborted) {
      aborted();
    }
    fail(QStringLiteral("aborted"));
  }

  std::function<void()> aborted;
};

class LegacyLoader : public AdImageLoader {
 public:
  using AdImageLoader::AdImageLoader;

  AdImageReply* load(const QUrl& source, QObject* parent = nullptr) override {
    ++calls;
    sources.push_back(source);
    auto* reply = new TestReply(parent);
    QTimer::singleShot(0, reply, [reply]() {
      QImage image(32, 24, QImage::Format_ARGB32_Premultiplied);
      image.fill(Qt::red);
      reply->complete(image);
    });
    return reply;
  }

  int calls = 0;
  QList<QUrl> sources;
};

class OptionsLoader final : public LegacyLoader {
 public:
  using LegacyLoader::LegacyLoader;
  using LegacyLoader::load;

  AdImageReply* load(const QUrl& source, const AdImageLoadOptions& options,
                     QObject* parent = nullptr) override {
    receivedOptions.push_back(options);
    return LegacyLoader::load(source, parent);
  }

  QList<AdImageLoadOptions> receivedOptions;
};

class DeferredLoader final : public AdImageLoader {
 public:
  using AdImageLoader::AdImageLoader;

  AdImageReply* load(const QUrl&, QObject* parent = nullptr) override {
    auto* reply = new TestReply(parent);
    reply->aborted = [this]() { ++abortCalls; };
    replies.push_back(reply);
    return reply;
  }

  int abortCalls = 0;
  QList<QPointer<TestReply>> replies;
};

QUrl dataUrl(const QImage& image) {
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "PNG");
  return QUrl(QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64()));
}
}  // namespace

class ImageTests final : public QObject {
  Q_OBJECT

 private slots:
  void defaultPreviewTextTracksLanguageAndPreservesOverrides() {
    AdImage image;
    QCOMPARE(image.previewText(), QStringLiteral("Preview"));

    QSignalSpy previewTextSpy(&image, &AdImage::previewTextChanged);
    ImageTranslator translator;
    QVERIFY(QCoreApplication::installTranslator(&translator));
    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&image, &languageChange);
    QCOMPARE(image.previewText(), QStringLiteral("translated:Preview"));
    QCOMPARE(previewTextSpy.count(), 1);
    QCOMPARE(previewTextSpy.constFirst().constFirst().toString(),
             QStringLiteral("translated:Preview"));

    image.setPreviewText(QStringLiteral("Inspect"));
    previewTextSpy.clear();
    QCoreApplication::sendEvent(&image, &languageChange);
    QCOMPARE(image.previewText(), QStringLiteral("Inspect"));
    QCOMPARE(previewTextSpy.count(), 0);
    QCoreApplication::removeTranslator(&translator);
  }

  void legacyLoaderCompatibilityOverload() {
    LegacyLoader loader;
    QObject owner;
    AdImageLoadOptions options;
    options.targetPixelSize = QSize(128, 64);
    AdImageLoader* base = &loader;
    AdImageReply* reply = base->load(QUrl(QStringLiteral("test:legacy")), options, &owner);
    QVERIFY(reply != nullptr);
    QTRY_VERIFY(reply->isFinished());
    QCOMPARE(loader.calls, 1);
  }

  void whenVisibleAndFitWidgetOptions() {
    OptionsLoader loader;
    AdImage image;
    image.resize(101, 75);
    image.setImageLoader(&loader);
    image.setLoadingPolicy(AdImage::LoadingPolicy::WhenVisible);
    image.setDecodePolicy(AdImage::DecodePolicy::FitWidget);
    image.setSource(QUrl(QStringLiteral("test:fit")));
    QCoreApplication::processEvents();
    QCOMPARE(loader.calls, 0);

    image.show();
    QTRY_COMPARE(loader.calls, 1);
    QCOMPARE(loader.receivedOptions.size(), 1);
    const QSize target = loader.receivedOptions.front().targetPixelSize;
    const qreal dpr = image.devicePixelRatioF();
    QVERIFY(target.width() >= qCeil(image.width() * dpr));
    QVERIFY(target.height() >= qCeil(image.height() * dpr));
    QVERIFY(target.width() - qCeil(image.width() * dpr) < 64);
    QVERIFY(target.height() - qCeil(image.height() * dpr) < 64);
    QVERIFY(!loader.receivedOptions.front().allowUpscale);
    QTRY_VERIFY(!image.loading());

    image.resize(300, 220);
    image.update();
    QCoreApplication::processEvents();
    QCOMPARE(loader.calls, 1);
  }

  void hiddenCarouselSlidesDoNotLoad() {
    LegacyLoader loader;
    AdCarousel carousel;
    carousel.resize(320, 180);
    carousel.setTransitionDuration(0);
    for (int index = 0; index < 2; ++index) {
      auto* image = new AdImage;
      image->setImageLoader(&loader);
      image->setLoadingPolicy(AdImage::LoadingPolicy::WhenVisible);
      image->setSource(QUrl(QStringLiteral("test:slide-%1").arg(index)));
      carousel.addSlide(image);
    }
    carousel.show();
    QTRY_COMPARE(loader.calls, 1);
    carousel.goTo(1, true);
    QTRY_COMPARE(loader.calls, 2);
  }

  void localAndDataSourcesDecodeAsynchronouslyAndScale() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("large.png"));
    QImage source(512, 256, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::green);
    QVERIFY(source.save(path, "PNG"));

    AdImageLoadOptions options;
    options.targetPixelSize = QSize(128, 128);
    QObject owner;
    AdImageReply* local = defaultAdImageLoader()->load(QUrl::fromLocalFile(path), options, &owner);
    QVERIFY(!local->isFinished());
    QSignalSpy localFinished(local, &AdImageReply::finished);
    QVERIFY(localFinished.wait());
    QVERIFY(local->isSuccessful());
    QCOMPARE(local->image().size(), QSize(128, 64));
    QCOMPARE(local->naturalSize(), source.size());
    local->deleteLater();

    AdImageReply* encoded = defaultAdImageLoader()->load(dataUrl(source), options, &owner);
    QVERIFY(!encoded->isFinished());
    QSignalSpy encodedFinished(encoded, &AdImageReply::finished);
    QVERIFY(encodedFinished.wait());
    QVERIFY(encoded->isSuccessful());
    QCOMPARE(encoded->image().size(), QSize(128, 64));
    QCOMPARE(encoded->naturalSize(), source.size());
    encoded->deleteLater();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(owner.findChildren<AdImageReply*>().size(), 0);
  }

  void identicalRequestsCompleteTogether() {
    QImage source(333, 177, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::blue);
    const QUrl url = dataUrl(source);
    QObject owner;
    AdImageReply* first = defaultAdImageLoader()->load(url, &owner);
    AdImageReply* second = defaultAdImageLoader()->load(url, &owner);
    QVERIFY(!first->isFinished());
    QVERIFY(!second->isFinished());
    QSignalSpy firstFinished(first, &AdImageReply::finished);
    QSignalSpy secondFinished(second, &AdImageReply::finished);
    QVERIFY(firstFinished.wait());
    QTRY_COMPARE(secondFinished.size(), 1);
    QCOMPARE(first->image(), second->image());
  }

  void cancellingOneSubscriberKeepsSharedDecodeAlive() {
    QImage source(2048, 1024, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::cyan);
    const QUrl url = dataUrl(source);
    QObject owner;
    AdImageReply* first = defaultAdImageLoader()->load(url, &owner);
    AdImageReply* second = defaultAdImageLoader()->load(url, &owner);
    QVERIFY(!second->isFinished());
    first->abort();
    first->deleteLater();

    QSignalSpy secondFinished(second, &AdImageReply::finished);
    QVERIFY(secondFinished.wait());
    QVERIFY(second->isSuccessful());
    QCOMPARE(second->image().size(), source.size());
  }

  void localCacheSeparatesSizesAndInvalidatesModifiedFiles() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("mutable.png"));
    QImage source(256, 128, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::red);
    QVERIFY(source.save(path, "PNG"));

    QObject owner;
    AdImageLoadOptions smallOptions;
    smallOptions.targetPixelSize = QSize(64, 64);
    AdImageReply* small =
        defaultAdImageLoader()->load(QUrl::fromLocalFile(path), smallOptions, &owner);
    QSignalSpy smallFinished(small, &AdImageReply::finished);
    QVERIFY(smallFinished.wait());
    QCOMPARE(small->image().size(), QSize(64, 32));

    AdImageLoadOptions largeOptions;
    largeOptions.targetPixelSize = QSize(128, 128);
    AdImageReply* large =
        defaultAdImageLoader()->load(QUrl::fromLocalFile(path), largeOptions, &owner);
    QSignalSpy largeFinished(large, &AdImageReply::finished);
    QVERIFY(largeFinished.wait());
    QCOMPARE(large->image().size(), QSize(128, 64));

    QImage replacement(300, 100, QImage::Format_ARGB32_Premultiplied);
    replacement.fill(Qt::yellow);
    QVERIFY(replacement.save(path, "PNG"));
    AdImageReply* modified =
        defaultAdImageLoader()->load(QUrl::fromLocalFile(path), largeOptions, &owner);
    QSignalSpy modifiedFinished(modified, &AdImageReply::finished);
    QVERIFY(modifiedFinished.wait());
    QVERIFY(modified->isSuccessful());
    QCOMPARE(modified->image().size(), QSize(128, 42));
  }

  void destroyedWidgetCancelsAndReleasesReply() {
    DeferredLoader loader;
    auto* image = new AdImage;
    image->setImageLoader(&loader);
    image->setSource(QUrl(QStringLiteral("test:destroyed")));
    QCOMPARE(loader.replies.size(), 1);
    const QPointer<TestReply> reply = loader.replies.constFirst();

    delete image;

    QCOMPARE(loader.abortCalls, 1);
    QVERIFY(reply.isNull());
  }

  void viewerNavigationAndCloseReleaseReplies() {
    DeferredLoader loader;
    QWidget owner;
    owner.resize(480, 320);
    owner.show();

    AdImageListModel model;
    model.setItems({{QUrl(QStringLiteral("test:viewer-0")), QStringLiteral("First")},
                    {QUrl(QStringLiteral("test:viewer-1")), QStringLiteral("Second")}});
    AdImageViewer viewer;
    viewer.setOwnerWindow(&owner);
    viewer.setImageLoader(&loader);
    viewer.setModel(&model);

    viewer.openAt(0);
    QTRY_VERIFY(viewer.isVisible());
    QCOMPARE(loader.replies.size(), 1);
    QImage firstImage(80, 60, QImage::Format_ARGB32_Premultiplied);
    firstImage.fill(Qt::green);
    loader.replies[0]->complete(firstImage);
    QTRY_VERIFY(loader.replies[0].isNull());

    viewer.activate(1);
    QCOMPARE(loader.replies.size(), 2);
    viewer.close();
    QTRY_VERIFY(!viewer.isVisible());
    QCOMPARE(loader.abortCalls, 1);
    QTRY_VERIFY(loader.replies[1].isNull());

    viewer.openAt(0);
    QTRY_VERIFY(viewer.isVisible());
    QCOMPARE(loader.replies.size(), 3);
    QImage reopenedImage(64, 48, QImage::Format_ARGB32_Premultiplied);
    reopenedImage.fill(Qt::blue);
    loader.replies[2]->complete(reopenedImage);
    QTRY_VERIFY(loader.replies[2].isNull());
    viewer.close();
    QTRY_VERIFY(!viewer.isVisible());
  }

  void staleCompletionsAreIgnoredAndRepliesReleased() {
    DeferredLoader loader;
    AdImage image;
    image.setImageLoader(&loader);
    image.setSource(QUrl(QStringLiteral("test:first")));
    image.setSource(QUrl(QStringLiteral("test:second")));
    QCOMPARE(loader.replies.size(), 2);
    QVERIFY(image.loading());

    QImage first(20, 20, QImage::Format_ARGB32_Premultiplied);
    first.fill(Qt::red);
    if (loader.replies[0] != nullptr && !loader.replies[0]->isFinished()) {
      loader.replies[0]->complete(first);
    }
    QVERIFY(image.loading());

    QImage second(40, 30, QImage::Format_ARGB32_Premultiplied);
    second.fill(Qt::green);
    loader.replies[1]->complete(second);
    QTRY_VERIFY(!image.loading());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(image.findChildren<AdImageReply*>().size(), 0);
  }
};

QTEST_MAIN(ImageTests)
#include "tst_image.moc"
