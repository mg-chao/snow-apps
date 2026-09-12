#include "translation_test_support.h"
#include "snow_shot/network/snowshotapiclient.h"
#include "snow_shot/presentation/selectedtexttranslationcoordinator.h"
#include "snow_shot/presentation/selectedtexttranslationcontroller.h"
#include "snow_shot/presentation/components/translationpagewidget.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/translationpagecontroller.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "widgets/modal.h"
#include "widgets/input_text_edit.h"
#include "widgets/select.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QCursor>
#include <QToolButton>
#include <QWindow>
#include <QLabel>
#include <QScreen>
#include <QTemporaryDir>
#include <QTranslator>

using namespace translation_tests;
using namespace snow_shot::presentation;
using adqt::widgets::AdModal;
namespace storage = snow_shot::storage;

namespace {
struct CaptureState {
    SelectedTextCaptureResult initial;
    SelectedTextCaptureResult result;
    int starts = 0;
    int polls = 0;
    int cancellations = 0;
};
class FakeCaptureBackend final : public SelectedTextCaptureBackend {
  public:
    explicit FakeCaptureBackend(std::shared_ptr<CaptureState> state) : m_state(std::move(state)) {}
    SelectedTextCaptureResult start() override {
        ++m_state->starts;
        return m_state->initial;
    }
    SelectedTextCaptureResult poll() override {
        ++m_state->polls;
        return m_state->result;
    }
    void cancel() override {
        ++m_state->cancellations;
    }

  private:
    std::shared_ptr<CaptureState> m_state;
};

void flushEvents() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

int visibleTranslationWindows() {
    int count = 0;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->isVisible() && widget->objectName() == QStringLiteral("ad-modal-overlay")) {
            ++count;
        }
    }
    return count;
}

void routingAndCancellation() {
    Server server;
    SnowShotApiClient client(server.url());
    auto state = std::make_shared<CaptureState>();
    int screenReads = 0;
    SelectedTextTranslationCoordinator coordinator(
        storage::ApplicationStorage::instance().configuration(), &client, nullptr,
        std::make_unique<FakeCaptureBackend>(state), [&] {
            ++screenReads;
            return qApp->primaryScreen();
        });
    auto* modal = coordinator.findChild<AdModal*>();
    int mainRequests = 0;
    QString mainText;
    QObject::connect(&coordinator, &SelectedTextTranslationCoordinator::mainTranslationRequested,
                     &coordinator, [&](const QString& text) {
                         ++mainRequests;
                         mainText = text;
                     });
    const storage::ExtendedFeaturesSettings settings;
    for (const bool standalone : {false, true}) {
        require(settings.setTranslationPageEnabled(false) &&
                    settings.setStandaloneTranslationWindow(standalone),
                "set disabled routing matrix");
        coordinator.capture();
        require(state->starts == 0 && screenReads == 0 && mainRequests == 0 && !modal->isOpen(),
                "master off never captures or presents either destination");
    }
    require(settings.setTranslationPageEnabled(true) &&
                settings.setStandaloneTranslationWindow(false),
            "enable main-page routing");
    state->initial = {SelectedTextStatus::Selected, QStringLiteral("main page")};
    coordinator.capture();
    require(mainRequests == 1 && mainText == QStringLiteral("main page") && !modal->isOpen(),
            "standalone off routes successful capture to the main-page signal");
    state->initial = {SelectedTextStatus::NoSelection, {}};
    coordinator.capture();
    require(mainRequests == 2 && mainText.isEmpty() && !modal->isOpen(),
            "standalone off routes empty selections to the main-page signal");
    require(settings.setStandaloneTranslationWindow(true), "enable standalone routing");
    state->initial = {};
    coordinator.capture();
    const int readsBeforeDuplicate = screenReads;
    const int startsBeforeDuplicate = state->starts;
    coordinator.capture();
    require(screenReads == readsBeforeDuplicate && state->starts == startsBeforeDuplicate,
            "ignored trigger cannot overwrite captured screen or destination");
    state->result = {SelectedTextStatus::Selected, QStringLiteral("standalone")};
    waitUntil([&] { return modal->isOpen(); }, "capture opens standalone window");
    require(mainRequests == 2 && visibleTranslationWindows() == 1 &&
                modal->windowScreen() == qApp->primaryScreen(),
            "standalone uses trigger screen and never requests main presentation");
    auto* page = qobject_cast<TranslationPageWidget*>(modal->contentWidget());
    require(page != nullptr, "standalone hosts the existing translation page");
    auto* controller = page->findChild<TranslationPageController*>();
    state->initial = {SelectedTextStatus::Selected, QStringLiteral("replacement")};
    coordinator.capture();
    require(modal->contentWidget() == page &&
                controller->sourceText() == QStringLiteral("replacement") &&
                visibleTranslationWindows() == 1 && mainRequests == 2,
            "retrigger reuses the same page and replaces source text");
    require(settings.setStandaloneTranslationWindow(false), "turn standalone off");
    require(!modal->isOpen() && !controller->active(),
            "setting off closes and deactivates page immediately");
    flushEvents();
    state->initial = {};
    state->result = {};
    coordinator.capture();
    const int cancellations = state->cancellations;
    require(settings.setStandaloneTranslationWindow(true), "change routing during pending capture");
    require(state->cancellations > cancellations, "setting change cancels pending capture");
    state->result = {SelectedTextStatus::Selected, QStringLiteral("stale")};
    flushEvents();
    require(!modal->isOpen() && mainRequests == 2,
            "cancelled capture does not open either destination");
    state->initial = {SelectedTextStatus::Selected, QStringLiteral("fresh")};
    coordinator.capture();
    require(modal->isOpen(), "next trigger reads new routing preference");
    require(settings.setTranslationPageEnabled(false), "disable master while open");
    require(!modal->isOpen() && settings.standaloneTranslationWindow(),
            "master off closes window and preserves child preference");
    require(settings.setTranslationPageEnabled(true), "re-enable master");
    for (const auto status : {SelectedTextStatus::NoSelection, SelectedTextStatus::TimedOut,
                              SelectedTextStatus::Failed, SelectedTextStatus::Busy,
                              SelectedTextStatus::Unsupported, SelectedTextStatus::Selected}) {
        // Only a Selected capture carries text; the other statuses report empty payloads.
        const QString payload =
            status == SelectedTextStatus::Selected ? QStringLiteral("  ") : QString();
        state->initial = {status, payload};
        coordinator.capture();
        require(modal->isOpen(), "unusable capture still opens the standalone translation page");
        QWidget* surface = modal->contentWidget()->window();
        auto* warning = surface->findChild<QLabel*>(QStringLiteral("ad-message-content"));
        require(warning != nullptr && warning->isVisible() &&
                    warning->text() == QStringLiteral("Failed to retrieve selected text"),
                "unusable capture warns on the opened page instead of notifying the system");
        modal->close();
        flushEvents();
    }
    require(!modal->isOpen() && mainRequests == 2,
            "every unusable capture stays on the standalone destination");
    state->initial = {};
    state->result = {};
    coordinator.capture();
    require(settings.setTranslationPageEnabled(false), "master cancels pending request");
    require(settings.setTranslationPageEnabled(true), "master re-enable permits new capture");
    state->initial = {SelectedTextStatus::Selected, QStringLiteral("after cancellation")};
    coordinator.capture();
    require(modal->isOpen(), "cancelled controller remains reusable");
    coordinator.shutdown();
    coordinator.capture();
    require(!modal->isOpen(), "shutdown closes presentation and prevents subsequent captures");
    flushEvents();
}

void nativeDisplayCheck() {
    Server server;
    server.holdModels = true;
    SnowShotApiClient client(server.url());
    auto state = std::make_shared<CaptureState>();
    const storage::ExtendedFeaturesSettings settings;
    require(settings.setTranslationPageEnabled(true) &&
                settings.setStandaloneTranslationWindow(true),
            "enable native standalone display check");
    SelectedTextTranslationCoordinator coordinator(
        storage::ApplicationStorage::instance().configuration(), &client, nullptr,
        std::make_unique<FakeCaptureBackend>(state));
    auto* modal = coordinator.findChild<AdModal*>();
    const QPoint previousCursor = QCursor::pos();
    for (QScreen* screen : qApp->screens()) {
        const QRect available = screen->availableGeometry();
        QCursor::setPos(available.center());
        state->initial = {};
        state->result = {};
        coordinator.capture();
        // Moving the pointer during asynchronous capture must not change its destination.
        QCursor::setPos(qApp->screens().last()->availableGeometry().center());
        state->result = {SelectedTextStatus::Selected, QStringLiteral("native display check")};
        waitUntil([&] { return modal->isOpen(); }, "native capture presents standalone");
        QWidget* surface = modal->contentWidget()->window();
        waitUntil([&] { return surface->windowHandle()->isExposed(); },
                  "native surface is exposed");
        require(surface->screen() == screen && modal->windowScreen() == screen,
                "native window uses the screen recorded before asynchronous capture");
        require((surface->geometry().center() - available.center()).manhattanLength() <= 2,
                "native window is centered on cursor display");
        const QRect geometry = surface->geometry();
        QWidget* content = modal->contentWidget();
        surface->showMinimized();
        state->initial = {SelectedTextStatus::Selected, QStringLiteral("native replacement")};
        coordinator.capture();
        waitUntil([&] { return !surface->isMinimized() && surface->isActiveWindow(); },
                  "native retrigger restores and activates standalone window");
        require(modal->contentWidget() == content && surface->geometry() == geometry,
                "native retrigger reuses page and preserves geometry");
        std::cout << "Verified display " << screen->name().toStdString()
                  << " DPR=" << screen->devicePixelRatio() << " size=" << available.width() << 'x'
                  << available.height() << '\n';
        modal->close();
        flushEvents();
    }
    QCursor::setPos(previousCursor);
}

void pageActionsAndLifecycle() {
    Server server;
    SnowShotApiClient client(server.url());
    auto state = std::make_shared<CaptureState>();
    const storage::ExtendedFeaturesSettings settings;
    require(settings.setTranslationPageEnabled(true) &&
                settings.setStandaloneTranslationWindow(true),
            "enable standalone page behavior");
    QWidget existingMain;
    existingMain.setObjectName(QStringLiteral("existing-main-window"));
    existingMain.resize(400, 300);
    existingMain.show();
    SelectedTextTranslationCoordinator coordinator(
        storage::ApplicationStorage::instance().configuration(), &client, nullptr,
        std::make_unique<FakeCaptureBackend>(state));
    auto* modal = coordinator.findChild<AdModal*>();
    state->initial = {SelectedTextStatus::Selected, QStringLiteral("translate this")};
    coordinator.capture();
    auto* page = qobject_cast<TranslationPageWidget*>(modal->contentWidget());
    require(page && modal->mode() == AdModal::Mode::Window && modal->windowModeDetached() &&
                modal->windowModality() == Qt::NonModal && modal->footerVisible() &&
                modal->windowTaskbarVisible() && modal->windowMinimizeButtonVisible() &&
                modal->windowAlwaysOnTopButtonVisible() && modal->ownerWindow() == nullptr,
            "detached taskbar-enabled modal with footer has no main-window owner");
    QWidget* surface = page->window();
    require(surface != &existingMain && surface->parentWidget() == nullptr &&
                modal->closeButtonVisible(),
            "standalone has its own title bar and native surface");
    require((surface->windowFlags() & Qt::WindowType_Mask) == Qt::Window &&
                surface->findChild<QToolButton*>(QStringLiteral("ad-modal-minimize")) &&
                surface->findChild<QToolButton*>(QStringLiteral("ad-modal-always-on-top")),
            "standalone chrome exposes a taskbar window with minimize and pin buttons");
    require(modal->acceptButton()->text() == QStringLiteral("Copy and Close") &&
                modal->rejectButton()->text() == QStringLiteral("Close"),
            "footer buttons offer copy-and-close and close");
    const QRect available = qApp->primaryScreen()->availableGeometry().adjusted(16, 16, -16, -16);
    require(surface->size() == QSize(700, 450).boundedTo(available.size()) &&
                surface->minimumSize() == QSize(650, 400).boundedTo(available.size()),
            "explicit geometry is clamped to available display");
    require(modal->componentTokens().contentPaddingHorizontal.value_or(-1) == 0 &&
                modal->componentTokens().contentPaddingVertical.value_or(-1) == 0,
            "standalone window pads only the modal chrome, not the content area");
    require(modal->componentTokens().headerMarginBottom.value_or(-1) == 0,
            "standalone window drops the chrome gap between header and options");
    surface->resize(surface->size() - QSize(40, 40));
    surface->move(surface->pos() + QPoint(9, 7));
    const QRect geometry = surface->geometry();
    auto* controller = page->findChild<TranslationPageController*>();
    auto* result =
        page->findChild<adqt::widgets::AdTextEdit*>(QStringLiteral("translationResultText"));
    auto* copy = page->findChild<QAction*>(QStringLiteral("translationCopy"));
    auto* copyClose = page->findChild<QAction*>(QStringLiteral("translationCopyAndClose"));
    waitUntil([&] { return server.streams.size() == 1; }, "standalone starts translation");
    server.delta(0, QStringLiteral("translated result"));
    waitUntil([&] { return result->toPlainText() == QStringLiteral("translated result"); },
              "standalone renders streamed output");
    const QString snapshotDirectory = qEnvironmentVariable("SNOW_TRANSLATION_QA_DIR");
    if (!snapshotDirectory.isEmpty()) {
        require(QDir().mkpath(snapshotDirectory),
                "create standalone visual verification directory");
        require(surface->grab().save(
                    QDir(snapshotDirectory).filePath(QStringLiteral("standalone-translation.png"))),
                "save standalone page rendering");
    }
    copy->trigger();
    require(qApp->clipboard()->text() == QStringLiteral("translated result") && modal->isOpen(),
            "copy preserves standalone window");
    styles::ThemeManager::instance().setThemeMode(styles::ThemeMode::Dark);
    flushEvents();
    bool themedLabel = false;
    for (auto* label : page->findChildren<QLabel*>()) {
        themedLabel |= label->palette().color(QPalette::WindowText) ==
                       styles::ThemeManager::instance().themeColorScheme().map.colorTextTertiary;
    }
    require(themedLabel && surface->geometry() == geometry,
            "theme change reaches page without resetting geometry");
    QTranslator translator;
    require(translator.load(QStringLiteral(SNOW_SHOT_TEST_TRANSLATIONS_DIR "/snow_shot_zh_CN.qm")),
            "load translated standalone title");
    qApp->installTranslator(&translator);
    LanguageManager::instance().languageChanged(QStringLiteral("zh_CN"), QLocale(QLocale::Chinese));
    flushEvents();
    require(modal->windowTitle() == QStringLiteral("\u7ffb\u8bd1") &&
                surface->geometry() == geometry,
            "language change retranslates title without resetting geometry");
    qApp->removeTranslator(&translator);
    LanguageManager::instance().languageChanged(QStringLiteral("en_US"), QLocale(QLocale::English));
    flushEvents();
    QPointer<TranslationPageWidget> oldPage = page;
    copyClose->trigger();
    require(!modal->isOpen() && existingMain.isVisible() && !controller->active(),
            "copy-and-close closes only standalone and deactivates streaming page");
    waitUntil([&] { return server.disconnected(0); }, "closing cancels translation stream");
    flushEvents();
    require(oldPage.isNull(), "closed content is disposed after action callback");
    coordinator.capture();
    page = qobject_cast<TranslationPageWidget*>(modal->contentWidget());
    require(page && page->window() == surface && visibleTranslationWindows() == 1,
            "reopening creates fresh content in the same modal surface");
    require(page->window()->size() == QSize(700, 450).boundedTo(available.size()),
            "reopen restores default size rather than persisting geometry");
    controller = page->findChild<TranslationPageController*>();
    waitUntil([&] { return server.streams.size() == 2; }, "reopened page translates");
    server.fail(1);
    waitUntil([&] { return !controller->errorText().isEmpty(); },
              "page exposes translation failure");
    controller->retry();
    waitUntil([&] { return server.streams.size() == 3; }, "retry retains existing page behavior");
    server.delta(2, QStringLiteral("retried"));
    server.finish(2);
    waitUntil([&] { return !controller->translating(); }, "retry finishes");
    auto* target =
        page->findChild<adqt::widgets::AdSelect*>(QStringLiteral("translationTargetLanguage"));
    auto* service = page->findChild<adqt::widgets::AdSelect*>(QStringLiteral("translationService"));
    require(target && service && !service->options().isEmpty(),
            "existing language and service controls available");
    controller->setPreferences(QStringLiteral("auto"), QStringLiteral("ja"),
                               QStringLiteral("general"));
    waitUntil([&] { return server.streams.size() == 4; },
              "preference changes start new translation");
    require(target->currentValue().toString() == QStringLiteral("ja"),
            "page synchronizes target selection");
    server.delta(3, QStringLiteral("final preference result"));
    server.finish(3);
    waitUntil([&] { return !controller->translating(); },
              "preference translation finishes before accepting");
    qApp->clipboard()->setText(QStringLiteral("stale clipboard"));
    modal->acceptButton()->click();
    require(!modal->isOpen() &&
                qApp->clipboard()->text() == QStringLiteral("final preference result"),
            "footer accept copies the result and closes the standalone window");
    require(settings.setStandaloneTranslationWindow(false), "disable standalone while streaming");
    waitUntil([&] { return server.disconnected(3); }, "setting off cancels active stream");
    require(!modal->isOpen() && existingMain.isVisible(),
            "disabling standalone preserves main window");
    flushEvents();
    styles::ThemeManager::instance().setThemeMode(styles::ThemeMode::Light);
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir directory;
    auto& storage = storage::ApplicationStorage::instance();
    require(directory.isValid() &&
                storage.initialize({directory.path(), directory.path(), 60000}).success,
            "initialize isolated standalone translation storage");
    styles::ThemeManager::instance().initialize(app);
    if (app.arguments().contains(QStringLiteral("--native-display-check"))) {
        nativeDisplayCheck();
        storage.shutdown();
        return 0;
    }
    routingAndCancellation();
    pageActionsAndLifecycle();
    storage.shutdown();
    return 0;
}
