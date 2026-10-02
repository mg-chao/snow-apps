#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QTest>
#include <QWidget>

#include "theme/theme.h"
#include "widgets/message.h"

using adqt::theme::ThemeConfig;
using adqt::theme::ThemeManager;
using adqt::widgets::AdMessage;

namespace {

void showOwner(QWidget* owner) {
  owner->resize(720, 480);
  owner->show();
  QVERIFY(QTest::qWaitForWindowExposed(owner));
  QCoreApplication::processEvents();
}

AdMessage::Request persistentRequest(const QString& content,
                                     AdMessage::Type type = AdMessage::Type::Info) {
  AdMessage::Request request;
  request.type = type;
  request.content = content;
  request.durationMs = 0;
  return request;
}

}  // namespace

class MessageClippingTest final : public QObject {
  Q_OBJECT

 private slots:
  void initTestCase() {
    qRegisterMetaType<AdMessage::Type>();
    qRegisterMetaType<AdMessage::CloseReason>();
    originalTheme_ = ThemeManager::instance().config();
  }

  void init() {
    ThemeConfig config = originalTheme_;
    config.motion = false;
    ThemeManager::instance().setConfig(config);
  }

  void cleanup() {
    adqt::widgets::AdMessageService::destroyAll();
    adqt::widgets::AdMessageService::setConfig({});
    ThemeManager::instance().setConfig(originalTheme_);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
  }

  void loadingMessageHidesWhenClippedAndReturnsAfterResize() {
    QWidget owner;
    showOwner(&owner);
    AdMessage messages(&owner);
    auto request = persistentRequest(QStringLiteral("Recognizing text"), AdMessage::Type::Loading);
    request.key = QStringLiteral("recognition");
    request.hideWhenClipped = true;
    auto* handle = messages.open(request);
    QVERIFY(handle);
    auto* notice = handle->noticeWidget();
    QVERIFY(notice->isVisible());
    const int requiredHeight = notice->geometry().bottom() + 1;

    owner.resize(720, requiredHeight);
    QCoreApplication::processEvents();
    QVERIFY(notice->isVisible());
    owner.resize(720, requiredHeight - 1);
    QCoreApplication::processEvents();
    QVERIFY(!notice->isVisible());
    QVERIFY(handle->isOpen());
    QCOMPARE(messages.count(), 1);
    QVERIFY(notice->parentWidget()->testAttribute(Qt::WA_TransparentForMouseEvents));

    owner.resize(40, 480);
    QCoreApplication::processEvents();
    QVERIFY(!notice->isVisible());
    owner.resize(720, 480);
    QCoreApplication::processEvents();
    QVERIFY(notice->isVisible());
    QVERIFY(owner.rect().contains(notice->geometry()));
    QVERIFY(!notice->parentWidget()->testAttribute(Qt::WA_TransparentForMouseEvents));

    owner.resize(80, 30);
    QCoreApplication::processEvents();
    request.content = QStringLiteral("Preparing text recognition components (50%)");
    QCOMPARE(messages.open(request), handle);
    QVERIFY(!notice->isVisible());
    messages.destroy(request.key);
    QCOMPARE(messages.count(), 0);
    owner.resize(720, 480);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
    QCOMPARE(messages.count(), 0);
  }

  void loadingMessageStartsHiddenInSmallOwnerWithMotion() {
    ThemeConfig config = originalTheme_;
    config.motion = true;
    ThemeManager::instance().setConfig(config);
    QWidget owner;
    showOwner(&owner);
    owner.resize(40, 30);
    AdMessage messages(&owner);
    auto request = persistentRequest(QStringLiteral("Recognizing text"), AdMessage::Type::Loading);
    request.hideWhenClipped = true;
    auto* handle = messages.open(request);
    QVERIFY(handle);
    QVERIFY(!handle->noticeWidget()->isVisible());
    QTest::qWait(400);
    QVERIFY(!handle->noticeWidget()->isVisible());
    owner.resize(720, 480);
    QCoreApplication::processEvents();
    QVERIFY(handle->noticeWidget()->isVisible());
  }

  void clippingSuppressionIsOptIn() {
    QWidget owner;
    showOwner(&owner);
    owner.resize(40, 30);
    AdMessage messages(&owner);
    auto* handle = messages.warning(persistentRequest(QStringLiteral("Warning")));
    QVERIFY(handle);
    QVERIFY(handle->noticeWidget()->isVisible());
  }

 private:
  ThemeConfig originalTheme_;
};

QTEST_MAIN(MessageClippingTest)

#include "message_clipping_tests.moc"
