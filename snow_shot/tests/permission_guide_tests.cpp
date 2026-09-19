#include "snow_shot/presentation/permissionguidecontroller.h"
#include "snow_shot/presentation/components/permissionguidewidget.h"
#include <QApplication>
#include <QDir>
#include <QAbstractButton>
#include <QDrag>
#include <QMouseEvent>
#include <QFile>
#include <QLabel>
#include <QMimeData>
#include <QPushButton>
#include <QPointer>
#include <QTemporaryDir>
#include <QStyleHints>
#include <QTranslator>
#include <cstdlib>
#include <iostream>

using namespace snow_shot::presentation;
namespace {
using P = AppPermission;
using S = AppPermissionStatus;
void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void flush() {
    for (int i = 0; i < 5; ++i)
        QApplication::processEvents();
}
PermissionGuideApplication bundle(const QString& root) {
    const QString path = root + QString::fromUtf8("/Snow Shot 测试.app");
    QDir().mkpath(path + QStringLiteral("/Contents"));
    QFile plist(path + QStringLiteral("/Contents/Info.plist"));
    require(plist.open(QIODevice::WriteOnly), "create bundle metadata");
    plist.write("<?xml version=\"1.0\"?><plist version=\"1.0\"><dict/></plist>");
    QPixmap icon(32, 32);
    icon.fill(QColor("#4080ff"));
    return {QStringLiteral("Snow Shot"), QUrl::fromLocalFile(path), QIcon(icon)};
}
class FakePermissions final : public AppPermissionBackend {
  public:
    AppPermissionSnapshot value;
    bool opens = true;
    int requests = 0;
    std::function<void()> completion;
    FakePermissions() {
        value.statuses.fill(S::Missing);
    }
    AppPermissionSnapshot query() override {
        return value;
    }
    void request(P, std::function<void()> done) override {
        ++requests;
        completion = std::move(done);
    }
    bool openSettings(P) override {
        return opens;
    }
};
class FakePlatform final : public PermissionGuidePlatform {
  public:
    PermissionGuideApplication app;
    PermissionGuideEnvironment state{
        true, true, false, {{1, QRectF(100, 80, 740, 620), 0, 1, true}}, {{0, 0, 1440, 900}}};
    std::function<void()> changed;
    qint64 now = 0;
    int queries = 0;
    int preparations = 0;
    bool observing = false;
    PermissionGuideApplication application() override {
        return app;
    }
    PermissionGuideEnvironment environment() override {
        ++queries;
        return state;
    }
    void start(std::function<void()> callback) override {
        changed = std::move(callback);
        observing = true;
    }
    void stop() override {
        observing = false;
        changed = {};
    }
    void prepareWindow(QWidget*) override {
        ++preparations;
    }
    qint64 monotonicMilliseconds() const override {
        return now;
    }
};
void placement() {
    QVector<PermissionGuideWindow> windows{{1, {0, 0, 740, 620}, 0, 1, true},
                                           {2, {10, 10, 760, 600}, 0, 1, true}};
    require(selectPermissionGuideWindow(windows, 2)->id == 2, "retain selected window");
    require(selectPermissionGuideWindow(windows, 99)->id == 1, "choose frontmost eligible window");
    windows[0].layer = 3;
    windows[1].onScreen = false;
    require(!selectPermissionGuideWindow(windows, 1), "ignore panels and off-space windows");
    windows[0].layer = 0;
    windows[0].alpha = 0;
    require(!selectPermissionGuideWindow(windows, 1), "ignore transparent windows");
    windows[0].alpha = 1;
    windows[0].bounds.setSize({200, 100});
    require(!selectPermissionGuideWindow(windows, 1), "ignore tiny windows");
    const QVector<QRect> screens{{0, 25, 1440, 850}, {-1920, -500, 1920, 1080}};
    require(permissionGuidePlacement(QRectF(100, 80, 740, 620), screens, 110) ==
                QRect(349, 574, 479, 110),
            "match reference margins and content column");
    const auto left = permissionGuidePlacement(QRectF(-1700, -400, 740, 620), screens, 110);
    require(screens[1].contains(left), "negative coordinates remain on the target screen");
    const auto partial = permissionGuidePlacement(QRectF(-300, 500, 740, 620), screens, 150);
    require(screens[0].contains(partial), "choose largest overlap and clamp offscreen geometry");
    const auto wide = permissionGuidePlacement(QRectF(0, 30, 1400, 800), screens, 100);
    require(wide.width() == 500, "large Settings window retains compact guide");
    require(permissionGuidePlacement(std::nullopt, screens, 100) == QRect(928, 759, 500, 100),
            "fallback stays within available display, above Dock");
    require(permissionGuidePlacement(std::nullopt, {}, 100).isEmpty(),
            "no display has no placement");
}
void lifecycle(const PermissionGuideApplication& app) {
    auto backend = std::make_unique<FakePermissions>();
    auto* native = backend.get();
    AppPermissionService service(std::move(backend));
    service.refresh();
    flush();
    auto platform = std::make_unique<FakePlatform>();
    auto* fake = platform.get();
    fake->app = app;
    PermissionGuideController controller(service, std::move(platform));
    require(!controller.widget() && !controller.tracking(), "no widget or polling before opening");
    int opened = 0;
    QObject::connect(&service, &AppPermissionService::settingsOpened, [&opened](P) { ++opened; });
    native->opens = false;
    require(!service.openSettings(P::ScreenRecording) && !controller.widget() && opened == 0,
            "failed Settings open never starts guidance");
    native->opens = true;
    require(service.openSettings(P::ScreenRecording), "open succeeds");
    auto* widget = controller.widget();
    require(widget && widget->isVisible() && controller.tracking() && service.polling(),
            "successful opening displays and observes guidance");
    require(opened == 1 && fake->observing, "successful signal starts native observers");
    service.openSettings(P::Accessibility);
    require(controller.widget() == widget, "repeated clicks reuse one guide");
    fake->state.windows[0].bounds.translate(80, 40);
    controller.updatePlacement();
    require(widget->geometry().right() == 907, "guide tracks movement");
    fake->state.settingsActive = false;
    controller.updatePlacement();
    require(!widget->isVisible() && !controller.tracking() && !service.polling(),
            "inactive Settings suspends guide polling");
    fake->state.settingsActive = true;
    fake->changed();
    flush();
    require(widget->isVisible(), "activation notification resumes guide");
    fake->state.windows[0].onScreen = false;
    controller.updatePlacement();
    require(!widget->isVisible(), "minimize hides guide");
    fake->now = 10000;
    controller.updatePlacement();
    require(!widget->isVisible(), "minimize never becomes screen-bottom fallback");
    fake->state.windows[0].onScreen = true;
    controller.updatePlacement();
    widget->setInteracting(true);
    controller.dismiss();
    require(widget->isVisible(), "dismissal waits for native drag to finish");
    widget->setInteracting(false);
    require(!widget->isVisible() && !controller.tracking() && !fake->observing &&
                !service.polling(),
            "drag completion releases all observation");
    service.openSettings(P::InputMonitoring);
    native->value.statuses[2] = S::Granted;
    service.refresh();
    flush();
    require(!widget->isVisible() && !controller.tracking(), "confirmed authorization closes guide");
    service.openSettings(P::ScreenRecording);
    fake->state.settingsRunning = false;
    controller.updatePlacement();
    require(!widget->isVisible() && !fake->observing, "Settings termination closes session");
    fake->state.settingsRunning = true;
    fake->state.windows = {{99, QRectF(0, 0, 50, 50), 3, 1, true}};
    service.openSettings(P::ScreenRecording);
    require(!widget->isVisible(), "discovery does not show in a guessed location immediately");
    fake->now += 4999;
    controller.updatePlacement();
    require(!widget->isVisible(), "wait five seconds for native geometry");
    ++fake->now;
    controller.updatePlacement();
    require(widget->isVisible(),
            "irrelevant native panels do not prevent bounded geometry fallback");
    auto* label = widget->findChild<QLabel*>(QStringLiteral("permissionGuideInstruction"));
    require(label->text().contains(QStringLiteral("in System Settings")),
            "fallback uses accurate wording");
    native->value.statuses[3] = S::NotDetermined;
    service.refresh();
    flush();
    service.openSettings(P::Microphone);
    fake->now += 5000;
    controller.updatePlacement();
    auto* request = widget->findChild<QPushButton*>(QStringLiteral("permissionGuideRequest"));
    require(request->isVisible() && request->isEnabled(), "microphone offers explicit request");
    request->click();
    require(native->requests == 1 && !request->isEnabled(),
            "deduplicate pending microphone requests");
    native->completion();
    flush();
    controller.dismiss();
    const int queries = fake->queries;
    controller.updatePlacement();
    require(fake->queries == queries, "dismissed guide performs no environment queries");
}
void payloadAndCopy(const PermissionGuideApplication& app) {
    PermissionGuideWidget widget(app);
    std::unique_ptr<QMimeData> mime(widget.createDragMimeData());
    require(mime && mime->urls() == QList<QUrl>{app.bundleUrl},
            "drag exports exactly the running Unicode app URL");
    widget.setPermission(P::Microphone, S::Denied, false, false);
    require(!widget.createDragMimeData(), "microphone is never draggable");
    auto* instruction = widget.findChild<QLabel*>(QStringLiteral("permissionGuideInstruction"));
    require(instruction->text().contains(QStringLiteral("Turn on microphone")),
            "denied microphone guidance");
    widget.setPermission(P::Microphone, S::Error, false, false);
    require(instruction->text().contains(QStringLiteral("unavailable")),
            "missing microphone metadata guidance");
    widget.setPermission(P::Accessibility, S::Restricted, false, false);
    require(!widget.createDragMimeData() &&
                instruction->text().contains(QStringLiteral("administrator")),
            "restricted access cannot be fixed by dragging");
    PermissionGuideWidget unbundled({QStringLiteral("Snow Shot"), {}, {}});
    require(!unbundled.createDragMimeData(), "unbundled executable cannot be dragged");
    require(unbundled.findChild<QLabel*>(QStringLiteral("permissionGuideInstruction"))
                ->text()
                .contains(QStringLiteral("installed application")),
            "unbundled executable explains remedy");
    widget.setPermission(P::Accessibility, S::Missing, false, false);
    require(widget.heightForGuideWidth(350) >= widget.heightForGuideWidth(500),
            "narrow translated layout grows vertically");
    require(!widget.findChild<QPushButton*>(QStringLiteral("permissionGuideClose"))
                 ->accessibleName()
                 .isEmpty(),
            "close button has accessible name");
}
void destruction(const PermissionGuideApplication& app) {
    auto backend = std::make_unique<FakePermissions>();
    AppPermissionService service(std::move(backend));
    auto platform = std::make_unique<FakePlatform>();
    auto* fake = platform.get();
    fake->app = app;
    auto controller = std::make_unique<PermissionGuideController>(service, std::move(platform));
    controller->showFor(P::Accessibility);
    auto late = fake->changed;
    QPointer<PermissionGuideWidget> widget(controller->widget());
    widget->setInteracting(true);
    controller.reset();
    late();
    require(widget && !widget->isVisible() && !service.polling(),
            "drag source survives owner destruction");
    widget->setInteracting(false);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(!widget, "drag source is deleted after native drag unwinds");
}
void dragInteractions(const PermissionGuideApplication& app) {
    PermissionGuideWidget widget(app);
    widget.resize(479, widget.heightForGuideWidth(479));
    widget.show();
    flush();
    auto* row = widget.findChild<QAbstractButton*>(QStringLiteral("permissionGuideApp"));
    const auto send = [row](QEvent::Type type, const QPoint& position, Qt::MouseButton button,
                            Qt::MouseButtons buttons) {
        QMouseEvent event(type, QPointF(position), QPointF(row->mapToGlobal(position)), button,
                          buttons, Qt::NoModifier);
        QApplication::sendEvent(row, &event);
    };
    send(QEvent::MouseButtonPress, {15, 20}, Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseMove, {16, 20}, Qt::NoButton, Qt::LeftButton);
    require(row->isDown(), "small pointer movement must not start a drag");
    send(QEvent::MouseButtonRelease, {16, 20}, Qt::LeftButton, Qt::NoButton);
    require(!widget.interacting(), "ordinary click releases interaction state");
    send(QEvent::MouseButtonPress, {15, 20}, Qt::LeftButton, Qt::LeftButton);
    QTimer::singleShot(0, &widget, [] { QDrag::cancel(); });
    send(QEvent::MouseMove, {15 + QApplication::startDragDistance() + 4, 20}, Qt::NoButton,
         Qt::LeftButton);
    require(!widget.interacting() && !row->isDown(),
            "cancelled native drag releases source and pressed state");
    require(widget.isVisible(), "drag cancellation never dismisses permission guidance");
}
void translationsAndAppearance(const PermissionGuideApplication& app) {
    PermissionGuideWidget widget(app);
    widget.resize(479, widget.heightForGuideWidth(479));
    widget.show();
    const auto* label = widget.findChild<QLabel*>(QStringLiteral("permissionGuideInstruction"));
    const QString english = label->text();
    for (const auto& locale : {QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
        QTranslator translator;
        require(translator.load(QStringLiteral(":/i18n/snow_shot_%1.qm").arg(locale)),
                "load complete guide catalog");
        QApplication::installTranslator(&translator);
        flush();
        require(label->text() != english && label->text().contains(app.name),
                "visible guide retranslates and preserves app name");
        widget.resize(479, widget.heightForGuideWidth(479));
        flush();
        require(label->height() >= label->heightForWidth(label->width()),
                "translated guidance is not clipped");
        QApplication::removeTranslator(&translator);
        flush();
        require(label->text() == english, "language change restores English");
    }
    widget.setColorScheme(Qt::ColorScheme::Dark);
    flush();
    require(widget.grab().toImage().pixelColor(20, 5).lightness() < 128,
            "system dark appearance is applied");
    widget.setColorScheme(Qt::ColorScheme::Light);
    flush();
    require(widget.grab().toImage().pixelColor(20, 5).lightness() > 128,
            "system light appearance is applied");
}
void render(const PermissionGuideApplication& app) {
    for (const auto scheme : {Qt::ColorScheme::Light, Qt::ColorScheme::Dark}) {
        QGuiApplication::styleHints()->setColorScheme(scheme);
        const QString appearance =
            scheme == Qt::ColorScheme::Dark ? QStringLiteral("dark") : QStringLiteral("light");
        for (const auto& locale :
             {QStringLiteral("en_US"), QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
            QTranslator translator;
            if (locale != QStringLiteral("en_US")) {
                require(translator.load(QStringLiteral(":/i18n/snow_shot_%1.qm").arg(locale)),
                        "load translations");
                QApplication::installTranslator(&translator);
            }
            PermissionGuideWidget widget(app);
            widget.setColorScheme(scheme);
            widget.resize(479, widget.heightForGuideWidth(479));
            widget.show();
            flush();
            require(widget.grab().save(
                        QStringLiteral("build/permission-guide-%1-%2.png").arg(locale, appearance)),
                    "save render");
            QApplication::removeTranslator(&translator);
        }
    }
    QGuiApplication::styleHints()->unsetColorScheme();
}

} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary app fixture");
    const auto application = bundle(directory.path());
    placement();
    lifecycle(application);
    payloadAndCopy(application);
    destruction(application);
    translationsAndAppearance(application);
    dragInteractions(application);
    if (app.arguments().contains(QStringLiteral("--render"))) {
        auto rendering = application;
        rendering.icon = QIcon(QStringLiteral("snow_shot/resources/app-icon.ico"));
        render(rendering);
    }
    return 0;
}
