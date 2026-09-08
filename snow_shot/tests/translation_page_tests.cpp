#include "translation_test_support.h"

#include "snow_shot/presentation/components/contentcardwidget.h"
#include "snow_shot/presentation/components/maincontentheaderwidget.h"
#include "snow_shot/presentation/components/sidebarwidget.h"
#include "snow_shot/presentation/components/translationpagewidget.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/mainwindow.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/translationpagecontroller.h"
#include "snow_shot/storage/applicationstorage.h"
#include "widgets/button.h"
#include "widgets/input_text_edit.h"
#include "widgets/navigation_menu.h"
#include "widgets/popover.h"
#include "widgets/scroll_area.h"
#include "widgets/select.h"
#include "widgets/tabs.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEnterEvent>
#include <QFileInfo>
#include <QFontDatabase>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTranslator>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace translation_tests;
using namespace adqt::widgets;
namespace settings = snow_shot::presentation::settings;
namespace styles = snow_shot::presentation::styles;

namespace {
template <typename T> T* child(QObject& owner, const char* name) {
    auto* widget = owner.findChild<T*>(QString::fromLatin1(name));
    if (widget == nullptr) {
        if (auto* popup = owner.findChild<AdPopover*>();
            popup != nullptr && popup->contentWidget()) {
            widget = popup->contentWidget()->findChild<T*>(QString::fromLatin1(name));
        }
    }
    require(widget != nullptr, name);
    return widget;
}

void key(QWidget* widget, int value, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent press(QEvent::KeyPress, value, modifiers);
    QApplication::sendEvent(widget, &press);
    QKeyEvent release(QEvent::KeyRelease, value, modifiers);
    QApplication::sendEvent(widget, &release);
    flushEvents();
}

void snapshot(QWidget& window, const QString& name) {
    const QString output = qEnvironmentVariable("SNOW_TRANSLATION_QA_DIR");
    if (!output.isEmpty()) {
        require(QDir().mkpath(output), "create visual verification directory");
        flushEvents();
        require(window.grab().save(QDir(output).filePath(name + QStringLiteral(".png"))),
                "save translation page rendering");
    }
}

void editorAndShortcutBehavior() {
    Server server;
    SnowShotApiClient client(server.url());
    QWidget owner;
    owner.resize(700, 650);
    auto* page = new TranslationPageWidget(&owner, &client, 0);
    page->setGeometry(owner.rect());
    QObject::connect(page, &TranslationPageWidget::hideWindowRequested, &owner, &QWidget::hide);
    owner.show();
    auto* source = child<AdTextEdit>(*page, "translationSourceText");
    auto* result = child<AdTextEdit>(*page, "translationResultText");
    auto* copy = child<AdButton>(*page, "translationCopy");
    auto* copyHide = child<AdButton>(*page, "translationCopyAndHide");
    auto* floating = child<AdButton>(*page, "translationActions");
    auto* popover = child<AdPopover>(*page, "translationActionsPopover");
    auto* controller = page->findChild<snow_shot::presentation::TranslationPageController*>();
    require(controller != nullptr && result->isReadOnly() && !copy->isEnabled() &&
                !child<AdButton>(*page, "translationResultCopy")->isEnabled(),
            "empty page has read-only result and disabled copy actions");
    QApplication::clipboard()->setText(QStringLiteral("sentinel"));
    key(source, Qt::Key_Q, Qt::ControlModifier);
    require(owner.isVisible() && QApplication::clipboard()->text() == QStringLiteral("sentinel"),
            "empty Copy and Hide leaves clipboard and window intact");

    const QString emoji = QString::fromUcs4(U"😀");
    const QString boundary = QString(4999, u'a') + emoji;
    source->setPlainText(boundary + QStringLiteral("overflow"));
    require(source->toPlainText() == boundary, "limit counts code points without splitting emoji");
    source->moveCursor(QTextCursor::End);
    QKeyEvent typed(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    QApplication::sendEvent(source, &typed);
    require(source->toPlainText() == boundary, "typing cannot exceed the Unicode limit");
    source->selectAll();
    QApplication::clipboard()->setText(boundary + QStringLiteral("paste overflow"));
    source->paste();
    require(source->toPlainText() == boundary, "paste obeys the Unicode limit");
    source->clear();
    QInputMethodEvent preedit(QStringLiteral("正在输入"), {});
    QApplication::sendEvent(source, &preedit);
    source->insertPlainText(QStringLiteral("not committed yet"));
    flushEvents();
    require(server.streams.isEmpty(), "composition blocks automatic translation");
    source->clear();
    QInputMethodEvent commit;
    commit.setCommitString(boundary + QStringLiteral("extra"));
    QApplication::sendEvent(source, &commit);
    require(source->toPlainText() == boundary, "committed IME text obeys the Unicode limit");
    source->clear();
    QMimeData dropText;
    dropText.setText(boundary + QStringLiteral("drop overflow"));
    QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &dropText, Qt::LeftButton,
                          Qt::NoModifier);
    QApplication::sendEvent(source->viewport(), &enter);
    QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &dropText, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(source->viewport(), &drop);
    require(source->toPlainText() == boundary, "drop obeys the Unicode limit");
    source->setPlainText(QStringLiteral("Hello, world!"));
    waitUntil([&]() { return server.streams.size() == 1; }, "source starts one translation");
    server.delta(0, QStringLiteral("你好，"));
    waitUntil([&]() { return result->toPlainText() == QStringLiteral("你好，"); },
              "show partial result");
    require(copy->isEnabled() && copyHide->isEnabled(), "partial results are copyable");
    source->setFocus();
    source->selectAll();
    key(source, Qt::Key_C, Qt::ControlModifier);
    require(QApplication::clipboard()->text() == source->toPlainText(),
            "Ctrl+C copies selected source text");
    QTextCursor cursor = source->textCursor();
    cursor.clearSelection();
    source->setTextCursor(cursor);
    key(source, Qt::Key_C, Qt::ControlModifier);
    require(QApplication::clipboard()->text() == result->toPlainText(),
            "Ctrl+C with no selection copies the partial result");
    QApplication::clipboard()->setText(QStringLiteral("before owner shortcut"));
    key(&owner, Qt::Key_C, Qt::ControlModifier);
    require(QApplication::clipboard()->text() == result->toPlainText(),
            "active page shortcuts also work with focus outside its editors");
    result->selectAll();
    const QString selected = result->textCursor().selectedText();
    server.delta(0, QStringLiteral("世界！\n\nSecond paragraph."));
    waitUntil([&]() { return result->toPlainText().contains(QStringLiteral("Second")); },
              "append additional streamed text");
    require(result->textCursor().selectedText() == selected,
            "streaming preserves output selection");
    key(result, Qt::Key_C, Qt::ControlModifier);
    require(QApplication::clipboard()->text() == selected, "Ctrl+C copies the selected output");
    copy->click();
    require(QApplication::clipboard()->text() == result->toPlainText(),
            "floating Copy always copies the whole result despite a selection");

    popover->setHoverOpenDelayMs(0);
    popover->setHoverCloseDelayMs(0);
    const QPointF local = floating->rect().center();
    QCursor::setPos(floating->mapToGlobal(local.toPoint()));
    QEnterEvent hover(local, local, floating->mapToGlobal(local.toPoint()));
    QApplication::sendEvent(floating, &hover);
    waitUntil([&]() { return popover->isVisible(); }, "hover reveals actions");
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(floating, &leave);
    QCursor::setPos(copy->mapToGlobal(QPoint(5, 5)));
    QEnterEvent actionHover(QPointF(5, 5), QPointF(5, 5), copy->mapToGlobal(QPoint(5, 5)));
    QApplication::sendEvent(copy, &actionHover);
    flushEvents();
    require(popover->isVisible(), "pointer can travel from floating button to action");
    key(copy, Qt::Key_Escape);
    require(!popover->isVisible(), "Escape dismisses the action popup");
    key(floating, Qt::Key_Return);
    require(popover->isVisible(), "keyboard activation reveals actions");
    key(copy, Qt::Key_Escape);
    key(floating, Qt::Key_Space);
    require(popover->isVisible(), "Space also reveals the actions");
    QApplication::clipboard()->setText(QStringLiteral("before keyboard action"));
    key(copy, Qt::Key_Return);
    require(!popover->isVisible() && QApplication::clipboard()->text() == result->toPlainText(),
            "Enter activates a focused popup action");
    server.delta(0, QStringLiteral("\nA line of translated text.").repeated(80));
    waitUntil([&]() { return result->verticalScrollBar()->maximum() > 0; },
              "long results can scroll inside their pane");
    cursor = result->textCursor();
    cursor.clearSelection();
    result->setTextCursor(cursor);
    result->verticalScrollBar()->setValue(0);
    server.delta(0, QStringLiteral("\nLast delta."));
    waitUntil([&]() { return result->toPlainText().endsWith(QStringLiteral("Last delta.")); },
              "append while the reader is scrolled upward");
    require(result->verticalScrollBar()->value() == 0,
            "streaming does not scroll away from the reader");
    key(source, Qt::Key_Q, Qt::ControlModifier);
    require(!owner.isVisible() && QApplication::clipboard()->text() == result->toPlainText(),
            "Ctrl+Q copies the partial result and hides the owning window");
    server.finish(0);
    waitUntil([&]() { return !controller->translating(); }, "hidden page finishes its stream");
    owner.show();
    require(!source->toPlainText().isEmpty() && !result->toPlainText().isEmpty(),
            "hide and reopen retains the same page draft");
    page->deactivate();
    source->setPlainText(QStringLiteral("after deactivation"));
    QApplication::clipboard()->setText(QStringLiteral("untouched"));
    key(source, Qt::Key_Q, Qt::ControlModifier);
    require(owner.isVisible() && QApplication::clipboard()->text() == QStringLiteral("untouched"),
            "inactive page cannot execute translation shortcuts");
}

void navigationThemesLanguagesAndGeometry() {
    Server server;
    qputenv("SNOW_SHOT_API_BASE_URL", server.url().toUtf8());
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    settings::SettingsRuntimeSession runtime(settings::builtInSettingsRegistry(), backend);
    MainWindow window(settings::builtInSettingsRegistry(), runtime);
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    window.resize(900, 556);
    window.show();
    auto* card = window.findChild<ContentCardWidget*>();
    auto* sidebar = window.findChild<SidebarWidget*>();
    require(card != nullptr && sidebar != nullptr, "main window exposes navigation and content");
    auto* navigation = sidebar->findChild<AdNavigationMenu*>();
    const auto* model = navigation->model();
    const auto role = AdNavigationMenu::StableIdRole;
    int translationRow = -1;
    for (int row = 0; row < model->rowCount(); ++row) {
        if (model->index(row, 0).data(role).toString() == QStringLiteral("/tools/translation")) {
            translationRow = row;
        }
    }
    require(translationRow > 0 && model->index(translationRow - 1, 0).data(role).toString() ==
                                      QStringLiteral("/global-mouse"),
            "Translation follows Global mouse in navigation");
    card->setCurrentRoute(QStringLiteral("/tools/translation"));
    auto* page = window.findChild<TranslationPageWidget*>();
    require(page != nullptr && card->currentSections().isEmpty() &&
                sidebar->currentRoute() == QStringLiteral("/tools/translation"),
            "Translation is a standalone route without section tabs");
    auto* source = child<AdTextEdit>(*page, "translationSourceText");
    auto* result = child<AdTextEdit>(*page, "translationResultText");
    auto* controller = page->findChild<snow_shot::presentation::TranslationPageController*>();
    waitUntil([&]() { return !controller->loadingModels(); }, "load services on main page");
    child<AdSelect>(*page, "translationTargetLanguage")->setCurrentValue(QStringLiteral("zh-Hans"));
    const auto inspectVariants = [&](const QString& state) {
        const QString capturedResult = result->toPlainText();
        const qsizetype capturedRequests = server.streams.size();
        for (const auto appearance :
             {styles::ThemeAppearance::Light, styles::ThemeAppearance::Dark}) {
            styles::ThemeManager::instance().setThemeAppearance(appearance);
            const QString theme = appearance == styles::ThemeAppearance::Light
                                      ? QStringLiteral("light")
                                      : QStringLiteral("dark");
            for (const auto& locale :
                 {QStringLiteral("en_US"), QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
                QTranslator translator;
                require(translator.load(QStringLiteral("snow_shot_%1.qm").arg(locale),
                                        QStringLiteral(SNOW_SHOT_TEST_TRANSLATIONS_DIR)),
                        "load complete translation catalog");
                QCoreApplication::installTranslator(&translator);
                flushEvents();
                require(child<AdButton>(*page, "translationCopyAndHide")->text() ==
                            translator.translate("TranslationPageWidget", "Copy and Hide (Ctrl+Q)"),
                        "floating actions retranslate immediately");
                for (const bool collapsed : {false, true}) {
                    sidebar->setCollapsed(collapsed);
                    for (const QSize size : {QSize(900, 556), QSize(512, 316), QSize(1200, 900)}) {
                        window.resize(size);
                        flushEvents();
                        flushEvents();
                        auto* scroll = page->findChild<AdScrollArea*>();
                        require(scroll != nullptr && scroll->horizontalScrollBar()->maximum() == 0,
                                "translation layout never needs horizontal scrolling");
                        const auto* floating = child<AdButton>(*page, "translationActions");
                        require(page->rect().contains(floating->geometry()),
                                "floating action remains within the visible page");
                        const QRect viewportGeometry(scroll->viewport()->mapTo(page, QPoint()),
                                                     scroll->viewport()->size());
                        require(!viewportGeometry.intersects(floating->geometry()),
                                "floating actions never cover scrolled text or inline controls");
                        require(source->width() > 100 && result->width() > 100,
                                "both text panes remain usable at minimum window size");
                        require(source->height() >= source->minimumSizeHint().height() &&
                                    result->height() >= result->minimumSizeHint().height() &&
                                    source->height() > 200,
                                "editor geometry retains its twelve-row baseline after updates");
                        if (source->mapTo(page, QPoint()).y() ==
                            result->mapTo(page, QPoint()).y()) {
                            require(std::abs(source->viewport()->height() -
                                             result->viewport()->height()) <= 2,
                                    "source and result editing surfaces have matching heights");
                        }
                        const QString snapshotName =
                            (state == QStringLiteral("completed")
                                 ? QStringLiteral("translation-")
                                 : QStringLiteral("translation-") + state + u'-') +
                            QStringLiteral("%1-%2-%3-%4x%5")
                                .arg(theme, locale,
                                     collapsed ? QStringLiteral("collapsed")
                                               : QStringLiteral("expanded"))
                                .arg(size.width())
                                .arg(size.height());
                        scroll->verticalScrollBar()->setValue(0);
                        snapshot(window, snapshotName);
                        if (scroll->verticalScrollBar()->maximum() > 0) {
                            scroll->verticalScrollBar()->setValue(
                                scroll->verticalScrollBar()->maximum());
                            flushEvents();
                            if (!result->toPlainText().isEmpty()) {
                                const auto* inlineCopy =
                                    child<AdButton>(*page, "translationResultCopy");
                                const QRect copyGeometry(
                                    inlineCopy->mapTo(scroll->viewport(), QPoint()),
                                    inlineCopy->size());
                                require(scroll->viewport()->rect().contains(copyGeometry),
                                        "scrolling to the bottom makes the inline copy fully "
                                        "reachable");
                            }
                            snapshot(window, snapshotName + QStringLiteral("-scrolled"));
                            scroll->verticalScrollBar()->setValue(0);
                        }
                    }
                }
                require(result->toPlainText() == capturedResult &&
                            server.streams.size() == capturedRequests,
                        "theme, layout and language changes preserve draft without retranslating");
                QCoreApplication::removeTranslator(&translator);
            }
        }
        sidebar->setCollapsed(false);
        window.resize(900, 556);
        styles::ThemeManager::instance().setThemeAppearance(styles::ThemeAppearance::Light);
        flushEvents();
    };
    snapshot(window, QStringLiteral("translation-empty"));
    inspectVariants(QStringLiteral("empty"));
    source->setPlainText(QStringLiteral("Hello, world!\n\nTranslate text between languages."));
    controller->retry();
    waitUntil([&]() { return server.streams.size() == 1; }, "translate main-window draft");
    server.delta(0, QStringLiteral("你好，世界！"));
    waitUntil([&]() { return !result->toPlainText().isEmpty(); }, "main-window streaming result");
    snapshot(window, QStringLiteral("translation-streaming"));
    inspectVariants(QStringLiteral("streaming"));
    server.fail(0);
    waitUntil([&]() { return !controller->errorText().isEmpty(); },
              "main-window error presentation");
    snapshot(window, QStringLiteral("translation-error"));
    const auto* actions = child<AdButton>(*page, "translationActions");
    const QRect footer(page->mapTo(&window, QPoint(0, actions->y())),
                       QSize(page->width(), page->height() - actions->y()));
    source->clearFocus();
    flushEvents();
    const QImage unfocusedFooter = window.grab(footer).toImage();
    source->setFocus();
    flushEvents();
    require(window.grab(footer).toImage() == unfocusedFooter,
            "editor focus effects remain clipped to the scroll viewport");
    inspectVariants(QStringLiteral("error"));
    child<AdButton>(*page, "translationResultCopy")->click();
    require(QApplication::clipboard()->text() == result->toPlainText() &&
                !QApplication::clipboard()->text().contains(QStringLiteral("test failure")),
            "copy after failure contains only the retained partial translation");
    controller->retry();
    waitUntil([&]() { return server.streams.size() == 2; }, "retry main-window translation");
    server.delta(1, QStringLiteral("你好，世界！\n\n在不同语言之间翻译文本。"));
    server.finish(1);
    waitUntil([&]() { return !controller->translating(); }, "main-window completed result");
    const QString originalResult = result->toPlainText();
    child<AdButton>(*page, "translationCopyAndHide")->click();
    require(!window.isVisible() && QApplication::clipboard()->text() == originalResult,
            "main window forwards the page's Copy and Hide request");
    window.showAndActivate();
    require(result->toPlainText() == originalResult, "main-window reopening retains the draft");
    inspectVariants(QStringLiteral("completed"));
    QPointer<TranslationPageWidget> oldPage(page);
    card->setCurrentRoute(QStringLiteral("/global-mouse"));
    require(!controller->active(), "navigation deactivates before deferred deletion");
    flushEvents();
    require(oldPage.isNull(), "navigation destroys the translation page");
    card->setCurrentRoute(QStringLiteral("/tools/translation"));
    page = window.findChild<TranslationPageWidget*>();
    require(page != nullptr &&
                child<AdTextEdit>(*page, "translationSourceText")->toPlainText().isEmpty(),
            "returning to Translation starts an empty draft");
    window.hide();
    QPointer<MainWindow> closing = new MainWindow(settings::builtInSettingsRegistry(), runtime);
    closing->show();
    closing->findChild<ContentCardWidget*>()->setCurrentRoute(QStringLiteral("/tools/translation"));
    QPointer<TranslationPageWidget> closingPage = closing->findChild<TranslationPageWidget*>();
    require(closingPage != nullptr, "closing window owns a translation page");
    child<AdTextEdit>(*closingPage, "translationSourceText")
        ->setPlainText(QStringLiteral("Discard me"));
    closing->close();
    flushEvents();
    require(closing.isNull() && closingPage.isNull(), "closing the main window destroys its draft");
    qunsetenv("SNOW_SHOT_API_BASE_URL");
}

#ifdef Q_OS_WIN
void nativeWindowInteraction() {
    Server server;
    SnowShotApiClient client(server.url());
    QWidget owner;
    owner.setWindowTitle(QStringLiteral("Snow Shot Translation interaction test"));
    owner.resize(800, 650);
    auto* page = new TranslationPageWidget(&owner, &client, 0);
    page->setGeometry(owner.rect());
    QObject::connect(page, &TranslationPageWidget::hideWindowRequested, &owner, &QWidget::hide);
    // This native scenario exercises mouse and copy shortcuts. Do not inherit an unfinished
    // composition from the user's active Windows IME when the test takes foreground focus.
    auto* source = child<AdTextEdit>(*page, "translationSourceText");
    source->setAttribute(Qt::WA_InputMethodEnabled, false);
    owner.show();
    owner.raise();
    owner.activateWindow();
    // Match the existing UIA driver's activation procedure: a background test process does not
    // automatically receive foreground rights when Windows creates its first window.
    const auto activate = [&owner]() {
        const HWND target = reinterpret_cast<HWND>(owner.winId());
        const DWORD currentThread = GetCurrentThreadId();
        const DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const bool attached = foregroundThread != 0 && foregroundThread != currentThread &&
                              AttachThreadInput(currentThread, foregroundThread, TRUE) != FALSE;
        BringWindowToTop(target);
        SetForegroundWindow(target);
        if (attached) {
            AttachThreadInput(currentThread, foregroundThread, FALSE);
        }
        return GetForegroundWindow() == target;
    };
    waitUntil(activate, "native translation test owns keyboard focus");
    auto* result = child<AdTextEdit>(*page, "translationResultText");
    source->setPlainText(QStringLiteral("Hello, world!"));
    waitUntil([&]() { return server.streams.size() == 1; }, "native page starts translation");
    server.delta(0, QStringLiteral("你好，世界！"));
    waitUntil([&]() { return !result->toPlainText().isEmpty(); },
              "native page displays streamed text");
    auto* floating = child<AdButton>(*page, "translationActions");
    auto* popover = child<AdPopover>(*page, "translationActionsPopover");
    QCursor::setPos(floating->mapToGlobal(floating->rect().center()));
    waitUntil([&]() { return popover->isVisible(); }, "native pointer hover opens the popup");
    snapshot(owner, QStringLiteral("translation-native-hover"));
    auto nativeClick = [](QWidget* widget) {
        QCursor::setPos(widget->mapToGlobal(widget->rect().center()));
        flushEvents();
        INPUT input[2]{};
        input[0].type = INPUT_MOUSE;
        input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        input[1].type = INPUT_MOUSE;
        input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        require(SendInput(2, input, sizeof(INPUT)) == 2, "send native mouse click");
    };
    auto nativeCopy = [&activate](WORD letter) {
        require(activate(), "native keyboard input is confined to the test window");
        INPUT input[4]{};
        for (auto& item : input) {
            item.type = INPUT_KEYBOARD;
        }
        input[0].ki.wVk = VK_CONTROL;
        input[1].ki.wVk = letter;
        input[2].ki.wVk = letter;
        input[2].ki.dwFlags = KEYEVENTF_KEYUP;
        input[3].ki.wVk = VK_CONTROL;
        input[3].ki.dwFlags = KEYEVENTF_KEYUP;
        require(SendInput(4, input, sizeof(INPUT)) == 4, "send native copy shortcut");
    };
    QApplication::clipboard()->setText(QStringLiteral("before native copy"));
    nativeClick(child<AdButton>(*page, "translationCopy"));
    waitUntil([&]() { return QApplication::clipboard()->text() == result->toPlainText(); },
              "native hover action copies the partial translation");
    source->setFocus();
    source->selectAll();
    nativeCopy('C');
    waitUntil([&]() { return QApplication::clipboard()->text() == source->toPlainText(); },
              "native Ctrl+C respects the source selection");
    auto cursor = source->textCursor();
    cursor.clearSelection();
    source->setTextCursor(cursor);
    nativeCopy('C');
    waitUntil([&]() { return QApplication::clipboard()->text() == result->toPlainText(); },
              "native Ctrl+C falls back to translation");
    nativeCopy('Q');
    waitUntil([&]() { return !owner.isVisible(); }, "native Ctrl+Q hides the window");
    server.finish(0);
    owner.show();
    owner.raise();
    owner.activateWindow();
    waitUntil(activate, "reactivate the native translation test after hiding");
    QCursor::setPos(owner.mapToGlobal(QPoint(8, 8)));
    flushEvents();
    QCursor::setPos(floating->mapToGlobal(floating->rect().center()));
    waitUntil([&]() { return popover->isVisible(); }, "native hover works after hide and reopen");
    nativeClick(child<AdButton>(*page, "translationCopyAndHide"));
    waitUntil([&]() { return !owner.isVisible(); }, "native Copy and Hide action hides the window");
}
#endif
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("translation_page_tests"));
#ifdef Q_OS_WIN
    const QDir fonts(qEnvironmentVariable("WINDIR") + QStringLiteral("/Fonts"));
    for (const QString& file : {QStringLiteral("segoeui.ttf"), QStringLiteral("seguisb.ttf"),
                                QStringLiteral("msyh.ttc"), QStringLiteral("msyhbd.ttc")}) {
        if (QFileInfo::exists(fonts.filePath(file))) {
            QFontDatabase::addApplicationFont(fonts.filePath(file));
        }
    }
#endif
    QTemporaryDir directory;
    require(directory.isValid(), "create isolated translation-page storage");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({directory.path(), directory.path(), 60000}).success,
            "initialize isolated translation-page storage");
    styles::ThemeManager::instance().initialize(app);
#ifdef Q_OS_WIN
    if (app.arguments().contains(QStringLiteral("--native-interaction"))) {
        nativeWindowInteraction();
    } else
#endif
    {
        editorAndShortcutBehavior();
        navigationThemesLanguagesAndGeometry();
    }
    storage.shutdown();
    return 0;
}
